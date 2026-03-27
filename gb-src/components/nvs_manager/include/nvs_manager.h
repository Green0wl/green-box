#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t nvs_manager_init(void);
esp_err_t nvs_manager_set_wifi(const char *ssid, const char *password);
esp_err_t nvs_manager_get_wifi(char *ssid, size_t ssid_len, char *password, size_t pwd_len);
esp_err_t nvs_manager_set_mqtt(const char *host, uint16_t port);
esp_err_t nvs_manager_get_mqtt(char *host, size_t host_len, uint16_t *port);
