/*
 * MQTT-driven Phase 2 (registration), Phase 3 (config push handling) and
 * Phase 4 (watering execution + telemetry).
 *
 * Single MQTT 5.0 client (espressif/mqtt managed component). Stays
 * connected for the device's entire post-Phase-1 lifetime. On broker
 * restart, esp-mqtt reconnects automatically and we re-subscribe to the
 * config-push topic from the MQTT_EVENT_CONNECTED handler.
 *
 * Watering runs in two dedicated FreeRTOS tasks (one per port), each
 * reading from its own queue. This makes the two pumps independent —
 * port 1 can water while port 2 is also watering. Timer callbacks are
 * non-blocking and just enqueue commands.
 *
 * Hardware:
 *   - PUMP1_GPIO / PUMP2_GPIO  → SS8050 NPN base, active HIGH
 *   - H1/H2 humidity sensors   → ADC1_CH4 / ADC1_CH5 (12-bit, 12 dB att.)
 */

#include "mqtt_manager.h"
#include "mqtt_client.h"
#include "nvs_manager.h"
#include "led_driver.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
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
#define HUMIDITY_CHECK_MS      (60 * 1000)
#define HUMIDITY_LOG_MS        (5 * 1000)
#define HUMIDITY_TELEMETRY_MS  (30 * 1000)
#define SCHEDULE_CHECK_MS      (30 * 1000)

/* ThingsBoard MQTT broker is assumed to run on the same host as Mosquitto
 * (provisioned via the form), on a non-conflicting port. The
 * docker-compose.yml maps the container's 1883 to host 1884. */
#define TB_MQTT_PORT           1884
#define TB_TELEMETRY_TOPIC     "v1/devices/me/telemetry"

/* Hardware GPIO from schematic */
#define PUMP1_GPIO           2
#define PUMP2_GPIO           25
#define H1_ADC_CHANNEL       ADC_CHANNEL_4  /* IO32 */
#define H2_ADC_CHANNEL       ADC_CHANNEL_5  /* IO33 */

#define EVT_CONNECTED        BIT0
#define EVT_DISCONNECTED     BIT1
#define EVT_SUBSCRIBED       BIT2
#define EVT_PUBLISHED        BIT3
#define EVT_DATA_RECEIVED    BIT4
#define EVT_ERROR            BIT5
#define EVT_CONFIG_RECEIVED  BIT6

static EventGroupHandle_t s_mqtt_events;
static esp_mqtt_client_handle_t s_client;
/* Optional second MQTT client connected to ThingsBoard for direct
 * telemetry. NULL if no token is configured. */
static esp_mqtt_client_handle_t s_tb_client;
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
    int  humidity_duration_s;
    int  num_slots;
    struct { int hour; int min; int duration_s; } slots[MAX_SCHEDULE_SLOTS];
    char config_id[52];
    bool active;
    /* Track last triggered minute to avoid re-firing within the same minute */
    int  last_triggered_min;
} port_config_t;

static port_config_t s_ports[2];

/* ---- Per-port sensor health ---- */

/*
 * The CSMS v2.0 reads at the ADC rails (0% / 100%) only at hardware
 * faults: floating input (sensor unplugged, ADC pin pulled by leakage)
 * or a short to GND/VCC. Three consecutive minute-cadence reads at the
 * rail flag the sensor as faulty. A subsequent in-band reading clears
 * the fault. While faulty, humidity-triggered watering is suppressed
 * for that port; schedule-triggered watering still runs (it does not
 * depend on the sensor) but the recorded humidity becomes JSON null.
 */
#define SENSOR_FAULT_THRESHOLD 3

typedef struct {
    int  consecutive_extreme;
    bool faulty;
} sensor_health_t;

static sensor_health_t s_sensor_health[2];

/* ---- Watering queue ---- */

typedef struct {
    int  port;
    int  duration_s;
    char trigger[16];
} watering_cmd_t;

