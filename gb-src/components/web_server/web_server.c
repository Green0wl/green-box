/*
 * Provisioning HTTP server (port 80).
 *
 * Active only during Phase 1, before the device has WAN connectivity. The
 * embedded HTML form (provisioning.html) is served as the index; the user's
 * POST to /connect drives wifi_manager + nvs_manager + a TCP probe to the
 * MQTT broker before signalling app_main via an event group.
 *
 * GET /status reports the temperature monitor's hibernate flag so the
 * frontend can disable the form during overheat.
 */

#include "web_server.h"
#include "wifi_manager.h"
#include "nvs_manager.h"
#include "led_driver.h"
#include "temp_monitor.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web_server";

static httpd_handle_t s_server;
static EventGroupHandle_t s_prov_done;

extern const uint8_t provisioning_html_start[] asm("_binary_provisioning_html_start");
extern const uint8_t provisioning_html_end[]   asm("_binary_provisioning_html_end");

/* ---- URL decoding ---- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 1; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            int hi = hex_val(src[si + 1]);
            int lo = hex_val(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)(hi * 16 + lo);
                si += 2;
                continue;
            }
        } else if (src[si] == '+') {
            dst[di++] = ' ';
            continue;
        }
        dst[di++] = src[si];
    }
    dst[di] = '\0';
}

/* ---- Form field extraction ---- */

static bool get_form_field(const char *body, const char *key, char *value, size_t value_size)
{
    size_t key_len = strlen(key);
    const char *p = body;

    while ((p = strstr(p, key)) != NULL) {
        /* Check it's at start or preceded by '&' */
        if (p != body && *(p - 1) != '&') {
            p += key_len;
            continue;
        }
        if (p[key_len] != '=') {
            p += key_len;
            continue;
        }

        const char *val_start = p + key_len + 1;
        const char *val_end = strchr(val_start, '&');
        size_t raw_len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);

        char raw[256];
        if (raw_len >= sizeof(raw)) raw_len = sizeof(raw) - 1;
        memcpy(raw, val_start, raw_len);
        raw[raw_len] = '\0';

        url_decode(value, raw, value_size);
        return true;
    }
    return false;
}

/* ---- Handlers ---- */

static esp_err_t get_root_handler(httpd_req_t *req)
{
    size_t len = provisioning_html_end - provisioning_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)provisioning_html_start, len);
}

/*
 * TCP-probe the MQTT broker before declaring provisioning successful. We
 * deliberately do this from the device side so the user gets immediate
 * feedback if they typed a wrong host/port — much better UX than failing
 * later in the MQTT registration retry loop.
 */
static bool check_mqtt_reachable(const char *host, uint16_t port)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "MQTT broker DNS lookup failed: %s", host);
        return false;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return false;
    }

    struct timeval tv = { .tv_sec = 5 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = (connect(sock, res->ai_addr, res->ai_addrlen) == 0);
    close(sock);
    freeaddrinfo(res);

    if (ok)
        ESP_LOGI(TAG, "MQTT broker reachable at %s:%u", host, port);
    else
        ESP_LOGW(TAG, "MQTT broker unreachable at %s:%u", host, port);

    return ok;
}

static esp_err_t get_status_handler(httpd_req_t *req)
{
    bool hib = temp_monitor_is_hibernating();
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"hibernating\":%s}", hib ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/*
 * POST /connect — the heart of provisioning.
 *  1. Refuse if the device is in hibernate (overheat).
 *  2. Parse SSID, password, MQTT host/port from the urlencoded body.
 *  3. Persist to NVS so reboots don't require re-provisioning.
 *  4. Try the WiFi connection (blocks ~15 s).
 *  5. On WiFi success, TCP-probe the MQTT broker.
 *  6. On full success, signal app_main via the event group.
 */
static esp_err_t post_connect_handler(httpd_req_t *req)
{
    if (temp_monitor_is_hibernating()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"status\":\"error\",\"reason\":\"overheat\"}", HTTPD_RESP_USE_STRLEN);
    }

    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    char mqtt_host[128] = {0};
    char mqtt_port_str[6] = {0};
    char tb_token[64] = {0};   /* optional, may stay empty */

    if (!get_form_field(buf, "ssid", ssid, sizeof(ssid)) ||
        !get_form_field(buf, "password", password, sizeof(password)) ||
        !get_form_field(buf, "mqtt_host", mqtt_host, sizeof(mqtt_host)) ||
        !get_form_field(buf, "mqtt_port", mqtt_port_str, sizeof(mqtt_port_str))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing fields");
        return ESP_FAIL;
    }

    /* Optional: ThingsBoard access token (cloud telemetry off if empty) */
    get_form_field(buf, "tb_token", tb_token, sizeof(tb_token));

    uint16_t mqtt_port = (uint16_t)atoi(mqtt_port_str);

    /* Store in NVS */
    nvs_manager_set_wifi(ssid, password);
    nvs_manager_set_mqtt(mqtt_host, mqtt_port);
    nvs_manager_set_tb_token(tb_token);

    /* LED: connecting */
    led_driver_set(LED_NETWORK, LED_STATE_RED_BLINKING);

    /* Try WiFi connection */
    wifi_connect_result_t result;
    esp_err_t err = wifi_manager_try_connect(ssid, password, &result);

    httpd_resp_set_type(req, "application/json");

    if (err != ESP_OK || result != WIFI_RESULT_OK) {
        const char *reason;
        switch (result) {
        case WIFI_RESULT_WRONG_PASSWORD: reason = "wrong_password"; break;
        case WIFI_RESULT_AP_NOT_FOUND:   reason = "ap_not_found";   break;
        default:                         reason = "timeout";         break;
        }

        led_driver_set(LED_NETWORK, LED_STATE_RED_STEADY);

        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"reason\":\"%s\"}", reason);
        return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }

    /* WiFi OK — now check MQTT broker reachability */
    if (!check_mqtt_reachable(mqtt_host, mqtt_port)) {
        led_driver_set(LED_NETWORK, LED_STATE_GREEN_STEADY);
        return httpd_resp_send(req,
            "{\"status\":\"error\",\"reason\":\"mqtt_unreachable\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    led_driver_set(LED_NETWORK, LED_STATE_GREEN_STEADY);

    /* Signal app_main that provisioning succeeded */
    xEventGroupSetBits(s_prov_done, WEB_SERVER_PROV_DONE_BIT);

    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

/* ---- Public API ---- */

esp_err_t web_server_start(EventGroupHandle_t prov_done)
{
    s_prov_done = prov_done;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t get_root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = get_root_handler,
    };
    const httpd_uri_t get_status = {
        .uri     = "/status",
        .method  = HTTP_GET,
        .handler = get_status_handler,
    };
    const httpd_uri_t post_connect = {
        .uri     = "/connect",
        .method  = HTTP_POST,
        .handler = post_connect_handler,
    };

    httpd_register_uri_handler(s_server, &get_root);
    httpd_register_uri_handler(s_server, &get_status);
    httpd_register_uri_handler(s_server, &post_connect);

    ESP_LOGI(TAG, "Provisioning server started on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Server stopped");
    }
    return ESP_OK;
}
