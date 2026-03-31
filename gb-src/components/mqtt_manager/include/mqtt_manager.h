#pragma once

#include "esp_err.h"

typedef enum {
    MQTT_REG_OK,
    MQTT_REG_REJECTED,
    MQTT_REG_MAX_RETRIES,
} mqtt_reg_result_t;

/**
 * Run the Phase 2 registration flow.
 * Keeps the MQTT client connected on success for Phase 3+4.
 */
esp_err_t mqtt_manager_register(mqtt_reg_result_t *result);

/**
 * Run Phase 3+4: subscribe to config/push, handle configs, run watering timers.
 * Blocks indefinitely. Must be called after successful registration.
 */
esp_err_t mqtt_manager_run(void);