static QueueHandle_t s_watering_queue[2];   /* one queue per port */
static adc_oneshot_unit_handle_t s_adc_handle;
static const int pump_gpio[2] = { PUMP1_GPIO, PUMP2_GPIO };
static volatile int s_active_pumps;        /* count of pumps currently running */
static portMUX_TYPE s_pump_mux = portMUX_INITIALIZER_UNLOCKED;

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

/*
 * Read humidity from CSMS v2.0 sensor via ADC.
 * Sensor outputs ~1.2V wet, ~2.8V dry (3.3V supply, 12-bit ADC → 0-4095).
 * Map: high raw = dry = low humidity%.
 *
 * Returns 0..100 on success or HUMIDITY_INVALID (-1.0f) when the ADC
 * read itself fails — the caller must check the sentinel before using
 * the value (publishing JSON, comparing to a threshold, etc.).
 */
#define HUMIDITY_INVALID (-1.0f)

static float read_humidity(adc_channel_t channel)
{
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, channel, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed on channel %d: %s",
                 channel, esp_err_to_name(err));
        return HUMIDITY_INVALID;
    }
    /* Linear map: 4095 → 0% (dry), 0 → 100% (wet) */
    float pct = 100.0f * (1.0f - (float)raw / 4095.0f);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
}

/*
 * Update the per-port sensor health based on a new reading. A reading
 * pinned to the ADC rails (≤0.5% or ≥99.5%) for SENSOR_FAULT_THRESHOLD
 * consecutive checks marks the sensor as faulty; any in-band reading
 * clears the fault. ADC hard errors (HUMIDITY_INVALID) count as
 * extreme.
 */
static void update_sensor_health(int port_idx, float pct)
{
    if (port_idx < 0 || port_idx > 1) return;
    sensor_health_t *h = &s_sensor_health[port_idx];

    bool extreme = (pct == HUMIDITY_INVALID) || (pct <= 0.5f) || (pct >= 99.5f);
    if (extreme) {
        h->consecutive_extreme++;
        if (h->consecutive_extreme == SENSOR_FAULT_THRESHOLD) {
            h->faulty = true;
            ESP_LOGW(TAG, "Port %d sensor FAULTY (%d consecutive rail reads)",
                     port_idx + 1, h->consecutive_extreme);
        }
    } else {
        if (h->faulty) {
            ESP_LOGI(TAG, "Port %d sensor recovered (reading=%.1f%%)",
                     port_idx + 1, pct);
        }
        h->consecutive_extreme = 0;
        h->faulty = false;
    }
}

static bool is_sensor_faulty(int port_idx)
{
    return (port_idx >= 0 && port_idx <= 1) && s_sensor_health[port_idx].faulty;
}

/* Format a humidity value for JSON output: numeric or "null" if invalid. */
static void humidity_to_json(float pct, char *out, size_t out_len)
{
    if (pct == HUMIDITY_INVALID) {
        strncpy(out, "null", out_len);
        out[out_len - 1] = '\0';
    } else {
        snprintf(out, out_len, "%.1f", pct);
    }
}

