// Config model + persistence. One NVS blob, factory defaults exactly as the
// stock app ships them (colors verified against a live stock device's state
// push and the factory manual's defaults table).
#include "pv.h"

#include <stddef.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pv_cfg";

#define CFG_NS    "pv"
#define CFG_KEY   "cfg"
#define CFG_MAGIC 0x50564333   // "PVC3": device_name appended
#define CFG_MAGIC_V2 0x50564332   // "PVC2": nine effects, no device_name
#define CFG_MAGIC_V1 0x50564331   // "PVC1": the seven effect layout

// The v1 layout, kept verbatim so a device that has been running since before
// Cylon and Bounce existed keeps its settings.
//
// PV_FX_COUNT is an array dimension in the middle of the config struct, so
// growing it did not append to the blob, it moved everything after
// rgb.simple. A size check alone would have thrown away all 65 settings on
// the first boot after the update, silently, and the only clue would have
// been one line in a serial log nobody was watching.
#define PV_FX_COUNT_V1 7

typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_t simple[PV_FX_COUNT_V1];
    uint8_t h2d_active[PV_ST_COUNT];
    pv_fx_param_t h2d[PV_ST_COUNT][PV_FX_COUNT_V1];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
} pv_rgb_cfg_v1_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v1_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
} pv_cfg_v1_t;

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
    snprintf(c->device_name, sizeof(c->device_name), "%s", PV_DEVICE_NAME_DEFAULT);
    c->motor_manual = false;         // AUTO is the factory default
    c->motor_manual_open = false;
}

// DELIBERATE DEPARTURE, documented rather than silent.
//
// Stock does NOT validate. Its store is one nvs_get_blob per subsystem
// through the wrapper at 0x400d8cdc (namespace "app_nvs", keys "sign",
// "wifi_info", "bambu_mqtt_info", "ui_info", "sys_rgb_mode" 0x138 bytes,
// "rgb_sundry" 6, "key_mode" 4), and the loader at 0x400d8e38 issues the
// three reads and returns 1 with no magic, no version and no range check on
// any field.
//
// We keep the magic AND clamp, for one concrete reason: resolve() indexes
// warnhot_bg[lvl][warnhot_current[lvl]] and warnhot_speed likewise, both
// [2][2]. pv_apply.c masks the wire value with & 1 so a message cannot break
// it, but nothing clamps what comes back from flash, and pv_cfg_load copies
// the blob wholesale. A corrupt-but-magic-valid blob therefore reads out of
// bounds. Stock has the same shape of exposure and lives with it; we do not.
static void cfg_clamp_loaded(void)
{
    pv_rgb_cfg_t *r = &g_cfg.rgb;
    if (r->light_mode > PV_MODE_WARNING)   r->light_mode = PV_MODE_SIMPLE;
    if (r->simple_current >= PV_FX_COUNT)  r->simple_current = PV_FX_STATIC;
    for (int st = 0; st < PV_ST_COUNT; ++st)
        if (r->h2d_active[st] >= PV_FX_COUNT) r->h2d_active[st] = PV_FX_STATIC;
    // The one that is actually reachable as an out-of-bounds READ.
    for (int lvl = 0; lvl < 2; ++lvl)
        if (r->warnhot_current[lvl] > 1) r->warnhot_current[lvl] = 0;
}

void pv_cfg_load(void)
{
    pv_cfg_factory_defaults(&g_cfg);
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config, factory defaults");
        return;
    }
    // Big enough for either layout. nvs_get_blob fills in the stored length,
    // and the magic says which shape those bytes are.
    union { pv_cfg_t v3; pv_cfg_v1_t v1; } stored;
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_blob(h, CFG_KEY, &stored, &size);
    nvs_close(h);

    if (err == ESP_OK && size == sizeof(pv_cfg_t) &&
        stored.v3.magic == CFG_MAGIC) {
        g_cfg = stored.v3;
        cfg_clamp_loaded();
        ESP_LOGI(TAG, "config loaded (%u B)", (unsigned)size);
        return;
    }

    // v2 -> v3. device_name was APPENDED, so every field before it kept its
    // offset and the whole prefix copies straight across. Copying exactly
    // offsetof(device_name) bytes rather than the stored size is what keeps
    // v2's trailing struct padding out of the new field.
    if (err == ESP_OK && stored.v3.magic == CFG_MAGIC_V2 &&
        size >= offsetof(pv_cfg_t, device_name) && size <= sizeof(pv_cfg_t)) {
        memcpy(&g_cfg, &stored.v3, offsetof(pv_cfg_t, device_name));
        g_cfg.magic = CFG_MAGIC;
        // device_name keeps the default pv_cfg_factory_defaults already put
        // there, which is the string stock hard-codes into the web app.
        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v2 -> v3 (%u B -> %u B), device_name defaulted",
                 (unsigned)size, (unsigned)sizeof(pv_cfg_t));
        pv_cfg_save();
        return;
    }

    if (err == ESP_OK && size == sizeof(pv_cfg_v1_t) &&
        stored.v1.magic == CFG_MAGIC_V1) {
        // Seven effects becoming nine moved everything after rgb.simple, so
        // this copies field by field. g_cfg already holds factory defaults,
        // which means the two new effect slots arrive correctly filled and
        // only the seven that existed get overwritten.
        const pv_rgb_cfg_v1_t *o = &stored.v1.rgb;
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT_V1; ++i)
            g_cfg.rgb.simple[i] = o->simple[i];
        for (int st = 0; st < PV_ST_COUNT; ++st) {
            g_cfg.rgb.h2d_active[st] = o->h2d_active[st];
            for (int f = 0; f < PV_FX_COUNT_V1; ++f)
                g_cfg.rgb.h2d[st][f] = o->h2d[st][f];
        }
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));

        g_cfg.printer           = stored.v1.printer;
        g_cfg.ap                = stored.v1.ap;
        memcpy(g_cfg.hostname, stored.v1.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v1.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v1.motor_manual;
        g_cfg.motor_manual_open = stored.v1.motor_manual_open;
        // device_name did not exist in v1 either; it keeps its default.
        g_cfg.magic             = CFG_MAGIC;

        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v1 -> v3 (%u B -> %u B), %d effects -> %d",
                 (unsigned)size, (unsigned)sizeof(pv_cfg_t),
                 PV_FX_COUNT_V1, PV_FX_COUNT);
        pv_cfg_save();      // write it back in the new shape straight away
        return;
    }

    ESP_LOGW(TAG, "stored config unusable (err=%d size=%u), defaults kept",
             err, (unsigned)size);
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
