#include "nvs_manager.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#define NVS_NAMESPACE "greenbox"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PWD  "wifi_pwd"
#define KEY_MQTT_HOST "mqtt_host"
#define KEY_MQTT_PORT "mqtt_port"
#define KEY_CFG_ID_FMT   "cfg_id_%d"
#define KEY_CFG_SCHED_FMT "cfg_sch_%d"
#define KEY_CFG_HUM_FMT   "cfg_hum_%d"

static const char *TAG = "nvs_manager";

esp_err_t nvs_manager_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t nvs_manager_set_wifi(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_WIFI_PWD, password);
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_get_wifi(char *ssid, size_t ssid_len, char *password, size_t pwd_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, KEY_WIFI_SSID, ssid, &ssid_len);
    if (err == ESP_OK) err = nvs_get_str(handle, KEY_WIFI_PWD, password, &pwd_len);

    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_set_mqtt(const char *host, uint16_t port)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, KEY_MQTT_HOST, host);
    if (err == ESP_OK) err = nvs_set_u16(handle, KEY_MQTT_PORT, port);
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_get_mqtt(char *host, size_t host_len, uint16_t *port)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, KEY_MQTT_HOST, host, &host_len);
    if (err == ESP_OK) err = nvs_get_u16(handle, KEY_MQTT_PORT, port);

    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_set_port_config(int port, const char *config_id,
                                      const char *schedule_json, int humidity_pct)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char key[16];
    snprintf(key, sizeof(key), KEY_CFG_ID_FMT, port);
    err = nvs_set_str(handle, key, config_id);

    if (err == ESP_OK) {
        snprintf(key, sizeof(key), KEY_CFG_SCHED_FMT, port);
        err = nvs_set_str(handle, key, schedule_json);
    }
    if (err == ESP_OK) {
        snprintf(key, sizeof(key), KEY_CFG_HUM_FMT, port);
        err = nvs_set_i32(handle, key, humidity_pct);
    }
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_get_port_config(int port, char *config_id, size_t cid_len,
                                      char *schedule_json, size_t sj_len, int *humidity_pct)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    char key[16];
    snprintf(key, sizeof(key), KEY_CFG_ID_FMT, port);
    err = nvs_get_str(handle, key, config_id, &cid_len);

    if (err == ESP_OK) {
        snprintf(key, sizeof(key), KEY_CFG_SCHED_FMT, port);
        err = nvs_get_str(handle, key, schedule_json, &sj_len);
    }
    if (err == ESP_OK) {
        snprintf(key, sizeof(key), KEY_CFG_HUM_FMT, port);
        int32_t val;
        err = nvs_get_i32(handle, key, &val);
        if (err == ESP_OK) *humidity_pct = (int)val;
    }

    nvs_close(handle);
    return err;
}
