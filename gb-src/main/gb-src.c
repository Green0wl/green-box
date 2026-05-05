/*
 * GreenBox firmware entry point.
 *
 * Orchestrates the four phases defined in docs/global-sequence.puml:
 *   Phase 1 - WiFi provisioning (or auto-reconnect with saved credentials)
 *   Phase 1.5 - SNTP time synchronisation (required for schedule logic and
 *               UTC timestamps in MQTT events)
 *   Phase 2 - MQTT registration with the application server
 *   Phase 3+4 - Config push handling and watering control loop (blocks forever)
 *
 * The RST button monitor and overheat (hibernate) callbacks run in parallel
 * background tasks for the entire device lifetime.
 */

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

/*
 * RST button held for 5 s: nuke all credentials and restart so the device
 * re-enters provisioning mode on next boot.
 */
static void on_rst_button(void)
{
    ESP_LOGW(TAG, "RST button: erasing WiFi credentials and restarting");
    led_driver_set(LED_NETWORK, LED_STATE_OFF);
    led_driver_set(LED_APPLICATION, LED_STATE_OFF);
    nvs_flash_erase();
    esp_restart();
}

/*
 * Hibernate-enter: shut down WiFi and the HTTP server. The order matters —
 * led_driver_set(PURPLE) is applied by temp_monitor *after* this callback
 * so that esp_wifi_stop() (which can reset the GPIO mux on strapping pins
 * like IO5 = LED2 Blue) does not clobber the LED state.
 */
static void on_hibernate_enter(void)
{
    ESP_LOGW(TAG, "Hibernate: stopping web server and WiFi");
    led_driver_set(LED_NETWORK, LED_STATE_OFF);
    web_server_stop();
    wifi_manager_stop();
}

/* Hibernate-exit: reverse of enter. AP comes up again so a user can still
 * provision while the device is recovering from overheat. */
static void on_hibernate_exit(void)
{
    ESP_LOGI(TAG, "Hibernate exit: restarting WiFi AP and web server");
    wifi_manager_restart_ap();
    web_server_start(s_prov_done);
    led_driver_set(LED_NETWORK, LED_STATE_BLUE_BLINKING);
}

/*
 * Try to connect with credentials from NVS. Returns true if STA is up and
 * we have an IP, false if no creds are saved or the connection failed.
 */
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
    /* ---- Phase 1: bring up subsystems and either auto-connect or
     * launch the provisioning AP + HTTP server.
     *
     * Init order matters: WiFi must initialise before the LED driver
     * because the WiFi stack briefly uses some LED GPIOs (IO5 is a
     * strapping pin that the WiFi RF init touches). */
    ESP_ERROR_CHECK(nvs_manager_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(led_driver_init());
    ESP_ERROR_CHECK(rst_button_start(on_rst_button));

    if (!try_saved_credentials()) {
        /* No saved creds or they failed — run the provisioning server.
         * temp_monitor runs in parallel: if the device overheats while
         * waiting for the user to submit the form, we shut the AP down
         * (callbacks above) and resume when temperature normalises. */
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

    /* ---- SNTP: required before MQTT so timestamps in events are real.
     * 15 attempts with 1 s timeout each. We continue even if all attempts
     * fail (clock will simply read 1970-01-01). */
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

    /* ---- Phase 2: register with the server. Blocks until success or
     * MAX_RETRIES (10 attempts with exponential backoff up to 60 s). */
    mqtt_reg_result_t reg_result;
    ESP_ERROR_CHECK(mqtt_manager_register(&reg_result));

    if (reg_result != MQTT_REG_OK) {
        ESP_LOGE(TAG, "Registration failed (result=%d), halting", reg_result);
        return;
    }

    ESP_LOGI(TAG, "Device registered, entering Phase 3+4");

    /* ---- Phase 3+4: keep MQTT session open, listen for config pushes
     * from the server, run schedule + humidity timers, drive pumps via
     * per-port FreeRTOS tasks. Blocks forever. */
    mqtt_manager_run();
}
