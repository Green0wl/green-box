idf_component_register(SRCS "mqtt_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqtt nvs_manager led_driver freertos esp_event)
