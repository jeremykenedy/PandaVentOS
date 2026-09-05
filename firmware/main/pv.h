#pragma once
// PandaVentOS: a from-scratch re-creation of the BIQU Panda Vent factory
// application. The contract is the FACTORY web UI (served byte-exact from
// this firmware) and the factory WebSocket protocol captured from a running
// stock device. Anything the factory app does, this app
// does, before anything new is added.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   // size_t, used in the declarations below
#include "esp_err.h"

// ---------------------------------------------------------------------------
// Version. ONE definition, used by the state document, the UI badge and the
// release tag, so those three can never disagree.
//
// PV_FW_VERSION is what the FACTORY protocol's fw_version field carries. It
// stays "V1.0.0" because anything speaking that protocol treats it as the
// protocol's own revision, and this firmware speaks revision 1.0.0 of it.
// Reporting something else there would be lying about what is on the wire.
// It says nothing about which firmware is running; PV_OS_VERSION does that.
//
// PV_OS_VERSION is THIS project's own version, semantic, and is what a user
// reads in the corner of the page and matches against a GitHub release.
#define PV_FW_VERSION  "V1.0.0"
#define PV_OS_NAME     "PandaVentOS"
#define PV_OS_VERSION  "1.1.0"

// ---------------------------------------------------------------------------
// Hardware map (recovered from the stock binary and BTT docs; proven on this
// exact device by the previous firmware generation).
// ---------------------------------------------------------------------------
#define PV_PIN_USER_BUTTON   12   // active-low, internal pull-up
#define PV_PIN_BOOT_BUTTON   0    // active-low; long-press = factory reset
#define PV_PIN_BUTTON_LED    27   // ring LED: off = AUTO, blink = MANUAL
#define PV_PIN_STRIP0        14   // WS2812, 16 px
#define PV_PIN_STRIP1        4    // WS2812, 16 px (both strips always driven)
// The count stock actually drives. It is a single initialised word, read
// once through an accessor, cached by the render task and passed down as
// every effect function's n. Stock drives two strips, so 32 LEDs in total,
// with a 3*n = 48 byte buffer each.
#define PV_LEDS_PER_STRIP    16
#define PV_STRIP_COUNT_MAX   2
#define PV_MOTOR_GROUPS      4

// SAFE BUILD SWITCH. When 1, the LEDC channels are never configured and every
// drive request is refused, so no code path can energise a motor. For bench
// debugging only after the 2026-08-29 incident where a button press drove the
// vent against its stops until it was unplugged.
#define PV_SAFE_NO_MOTORS    0

// ---------------------------------------------------------------------------
// Factory data model. Field names and value ranges mirror the factory
// WebSocket schema one to one (rgb_mode / rgb_switch / printer / ap / sta /
// settings). Brightness and speed are 0..100 in steps of 5, colors are
// "RRGGBB" uppercase hex, exactly as the factory UI sends and expects.
// ---------------------------------------------------------------------------
// 0..6 are stock's seven, in stock's order, and their ids are part of the
// factory wire protocol. Never renumber them.
#define PV_FX_STATIC      0
#define PV_FX_BREATHING   1
#define PV_FX_STROBING    2
#define PV_FX_WAVE        3
#define PV_FX_MARQUEE     4
#define PV_FX_COLOR_CYCLE 5
#define PV_FX_RAINBOW     6
// ADDITIONS, appended so every stock id keeps its meaning. Growing this count
// grows pv_cfg_t, which the loader size-checks, so pv_cfg.c migrates the older
// layout rather than throwing the settings away. See PV_CFG_MAGIC_V1 there.
#define PV_FX_CYLON       7
#define PV_FX_BOUNCE      8
// The centre-referenced family, added 2026-08-30. Everything from 10 up comes
// in an outward/inward pair that share one renderer body and differ only in
// which end of the strip is the origin, so the two of a pair always move at
// the same rate and stop at the same place.
#define PV_FX_PROGRESS         9    // print percentage as a filled bar
#define PV_FX_MARQUEE_OUT      10   // two blobs, middle -> ends, repeat
#define PV_FX_MARQUEE_IN       11   // two blobs, ends -> middle, repeat
#define PV_FX_FILL_OUT         12   // solid grows middle -> ends, repeat
#define PV_FX_FILL_IN          13   // solid grows ends -> middle, repeat
#define PV_FX_BOUNCE_OUT       14   // blobs out, then back in, forever
#define PV_FX_BOUNCE_IN        15   // blobs in, then back out, forever
#define PV_FX_BOUNCE_FILL_OUT  16   // fill out, then unfill back in, forever
#define PV_FX_BOUNCE_FILL_IN   17   // fill in, then unfill back out, forever
// NOT STOCK. Two more progress effects, so a print in progress reads as one
// from across a room rather than as a static bar you have to look twice at.
#define PV_FX_PROGRESS_ANIM    18   // the fill, with a chase and a live tip
#define PV_FX_BARBER           19   // two-colour pole crawling through the fill
// NOT STOCK. The bed temperature, as a colour. The strip runs from the
// effect's INACTIVE colour at the cold end to its ACTIVE colour at the hot
// end, between two temperatures the user picks, which makes "is it still hot
// in there" answerable from across a room.
#define PV_FX_TEMP_GRADIENT    20
// NOT STOCK. Plays whatever animation has been uploaded into RAM. With
// nothing uploaded it renders as Static, so selecting it never leaves the
// strip dark with no explanation.
#define PV_FX_ANIM             21
#define PV_FX_COUNT       22
#define PV_FX_STOCK_COUNT 7

