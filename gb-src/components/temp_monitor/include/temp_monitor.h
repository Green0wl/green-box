#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    void (*on_hibernate_enter)(void);
    void (*on_hibernate_exit)(void);
} temp_monitor_callbacks_t;

/**
 * Start the temperature monitoring task.
 * Simulates overheat cycle: 2 min normal, 1 min hibernate.
 * During hibernate: calls on_hibernate_enter (to stop WiFi/server),
 * sets LED 2 = purple. On exit: calls on_hibernate_exit (to restart them).
 */
esp_err_t temp_monitor_start(const temp_monitor_callbacks_t *callbacks);

/**
 * Returns true if the device is in hibernate (overheat) state.
 */
bool temp_monitor_is_hibernating(void);

/**
 * Stop the temperature monitoring task.
 */
void temp_monitor_stop(void);