static bool wait_bits(EventBits_t bits, uint32_t timeout_ms, EventBits_t *out)
{
    *out = xEventGroupWaitBits(s_mqtt_events, bits,
                               pdTRUE, pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
    return (*out & bits) != 0;
}

/* ---- MQTT event handler ---- */

/*
 * esp-mqtt event dispatcher. Sets event-group bits so the registration
 * state machine and the config-push wait loop can synchronise.
 *
 * On (re)connect we re-subscribe to the config topic so the device
 * keeps receiving pushes after a broker restart. s_running ensures we
 * only do this in Phases 3+4 — during Phase 2 the registration code
 * subscribes to /reg/response itself.
 *
 * MQTT_EVENT_DATA arrives on multiple topics (reg/response and
 * config/push); we route by inspecting the topic substring.
 */
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

    adc_channel_t ch = (port == 1) ? H1_ADC_CHANNEL : H2_ADC_CHANNEL;
    float humidity = read_humidity(ch);

    /* Faulty sensor → emit JSON null instead of a misleading 0/100 */
    char humidity_str[16];
    if (is_sensor_faulty(port - 1)) {
        humidity_to_json(HUMIDITY_INVALID, humidity_str, sizeof(humidity_str));
    } else {
        humidity_to_json(humidity, humidity_str, sizeof(humidity_str));
    }

    char payload[384];
    if (strcmp(event_type, "watering_started") == 0) {
        snprintf(payload, sizeof(payload),
                 "{\"device_id\":\"%s\",\"port\":%d,\"event\":\"%s\","
                 "\"timestamp\":\"%s\",\"trigger\":\"%s\","
                 "\"humidity_at_start_pct\":%s,\"planned_duration_s\":%d}",
                 s_device_id, port, event_type, ts, trigger, humidity_str, duration_s);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"device_id\":\"%s\",\"port\":%d,\"event\":\"%s\","
                 "\"timestamp\":\"%s\",\"trigger\":\"%s\","
                 "\"actual_duration_s\":%d,\"humidity_at_stop_pct\":%s,"
                 "\"stop_reason\":\"%s\"}",
                 s_device_id, port, event_type, ts, trigger,
                 duration_s, humidity_str, stop_reason);
    }

    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Watering event: port=%d %s trigger=%s", port, event_type, trigger);
}

/*
 * Per-port watering task. One instance per pump (port 1 and port 2),
 * each with its own queue. Reads commands from the queue, drives the
 * pump GPIO HIGH, sleeps for the requested duration, then drives LOW.
 *
 * LED 2 stays cyan-blinking while at least one pump is active and
 * returns to green-steady only when the last pump finishes (the
 * s_active_pumps counter, protected by s_pump_mux, makes this safe
 * across the two parallel tasks).
 */
static void watering_task(void *arg)
{
    int port = (int)(uintptr_t)arg;     /* 1 or 2 */
    int idx = port - 1;
    int pin = pump_gpio[idx];
    watering_cmd_t cmd;

    for (;;) {
        if (xQueueReceive(s_watering_queue[idx], &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        ESP_LOGI(TAG, "Pump %d ON (GPIO%d) for %ds (trigger=%s)", port, pin, cmd.duration_s, cmd.trigger);
        gpio_set_level(pin, 1);
        portENTER_CRITICAL(&s_pump_mux);
        s_active_pumps++;
        portEXIT_CRITICAL(&s_pump_mux);
        led_driver_set(LED_APPLICATION, LED_STATE_CYAN_BLINKING);
        publish_watering_event(port, "watering_started", cmd.trigger, cmd.duration_s, NULL);

        vTaskDelay(pdMS_TO_TICKS(cmd.duration_s * 1000));

        gpio_set_level(pin, 0);
        ESP_LOGI(TAG, "Pump %d OFF (GPIO%d)", port, pin);
        portENTER_CRITICAL(&s_pump_mux);
        int remaining = --s_active_pumps;
        portEXIT_CRITICAL(&s_pump_mux);
        if (remaining == 0)
            led_driver_set(LED_APPLICATION, LED_STATE_GREEN_STEADY);
        publish_watering_event(port, "watering_stopped", cmd.trigger, cmd.duration_s, "duration_complete");
    }
}

/* ---- Timer callbacks (non-blocking, post to queue) ---- */

/*
 * Scheduled-watering check, runs every SCHEDULE_CHECK_MS (30 s). For
 * each port, walk the configured slots and compare against the current
 * UTC HH:MM. On a match, enqueue a watering command for that port's
 * task.
 *
 * `last_triggered_min` (a minute-of-day integer) prevents the same slot
 * from firing twice if the 30 s tick happens to land twice in the
 * matching minute.
 */
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
                xQueueSend(s_watering_queue[i], &cmd, 0);
                ESP_LOGI(TAG, "Schedule match %02d:%02d → port %d queued",
                         cur_hour, cur_min, i + 1);
            }
        }
    }
}

