// Bambu Lab LAN link: MQTT over TLS (self-signed per-device cert, so no
// verification — LAN only), device/<sn>/report subscription, one pushall on
// connect. SSDP discovery for the factory printer{scan,list} flow.
#include "pv.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "cJSON.h"

static const char *TAG = "pv_bambu";

static esp_mqtt_client_handle_t s_client;
static char s_report_topic[80], s_request_topic[80];
static bool s_started;

bool pv_bambu_started(void) { return s_started; }
static volatile TickType_t s_last_report;   // 0 until the bound SN answers
static volatile TickType_t s_connected_at;
static void relocate_start(void);           // defined with the scan code

static const char PUSHALL[] =
    "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\","
    "\"version\":1,\"push_target\":1}}";

// NOT STOCK. Sending TO the printer.
//
// Everything else in this file listens. These two write, which is a different
// kind of thing: they change a machine that is not this one, in the middle of
// somebody's print. Both are refused unless the link is actually up, both
// clamp their argument rather than trusting it, and neither has a retry: a
// fan command that arrives twice because the first was assumed lost is worse
// than one that does not arrive, since the UI shows what the printer reports
// back and the owner can simply ask again.
// Each command gets its own sequence_id, as Bambu Studio does.
//
// This was "0" on every message, copied from the pushall above. That was
// wrong, but it turned out NOT to be why commands were failing: a printer that
// accepts LAN reads while requiring cloud authentication for control refuses
// everything with "mqtt message verify failed", whatever the envelope looks
// like. That was established by trying five payload shapes from a direct MQTT
// client, which is worth recording because the two failures are
// indistinguishable from the UI. The fix for that one is LAN Only Mode on the
// printer, not anything here.
//
// It starts high enough not to collide with whatever the printer has already
// seen this session from a phone or from Studio.
static int s_seq = 1000;

// The printer's answer to the last command. See pv.h for why it is kept.
static pv_cmd_ack_t s_ack;
static portMUX_TYPE s_ack_lock = portMUX_INITIALIZER_UNLOCKED;

void pv_bambu_last_ack(pv_cmd_ack_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_ack_lock);
    *out = s_ack;
    portEXIT_CRITICAL(&s_ack_lock);
}

static bool bambu_send_top(const char *top, const char *cmd_json_body)
{
    if (!s_client || g_live.printer_state != 3) {
        ESP_LOGW(TAG, "not connected; command dropped");
        return false;
    }
    // No invented user_id.
    //
    // Bambu Studio sends one, so it was tried here: a made-up value, on the
    // theory that the printer only checks for the field's presence. It changed
    // nothing, on either command, which makes it a guess that happened not to
    // help. A fabricated account number in a payload nobody can explain is
    // worse than a payload without one, so it is gone.
    char payload[320];
    int n = snprintf(payload, sizeof(payload),
                     "{\"%s\":{\"sequence_id\":\"%d\",%s}}",
                     top, ++s_seq, cmd_json_body);
    if (n < 0 || n >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "command too long");
        return false;
    }
    int id = esp_mqtt_client_publish(s_client, s_request_topic, payload, 0, 0, 0);
    if (id < 0) { ESP_LOGW(TAG, "publish failed"); return false; }
    // Ask for a fresh report so the UI shows what the printer actually did
    // rather than what it was told to do. They differ: a chamber fan command
    // on a printer with no chamber fan changes nothing at all.
    esp_mqtt_client_publish(s_client, s_request_topic, PUSHALL, 0, 0, 0);
    return true;
}

static bool bambu_send(const char *cmd_json_body)
{
    return bambu_send_top("print", cmd_json_body);
}

// NOT STOCK, and the only command this printer actually accepts.
//
// `system.ledctrl` is not signature checked. Everything under `print` is:
// measured on a real P2S, one command at a time, every gcode_line and
// print_speed came back `mqtt message verify failed` while ledctrl came back
// `result: success`, on the same connection, seconds apart. So the light
// works and the fans do not, and that is the printer's rule, not ours.
//
// The four timing fields are required even when the mode is not flashing.
bool pv_bambu_set_light(const char *node, bool on)
{
    if (!node) return false;
    // Only the nodes a machine this firmware talks to actually has. An unknown
    // node is answered by the printer with "did not find the valid led", which
    // is a round trip spent to learn something already known here.
    if (strcmp(node, "chamber_light") && strcmp(node, "work_light")) {
        ESP_LOGW(TAG, "light: unknown node \"%s\"", node);
        return false;
    }
    char buf[240];
    snprintf(buf, sizeof(buf),
             "\"command\":\"ledctrl\",\"led_node\":\"%s\","
             "\"led_mode\":\"%s\",\"led_on_time\":500,\"led_off_time\":500,"
             "\"loop_times\":0,\"interval_time\":0",
             node, on ? "on" : "off");
    ESP_LOGI(TAG, "%s -> %s", node, on ? "on" : "off");
    return bambu_send_top("system", buf);
}

// NOT STOCK. Recording on or off, via the camera envelope.
bool pv_bambu_set_record(bool on)
{
    char buf[120];
    snprintf(buf, sizeof(buf),
             "\"command\":\"ipcam_record_set\",\"control\":\"%s\"",
             on ? "enable" : "disable");
    ESP_LOGI(TAG, "camera recording -> %s", on ? "on" : "off");
    return bambu_send_top("camera", buf);
}

// M106 P<part> S<0..255>. Bambu's part indices, from its own gcode:
//   P1 the part cooling fan, P2 the aux fan, P3 the chamber fan.
// The UI works in percent because that is what the printer reports back; the
// conversion is here so there is exactly one place that knows about 255.
bool pv_bambu_set_fan(int which, int percent)
{
    if (which < PV_FAN_PART || which > PV_FAN_CHAMBER) return false;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int s = (percent * 255 + 50) / 100;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "\"command\":\"gcode_line\",\"param\":\"M106 P%d S%d\\n\"", which, s);
    ESP_LOGI(TAG, "fan P%d -> %d%% (S%d)", which, percent, s);
    return bambu_send(buf);
}

// print_speed takes 1..4: silent, standard, sport, ludicrous. The same four
// the printer's own screen offers, and the same four the status page names.
bool pv_bambu_set_speed(int level)
{
    if (level < 1 || level > 4) return false;
    char buf[96];
    snprintf(buf, sizeof(buf),
             "\"command\":\"print_speed\",\"param\":\"%d\"", level);
    ESP_LOGI(TAG, "print speed -> %d", level);
    return bambu_send(buf);
}

// ---------- report parsing ----------

