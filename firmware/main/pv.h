#pragma once
// PandaVent: a from-scratch re-creation of the BIQU Panda Vent factory
// application. The contract is the FACTORY web UI (served byte-exact from
// this firmware) and the factory WebSocket protocol captured from a running
// stock device; see PROTOCOL.md. Anything the factory app does, this app
// does, before anything new is added.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// ---------------------------------------------------------------------------
// Version. ONE definition, used by the state document, the UI badge and the
// release tag, so those three can never disagree.
//
// PV_FW_VERSION is what the FACTORY protocol's fw_version field carries. It
// stays "V1.0.0" because the factory web app and anything else speaking that
// protocol treat it as the Panda Vent firmware revision, and this project is a
// clone of that firmware: reporting something else there would be lying about
// which protocol revision is on the wire.
//
// PV_OS_VERSION is THIS project's own version, semantic, and is what a user
// reads in the corner of the page and matches against a GitHub release.
#define PV_FW_VERSION  "V1.0.0"
#define PV_OS_NAME     "PandaVent OS"
#define PV_OS_VERSION  "0.1.0"

// ---------------------------------------------------------------------------
// Hardware map (recovered from the stock binary and BTT docs; proven on this
// exact device by the previous firmware generation).
// ---------------------------------------------------------------------------
#define PV_PIN_USER_BUTTON   12   // active-low, internal pull-up
#define PV_PIN_BOOT_BUTTON   0    // active-low; long-press = factory reset
#define PV_PIN_BUTTON_LED    27   // ring LED: off = AUTO, blink = MANUAL
#define PV_PIN_STRIP0        14   // WS2812, 16 px
#define PV_PIN_STRIP1        4    // WS2812, 16 px (both strips always driven)
// The count stock actually drives. It is the 32-bit word at DRAM 0x3ffb0318
// in the shipping image's initialised data (file offset 0x3a714), returned by
// the accessor at 0x400dc934, cached in the byte at 0x3ffb68d1 by the render
// task at 0x400dcaf9, and passed down as every effect function's n. Two
// strips (blti a7,2 in rgb_init 0x400dc4fc, bgei a2,2 in every effect), so
// 32 LEDs driven in total, with a 3*n = 48 byte buffer each (0x400dce3e).
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
#define PV_FX_COUNT       18
#define PV_FX_STOCK_COUNT 7

// Not user selectable and not part of the config arrays. Stock's warning
// override renderer at 0x400ddeac is a separate function, not one of the
// seven: it paints every pixel R=127 G=0 B=0 directly, with no brightness
// scaling, and delays 10 ticks = 100 ms.
#define PV_FX_OVERRIDE_RED 100

// The other two non-selectable renderers the rgb task reaches, both recovered
// 2026-08-28 by enumerating every vTaskDelay in the render region.
//
// PV_FX_FAULT_STROBE is stock's 0x400dd33c: it does not render anything
// itself, it builds a stack record { brightness 100, speed 150, r, g, b } and
// tail calls Strobing at 0x400dd1b0. speed 150 through Strobing's own
// 200 - speed gives a 50 ms half period.
//
// PV_FX_LINK_MARQUEE is stock's 0x400dd840. Same prologue as Static, same
// travelling Gaussian as Marquee (fminf, 5.0f cutoff at 0x400d0cfc, 0.3f
// step at 0x400d0cd8), but it takes no speed and its frame is a fixed
// vTaskDelay(5) = 50 ms at 0x400dda16, and it keeps its OWN position global
// at 0x3ffb6910 rather than sharing Marquee's.
#define PV_FX_FAULT_STROBE 101
#define PV_FX_LINK_MARQUEE 102

// Not a renderer at all. Stock's test mode 1 falls through to 0x400dcab0 when
// the radio self test has no verdict yet: vTaskDelay(50) and NOTHING is
// written, so the previous frame stays on the strip for 500 ms. That is not
// the same as rendering black, so it needs its own id.
#define PV_FX_HOLD         103

// Warning Hot boundary, stated verbatim in the factory app's own copy:
// the printer's maximum temperature crossing 50 C is the burn-risk line.
// Warning Hot boundary, read out of the image at 0x400dc5da: movi.n a9, 50
// then blt/bge. STRICT greater-than, on either temperature, and stock
// compares the strtol'd INTEGER, so 50.9 is 50 and reads safe.
#define PV_WARN_HOT_C     50

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