// Not user selectable and not part of the config arrays. Stock's warning
// override renderer is a separate function, not one of the seven: it paints
// every pixel R=127 G=0 B=0 directly, with no brightness scaling, and delays
// 10 ticks = 100 ms.
#define PV_FX_OVERRIDE_RED 100

// The other two non-selectable renderers the rgb task reaches.
//
// PV_FX_FAULT_STROBE does not render anything itself: it builds a record
// { brightness 100, speed 150, r, g, b } and tail calls Strobing. Speed 150
// through Strobing's own 200 - speed gives a 50 ms half period.
//
// PV_FX_LINK_MARQUEE has the same prologue as Static and the same travelling
// Gaussian as Marquee (5.0 cutoff, 0.3 step), but it takes no speed, its
// frame is a fixed 50 ms, and it keeps its OWN position rather than sharing
// Marquee's.
#define PV_FX_FAULT_STROBE 101
#define PV_FX_LINK_MARQUEE 102


// Warning Hot boundary, stated verbatim in the factory app's own copy:
// the printer's maximum temperature crossing 50 C is the burn-risk line.
// The comparison is a STRICT greater-than, on either temperature, and stock
// compares the parsed INTEGER, so 50.9 is 50 and reads safe.
#define PV_WARN_HOT_C     50

/* The hotspot's own address, as a string for the redirect and as a network
   number for the DNS answer and the "did this come from the hotspot" test.
   Kept beside each other so the two can never drift apart. */
#define PV_AP_PORTAL_IP   "192.168.254.1"
#define PV_AP_PORTAL_NET  0xC0A8FE00u          /* 192.168.254.0 */

#define PV_MODE_SIMPLE    0
#define PV_MODE_H2D       1
#define PV_MODE_WARNING   2

// H2D (Advance mode) device states, factory button order.
#define PV_ST_IDLE        0
#define PV_ST_PREPARE     1
#define PV_ST_PRINTING    2
#define PV_ST_PAUSED      3
#define PV_ST_COMPLETE    4
#define PV_ST_ERROR       5
#define PV_ST_COUNT       6

// Colours are stored as three RAW BYTES, not as the seven byte "RRGGBB" text
// the wire protocol uses. That is a deliberate 2026-08-30 change and it is not
// cosmetic: the NVS partition is 12 KB of which roughly 8 KB is usable, NVS
// needs room for the old AND the new copy while it rewrites a blob, and this
// struct is multiplied by 18 effects and again by 6 device states. At 16 bytes
// the config reached 2320 bytes, the rewrite stopped fitting, saves began
// failing silently, and a reboot lost every setting. At 8 bytes the whole
// config is about 1300, smaller than it was before the second colour existed.
// Text is a wire format; it does not belong in storage.
// Four colours per effect. The ACTIVE pair is what the effect paints where it
// is lit; the INACTIVE pair is what it paints where it would otherwise be dark.
// Which of each pair is used depends on where the vent actually is, so the
// strip reports the vent position without the effect having to change.
//
// An inactive colour is OPTIONAL. When its bit in `opt_set` is clear the unlit
// pixels are black, which is exactly what every effect did before this existed;
// that is why the UI's clear button just clears the bit.
//
// `bright_end` is optional in the same way and for the same reason: when its
// bit is clear the effect runs at one brightness, exactly as it always did.
// When it is set the brightness RAMPS from `brightness` at the start of each
// cycle to `bright_end` at the end of it, then starts over.
#define PV_BG_OPEN     0x01   // opt_set: the vent-open inactive colour is set
#define PV_BG_CLOSED   0x02   // opt_set: the vent-closed inactive colour is set
#define PV_BRIGHT_END  0x04   // opt_set: bright_end is set, so brightness ramps
#define PV_AUX         0x08   // opt_set: aux is set, so the effect uses it
// NOT STOCK. This effect runs the other way round.
//
// A FLIP, not a direction: it is combined with the master switch and with the
// per-strip flags by exclusive-or, so each of the three answers the question
// "should this be turned around" and no one of them silently wins. Stock has
// exactly one direction flag for the whole device; this is per effect, and
// because the per-state tables hold their own parameters, per state as well.
#define PV_FX_REVERSE  0x10