static void humidity_log_callback(TimerHandle_t timer)
{
    float h1 = read_humidity(H1_ADC_CHANNEL);
    float h2 = read_humidity(H2_ADC_CHANNEL);

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);

    char h1_str[16], h2_str[16];
    humidity_to_json(h1, h1_str, sizeof(h1_str));
    humidity_to_json(h2, h2_str, sizeof(h2_str));

    ESP_LOGI(TAG, "[%02d:%02d:%02d] Humidity: H1=%s%% H2=%s%%%s%s",
             tm.tm_hour, tm.tm_min, tm.tm_sec, h1_str, h2_str,
             is_sensor_faulty(0) ? " [H1 FAULT]" : "",
             is_sensor_faulty(1) ? " [H2 FAULT]" : "");
}

/*
 * Periodically publish humidity telemetry over MQTT so that the cloud
 * dashboard (ThingsBoard) can plot continuous humidity time-series even
 * when no watering event is happening. The DB is intentionally NOT updated
 * here — high-rate telemetry belongs in the time-series store, not the
 * relational watering_events table.
 */
static void humidity_telemetry_callback(TimerHandle_t timer)
{
    float h1 = read_humidity(H1_ADC_CHANNEL);
    float h2 = read_humidity(H2_ADC_CHANNEL);

    /* Faulty-sensor readings are emitted as JSON null so subscribers
     * (server, ThingsBoard) can distinguish "0% (truly dry)" from
     * "sensor unplugged". */
    char h1_str[16], h2_str[16];
    humidity_to_json(is_sensor_faulty(0) ? HUMIDITY_INVALID : h1,
                     h1_str, sizeof(h1_str));
    humidity_to_json(is_sensor_faulty(1) ? HUMIDITY_INVALID : h2,
                     h2_str, sizeof(h2_str));

    /* Publish to Mosquitto on the application topic so the server can
     * mark the device as alive even when the TB connection is down. */
    char topic[64];
    snprintf(topic, sizeof(topic), "greenbox/%s/event/humidity", s_device_id);

    char ts[32];
    get_iso_timestamp(ts, sizeof(ts));

    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"timestamp\":\"%s\","
             "\"humidity_h1_pct\":%s,\"humidity_h2_pct\":%s}",
             s_device_id, ts, h1_str, h2_str);

    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);

    /* Publish directly to ThingsBoard telemetry topic if a token was
     * configured during provisioning. ThingsBoard expects a flat
     * key/value object on v1/devices/me/telemetry. */
    if (s_tb_client) {
        char tb_payload[128];
        snprintf(tb_payload, sizeof(tb_payload),
                 "{\"humidity_h1\":%s,\"humidity_h2\":%s}", h1_str, h2_str);
        esp_mqtt_client_publish(s_tb_client, TB_TELEMETRY_TOPIC,
                                tb_payload, 0, 1, 0);
    }
}

/*
 * Open a second MQTT connection to the ThingsBoard broker if a token
 * was provisioned via NVS. ThingsBoard MQTT auth uses the device access
 * token as the username (no password). We assume TB runs on the same
 * host as Mosquitto on TB_MQTT_PORT (1884 in the bundled
 * docker-compose). On any failure we silently disable telemetry —
 * the device must keep working even if cloud forwarding is broken.
 */