typedef struct {
    uint8_t brightness;      // 0..100, and the START of the ramp when one is set
    uint8_t speed;           // 0..100
    uint8_t rgb[3];          // ACTIVE while the vent is OPEN
    uint8_t rgb_closed[3];   // ACTIVE while the vent is CLOSED
    uint8_t bg[3];           // INACTIVE while the vent is OPEN
    uint8_t bg_closed[3];    // INACTIVE while the vent is CLOSED
    uint8_t bright_end;      // 0..100, the END of the ramp; only when PV_BRIGHT_END
    uint8_t opt_set;         // PV_BG_* / PV_BRIGHT_END bits; clear means "as before"
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
    bool    reverse;             // is_reverse
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
} pv_rgb_cfg_t;

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
// Material-aware vent policy. AN ADDITION, NOT PART OF THE STOCK CLONE.
//
// Deliberately NOT a member of pv_cfg_t: that blob is a fixed-size struct
// guarded by a magic and a size equality test, so growing it would discard
// every stored setting on the first boot after an update. This lives in its
// own NVS key and defaults cleanly when absent.
// ---------------------------------------------------------------------------
#define PV_MAT_COUNT           9
#define PV_BED_OPEN_C_DEFAULT  45      // DragonVent's BED_OPEN_C_DEFAULT
#define PV_BED_CLOSE_C_DEFAULT 35      // DragonVent's BED_CLOSE_C_DEFAULT

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
// Live (non-persisted) state pushed to the UI.
// ---------------------------------------------------------------------------
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
    // int, not float: stock parses these with strtol base 10 at 0x400d92cd,
    // which stops at the decimal point. Zero at boot, because stock's report
    // array is in .bss and nothing ever clears it.
    int bed_temp;
    int nozzle_temp;
    bool  vent_open;             // current vent target (all groups)
    // The printer's own chamber light, from print.lights_report. Drives the
    // "Follow Printer Light" switch, which the factory app describes as
    // "Automatically turns RGB effect ON and OFF following the printers
    // stock light."
    bool  printer_light;
    // print_error, report key index 1. Stock feeds it to the classifier at
    // 0x400d8fa4; see pv_bambu.c. Zero at boot because stock's report array is
    // .bss and nothing ever clears it.
    int   print_error;
    // Whether the last report's hms array carried the one pair stock looks
    // for at 0x400d900c.
    bool  hms_fault;
    // gcode_state as stock stores it, at report base + 124 = 0x3ffb568c.
    // 0 IDLE, 1 RUNNING, 2 PREPARE, 3 PAUSE, 4 FINISH, 5 FAILED (0x400d9300
    // through 0x400d9379). This is the discriminant of the H2D state machine.
    int   gcode_state;
    // Report keys 4 and 3. Both are consumed, by the stage classifier at
    // 0x400dc300 (0x3ffb5620 and 0x3ffb561c).
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
void pv_cfg_factory_defaults(pv_cfg_t *c);          // whole config
void pv_cfg_rgb_mode_defaults(pv_rgb_cfg_t *r, int mode); // one mode's defaults
void pv_factory_reset_and_reboot(void);

// pv_json.c — full state document, factory shape and order.
char *pv_json_state(void);                          // caller frees
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
void pv_wifi_scan_start(void);
// Whether the last completed scan saw an AP named "test1". Stock's factory
// self-test at 0x400dc9e8 strcmps that name (string at 0x3f4039b8, strcmp is
// ROM 0x40001274) against all 20 scan records at 0x3ffb63f8.
bool pv_wifi_saw_test_ap(void);
// 0 idle, 1 scanning, 2 complete. Mirrors the word stock reads at 0x3ffb63f0
// to pick blue / green / red in test mode 1.
int  pv_wifi_test_scan_state(void);
void pv_wifi_join(const char *ssid, const char *password);
void pv_ap_apply(void);                             // reconfigure softAP now
void pv_hostname_apply(void);                       // mDNS + netif hostname

// pv_bambu.c
void pv_bambu_start(void);
void pv_bambu_rebind(void);                         // apply g_cfg.printer now
void pv_bambu_disconnect(void);
void pv_bambu_scan_start(void);                     // SSDP discovery
// Whether the link layer has run once. Stock's equivalent is the init at
// 0x400d9840, which is what first evaluates the level 3 indicator; until then
// the word keeps the 2 the rgb task armed at 0x400dcabd.
bool pv_bambu_started(void);

// pv_rgb.c
void pv_rgb_start(void);
void pv_rgb_notify(void);                           // config/state changed
// Stops the render task the way stock does: notification value 255 at
// 0x400dcae5, all-off through 0x400ddf98, then the task returns. Used before
// an OTA so the strip goes dark and RMT is released.
void pv_rgb_stop(void);
// Advances stock's test mode, 0x400dc980. Registered as the SHORT click
// handler for GPIO 0 at 0x400de965, so it ships on every unit.
void pv_rgb_test_cycle(void);

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
// Any group latched in fault. Stock keeps four per-group bytes at 0x3ffb6958
// and ORs them into 0x3ffb6954 at 0x400de524; that byte is what the rgb task
// reads through 0x400de550 to raise the red strobe.
bool pv_motor_fault_any(void);