typedef struct {
    uint8_t brightness;      // 0..100, and the START of the ramp when one is set
    uint8_t speed;           // 0..100
    uint8_t rgb[3];          // ACTIVE while the vent is OPEN
    uint8_t rgb_closed[3];   // ACTIVE while the vent is CLOSED
    uint8_t bg[3];           // INACTIVE while the vent is OPEN
    uint8_t bg_closed[3];    // INACTIVE while the vent is CLOSED
    uint8_t bright_end;      // 0..100, the END of the ramp; only when PV_BRIGHT_END
    // NOT STOCK. One spare number per effect, whose MEANING belongs to the
    // effect that reads it. Barber Pole reads it as the band width in pixels.
    // An effect that does not read it ignores it, and an effect that does
    // falls back to its own default while PV_AUX is clear, so "never set" and
    // "set to zero" stay different answers.
    //
    // One shared byte rather than one byte per knob: the alternative is a
    // field per effect-specific setting, on a device whose whole configuration
    // has to fit in a single NVS blob.
    uint8_t aux;
    uint8_t opt_set;         // PV_BG_* / PV_BRIGHT_END / PV_AUX; clear means "as before"
} pv_fx_param_t;

// The two conversions between the stored bytes and the wire's "RRGGBB".
void pv_hex_to_rgb3(const char *hex, uint8_t out[3]);
void pv_rgb3_to_hex(const uint8_t rgb[3], char out[7]);

typedef struct {
    // rgb_switch / rgb_mode toggles
    bool    light_on;            // total_switch / light_on_off
    bool    warning_sw;          // warning_overide
    bool    follow_printer;      // is_follow_printer
    bool    follow_vent;         // is_follow_vent
    bool    reverse;             // is_reverse, the master flip
    // NOT STOCK. One bit per strip, so a run that is physically mounted the
    // other way round can be turned around on its own instead of forcing the
    // whole device to match it. Combined with the master flip and the
    // per-effect one by exclusive-or.
    uint8_t reverse_strips;
    uint8_t light_mode;          // rgb_light_mode / current_light_mode 0..2
    // Simple mode
    uint8_t simple_current;      // current_simple_effect 0..6
    pv_fx_param_t simple[PV_FX_COUNT];
    // H2D (Advance) mode. The per-state EFFECT TABLES are NOT here: at 18
    // effects and four colours they are 1620 bytes, which does not fit in one
    // NVS blob beside everything else. They live in g_h2d, one NVS key per
    // device state, so a change to one state rewrites 270 bytes rather than
    // relocating the entire configuration. See pv_cfg.c.
    uint8_t h2d_active[PV_ST_COUNT];              // active_effect_id per state
    // Warning hot mode: [0]=safe(green) [1]=warn(red); effect 0 Static 1 Strobing
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];    // [level][effect]
    uint8_t warnhot_speed[2][2];

    // NOT STOCK. The temperature the warning mode calls hot.
    //
    // Stock burns 50 C into the comparison. Fifty is right for a machine that
    // prints PLA and wrong for one that prints ABS, where the bed sits at a
    // hundred and the light would say "hot" for the whole job. 0 means "use
    // the stock number", so an untouched device behaves exactly as stock does.
    uint8_t warn_hot_c;

    // NOT STOCK. What a printer fault looks like.
    //
    // Stock hard-codes solid red at 127 with no brightness scaling. That is a
    // fine default and a poor rule: red is the one colour a red-green
    // colourblind owner cannot pick out, and a fault that does not move is a
    // fault that gets walked past. err_set is what makes these mean anything;
    // clear, the override is stock's, byte for byte.
    uint8_t err_rgb[3];
    uint8_t err_bright;          // 0..100
    uint8_t err_strobe;          // 0 solid, 1 strobing
    bool    err_set;

    // NOT STOCK. Treat the two runs as ONE.
    //
    // The strips are separate outputs and every effect renders on each of them
    // from its own start, so a marquee runs twice, side by side. Contiguous
    // renders one strip of leds[0] + leds[1] pixels and gives each output its
    // own slice of it, so the light travels the whole length once.
    bool    contiguous;

    // NOT STOCK. The two ends of the temperature gradient, in Celsius.
    // Zero for either means the compiled default.
    uint8_t grad_min_c;
    uint8_t grad_max_c;
} pv_rgb_cfg_t;

// NOT STOCK. The compiled defaults for the settings above, used whenever the
// stored byte is zero. Zero means "never set", which has to stay different
// from a real value: a gradient that runs to 0 C and a gradient nobody has
// configured are not the same thing.
#define PV_GRAD_MIN_C_DEFAULT 25
#define PV_GRAD_MAX_C_DEFAULT 60

typedef struct {
    char name[32];
    char sn[24];
    char access_code[16];
    char ip[16];
} pv_printer_cfg_t;

typedef struct {
    char ssid[33];
    char password[65];
    char ip[16];
    bool on;
} pv_ap_cfg_t;

