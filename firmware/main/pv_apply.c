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
    // NOT a stock key. An empty string, or the word "default", puts the
    // stock name back, so the UI's reset button does not have to know it.
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "device_name")) && cJSON_IsString(e)) {
        const char *v = e->valuestring;
        if (!v[0] || strcmp(v, "default") == 0) {
            snprintf(g_cfg.device_name, sizeof(g_cfg.device_name),
                     "%s", PV_DEVICE_NAME_DEFAULT);
        } else {
            snprintf(g_cfg.device_name, sizeof(g_cfg.device_name), "%s", v);
        }
        pv_cfg_save();
        ESP_LOGI(TAG, "device name -> %s", g_cfg.device_name);
        pv_ws_broadcast(pv_json_response("set_device_name", 1));
        pv_ws_push_state();
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

// Effects 5 (Color_Cycle) and 6 (Rainbow) generate their own colours, and
// the factory app refuses to send one for them ("Cannot Customize Color").
// The FIRMWARE, however, stores whatever it is given: probed on a stock unit
// by writing 00FF00 to simple effect 5, which read back as 00FF00 rather than
// being coerced. So no guard here. The colour is simply never used while
// those effects are running.
static void apply_fx_fields(pv_fx_param_t *p, cJSON *o)
{
    cJSON *e;
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "bg")) && cJSON_IsNumber(e))
        p->brightness = clamp100(e->valuedouble);
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "speed")) && cJSON_IsNumber(e))
        p->speed = clamp100(e->valuedouble);
    char hex[7];
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "rgb"))) {
        pv_rgb3_to_hex(p->rgb, hex);          // keep the old value if unparsable
        color_from_json(e, hex);
        pv_hex_to_rgb3(hex, p->rgb);
    }
    // NOT STOCK. "rgb" stays the open colour so the factory app's own messages
    // keep working unchanged; every other colour needs an explicit key.
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "rgb_closed"))) {
        pv_rgb3_to_hex(p->rgb_closed, hex);
        color_from_json(e, hex);
        pv_hex_to_rgb3(hex, p->rgb_closed);
    }
    // The two INACTIVE colours. Deliberately NOT called "bg": stock already
    // uses "bg" for the brightness percent on this very object, and a key that
    // means a number to the factory app and a colour to this one is a bug
    // waiting to happen. JSON null, or the string "", CLEARS one: that is the
    // UI's X button, and clearing puts the unlit pixels back to black.
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "inactive"))) {
        if (cJSON_IsNull(e) || (cJSON_IsString(e) && e->valuestring[0] == '\0')) {
            p->opt_set &= (uint8_t)~PV_BG_OPEN;
        } else {
            pv_rgb3_to_hex(p->bg, hex);
            color_from_json(e, hex);
            pv_hex_to_rgb3(hex, p->bg);
            p->opt_set |= PV_BG_OPEN;
        }
    }
    // The optional END brightness. A number sets the ramp; JSON null, or the
    // string "", clears it and the effect goes back to one brightness. Same
    // shape as the inactive colours and the same X button drives it.
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "bright_end"))) {
        if (cJSON_IsNull(e) || (cJSON_IsString(e) && e->valuestring[0] == '\0')) {
            p->opt_set &= (uint8_t)~PV_BRIGHT_END;
            p->bright_end = p->brightness;
        } else if (cJSON_IsNumber(e)) {
            p->bright_end = clamp100(e->valuedouble);
            p->opt_set |= PV_BRIGHT_END;
        }
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "inactive_closed"))) {
        if (cJSON_IsNull(e) || (cJSON_IsString(e) && e->valuestring[0] == '\0')) {
            p->opt_set &= (uint8_t)~PV_BG_CLOSED;
        } else {
            pv_rgb3_to_hex(p->bg_closed, hex);
            color_from_json(e, hex);
            pv_hex_to_rgb3(hex, p->bg_closed);
            p->opt_set |= PV_BG_CLOSED;
        }
    }
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
            apply_fx_fields(&g_h2d[mode->valueint][fx->valueint], e);
            pv_cfg_h2d_save(mode->valueint);
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

