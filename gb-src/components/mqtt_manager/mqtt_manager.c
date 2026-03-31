#include "mqtt_manager.h"
#include "mqtt_client.h"
#include "nvs_manager.h"
#include "led_driver.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "mqtt_manager";

#define DEVICE_ID_PREFIX     "GB-"
#define PRE_SHARED_KEY       "greenbox"
#define FIRMWARE_VERSION     "1.0.0"
#define HARDWARE_REVISION    "rev3"

#define MAX_RETRIES          10
#define RESPONSE_TIMEOUT_MS  10000
#define MAX_SCHEDULE_SLOTS   8
#define HUMIDITY_CHECK_MS    (60 * 1000)
#define SCHEDULE_CHECK_MS    (30 * 1000)

#define EVT_CONNECTED        BIT0
#define EVT_DISCONNECTED     BIT1
#define EVT_SUBSCRIBED       BIT2
#define EVT_PUBLISHED        BIT3
#define EVT_DATA_RECEIVED    BIT4
#define EVT_ERROR            BIT5
#define EVT_CONFIG_RECEIVED  BIT6

static EventGroupHandle_t s_mqtt_events;
static esp_mqtt_client_handle_t s_client;
static char s_device_id[20];
static char s_response_buf[512];
static char s_config_buf[512];
static bool s_running;   /* true after mqtt_manager_run() starts */

static const uint32_t backoff_ms[MAX_RETRIES] = {
    1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000, 60000
};

/* ---- Per-port config ---- */

typedef struct {
    int  humidity_threshold_pct;
    int  num_slots;
    struct { int hour; int min; int duration_s; } slots[MAX_SCHEDULE_SLOTS];
    char config_id[52];
    bool active;
    /* Track last triggered minute to avoid re-firing within the same minute */
    int  last_triggered_min;
} port_config_t;

static port_config_t s_ports[2];

/* ---- Watering queue ---- */

typedef struct {
    int  port;
    int  duration_s;
    char trigger[16];
} watering_cmd_t;

static QueueHandle_t s_watering_queue;

/* ---- Topics ---- */

static char s_topic_config_push[64];
static char s_topic_reg_resp[64];

/* ---- Helpers ---- */

static void get_device_id(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, DEVICE_ID_PREFIX "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void get_mac_str(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void get_iso_timestamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    *out = atoi(p);
    return true;
}

static bool json_get_array(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '[') return false;
    const char *start = p;
    int depth = 0;
    for (; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') { depth--; if (depth == 0) { p++; break; } }
    }
    size_t len = (size_t)(p - start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static int parse_schedule(const char *json_array, port_config_t *pc)
{
    pc->num_slots = 0;
    const char *p = json_array;
    while (*p && pc->num_slots < MAX_SCHEDULE_SLOTS) {
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) break;
        const char *t = strstr(obj, "\"time\"");
        if (t && t < obj_end) {
            t = strchr(t + 6, '"');
            if (t) {
                t++;
                int h = 0, m = 0;
                if (sscanf(t, "%d:%d", &h, &m) == 2) {
                    pc->slots[pc->num_slots].hour = h;
                    pc->slots[pc->num_slots].min = m;
                }
            }
        }
        const char *d = strstr(obj, "\"duration_s\"");
        if (d && d < obj_end) {
            d += 12;
            while (*d == ' ' || *d == ':') d++;
            pc->slots[pc->num_slots].duration_s = atoi(d);
        }
        pc->num_slots++;
        p = obj_end + 1;
    }
    return pc->num_slots;
}

