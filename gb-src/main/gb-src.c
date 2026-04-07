#include "nvs_manager.h"
#include "led_driver.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "temp_monitor.h"
#include "mqtt_manager.h"
#include "rst_button.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <time.h>

static const char *TAG = "app_main";
static EventGroupHandle_t s_prov_done;

static void on_rst_button(void)
{
    ESP_LOGW(TAG, "RST button: erasing WiFi credentials and restarting");
    led_driver_set(LED_NETWORK, LED_STATE_OFF);
    led_driver_set(LED_APPLICATION, LED_STATE_OFF);
    nvs_flash_erase();
    esp_restart();
}

static void on_hibernate_enter(void)
{
    ESP_LOGW(TAG, "Hibernate: stopping web server and WiFi");
    led_driver_set(LED_NETWORK, LED_STATE_OFF);
    web_server_stop();
    wifi_manager_stop();
}

static void on_hibernate_exit(void)
{
    ESP_LOGI(TAG, "Hibernate exit: restarting WiFi AP and web server");
    wifi_manager_restart_ap();
    web_server_start(s_prov_done);
    led_driver_set(LED_NETWORK, LED_STATE_BLUE_BLINKING);
}

static bool try_saved_credentials(void)
{
    char ssid[33] = {0};
    char password[65] = {0};

    if (nvs_manager_get_wifi(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK
        || ssid[0] == '\0') {
        ESP_LOGI(TAG, "No saved WiFi credentials");
        return false;
    }

    ESP_LOGI(TAG, "Trying saved credentials for SSID: %s", ssid);
    led_driver_set(LED_NETWORK, LED_STATE_RED_BLINKING);

    wifi_connect_result_t result;
    wifi_manager_try_connect(ssid, password, &result);

    if (result == WIFI_RESULT_OK) {
        led_driver_set(LED_NETWORK, LED_STATE_GREEN_STEADY);
        ESP_LOGI(TAG, "Connected with saved credentials");
        return true;
    }

    ESP_LOGW(TAG, "Saved credentials failed (result=%d)", result);
    led_driver_set(LED_NETWORK, LED_STATE_OFF);
    return false;
}

void app_main(void)
{
    /* ---- Phase 1: WiFi provisioning ---- */
    ESP_ERROR_CHECK(nvs_manager_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(led_driver_init());
    ESP_ERROR_CHECK(rst_button_start(on_rst_button));

    if (!try_saved_credentials()) {
        /* No saved creds or they failed — run provisioning server */
        temp_monitor_callbacks_t cb = {
            .on_hibernate_enter = on_hibernate_enter,
            .on_hibernate_exit  = on_hibernate_exit,
        };
        ESP_ERROR_CHECK(temp_monitor_start(&cb));

        s_prov_done = xEventGroupCreate();
        ESP_ERROR_CHECK(web_server_start(s_prov_done));

        led_driver_set(LED_NETWORK, LED_STATE_BLUE_BLINKING);
        ESP_LOGI(TAG, "Waiting for provisioning...");
        xEventGroupWaitBits(s_prov_done, WEB_SERVER_PROV_DONE_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        temp_monitor_stop();
        web_server_stop();
        vEventGroupDelete(s_prov_done);
        s_prov_done = NULL;
    }

    wifi_manager_stop_ap();
    ESP_LOGI(TAG, "Provisioning complete, WiFi connected");

    /* Sync time via SNTP */
    ESP_LOGI(TAG, "Syncing time via SNTP...");
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);
    for (int i = 0; i < 15; i++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) == ESP_OK) break;
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    ESP_LOGI(TAG, "Time synced: %04d-%02d-%02dT%02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    /* ---- Phase 2: MQTT registration ---- */
    mqtt_reg_result_t reg_result;
    ESP_ERROR_CHECK(mqtt_manager_register(&reg_result));

    if (reg_result != MQTT_REG_OK) {
        ESP_LOGE(TAG, "Registration failed (result=%d), halting", reg_result);
        return;
    }

    ESP_LOGI(TAG, "Device registered, entering Phase 3+4");

    /* ---- Phase 3+4: Config + Watering (blocks forever) ---- */
    mqtt_manager_run();
}
