#include "led_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "led_driver";

/*
 * GPIO pins are placeholders until the PCB is wired.
 * Each RGB LED uses 3 channels (R, G, B).
 */
#define LED1_R_GPIO 25
#define LED1_G_GPIO 26
#define LED1_B_GPIO 27
#define LED2_R_GPIO 14
#define LED2_G_GPIO 12
#define LED2_B_GPIO 13

static TimerHandle_t blink_timers[2];
static led_state_t current_state[2];
static uint8_t blink_phase[2];

static void set_rgb(led_id_t led, uint8_t r, uint8_t g, uint8_t b)
{
    /* TODO: drive actual GPIOs via LEDC PWM once hardware is available */
    ESP_LOGD(TAG, "LED%d: R=%d G=%d B=%d", led, r, g, b);
    (void)led; (void)r; (void)g; (void)b;
}

typedef struct {
    uint8_t r, g, b;
    uint16_t period_ms;
} blink_color_t;

static blink_color_t get_blink_color(led_state_t state)
{
    switch (state) {
    case LED_STATE_RED_BLINKING:      return (blink_color_t){255, 0,   0,   500};
    case LED_STATE_RED_DOUBLE_BLINK:  return (blink_color_t){255, 0,   0,   200};
    case LED_STATE_RED_SLOW_BLINK:    return (blink_color_t){255, 0,   0,   1000};
    case LED_STATE_RED_FAST_BLINK:    return (blink_color_t){255, 0,   0,   150};
    case LED_STATE_YELLOW_BLINKING:   return (blink_color_t){255, 180, 0,   500};
    case LED_STATE_ORANGE_BLINKING:   return (blink_color_t){255, 100, 0,   500};
    case LED_STATE_BLUE_BLINKING:     return (blink_color_t){0,   0,   255, 500};
    case LED_STATE_CYAN_BLINKING:     return (blink_color_t){0,   255, 255, 500};
    default:                          return (blink_color_t){0,   0,   0,   500};
    }
}

static void blink_callback(TimerHandle_t timer)
{
    led_id_t led = (led_id_t)(uintptr_t)pvTimerGetTimerID(timer);
    blink_color_t c = get_blink_color(current_state[led]);

    /*
     * Double-blink pattern: ON-OFF-ON-OFF---pause
     * Phases 0,2 = on; 1,3 = off; 4..7 = pause (off)
     */
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

    ESP_LOGI(TAG, "LED driver initialized (GPIO stubs)");
    return ESP_OK;
}

esp_err_t led_driver_set(led_id_t led, led_state_t state)
{
    if (led > LED_APPLICATION) return ESP_ERR_INVALID_ARG;

    xTimerStop(blink_timers[led], 0);
    current_state[led] = state;
    blink_phase[led] = 0;

    switch (state) {
    case LED_STATE_OFF:
        set_rgb(led, 0, 0, 0);
        break;
    case LED_STATE_RED_STEADY:
        set_rgb(led, 255, 0, 0);
        break;
    case LED_STATE_GREEN_STEADY:
        set_rgb(led, 0, 255, 0);
        break;
    case LED_STATE_ORANGE_STEADY:
        set_rgb(led, 255, 100, 0);
        break;
    case LED_STATE_PURPLE_STEADY:
        set_rgb(led, 128, 0, 128);
        break;
    case LED_STATE_RED_BLINKING:
    case LED_STATE_RED_DOUBLE_BLINK:
    case LED_STATE_RED_SLOW_BLINK:
    case LED_STATE_RED_FAST_BLINK:
    case LED_STATE_YELLOW_BLINKING:
    case LED_STATE_ORANGE_BLINKING:
    case LED_STATE_BLUE_BLINKING:
    case LED_STATE_CYAN_BLINKING:
        start_blink(led, state);
        break;
    }

    ESP_LOGI(TAG, "LED%d -> state %d", led, state);
    return ESP_OK;
}
