// State documents in the exact shape, key order, and formatting the factory
// app produces (cJSON_Print, tab-indented — verified against a live stock
// capture). The UI tolerates partial documents; connect-time pushes send the
// full document exactly like stock.
#include "pv.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static cJSON *fx_param(int id_key_is_effect, int id, const pv_fx_param_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, id_key_is_effect ? "effect_id" : "id", id);
    cJSON_AddNumberToObject(o, "brightness", p->brightness);
    cJSON_AddNumberToObject(o, "speed", p->speed);
    char hex[7];
    pv_rgb3_to_hex(p->rgb, hex);
    cJSON_AddStringToObject(o, "color", hex);
    // NOT STOCK from here down. A stock device never sends these keys, and the
    // factory UI ignores keys it does not know, so they cost nothing there.
    pv_rgb3_to_hex(p->rgb_closed, hex);
    cJSON_AddStringToObject(o, "color_closed", hex);
    // The INACTIVE pair. Absent from the document when unset, which is how the
    // UI knows to show its clear button as already cleared. Named "inactive"
    // and not "bg" on purpose: stock's "bg" is the brightness percent.
    if (p->opt_set & PV_BG_OPEN) {
        pv_rgb3_to_hex(p->bg, hex);
        cJSON_AddStringToObject(o, "inactive", hex);
    }
    // Absent when the ramp is unset, which is how the UI knows to show its
    // clear button as already cleared.
    if (p->opt_set & PV_BRIGHT_END)
        cJSON_AddNumberToObject(o, "bright_end", p->bright_end);
    if (p->opt_set & PV_BG_CLOSED) {
        pv_rgb3_to_hex(p->bg_closed, hex);
        cJSON_AddStringToObject(o, "inactive_closed", hex);
    }
    return o;
}