static void thingsboard_mqtt_start(void)
{
    char token[64] = {0};
    if (nvs_manager_get_tb_token(token, sizeof(token)) != ESP_OK ||
        token[0] == '\0') {
        ESP_LOGI(TAG, "ThingsBoard token not set, telemetry disabled");
        return;
    }

    char mqtt_host[128] = {0};
    uint16_t mqtt_port = 1883;
    nvs_manager_get_mqtt(mqtt_host, sizeof(mqtt_host), &mqtt_port);
    if (mqtt_host[0] == '\0') return;

    char tb_uri[160];
    snprintf(tb_uri, sizeof(tb_uri), "mqtt://%s:%u", mqtt_host, TB_MQTT_PORT);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = tb_uri,
        .credentials = {
            .client_id = s_device_id,
            .username  = token,
            /* ThingsBoard uses an empty password; the token is the username */
        },
        .session = {
            .protocol_ver = MQTT_PROTOCOL_V_5,
            .keepalive = 60,
        },
        .network.timeout_ms = 10000,
    };

    s_tb_client = esp_mqtt_client_init(&cfg);
    if (!s_tb_client) {
        ESP_LOGW(TAG, "Failed to init ThingsBoard MQTT client");
        return;
    }
    if (esp_mqtt_client_start(s_tb_client) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start ThingsBoard MQTT client");
        esp_mqtt_client_destroy(s_tb_client);
        s_tb_client = NULL;
        return;
    }
    ESP_LOGI(TAG, "ThingsBoard MQTT started: %s", tb_uri);
}

