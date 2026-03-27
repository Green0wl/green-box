#include "nvs_manager.h"
#include "led_driver.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "temp_monitor.h"
#include "mqtt_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "app_main";
static EventGroupHandle_t s_prov_done;

static void on_hibernate_enter(void)
{
    ESP_LOGW(TAG, "Hibernate: stopping web server and WiFi");
    web_server_stop();
    wifi_manager_stop();
}

static void on_hibernate_exit(void)
{
    ESP_LOGI(TAG, "Hibernate exit: restarting WiFi AP and web server");
    wifi_manager_restart_ap();
    web_server_start(s_prov_done);
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
    ESP_ERROR_CHECK(led_driver_init());
    ESP_ERROR_CHECK(wifi_manager_init());

    if (!try_saved_credentials()) {
        /* No saved creds or they failed — run provisioning server */
        temp_monitor_callbacks_t cb = {
            .on_hibernate_enter = on_hibernate_enter,
            .on_hibernate_exit  = on_hibernate_exit,
        };
        ESP_ERROR_CHECK(temp_monitor_start(&cb));

        s_prov_done = xEventGroupCreate();
        ESP_ERROR_CHECK(web_server_start(s_prov_done));

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

    /* ---- Phase 2: MQTT registration ---- */
    mqtt_reg_result_t reg_result;
    ESP_ERROR_CHECK(mqtt_manager_register(&reg_result));

    if (reg_result == MQTT_REG_OK) {
        ESP_LOGI(TAG, "Device registered, ready for Phase 3");
    } else {
        ESP_LOGE(TAG, "Registration failed (result=%d)", reg_result);
    }

    /* Phase 3 (config push) starts here */
}