char *pv_json_state(void)
{
    cJSON *root = cJSON_CreateObject();

    // wifi
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", g_live.sta_ssid);
    cJSON_AddStringToObject(wifi, "password", g_live.sta_password);
    cJSON_AddNumberToObject(wifi, "scan", g_live.wifi_scan);

    // sta
    cJSON *sta = cJSON_AddObjectToObject(root, "sta");
    cJSON_AddStringToObject(sta, "hostname", g_cfg.hostname);
    cJSON_AddStringToObject(sta, "ip", g_live.sta_ip);
    cJSON_AddNumberToObject(sta, "state", g_live.sta_state);
    cJSON_AddNumberToObject(sta, "auth_err_reason", 0);

    // ap
    cJSON *ap = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddStringToObject(ap, "ssid", g_cfg.ap.ssid);
    cJSON_AddStringToObject(ap, "password", g_cfg.ap.password);
    cJSON_AddStringToObject(ap, "ip", g_cfg.ap.ip);
    cJSON_AddNumberToObject(ap, "on", g_cfg.ap.on ? 1 : 0);

    // printer
    cJSON *pr = cJSON_AddObjectToObject(root, "printer");
    cJSON_AddStringToObject(pr, "name", g_cfg.printer.name);
    cJSON_AddStringToObject(pr, "sn", g_cfg.printer.sn);
    cJSON_AddStringToObject(pr, "access_code", g_cfg.printer.access_code);
    cJSON_AddStringToObject(pr, "ip", g_cfg.printer.ip);
    cJSON_AddNumberToObject(pr, "state", g_live.printer_state);
    cJSON_AddNumberToObject(pr, "scan", g_live.printer_scan);

    // rgb_mode
    cJSON *rm = cJSON_AddObjectToObject(root, "rgb_mode");
    cJSON_AddNumberToObject(rm, "rgb_light_mode", g_cfg.rgb.light_mode);
    cJSON_AddBoolToObject(rm, "light_on_off", g_cfg.rgb.light_on);
    cJSON_AddBoolToObject(rm, "warning_sw", g_cfg.rgb.warning_sw);
    cJSON_AddBoolToObject(rm, "is_follow_printer", g_cfg.rgb.follow_printer);
    cJSON_AddBoolToObject(rm, "is_follow_vent", g_cfg.rgb.follow_vent);
    // NOT STOCK. A live preview is running; the page shows a countdown and an
    // obvious way out. Absent when nothing is being previewed.
    if (pv_rgb_preview_left() > 0) {
        cJSON_AddNumberToObject(rm, "preview_left", pv_rgb_preview_left());
        // Absent unless the preview pinned one, so "not pinned" and "pinned to
        // zero" stay different things.
        if (pv_rgb_preview_state() >= 0)
            cJSON_AddNumberToObject(rm, "preview_state", pv_rgb_preview_state());
        if (pv_rgb_preview_percent() >= 0)
            cJSON_AddNumberToObject(rm, "preview_percent", pv_rgb_preview_percent());
    }
    cJSON_AddBoolToObject(rm, "is_reverse", g_cfg.rgb.reverse);
    cJSON_AddNumberToObject(rm, "current_simple_effect", g_cfg.rgb.simple_current);
    cJSON *effects = cJSON_AddArrayToObject(rm, "effects");
    for (int i = 0; i < PV_FX_COUNT; ++i)
        cJSON_AddItemToArray(effects, fx_param(0, i, &g_cfg.rgb.simple[i]));

    cJSON *h2d = cJSON_AddObjectToObject(rm, "h2d_mode");
    cJSON *ds = cJSON_AddArrayToObject(h2d, "device_states");
    for (int s = 0; s < PV_ST_COUNT; ++s) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddNumberToObject(st, "device_state_id", s);
        cJSON_AddNumberToObject(st, "active_effect_id", g_cfg.rgb.h2d_active[s]);
        cJSON *efs = cJSON_AddArrayToObject(st, "effects");
        for (int f = 0; f < PV_FX_COUNT; ++f)
            cJSON_AddItemToArray(efs, fx_param(1, f, &g_h2d[s][f]));
        cJSON_AddItemToArray(ds, st);
    }

    cJSON *wh = cJSON_AddObjectToObject(rm, "warning_hot_mode");
    static const char *lvl_name[2] = { "safe", "warn" };
    for (int lvl = 0; lvl < 2; ++lvl) {
        cJSON *l = cJSON_AddObjectToObject(wh, lvl_name[lvl]);
        cJSON_AddNumberToObject(l, "current_effect", g_cfg.rgb.warnhot_current[lvl]);
        cJSON *params = cJSON_AddArrayToObject(l, "params");
        for (int fx = 0; fx < 2; ++fx) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "index", fx);
            cJSON_AddNumberToObject(p, "bg", g_cfg.rgb.warnhot_bg[lvl][fx]);
            cJSON_AddNumberToObject(p, "speed", g_cfg.rgb.warnhot_speed[lvl][fx]);
            cJSON_AddItemToArray(params, p);
        }
    }

    // settings
    cJSON *se = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddStringToObject(se, "fw_version", PV_FW_VERSION);
    // NOT STOCK. This project's own version, shown in the corner of the
    // page. A stock device never sends it and the badge falls back.
    cJSON_AddStringToObject(se, "os_name", PV_OS_NAME);
    cJSON_AddStringToObject(se, "os_version", PV_OS_VERSION);
    // NOT STOCK. Non-zero means the last save did not reach flash.
    cJSON_AddNumberToObject(se, "cfg_save_failed", g_live.cfg_save_failed ? 1 : 0);
    cJSON_AddStringToObject(se, "language", g_cfg.language);
    // NOT a stock key. What the Control Panel's Device row shows; stock has
    // the same string baked into the web app as a translation entry.
    cJSON_AddStringToObject(se, "device_name",
        g_cfg.device_name[0] ? g_cfg.device_name : PV_DEVICE_NAME_DEFAULT);
    cJSON_AddStringToObject(se, "device_name_default", PV_DEVICE_NAME_DEFAULT);

    // vent_policy: material-aware venting. NOT a stock key. The factory app
    // ignores objects it does not know, so its presence is harmless there.
    cJSON *vp = cJSON_AddObjectToObject(root, "vent_policy");
    cJSON_AddNumberToObject(vp, "enable", g_pol.enable ? 1 : 0);
    cJSON_AddNumberToObject(vp, "heat_hold", g_pol.heat_hold ? 1 : 0);
    cJSON_AddNumberToObject(vp, "bed_open_c", g_pol.bed_open_c);
    cJSON_AddNumberToObject(vp, "bed_close_c", g_pol.bed_close_c);
    // Live, read-only: what the printer says is loaded and which rule that
    // hits. Lets the UI show why the vent is doing what it is doing.
    cJSON_AddStringToObject(vp, "material", g_live.material);
    cJSON_AddNumberToObject(vp, "matched", pv_policy_match(g_live.material));
    // Where the vent actually is, and what the printer is doing. Read-only.
    // Makes the card's behaviour checkable from outside instead of only from
    // the serial log.
    cJSON_AddNumberToObject(vp, "vent_open", g_live.vent_open ? 1 : 0);
    // NOT STOCK. Which of the three the user has selected, as opposed to
    // where the flap happens to be right now.
    {
        static const char *const mode_name[3] = { "auto", "open", "closed" };
        int vm = pv_motor_get_mode();
        if (vm < 0 || vm > 2) vm = PV_VENT_AUTO;
        cJSON_AddStringToObject(vp, "vent_mode", mode_name[vm]);
    }
    // NOT STOCK. What the button ring is doing.
    cJSON_AddNumberToObject(vp, "ring_mode", g_cfg.ring_mode);
    cJSON_AddNumberToObject(vp, "ring_blink", g_cfg.ring_blink ? 1 : 0);
    // NOT STOCK. How many LEDs each strip actually has. Stock assumes 16;
    // the factory manual says two strip groups total 27, so at least one
    // run is shorter and every centred or scaled effect needs to know.
    {
        cJSON *ls = cJSON_AddArrayToObject(vp, "leds");
        for (int i = 0; i < PV_STRIP_COUNT_MAX; ++i)
            cJSON_AddItemToArray(ls, cJSON_CreateNumber(g_cfg.leds[i]));
    }
    cJSON_AddNumberToObject(vp, "device_state", g_live.device_state);
    cJSON_AddNumberToObject(vp, "bed_temp", g_live.bed_temp);
    // NOT STOCK. print.mc_percent, which drives the Progress Bar effect.
    // Exposed as read-only telemetry beside the other live values so the
    // effect can be checked from outside instead of only by looking at it.
    cJSON_AddNumberToObject(vp, "print_percent", g_live.print_percent);
    // NOT STOCK. Everything the Status page shows and nothing the vent acts
    // on. A value the printer has never reported is sent as null rather than
    // as a number, so the page can say "not reported" instead of inventing a
    // chamber at 0 C or a fan at 0%.
    {
        cJSON *st = cJSON_AddObjectToObject(vp, "status");
        #define NUM_OR_NULL(k, v)  do { \
            if ((v) >= 0) cJSON_AddNumberToObject(st, k, (v)); \
            else          cJSON_AddNullToObject(st, k); } while (0)
        cJSON_AddNumberToObject(st, "nozzle_temp", g_live.nozzle_temp);
        NUM_OR_NULL("chamber_temp", g_live.chamber_temp);
        NUM_OR_NULL("fan_part",     g_live.fan_part);
        NUM_OR_NULL("fan_aux",      g_live.fan_aux);
        NUM_OR_NULL("fan_chamber",  g_live.fan_chamber);
        NUM_OR_NULL("layer_total",  g_live.layer_total);
        NUM_OR_NULL("remain_min",   g_live.remain_min);
        NUM_OR_NULL("spd_lvl",      g_live.spd_lvl);
        #undef NUM_OR_NULL
        cJSON_AddNumberToObject(st, "layer_num", g_live.layer_num);
        cJSON_AddNumberToObject(st, "gcode_state", g_live.gcode_state);
        cJSON_AddNumberToObject(st, "stg_cur", g_live.stg_cur);
        cJSON_AddNumberToObject(st, "print_error", g_live.print_error);
        cJSON_AddNumberToObject(st, "hms_fault", g_live.hms_fault ? 1 : 0);
        cJSON_AddNumberToObject(st, "printer_light", g_live.printer_light ? 1 : 0);
        cJSON_AddStringToObject(st, "job_name", g_live.job_name);
        cJSON_AddStringToObject(st, "printer_rssi", g_live.printer_rssi);
        // The VENT's own signal, read at the moment of the push rather than
        // cached, because it is the one number here that is about this device
        // and it moves.
        {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                cJSON_AddNumberToObject(st, "wifi_rssi", ap.rssi);
            else
                cJSON_AddNullToObject(st, "wifi_rssi");
        }
        // Seconds since boot. Cheap, and the first thing worth knowing when a
        // device is misbehaving is whether it has been restarting.
        cJSON_AddNumberToObject(st, "uptime_s", (int)(esp_timer_get_time() / 1000000));
        cJSON_AddNumberToObject(st, "heap_free", (int)esp_get_free_heap_size());
        cJSON_AddNumberToObject(st, "heap_min", (int)esp_get_minimum_free_heap_size());
    }

    cJSON *mats = cJSON_AddArrayToObject(vp, "materials");
    for (int i = 0; i < PV_MAT_COUNT; ++i) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "index", i);
        cJSON_AddStringToObject(m, "name", pv_material_name[i]);
        cJSON_AddNumberToObject(m, "seal", pv_material_seal[i] ? 1 : 0);
        cJSON_AddNumberToObject(m, "on", (g_pol.rule_on & (1u << i)) ? 1 : 0);
        cJSON_AddItemToArray(mats, m);
    }

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    return out;
}

