// Inbound WebSocket dispatch: exactly the message grammar the factory UI
// emits, enumerated from all 44 ws_send_data() call sites in the factory
// app. Field spellings are factory contract and must not be "cleaned up":
// brightness is "bg", colour is "rgb", the H2D device state is "mode" and
// the effect index is "effect", and warning_hot nests under "safe"/"warn".
// The colour value is an uppercase "RRGGBB" string; the {r,g,b} object form
// is also accepted because the app's hexToRgb() can produce it.
#include "pv.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pv_apply";

static uint8_t clamp100(double v) { return v < 0 ? 0 : v > 100 ? 100 : (uint8_t)v; }

static void restart_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(400)); esp_restart(); }
static void schedule_restart(void) { xTaskCreate(restart_task, "pv_rst", 2048, NULL, 5, NULL); }

static void color_from_json(cJSON *rgb, char out[7])
{
    if (cJSON_IsString(rgb) && strlen(rgb->valuestring) == 6) {
        for (int i = 0; i < 6; ++i) {
            char c = rgb->valuestring[i];
            out[i] = (c >= 'a' && c <= 'f') ? c - 32 : c;
        }
        out[6] = '\0';
    } else if (cJSON_IsObject(rgb)) {
        int r = 0, g = 0, b = 0;
        cJSON *e;
        if ((e = cJSON_GetObjectItemCaseSensitive(rgb, "r")) && cJSON_IsNumber(e)) r = e->valueint;
        if ((e = cJSON_GetObjectItemCaseSensitive(rgb, "g")) && cJSON_IsNumber(e)) g = e->valueint;
        if ((e = cJSON_GetObjectItemCaseSensitive(rgb, "b")) && cJSON_IsNumber(e)) b = e->valueint;
        snprintf(out, 7, "%02X%02X%02X", r & 255, g & 255, b & 255);
    }
}

static void apply_settings(cJSON *o)
{
    cJSON *e;
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "language")) && cJSON_IsString(e)) {
        snprintf(g_cfg.language, sizeof(g_cfg.language), "%s", e->valuestring);
        pv_cfg_save();
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "reset")) && cJSON_IsNumber(e) && e->valueint) {
        ESP_LOGW(TAG, "restart requested");
        schedule_restart();
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "factory_reset")) && cJSON_IsNumber(e) && e->valueint) {
        pv_ws_broadcast(pv_json_response("factory_reset", 1));
        vTaskDelay(pdMS_TO_TICKS(300));
        pv_factory_reset_and_reboot();
    }
}

static void apply_wifi(cJSON *o)
{
    cJSON *e;
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "scan")) && cJSON_IsNumber(e) && e->valueint) {
        pv_wifi_scan_start();
        return;
    }
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(o, "ssid");
    cJSON *pw   = cJSON_GetObjectItemCaseSensitive(o, "password");
    if (cJSON_IsString(ssid) && ssid->valuestring[0]) {
        pv_wifi_join(ssid->valuestring, cJSON_IsString(pw) ? pw->valuestring : "");
    }
}

static void apply_sta(cJSON *o)
{
    cJSON *hn = cJSON_GetObjectItemCaseSensitive(o, "hostname");
    if (cJSON_IsString(hn)) {
        snprintf(g_cfg.hostname, sizeof(g_cfg.hostname), "%s", hn->valuestring);
        pv_cfg_save();
        pv_hostname_apply();
        pv_ws_broadcast(pv_json_response("set_hostname", 1));
        // The UI's OK button restarts the device itself via settings.reset.
    }
}

static void apply_ap(cJSON *o)
{
    cJSON *on   = cJSON_GetObjectItemCaseSensitive(o, "on");
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(o, "ssid");
    cJSON *pw   = cJSON_GetObjectItemCaseSensitive(o, "password");
    cJSON *ip   = cJSON_GetObjectItemCaseSensitive(o, "ip");
    if (cJSON_IsNumber(on) && !cJSON_IsString(ssid)) {
        g_cfg.ap.on = on->valueint != 0;
        pv_cfg_save();
        pv_ap_apply();
        return;
    }
    if (cJSON_IsString(ssid) && ssid->valuestring[0]) {
        int ok = 1;
        if (cJSON_IsString(pw) && strlen(pw->valuestring) > 0 && strlen(pw->valuestring) < 8)
            ok = 0;   // softAP WPA2 minimum
        if (ok) {
            snprintf(g_cfg.ap.ssid, sizeof(g_cfg.ap.ssid), "%s", ssid->valuestring);
            if (cJSON_IsString(pw))
                snprintf(g_cfg.ap.password, sizeof(g_cfg.ap.password), "%s", pw->valuestring);
            if (cJSON_IsString(ip) && ip->valuestring[0])
                snprintf(g_cfg.ap.ip, sizeof(g_cfg.ap.ip), "%s", ip->valuestring);
            pv_cfg_save();
            pv_ap_apply();
        }
        pv_ws_broadcast(pv_json_response(cJSON_IsString(ip) ? "set_hotspot_ip" : "set_ap", ok));
    }
}

