idf_component_register(SRCS "web_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server freertos
                       PRIV_REQUIRES wifi_manager nvs_manager led_driver temp_monitor lwip
                       EMBED_FILES "provisioning.html")