// NOT STOCK. The device log as a document of its own.
//
// Deliberately NOT part of the state push. The state document goes out on
// every change to every client; 64 lines of log would put 8 KB on the wire
// each time a slider moved, on a device with 4 MB of flash and a single
// threaded HTTP server. The page asks for this when it wants it.
char *pv_json_logs(void)
{
    static pv_log_line_t lines[PV_LOG_MAX];
    int n = pv_log_read(lines, PV_LOG_MAX);

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *lg = cJSON_AddObjectToObject(root, "logs");
    cJSON_AddNumberToObject(lg, "uptime_s", (int)(esp_timer_get_time() / 1000000));
    cJSON *arr = cJSON_AddArrayToObject(lg, "lines");
    for (int i = 0; i < n; ++i) {
        cJSON *o = cJSON_CreateObject();
        // Seconds since boot with a millisecond, which is what makes two
        // lines a hundred milliseconds apart tell you something.
        cJSON_AddNumberToObject(o, "t", (double)lines[i].us / 1000000.0);
        cJSON_AddStringToObject(o, "s", lines[i].text);
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *pv_json_response(const char *type, int ok)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *r = cJSON_AddObjectToObject(root, "response");
    cJSON_AddStringToObject(r, "type", type);
    cJSON_AddNumberToObject(r, "ok", ok);
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    return out;
}