// ---------------------------------------------------------------------------
// THE ERROR PREDICATE, recovered 2026-08-29 from 0x400d9158.
//
// Stock does not derive its ERROR state from gcode_state. The chain is:
//
//   error = classify(print_error) || hms_has_pair()
//   error -> internal 1 at 0x400d91c7
//         -> H2D state 5 at 0x400dc424   (5 is reachable ONLY from internal 1:
//            0x400dc300 returns only 1 or 2 across all eleven of its exits and
//            0x400dc3a8 returns only 4 or 0)
//         -> the warning override red
//
// The clone gated on gcode_state == "FAILED" until now, which is a different
// signal entirely: an HMS fault during a RUNNING print showed red on stock and
// the printing effect here.
//
// classify() is 0x400d8fa4. Three tables, all read out of DROM:
//
//   A  0x3f416e5c  4 entries, stride 4, exact match  -> IS an error
//   B  0x3f416e6c  3 entries, stride 8 {value, mask} -> NOT an error when the
//                  bits that differ from value all fall inside mask
//                  (ball a8, a10 at 0x400d8fd2)
//   C  0x3f416e84  42 entries, stride 4, exact match -> NOT an error
//
// Order matters: zero first, then A, then B, then C, then default to error.
static const uint32_t ERR_FORCE[4] = {          // table A
    0x05004014, 0x0500402D, 0x0500402E, 0x0500402F,
};
static const struct { uint32_t value, mask; } ERR_MASKED[3] = {   // table B
    { 0x05004017, 0x0000000F },
    { 0x05004020, 0x0000000F },
    { 0x0501401A, 0x00000FFF },
};
static const uint32_t ERR_IGNORE[42] = {        // table C
    0x07FFC008, 0x07FF8003, 0x07FFC003, 0x07FE8006, 0x07FE8007, 0x07FEC006,
    0x07FEC009, 0x07FEC00A, 0x07FEC010, 0x07FEC011, 0x07FEC012, 0x07FF8006,
    0x07FF8007, 0x07FFC006, 0x07FFC009, 0x07FFC00A, 0x07FFC010, 0x07FFC011,
    0x07FFC012, 0x18FE8006, 0x18FE8007, 0x18FEC006, 0x18FEC009, 0x18FEC00A,
    0x18FF8006, 0x18FF8007, 0x18FFC006, 0x18FFC009, 0x18FFC00A, 0x05008079,
    0x03008054, 0x03004067, 0x0300400C, 0x0500400E, 0x05008030, 0x0500C011,
    0x0C008002, 0x05004001, 0x0300800C, 0x03008013, 0x12FF8007, 0x12FFC003,
};

// The single hms pair stock matches at 0x400d9015. Entry field 0 is compared
// against the literal at 0x400d087c and field 4 against 0x400d0878.
//
// INFERRED, and the only inferred thing here: which of the two JSON members is
// which. The pair itself is exact. Bambu reports hms entries as
// { "attr": N, "code": M } and the magnitudes make attr the 0x0300_1200 half,
// so that is the assignment used. If it is ever shown to be reversed, swap
// these two constants and nothing else changes.
#define HMS_FAULT_ATTR  0x03001200u
#define HMS_FAULT_CODE  0x00020001u

// 0x400d9158: classify(print_error) first, and only if that says no does the
// hms pair get consulted.
static bool error_now(void);

static bool print_error_is_fault(uint32_t code)
{
    if (code == 0) return false;                        // 0x400d8fa7
    for (int i = 0; i < 4; ++i)                         // 0x400d8fb0
        if (ERR_FORCE[i] == code) return true;
    for (int i = 0; i < 3; ++i) {                       // 0x400d8fc5
        uint32_t diff = ERR_MASKED[i].value ^ code;
        if ((diff & ~ERR_MASKED[i].mask) == 0) return false;
    }
    for (int i = 0; i < 42; ++i)                        // 0x400d8fe0
        if (ERR_IGNORE[i] == code) return false;
    return true;                                        // 0x400d8ff2
}

static bool error_now(void)
{
    return print_error_is_fault((uint32_t)g_live.print_error) || g_live.hms_fault;
}

// ---------------------------------------------------------------------------
// THE H2D STATE MACHINE, recovered 2026-08-29 from 0x400d9178 and 0x400dc400.
//
// The discriminant is gcode_state, stored by stock as an int at REPORT BASE
// + 124 = 0x3ffb568c (writers 0x400d9300 through 0x400d9379):
//
//     IDLE 0    RUNNING 1    PREPARE 2    PAUSE 3    FINISH 4    FAILED 5
//
// That address was invisible to three earlier scans because they were anchored
// on the printer block base with offset 20, or on addmi from 0x3ffb4a78. Same
// address, different base and offset. A scan that found only two writers, both
// zero, and concluded the machine was dead was wrong for exactly that reason.
//
// 0x400d9178 turns gcode_state into the internal state at 0x3ffb5690, and
// 0x400dc400 maps that to the H2D state the renderer indexes with.
#define FINISH_HOLD_MS 30015          // literal at 0x400d0898

// 0x400dc300. Splits a RUNNING print into PREPARE or PRINTING using stg_cur
// (0x3ffb5620, report index 4) and layer_num (0x3ffb561c, index 3).
static int stage_split(int stg, int layer)
{
    if (stg == -1) return PV_ST_PREPARE;                    // 0x400dc309
    if (stg > 54)                                           // 0x400dc314
        return (stg == 255) ? (layer ? PV_ST_PRINTING : PV_ST_PREPARE)
                            : PV_ST_PRINTING;               // 0x400dc350
    if (stg >= 24)                                          // 0x400dc319
        return ((0x48008021u >> (stg - 24)) & 1u)           // 0x400d0c04
               ? PV_ST_PREPARE : PV_ST_PRINTING;            // 0x400dc32f
    if (stg > 19) return PV_ST_PRINTING;                    // 0x400dc31e
    if (stg < 0)  return PV_ST_PRINTING;                    // 0x400dc321
    uint32_t bit = 1u << stg;                               // 0x400dc338
    if (bit & 0x0008698Eu) return PV_ST_PREPARE;            // 0x400d0c00
    if (bit & 0x00000011u)                                  // 0x400dc346
        return layer ? PV_ST_PRINTING : PV_ST_PREPARE;      // 0x400dc35c
    return PV_ST_PRINTING;                                  // 0x400dc34b
}