static bool wait_bits(EventBits_t bits, uint32_t timeout_ms, EventBits_t *out)
{
    *out = xEventGroupWaitBits(s_mqtt_events, bits,
                               pdTRUE, pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
    return (*out & bits) != 0;
}

/* ---- MQTT event handler ---- */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        /* Re-subscribe to config topic after reconnect */
        if (s_running) {
            esp_mqtt_client_subscribe(s_client, s_topic_config_push, 1);
            ESP_LOGI(TAG, "Re-subscribed to %s", s_topic_config_push);
        }
        xEventGroupSetBits(s_mqtt_events, EVT_CONNECTED);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        xEventGroupSetBits(s_mqtt_events, EVT_DISCONNECTED);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        xEventGroupSetBits(s_mqtt_events, EVT_SUBSCRIBED);
        break;
    case MQTT_EVENT_PUBLISHED:
        xEventGroupSetBits(s_mqtt_events, EVT_PUBLISHED);
        break;
    case MQTT_EVENT_DATA: {
        bool is_config = (event->topic_len > 0 &&
                          strstr(event->topic, "/config/push") != NULL);
        if (is_config) {
            if (event->data_len < (int)sizeof(s_config_buf)) {
                memcpy(s_config_buf, event->data, event->data_len);
                s_config_buf[event->data_len] = '\0';
            }
            xEventGroupSetBits(s_mqtt_events, EVT_CONFIG_RECEIVED);
        } else {
            if (event->data_len < (int)sizeof(s_response_buf)) {
                memcpy(s_response_buf, event->data, event->data_len);
                s_response_buf[event->data_len] = '\0';
            }
            xEventGroupSetBits(s_mqtt_events, EVT_DATA_RECEIVED);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        xEventGroupSetBits(s_mqtt_events, EVT_ERROR);
        break;
    default:
        break;
    }
}

/* ---- Watering task (runs in its own FreeRTOS task, safe to block) ---- */

static void publish_watering_event(int port, const char *event_type, const char *trigger,
                                   int duration_s, const char *stop_reason)
{
    char topic[64];
    snprintf(topic, sizeof(topic), "greenbox/%s/event/watering", s_device_id);

    char ts[32];
    get_iso_timestamp(ts, sizeof(ts));
    float humidity_stub = 42.0f;

    char payload[384];
    if (strcmp(event_type, "watering_started") == 0) {
        snprintf(payload, sizeof(payload),
                 "{\"device_id\":\"%s\",\"port\":%d,\"event\":\"%s\","
                 "\"timestamp\":\"%s\",\"trigger\":\"%s\","
                 "\"humidity_at_start_pct\":%.1f,\"planned_duration_s\":%d}",
                 s_device_id, port, event_type, ts, trigger, humidity_stub, duration_s);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"device_id\":\"%s\",\"port\":%d,\"event\":\"%s\","
                 "\"timestamp\":\"%s\",\"trigger\":\"%s\","
                 "\"actual_duration_s\":%d,\"humidity_at_stop_pct\":%.1f,"
                 "\"stop_reason\":\"%s\"}",
                 s_device_id, port, event_type, ts, trigger,
                 duration_s, humidity_stub, stop_reason);
    }

    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Watering event: port=%d %s trigger=%s", port, event_type, trigger);
}

static void watering_task(void *arg)
{
    watering_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_watering_queue, &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        ESP_LOGI(TAG, "STUB: pump %d ON for %ds (trigger=%s)", cmd.port, cmd.duration_s, cmd.trigger);
        led_driver_set(LED_APPLICATION, LED_STATE_CYAN_BLINKING);
        publish_watering_event(cmd.port, "watering_started", cmd.trigger, cmd.duration_s, NULL);

        vTaskDelay(pdMS_TO_TICKS(cmd.duration_s * 1000));

        ESP_LOGI(TAG, "STUB: pump %d OFF", cmd.port);
        led_driver_set(LED_APPLICATION, LED_STATE_GREEN_STEADY);
        publish_watering_event(cmd.port, "watering_stopped", cmd.trigger, cmd.duration_s, "duration_complete");
    }
}

/* ---- Timer callbacks (non-blocking, post to queue) ---- */

static void schedule_check_callback(TimerHandle_t timer)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    int cur_hour = tm.tm_hour;
    int cur_min  = tm.tm_min;

    for (int i = 0; i < 2; i++) {
        port_config_t *pc = &s_ports[i];
        if (!pc->active) continue;
        for (int s = 0; s < pc->num_slots; s++) {
            if (pc->slots[s].hour == cur_hour && pc->slots[s].min == cur_min) {
                /* Avoid re-triggering within the same minute */
                int minute_id = cur_hour * 60 + cur_min;
                if (pc->last_triggered_min == minute_id) continue;
                pc->last_triggered_min = minute_id;

                watering_cmd_t cmd = {
                    .port = i + 1,
                    .duration_s = pc->slots[s].duration_s,
                };
                strncpy(cmd.trigger, "schedule", sizeof(cmd.trigger));
                xQueueSend(s_watering_queue, &cmd, 0);
                ESP_LOGI(TAG, "Schedule match %02d:%02d → port %d queued",
                         cur_hour, cur_min, i + 1);
            }
        }
    }
}

