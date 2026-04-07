#include "led_driver.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "led_driver";

/* GPIO assignments from schematic (Rev 1/1, 2026-03-24) */
#define LED1_R_GPIO 17
#define LED1_G_GPIO 16
#define LED1_B_GPIO  4
#define LED2_R_GPIO 19
#define LED2_G_GPIO 18
#define LED2_B_GPIO  5

static const int gpio_map[2][3] = {
    { LED1_R_GPIO, LED1_G_GPIO, LED1_B_GPIO },
    { LED2_R_GPIO, LED2_G_GPIO, LED2_B_GPIO },
};

static TimerHandle_t blink_timers[2];
static led_state_t current_state[2];
static uint8_t blink_phase[2];

/*
 * Digital on/off per R/G/B channel.
 * Colors: Red=100, Green=010, Yellow=110, White=111, Purple=101, Blue=001, Cyan=011
 */
static void set_rgb(led_id_t led, int r, int g, int b)
{
    gpio_set_level(gpio_map[led][0], r);
    gpio_set_level(gpio_map[led][1], g);
    gpio_set_level(gpio_map[led][2], b);
}

typedef struct {
    int r, g, b;
    uint16_t period_ms;
} blink_color_t;

static blink_color_t get_blink_color(led_state_t state)
{
    switch (state) {
    case LED_STATE_RED_BLINKING:      return (blink_color_t){1, 0, 0, 500};
    case LED_STATE_RED_DOUBLE_BLINK:  return (blink_color_t){1, 0, 0, 200};
    case LED_STATE_RED_SLOW_BLINK:    return (blink_color_t){1, 0, 0, 1000};
    case LED_STATE_RED_FAST_BLINK:    return (blink_color_t){1, 0, 0, 150};
    case LED_STATE_YELLOW_BLINKING:   return (blink_color_t){1, 1, 0, 500};
    case LED_STATE_WHITE_BLINKING:    return (blink_color_t){1, 1, 1, 500};
    case LED_STATE_BLUE_BLINKING:     return (blink_color_t){0, 0, 1, 500};
    case LED_STATE_CYAN_BLINKING:     return (blink_color_t){0, 1, 1, 500};
    default:                          return (blink_color_t){0, 0, 0, 500};
    }
}

static void blink_callback(TimerHandle_t timer)
{
    led_id_t led = (led_id_t)(uintptr_t)pvTimerGetTimerID(timer);
    blink_color_t c = get_blink_color(current_state[led]);

    if (current_state[led] == LED_STATE_RED_DOUBLE_BLINK) {
        uint8_t p = blink_phase[led] % 8;
        if (p == 0 || p == 2)
            set_rgb(led, c.r, c.g, c.b);
        else
            set_rgb(led, 0, 0, 0);
        blink_phase[led]++;
    } else {
        blink_phase[led] = !blink_phase[led];
        if (blink_phase[led])
            set_rgb(led, c.r, c.g, c.b);
        else
            set_rgb(led, 0, 0, 0);
    }
}

static void start_blink(led_id_t led, led_state_t state)
{
    blink_color_t c = get_blink_color(state);
    set_rgb(led, c.r, c.g, c.b);
    blink_phase[led] = 1;
    xTimerChangePeriod(blink_timers[led], pdMS_TO_TICKS(c.period_ms), 0);
    xTimerStart(blink_timers[led], 0);
}

esp_err_t led_driver_init(void)
{
    for (int led = 0; led < 2; led++) {
        for (int ch = 0; ch < 3; ch++) {
            gpio_config_t cfg = {
                .pin_bit_mask = 1ULL << gpio_map[led][ch],
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
            };
            ESP_ERROR_CHECK(gpio_config(&cfg));
            gpio_set_level(gpio_map[led][ch], 0);
        }
    }

    for (int i = 0; i < 2; i++) {
        current_state[i] = LED_STATE_OFF;
        blink_phase[i] = 0;
        blink_timers[i] = xTimerCreate(
            i == 0 ? "led1_blink" : "led2_blink",
            pdMS_TO_TICKS(500),
            pdTRUE,
            (void *)(uintptr_t)i,
            blink_callback
        );
        if (!blink_timers[i]) return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LED driver initialized (GPIO)");
    return ESP_OK;
}

esp_err_t led_driver_set(led_id_t led, led_state_t state)
{
    if (led > LED_APPLICATION) return ESP_ERR_INVALID_ARG;

    xTimerStop(blink_timers[led], 0);
    current_state[led] = state;
    blink_phase[led] = 0;

    switch (state) {
    case LED_STATE_OFF:           set_rgb(led, 0, 0, 0); break;
    case LED_STATE_RED_STEADY:    set_rgb(led, 1, 0, 0); break;
    case LED_STATE_GREEN_STEADY:  set_rgb(led, 0, 1, 0); break;
    case LED_STATE_WHITE_STEADY:  set_rgb(led, 1, 1, 1); break;
    case LED_STATE_PURPLE_STEADY: set_rgb(led, 1, 0, 1); break;
    case LED_STATE_RED_BLINKING:
    case LED_STATE_RED_DOUBLE_BLINK:
    case LED_STATE_RED_SLOW_BLINK:
    case LED_STATE_RED_FAST_BLINK:
    case LED_STATE_YELLOW_BLINKING:
    case LED_STATE_WHITE_BLINKING:
    case LED_STATE_BLUE_BLINKING:
    case LED_STATE_CYAN_BLINKING:
        start_blink(led, state);
        break;
    }

    ESP_LOGI(TAG, "LED%d -> state %d", led, state);
    return ESP_OK;
}
