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

# The web app is ONE editable file, and the build makes the gzip.
#
# It used to be the other way round: the repository held only
# factory_index.html.gz and the readable source lived nowhere the project could
# see. Editing the UI meant having the un-gzipped copy from somewhere else,
# which is not a thing a repository should require of anyone.
#
# Edit main/ui.html. This regenerates the gzip whenever it is newer, before the
# embed reads it, so there is no second step to remember and no way to ship a
# page that does not match its source.
#
# gzip -9 -n: -n leaves the source name and timestamp out of the header, so the
# same input always produces byte-identical output and a rebuild that changed
# nothing does not show up as a changed binary.
find_program(GZIP_EXE gzip REQUIRED)
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/factory_index.html.gz"
    COMMAND "${GZIP_EXE}" -9 -n -c "${CMAKE_CURRENT_SOURCE_DIR}/ui.html"
            > "${CMAKE_CURRENT_SOURCE_DIR}/factory_index.html.gz"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui.html"
    COMMENT "gzipping ui.html -> factory_index.html.gz"
    VERBATIM)
add_custom_target(pv_ui DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/factory_index.html.gz")
add_dependencies(${COMPONENT_LIB} pv_ui)

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

# Test-only frame dump, same rule and the same switch shape as the hook above:
#
#   PV_FXDUMP=1 idf.py fullclean build     # instrumented
#   idf.py fullclean build                 # shipping
#
# It writes every rendered frame to the log as hex, so an effect can be checked
# against private/SPEC/effects-math.md from what the DEVICE painted rather than
# from a host build of the same source. Verify a shipping image with:
#
#   strings build/panda_vent.bin | grep -c "FXD %d"            -> 0
if(DEFINED ENV{PV_FXDUMP})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PV_FXDUMP=1)
    message(WARNING "PV_FXDUMP is ON: this is a TEST image, do not ship it")
endif()