// vent_policy. NOT a stock message. Accepted shapes:
//   {"vent_policy":{"enable":0|1}}
//   {"vent_policy":{"heat_hold":0|1}}
//   {"vent_policy":{"material":{"index":0..8,"on":0|1}}}
//   {"vent_policy":{"bed_open_c":45,"bed_close_c":35}}
#if PV_POLICY_TEST_HOOK
bool g_test_live_lock = false;
#endif

static void apply_vent_policy(cJSON *o)
{
    cJSON *e;
    bool touched = false;

#if PV_POLICY_TEST_HOOK
    // TEST BUILD ONLY. Compiled out of every shipping image. Lets a test set
    // the live inputs the policy reads, and freeze the printer report so they
    // are not overwritten a second later, which is the only way to drive the
    // sealing branch on a machine with nothing but PLA loaded.
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "__test_lock")) && cJSON_IsNumber(e)) {
        g_test_live_lock = e->valueint != 0;
        ESP_LOGW(TAG, "TEST: live lock %s", g_test_live_lock ? "on" : "off");
        touched = true;
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "__test_material")) && cJSON_IsString(e)) {
        snprintf(g_live.material, sizeof(g_live.material), "%s", e->valuestring);
        ESP_LOGW(TAG, "TEST: material := %s", g_live.material);
        touched = true;
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "__test_state")) && cJSON_IsNumber(e)) {
        g_live.device_state = e->valueint;
        ESP_LOGW(TAG, "TEST: device_state := %d", g_live.device_state);
        touched = true;
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "__test_bed")) && cJSON_IsNumber(e)) {
        g_live.bed_temp = e->valueint;
        ESP_LOGW(TAG, "TEST: bed_temp := %d", g_live.bed_temp);
        touched = true;
    }
    // A whole printer report, pushed through the real parser.
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "__test_report")) && cJSON_IsString(e)) {
        ESP_LOGW(TAG, "TEST: feeding a %d byte report", (int)strlen(e->valuestring));
        pv_test_feed_report(e->valuestring, (int)strlen(e->valuestring));
        pv_ws_push_state();
        return;        // the report is the whole message; nothing else to apply
    }
#endif

    if ((e = cJSON_GetObjectItemCaseSensitive(o, "enable")) && cJSON_IsNumber(e)) {
        g_pol.enable = e->valueint != 0;
        touched = true;
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "heat_hold")) && cJSON_IsNumber(e)) {
        g_pol.heat_hold = e->valueint != 0;
        touched = true;
    }
    if ((e = cJSON_GetObjectItemCaseSensitive(o, "material")) && cJSON_IsObject(e)) {
        cJSON *ix = cJSON_GetObjectItemCaseSensitive(e, "index");
        cJSON *on = cJSON_GetObjectItemCaseSensitive(e, "on");
        if (cJSON_IsNumber(ix) && cJSON_IsNumber(on) &&
            ix->valueint >= 0 && ix->valueint < PV_MAT_COUNT) {
            uint16_t bit = (uint16_t)(1u << ix->valueint);
            if (on->valueint) g_pol.rule_on |= bit;
            else              g_pol.rule_on &= (uint16_t)~bit;
            touched = true;
        }
    }
    // Both thresholds arrive together from the UI so the open > close
    // invariant can be checked once, after both have landed. pv_policy_save
    // clamps, so a bad pair cannot brick the hysteresis.
    cJSON *bo = cJSON_GetObjectItemCaseSensitive(o, "bed_open_c");
    cJSON *bc = cJSON_GetObjectItemCaseSensitive(o, "bed_close_c");
    if (cJSON_IsNumber(bo)) { g_pol.bed_open_c  = (int16_t)bo->valuedouble; touched = true; }
    if (cJSON_IsNumber(bc)) { g_pol.bed_close_c = (int16_t)bc->valuedouble; touched = true; }

    if (!touched) return;
    pv_policy_save();
    pv_motor_update();            // apply the new rule immediately
    pv_ws_broadcast(pv_json_response("vent_policy", 1));
    pv_ws_push_state();
}