// 0x400d9178. Produces the internal state at 0x3ffb5690.
static int internal_state_now(void)
{
    static int  prev = 0;
    static bool armed, fired;
    static int64_t t0;

    int gs = g_live.gcode_state;

    if (gs == 4 && prev == 1 && !armed) {        // 0x400d9189
        armed = true;
        t0 = esp_timer_get_time();
        fired = false;
    } else if (gs != 4) {                        // 0x400d91b0
        armed = false;
    }
    prev = gs;                                   // 0x400d91b8

    if (error_now()) { armed = false; return 1; }             // 0x400d91c7
    if (gs == 2) return 3;                                    // 0x400d91d8
    if (gs == 3) return 4;                                    // 0x400d91e5
    if (gs == 1) return 2;                                    // 0x400d91f4
    if (armed && !fired) {                                    // 0x400d9207
        if ((esp_timer_get_time() - t0) / 1000 <= FINISH_HOLD_MS)
            return 5;                                         // 0x400d9225
        fired = true;                                         // 0x400d9230
    }
    return 0;                                                 // 0x400d9238
}

// 0x400dc400.
//
// Internal 5 does NOT map straight to COMPLETE: it goes through 0x400dc3a8,
// which is a SECOND 30 second hold with its own latch byte at 0x3ffb68e8 and
// its own timestamp pair at 0x3ffb68e0, measured in microseconds against
// 29999999 (0x400d0c14). Internals 1, 2 and 4 each clear that latch on the way
// past. The two timers are chained, not duplicated, so both are modelled.
static int h2d_state_now(void)
{
    static bool fin_latched;          // 0x3ffb68e8
    static int64_t fin_t0;            // 0x3ffb68e0

    switch (internal_state_now()) {
    case 1: fin_latched = false; return PV_ST_ERROR;      // 0x400dc424
    case 2: fin_latched = false;                          // 0x400dc431
            return stage_split(g_live.stg_cur, g_live.layer_num);
    case 3: return PV_ST_PREPARE;                         // 0x400dc448
    case 4: fin_latched = false; return PV_ST_PAUSED;     // 0x400dc455
    case 5:                                               // 0x400dc462
        if (!fin_latched) {                               // 0x400dc3b1
            fin_t0 = esp_timer_get_time();
            fin_latched = true;
            return PV_ST_COMPLETE;                        // 0x400dc3c8
        }
        if (esp_timer_get_time() - fin_t0 > 29999999)     // 0x400d0c14
            return PV_ST_IDLE;                            // 0x400dc3f5
        return PV_ST_COMPLETE;                            // 0x400dc3fa
    default: return PV_ST_IDLE;                           // 0x400dc414
    }
}

// NOT STOCK. Reads the filament in the active tray so the material-aware vent
// policy has something to decide on. Stock never touches the AMS.
//
// Bambu's report carries it as:
//   print.ams.tray_now  "0".."15" global tray index, "254" = external spool
//   print.ams.ams[]     one object per AMS unit, id "0".., tray[] of four
//   print.vt_tray       the external spool, id "254"
// A partial report often omits tray_now, so the last one seen is kept.
static int s_tray_now = -1;

static void take_material(const char *s)
{
    if (!s || !s[0]) return;
    if (strcmp(s, g_live.material) == 0) return;
    snprintf(g_live.material, sizeof(g_live.material), "%s", s);
    ESP_LOGI(TAG, "filament -> %s", g_live.material);
}

static void parse_material(cJSON *print)
{
    cJSON *vt = cJSON_GetObjectItemCaseSensitive(print, "vt_tray");
    cJSON *ams = cJSON_GetObjectItemCaseSensitive(print, "ams");

    if (cJSON_IsObject(ams)) {
        cJSON *tn = cJSON_GetObjectItemCaseSensitive(ams, "tray_now");
        if (cJSON_IsString(tn) && tn->valuestring[0]) {
            s_tray_now = atoi(tn->valuestring);
        }
    }

    // External spool. 255 is Bambu's "nothing selected".
    if (s_tray_now == 254 && cJSON_IsObject(vt)) {
        cJSON *tt = cJSON_GetObjectItemCaseSensitive(vt, "tray_type");
        if (cJSON_IsString(tt)) take_material(tt->valuestring);
        return;
    }
    if (s_tray_now < 0 || s_tray_now > 15) {
        // No AMS selection yet. A printer with no AMS at all still reports
        // vt_tray, so fall back to it rather than staying blank forever.
        if (cJSON_IsObject(vt)) {
            cJSON *tt = cJSON_GetObjectItemCaseSensitive(vt, "tray_type");
            if (cJSON_IsString(tt)) take_material(tt->valuestring);
        }
        return;
    }

    if (!cJSON_IsObject(ams)) return;
    cJSON *units = cJSON_GetObjectItemCaseSensitive(ams, "ams");
    if (!cJSON_IsArray(units)) return;
    cJSON *unit;
    cJSON_ArrayForEach(unit, units) {
        cJSON *uid = cJSON_GetObjectItemCaseSensitive(unit, "id");
        if (!cJSON_IsString(uid)) continue;
        int ubase = atoi(uid->valuestring) * 4;
        cJSON *trays = cJSON_GetObjectItemCaseSensitive(unit, "tray");
        if (!cJSON_IsArray(trays)) continue;
        cJSON *tray;
        cJSON_ArrayForEach(tray, trays) {
            cJSON *tid = cJSON_GetObjectItemCaseSensitive(tray, "id");
            if (!cJSON_IsString(tid)) continue;
            if (ubase + atoi(tid->valuestring) != s_tray_now) continue;
            cJSON *tt = cJSON_GetObjectItemCaseSensitive(tray, "tray_type");
            if (cJSON_IsString(tt)) take_material(tt->valuestring);
            return;
        }
    }
}

// NOT STOCK. Bambu reports its fan speeds as a STRING holding 0..15, one step
// per 1/15th, not as a percentage. "15" is full, "0" is off, and a report that
// omits the key is not the same as one that says zero.
static int fan_pct(cJSON *print, const char *key)
{
    cJSON *e = cJSON_GetObjectItemCaseSensitive(print, key);
    long raw;
    if (cJSON_IsString(e) && e->valuestring)      raw = strtol(e->valuestring, NULL, 10);
    else if (cJSON_IsNumber(e))                   raw = e->valueint;
    else                                          return -2;   // not reported
    if (raw < 0)  raw = 0;
    if (raw > 15) raw = 15;
    return (int)((raw * 100 + 7) / 15);
}

