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
        "pv_log.c"
        "pv_anim.c"
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

# Test-only instrumentation for the vent policy. OFF unless the environment
# variable is set at configure time, which nothing in the tree does:
#
#   PV_POLICY_TEST_HOOK=1 idf.py fullclean build     # instrumented
#   idf.py fullclean build                           # shipping
#
# It exists because the sealing branch of the policy cannot be driven on a
# machine that only has PLA loaded. Verify a shipping image with:
#
#   strings build/panda_vent.bin | grep -c "TEST: material"    -> 0
if(DEFINED ENV{PV_POLICY_TEST_HOOK})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PV_POLICY_TEST_HOOK=1)
    message(WARNING "PV_POLICY_TEST_HOOK is ON: this is a TEST image, do not ship it")
endif()