// vent. NOT a stock message. Three-way manual override of the flap.
//   {"vent":{"mode":"auto"}}    follow the printer (factory behaviour)
//   {"vent":{"mode":"open"}}    hold it open
//   {"vent":{"mode":"closed"}}  hold it closed
// Anything else is ignored rather than guessed at, because the wrong guess
// here drives a motor.
static void apply_vent(cJSON *o)
{
    cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "mode");
    if (!cJSON_IsString(m) || !m->valuestring) return;
    int mode;
    if      (!strcmp(m->valuestring, "auto"))   mode = PV_VENT_AUTO;
    else if (!strcmp(m->valuestring, "open"))   mode = PV_VENT_OPEN;
    else if (!strcmp(m->valuestring, "closed")) mode = PV_VENT_CLOSED;
    else { ESP_LOGW(TAG, "vent: unknown mode \"%s\"", m->valuestring); return; }
    ESP_LOGI(TAG, "vent mode -> %s", m->valuestring);
    pv_motor_set_mode(mode);
    pv_ws_push_state();
    pv_ws_broadcast(pv_json_response("vent", 1));
}

// ring. NOT a stock message. The button's ring LED.
//   {"ring":{"mode":0..4}}    PV_RING_*
//   {"ring":{"blink":0|1}}    blink while in MANUAL, or hold steady
// leds. NOT a stock message. How many LEDs each strip physically has.
//   {"leds":{"strip":0,"count":11}}
static void apply_leds(cJSON *o)
{
    cJSON *si = cJSON_GetObjectItemCaseSensitive(o, "strip");
    cJSON *ct = cJSON_GetObjectItemCaseSensitive(o, "count");
    if (!cJSON_IsNumber(si) || !cJSON_IsNumber(ct)) return;
    int i = si->valueint, n = ct->valueint;
    if (i < 0 || i >= PV_STRIP_COUNT_MAX) return;
    // Refused, not clamped: a count outside the buffer is a mistake worth
    // seeing rather than silently rounding into something that looks fine.
    if (n < 1 || n > PV_LEDS_PER_STRIP) {
        ESP_LOGW(TAG, "leds: strip %d count %d out of range 1..%d", i, n, PV_LEDS_PER_STRIP);
        return;
    }
    g_cfg.leds[i] = (uint8_t)n;
    ESP_LOGI(TAG, "strip %d -> %d LEDs", i, n);
    pv_cfg_save();
    pv_rgb_notify();
    pv_ws_push_state();
    pv_ws_broadcast(pv_json_response("leds", 1));
}

static void apply_ring(cJSON *o)
{
    bool touched = false;
    cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "mode");
    if (cJSON_IsNumber(m) && m->valueint >= 0 && m->valueint < PV_RING_COUNT) {
        g_cfg.ring_mode = (uint8_t)m->valueint;
        touched = true;
    }
    cJSON *b = cJSON_GetObjectItemCaseSensitive(o, "blink");
    if (cJSON_IsBool(b) || cJSON_IsNumber(b)) {
        g_cfg.ring_blink = cJSON_IsBool(b) ? cJSON_IsTrue(b) : (b->valueint != 0);
        touched = true;
    }
    if (!touched) return;
    ESP_LOGI(TAG, "ring -> mode %d blink %d", g_cfg.ring_mode, g_cfg.ring_blink);
    pv_cfg_save();
    pv_ws_push_state();
    pv_ws_broadcast(pv_json_response("ring", 1));
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
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "vent_policy"))) apply_vent_policy(o);
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "vent")))       apply_vent(o);
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "ring")))       apply_ring(o);
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "leds")))       apply_leds(o);
    cJSON_Delete(root);
}
