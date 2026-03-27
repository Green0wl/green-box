#pragma once

#include "esp_err.h"

typedef enum {
    LED_NETWORK,
    LED_APPLICATION,
} led_id_t;

typedef enum {
    LED_STATE_OFF,
    LED_STATE_RED_STEADY,
    LED_STATE_RED_BLINKING,
    LED_STATE_RED_DOUBLE_BLINK,
    LED_STATE_RED_SLOW_BLINK,
    LED_STATE_RED_FAST_BLINK,
    LED_STATE_GREEN_STEADY,
    LED_STATE_YELLOW_BLINKING,
    LED_STATE_ORANGE_STEADY,
    LED_STATE_ORANGE_BLINKING,
    LED_STATE_PURPLE_STEADY,
    LED_STATE_BLUE_BLINKING,
    LED_STATE_CYAN_BLINKING,
} led_state_t;

esp_err_t led_driver_init(void);
esp_err_t led_driver_set(led_id_t led, led_state_t state);
