/*
 * WiFi state machine for both AP-only provisioning and STA connection
 * attempts. Runs in APSTA mode so a user can stay connected to the
 * provisioning AP while we test their credentials against the home WiFi.
 *
 * The AP SSID and password are derived from the device MAC so every unit
 * has unique, predictable credentials printed on its label.
 *
 * Connection attempts are guarded by an `s_connecting` flag: stale
 * disconnect events from `esp_wifi_disconnect()` (which we call to drop
 * any previous attempt) must not be mistaken for the new attempt's
 * outcome. This is a known async-event hazard in ESP-IDF.
 */

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_manager";

#define AP_CHANNEL    1
#define AP_MAX_CONN   4

#define STA_CONNECT_BIT   BIT0
#define STA_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static wifi_connect_result_t s_last_result;
static volatile bool s_connecting;

/* AP credentials derived from MAC — filled once in wifi_manager_init */
static char s_ap_ssid[20];     /* "GB-XXXXXXXXXXXX" */
static char s_ap_password[9];  /* first 8 hex chars of MAC */

static void build_ap_credentials(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "GB-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_ap_password, sizeof(s_ap_password), "%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3]);
}

static void apply_ap_config(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .channel        = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(ap_config.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    memcpy(ap_config.ap.password, s_ap_password, strlen(s_ap_password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = event_data;
            ESP_LOGW(TAG, "STA disconnected, reason=%d", ev->reason);
            if (!s_connecting) break;
            switch (ev->reason) {
            case WIFI_REASON_NO_AP_FOUND:
            case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
                s_last_result = WIFI_RESULT_AP_NOT_FOUND;
                break;
            case WIFI_REASON_AUTH_FAIL:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_MIC_FAILURE:
                s_last_result = WIFI_RESULT_WRONG_PASSWORD;
                break;
            default:
                s_last_result = WIFI_RESULT_TIMEOUT;
                break;
            }
            xEventGroupSetBits(s_wifi_event_group, STA_FAIL_BIT);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = event_data;
            ESP_LOGI(TAG, "Station %02x:%02x:%02x:%02x:%02x:%02x joined AP",
                     ev->mac[0], ev->mac[1], ev->mac[2],
                     ev->mac[3], ev->mac[4], ev->mac[5]);
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_last_result = WIFI_RESULT_OK;
        xEventGroupSetBits(s_wifi_event_group, STA_CONNECT_BIT);
    }
}

/*
 * Bring the WiFi stack up in APSTA mode and start the soft-AP so the
 * provisioning HTTP server has something to serve over. STA stays idle
 * until wifi_manager_try_connect() is called.
 */
esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    build_ap_credentials();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    apply_ap_config();
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Soft-AP started: SSID=%s PWD=%s", s_ap_ssid, s_ap_password);
    return ESP_OK;
}

/*
 * Attempt an STA connection with the given credentials. Blocks up to 15 s
 * until either an IP is obtained (success) or a disconnect event with a
 * meaningful reason arrives (wrong password / AP not found / timeout).
 *
 * The 100 ms vTaskDelay below is intentional: it lets the disconnect
 * event from the preceding esp_wifi_disconnect() drain *before* we clear
 * the event-group bits and arm s_connecting. Without this, a stale event
 * could short-circuit the new attempt.
 */
esp_err_t wifi_manager_try_connect(const char *ssid, const char *password,
                                   wifi_connect_result_t *result)
{
    s_connecting = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupClearBits(s_wifi_event_group, STA_CONNECT_BIT | STA_FAIL_BIT);

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    s_connecting = true;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_connecting = false;
        *result = WIFI_RESULT_TIMEOUT;
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           STA_CONNECT_BIT | STA_FAIL_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    s_connecting = false;

    if (bits & STA_CONNECT_BIT) {
        *result = WIFI_RESULT_OK;
    } else if (bits & STA_FAIL_BIT) {
        *result = s_last_result;
    } else {
        *result = WIFI_RESULT_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_stop_ap(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_netif_destroy(s_ap_netif);
    s_ap_netif = NULL;
    ESP_LOGI(TAG, "Soft-AP stopped");
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_LOGI(TAG, "WiFi stopped (hibernate)");
    return ESP_OK;
}

esp_err_t wifi_manager_restart_ap(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    apply_ap_config();
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi restarted after hibernate: SSID=%s", s_ap_ssid);
    return ESP_OK;
}
