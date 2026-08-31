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
#include "esp_log.h"

static const char *TAG = "pv_json";
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
    // Absent when unset, for the same reason the ramp is: the page has to be
    // able to tell "never set" from "set to zero".
    if (p->opt_set & PV_AUX)
        cJSON_AddNumberToObject(o, "aux", p->aux);
    // Always sent, unlike the optional ones above: an effect is either
    // reversed or it is not, and there is no third answer to leave out.
    cJSON_AddBoolToObject(o, "reverse", (p->opt_set & PV_FX_REVERSE) != 0);
    return o;
}

// NOT STOCK. The state document is built and printed in PARTS, into one
// static buffer, and never as a single allocation.
//
// WHAT WENT WRONG, so it is not repeated:
//
// The whole document was one cJSON tree printed with cJSON_Print, which grows
// its output by doubling: to reach twenty-odd kilobytes it asks the allocator
// for a 32 KB CONTIGUOUS block, on top of a tree of roughly fourteen hundred
// nodes. Total free heap said 98 KB and the allocation still failed, because
// free heap is not one block: the Wi-Fi and TLS buffers leave nothing that
// large in one piece. pv_json_state() returned NULL, every client got no state
// document at all, and the page sat on placeholders with no way to recover.
// The device's own low-water mark had been 2.5 KB for some time before that,
// which was the warning nobody could see until the Status page reported it.
//
// So: one static buffer, printed into with cJSON_PrintPreallocated, and the
// document split into parts small enough that the tree for any one of them is
// a few hundred nodes rather than fourteen hundred. Nothing here can fail for
// want of a big block, because nothing here asks for one.
//
// The parts are separate documents on the wire. That is not a new contract:
// the page has always merged partial documents key by key, and stock's own
// pushes are partial for everything except the one sent on connect.
#define STATE_BUF 12288
static char s_state_buf[STATE_BUF];

// Prints, deletes the tree, and hands back the static buffer. NULL when the
// document did not fit, which is a bug in the sizing above rather than a
// runtime condition, so it is logged loudly.
static const char *state_finish(cJSON *root, int part)
{
    if (!root) return NULL;
    cJSON_bool ok = cJSON_PrintPreallocated(root, s_state_buf, STATE_BUF, 1);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGE(TAG, "state part %d did not fit in %d bytes", part, STATE_BUF);
        return NULL;
    }
    return s_state_buf;
}

