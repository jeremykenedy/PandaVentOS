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

static cJSON *fx_param(int id_key_is_effect, int id, const pv_fx_param_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, id_key_is_effect ? "effect_id" : "id", id);
    cJSON_AddNumberToObject(o, "brightness", p->brightness);
    cJSON_AddNumberToObject(o, "speed", p->speed);
    cJSON_AddStringToObject(o, "color", p->color);
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
            cJSON_AddItemToArray(efs, fx_param(1, f, &g_cfg.rgb.h2d[s][f]));
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
    cJSON_AddStringToObject(se, "fw_version", "V1.0.0");
    cJSON_AddStringToObject(se, "language", g_cfg.language);

    char *out = cJSON_Print(root);
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
