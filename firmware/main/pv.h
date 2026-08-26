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
// Hardware map (recovered from the stock binary and BTT docs; proven on this
// exact device by the previous firmware generation).
// ---------------------------------------------------------------------------
#define PV_PIN_USER_BUTTON   12   // active-low, internal pull-up
#define PV_PIN_BOOT_BUTTON   0    // active-low; long-press = factory reset
#define PV_PIN_BUTTON_LED    27   // ring LED: off = AUTO, blink = MANUAL
#define PV_PIN_STRIP0        14   // WS2812, 30 px
#define PV_PIN_STRIP1        4    // WS2812, 30 px (2-strip kits)
#define PV_LEDS_PER_STRIP    30
#define PV_STRIP_COUNT_MAX   2
#define PV_MOTOR_GROUPS      4

// ---------------------------------------------------------------------------
// Factory data model. Field names and value ranges mirror the factory
// WebSocket schema one to one (rgb_mode / rgb_switch / printer / ap / sta /
// settings). Brightness and speed are 0..100 in steps of 5, colors are
// "RRGGBB" uppercase hex, exactly as the factory UI sends and expects.
// ---------------------------------------------------------------------------
#define PV_FX_STATIC      0
#define PV_FX_BREATHING   1
#define PV_FX_STROBING    2
#define PV_FX_WAVE        3
#define PV_FX_MARQUEE     4
#define PV_FX_COLOR_CYCLE 5
#define PV_FX_RAINBOW     6
#define PV_FX_COUNT       7

// Warning Hot boundary, stated verbatim in the factory app's own copy:
// the printer's maximum temperature crossing 50 C is the burn-risk line.
#define PV_WARN_HOT_C     50.0f

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

typedef struct {
    uint8_t brightness;      // 0..100
    uint8_t speed;           // 0..100
    char    color[7];        // "RRGGBB"
} pv_fx_param_t;

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
    // H2D (Advance) mode
    uint8_t h2d_active[PV_ST_COUNT];              // active_effect_id per state
    pv_fx_param_t h2d[PV_ST_COUNT][PV_FX_COUNT];
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
} pv_cfg_t;

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
    float bed_temp;
    float nozzle_temp;
    bool  vent_open;             // current vent target (all groups)
    // The printer's own chamber light, from print.lights_report. Drives the
    // "Follow Printer Light" switch, which the factory app describes as
    // "Automatically turns RGB effect ON and OFF following the printers
    // stock light."
    bool  printer_light;
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

// pv_apply.c — inbound WS message dispatch.
void pv_apply_message(const char *json, int len);

// pv_wifi.c
void pv_wifi_start(void);
void pv_wifi_scan_start(void);
void pv_wifi_join(const char *ssid, const char *password);
void pv_ap_apply(void);                             // reconfigure softAP now
void pv_hostname_apply(void);                       // mDNS + netif hostname

// pv_bambu.c
void pv_bambu_start(void);
void pv_bambu_rebind(void);                         // apply g_cfg.printer now
void pv_bambu_disconnect(void);
void pv_bambu_scan_start(void);                     // SSDP discovery

// pv_rgb.c
void pv_rgb_start(void);
void pv_rgb_notify(void);                           // config/state changed

// pv_motor.c
void pv_motor_start(void);
void pv_motor_set_auto(bool auto_mode);
void pv_motor_manual_toggle(void);
void pv_motor_update(void);                         // printer state changed