const char *pv_json_state_part(int part)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    // Parts 2..7 are one H2D device state each. Twenty effects with four
    // colours apiece is the largest thing in the document by a wide margin,
    // and six of them together is what could not be printed.
    if (part >= 2 && part < 2 + PV_ST_COUNT) {
        int s = part - 2;
        cJSON *rm  = cJSON_AddObjectToObject(root, "rgb_mode");
        cJSON *h2d = cJSON_AddObjectToObject(rm, "h2d_mode");
        cJSON *ds  = cJSON_AddArrayToObject(h2d, "device_states");
        cJSON *st  = cJSON_CreateObject();
        cJSON_AddNumberToObject(st, "device_state_id", s);
        cJSON_AddNumberToObject(st, "active_effect_id", g_cfg.rgb.h2d_active[s]);
        cJSON *efs = cJSON_AddArrayToObject(st, "effects");
        for (int f = 0; f < PV_FX_COUNT; ++f)
            cJSON_AddItemToArray(efs, fx_param(1, f, &g_h2d[s][f]));
        cJSON_AddItemToArray(ds, st);
        return state_finish(root, part);
    }

    if (part == 1) {
        cJSON *rm = cJSON_AddObjectToObject(root, "rgb_mode");
        cJSON_AddNumberToObject(rm, "rgb_light_mode", g_cfg.rgb.light_mode);
        cJSON_AddBoolToObject(rm, "light_on_off", g_cfg.rgb.light_on);
        cJSON_AddBoolToObject(rm, "warning_sw", g_cfg.rgb.warning_sw);
        cJSON_AddBoolToObject(rm, "is_follow_printer", g_cfg.rgb.follow_printer);
        cJSON_AddBoolToObject(rm, "is_follow_vent", g_cfg.rgb.follow_vent);
        // NOT STOCK. A live preview is running; the page shows a countdown and
        // an obvious way out. Absent when nothing is being previewed.
        if (pv_rgb_preview_left() > 0) {
            cJSON_AddNumberToObject(rm, "preview_left", pv_rgb_preview_left());
            // Absent unless the preview pinned one, so "not pinned" and
            // "pinned to zero" stay different things.
            if (pv_rgb_preview_state() >= 0)
                cJSON_AddNumberToObject(rm, "preview_state", pv_rgb_preview_state());
            if (pv_rgb_preview_percent() >= 0)
                cJSON_AddNumberToObject(rm, "preview_percent", pv_rgb_preview_percent());
        }
        cJSON_AddBoolToObject(rm, "is_reverse", g_cfg.rgb.reverse);
        // NOT STOCK. One flag per strip, so a run mounted the other way round
        // can be turned around on its own.
        {
            cJSON *rs = cJSON_AddArrayToObject(rm, "reverse_strips");
            for (int i = 0; i < PV_STRIP_COUNT_MAX; ++i)
                cJSON_AddItemToArray(rs, cJSON_CreateBool(
                    (g_cfg.rgb.reverse_strips >> i) & 1));
        }
        cJSON_AddNumberToObject(rm, "current_simple_effect", g_cfg.rgb.simple_current);
        cJSON *effects = cJSON_AddArrayToObject(rm, "effects");
        for (int i = 0; i < PV_FX_COUNT; ++i)
            cJSON_AddItemToArray(effects, fx_param(0, i, &g_cfg.rgb.simple[i]));

        // NOT STOCK. The four settings that used to be compiled in. Sent as
        // what they ARE, including zero for "never set", so the page can show
        // the compiled default as a placeholder rather than as a value the
        // owner chose.
        cJSON_AddNumberToObject(rm, "warn_hot_c", g_cfg.rgb.warn_hot_c);
        cJSON_AddNumberToObject(rm, "warn_hot_c_default", PV_WARN_HOT_C);
        cJSON_AddBoolToObject(rm, "contiguous", g_cfg.rgb.contiguous);
        {
            cJSON *gr = cJSON_AddObjectToObject(rm, "gradient");
            cJSON_AddNumberToObject(gr, "min_c", g_cfg.rgb.grad_min_c);
            cJSON_AddNumberToObject(gr, "max_c", g_cfg.rgb.grad_max_c);
            cJSON_AddNumberToObject(gr, "min_c_default", PV_GRAD_MIN_C_DEFAULT);
            cJSON_AddNumberToObject(gr, "max_c_default", PV_GRAD_MAX_C_DEFAULT);
        }
        {
            cJSON *ef = cJSON_AddObjectToObject(rm, "error_flash");
            char ehex[7];
            pv_rgb3_to_hex(g_cfg.rgb.err_rgb, ehex);
            cJSON_AddBoolToObject(ef, "set", g_cfg.rgb.err_set);
            cJSON_AddStringToObject(ef, "rgb", g_cfg.rgb.err_set ? ehex : "7F0000");
            cJSON_AddNumberToObject(ef, "bright", g_cfg.rgb.err_set ? g_cfg.rgb.err_bright : 100);
            cJSON_AddBoolToObject(ef, "strobe", g_cfg.rgb.err_strobe != 0);
        }

        cJSON *wh = cJSON_AddObjectToObject(rm, "warning_hot_mode");
        static const char *lvl_name[2] = { "safe", "warn" };
        for (int lvl = 0; lvl < 2; ++lvl) {
            cJSON *l = cJSON_AddObjectToObject(wh, lvl_name[lvl]);
            cJSON_AddNumberToObject(l, "current_effect", g_cfg.rgb.warnhot_current[lvl]);
            cJSON *params = cJSON_AddArrayToObject(l, "params");
            for (int fx = 0; fx < 2; ++fx) {
                cJSON *pp = cJSON_CreateObject();
                cJSON_AddNumberToObject(pp, "index", fx);
                cJSON_AddNumberToObject(pp, "bg", g_cfg.rgb.warnhot_bg[lvl][fx]);
                cJSON_AddNumberToObject(pp, "speed", g_cfg.rgb.warnhot_speed[lvl][fx]);
                cJSON_AddItemToArray(params, pp);
            }
        }
        return state_finish(root, part);
    }

    // Part 0: everything that is not lighting. Small, and FIRST, because it
    // carries the two states the page needs before it can decide which screen
    // to open on.
    if (part != 0) { cJSON_Delete(root); return NULL; }

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
    // NOT STOCK. The exact image this device is running.
    //
    // flash-verify.sh proves a flash landed by re-reading the page the device
    // serves and comparing its hash. That catches a lost upload, which has
    // happened three times in this project, but it says NOTHING about a build
    // where only the firmware changed: the page is byte-identical and the
    // check reports "already running this exact UI" while the device keeps
    // running the old code. This is the other half of that check.
    {
        const esp_app_desc_t *d = esp_app_get_description();
        char sha[17];
        for (int i = 0; i < 8; ++i)
            snprintf(sha + i * 2, 3, "%02x", d->app_elf_sha256[i]);
        cJSON_AddStringToObject(se, "build", sha);
    }

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
    // NOT STOCK. Whether the flap is travelling right now, as opposed to
    // where it last arrived. See pv_motor_moving in pv.h.
    cJSON_AddNumberToObject(vp, "vent_moving", pv_motor_moving() ? 1 : 0);
    // NOT STOCK. The last endstop check, and whether one is running now.
    // Reported always, because a page that only learns about it by having
    // asked for it cannot show a check a second browser started.
    {
        pv_cal_t c;
        pv_motor_calibrate_get(&c);
        cJSON *cal = cJSON_AddObjectToObject(vp, "calibrate");
        cJSON_AddNumberToObject(cal, "state", c.state);
        cJSON_AddNumberToObject(cal, "step", c.step);
        // Only report readings from a check that actually finished. A run in
        // progress carries the previous run's numbers otherwise, and a stale
        // number beside a spinner is read as a live one.
        if (c.state == PV_CAL_DONE || c.state == PV_CAL_FAILED) {
            cJSON_AddNumberToObject(cal, "closed_mv", c.closed_mv);
            cJSON_AddNumberToObject(cal, "open_mv", c.open_mv);
            cJSON_AddBoolToObject(cal, "closed_ok", c.closed_ok);
            cJSON_AddBoolToObject(cal, "open_ok", c.open_ok);
            cJSON_AddNumberToObject(cal, "at_s", c.at_s);
        }
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
        // NOT STOCK. -1 means the printer has never mentioned a toolhead
        // light, which the page draws as "no such control" rather than "off".
        NUM_OR_NULL("work_light", g_live.work_light);
        NUM_OR_NULL("fan_part",     g_live.fan_part);
        NUM_OR_NULL("fan_aux",      g_live.fan_aux);
        NUM_OR_NULL("fan_chamber",  g_live.fan_chamber);
        NUM_OR_NULL("layer_total",  g_live.layer_total);
        NUM_OR_NULL("remain_min",   g_live.remain_min);
        NUM_OR_NULL("spd_lvl",      g_live.spd_lvl);
        // NOT STOCK. Readings the printer was already sending and nothing was
        // reading. Same rule as everything above: never reported is null, not
        // zero.
        NUM_OR_NULL("filament_in",      g_live.filament_in);
        NUM_OR_NULL("spd_mag",          g_live.spd_mag);
        NUM_OR_NULL("ams_humidity",     g_live.ams_humidity);
        NUM_OR_NULL("ams_humidity_pct", g_live.ams_humidity_pct);
        NUM_OR_NULL("ams_temp",         g_live.ams_temp);
        NUM_OR_NULL("door_open",        g_live.door_open);
        NUM_OR_NULL("fw_update",        g_live.fw_update);
        NUM_OR_NULL("tray_now",         g_live.tray_now);
        NUM_OR_NULL("cam_present",      g_live.cam_present);
        NUM_OR_NULL("cam_record",       g_live.cam_record);
        NUM_OR_NULL("cam_timelapse",    g_live.cam_timelapse);
        NUM_OR_NULL("cam_free_mb",      g_live.cam_free_mb);
        NUM_OR_NULL("cam_total_mb",     g_live.cam_total_mb);
        #undef NUM_OR_NULL
        cJSON_AddNumberToObject(st, "layer_num", g_live.layer_num);
        cJSON_AddNumberToObject(st, "gcode_state", g_live.gcode_state);
        cJSON_AddNumberToObject(st, "stg_cur", g_live.stg_cur);
        cJSON_AddNumberToObject(st, "print_error", g_live.print_error);
        cJSON_AddNumberToObject(st, "hms_fault", g_live.hms_fault ? 1 : 0);
        cJSON_AddNumberToObject(st, "printer_light", g_live.printer_light ? 1 : 0);
        cJSON_AddStringToObject(st, "job_name", g_live.job_name);
        cJSON_AddStringToObject(st, "printer_rssi", g_live.printer_rssi);
        // Strings are sent only when they hold something. An empty nozzle
        // type is not a nozzle type of "".
        if (g_live.nozzle_dia[0])  cJSON_AddStringToObject(st, "nozzle_dia", g_live.nozzle_dia);
        if (g_live.nozzle_kind[0]) cJSON_AddStringToObject(st, "nozzle_kind", g_live.nozzle_kind);
        if (g_live.hms_code[0])    cJSON_AddStringToObject(st, "hms_code", g_live.hms_code);
        if (g_live.cam_res[0])     cJSON_AddStringToObject(st, "cam_res", g_live.cam_res);
        if (g_live.cam_rtsp[0])    cJSON_AddStringToObject(st, "cam_rtsp", g_live.cam_rtsp);
        // The AMS trays. Sent as an array only when at least one slot holds
        // something, so a printer with no AMS sends no key at all rather than
        // four empty objects the page then has to decide not to draw.
        {
            bool any = false;
            for (int i = 0; i < PV_TRAY_MAX; ++i)
                if (g_live.tray[i].type[0]) { any = true; break; }
            if (any) {
                cJSON *arr = cJSON_AddArrayToObject(st, "trays");
                for (int i = 0; i < PV_TRAY_MAX; ++i) {
                    if (!g_live.tray[i].type[0]) continue;
                    cJSON *t = cJSON_CreateObject();
                    cJSON_AddNumberToObject(t, "i", i);
                    cJSON_AddStringToObject(t, "type", g_live.tray[i].type);
                    if (g_live.tray[i].color[0])
                        cJSON_AddStringToObject(t, "color", g_live.tray[i].color);
                    if (g_live.tray[i].remain >= 0)
                        cJSON_AddNumberToObject(t, "remain", g_live.tray[i].remain);
                    cJSON_AddItemToArray(arr, t);
                }
            }
        }
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
        // NOT STOCK. What the renderer is doing. A strip that looks wrong and
        // a strip that is not being drawn at all look the same from across a
        // room; these tell them apart without a serial cable.
        {
            pv_rgb_stats_t rs;
            pv_rgb_stats(&rs);
            // NOT STOCK. What animation is loaded, and what it costs. Reported
            // beside the render stats rather than with the effect settings,
            // because it is not a setting: it is in RAM and gone at the next
            // reboot, and the page has to be able to say so.
            {
                pv_anim_info_t ai;
                pv_anim_info(&ai);
                cJSON *an = cJSON_AddObjectToObject(st, "anim");
                cJSON_AddNumberToObject(an, "frames", ai.frames);
                cJSON_AddNumberToObject(an, "pixels", ai.pixels);
                cJSON_AddNumberToObject(an, "bytes", ai.bytes);
                cJSON_AddNumberToObject(an, "max_frames", PV_ANIM_MAX_FRAMES);
                cJSON_AddNumberToObject(an, "max_pixels", PV_ANIM_PIXELS);
                cJSON_AddNumberToObject(an, "max_bytes", PV_ANIM_MAX_BYTES);
            }
            // NOT STOCK. The printer's answer to the last command sent to it,
            // in its own words. A control that springs back with no
            // explanation is the worst of both: it looks like this firmware
            // failed when the printer refused.
            {
                pv_cmd_ack_t ack;
                pv_bambu_last_ack(&ack);
                if (ack.cmd[0]) {
                    cJSON *ca = cJSON_AddObjectToObject(st, "cmd");
                    cJSON_AddStringToObject(ca, "name", ack.cmd);
                    cJSON_AddBoolToObject(ca, "ok", ack.ok);
                    cJSON_AddStringToObject(ca, "reason", ack.reason);
                    cJSON_AddNumberToObject(ca, "at_s", ack.at_s);
                }
            }
            cJSON_AddNumberToObject(st, "fx_frames", (double)rs.frames);
            cJSON_AddNumberToObject(st, "fx_push_failed", (double)rs.push_failed);
            cJSON_AddNumberToObject(st, "fx_interval_ms", (double)rs.interval_ms);
            if (rs.fps >= 0) cJSON_AddNumberToObject(st, "fx_fps", rs.fps);
            else             cJSON_AddNullToObject(st, "fx_fps");
            if (rs.effect >= 0) cJSON_AddNumberToObject(st, "fx_now", rs.effect);
            else                cJSON_AddNullToObject(st, "fx_now");
        }
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

    return state_finish(root, part);
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