// NOT STOCK. Everything the Status page shows and nothing the vent acts on.
static void parse_status(cJSON *print)
{
    cJSON *e;

    // Chamber temperature, from EITHER shape.
    //
    // The P2S does not send chamber_temper at all. It sends
    // device.ctc.info.temp, where ctc is the chamber temperature controller,
    // and it is an integer in Celsius. The Status page was showing a dash for
    // the chamber on a machine that was reporting 33 C the whole time.
    //
    // Both are read, newest first, because the older flat field is what a P1
    // and an X1 send and this firmware has to be right on all of them.
    bool got_chamber = false;
    cJSON *dev = cJSON_GetObjectItemCaseSensitive(print, "device");
    if (cJSON_IsObject(dev)) {
        cJSON *ctc = cJSON_GetObjectItemCaseSensitive(dev, "ctc");
        if (cJSON_IsObject(ctc)) {
            cJSON *inf = cJSON_GetObjectItemCaseSensitive(ctc, "info");
            if (cJSON_IsObject(inf)) {
                e = cJSON_GetObjectItemCaseSensitive(inf, "temp");
                if (cJSON_IsNumber(e)) {
                    g_live.chamber_temp = (int)e->valuedouble;
                    got_chamber = true;
                }
            }
        }
    }
    if (!got_chamber) {
        e = cJSON_GetObjectItemCaseSensitive(print, "chamber_temper");
        if (cJSON_IsNumber(e)) g_live.chamber_temp = (int)e->valuedouble;
    }

    int f;
    if ((f = fan_pct(print, "cooling_fan_speed")) != -2) g_live.fan_part    = f;
    if ((f = fan_pct(print, "big_fan1_speed"))    != -2) g_live.fan_aux     = f;
    if ((f = fan_pct(print, "big_fan2_speed"))    != -2) g_live.fan_chamber = f;

    e = cJSON_GetObjectItemCaseSensitive(print, "total_layer_num");
    if (cJSON_IsNumber(e)) g_live.layer_total = e->valueint;

    // Sent as a number on most firmwares and a string on a few, same as
    // mc_percent.
    e = cJSON_GetObjectItemCaseSensitive(print, "mc_remaining_time");
    if (cJSON_IsNumber(e))      g_live.remain_min = e->valueint;
    else if (cJSON_IsString(e)) g_live.remain_min = (int)strtol(e->valuestring, NULL, 10);

    e = cJSON_GetObjectItemCaseSensitive(print, "spd_lvl");
    if (cJSON_IsNumber(e)) g_live.spd_lvl = e->valueint;

    // subtask_name is the job as the user named it. gcode_file is the path it
    // was sliced to, which is the same thing spelled less kindly, so it is
    // only used when there is no subtask_name at all.
    e = cJSON_GetObjectItemCaseSensitive(print, "subtask_name");
    if (!cJSON_IsString(e) || !e->valuestring[0])
        e = cJSON_GetObjectItemCaseSensitive(print, "gcode_file");
    if (cJSON_IsString(e) && e->valuestring[0]) {
        const char *n = e->valuestring;
        // Keep the file name, drop the folder it happened to be in.
        const char *slash = strrchr(n, '/');
        if (slash) n = slash + 1;
        strlcpy(g_live.job_name, n, sizeof g_live.job_name);
    }

    e = cJSON_GetObjectItemCaseSensitive(print, "wifi_signal");
    if (cJSON_IsString(e) && e->valuestring[0])
        strlcpy(g_live.printer_rssi, e->valuestring, sizeof g_live.printer_rssi);

    // ---- NOT STOCK. Everything below was already on the wire and nothing
    // was reading it. All readings; none of it is written from here.

    // Filament present in the extruder. This is the runout sensor, and it is
    // the one number that says why a print stopped when nothing else does.
    e = cJSON_GetObjectItemCaseSensitive(print, "hw_switch_state");
    if (cJSON_IsNumber(e)) g_live.filament_in = e->valueint ? 1 : 0;

    // Speed MAGNITUDE, which is not the speed LEVEL beside it. The level is
    // one of four presets; the magnitude is the percentage those presets
    // currently work out to, and they disagree often enough to be worth both.
    e = cJSON_GetObjectItemCaseSensitive(print, "spd_mag");
    if (cJSON_IsNumber(e)) g_live.spd_mag = e->valueint;

    e = cJSON_GetObjectItemCaseSensitive(print, "nozzle_diameter");
    if (cJSON_IsString(e) && e->valuestring[0])
        strlcpy(g_live.nozzle_dia, e->valuestring, sizeof g_live.nozzle_dia);
    else if (cJSON_IsNumber(e))
        snprintf(g_live.nozzle_dia, sizeof g_live.nozzle_dia, "%.1f", e->valuedouble);

    e = cJSON_GetObjectItemCaseSensitive(print, "nozzle_type");
    if (cJSON_IsString(e) && e->valuestring[0])
        strlcpy(g_live.nozzle_kind, e->valuestring, sizeof g_live.nozzle_kind);

    // The newer shape carries the nozzle under device.nozzle.info[0], where
    // the type is a code like "HH01" rather than the older "hardened_steel".
    cJSON *dev2 = cJSON_GetObjectItemCaseSensitive(print, "device");
    if (cJSON_IsObject(dev2)) {
        cJSON *nz = cJSON_GetObjectItemCaseSensitive(dev2, "nozzle");
        cJSON *inf = nz ? cJSON_GetObjectItemCaseSensitive(nz, "info") : NULL;
        cJSON *first = cJSON_IsArray(inf) ? cJSON_GetArrayItem(inf, 0) : NULL;
        if (cJSON_IsObject(first)) {
            cJSON *d = cJSON_GetObjectItemCaseSensitive(first, "diameter");
            if (cJSON_IsNumber(d))
                snprintf(g_live.nozzle_dia, sizeof g_live.nozzle_dia, "%.1f", d->valuedouble);
            cJSON *ty = cJSON_GetObjectItemCaseSensitive(first, "type");
            if (cJSON_IsString(ty) && ty->valuestring[0])
                strlcpy(g_live.nozzle_kind, ty->valuestring, sizeof g_live.nozzle_kind);
        }
    }

    // The door, from `stat`. A hex string, and bit 0x00800000 is the door.
    // The older machines put it in home_flag instead, which is a signed int
    // that arrives negative, so it is masked rather than compared.
    e = cJSON_GetObjectItemCaseSensitive(print, "stat");
    if (cJSON_IsString(e) && e->valuestring[0]) {
        unsigned long v = strtoul(e->valuestring, NULL, 16);
        g_live.door_open = (v & 0x00800000UL) ? 1 : 0;
    }

    // A firmware update waiting on the printer. 1 means one is available and
    // 2 means none, which is the opposite way round from every other flag in
    // this report, so it is read explicitly rather than as a truth value.
    cJSON *up = cJSON_GetObjectItemCaseSensitive(print, "upgrade_state");
    if (cJSON_IsObject(up)) {
        e = cJSON_GetObjectItemCaseSensitive(up, "new_version_state");
        if (cJSON_IsNumber(e)) g_live.fw_update = (e->valueint == 1) ? 1 : 0;
    }

    // The first HMS fault, spelled the way Bambu's own wiki spells them, so
    // the code on the page can be pasted into a search and find the answer.
    // The count is kept separately and already exists.
    cJSON *hms = cJSON_GetObjectItemCaseSensitive(print, "hms");
    if (cJSON_IsArray(hms)) {
        cJSON *h0 = cJSON_GetArrayItem(hms, 0);
        if (cJSON_IsObject(h0)) {
            cJSON *at = cJSON_GetObjectItemCaseSensitive(h0, "attr");
            cJSON *cd = cJSON_GetObjectItemCaseSensitive(h0, "code");
            if (cJSON_IsNumber(at) && cJSON_IsNumber(cd)) {
                unsigned a = (unsigned)at->valuedouble, c = (unsigned)cd->valuedouble;
                snprintf(g_live.hms_code, sizeof g_live.hms_code,
                         "%04X_%04X_%04X_%04X",
                         a >> 16, a & 0xFFFF, c >> 16, c & 0xFFFF);
            }
        } else {
            g_live.hms_code[0] = 0;
        }
    }

    // The camera. Reported under its own object, and every field of it is a
    // reading: the only one that can be changed from here is the recording
    // switch, and that goes out on the camera envelope rather than as G-code.
    cJSON *cam = cJSON_GetObjectItemCaseSensitive(print, "ipcam");
    if (cJSON_IsObject(cam)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItemCaseSensitive(cam, "ipcam_dev"))) {
            if (cJSON_IsString(v))      g_live.cam_present = v->valuestring[0] == '1';
            else if (cJSON_IsNumber(v)) g_live.cam_present = v->valueint ? 1 : 0;
        }
        if ((v = cJSON_GetObjectItemCaseSensitive(cam, "ipcam_record")) && cJSON_IsString(v))
            g_live.cam_record = !strcmp(v->valuestring, "enable");
        if ((v = cJSON_GetObjectItemCaseSensitive(cam, "timelapse")) && cJSON_IsString(v))
            g_live.cam_timelapse = !strcmp(v->valuestring, "enable");
        if ((v = cJSON_GetObjectItemCaseSensitive(cam, "resolution")) &&
            cJSON_IsString(v) && v->valuestring[0])
            strlcpy(g_live.cam_res, v->valuestring, sizeof g_live.cam_res);
        if ((v = cJSON_GetObjectItemCaseSensitive(cam, "rtsp_url")) &&
            cJSON_IsString(v) && v->valuestring[0])
            strlcpy(g_live.cam_rtsp, v->valuestring, sizeof g_live.cam_rtsp);
        // Kilobytes on the wire. Megabytes are what a person reads.
        if ((v = cJSON_GetObjectItemCaseSensitive(cam, "tl_internal_free_kb")) && cJSON_IsNumber(v))
            g_live.cam_free_mb = (int)(v->valuedouble / 1024.0);
        if ((v = cJSON_GetObjectItemCaseSensitive(cam, "tl_internal_total_kb")) && cJSON_IsNumber(v))
            g_live.cam_total_mb = (int)(v->valuedouble / 1024.0);
    }

    // The AMS: its humidity and temperature, and what is in each tray.
    cJSON *ams = cJSON_GetObjectItemCaseSensitive(print, "ams");
    if (cJSON_IsObject(ams)) {
        e = cJSON_GetObjectItemCaseSensitive(ams, "tray_now");
        if (cJSON_IsString(e))      g_live.tray_now = (int)strtol(e->valuestring, NULL, 10);
        else if (cJSON_IsNumber(e)) g_live.tray_now = e->valueint;

        cJSON *units = cJSON_GetObjectItemCaseSensitive(ams, "ams");
        cJSON *u0 = cJSON_IsArray(units) ? cJSON_GetArrayItem(units, 0) : NULL;
        if (cJSON_IsObject(u0)) {
            // humidity is a LEVEL, 1 to 5. humidity_raw is the percentage.
            // They are different numbers and showing one as the other is how
            // a 33% reading becomes "2%".
            e = cJSON_GetObjectItemCaseSensitive(u0, "humidity");
            if (cJSON_IsString(e))      g_live.ams_humidity = (int)strtol(e->valuestring, NULL, 10);
            else if (cJSON_IsNumber(e)) g_live.ams_humidity = e->valueint;

            e = cJSON_GetObjectItemCaseSensitive(u0, "humidity_raw");
            if (cJSON_IsString(e))      g_live.ams_humidity_pct = (int)strtol(e->valuestring, NULL, 10);
            else if (cJSON_IsNumber(e)) g_live.ams_humidity_pct = e->valueint;

            e = cJSON_GetObjectItemCaseSensitive(u0, "temp");
            if (cJSON_IsString(e))      g_live.ams_temp = (int)strtod(e->valuestring, NULL);
            else if (cJSON_IsNumber(e)) g_live.ams_temp = (int)e->valuedouble;

            cJSON *trays = cJSON_GetObjectItemCaseSensitive(u0, "tray");
            if (cJSON_IsArray(trays)) {
                int i = 0;
                cJSON *tr;
                cJSON_ArrayForEach(tr, trays) {
                    if (i >= PV_TRAY_MAX - 1) break;   // the last slot is the external spool
                    g_live.tray[i].type[0] = 0;
                    g_live.tray[i].color[0] = 0;
                    g_live.tray[i].remain = -1;
                    cJSON *v;
                    if ((v = cJSON_GetObjectItemCaseSensitive(tr, "tray_type")) &&
                        cJSON_IsString(v) && v->valuestring[0])
                        strlcpy(g_live.tray[i].type, v->valuestring, sizeof g_live.tray[i].type);
                    if ((v = cJSON_GetObjectItemCaseSensitive(tr, "tray_color")) &&
                        cJSON_IsString(v) && v->valuestring[0])
                        strlcpy(g_live.tray[i].color, v->valuestring, sizeof g_live.tray[i].color);
                    if ((v = cJSON_GetObjectItemCaseSensitive(tr, "remain")) && cJSON_IsNumber(v))
                        g_live.tray[i].remain = v->valueint;
                    ++i;
                }
            }
        }
    }
}

