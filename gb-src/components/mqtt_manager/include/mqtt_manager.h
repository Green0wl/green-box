#pragma once

#include "esp_err.h"

typedef enum {
    MQTT_REG_OK,
    MQTT_REG_REJECTED,
    MQTT_REG_MAX_RETRIES,
} mqtt_reg_result_t;

/**
 * Run the Phase 2 registration flow.
 * Reads MQTT broker host/port from NVS, connects, subscribes, publishes
 * registration request, and waits for server response.
 * Retries with exponential backoff up to 10 times.
 * Blocks until registered or all retries exhausted.
 */
esp_err_t mqtt_manager_register(mqtt_reg_result_t *result);