typedef struct {
    uint32_t magic;              // schema guard
    pv_rgb_cfg_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];            // "en" / "zh"
    bool motor_manual;           // false = AUTO (factory default)
    bool motor_manual_open;      // manual-mode vent target
    // ADDITION, appended on purpose. Everything above keeps its offset, so a
    // config stored by the previous build migrates by copying up to here and
    // letting this field take its default. What is shown on the Control
    // Panel's Device row; empty means use PV_DEVICE_NAME_DEFAULT.
    char device_name[32];
    // ADDITIONS 2026-08-30, appended so every offset above is unchanged and a
    // v5 blob migrates by copying its prefix. See PV_RING_* below.
    uint8_t ring_mode;           // PV_RING_*
    bool    ring_blink;          // blink the ring while in MANUAL
    // ADDITION 2026-08-30. How many LEDs each strip ACTUALLY has.
    //
    // Stock drives a fixed 16 per strip. The factory manual says the hardware
    // is 16 LEDs with one strip group connected and 27 with two, so with two
    // groups at least one run is shorter than the 16 being driven. Every
    // effect that has a centre or a scale then lands wrong on that run: a
    // centre computed for 16 shows up around 68% of the way along an 11 LED
    // strip, and a progress bar scaled to 16 fills a shorter strip early.
    //
    // The counts are configurable because the split is a property of the
    // wiring, not of the firmware, and the strips cannot be interrogated:
    // WS2812 is write-only. 16/16 reproduces stock exactly.
    uint8_t leds[PV_STRIP_COUNT_MAX];
} pv_cfg_t;

// The H2D effect tables, kept out of pv_cfg_t and persisted per device state.
extern pv_fx_param_t g_h2d[PV_ST_COUNT][PV_FX_COUNT];
// Persist one device state's effect table. Cheaper than a full save and
// the only thing a change to one state actually needs.
void pv_cfg_h2d_save(int st);

// Stock hard-codes this string into the web app, where it is a translation
// entry rather than a setting. Same text, now editable.
#define PV_DEVICE_NAME_DEFAULT "Panda Vent"

// ---------------------------------------------------------------------------
// Material-aware vent policy. AN ADDITION. Stock has nothing like it.
//
// Deliberately NOT a member of pv_cfg_t: that blob is a fixed-size struct
// guarded by a magic and a size equality test, so growing it would discard
// every stored setting on the first boot after an update. This lives in its
// own NVS key and defaults cleanly when absent.
// ---------------------------------------------------------------------------
#define PV_MAT_COUNT           9
// Carried over from DragonVent, this author's earlier vent firmware, where
// both numbers were arrived at by watching a P2S cool down. See pv_policy.c.
#define PV_BED_OPEN_C_DEFAULT  45
#define PV_BED_CLOSE_C_DEFAULT 35

extern const char *const pv_material_name[PV_MAT_COUNT];
extern const bool        pv_material_seal[PV_MAT_COUNT];

typedef struct {
    uint32_t magic;
    bool     enable;         // master switch
    uint16_t rule_on;        // bit i = material rule i is active
    bool     heat_hold;      // residual-heat hysteresis once the print ends
    int16_t  bed_open_c;     // hold OPEN above this bed temperature
    int16_t  bed_close_c;    // allow CLOSED below this one
} pv_policy_cfg_t;

extern pv_policy_cfg_t g_pol;

// TEST BUILD ONLY, and off unless the environment variable PV_POLICY_TEST_HOOK
// is set at configure time (see main/CMakeLists.txt). It lets a test drive
// g_live.material, device_state and bed_temp directly and freezes the printer
// report so the injected values stand, which is the only way to exercise the
// sealing branch on a machine that has nothing but PLA in it. Nothing in the
// tree turns it on; a shipping image contains none of this code.
#ifndef PV_POLICY_TEST_HOOK
#define PV_POLICY_TEST_HOOK 0
#endif

// TEST BUILD ONLY, same rule as the hook above: nothing in the tree sets it and
// a shipping image contains none of it. It writes each rendered frame to the
// log as hex so the effects can be checked against private/SPEC/
// effects-math.md from what the DEVICE actually painted, rather than from a
// host build of the same source. Off by default; a build that wants it passes
// -DPV_FXDUMP=1 at configure time.
#ifndef PV_FXDUMP
#define PV_FXDUMP 0
#endif

// TEST BUILD ONLY. Logs every transition of both buttons, and the idle level
// of each at the first poll after boot. Kept SEPARATE from PV_FXDUMP on
// purpose: the frame dump writes two lines per frame and turns the 64-line log
// ring over in about eleven seconds, which evicts button events before they
// can be read. Enable one or the other, not both.
#ifndef PV_BTNDUMP
#define PV_BTNDUMP 0
#endif
#if PV_POLICY_TEST_HOOK
extern bool g_test_live_lock;
void pv_test_feed_report(const char *json, int len);
#endif

void pv_policy_defaults(pv_policy_cfg_t *p);
void pv_policy_load(void);
void pv_policy_save(void);
int  pv_policy_match(const char *material);          // rule index or -1
bool pv_policy_decide(bool stock_open, bool hold);   // final vent target

// ---------------------------------------------------------------------------
// NOT STOCK. The device log, kept in RAM so the page can show it.
// See pv_log.c for why it exists and what it promises.
// ---------------------------------------------------------------------------
#define PV_LOG_MAX      64
#define PV_LOG_TEXT     128

typedef struct {
    int64_t us;                  // microseconds since boot
    char    text[PV_LOG_TEXT];
} pv_log_line_t;

void pv_log_init(void);
// Fills out[] oldest first and returns how many lines were written.
int  pv_log_read(pv_log_line_t *out, int max);
void pv_log_clear(void);
// The log as a WebSocket document. Caller frees.
char *pv_json_logs(void);