static void handle_report(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    cJSON *print = cJSON_GetObjectItemCaseSensitive(root, "print");
#if PV_POLICY_TEST_HOOK
    // TEST BUILD ONLY, compiled out of every shipping image. Holds the live
    // values a test injected so the next report does not wipe them.
    if (g_test_live_lock) print = NULL;
#endif
    if (print) {
        // NOT STOCK. The printer's answer to a command we sent.
        //
        // A command that is refused looks exactly like one that was never
        // sent: nothing changes and nothing is said. The printer DOES answer,
        // on this same topic, echoing the command name and a result, so the
        // answer is logged. Without this the print_speed bug looked like a
        // fault in the vent for as long as anyone cared to look, when the
        // printer had been rejecting a duplicate sequence_id all along.
        {
            cJSON *cmd = cJSON_GetObjectItemCaseSensitive(print, "command");
            cJSON *res = cJSON_GetObjectItemCaseSensitive(print, "result");
            if (cJSON_IsString(cmd) && cJSON_IsString(res)
                && strcmp(cmd->valuestring, "push_status") != 0) {
                cJSON *sq = cJSON_GetObjectItemCaseSensitive(print, "sequence_id");
                // The reason, when there is one. A bare "failed" says the
                // command was refused but not what was wrong with it, and
                // that is the difference between a fix and a guess.
                cJSON *rs = cJSON_GetObjectItemCaseSensitive(print, "reason");
                if (!cJSON_IsString(rs)) rs = cJSON_GetObjectItemCaseSensitive(print, "err");
                if (!cJSON_IsString(rs)) rs = cJSON_GetObjectItemCaseSensitive(print, "errno");
                ESP_LOGI(TAG, "printer answered \"%s\" seq %s: %s%s%s",
                         cmd->valuestring,
                         cJSON_IsString(sq) ? sq->valuestring : "?",
                         res->valuestring,
                         cJSON_IsString(rs) ? " reason=" : "",
                         cJSON_IsString(rs) ? rs->valuestring : "");
                // Keep it, so the page can say what the printer said rather
                // than showing a control that silently springs back.
                {
                    pv_cmd_ack_t a = { 0 };
                    snprintf(a.cmd, sizeof(a.cmd), "%s", cmd->valuestring);
                    a.ok = (strcasecmp(res->valuestring, "success") == 0);
                    if (!a.ok && cJSON_IsString(rs))
                        snprintf(a.reason, sizeof(a.reason), "%s", rs->valuestring);
                    a.at_s = (int)(esp_timer_get_time() / 1000000);
                    portENTER_CRITICAL(&s_ack_lock);
                    s_ack = a;
                    portEXIT_CRITICAL(&s_ack_lock);
                    pv_ws_push_state();
                }
            }
        }
        // Report key index 1. Parsed before gcode_state because the ERROR
        // decision below depends on it.
        cJSON *pe = cJSON_GetObjectItemCaseSensitive(print, "print_error");
        if (cJSON_IsNumber(pe)) g_live.print_error = pe->valueint;

        // 0x400d900c walks the array looking for one exact pair. A report
        // that omits hms leaves the previous verdict standing, matching
        // stock, whose list and count are only ever overwritten by a parse.
        cJSON *hms = cJSON_GetObjectItemCaseSensitive(print, "hms");
        if (cJSON_IsArray(hms)) {
            bool hit = false;
            cJSON *ent;
            cJSON_ArrayForEach(ent, hms) {
                cJSON *at = cJSON_GetObjectItemCaseSensitive(ent, "attr");
                cJSON *cd = cJSON_GetObjectItemCaseSensitive(ent, "code");
                if (cJSON_IsNumber(at) && cJSON_IsNumber(cd) &&
                    (uint32_t)at->valuedouble == HMS_FAULT_ATTR &&
                    (uint32_t)cd->valuedouble == HMS_FAULT_CODE) {
                    hit = true;
                    break;
                }
            }
            g_live.hms_fault = hit;
        }

        // Stored as stock's enum, not mapped to a device state here. A report
        // that omits the key leaves the value alone, exactly as stock's slot is
        // only ever touched by a parse.
        cJSON *gs = cJSON_GetObjectItemCaseSensitive(print, "gcode_state");
        if (cJSON_IsString(gs)) {
            const char *st = gs->valuestring;
            if      (!strcmp(st, "IDLE"))    g_live.gcode_state = 0;
            else if (!strcmp(st, "RUNNING")) g_live.gcode_state = 1;
            else if (!strcmp(st, "PREPARE")) g_live.gcode_state = 2;
            else if (!strcmp(st, "PAUSE"))   g_live.gcode_state = 3;
            else if (!strcmp(st, "FINISH"))  g_live.gcode_state = 4;
            else if (!strcmp(st, "FAILED"))  g_live.gcode_state = 5;
            // Stock matches these six strings and nothing else, so "INIT" and
            // "SLICING", which the clone used to fold into IDLE and PREPARE,
            // are not stock behaviour and are no longer matched.
        }
        cJSON *sc = cJSON_GetObjectItemCaseSensitive(print, "stg_cur");
        if (cJSON_IsNumber(sc)) g_live.stg_cur = sc->valueint;
        cJSON *ln = cJSON_GetObjectItemCaseSensitive(print, "layer_num");
        if (cJSON_IsNumber(ln)) g_live.layer_num = ln->valueint;
        // NOT STOCK. mc_percent is the printer's own completion figure, the
        // same number its screen shows. Bambu sends it as a number on most
        // firmwares and as a decimal string on a few, so accept both, and
        // clamp: a report has been seen carrying 101 at the very end of a job.
        cJSON *pc = cJSON_GetObjectItemCaseSensitive(print, "mc_percent");
        if (cJSON_IsNumber(pc) || cJSON_IsString(pc)) {
            int v = cJSON_IsNumber(pc) ? pc->valueint
                                       : (int)strtol(pc->valuestring, NULL, 10);
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            g_live.print_percent = v;
        }

        // Stock runs the machine on EVERY report pass: 0x400d91c1 and the
        // branches after it are reached unconditionally, including down the
        // 0x400d91b0 path.
        int ds = h2d_state_now();
        if (ds != g_live.device_state) {
            g_live.device_state = ds;
            ESP_LOGI(TAG, "printer state -> %d (gcode_state %d)", ds, g_live.gcode_state);
            pv_rgb_notify();
            pv_motor_update();
        }
        cJSON *bed = cJSON_GetObjectItemCaseSensitive(print, "bed_temper");
        if (cJSON_IsNumber(bed)) g_live.bed_temp = (int)bed->valuedouble;
        cJSON *noz = cJSON_GetObjectItemCaseSensitive(print, "nozzle_temper");
        if (cJSON_IsNumber(noz)) g_live.nozzle_temp = (int)noz->valuedouble;

        // NOT STOCK. Telemetry the vent does not act on. It changes nothing
        // about what the flap or the strip do; it exists so the Status page
        // can show what the printer is doing. A field the printer omits is
        // left exactly as it was, so a partial report cannot blank the page.
        parse_status(print);

        // NOT STOCK. Stock never looks at the AMS; this is here only to feed
        // the material-aware vent policy.
        parse_material(print);

        // The printer's own chamber light, for "Follow Printer Light".
        // Bambu reports it as print.lights_report:
        //   [ { "node": "chamber_light", "mode": "on" | "off" }, ... ]
        cJSON *lights = cJSON_GetObjectItemCaseSensitive(print, "lights_report");
        if (cJSON_IsArray(lights)) {
            cJSON *e;
            cJSON_ArrayForEach(e, lights) {
                cJSON *node = cJSON_GetObjectItemCaseSensitive(e, "node");
                cJSON *mode = cJSON_GetObjectItemCaseSensitive(e, "mode");
                if (!cJSON_IsString(node) || !cJSON_IsString(mode)) continue;
                bool on = !strcmp(mode->valuestring, "on");
                if (!strcmp(node->valuestring, "chamber_light")) {
                    if (on != g_live.printer_light) {
                        g_live.printer_light = on;
                        ESP_LOGI(TAG, "printer chamber light -> %s",
                                 on ? "on" : "off");
                        pv_rgb_notify();
                    }
                } else if (!strcmp(node->valuestring, "work_light")) {
                    // NOT STOCK. The toolhead light. Reported by every machine
                    // that has one, and -1 until one says so, because "off"
                    // and "this printer has no such light" are different
                    // answers and the page draws them differently.
                    g_live.work_light = on ? 1 : 0;
                }
            }
        }

        // NOT STOCK. Stock only re-evaluates the vent when the device state
        // changes, which is why this is a second call rather than a move: with
        // the policy off, nothing below runs and the stock path above is the
        // only one that ever touches the motor. With it on, the vent has to
        // react to bed temperature and to a filament swap, neither of which
        // moves the device state.
        if (g_pol.enable) pv_motor_update();
    }
    cJSON_Delete(root);
}

