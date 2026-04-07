#include "rst_button.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rst_button";

#define RST_GPIO        12
#define POLL_MS         100
#define HOLD_THRESHOLD  (5000 / POLL_MS)  /* 5 seconds */

static rst_button_callback_t s_callback;

static void rst_button_task(void *arg)
{
    int hold_count = 0;

    for (;;) {
        if (gpio_get_level(RST_GPIO) == 1) {
            hold_count++;
            if (hold_count >= HOLD_THRESHOLD) {
                ESP_LOGW(TAG, "RST button held for 5s — triggering reset");
                if (s_callback) s_callback();
                hold_count = 0;
            }
        } else {
            hold_count = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t rst_button_start(rst_button_callback_t on_reset)
{
    s_callback = on_reset;

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << RST_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  /* external R11 10K pulldown */
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    BaseType_t ret = xTaskCreate(rst_button_task, "rst_btn", 2048, NULL, 5, NULL);
    if (ret != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "RST button monitor started on GPIO%d (hold 5s to reset)", RST_GPIO);
    return ESP_OK;
}