static void humidity_check_callback(TimerHandle_t timer)
{
    float humidity = 42.0f; /* TODO: read real sensor */

    for (int i = 0; i < 2; i++) {
        port_config_t *pc = &s_ports[i];
        if (!pc->active || pc->humidity_threshold_pct <= 0) continue;
        if (humidity < (float)pc->humidity_threshold_pct) {
            int duration = pc->num_slots > 0 ? pc->slots[0].duration_s : 30;
            ESP_LOGI(TAG, "Humidity %.1f%% < threshold %d%% on port %d",
                     humidity, pc->humidity_threshold_pct, i + 1);
            watering_cmd_t cmd = {
                .port = i + 1,
                .duration_s = duration,
            };
            strncpy(cmd.trigger, "humidity", sizeof(cmd.trigger));
            xQueueSend(s_watering_queue, &cmd, 0);
        }
    }
}

/* ---- Config handling (Phase 3) ---- */

static void publish_config_ack(int port, const char *config_id, const char *status,
                               const char *reason)
{
    char topic[64];
    snprintf(topic, sizeof(topic), "greenbox/%s/config/ack", s_device_id);

    char ts[32];
    get_iso_timestamp(ts, sizeof(ts));

    char payload[256];
    if (reason) {
        snprintf(payload, sizeof(payload),
                 "{\"config_id\":\"%s\",\"status\":\"%s\","
                 "\"timestamp\":\"%s\",\"device_id\":\"%s\",\"port\":%d,"
                 "\"reason\":\"%s\"}",
                 config_id, status, ts, s_device_id, port, reason);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"config_id\":\"%s\",\"status\":\"%s\","
                 "\"timestamp\":\"%s\",\"device_id\":\"%s\",\"port\":%d}",
                 config_id, status, ts, s_device_id, port);
    }

    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Config ack: config_id=%s status=%s", config_id, status);
}

static void handle_config_message(const char *json)
{
    ESP_LOGI(TAG, "Config received: %s", json);
    led_driver_set(LED_APPLICATION, LED_STATE_BLUE_BLINKING);

    char config_id[52] = {0};
    int port = 0, humidity_pct = 0;
    char schedule_raw[256] = {0};

    if (!json_get_str(json, "config_id", config_id, sizeof(config_id)) ||
        !json_get_int(json, "port", &port) ||
        !json_get_int(json, "humidity_threshold_pct", &humidity_pct) ||
        !json_get_array(json, "watering_schedule", schedule_raw, sizeof(schedule_raw))) {
        ESP_LOGW(TAG, "Config validation failed: missing fields");
        publish_config_ack(port > 0 ? port : 1, config_id, "rejected", "missing_fields");
        led_driver_set(LED_APPLICATION, LED_STATE_ORANGE_STEADY);
        vTaskDelay(pdMS_TO_TICKS(10000));
        led_driver_set(LED_APPLICATION, LED_STATE_GREEN_STEADY);
        return;
    }

    if (port < 1 || port > 2 || humidity_pct < 0 || humidity_pct > 100) {
        ESP_LOGW(TAG, "Config validation failed: port=%d hum=%d", port, humidity_pct);
        publish_config_ack(port, config_id, "rejected", "invalid_values");
        led_driver_set(LED_APPLICATION, LED_STATE_ORANGE_STEADY);
        vTaskDelay(pdMS_TO_TICKS(10000));
        led_driver_set(LED_APPLICATION, LED_STATE_GREEN_STEADY);
        return;
    }

    int idx = port - 1;
    port_config_t *pc = &s_ports[idx];

    if (strcmp(pc->config_id, config_id) == 0) {
        ESP_LOGI(TAG, "Config %s already applied (idempotent), re-sending ack", config_id);
        publish_config_ack(port, config_id, "applied", NULL);
        led_driver_set(LED_APPLICATION, LED_STATE_GREEN_STEADY);
        return;
    }

    nvs_manager_set_port_config(port, config_id, schedule_raw, humidity_pct);

    strncpy(pc->config_id, config_id, sizeof(pc->config_id) - 1);
    pc->humidity_threshold_pct = humidity_pct;
    parse_schedule(schedule_raw, pc);
    pc->active = true;
    pc->last_triggered_min = -1;

    ESP_LOGI(TAG, "Config applied: port=%d config_id=%s threshold=%d%% slots=%d",
             port, config_id, humidity_pct, pc->num_slots);
    for (int i = 0; i < pc->num_slots; i++) {
        ESP_LOGI(TAG, "  slot %d: %02d:%02d for %ds",
                 i, pc->slots[i].hour, pc->slots[i].min, pc->slots[i].duration_s);
    }

    publish_config_ack(port, config_id, "applied", NULL);
    led_driver_set(LED_APPLICATION, LED_STATE_GREEN_STEADY);
}