#if PV_POLICY_TEST_HOOK
// TEST BUILD ONLY, compiled out of every shipping image. Pushes a report
// through the real parser, so the filament path can be exercised with real
// captured printer JSON instead of only with whatever spool happens to be
// loaded. The lock is lifted for this one call so the report is not the thing
// it is meant to hold off.
void pv_test_feed_report(const char *json, int len)
{
    bool saved = g_test_live_lock;
    g_test_live_lock = false;
    handle_report(json, len);
    g_test_live_lock = saved;
}
#endif

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        // The broker accepted "bblp" + the access code, so the address and
        // the code are both right. Whether the SERIAL is right is not known
        // until something arrives on device/<sn>/report; sn_watch_task
        // decides that.
        g_live.printer_state = 3;
        s_last_report = 0;
        s_connected_at = xTaskGetTickCount();
        esp_mqtt_client_subscribe(s_client, s_report_topic, 0);
        esp_mqtt_client_publish(s_client, s_request_topic, PUSHALL, 0, 0, 0);
        pv_ws_push_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (g_live.printer_state == 3) g_live.printer_state = 2;
        pv_ws_push_state();
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle &&
            ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            g_live.printer_state = 6;   // access code rejected
        } else if (ev->error_handle &&
                   ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            g_live.printer_state = 4;   // no route / ip error
            // Factory behaviour: do not just show the error. Re-run discovery
            // for the same serial and follow the printer to its new address.
            relocate_start();
        }
        pv_ws_push_state();
        break;
    case MQTT_EVENT_DATA:
        // Reports up to ~20 KB arrive fragmented; only parse single-part
        // messages and the first fragment carrying the JSON start (state and
        // temperature keys sit early in Bambu reports).
        if (ev->current_data_offset == 0 && ev->topic_len &&
            strncmp(ev->topic, s_report_topic, ev->topic_len) == 0) {
            if (ev->total_data_len == ev->data_len) {
                s_last_report = xTaskGetTickCount();
                handle_report(ev->data, ev->data_len);
            } else {
                // Fragment: parse best-effort by truncating to last full JSON
                // not possible; skip. pushall arrives once, deltas are small.
            }
        }
        break;
    default:
        break;
    }
}

