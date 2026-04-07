#include "temp_monitor.h"
#include "led_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "temp_monitor";

/* TODO: replace with real sensor reading when hardware is available */
#define NORMAL_PERIOD_MS    (2 * 60 * 1000)
#define HIBERNATE_PERIOD_MS (1 * 60 * 1000)

static volatile bool s_hibernating = false;
static TaskHandle_t s_task_handle = NULL;
static temp_monitor_callbacks_t s_cb;

static void temp_monitor_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(NORMAL_PERIOD_MS));

        ESP_LOGW(TAG, "Overheat detected — entering hibernate");
        s_hibernating = true;
        if (s_cb.on_hibernate_enter) s_cb.on_hibernate_enter();
        led_driver_set(LED_APPLICATION, LED_STATE_PURPLE_STEADY);

        vTaskDelay(pdMS_TO_TICKS(HIBERNATE_PERIOD_MS));

        ESP_LOGI(TAG, "Temperature normalized — leaving hibernate");
        s_hibernating = false;
        led_driver_set(LED_APPLICATION, LED_STATE_OFF);

        if (s_cb.on_hibernate_exit) s_cb.on_hibernate_exit();
    }
}

esp_err_t temp_monitor_start(const temp_monitor_callbacks_t *callbacks)
{
    if (callbacks) s_cb = *callbacks;

    BaseType_t ret = xTaskCreate(temp_monitor_task, "temp_mon", 4096, NULL, 5, &s_task_handle);
    if (ret != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Temperature monitor started (simulated: %ds normal / %ds hibernate)",
             NORMAL_PERIOD_MS / 1000, HIBERNATE_PERIOD_MS / 1000);
    return ESP_OK;
}

bool temp_monitor_is_hibernating(void)
{
    return s_hibernating;
}

void temp_monitor_stop(void)
{
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
        s_hibernating = false;
        ESP_LOGI(TAG, "Temperature monitor stopped");
    }
}
