#pragma once

#include "esp_err.h"

typedef void (*rst_button_callback_t)(void);

/**
 * Start monitoring the RST button on IO12.
 * When held HIGH for >= 5 seconds, calls the callback.
 */
esp_err_t rst_button_start(rst_button_callback_t on_reset);
