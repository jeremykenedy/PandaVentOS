// Config model + persistence. One NVS blob, factory defaults exactly as the
// stock app ships them (colors verified against a live stock device's state
// push and the factory manual's defaults table).
#include "pv.h"

#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pv_cfg";

#define CFG_NS    "pv"
#define CFG_KEY   "cfg"
#define CFG_MAGIC 0x50564331   // "PVC1"

pv_cfg_t  g_cfg;
pv_live_t g_live = {
    .sta_state = 1, .printer_state = 0, .device_state = PV_ST_IDLE,
    .bed_temp = -1.0f, .nozzle_temp = -1.0f,
};

static void fx_set(pv_fx_param_t *p, const char *color)
{
    p->brightness = 50;
    p->speed = 50;
    snprintf(p->color, sizeof(p->color), "%s", color);
}

void pv_cfg_rgb_mode_defaults(pv_rgb_cfg_t *r, int mode)
{
    if (mode == PV_MODE_SIMPLE) {
        r->simple_current = PV_FX_STATIC;
        for (int i = 0; i < PV_FX_COUNT; ++i) fx_set(&r->simple[i], "FF3700");
    } else if (mode == PV_MODE_H2D) {
        // Factory per-state defaults, taken from the default colour table in
        // the stock image at file offset 0x1707c (six RGB triples: FFFFFF,
        // FF8000, FFFFFF, FFFFFF, 00FF00, FF0000) and confirmed against a
        // live stock unit. Note the printed manual lists Preparation as
        // F8A323 and Completed as 00FF2A; the shipping firmware does not use
        // those values, and neither does a real device. Active effect per
        // state is Static except Printing, which ships on Rainbow.
        static const char *state_color[PV_ST_COUNT] = {
            "FFFFFF", "FF8000", "FFFFFF", "FFFFFF", "00FF00", "FF0000",
        };
        static const uint8_t state_fx[PV_ST_COUNT] = {
            PV_FX_STATIC, PV_FX_STATIC, PV_FX_RAINBOW,
            PV_FX_STATIC, PV_FX_STATIC, PV_FX_STATIC,
        };
        for (int s = 0; s < PV_ST_COUNT; ++s) {
            r->h2d_active[s] = state_fx[s];
            for (int f = 0; f < PV_FX_COUNT; ++f)
                fx_set(&r->h2d[s][f], state_color[s]);
        }
    } else if (mode == PV_MODE_WARNING) {
        for (int lvl = 0; lvl < 2; ++lvl) {
            r->warnhot_current[lvl] = PV_FX_STATIC;
            for (int fx = 0; fx < 2; ++fx) {
                r->warnhot_bg[lvl][fx] = 50;
                r->warnhot_speed[lvl][fx] = 50;
            }
        }
    }
}

void pv_cfg_factory_defaults(pv_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->magic = CFG_MAGIC;
    c->rgb.light_on = true;
    c->rgb.warning_sw = true;
    c->rgb.follow_printer = false;
    c->rgb.follow_vent = true;
    c->rgb.reverse = false;
    c->rgb.light_mode = PV_MODE_SIMPLE;
    pv_cfg_rgb_mode_defaults(&c->rgb, PV_MODE_SIMPLE);
    pv_cfg_rgb_mode_defaults(&c->rgb, PV_MODE_H2D);
    pv_cfg_rgb_mode_defaults(&c->rgb, PV_MODE_WARNING);
    // Stock AP. The ssid is left empty here on purpose: stock builds it at
    // run time as "Panda_Vent_" + the six STA MAC bytes in uppercase hex
    // (format string "%s%02X%02X%02X%02X%02X%02X" in the stock image, and a
    // live unit with STA MAC AA:BB:CC:DD:EE:10 advertises
    // "Panda_Vent_AABBCCDDEE10"). pv_wifi_start() fills it the same way.
    c->ap.ssid[0] = '\0';
    snprintf(c->ap.password, sizeof(c->ap.password), "987654321");
    snprintf(c->ap.ip, sizeof(c->ap.ip), "192.168.254.1");
    c->ap.on = true;
    c->hostname[0] = '\0';           // empty = derive from AP suffix
    snprintf(c->language, sizeof(c->language), "en");
    c->motor_manual = false;         // AUTO is the factory default
    c->motor_manual_open = false;
}

void pv_cfg_load(void)
{
    pv_cfg_factory_defaults(&g_cfg);
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config, factory defaults");
        return;
    }
    pv_cfg_t stored;
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_blob(h, CFG_KEY, &stored, &size);
    nvs_close(h);
    if (err == ESP_OK && size == sizeof(stored) && stored.magic == CFG_MAGIC) {
        g_cfg = stored;
        ESP_LOGI(TAG, "config loaded (%u B)", (unsigned)size);
    } else {
        ESP_LOGW(TAG, "stored config unusable (err=%d size=%u), defaults kept",
                 err, (unsigned)size);
    }
}

void pv_cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %d", err); return; }
    err = nvs_set_blob(h, CFG_KEY, &g_cfg, sizeof(g_cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %d", err);
}

void pv_factory_reset_and_reboot(void)
{
    // The factory manual: reset clears Wi-Fi, printer binding, lighting, and
    // preferences. Wipe the whole NVS partition (esp_wifi creds included).
    ESP_LOGW(TAG, "FACTORY RESET");
    nvs_flash_erase();
    esp_restart();
}
