// PandaVent bring-up.
//
// TWO RULES THIS FILE EXISTS TO ENFORCE, both learned the hard way:
//
// 1. NETWORK FIRST. Wi-Fi and the HTTP/OTA server come up before any
//    peripheral is touched. If a motor, ADC or LED driver misbehaves, the
//    device is still on the network and can be re-flashed wirelessly. A
//    device that cannot be reached is a device that needs a cable, and a
//    cable is not something the owner should ever need.
//
// 2. NOTHING IN THE BOOT PATH MAY ABORT. No ESP_ERROR_CHECK outside of the
//    two calls that genuinely cannot be survived (NVS and netif init).
//    Every peripheral failure is logged and stepped over. A crash loop
//    prints nothing, serves nothing, and hides the cause.
//
// Paired with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE: this image boots on
// probation and is only confirmed once it proves it reached the network
// (see health_task). If it never does, the bootloader restores the previous
// image on its own.
#include "pv.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "pandavent";

// How long a fresh image gets to prove itself before we let the bootloader
// roll it back. Generous: a cold Wi-Fi join with a slow AP can take ~30 s.
#define PV_PROBATION_SECONDS 120

static void health_task(void *arg)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    bool on_probation =
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;

    if (on_probation)
        ESP_LOGW(TAG, "image on probation: must reach the network within %d s "
                      "or the bootloader rolls back", PV_PROBATION_SECONDS);

    for (int elapsed = 0;; elapsed += 2) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        bool online = g_live.sta_state == 3;      // STA has an IP
        bool serving = pv_http_is_up();           // and we answer requests

        if (on_probation && online && serving) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "image confirmed good (ip %s), rollback cancelled",
                     g_live.sta_ip);
            on_probation = false;
        }

        if (on_probation && elapsed >= PV_PROBATION_SECONDS) {
            ESP_LOGE(TAG, "never reached the network in %d s; rolling back to "
                          "the previous firmware", PV_PROBATION_SECONDS);
            esp_ota_mark_app_invalid_rollback_and_reboot();   // does not return
        }

        // Past probation this task is just a heartbeat for the AP-only case:
        // a device with no saved Wi-Fi is not broken, it is waiting for setup.
        if (!on_probation && elapsed % 60 == 0)
            ESP_LOGI(TAG, "up: sta=%d ip=%s printer=%d",
                     g_live.sta_state, g_live.sta_ip, g_live.printer_state);
    }
}

void app_main(void)
{
    // NVS first: everything reads config, and a read before nvs_flash_init()
    // silently returns compiled defaults instead of failing loudly.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs unusable (%s), erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));

    pv_cfg_load();

    // Probation watchdog starts before anything can go wrong.
    xTaskCreate(health_task, "pv_health", 3072, NULL, 2, NULL);

    // ---- network first ----
    pv_wifi_start();
    if (pv_http_start() != ESP_OK)
        ESP_LOGE(TAG, "http server did not start");

    // ---- then the hardware; failures here must not take the device off air ----
    pv_rgb_start();
    pv_motor_start();
    pv_bambu_start();

    ESP_LOGI(TAG, "boot complete: hostname=%s ap=%s mode=%s",
             g_cfg.hostname[0] ? g_cfg.hostname : "(default)",
             g_cfg.ap.ssid, g_cfg.motor_manual ? "MANUAL" : "AUTO");
}