void pv_bambu_disconnect(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    // printer_state is NOT reset. Every writer of stock's link word at
    // 0x3ffb4a7c was enumerated 2026-08-28 and the only values ever stored
    // are 3, 7, 5, 5, 2, 4, 4, 6 (0x400d9584, 0x400d95bc, 0x400d95f4,
    // 0x400d960a, 0x400d9632, 0x400d9645, 0x400d964e, 0x400d965f). Nothing
    // stores 0 after boot, so stock leaves the last state standing on
    // disconnect and so do we. Setting it to 0 here was an invented reset,
    // and it also cleared the level 3 yellow marquee that stock keeps up.
    g_live.device_state = PV_ST_IDLE;
    // Deliberately NOT reset. Stock's report array is written only by the
    // parser at 0x400d9244 and nothing in the image ever clears it: not on
    // disconnect, not on unbind, and a report that omits a key leaves the
    // slot alone (the loop just continues, 0x400d92bd). So stock keeps the
    // last temperature it ever saw, and stays hot on a dropped printer.
    // Resetting here made the vent go green where stock stays red.
    g_live.printer_light = false;
    pv_rgb_notify();
}

void pv_bambu_rebind(void)
{
    if (!s_started) return;
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    if (!g_cfg.printer.sn[0] || !g_cfg.printer.ip[0] || !g_cfg.printer.access_code[0]) {
        g_live.printer_state = g_cfg.printer.sn[0] ? 1 : 0;
        return;
    }
    snprintf(s_report_topic, sizeof(s_report_topic), "device/%s/report", g_cfg.printer.sn);
    snprintf(s_request_topic, sizeof(s_request_topic), "device/%s/request", g_cfg.printer.sn);
    char uri[64];
    snprintf(uri, sizeof(uri), "mqtts://%s:8883", g_cfg.printer.ip);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = "bblp",
        .credentials.authentication.password = g_cfg.printer.access_code,
        // A P-series pushall report measures ~19 KB; the buffer must hold it
        // whole or the connect-time state snapshot is lost to fragmentation.
        .buffer.size = 24576,
        .buffer.out_size = 1024,
        .network.timeout_ms = 8000,
        .session.keepalive = 30,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { g_live.printer_state = 7; return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    g_live.printer_state = 2;
    esp_mqtt_client_start(s_client);
    pv_ws_push_state();
}

// A wrong serial still connects: the broker authenticates the access code,
// not the topic. The only symptom is silence on device/<sn>/report. The
// factory app distinguishes SN errors (state 5) from access code errors
// (state 6) and shows different text, so the firmware has to tell them apart.
#define PV_SN_ANSWER_TIMEOUT_MS 20000

static void sn_watch_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (g_live.printer_state != 3) continue;
        if (s_last_report) continue;                 // the serial answered
        TickType_t since = xTaskGetTickCount() - s_connected_at;
        if (since > pdMS_TO_TICKS(PV_SN_ANSWER_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "no report on device/%s/report in %d s: wrong serial",
                     g_cfg.printer.sn, PV_SN_ANSWER_TIMEOUT_MS / 1000);
            g_live.printer_state = 5;                // sn error
            pv_ws_push_state();
        }
    }
}