// ---------------------------------------------------------------------------
// Live (non-persisted) state pushed to the UI.
// ---------------------------------------------------------------------------
// Four AMS slots plus the external spool.
#define PV_TRAY_MAX 5

typedef struct {
    // sta.state: 1 nossid, 2 connecting, 3 connected, 4 reconnecting, 5 pw err
    int  sta_state;
    char sta_ip[16];
    char sta_ssid[33];
    char sta_password[65];
    // printer.state: 0/1 unbound, 2 connecting, 3 connected,
    // 4 ip err, 5 sn err, 6 access code err, 7 unknown err
    int  printer_state;
    // wifi.scan / printer.scan: 0 idle, 1 scanning, 2 done
    // printer extras: 3 ip-change scanning, 4 sn not matched,
    // 5 ip not changed, 6 new ip applied
    int  wifi_scan;
    int  printer_scan;
    // Printer telemetry driving H2D + warning modes and AUTO venting.
    int   device_state;          // PV_ST_*
    // int, not float: stock parses these base 10 and stops at the decimal
    // point. Zero at boot, because stock's report array is in .bss and
    // nothing ever clears it.
    int bed_temp;
    int nozzle_temp;
    bool  vent_open;             // current vent target (all groups)
    // The printer's own chamber light, from print.lights_report. Drives the
    // "Follow Printer Light" switch, which the factory app describes as
    // "Automatically turns RGB effect ON and OFF following the printers
    // stock light."
    bool  printer_light;
    // print_error, report key index 1. Stock feeds it to its error
    // classifier; see pv_bambu.c. Zero at boot because stock's report array
    // is .bss and nothing ever clears it.
    int   print_error;
    // Whether the last report's hms array carried the one pair stock looks
    // for.
    bool  hms_fault;
    // gcode_state as stock stores it: 0 IDLE, 1 RUNNING, 2 PREPARE,
    // 3 PAUSE, 4 FINISH, 5 FAILED. This is the discriminant of the H2D
    // state machine.
    int   gcode_state;
    // Report keys 4 and 3. Both are consumed by the stage classifier.
    int   stg_cur;
    int   layer_num;
    // Filament in the active tray, as the printer names it ("PLA", "ABS",
    // "PETG"...). Empty until a report carries one. NOT a stock field: stock
    // never reads the AMS. Feeds the material-aware vent policy only.
    char  material[24];
    // print.mc_percent, 0..100. NOT a stock field: stock never reads it.
    // Drives the Progress Bar effect only, and stays 0 when nothing is
    // printing, which is what an empty bar should show anyway.
    int   print_percent;

    // NOT STOCK. Telemetry the vent does not act on, reported so the Status
    // page can show what the printer and the controller are doing without
    // anyone having to open a second app. RAM only: none of it is
    // configuration, so none of it costs NVS.
    //
    // -1 means "the printer has not told us", which has to stay a different
    // thing from zero: a chamber at 0 C and a printer that reports no chamber
    // at all are not the same reading.
    int   chamber_temp;          // C
    // NOT STOCK. The toolhead light: 1 on, 0 off, -1 the printer has never
    // mentioned one. Three states, not two, because a page that draws a switch
    // for a light that does not exist is worse than one that draws nothing.
    int   work_light;            // -1 unknown, 0 off, 1 on

    // NOT STOCK. Everything else the printer was already sending and nothing
    // was reading. All of it is a READING; none of it can be written from
    // here, and most of it cannot be written from anywhere without the
    // printer being in Developer Mode.
    //
    // Every one starts at -1 or empty, because "not reported" and "zero" are
    // different answers and the page draws them differently. A printer with no
    // AMS is not a printer with an AMS at 0% humidity.
    int   filament_in;           // -1 unknown, 0 no, 1 yes (runout sensor)
    int   spd_mag;               // speed magnitude, percent; -1 unknown
    int   ams_humidity;          // 1..5 level; -1 unknown
    int   ams_humidity_pct;      // 1..100; -1 unknown
    int   ams_temp;              // C; -1 unknown
    int   door_open;             // -1 unknown, 0 shut, 1 open
    int   fw_update;             // -1 unknown, 0 none, 1 one is waiting
    char  nozzle_dia[8];         // "0.4"; empty until reported
    char  nozzle_kind[8];        // "HH01" / "stainless_steel"; empty until reported
    // The first HMS code, formatted the way Bambu's own wiki spells them:
    // XXXX_XXXX_XXXX_XXXX. Empty when there are none. The COUNT is separate,
    // because "three faults" and "this is the first of three" are both worth
    // saying and neither is the other.
    char  hms_code[24];
    // The AMS trays, as reported. Four is what every AMS has; a fifth slot
    // holds the external spool when there is one.
    struct {
        char type[10];           // "PLA"; empty means the slot is empty
        char color[9];           // "RRGGBBAA"
        int  remain;             // percent, -1 unknown
    } tray[PV_TRAY_MAX];
    int   tray_now;              // active tray index, -1 none, 254 external

    // NOT STOCK. The camera.
    //
    // Reported so the Camera page can say what is there. RTSP cannot be
    // played by a browser at all, so the URL is something to paste into VLC or
    // a recorder, not something this page can show; saying so is the whole
    // point of having the page.
    int   cam_present;           // -1 unknown, 0 no camera, 1 there is one
    int   cam_record;            // -1 unknown, 0 off, 1 on
    int   cam_timelapse;         // -1 unknown, 0 off, 1 on
    int   cam_free_mb;           // internal storage free, -1 unknown
    int   cam_total_mb;          // internal storage total, -1 unknown
    char  cam_res[12];           // "1080p"; empty until reported
    // "disable" when the printer has RTSP turned off, otherwise the URL.
    // Both are worth showing and they are not the same message.
    char  cam_rtsp[96];
    int   fan_part;              // 0..100
    int   fan_aux;               // 0..100
    int   fan_chamber;           // 0..100
    int   layer_total;           // total_layer_num
    int   remain_min;            // mc_remaining_time, minutes
    int   spd_lvl;               // 1 silent, 2 standard, 3 sport, 4 ludicrous
    char  job_name[52];          // subtask_name
    char  printer_rssi[12];      // wifi_signal, as the printer words it
    int   wifi_rssi;             // the VENT's own signal, dBm, 0 when unknown
    // NOT STOCK. True when the last attempt to persist the config failed, which
    // in practice means NVS is full. The settings are live in RAM and will be
    // lost on the next reboot, so this has to be visible somewhere.
    bool  cfg_save_failed;
} pv_live_t;