/* ---- Phase 2: Registration ---- */

esp_err_t mqtt_manager_register(mqtt_reg_result_t *result)
{
    s_mqtt_events = xEventGroupCreate();
    get_device_id(s_device_id, sizeof(s_device_id));

    snprintf(s_topic_config_push, sizeof(s_topic_config_push),
             "greenbox/%s/config/push", s_device_id);
    snprintf(s_topic_reg_resp, sizeof(s_topic_reg_resp),
             "greenbox/%s/reg/response", s_device_id);

    char mqtt_host[128] = {0};
    uint16_t mqtt_port = 1883;
    nvs_manager_get_mqtt(mqtt_host, sizeof(mqtt_host), &mqtt_port);

    char broker_uri[160];
    snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%u", mqtt_host, mqtt_port);
    ESP_LOGI(TAG, "Broker: %s, device_id: %s", broker_uri, s_device_id);

    char topic_req[64];
    snprintf(topic_req, sizeof(topic_req), "greenbox/%s/reg/request", s_device_id);

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        ESP_LOGI(TAG, "Registration attempt %d/%d", attempt + 1, MAX_RETRIES);
        led_driver_set(LED_APPLICATION, LED_STATE_YELLOW_BLINKING);

        esp_mqtt_client_config_t cfg = {
            .broker.address.uri = broker_uri,
            .credentials = {
                .client_id = s_device_id,
                .username  = s_device_id,
                .authentication.password = PRE_SHARED_KEY,
            },
            .session = {
                .protocol_ver = MQTT_PROTOCOL_V_5,
                .disable_clean_session = false,
                .keepalive = 60,
            },
            .network.timeout_ms = 10000,
        };

        esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                       mqtt_event_handler, NULL);

        xEventGroupClearBits(s_mqtt_events, 0xFF);
        esp_mqtt_client_start(client);

        EventBits_t bits;
        if (!wait_bits(EVT_CONNECTED | EVT_ERROR | EVT_DISCONNECTED, 15000, &bits)
            || !(bits & EVT_CONNECTED)) {
            if (bits & EVT_ERROR)
                led_driver_set(LED_APPLICATION, LED_STATE_RED_FAST_BLINK);
            else
                led_driver_set(LED_APPLICATION, LED_STATE_RED_SLOW_BLINK);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            continue;
        }

        xEventGroupClearBits(s_mqtt_events, EVT_SUBSCRIBED);
        esp_mqtt_client_subscribe(client, s_topic_reg_resp, 1);
        if (!wait_bits(EVT_SUBSCRIBED | EVT_DISCONNECTED, 10000, &bits)
            || !(bits & EVT_SUBSCRIBED)) {
            led_driver_set(LED_APPLICATION, LED_STATE_RED_SLOW_BLINK);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            continue;
        }

        char mac_str[18];
        get_mac_str(mac_str, sizeof(mac_str));
        int64_t uptime_ms = esp_timer_get_time() / 1000;

        char payload[256];
        snprintf(payload, sizeof(payload),
                 "{\"device_id\":\"%s\","
                 "\"mac_address\":\"%s\","
                 "\"firmware_version\":\"" FIRMWARE_VERSION "\","
                 "\"hardware_revision\":\"" HARDWARE_REVISION "\","
                 "\"uptime_ms\":%lld}",
                 s_device_id, mac_str, (long long)uptime_ms);

        xEventGroupClearBits(s_mqtt_events, EVT_PUBLISHED | EVT_DATA_RECEIVED);
        esp_mqtt_client_publish(client, topic_req, payload, 0, 1, 0);

        if (!wait_bits(EVT_PUBLISHED | EVT_DISCONNECTED, 10000, &bits)
            || !(bits & EVT_PUBLISHED)) {
            led_driver_set(LED_APPLICATION, LED_STATE_RED_SLOW_BLINK);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            continue;
        }

        if (!wait_bits(EVT_DATA_RECEIVED | EVT_DISCONNECTED, RESPONSE_TIMEOUT_MS, &bits)
            || !(bits & EVT_DATA_RECEIVED)) {
            led_driver_set(LED_APPLICATION, LED_STATE_ORANGE_BLINKING);
            esp_mqtt_client_disconnect(client);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            continue;
        }

        char status[32] = {0};
        json_get_str(s_response_buf, "status", status, sizeof(status));

        if (strcmp(status, "rejected") == 0) {
            led_driver_set(LED_APPLICATION, LED_STATE_RED_STEADY);
            esp_mqtt_client_disconnect(client);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            continue;
        }

        if (strcmp(status, "registered") == 0 ||
            strcmp(status, "already_registered") == 0) {
            ESP_LOGI(TAG, "Registration successful: %s", status);
            led_driver_set(LED_APPLICATION, LED_STATE_GREEN_STEADY);
            s_client = client;
            *result = MQTT_REG_OK;
            return ESP_OK;
        }

        esp_mqtt_client_disconnect(client);
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
    }

    ESP_LOGE(TAG, "Max retries exhausted");
    led_driver_set(LED_APPLICATION, LED_STATE_RED_STEADY);
    vEventGroupDelete(s_mqtt_events);
    s_mqtt_events = NULL;
    *result = MQTT_REG_MAX_RETRIES;
    return ESP_OK;
}