void pv_bambu_start(void)
{
    s_started = true;
    xTaskCreate(sn_watch_task, "pv_snwatch", 3072, NULL, 3, NULL);
    // The actual connect happens when STA gets an IP (pv_wifi calls rebind).
}

// ---------- SSDP discovery (factory printer scan) ----------

#define SSDP_GROUP "239.255.255.250"
#define SSDP_PORT  2021

typedef struct { char name[32]; char sn[24]; char ip[16]; } found_t;

static int ssdp_collect(found_t *found, int max)
{
    int nfound = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in addr = { .sin_family = AF_INET,
            .sin_port = htons(SSDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
        bind(sock, (struct sockaddr *)&addr, sizeof(addr));
        struct ip_mreq mreq = {0};
        mreq.imr_multiaddr.s_addr = inet_addr(SSDP_GROUP);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

        const char *msearch =
            "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:2021\r\n"
            "MAN: \"ssdp:discover\"\r\nMX: 3\r\n"
            "ST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";
        struct sockaddr_in dst = { .sin_family = AF_INET,
            .sin_port = htons(SSDP_PORT), .sin_addr.s_addr = inet_addr(SSDP_GROUP) };
        sendto(sock, msearch, strlen(msearch), 0, (struct sockaddr *)&dst, sizeof(dst));

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(4000);
        char buf[1024];
        while (xTaskGetTickCount() < end && nfound < max) {
            struct sockaddr_in src; socklen_t sl = sizeof(src);
            int r = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &sl);
            if (r <= 0) continue;
            buf[r] = '\0';
            found_t f = {0};
            inet_ntoa_r(src.sin_addr, f.ip, sizeof(f.ip));
            for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
                if (!strncasecmp(line, "USN:", 4)) {
                    const char *v = line + 4; while (*v == ' ') ++v;
                    snprintf(f.sn, sizeof(f.sn), "%s", v);
                } else if (!strncasecmp(line, "DevName.bambu.com:", 18)) {
                    const char *v = line + 18; while (*v == ' ') ++v;
                    snprintf(f.name, sizeof(f.name), "%s", v);
                } else if (!strncasecmp(line, "Location:", 9)) {
                    const char *v = line + 9; while (*v == ' ') ++v;
                    if (*v) snprintf(f.ip, sizeof(f.ip), "%s", v);
                }
            }
            if (!f.sn[0]) continue;
            bool dup = false;
            for (int i = 0; i < nfound; ++i)
                if (!strcmp(found[i].sn, f.sn)) dup = true;
            if (!dup) {
                if (!f.name[0]) snprintf(f.name, sizeof(f.name), "%s", f.sn);
                found[nfound++] = f;
            }
        }
        close(sock);
    }
    return nfound;
}

// The plain user-initiated scan: collect and publish the list.
static void scan_task(void *arg)
{
    found_t found[6];
    int nfound = ssdp_collect(found, 6);

    cJSON *root = cJSON_CreateObject();
    cJSON *pr = cJSON_AddObjectToObject(root, "printer");
    cJSON_AddNumberToObject(pr, "scan", 2);
    cJSON *list = cJSON_AddArrayToObject(pr, "list");
    for (int i = 0; i < nfound; ++i) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", found[i].name);
        cJSON_AddStringToObject(e, "sn", found[i].sn);
        cJSON_AddStringToObject(e, "ip", found[i].ip);
        cJSON_AddStringToObject(e, "access_code", "");
        cJSON_AddItemToArray(list, e);
    }
    g_live.printer_scan = 0;
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    pv_ws_broadcast(out);
    ESP_LOGI(TAG, "ssdp scan done, %d printer(s)", nfound);
    vTaskDelete(NULL);
}


// ---------------------------------------------------------------------------
// IP-change recovery. When a bound printer stops answering at its stored
// address the factory does not just sit on an error: it re-runs discovery
// looking for the SAME serial, and reports the outcome through printer.scan.
// The factory app has a dialog for each result, which is how the state
// numbering is known:
//
//   3  ip_change_scanning   "..." (spinner)
//   4  sn not matched       "No printer with the same SN code was scanned,
//                            rescanning."
//   5  ip not changed       "The IP has not changed, reconnecting. (Or please
//                            go to the AP menu to confirm if the Hotspot IP
//                            conflicts with the IP range of your own router)"
//   6  new ip applied       "The IP has changed, reconnecting with the new IP."
// ---------------------------------------------------------------------------

static bool s_relocating;

static void scan_publish(int scan_state)
{
    g_live.printer_scan = scan_state;
    pv_ws_push_state();
}

static void relocate_task(void *arg)
{
    found_t found[6];
    char want[24];
    snprintf(want, sizeof(want), "%s", g_cfg.printer.sn);

    for (int attempt = 0; attempt < 3 && want[0]; ++attempt) {
        scan_publish(3);                       // ip_change_scanning
        int n = ssdp_collect(found, 6);

        const found_t *hit = NULL;
        for (int i = 0; i < n; ++i)
            if (!strcmp(found[i].sn, want)) { hit = &found[i]; break; }

        if (!hit) {
            ESP_LOGW(TAG, "relocate: sn %s not seen (attempt %d)", want, attempt + 1);
            scan_publish(4);                   // sn not matched, will rescan
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        if (!strcmp(hit->ip, g_cfg.printer.ip)) {
            ESP_LOGW(TAG, "relocate: sn %s still at %s", want, hit->ip);
            scan_publish(5);                   // ip not changed
            vTaskDelay(pdMS_TO_TICKS(500));
            g_live.printer_scan = 0;
            pv_bambu_rebind();                 // "reconnecting"
            break;
        }

        ESP_LOGW(TAG, "relocate: sn %s moved %s -> %s", want,
                 g_cfg.printer.ip, hit->ip);
        snprintf(g_cfg.printer.ip, sizeof(g_cfg.printer.ip), "%s", hit->ip);
        pv_cfg_save();
        scan_publish(6);                       // new ip applied
        vTaskDelay(pdMS_TO_TICKS(500));
        g_live.printer_scan = 0;
        pv_bambu_rebind();                     // "reconnecting with the new IP"
        break;
    }

    g_live.printer_scan = 0;
    s_relocating = false;
    pv_ws_push_state();
    vTaskDelete(NULL);
}

// Called when a bound printer looks unreachable at its stored address.
static void relocate_start(void)
{
    if (s_relocating || !g_cfg.printer.sn[0]) return;
    s_relocating = true;
    xTaskCreate(relocate_task, "pv_reloc", 4096, NULL, 4, NULL);
}

void pv_bambu_scan_start(void)
{
    if (g_live.printer_scan == 1) return;
    g_live.printer_scan = 1;
    pv_ws_push_state();
    xTaskCreate(scan_task, "pv_ssdp", 4096, NULL, 4, NULL);
}
