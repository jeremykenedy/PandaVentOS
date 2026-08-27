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
        // Factory defaults, read back from a stock device after sending it
        // the factory app's own reset command, {"rgb_mode":{"reset":1}}.
        // This is not inferred from the manual or the UI: it is what the
        // shipping firmware writes.
        //
        // Every effect of every state starts WHITE. Only the state's default
        // ("active") effect carries a signature colour, and only three states
        // have one that is not white:
        //
        //   Idle              active 1 Breathing   all FFFFFF
        //   Download/Prepare  active 4 Marquee     effect 4 -> FF8000
        //   Printing          active 6 Rainbow     all FFFFFF
        //   Paused            active 1 Breathing   all FFFFFF
        //   Finished          active 0 Static      effect 0 -> 00FF00
        //   Error             active 2 Strobing    effect 2 -> FF0000
        //
        // The stock image builds it the same way: a loop writes white to all
        // seven slots, then a switch overwrites the active slot for states 1,
        // 4 and 5 only (0x400dc845 onward, colour literals at file offsets
        // 0x17060 and the table at 0x1707c).
        //
        // Note the printed manual is wrong here twice over: it lists
        // Preparation as F8A323 and Completed as 00FF2A, and neither value
        // exists anywhere in the shipping firmware.
        static const uint8_t state_fx[PV_ST_COUNT] = {
            PV_FX_BREATHING, PV_FX_MARQUEE, PV_FX_RAINBOW,
            PV_FX_BREATHING, PV_FX_STATIC,  PV_FX_STROBING,
        };
        static const char *active_color[PV_ST_COUNT] = {
            "FFFFFF", "FF8000", "FFFFFF", "FFFFFF", "00FF00", "FF0000",
        };
        for (int st = 0; st < PV_ST_COUNT; ++st) {
            r->h2d_active[st] = state_fx[st];
            for (int f = 0; f < PV_FX_COUNT; ++f)
                fx_set(&r->h2d[st][f], "FFFFFF");
            fx_set(&r->h2d[st][state_fx[st]], active_color[st]);
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