static void humidity_check_callback(TimerHandle_t timer)
{
    adc_channel_t channels[2] = { H1_ADC_CHANNEL, H2_ADC_CHANNEL };

    /* Read both sensors first so health is updated even for ports
     * without an active humidity trigger. This is what feeds the
     * SENSOR_FAULT_THRESHOLD counter; a faulty sensor is marked the
     * minute its third consecutive rail-pinned read lands here. */
    float readings[2];
    for (int i = 0; i < 2; i++) {
        readings[i] = read_humidity(channels[i]);
        update_sensor_health(i, readings[i]);
    }

    for (int i = 0; i < 2; i++) {
        port_config_t *pc = &s_ports[i];
        if (!pc->active || pc->humidity_threshold_pct <= 0 || pc->humidity_duration_s <= 0) continue;

        /* Suppress humidity-triggered watering when the sensor is
         * known-faulty: a stuck-at-0% reading would otherwise make
         * the pump run on every check. Schedule-triggered watering
         * stays unaffected — it does not depend on the sensor. */
        if (is_sensor_faulty(i)) {
            ESP_LOGW(TAG, "Port %d humidity trigger SUPPRESSED (sensor faulty)", i + 1);
            continue;
        }

        if (readings[i] < (float)pc->humidity_threshold_pct) {
            int duration = pc->humidity_duration_s;
            ESP_LOGI(TAG, "Humidity %.1f%% < threshold %d%% on port %d",
                     readings[i], pc->humidity_threshold_pct, i + 1);
            watering_cmd_t cmd = {
                .port = i + 1,
                .duration_s = duration,
            };
            strncpy(cmd.trigger, "humidity", sizeof(cmd.trigger));
            xQueueSend(s_watering_queue[i], &cmd, 0);
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

/*
 * Process a /config/push message: validate, persist to NVS, apply.
 *
 * Idempotency: if pc->config_id matches the new config_id, we just
 * re-send the "applied" ack and return — covers QoS 1 redelivery and
 * retained-message delivery on broker reconnect.
 *
 * Validation failures publish a "rejected" ack with a reason and
 * flash LED 2 white-steady for 10 s before returning to green.
 */
static void handle_config_message(const char *json)
{
    ESP_LOGI(TAG, "Config received: %s", json);
    led_driver_set(LED_APPLICATION, LED_STATE_BLUE_BLINKING);

    char config_id[52] = {0};
    int port = 0, humidity_pct = 0, humidity_dur = 0;
    char schedule_raw[256] = {0};

    if (!json_get_str(json, "config_id", config_id, sizeof(config_id)) ||
        !json_get_int(json, "port", &port)) {
        ESP_LOGW(TAG, "Config validation failed: missing fields");
        publish_config_ack(port > 0 ? port : 1, config_id, "rejected", "missing_fields");
        led_driver_set(LED_APPLICATION, LED_STATE_WHITE_STEADY);
        vTaskDelay(pdMS_TO_TICKS(10000));
        led_driver_set(LED_APPLICATION, LED_STATE_GREEN_STEADY);
        return;
    }

    /* Optional fields — default to 0 / empty if missing */
    json_get_int(json, "humidity_threshold_pct", &humidity_pct);
    json_get_int(json, "humidity_duration_s", &humidity_dur);
    json_get_array(json, "watering_schedule", schedule_raw, sizeof(schedule_raw));

    if (port < 1 || port > 2 || humidity_pct < 0 || humidity_pct > 100) {
        ESP_LOGW(TAG, "Config validation failed: port=%d hum=%d", port, humidity_pct);
        publish_config_ack(port, config_id, "rejected", "invalid_values");
        led_driver_set(LED_APPLICATION, LED_STATE_WHITE_STEADY);
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

    nvs_manager_set_port_config(port, config_id, schedule_raw, humidity_pct, humidity_dur);

    strncpy(pc->config_id, config_id, sizeof(pc->config_id) - 1);
    pc->humidity_threshold_pct = humidity_pct;
    pc->humidity_duration_s = humidity_dur;
    parse_schedule(schedule_raw, pc);
    pc->active = true;
    pc->last_triggered_min = -1;

    ESP_LOGI(TAG, "Config applied: port=%d config_id=%s threshold=%d%% hum_dur=%ds slots=%d",
             port, config_id, humidity_pct, humidity_dur, pc->num_slots);
    for (int i = 0; i < pc->num_slots; i++) {
        ESP_LOGI(TAG, "  slot %d: %02d:%02d for %ds",
                 i, pc->slots[i].hour, pc->slots[i].min, pc->slots[i].duration_s);
    }

    publish_config_ack(port, config_id, "applied", NULL);
    led_driver_set(LED_APPLICATION, LED_STATE_GREEN_STEADY);
}

/* ---- Phase 2: Registration ---- */

/*
 * Phase 2 retry loop. Up to MAX_RETRIES attempts with exponential
 * backoff (1, 2, 4, 8, 16, 32, 60, 60, 60, 60 s). Each attempt creates a
 * fresh MQTT client, runs CONNECT → SUBSCRIBE(reg/response) →
 * PUBLISH(reg/request) → wait for response → parse status. On success
 * the client is kept alive (s_client) for the lifetime of Phase 3+4.
 */
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
            led_driver_set(LED_APPLICATION, LED_STATE_WHITE_BLINKING);
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

/*
 * Phase 3+4 entry point. Initialises pump GPIOs and ADC, restores any
 * saved port configs from NVS, spawns the two watering tasks, subscribes
 * to /config/push, starts the 30 s schedule timer, the 60 s humidity
 * threshold timer, the 5 s humidity log timer, and the 30 s humidity
 * telemetry timer. Then sits in an infinite loop waiting for /config/push
 * messages and dispatching them to handle_config_message().
 */
esp_err_t mqtt_manager_run(void)
{
    if (!s_client || !s_mqtt_events) {
        ESP_LOGE(TAG, "MQTT client not connected — register first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Init pump GPIOs */
    for (int i = 0; i < 2; i++) {
        gpio_config_t io_cfg = {
            .pin_bit_mask = 1ULL << pump_gpio[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&io_cfg);
        gpio_set_level(pump_gpio[i], 0);
    }
    ESP_LOGI(TAG, "Pump GPIOs initialized: P1=IO%d P2=IO%d", PUMP1_GPIO, PUMP2_GPIO);

    /* Init ADC for humidity sensors */
    adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, H1_ADC_CHANNEL, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, H2_ADC_CHANNEL, &chan_cfg));
    ESP_LOGI(TAG, "ADC initialized: H1=CH%d(IO32) H2=CH%d(IO33)", H1_ADC_CHANNEL, H2_ADC_CHANNEL);

    /* Create per-port watering queues and tasks (allows parallel watering) */
    s_watering_queue[0] = xQueueCreate(4, sizeof(watering_cmd_t));
    s_watering_queue[1] = xQueueCreate(4, sizeof(watering_cmd_t));
    xTaskCreate(watering_task, "watering_p1", 4096, (void *)(uintptr_t)1, 5, NULL);
    xTaskCreate(watering_task, "watering_p2", 4096, (void *)(uintptr_t)2, 5, NULL);

    /* Load saved configs from NVS */
    for (int port = 1; port <= 2; port++) {
        int idx = port - 1;
        char cid[52] = {0}, sched[256] = {0};
        int hum = 0, hum_dur = 0;
        if (nvs_manager_get_port_config(port, cid, sizeof(cid),
                                        sched, sizeof(sched), &hum, &hum_dur) == ESP_OK) {
            strncpy(s_ports[idx].config_id, cid, sizeof(s_ports[idx].config_id) - 1);
            s_ports[idx].humidity_threshold_pct = hum;
            s_ports[idx].humidity_duration_s = hum_dur;
            parse_schedule(sched, &s_ports[idx]);
            s_ports[idx].active = true;
            s_ports[idx].last_triggered_min = -1;
            ESP_LOGI(TAG, "Loaded NVS config for port %d: config_id=%s slots=%d hum=%d%% hum_dur=%ds",
                     port, cid, s_ports[idx].num_slots, hum, hum_dur);
        }
    }

    /* Subscribe to config/push (also re-subscribed on reconnect via event handler) */
    s_running = true;
    xEventGroupClearBits(s_mqtt_events, EVT_SUBSCRIBED);
    esp_mqtt_client_subscribe(s_client, s_topic_config_push, 1);
    EventBits_t bits;
    wait_bits(EVT_SUBSCRIBED | EVT_DISCONNECTED, 10000, &bits);

    /* Bring up the optional ThingsBoard telemetry connection. Failure
     * is logged but never propagated — Mosquitto-side functionality
     * must keep working regardless of TB availability. */
    thingsboard_mqtt_start();

    /* Start periodic timers (non-blocking callbacks) */
    TimerHandle_t sched_timer = xTimerCreate("sched_chk", pdMS_TO_TICKS(SCHEDULE_CHECK_MS),
                                             pdTRUE, NULL, schedule_check_callback);
    TimerHandle_t hum_timer = xTimerCreate("hum_chk", pdMS_TO_TICKS(HUMIDITY_CHECK_MS),
                                           pdTRUE, NULL, humidity_check_callback);
    TimerHandle_t hum_log_timer = xTimerCreate("hum_log", pdMS_TO_TICKS(HUMIDITY_LOG_MS),
                                               pdTRUE, NULL, humidity_log_callback);
    TimerHandle_t hum_tele_timer = xTimerCreate("hum_tele", pdMS_TO_TICKS(HUMIDITY_TELEMETRY_MS),
                                                pdTRUE, NULL, humidity_telemetry_callback);
    xTimerStart(sched_timer, 0);
    xTimerStart(hum_timer, 0);
    xTimerStart(hum_log_timer, 0);
    xTimerStart(hum_tele_timer, 0);

    ESP_LOGI(TAG, "Phase 3+4 running: schedule %ds, hum check %ds, hum log %ds, hum telemetry %ds",
             SCHEDULE_CHECK_MS / 1000, HUMIDITY_CHECK_MS / 1000,
             HUMIDITY_LOG_MS / 1000, HUMIDITY_TELEMETRY_MS / 1000);

    /* Main loop: wait for config messages */
    for (;;) {
        xEventGroupWaitBits(s_mqtt_events, EVT_CONFIG_RECEIVED,
                            pdTRUE, pdFALSE, portMAX_DELAY);
        handle_config_message(s_config_buf);
    }

    return ESP_OK;
}