/* ---- Phase 3+4: Config + Watering ---- */

esp_err_t mqtt_manager_run(void)
{
    if (!s_client || !s_mqtt_events) {
        ESP_LOGE(TAG, "MQTT client not connected — register first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create watering queue and task */
    s_watering_queue = xQueueCreate(4, sizeof(watering_cmd_t));
    xTaskCreate(watering_task, "watering", 4096, NULL, 5, NULL);

    /* Load saved configs from NVS */
    for (int port = 1; port <= 2; port++) {
        int idx = port - 1;
        char cid[52] = {0}, sched[256] = {0};
        int hum = 0;
        if (nvs_manager_get_port_config(port, cid, sizeof(cid),
                                        sched, sizeof(sched), &hum) == ESP_OK) {
            strncpy(s_ports[idx].config_id, cid, sizeof(s_ports[idx].config_id) - 1);
            s_ports[idx].humidity_threshold_pct = hum;
            parse_schedule(sched, &s_ports[idx]);
            s_ports[idx].active = true;
            s_ports[idx].last_triggered_min = -1;
            ESP_LOGI(TAG, "Loaded NVS config for port %d: config_id=%s slots=%d hum=%d%%",
                     port, cid, s_ports[idx].num_slots, hum);
        }
    }

    /* Subscribe to config/push (also re-subscribed on reconnect via event handler) */
    s_running = true;
    xEventGroupClearBits(s_mqtt_events, EVT_SUBSCRIBED);
    esp_mqtt_client_subscribe(s_client, s_topic_config_push, 1);
    EventBits_t bits;
    wait_bits(EVT_SUBSCRIBED | EVT_DISCONNECTED, 10000, &bits);

    /* Start periodic timers (non-blocking callbacks) */
    TimerHandle_t sched_timer = xTimerCreate("sched_chk", pdMS_TO_TICKS(SCHEDULE_CHECK_MS),
                                             pdTRUE, NULL, schedule_check_callback);
    TimerHandle_t hum_timer = xTimerCreate("hum_chk", pdMS_TO_TICKS(HUMIDITY_CHECK_MS),
                                           pdTRUE, NULL, humidity_check_callback);
    xTimerStart(sched_timer, 0);
    xTimerStart(hum_timer, 0);

    ESP_LOGI(TAG, "Phase 3+4 running: configs, schedule checks every %ds, humidity every %ds",
             SCHEDULE_CHECK_MS / 1000, HUMIDITY_CHECK_MS / 1000);

    /* Main loop: wait for config messages */
    for (;;) {
        xEventGroupWaitBits(s_mqtt_events, EVT_CONFIG_RECEIVED,
                            pdTRUE, pdFALSE, portMAX_DELAY);
        handle_config_message(s_config_buf);
    }

    return ESP_OK;
}
