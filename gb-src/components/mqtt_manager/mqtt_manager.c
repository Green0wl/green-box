#include "mqtt_manager.h"
#include "mqtt_client.h"
#include "nvs_manager.h"
#include "led_driver.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "mqtt_manager";

#define DEVICE_ID_PREFIX     "GB-"
#define PRE_SHARED_KEY       "greenbox"
#define FIRMWARE_VERSION     "1.0.0"
#define HARDWARE_REVISION    "rev3"

#define MAX_RETRIES          10
#define RESPONSE_TIMEOUT_MS  10000

#define EVT_CONNECTED        BIT0
#define EVT_DISCONNECTED     BIT1
#define EVT_SUBSCRIBED       BIT2
#define EVT_PUBLISHED        BIT3
#define EVT_DATA_RECEIVED    BIT4
#define EVT_ERROR            BIT5

static EventGroupHandle_t s_mqtt_events;
static char s_response_buf[512];
static int  s_response_len;

/* Backoff intervals in ms: 1s, 2s, 4s, 8s, 16s, 32s, 60s, 60s, 60s, 60s */
static const uint32_t backoff_ms[MAX_RETRIES] = {
    1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000, 60000
};

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

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        xEventGroupSetBits(s_mqtt_events, EVT_CONNECTED);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        xEventGroupSetBits(s_mqtt_events, EVT_DISCONNECTED);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed, msg_id=%d", event->msg_id);
        xEventGroupSetBits(s_mqtt_events, EVT_SUBSCRIBED);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "Published, msg_id=%d", event->msg_id);
        xEventGroupSetBits(s_mqtt_events, EVT_PUBLISHED);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Data received on topic %.*s", event->topic_len, event->topic);
        if (event->data_len < (int)sizeof(s_response_buf)) {
            memcpy(s_response_buf, event->data, event->data_len);
            s_response_len = event->data_len;
            s_response_buf[s_response_len] = '\0';
        }
        xEventGroupSetBits(s_mqtt_events, EVT_DATA_RECEIVED);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        xEventGroupSetBits(s_mqtt_events, EVT_ERROR);
        break;
    default:
        break;
    }
}

/*
 * Minimal JSON field extraction (avoids pulling in cJSON for one use).
 * Finds "key":"value" and copies value into out. Returns true if found.
 */
static bool json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    /* skip optional whitespace and colon */
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

static bool wait_bits(EventBits_t bits, uint32_t timeout_ms, EventBits_t *out)
{
    *out = xEventGroupWaitBits(s_mqtt_events, bits,
                               pdTRUE, pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
    return (*out & bits) != 0;
}

esp_err_t mqtt_manager_register(mqtt_reg_result_t *result)
{
    s_mqtt_events = xEventGroupCreate();

    char device_id[20];
    get_device_id(device_id, sizeof(device_id));

    char mqtt_host[128] = {0};
    uint16_t mqtt_port = 1883;
    nvs_manager_get_mqtt(mqtt_host, sizeof(mqtt_host), &mqtt_port);

    char broker_uri[160];
    snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%u", mqtt_host, mqtt_port);
    ESP_LOGI(TAG, "Broker: %s, device_id: %s", broker_uri, device_id);

    char topic_req[64], topic_resp[64];
    snprintf(topic_req,  sizeof(topic_req),  "greenbox/%s/reg/request",  device_id);
    snprintf(topic_resp, sizeof(topic_resp), "greenbox/%s/reg/response", device_id);

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        ESP_LOGI(TAG, "Registration attempt %d/%d", attempt + 1, MAX_RETRIES);
        led_driver_set(LED_APPLICATION, LED_STATE_YELLOW_BLINKING);

        /* --- 2.1-2.2 Connect to broker (DNS + TCP handled by esp-mqtt) --- */
        esp_mqtt_client_config_t cfg = {
            .broker.address.uri = broker_uri,
            .credentials = {
                .client_id = device_id,
                .username  = device_id,
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
            ESP_LOGW(TAG, "Connection failed");
            if (bits & EVT_ERROR)
                led_driver_set(LED_APPLICATION, LED_STATE_RED_FAST_BLINK);
            else
                led_driver_set(LED_APPLICATION, LED_STATE_RED_SLOW_BLINK);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            continue;
        }

        /* --- 2.4 Subscribe to response topic --- */
        xEventGroupClearBits(s_mqtt_events, EVT_SUBSCRIBED);
        esp_mqtt_client_subscribe(client, topic_resp, 1);

        if (!wait_bits(EVT_SUBSCRIBED | EVT_DISCONNECTED, 10000, &bits)
            || !(bits & EVT_SUBSCRIBED)) {
            ESP_LOGW(TAG, "Subscribe failed");
            led_driver_set(LED_APPLICATION, LED_STATE_RED_SLOW_BLINK);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            continue;
        }

        /* --- 2.5 Publish registration request --- */
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
                 device_id, mac_str, (long long)uptime_ms);

        xEventGroupClearBits(s_mqtt_events, EVT_PUBLISHED | EVT_DATA_RECEIVED);
        esp_mqtt_client_publish(client, topic_req, payload, 0, 1, 0);

        if (!wait_bits(EVT_PUBLISHED | EVT_DISCONNECTED, 10000, &bits)
            || !(bits & EVT_PUBLISHED)) {
            ESP_LOGW(TAG, "Publish failed");
            led_driver_set(LED_APPLICATION, LED_STATE_RED_SLOW_BLINK);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            continue;
        }

        /* --- 2.7 Wait for server response --- */
        if (!wait_bits(EVT_DATA_RECEIVED | EVT_DISCONNECTED, RESPONSE_TIMEOUT_MS, &bits)
            || !(bits & EVT_DATA_RECEIVED)) {
            ESP_LOGW(TAG, "No response from server within %d ms", RESPONSE_TIMEOUT_MS);
            led_driver_set(LED_APPLICATION, LED_STATE_ORANGE_BLINKING);
            esp_mqtt_client_disconnect(client);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            continue;
        }

        /* Parse response */
        ESP_LOGI(TAG, "Response: %s", s_response_buf);
        char status[32] = {0};
        json_get_str(s_response_buf, "status", status, sizeof(status));

        if (strcmp(status, "rejected") == 0) {
            ESP_LOGW(TAG, "Registration rejected by server");
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

            /* Store server config in NVS (telemetry_interval, ota_check_interval) */
            /* TODO: parse config block and store when Phase 3 is implemented */

            esp_mqtt_client_disconnect(client);
            esp_mqtt_client_stop(client);
            esp_mqtt_client_destroy(client);
            vEventGroupDelete(s_mqtt_events);
            s_mqtt_events = NULL;
            *result = MQTT_REG_OK;
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Unexpected status: %s", status);
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