// ---------------------------------------------------------------------------
extern pv_cfg_t  g_cfg;
extern pv_live_t g_live;

// pv_cfg.c
void pv_cfg_load(void);
void pv_cfg_save(void);
// What the last failing config save actually returned (an esp_err_t, 0 when
// none), and a bit per unstored key: 0x01 the main config, 0x02 << state for
// each per-state lighting table. "A save failed" alone cannot tell a full NVS
// from a corrupt page, and those need different answers.
int      pv_cfg_save_err(void);
uint16_t pv_cfg_save_keys(void);
void pv_cfg_factory_defaults(pv_cfg_t *c);          // whole config
void pv_cfg_rgb_mode_defaults(pv_rgb_cfg_t *r, int mode); // one mode's defaults
void pv_factory_reset_and_reboot(void);

// pv_json.c — full state document, factory shape and order.
// NOT STOCK. The state document, in parts. See pv_json.c for why.
// The returned pointer is a STATIC buffer, valid until the next call, and must
// not be freed. Every caller is on the HTTP server task, which is single
// threaded, so the buffer cannot be pulled out from under one of them.
#define PV_STATE_PARTS (2 + PV_ST_COUNT)
const char *pv_json_state_part(int part);                          // caller frees
char *pv_json_response(const char *type, int ok);   // caller frees

// pv_http.c
esp_err_t pv_http_start(void);
bool pv_http_is_up(void);
void pv_ws_broadcast(char *json_take_ownership);    // frees the string
void pv_ws_push_state(void);                        // broadcast full state
void pv_ws_push_state_to(int fd);                   // NOT STOCK: one client, on connect

// pv_apply.c — inbound WS message dispatch.
void pv_apply_message(const char *json, int len);

// pv_wifi.c
void pv_wifi_start(void);
/* Read stock's Wi-Fi straight out of the NVS partition, before an erase
   destroys it. Safe to call before nvs_flash_init(). */