static void apply_printer(cJSON *o)
{
    cJSON *e;
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "scan")) && cJSON_IsNumber(e) && e->valueint) {
        pv_bambu_scan_start();
        return;
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "disconnect")) && cJSON_IsNumber(e) && e->valueint) {
        memset(&g_cfg.printer, 0, sizeof(g_cfg.printer));
        pv_cfg_save();
        pv_bambu_disconnect();
        pv_ws_push_state();
        return;
    }
    cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
    cJSON *sn   = cJSON_GetObjectItemCaseSensitive(o, "sn");
    cJSON *ac   = cJSON_GetObjectItemCaseSensitive(o, "access_code");
    cJSON *ip   = cJSON_GetObjectItemCaseSensitive(o, "ip");
    if (cJSON_IsString(sn) && sn->valuestring[0]) {
        snprintf(g_cfg.printer.name, sizeof(g_cfg.printer.name), "%s",
                 cJSON_IsString(name) ? name->valuestring : "");
        snprintf(g_cfg.printer.sn, sizeof(g_cfg.printer.sn), "%s", sn->valuestring);
        if (cJSON_IsString(ac))
            snprintf(g_cfg.printer.access_code, sizeof(g_cfg.printer.access_code), "%s", ac->valuestring);
        if (cJSON_IsString(ip))
            snprintf(g_cfg.printer.ip, sizeof(g_cfg.printer.ip), "%s", ip->valuestring);
        pv_cfg_save();
        pv_bambu_rebind();
    }
}

static void apply_rgb_switch(cJSON *o)
{
    cJSON *e;
    bool changed = false;
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "total_switch")) && cJSON_IsNumber(e))
        { g_cfg.rgb.light_on = e->valueint != 0; changed = true; }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "follow_printer")) && cJSON_IsNumber(e))
        { g_cfg.rgb.follow_printer = e->valueint != 0; changed = true; }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "warning_overide")) && cJSON_IsNumber(e))
        { g_cfg.rgb.warning_sw = e->valueint != 0; changed = true; }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "reverse_light")) && cJSON_IsNumber(e))
        { g_cfg.rgb.reverse = e->valueint != 0; changed = true; }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "follow_vent")) && cJSON_IsNumber(e))
        { g_cfg.rgb.follow_vent = e->valueint != 0; changed = true; }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "current_light_mode")) && cJSON_IsNumber(e))
        { g_cfg.rgb.light_mode = e->valueint % 3; changed = true; }
    if (changed) { pv_cfg_save(); pv_rgb_notify(); }
}

static void apply_fx_fields(pv_fx_param_t *p, cJSON *o)
{
    cJSON *e;
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "bg")) && cJSON_IsNumber(e))
        p->brightness = clamp100(e->valuedouble);
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "speed")) && cJSON_IsNumber(e))
        p->speed = clamp100(e->valuedouble);
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "rgb")))
        color_from_json(e, p->color);
}

static void apply_rgb_mode(cJSON *o)
{
    cJSON *e;
    // {} = query -> full state push (factory behavior).
    if (cJSON_GetArraySize(o) == 0) { pv_ws_push_state(); return; }

    if ((e = cJSON_GetObjectItemCaseSensitive(o, "reset")) && cJSON_IsNumber(e)) {
        int mode = e->valueint % 3;
        pv_cfg_rgb_mode_defaults(&g_cfg.rgb, mode);
        pv_cfg_save();
        pv_rgb_notify();
        pv_ws_push_state();
        return;
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "simple_mode")) && cJSON_IsObject(e)) {
        cJSON *fx = cJSON_GetObjectItemCaseSensitive(e, "effect");
        if (cJSON_IsNumber(fx) && fx->valueint >= 0 && fx->valueint < PV_FX_COUNT) {
            g_cfg.rgb.simple_current = fx->valueint;
            apply_fx_fields(&g_cfg.rgb.simple[fx->valueint], e);
        }
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "h2d_mode")) && cJSON_IsObject(e)) {
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(e, "mode");
        cJSON *fx   = cJSON_GetObjectItemCaseSensitive(e, "effect");
        if (cJSON_IsNumber(mode) && cJSON_IsNumber(fx) &&
            mode->valueint >= 0 && mode->valueint < PV_ST_COUNT &&
            fx->valueint >= 0 && fx->valueint < PV_FX_COUNT) {
            g_cfg.rgb.h2d_active[mode->valueint] = fx->valueint;
            apply_fx_fields(&g_cfg.rgb.h2d[mode->valueint][fx->valueint], e);
        }
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "warning_hot_mode")) && cJSON_IsObject(e)) {
        static const char *lvl_name[2] = { "safe", "warn" };
        for (int lvl = 0; lvl < 2; ++lvl) {
            cJSON *l = cJSON_GetObjectItemCaseSensitive(e, lvl_name[lvl]);
            if (!cJSON_IsObject(l)) continue;
            cJSON *fx = cJSON_GetObjectItemCaseSensitive(l, "effect");
            int f = cJSON_IsNumber(fx) ? fx->valueint & 1 : g_cfg.rgb.warnhot_current[lvl];
            g_cfg.rgb.warnhot_current[lvl] = f;
            cJSON *bg = cJSON_GetObjectItemCaseSensitive(l, "bg");
            if (cJSON_IsNumber(bg)) g_cfg.rgb.warnhot_bg[lvl][f] = clamp100(bg->valuedouble);
            cJSON *sp = cJSON_GetObjectItemCaseSensitive(l, "speed");
            if (cJSON_IsNumber(sp)) g_cfg.rgb.warnhot_speed[lvl][f] = clamp100(sp->valuedouble);
        }
    }
    pv_cfg_save();
    pv_rgb_notify();
}

void pv_apply_message(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) { ESP_LOGW(TAG, "bad json"); return; }
    cJSON *o;
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "settings")))   apply_settings(o);
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "wifi")))       apply_wifi(o);
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "sta")))        apply_sta(o);
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "ap")))         apply_ap(o);
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "printer")))    apply_printer(o);
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "rgb_switch"))) apply_rgb_switch(o);
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "rgb_mode")))   apply_rgb_mode(o);
    cJSON_Delete(root);
}
