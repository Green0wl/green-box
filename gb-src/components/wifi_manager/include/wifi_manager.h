#pragma once

#include "esp_err.h"

typedef enum {
    WIFI_RESULT_OK,
    WIFI_RESULT_WRONG_PASSWORD,
    WIFI_RESULT_AP_NOT_FOUND,
    WIFI_RESULT_TIMEOUT,
} wifi_connect_result_t;

/**
 * Initialize WiFi in APSTA mode and start the soft-AP.
 * Also initializes netif and the default event loop.
 */
esp_err_t wifi_manager_init(void);

/**
 * Attempt STA connection with the given credentials.
 * Blocks until connected, rejected, or timed out (~15 s).
 */
esp_err_t wifi_manager_try_connect(const char *ssid, const char *password,
                                   wifi_connect_result_t *result);

/**
 * Switch from APSTA to STA-only mode, tearing down the soft-AP.
 */
esp_err_t wifi_manager_stop_ap(void);

/**
 * Stop WiFi entirely (AP + STA). Used for hibernate.
 */
esp_err_t wifi_manager_stop(void);

/**
 * Restart WiFi in APSTA mode after hibernate.
 */
esp_err_t wifi_manager_restart_ap(void);