bool pv_wifi_salvage_stock(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
void pv_wifi_scan_start(void);
// Whether the last completed scan saw an AP named "test1". Stock's factory
// self-test compares that name against all 20 scan records.
bool pv_wifi_saw_test_ap(void);
// 0 idle, 1 scanning, 2 complete. Mirrors the word stock reads to pick
// blue / green / red in test mode 1.
int  pv_wifi_test_scan_state(void);
void pv_wifi_join(const char *ssid, const char *password);
void pv_ap_apply(void);                             // reconfigure softAP now
void pv_hostname_apply(void);
void pv_hostname_sanitise(char *hn, size_t len);                       // mDNS + netif hostname

// pv_bambu.c
void pv_bambu_start(void);
void pv_bambu_rebind(void);                         // apply g_cfg.printer now
void pv_bambu_disconnect(void);
void pv_bambu_scan_start(void);                     // SSDP discovery

// NOT STOCK. Telling the printer to do something, rather than only listening.
//
// Bambu's own fan part numbers, used verbatim in the M106 that goes out, so
// there is no second numbering to keep in step with theirs.
#define PV_FAN_PART     1
#define PV_FAN_AUX      2
#define PV_FAN_CHAMBER  3
// percent 0..100, clamped. Returns false if the link is down.
bool pv_bambu_set_fan(int which, int percent);
// 1 silent, 2 standard, 3 sport, 4 ludicrous. Returns false if the link is
// down or the level is not one of those four.
bool pv_bambu_set_speed(int level);

// NOT STOCK. The printer's lights, via system.ledctrl.
//
// The ONLY write a printer outside Developer Mode accepts. Everything under
// the "print" envelope is signature checked and comes back "mqtt message
// verify failed"; ledctrl is not. Nodes: "chamber_light", "work_light".
bool pv_bambu_set_light(const char *node, bool on);

// NOT STOCK. The camera's recording switch, via camera.ipcam_record_set.
//
// A THIRD command family that is not signature checked, alongside
// system.ledctrl. Measured the same way: this returns "SUCCESS" on a printer
// that refuses every gcode_line. camera.ipcam_timelapse on the same printer
// returns "unsupport common", which is the printer declining the feature
// rather than declining the sender, so it is not offered.
bool pv_bambu_set_record(bool on);

// NOT STOCK. What the printer said about the last command sent to it.
//
// A command can leave this device correctly and still not happen, and the two
// look identical from the UI: the control snaps back to whatever the printer
// is still reporting. The printer does answer, so its answer is kept and put
// in the state document.
//
// This is not a nicety. Every command to a printer that accepts LAN reads but
// requires cloud authentication for control comes back "mqtt message verify
// failed", and without the reason on screen that is indistinguishable from a
// bug in this firmware. It took a direct MQTT client to tell them apart once;
// it should not take one again.
typedef struct {
    char cmd[20];       // "print_speed", "gcode_line"; empty if none yet
    char reason[48];    // the printer's own words, empty when it succeeded
    bool ok;
    int  at_s;          // uptime when the answer came back
} pv_cmd_ack_t;

void pv_bambu_last_ack(pv_cmd_ack_t *out);
// Whether the link layer has run once. Stock's equivalent is its link init,
// which is what first evaluates the level 3 indicator; until then the word
// keeps the 2 the rgb task armed at boot.
bool pv_bambu_started(void);

// pv_rgb.c
void pv_rgb_start(void);
void pv_rgb_notify(void);                           // config/state changed
// Stops the render task the way stock does: notification value 255, the
// strip driven all-off, then the task returns. Used before an OTA so the
// strip goes dark and RMT is released.
void pv_rgb_stop(void);
void pv_rgb_resume(void);

// NOT STOCK. LIVE PREVIEW: render a set of effect parameters right now, without
// storing them anywhere, and go back to the real configuration on its own.
//
// Everything else in this firmware saves the moment you touch it, which is
// stock's model and is right for a control panel. It is wrong for choosing a
// colour or an effect: you cannot see the strip and the picker at the same
// time, so the only way to judge a change was to commit it and then undo it,
// and undoing it is what wore out the NVS and lost settings.
//
// A preview deliberately ignores the master switch and the follow gates. If it
// did not, previewing with the lights off, or with Follow Printer on and the
// chamber light off, would show nothing and read as a broken button.
//
// state pins the printer state the LIGHT answers to, and percent pins the
// progress the progress effects fill to. Both are -1 for "use the live value".
// Neither is written back into g_live: the motor and the vent policy read the
// real device state, and a preview must never move the flap.
typedef struct {
    bool          active;
    int64_t       until_us;
    int           fx;            // PV_FX_*
    pv_fx_param_t p;             // the four colours, brightness, speed, ramp
    int8_t        state;         // -1 live, else PV_ST_*
    int8_t        percent;       // -1 live, else 0..100
} pv_preview_t;

#define PV_PREVIEW_MAX_S 120

void pv_rgb_preview(int fx, const pv_fx_param_t *p, int state, int percent, int seconds);
void pv_rgb_preview_cancel(void);
// Seconds remaining, or 0 when no preview is running.
int  pv_rgb_preview_left(void);
// The forced printer state and pinned percentage, or -1 for "use the live one".
int  pv_rgb_preview_state(void);
int  pv_rgb_preview_percent(void);

// NOT STOCK. What the renderer is actually doing, for the Status page.
// A strip that looks wrong and a strip that is not being drawn at all look the
// same from across a room; these tell them apart without a serial cable.
typedef struct {
    uint32_t frames;         // frames rendered since boot
    uint32_t push_failed;    // RMT transactions the driver refused
    int      effect;         // PV_FX_* last drawn, -1 before the first frame
    uint32_t interval_ms;    // what that effect asked to wait
    int      fps;            // over the window since this was last read, -1 unknown
} pv_rgb_stats_t;

void pv_rgb_stats(pv_rgb_stats_t *out);

// NOT STOCK. One rendered frame, the SHIPPING one: render_task calls it and
// does nothing else, and tools/fxdump calls it with a buffer and push = false
// so a host test exercises resolve, the brightness ramp, the per-strip lengths
// and the phase rewind between strips rather than just render_effect.
// Declared in pv_rgb.c, where rgb_t lives; nothing else in the firmware calls it.

// pv_motor.c
void pv_motor_start(void);
void pv_motor_set_auto(bool auto_mode);
// NOT STOCK. Three-way vent control for the Control Panel.
#define PV_VENT_AUTO   0
#define PV_VENT_OPEN   1
#define PV_VENT_CLOSED 2
void pv_motor_set_mode(int mode);
int  pv_motor_get_mode(void);

// NOT STOCK. What the button's ring LED does.
//
// AUTO is stock exactly: dark in AUTO, blinking in MANUAL. The other four are
// additions. ON_OPEN and OFF_CLOSED are deliberately NOT the same rule: each
// takes over one side of the vent's travel and leaves the other side to the
// stock behaviour, which is what makes them worth having as separate choices.
//
//   AUTO        stock: dark in AUTO mode, blinking in MANUAL mode
//   ALWAYS_ON   lit, always
//   ALWAYS_OFF  dark, always
//   ON_OPEN     lit whenever the vent is open; stock behaviour when closed
//   OFF_CLOSED  dark whenever the vent is closed; stock behaviour when open
#define PV_RING_AUTO       0
#define PV_RING_ALWAYS_ON  1
#define PV_RING_ALWAYS_OFF 2
#define PV_RING_ON_OPEN    3
#define PV_RING_OFF_CLOSED 4
#define PV_RING_COUNT      5
void pv_motor_manual_toggle(void);
void pv_motor_update(void);                         // printer state changed
// Any group latched in fault. Stock keeps four per-group bytes and ORs them
// into one; that byte is what its rgb task reads to raise the red strobe.
bool pv_motor_fault_any(void);

// NOT STOCK. Whether the flap is travelling right now.
//
// The state document reported where the vent HAD got to and never that it was
// still on its way, so the page could only ever draw two states for a thing
// with three. Travel takes a few seconds and is the moment somebody is most
// likely to be looking at the dial wondering whether their tap did anything.
bool pv_motor_moving(void);

// NOT STOCK. An animation, uploaded and held in RAM.
//
// RAM, deliberately. The obvious home for an uploaded animation is flash, and
// this device has no room in flash that is not already spoken for: the
// partition table is BIQU's, both OTA slots are needed for the revert path
// that makes this firmware safe to try, and repartitioning would take that
// away. So the animation lives in the heap and is gone on the next reboot.
// That is a real limitation and it is stated plainly in the UI rather than
// worked around, because the alternative costs the one property that matters
// more.
//
// The device does NOT decode images. The page reads the file with a canvas,
// scales each row to the strip length and uploads raw RGB, which means no
// image decoder on an ESP32 with 80 KB of heap free, and a preview on screen
// that is exactly what will play.
//
// The cap is chosen against the free heap rather than against how long an
// animation anyone might want: at 96 bytes a frame, 16 KB is about 170
// frames, which is five or six seconds. Asking for more than the device can
// give is refused with the numbers, not with a generic failure.
#define PV_ANIM_MAX_BYTES   16384
#define PV_ANIM_PIXELS      (PV_LEDS_PER_STRIP * PV_STRIP_COUNT_MAX)
#define PV_ANIM_MAX_FRAMES  (PV_ANIM_MAX_BYTES / (PV_ANIM_PIXELS * 3))

typedef struct {
    int frames;         // 0 when nothing is loaded
    int pixels;         // per frame, <= PV_ANIM_PIXELS
    int bytes;          // what it is costing
} pv_anim_info_t;

// Replaces whatever was loaded. rgb is frames*pixels*3 bytes, row major,
// pixel 0 first. Returns false if the numbers are out of range or the heap
// cannot give the block; the previous animation is left alone in that case.
bool pv_anim_set(const uint8_t *rgb, int frames, int pixels);
void pv_anim_clear(void);
void pv_anim_info(pv_anim_info_t *out);
// Copies one frame into dst, wrapping the index. Returns the number of pixels
// written, 0 when nothing is loaded. A COPY rather than a pointer: an upload
// frees the buffer, and the renderer must not be reading it when that happens.
int pv_anim_copy(int i, uint8_t *dst, int max_pixels);
// Rewinds the renderer's frame counter, so a fresh upload starts at its first
// row rather than wherever the previous animation had got to.
void pv_rgb_anim_rewind(void);

// NOT STOCK. Re-seat the vent on both endstops and report what the hall
// sensor read at each of them.
//
// There is nothing to "calibrate" in the sense of writing a number: the hall
// bands are fixed by the sensor and the magnet, not by firmware, so they stay
// constants here. What goes wrong in practice is the other half of the
// question. A vent that was unplugged mid-travel, or whose sensor has drifted,
// sits at a reading that is in no band at all, and every symptom of that looks
// like something else: the vent refuses to move, or moves and reports the
// wrong position, or strobes red for no reason anyone can see.
//
// So this drives to each end in turn, reads the millivolts there, and says
// whether each one landed in the band it is supposed to. That re-seats a vent
// left half open as a side effect, which is the part owners actually want, and
// it turns "the vent is being weird" into two numbers.
#define PV_CAL_IDLE     0
#define PV_CAL_RUNNING  1
#define PV_CAL_DONE     2
#define PV_CAL_FAILED   3

typedef struct {
    int  state;             // PV_CAL_*
    int  step;              // 0 closing, 1 opening, 2 restoring
    int  closed_mv;         // what the hall read at the closed end
    int  open_mv;           // and at the open end
    bool closed_ok;         // and whether each was inside its band
    bool open_ok;
    int  at_s;              // uptime when it finished, 0 if it never has
} pv_cal_t;

// Returns false if one is already running, or if this is a no-motor build.
bool pv_motor_calibrate_start(void);
void pv_motor_calibrate_get(pv_cal_t *out);
