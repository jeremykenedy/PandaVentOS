idf_component_register(
    SRCS
        "app_main.c"
        "pv_cfg.c"
        "pv_json.c"
        "pv_apply.c"
        "pv_wifi.c"
        "pv_bambu.c"
        "pv_rgb.c"
        "pv_motor.c"
        "pv_policy.c"
        "pv_http.c"
    INCLUDE_DIRS "."
    EMBED_FILES "factory_index.html.gz"
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_adc
        esp_driver_gpio
        esp_driver_ledc
        app_update
        mqtt
        json
)
