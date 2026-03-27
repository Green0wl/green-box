#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WEB_SERVER_PROV_DONE_BIT BIT0

/**
 * Start the provisioning HTTP server on port 80.
 * Sets WEB_SERVER_PROV_DONE_BIT on @p prov_done when WiFi connects successfully.
 */
esp_err_t web_server_start(EventGroupHandle_t prov_done);

/**
 * Stop the HTTP server and free resources.
 */
esp_err_t web_server_stop(void);
