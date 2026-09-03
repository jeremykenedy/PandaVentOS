// Config model + persistence. One NVS blob, factory defaults exactly as the
// stock app ships them (colors verified against a live stock device's state
// push and the factory manual's defaults table).
#include "pv.h"

#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pv_cfg";

#define CFG_NS    "pv"
#define CFG_KEY   "cfg"
#define CFG_MAGIC 0x50564344   // "PVCD": the uploaded-animation effect
#define CFG_MAGIC_V12 0x50564343  // "PVCC": temperature gradient, four settings
                               //          that used to be compiled in
#define CFG_MAGIC_V11 0x50564342  // "PVCB": per-strip direction flags
#define CFG_MAGIC_V10 0x50564341  // "PVCA": twenty effects, one spare byte each
#define CFG_MAGIC_V9 0x50564339   // "PVC9": eighteen effects, brightness ramp
#define CFG_MAGIC_V8 0x50564338   // "PVC8": four colours, H2D split out
#define CFG_MAGIC_V7 0x50564337   // "PVC7": one blob, two colours
#define H2D_MAGIC 0x50564843   // "PVHC": twenty-two effects
#define H2D_MAGIC_V12 0x50564842  // "PVHB": twenty-one effects
#define H2D_MAGIC_V11 0x50564841  // "PVHA": twenty effects, one spare byte each
#define H2D_MAGIC_V9 0x50564839   // "PVH9": eighteen effects, brightness ramp
#define H2D_MAGIC_V8 0x50564838   // "PVH8": one H2D state table, no ramp
#define CFG_MAGIC_V6 0x50564336   // "PVC6": ring light, colours as text
#define CFG_MAGIC_V5 0x50564335   // "PVC5": two colours per effect, as text
#define CFG_MAGIC_V4 0x50564334   // "PVC4": eighteen effects, one colour
#define CFG_MAGIC_V3 0x50564333   // "PVC3": nine effects, device_name
#define CFG_MAGIC_V2 0x50564332   // "PVC2": nine effects, no device_name
#define CFG_MAGIC_V1 0x50564331   // "PVC1": the seven effect layout

// Every layout before v5 stored ONE colour per effect, so they all share this
// nine byte parameter. pv_fx_param_t itself is now sixteen bytes, and using it
// in the old shapes would silently mis-read every array in the blob.
typedef struct {
    uint8_t brightness;
    uint8_t speed;
    char    color[7];
} pv_fx_param_v4_t;

// The v1 layout, kept verbatim so a device that has been running since before
// Cylon and Bounce existed keeps its settings.
//
// PV_FX_COUNT is an array dimension in the middle of the config struct, so
// growing it did not append to the blob, it moved everything after
// rgb.simple. A size check alone would have thrown away all 65 settings on
// the first boot after the update, silently, and the only clue would have
// been one line in a serial log nobody was watching.
#define PV_FX_COUNT_V1 7

// The nine effect layout, shared by v2 and v3. Those two differ only by the
// device_name field at the very end, so one rgb shape covers both.
#define PV_FX_COUNT_V3 9

// The eighteen effect layout, shared by v4 through v9. Frozen here for the
// same reason PV_FX_COUNT_V1 is frozen: these structs describe bytes that are
// ALREADY IN FLASH on somebody's device, and a struct that follows the current
// PV_FX_COUNT stops describing them the moment an effect is added. Writing
// PV_FX_COUNT in a v8 struct is not a shorthand, it is a bug waiting for the
// next effect.
#define PV_FX_COUNT_V9 18

// The twenty effect layout, shared by v10 and v11. Frozen for the same reason
// every count above it is frozen: these structs describe bytes that are
// ALREADY IN FLASH.
#define PV_FX_COUNT_V11 20

// The twenty-one effect layout: v12, the one that added the temperature
// gradient.
#define PV_FX_COUNT_V12 21

// The effect parameter shape as it stood for v10 through v12, frozen.
//
// v11's struct used the LIVE pv_fx_param_t, which was a bug of exactly the
// kind the frozen counts above exist to prevent: it happened to be correct
// only because that shape had not changed since. The next field added to
// pv_fx_param_t would have silently redefined what a v11 blob in somebody's
// flash means. Frozen here, with a check below that it still matches today.
typedef struct {
    uint8_t brightness;
    uint8_t speed;
    uint8_t rgb[3];
    uint8_t rgb_closed[3];
    uint8_t bg[3];
    uint8_t bg_closed[3];
    uint8_t bright_end;
    uint8_t aux;
    uint8_t opt_set;
} pv_fx_param_v12_t;

// If this fires, pv_fx_param_t has changed and the migrations below need a
// new frozen shape rather than a wider copy of this one.
_Static_assert(sizeof(pv_fx_param_v12_t) == sizeof(pv_fx_param_t),
               "pv_fx_param_t changed shape: freeze a new legacy struct "
               "instead of letting the old migrations follow it");

typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_v4_t simple[PV_FX_COUNT_V3];
    uint8_t h2d_active[PV_ST_COUNT];
    pv_fx_param_v4_t h2d[PV_ST_COUNT][PV_FX_COUNT_V3];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
} pv_rgb_cfg_v3_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v3_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
} pv_cfg_v3_t;

// The eighteen effect, one colour layout. Same field order as v5; only the
// parameter inside the effect arrays is different.
typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_v4_t simple[PV_FX_COUNT_V9];
    uint8_t h2d_active[PV_ST_COUNT];
    pv_fx_param_v4_t h2d[PV_ST_COUNT][PV_FX_COUNT_V9];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
} pv_rgb_cfg_v4_t;

// v7 stored ONE blob with everything in it, and the effect parameter carried
// two colours as raw bytes. That blob reached 1312 bytes; four colours would
// have taken it past 2100, which does not fit the NVS partition twice over
// during a rewrite. v8 splits the H2D tables into their own keys.
typedef struct {
    uint8_t brightness;
    uint8_t speed;
    uint8_t rgb[3];
    uint8_t rgb_closed[3];
} pv_fx_param_v7_t;

// v8 is the current layout minus `bright_end`: four colours and the flag byte,
// but one brightness. v9 appends the ramp's end brightness and gives the flag
// byte a third bit; a v8 effect migrates with the ramp simply not set, which
// renders identically to what it did.
typedef struct {
    uint8_t brightness;
    uint8_t speed;
    uint8_t rgb[3];
    uint8_t rgb_closed[3];
    uint8_t bg[3];
    uint8_t bg_closed[3];
    uint8_t opt_set;
} pv_fx_param_v8_t;

// v11 is the current layout minus the four settings that used to be compiled
// in, and one effect short. The effect parameters themselves are unchanged, so
// the only thing new about an H2D table is that it has one more entry in it.
typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t reverse_strips;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_v12_t simple[PV_FX_COUNT_V11];
    uint8_t h2d_active[PV_ST_COUNT];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
} pv_rgb_cfg_v11_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v11_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
    uint8_t ring_mode;
    bool    ring_blink;
    uint8_t leds[PV_STRIP_COUNT_MAX];
} pv_cfg_v11_t;

typedef struct {
    uint32_t magic;
    pv_fx_param_v12_t fx[PV_FX_COUNT_V11];
} pv_h2d_blob_v11_t;

// v12 is the current layout minus the one effect that plays an uploaded
// animation. Nothing else about it changed: the four settings that arrived
// with v12 are all still here, in the same order, so this migration IS an
// append plus a wider effect array.
typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t reverse_strips;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_v12_t simple[PV_FX_COUNT_V12];
    uint8_t h2d_active[PV_ST_COUNT];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
    uint8_t warn_hot_c;
    uint8_t err_rgb[3];
    uint8_t err_bright;
    bool    err_strobe;
    bool    err_set;
    bool    contiguous;
    uint8_t grad_min_c;
    uint8_t grad_max_c;
} pv_rgb_cfg_v12_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v12_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
    uint8_t ring_mode;
    bool    ring_blink;
    uint8_t leds[PV_STRIP_COUNT_MAX];
} pv_cfg_v12_t;

typedef struct {
    uint32_t magic;
    pv_fx_param_v12_t fx[PV_FX_COUNT_V12];
} pv_h2d_blob_v12_t;

// NOTE: this is the SAME SIZE as pv_cfg_t. The bools before the new byte left
// a padding hole and it landed in one, so the stored length cannot tell v10
// from v11 and the magic is doing all the work. That is fine, and it is why
// every branch below checks the magic FIRST and treats the size as a
// corroboration rather than as the test.
//
// v10 is the current layout minus the per-strip direction flags. Only
// pv_rgb_cfg_t changes, so the H2D tables are untouched and keep their magic:
// the effect parameters are the same bytes they were, and the new per-effect
// direction lives in a bit of the flag byte those bytes already carry.
typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t light_mode;
    uint8_t simple_current;
    // The FROZEN count, not the live one. This struct describes bytes that are
    // already in somebody's flash, and following PV_FX_COUNT made it grow with
    // the twenty-first effect and stop describing them: v10 measured LARGER
    // than v11, which is impossible and which migcheck caught immediately.
    pv_fx_param_t simple[PV_FX_COUNT_V11];
    uint8_t h2d_active[PV_ST_COUNT];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
} pv_rgb_cfg_v10_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v10_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
    uint8_t ring_mode;
    bool    ring_blink;
    uint8_t leds[PV_STRIP_COUNT_MAX];
} pv_cfg_v10_t;

// v9 is the current layout minus the spare per-effect byte, and two effects
// short. v10 appends `aux` to each effect and gives the flag byte a fourth
// bit; a v9 effect migrates with aux simply not set, which renders identically
// to what it did, and the two new effects arrive at their defaults.
typedef struct {
    uint8_t brightness;
    uint8_t speed;
    uint8_t rgb[3];
    uint8_t rgb_closed[3];
    uint8_t bg[3];
    uint8_t bg_closed[3];
    uint8_t bright_end;
    uint8_t opt_set;
} pv_fx_param_v9_t;

typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_v9_t simple[PV_FX_COUNT_V9];
    uint8_t h2d_active[PV_ST_COUNT];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
} pv_rgb_cfg_v9_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v9_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
    uint8_t ring_mode;
    bool    ring_blink;
    uint8_t leds[PV_STRIP_COUNT_MAX];
} pv_cfg_v9_t;

typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_v8_t simple[PV_FX_COUNT_V9];
    uint8_t h2d_active[PV_ST_COUNT];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
} pv_rgb_cfg_v8_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v8_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
    uint8_t ring_mode;
    bool    ring_blink;
    uint8_t leds[PV_STRIP_COUNT_MAX];
} pv_cfg_v8_t;

typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_v7_t simple[PV_FX_COUNT_V9];
    uint8_t h2d_active[PV_ST_COUNT];
    pv_fx_param_v7_t h2d[PV_ST_COUNT][PV_FX_COUNT_V9];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
} pv_rgb_cfg_v7_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v7_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
    uint8_t ring_mode;
    bool    ring_blink;
} pv_cfg_v7_t;

// v5 and v6 stored colours as seven byte text, which is what made the config
// too big for the NVS partition. They share one parameter and one rgb shape;
// only the two trailing ring fields tell them apart.
typedef struct {
    uint8_t brightness;
    uint8_t speed;
    char    color[7];
    char    color_closed[7];
} pv_fx_param_v6_t;

typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_v6_t simple[PV_FX_COUNT_V9];
    uint8_t h2d_active[PV_ST_COUNT];
    pv_fx_param_v6_t h2d[PV_ST_COUNT][PV_FX_COUNT_V9];
    uint8_t warnhot_current[2];
    uint8_t warnhot_bg[2][2];
    uint8_t warnhot_speed[2][2];
} pv_rgb_cfg_v6_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v6_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
} pv_cfg_v5_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v6_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
    uint8_t ring_mode;
    bool    ring_blink;
} pv_cfg_v6_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v4_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
    char device_name[32];
} pv_cfg_v4_t;

typedef struct {
    uint32_t magic;
    pv_rgb_cfg_v3_t rgb;
    pv_printer_cfg_t printer;
    pv_ap_cfg_t ap;
    char hostname[32];
    char language[6];
    bool motor_manual;
    bool motor_manual_open;
} pv_cfg_v2_t;

typedef struct {
    bool    light_on;
    bool    warning_sw;
    bool    follow_printer;
    bool    follow_vent;
    bool    reverse;
    uint8_t light_mode;
    uint8_t simple_current;
    pv_fx_param_v4_t simple[PV_FX_COUNT_V1];
    uint8_t h2d_active[PV_ST_COUNT];
    pv_fx_param_v4_t h2d[PV_ST_COUNT][PV_FX_COUNT_V1];
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

// ---------------------------------------------------------------------------
// THE SIZE BUDGET. Read this before adding a field.
//
// The NVS partition is 0x3000 = 12 KB, three 4 KB pages, and it is stock's
// layout: enlarging it would break OTA back to the factory firmware, which is
// the one thing this project must never give up. NVS keeps one page in
// reserve for garbage collection, so about 8 KB is usable, and rewriting a
// blob needs room for the OLD and the NEW copy at once. Sharing that space
// are the Wi-Fi credentials esp_wifi stores, the vent-policy blob, and this.
//
// On 2026-08-30 this struct reached 2320 bytes. Two copies of it plus the
// Wi-Fi credentials no longer fit, nvs_set_blob started returning
// ESP_ERR_NVS_NOT_ENOUGH_SPACE, the failure was logged and otherwise ignored,
// and the device ran perfectly from RAM until the next reboot threw every
// setting away. That is the bug this budget exists to prevent.
//
// 1600 bytes leaves room for two copies plus everything else with margin. If
// this fails to compile, do NOT raise the number: make the config smaller, or
// split it across separate NVS keys so a rewrite only relocates what changed.
#define PV_CFG_MAX_BYTES 1600
_Static_assert(sizeof(pv_cfg_t) <= PV_CFG_MAX_BYTES,
               "pv_cfg_t has outgrown the NVS budget; see PV_CFG_MAX_BYTES");

pv_cfg_t  g_cfg;
pv_fx_param_t g_h2d[PV_ST_COUNT][PV_FX_COUNT];

// One NVS key per device state: "h2d0".."h2d5".
typedef struct {
    uint32_t magic;
    pv_fx_param_t fx[PV_FX_COUNT];
} pv_h2d_blob_t;

// The same table as stored before the brightness ramp existed. Read only, so a
// device that has been through v8 keeps its H2D colours across the update.
typedef struct {
    uint32_t magic;
    pv_fx_param_v9_t fx[PV_FX_COUNT_V9];
} pv_h2d_blob_v9_t;

typedef struct {
    uint32_t magic;
    pv_fx_param_v8_t fx[PV_FX_COUNT_V9];
} pv_h2d_blob_v8_t;

pv_live_t g_live = {
    .sta_state = 1, .printer_state = 0, .device_state = PV_ST_IDLE,
    .bed_temp = -1.0f, .nozzle_temp = -1.0f,
    // NOT STOCK. Nothing has been reported yet, and "not reported" is not
    // zero. Everything the Status page shows starts unknown and stays that
    // way until the printer actually says otherwise.
    .chamber_temp = -1, .fan_part = -1, .fan_aux = -1, .fan_chamber = -1,
    .layer_total = -1, .remain_min = -1, .spd_lvl = -1, .work_light = -1,
    .filament_in = -1, .spd_mag = -1, .ams_humidity = -1, .ams_humidity_pct = -1,
    .ams_temp = -1, .door_open = -1, .fw_update = -1, .tray_now = -1,
    .cam_present = -1, .cam_record = -1, .cam_timelapse = -1,
    .cam_free_mb = -1, .cam_total_mb = -1,
    .tray = { { .remain = -1 }, { .remain = -1 }, { .remain = -1 },
              { .remain = -1 }, { .remain = -1 } },
};

// Every layout before v5 carried one colour per effect. Lifting it means
// giving the closed position the same colour the effect already had, so a
// migrated device looks exactly as it did before the update.
// v5 and v6 stored both colours, as text. Only the representation changes.
// v7 already stored both colours as bytes; only the two inactive ones and the
// flag byte are new, and they start cleared so nothing looks different.
// v8 kept all four colours and the flag byte; only the ramp is new, and it
// starts unset, so bright_end is parked on the one brightness the effect had.
// v9 kept everything v10 has except the spare byte, and had two fewer effects.
// aux starts unset, so every effect that reads it falls back to the same
// default it would have used before the byte existed.
static void fx_lift_v9(pv_fx_param_t *dst, const pv_fx_param_v9_t *src)
{
    dst->brightness = src->brightness;
    dst->speed = src->speed;
    memcpy(dst->rgb, src->rgb, 3);
    memcpy(dst->rgb_closed, src->rgb_closed, 3);
    memcpy(dst->bg, src->bg, 3);
    memcpy(dst->bg_closed, src->bg_closed, 3);
    dst->bright_end = src->bright_end;
    dst->aux = 0;
    dst->opt_set = (uint8_t)(src->opt_set &
                             (PV_BG_OPEN | PV_BG_CLOSED | PV_BRIGHT_END));
}

static void fx_lift_v8(pv_fx_param_t *dst, const pv_fx_param_v8_t *src)
{
    dst->brightness = src->brightness;
    dst->speed = src->speed;
    memcpy(dst->rgb, src->rgb, 3);
    memcpy(dst->rgb_closed, src->rgb_closed, 3);
    memcpy(dst->bg, src->bg, 3);
    memcpy(dst->bg_closed, src->bg_closed, 3);
    dst->bright_end = src->brightness;
    dst->aux = 0;
    dst->opt_set = (uint8_t)(src->opt_set & (PV_BG_OPEN | PV_BG_CLOSED));
}

static void fx_lift_v7(pv_fx_param_t *dst, const pv_fx_param_v7_t *src)
{
    dst->brightness = src->brightness;
    dst->speed = src->speed;
    memcpy(dst->rgb, src->rgb, 3);
    memcpy(dst->rgb_closed, src->rgb_closed, 3);
    dst->bg[0] = dst->bg[1] = dst->bg[2] = 0;
    dst->bg_closed[0] = dst->bg_closed[1] = dst->bg_closed[2] = 0;
    // No ramp: one brightness, exactly as this effect always ran.
    dst->bright_end = src->brightness;
    dst->aux = 0;
    dst->opt_set = 0;
}

static void fx_lift_v6(pv_fx_param_t *dst, const pv_fx_param_v6_t *src)
{
    dst->brightness = src->brightness;
    dst->speed = src->speed;
    pv_hex_to_rgb3(src->color, dst->rgb);
    pv_hex_to_rgb3(src->color_closed, dst->rgb_closed);
    dst->bg[0] = dst->bg[1] = dst->bg[2] = 0;
    dst->bg_closed[0] = dst->bg_closed[1] = dst->bg_closed[2] = 0;
    // No ramp: one brightness, exactly as this effect always ran.
    dst->bright_end = src->brightness;
    dst->aux = 0;
    dst->opt_set = 0;
}

static void fx_lift_v4(pv_fx_param_t *dst, const pv_fx_param_v4_t *src)
{
    dst->brightness = src->brightness;
    dst->speed = src->speed;
    pv_hex_to_rgb3(src->color, dst->rgb);
    pv_hex_to_rgb3(src->color, dst->rgb_closed);
    dst->bg[0] = dst->bg[1] = dst->bg[2] = 0;
    dst->bg_closed[0] = dst->bg_closed[1] = dst->bg_closed[2] = 0;
    // No ramp: one brightness, exactly as this effect always ran.
    dst->bright_end = src->brightness;
    dst->aux = 0;
    dst->opt_set = 0;
}

static uint8_t hexnib(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}

// "RRGGBB" (the wire format) to three stored bytes. Anything that is not six
// hex digits yields black rather than garbage, because a malformed colour from
// the network must not be able to put an undefined value on the strip.
void pv_hex_to_rgb3(const char *hex, uint8_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!hex) return;
    size_t n = strlen(hex);
    if (n != 6) return;
    for (int i = 0; i < 6; ++i)
        if (!isxdigit((unsigned char)hex[i])) return;
    out[0] = (uint8_t)((hexnib(hex[0]) << 4) | hexnib(hex[1]));
    out[1] = (uint8_t)((hexnib(hex[2]) << 4) | hexnib(hex[3]));
    out[2] = (uint8_t)((hexnib(hex[4]) << 4) | hexnib(hex[5]));
}

// Three stored bytes back to the "RRGGBB" the factory protocol expects.
void pv_rgb3_to_hex(const uint8_t rgb[3], char out[7])
{
    static const char H[] = "0123456789ABCDEF";
    out[0] = H[(rgb[0] >> 4) & 15]; out[1] = H[rgb[0] & 15];
    out[2] = H[(rgb[1] >> 4) & 15]; out[3] = H[rgb[1] & 15];
    out[4] = H[(rgb[2] >> 4) & 15]; out[5] = H[rgb[2] & 15];
    out[6] = '\0';
}

static void fx_set(pv_fx_param_t *p, const char *color)
{
    p->brightness = 50;
    p->speed = 50;
    pv_hex_to_rgb3(color, p->rgb);
    // Factory defaults give both vent positions the same colour, so a device
    // out of the box behaves exactly as it did before the second colour
    // existed. Anyone who wants the strip to change on the vent sets it.
    pv_hex_to_rgb3(color, p->rgb_closed);
    // Both INACTIVE colours start unset, which means the unlit pixels stay
    // black: exactly what every effect did before they existed.
    p->bg[0] = p->bg[1] = p->bg[2] = 0;
    p->bg_closed[0] = p->bg_closed[1] = p->bg_closed[2] = 0;
    // The ramp starts unset too, so an effect runs at one brightness the way
    // it always did until someone asks for a second one.
    p->bright_end = p->brightness;
    p->opt_set = 0;
}

void pv_cfg_rgb_mode_defaults(pv_rgb_cfg_t *r, int mode)
{
    if (mode == PV_MODE_SIMPLE) {
        r->simple_current = PV_FX_STATIC;
        for (int i = 0; i < PV_FX_COUNT; ++i) fx_set(&r->simple[i], "FF3700");
        // NOT STOCK. The temperature gradient runs from its INACTIVE colour to
        // its ACTIVE one, so it is the one effect whose inactive colour has to
        // start SET: unset means dark, and a gradient from black to orange
        // reads as a dimmer rather than as a temperature. Blue cold, orange
        // hot, which is the one colour pair everybody already reads that way.
        {
            pv_fx_param_t *g = &r->simple[PV_FX_TEMP_GRADIENT];
            pv_hex_to_rgb3("FF6A00", g->rgb);
            pv_hex_to_rgb3("FF6A00", g->rgb_closed);
            pv_hex_to_rgb3("0060FF", g->bg);
            pv_hex_to_rgb3("0060FF", g->bg_closed);
            g->opt_set |= PV_BG_OPEN | PV_BG_CLOSED;
        }
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
        // 4 and 5 only.
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
                fx_set(&g_h2d[st][f], "FFFFFF");
            fx_set(&g_h2d[st][state_fx[st]], active_color[st]);
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

// WARNING: this also rewrites the GLOBAL g_h2d, whatever struct you pass it.
//
// The per-state tables do not live inside pv_cfg_t (they are stored under
// their own NVS keys, for the size reasons above), so there is nowhere else
// for their defaults to be written. That makes calling this on a scratch
// struct, to find out what a factory value looks like, destructive: it wipes
// the tables the caller was reading. That has already caught a test out once,
// which is why it is written here rather than left to be rediscovered.
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
    // Stock's ring behaviour: dark in AUTO, blinking in MANUAL.
    c->ring_mode = PV_RING_AUTO;
    c->ring_blink = true;
    // 16 per strip is what stock drives, so this default reproduces stock.
    for (int i = 0; i < PV_STRIP_COUNT_MAX; ++i) c->leds[i] = PV_LEDS_PER_STRIP;
}

// DELIBERATE DEPARTURE, documented rather than silent.
//
// Stock does NOT validate. Its store is one nvs_get_blob per subsystem
// through a single wrapper (namespace "app_nvs", keys "sign", "wifi_info",
// "bambu_mqtt_info", "ui_info", "sys_rgb_mode" 0x138 bytes, "rgb_sundry" 6,
// "key_mode" 4), and its loader issues the three reads and returns 1 with no
// magic, no version and no range check on any field.
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
    if (g_cfg.ring_mode >= PV_RING_COUNT) g_cfg.ring_mode = PV_RING_AUTO;
    // A zero here would divide by zero in the effects; anything above the
    // buffer would read past it.
    for (int i = 0; i < PV_STRIP_COUNT_MAX; ++i)
        if (g_cfg.leds[i] < 1 || g_cfg.leds[i] > PV_LEDS_PER_STRIP)
            g_cfg.leds[i] = PV_LEDS_PER_STRIP;
}

// The H2D tables, one NVS key per device state.
//
// Splitting them out is the whole point: at 18 effects and four colours the
// six tables are 1620 bytes. NVS writes the new copy of a blob before
// releasing the old one, so a single 1620 byte blob needs 3240 bytes free to
// rewrite, in a partition that has about 4 KB free after the Wi-Fi stack has
// taken its share. Per state, a rewrite moves 274 bytes and touches nothing
// else.
static void h2d_key(int st, char out[8])
{
    snprintf(out, 8, "h2d%d", st);
}

// WHICH save failed, not merely THAT one did.
//
// The warning banner is "settings are live but will be lost when it restarts",
// which is a serious thing to tell someone, so it has to be true for as long
// as it is shown and no longer. There are seven independent blobs: the config
// and one per device state. Each used to write into the same single boolean,
// and only the config path ever cleared it, so one transient failure while the
// six per-state tables were being rewritten during a migration left the banner
// up for the life of the device. Everything was in fact stored; the page just
// had no way to learn that.
//
// One bit per blob now. The banner is shown while ANY bit is set and withdrawn
// the moment the last one clears, and the page is told on every change rather
// than only on the way in.
#define SAVE_CFG    0x01
#define SAVE_H2D0   0x02        /* .. shifted left by the state index */
static uint16_t s_save_failed;

static void save_failed_set(uint16_t bit, bool failed)
{
    uint16_t was = s_save_failed;
    if (failed) s_save_failed |= bit;
    else        s_save_failed &= (uint16_t)~bit;
    bool now_any = (s_save_failed != 0);
    if ((was != 0) == now_any) return;          // nothing the page can see changed
    g_live.cfg_save_failed = now_any;
    if (now_any)
        ESP_LOGE(TAG, "SAVE FAILED: settings are live but NOT stored "
                      "and will be lost on reboot");
    else
        ESP_LOGW(TAG, "saves are working again; settings are stored");
    pv_ws_push_state();
}

static void h2d_save_state(int st)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[8]; h2d_key(st, key);
    pv_h2d_blob_t b;
    b.magic = H2D_MAGIC;
    memcpy(b.fx, g_h2d[st], sizeof(b.fx));
    esp_err_t err = nvs_set_blob(h, key, &b, sizeof(b));
    if (err != ESP_OK) {
        // Same recovery as the main config: release the old copy so the new
        // one only needs room for itself.
        if (nvs_erase_key(h, key) == ESP_OK) {
            nvs_commit(h);
            err = nvs_set_blob(h, key, &b, sizeof(b));
        }
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    save_failed_set(SAVE_H2D0 << st, err != ESP_OK);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "h2d[%d] save failed: %d", st, err);
}

void pv_cfg_h2d_save(int st)
{
    if (st < 0 || st >= PV_ST_COUNT) return;
    h2d_save_state(st);
}

static void h2d_save_all(void)
{
    for (int st = 0; st < PV_ST_COUNT; ++st) h2d_save_state(st);
}

// Returns true when every state was read back. A partial read leaves the
// missing states on the factory defaults already in g_h2d.
static bool h2d_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool all = true;
    bool migrated = false;
    for (int st = 0; st < PV_ST_COUNT; ++st) {
        char key[8]; h2d_key(st, key);
        // Big enough for either shape; the stored length and magic say which.
        union { pv_h2d_blob_t v13; pv_h2d_blob_v12_t v12; pv_h2d_blob_v11_t v11;
                pv_h2d_blob_v9_t v9; pv_h2d_blob_v8_t v8; } b;
        size_t len = sizeof(b);
        esp_err_t e = nvs_get_blob(h, key, &b, &len);
        if (e == ESP_OK && len == sizeof(pv_h2d_blob_t) && b.v13.magic == H2D_MAGIC) {
            memcpy(g_h2d[st], b.v13.fx, sizeof(g_h2d[st]));
        } else if (e == ESP_OK && len == sizeof(pv_h2d_blob_v12_t) &&
                   b.v12.magic == H2D_MAGIC_V12) {
            // v12 -> v13, per state. The twenty-one that existed keep every
            // byte; the animation effect keeps the factory default already in
            // g_h2d, which is why this stops at twenty-one rather than
            // clearing first.
            for (int f = 0; f < PV_FX_COUNT_V12; ++f)
                memcpy(&g_h2d[st][f], &b.v12.fx[f], sizeof(pv_fx_param_v12_t));
            ESP_LOGW(TAG, "migrated h2d[%d] v12 -> v13 (%u B -> %u B)",
                     st, (unsigned)len, (unsigned)sizeof(pv_h2d_blob_t));
            migrated = true;
        } else if (e == ESP_OK && len == sizeof(pv_h2d_blob_v11_t) &&
                   b.v11.magic == H2D_MAGIC_V11) {
            // v11 -> v13, per state. The twenty that existed keep every byte;
            // the two added since keep the factory defaults already in g_h2d,
            // which is why this stops at twenty rather than clearing first.
            for (int f = 0; f < PV_FX_COUNT_V11; ++f)
                memcpy(&g_h2d[st][f], &b.v11.fx[f], sizeof(pv_fx_param_v12_t));
            ESP_LOGW(TAG, "migrated h2d[%d] v11 -> v13 (%u B -> %u B)",
                     st, (unsigned)len, (unsigned)sizeof(pv_h2d_blob_t));
            migrated = true;
        } else if (e == ESP_OK && len == sizeof(pv_h2d_blob_v9_t) &&
                   b.v9.magic == H2D_MAGIC_V9) {
            // v9 -> v10, per effect. The eighteen that existed keep every
            // colour and setting they had; the two new ones keep the factory
            // defaults already sitting in g_h2d, which is why this loop stops
            // at eighteen rather than clearing the table first.
            for (int f = 0; f < PV_FX_COUNT_V9; ++f)
                fx_lift_v9(&g_h2d[st][f], &b.v9.fx[f]);
            ESP_LOGW(TAG, "migrated h2d[%d] v9 -> v10 (%u B -> %u B)",
                     st, (unsigned)len, (unsigned)sizeof(pv_h2d_blob_t));
            migrated = true;
        } else if (e == ESP_OK && len == sizeof(pv_h2d_blob_v8_t) &&
                   b.v8.magic == H2D_MAGIC_V8) {
            // v8 -> v10, per effect. The ramp and the spare byte both come out
            // unset, so this state's strip looks exactly as it did.
            for (int f = 0; f < PV_FX_COUNT_V9; ++f)
                fx_lift_v8(&g_h2d[st][f], &b.v8.fx[f]);
            ESP_LOGW(TAG, "migrated h2d[%d] v8 -> v10 (%u B -> %u B)",
                     st, (unsigned)len, (unsigned)sizeof(pv_h2d_blob_t));
            migrated = true;
        } else {
            all = false;
        }
    }
    nvs_close(h);
    // Write the lifted tables back in the new shape, or the migration runs
    // again on every boot and a save of one state would sit next to five in
    // the old layout.
    if (migrated) h2d_save_all();
    return all;
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
    union { pv_cfg_t v13; pv_cfg_v12_t v12; pv_cfg_v11_t v11; pv_cfg_v10_t v10; pv_cfg_v9_t v9; pv_cfg_v8_t v8; pv_cfg_v7_t v7; pv_cfg_v6_t v6; pv_cfg_v5_t v5; pv_cfg_v4_t v4; pv_cfg_v3_t v3; pv_cfg_v2_t v2;
            pv_cfg_v1_t v1; } stored;
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_blob(h, CFG_KEY, &stored, &size);
    nvs_close(h);

    if (err == ESP_OK && size == sizeof(pv_cfg_t) &&
        stored.v13.magic == CFG_MAGIC) {
        g_cfg = stored.v13;
        if (!h2d_load_all())
            ESP_LOGW(TAG, "some H2D tables missing; defaults kept for those");
        cfg_clamp_loaded();
        ESP_LOGI(TAG, "config loaded (%u B)", (unsigned)size);
        return;
    }

    // v12 -> v13. One more effect, the one that plays an uploaded animation.
    //
    // The effect count moved, so this is not an append: rgb.simple changes
    // shape and everything after it moves. Nothing else changed, which is why
    // every field below is a straight copy and the four settings that arrived
    // with v12 come across as they were rather than being reset.
    if (err == ESP_OK && stored.v12.magic == CFG_MAGIC_V12 &&
        size == sizeof(pv_cfg_v12_t)) {
        const pv_rgb_cfg_v12_t *o = &stored.v12.rgb;
        pv_cfg_factory_defaults(&g_cfg);      // the new effect starts here
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.reverse_strips = o->reverse_strips;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT_V12; ++i)
            memcpy(&g_cfg.rgb.simple[i], &o->simple[i], sizeof(pv_fx_param_v12_t));
        memcpy(g_cfg.rgb.h2d_active, o->h2d_active, sizeof(g_cfg.rgb.h2d_active));
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));
        // The four that arrived with v12 come across exactly as stored: a
        // migration that quietly reset somebody's chosen warning temperature
        // because a new effect was added would be a bug, not a migration.
        g_cfg.rgb.warn_hot_c  = o->warn_hot_c;
        memcpy(g_cfg.rgb.err_rgb, o->err_rgb, sizeof(g_cfg.rgb.err_rgb));
        g_cfg.rgb.err_bright  = o->err_bright;
        g_cfg.rgb.err_strobe  = o->err_strobe;
        g_cfg.rgb.err_set     = o->err_set;
        g_cfg.rgb.contiguous  = o->contiguous;
        g_cfg.rgb.grad_min_c  = o->grad_min_c;
        g_cfg.rgb.grad_max_c  = o->grad_max_c;
        g_cfg.printer = stored.v12.printer;
        g_cfg.ap      = stored.v12.ap;
        memcpy(g_cfg.hostname, stored.v12.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v12.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v12.motor_manual;
        g_cfg.motor_manual_open = stored.v12.motor_manual_open;
        memcpy(g_cfg.device_name, stored.v12.device_name, sizeof(g_cfg.device_name));
        g_cfg.ring_mode  = stored.v12.ring_mode;
        g_cfg.ring_blink = stored.v12.ring_blink;
        memcpy(g_cfg.leds, stored.v12.leds, sizeof(g_cfg.leds));
        g_cfg.magic = CFG_MAGIC;
        if (!h2d_load_all())
            ESP_LOGW(TAG, "some H2D tables missing; defaults kept for those");
        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v12 -> v13 (%u B -> %u B), %d effects -> %d",
                 (unsigned)size, (unsigned)sizeof(pv_cfg_t),
                 PV_FX_COUNT_V12, PV_FX_COUNT);
        pv_cfg_save();
        h2d_save_all();
        return;
    }

    // v11 -> v13. One more effect, and four numbers that used to be compiled
    // in: the temperature the warning mode calls hot, what a fault looks like,
    // whether the two runs are one, and the two ends of the gradient. Every
    // one of them arrives as zero, which each reader takes to mean "use the
    // number that was compiled in", so a migrated device behaves exactly as it
    // did before the settings existed.
    //
    // The effect count moved, so this is not an append: rgb.simple changes
    // shape and everything after it moves.
    if (err == ESP_OK && stored.v11.magic == CFG_MAGIC_V11 &&
        size == sizeof(pv_cfg_v11_t)) {
        const pv_rgb_cfg_v11_t *o = &stored.v11.rgb;
        pv_cfg_factory_defaults(&g_cfg);      // the new effect starts here
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.reverse_strips = o->reverse_strips;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT_V11; ++i)
            memcpy(&g_cfg.rgb.simple[i], &o->simple[i], sizeof(pv_fx_param_v12_t));
        memcpy(g_cfg.rgb.h2d_active, o->h2d_active, sizeof(g_cfg.rgb.h2d_active));
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));
        // Never set, and never set is what was compiled in.
        g_cfg.rgb.warn_hot_c = 0;
        g_cfg.rgb.err_set = false;
        g_cfg.rgb.contiguous = false;
        g_cfg.rgb.grad_min_c = 0;
        g_cfg.rgb.grad_max_c = 0;

        g_cfg.printer           = stored.v11.printer;
        g_cfg.ap                = stored.v11.ap;
        memcpy(g_cfg.hostname, stored.v11.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v11.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v11.motor_manual;
        g_cfg.motor_manual_open = stored.v11.motor_manual_open;
        memcpy(g_cfg.device_name, stored.v11.device_name, sizeof(g_cfg.device_name));
        g_cfg.ring_mode         = stored.v11.ring_mode;
        g_cfg.ring_blink        = stored.v11.ring_blink;
        memcpy(g_cfg.leds, stored.v11.leds, sizeof(g_cfg.leds));
        g_cfg.magic             = CFG_MAGIC;

        if (!h2d_load_all())
            ESP_LOGW(TAG, "some H2D tables missing; defaults kept for those");
        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v11 -> v12 (%u B -> %u B), %d effects -> %d",
                 (unsigned)size, (unsigned)sizeof(pv_cfg_t),
                 PV_FX_COUNT_V11, PV_FX_COUNT);
        pv_cfg_save();
        h2d_save_all();
        return;
    }

    // v10 -> v11. One byte: the per-strip direction flags, which start clear so
    // every strip follows the master flip exactly as it did. The effect
    // parameters are byte-identical, so the H2D tables are not touched at all.
    if (err == ESP_OK && stored.v10.magic == CFG_MAGIC_V10 &&
        size == sizeof(pv_cfg_v10_t)) {
        const pv_rgb_cfg_v10_t *o = &stored.v10.rgb;
        // The new effect and the four new settings start from the factory,
        // then every stored field is written over the top of them.
        pv_cfg_factory_defaults(&g_cfg);
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.reverse_strips = 0;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT_V11; ++i)
            memcpy(&g_cfg.rgb.simple[i], &o->simple[i], sizeof(pv_fx_param_v12_t));
        memcpy(g_cfg.rgb.h2d_active, o->h2d_active, sizeof(g_cfg.rgb.h2d_active));
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));

        g_cfg.printer           = stored.v10.printer;
        g_cfg.ap                = stored.v10.ap;
        memcpy(g_cfg.hostname, stored.v10.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v10.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v10.motor_manual;
        g_cfg.motor_manual_open = stored.v10.motor_manual_open;
        memcpy(g_cfg.device_name, stored.v10.device_name, sizeof(g_cfg.device_name));
        g_cfg.ring_mode         = stored.v10.ring_mode;
        g_cfg.ring_blink        = stored.v10.ring_blink;
        memcpy(g_cfg.leds, stored.v10.leds, sizeof(g_cfg.leds));
        g_cfg.magic             = CFG_MAGIC;

        if (!h2d_load_all())
            ESP_LOGW(TAG, "some H2D tables missing; defaults kept for those");
        h2d_save_all();          // the tables grew an entry, so rewrite them
        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v10 -> v12 (%u B -> %u B), per-strip direction added clear",
                 (unsigned)size, (unsigned)sizeof(pv_cfg_t));
        pv_cfg_save();
        return;
    }

    // v9 -> v10. Two more effects and one spare byte per effect. Both are
    // additions at the end of each effect, but PV_FX_COUNT is an array
    // dimension in the middle of the config, so this is not an append: the
    // whole of rgb.simple changes shape and everything after it moves. A size
    // check alone would have thrown away every setting on the first boot after
    // the update, which is the exact fault the v1 -> v4 comment below records.
    //
    // The two new effects arrive at their defaults; every existing one comes
    // out with aux unset, which renders exactly as v9 did.
    if (err == ESP_OK && stored.v9.magic == CFG_MAGIC_V9 &&
        size == sizeof(pv_cfg_v9_t)) {
        const pv_rgb_cfg_v9_t *o = &stored.v9.rgb;
        pv_cfg_factory_defaults(&g_cfg);    // the two new effects start here
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT_V9; ++i)
            fx_lift_v9(&g_cfg.rgb.simple[i], &o->simple[i]);
        memcpy(g_cfg.rgb.h2d_active, o->h2d_active, sizeof(g_cfg.rgb.h2d_active));
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));

        g_cfg.printer           = stored.v9.printer;
        g_cfg.ap                = stored.v9.ap;
        memcpy(g_cfg.hostname, stored.v9.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v9.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v9.motor_manual;
        g_cfg.motor_manual_open = stored.v9.motor_manual_open;
        memcpy(g_cfg.device_name, stored.v9.device_name, sizeof(g_cfg.device_name));
        g_cfg.ring_mode         = stored.v9.ring_mode;
        g_cfg.ring_blink        = stored.v9.ring_blink;
        memcpy(g_cfg.leds, stored.v9.leds, sizeof(g_cfg.leds));
        g_cfg.magic             = CFG_MAGIC;

        if (!h2d_load_all())
            ESP_LOGW(TAG, "some H2D tables missing; defaults kept for those");
        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v9 -> v10 (%u B -> %u B), %d effects -> %d",
                 (unsigned)size, (unsigned)sizeof(pv_cfg_t),
                 PV_FX_COUNT_V9, PV_FX_COUNT);
        pv_cfg_save();
        h2d_save_all();
        return;
    }

    // v8 -> v9. One field: the ramp's end brightness, plus a third bit in the
    // flag byte. The H2D tables are already in their own keys and get the same
    // treatment in h2d_load_all, so this only has to lift the simple table and
    // copy everything else across unchanged. Every effect comes out with the
    // ramp unset, which renders exactly as v8 did.
    if (err == ESP_OK && stored.v8.magic == CFG_MAGIC_V8 &&
        size == sizeof(pv_cfg_v8_t)) {
        const pv_rgb_cfg_v8_t *o = &stored.v8.rgb;
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        pv_cfg_factory_defaults(&g_cfg);    // the two new effects start here
        for (int i = 0; i < PV_FX_COUNT_V9; ++i)
            fx_lift_v8(&g_cfg.rgb.simple[i], &o->simple[i]);
        memcpy(g_cfg.rgb.h2d_active, o->h2d_active, sizeof(g_cfg.rgb.h2d_active));
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));

        g_cfg.printer           = stored.v8.printer;
        g_cfg.ap                = stored.v8.ap;
        memcpy(g_cfg.hostname, stored.v8.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v8.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v8.motor_manual;
        g_cfg.motor_manual_open = stored.v8.motor_manual_open;
        memcpy(g_cfg.device_name, stored.v8.device_name, sizeof(g_cfg.device_name));
        g_cfg.ring_mode         = stored.v8.ring_mode;
        g_cfg.ring_blink        = stored.v8.ring_blink;
        memcpy(g_cfg.leds, stored.v8.leds, sizeof(g_cfg.leds));
        g_cfg.magic             = CFG_MAGIC;

        if (!h2d_load_all())
            ESP_LOGW(TAG, "some H2D tables missing; defaults kept for those");
        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v8 -> v10 (%u B -> %u B), ramp and spare byte unset",
                 (unsigned)size, (unsigned)sizeof(pv_cfg_t));
        pv_cfg_save();
        h2d_save_all();
        return;
    }

    // v7 -> v8. Two changes at once: the effect parameter grew two colours and
    // a flag byte, and the H2D tables moved out of this blob into one key per
    // device state. Field by field, and every inactive colour starts unset so
    // the strip looks exactly as it did.
    if (err == ESP_OK && stored.v7.magic == CFG_MAGIC_V7 &&
        size == sizeof(pv_cfg_v7_t)) {
        const pv_rgb_cfg_v7_t *o = &stored.v7.rgb;
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT; ++i)
            fx_lift_v7(&g_cfg.rgb.simple[i], &o->simple[i]);
        for (int st = 0; st < PV_ST_COUNT; ++st) {
            g_cfg.rgb.h2d_active[st] = o->h2d_active[st];
            for (int f = 0; f < PV_FX_COUNT; ++f)
                fx_lift_v7(&g_h2d[st][f], &o->h2d[st][f]);
        }
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));

        g_cfg.printer           = stored.v7.printer;
        g_cfg.ap                = stored.v7.ap;
        memcpy(g_cfg.hostname, stored.v7.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v7.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v7.motor_manual;
        g_cfg.motor_manual_open = stored.v7.motor_manual_open;
        memcpy(g_cfg.device_name, stored.v7.device_name, sizeof(g_cfg.device_name));
        g_cfg.ring_mode         = stored.v7.ring_mode;
        g_cfg.ring_blink        = stored.v7.ring_blink;
        g_cfg.magic             = CFG_MAGIC;

        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v7 -> v8 (%u B -> %u B), H2D split into per-state keys",
                 (unsigned)size, (unsigned)sizeof(pv_cfg_t));
        pv_cfg_save();
        h2d_save_all();
        return;
    }

    // v6/v5 -> v7. The effect PARAMETER shrank from sixteen bytes to eight
    // when colours stopped being stored as text, which moves every byte after
    // rgb.simple[0]. Field by field, as with every other layout change here.
    //
    // v6 and v5 differ only by the two ring bytes at the very end, so one arm
    // reads both and v5 simply leaves the ring settings at their defaults.
    if (err == ESP_OK &&
        ((stored.v6.magic == CFG_MAGIC_V6 && size == sizeof(pv_cfg_v6_t)) ||
         (stored.v5.magic == CFG_MAGIC_V5 && size == sizeof(pv_cfg_v5_t)))) {
        bool has_ring = (stored.v6.magic == CFG_MAGIC_V6);
        const pv_rgb_cfg_v6_t *o = &stored.v6.rgb;
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT; ++i)
            fx_lift_v6(&g_cfg.rgb.simple[i], &o->simple[i]);
        for (int st = 0; st < PV_ST_COUNT; ++st) {
            g_cfg.rgb.h2d_active[st] = o->h2d_active[st];
            for (int f = 0; f < PV_FX_COUNT; ++f)
                fx_lift_v6(&g_h2d[st][f], &o->h2d[st][f]);
        }
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));

        g_cfg.printer           = stored.v6.printer;
        g_cfg.ap                = stored.v6.ap;
        memcpy(g_cfg.hostname, stored.v6.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v6.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v6.motor_manual;
        g_cfg.motor_manual_open = stored.v6.motor_manual_open;
        memcpy(g_cfg.device_name, stored.v6.device_name, sizeof(g_cfg.device_name));
        if (has_ring) {
            g_cfg.ring_mode  = stored.v6.ring_mode;
            g_cfg.ring_blink = stored.v6.ring_blink;
        }
        g_cfg.magic             = CFG_MAGIC;

        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v%d -> v7 (%u B -> %u B), colours now stored as bytes",
                 has_ring ? 6 : 5, (unsigned)size, (unsigned)sizeof(pv_cfg_t));
        pv_cfg_save();
        h2d_save_all();
        return;
    }

    // v4 -> v5. The effect PARAMETER grew from nine bytes to sixteen, which
    // moves every byte after rgb.simple[0]. Same trap as v1 and v3: a prefix
    // memcpy would shift the printer binding and the Wi-Fi credentials. Field
    // by field, and every effect's closed colour starts equal to its open one.
    if (err == ESP_OK && stored.v4.magic == CFG_MAGIC_V4 &&
        size == sizeof(pv_cfg_v4_t)) {
        const pv_rgb_cfg_v4_t *o = &stored.v4.rgb;
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT; ++i)
            fx_lift_v4(&g_cfg.rgb.simple[i], &o->simple[i]);
        for (int st = 0; st < PV_ST_COUNT; ++st) {
            g_cfg.rgb.h2d_active[st] = o->h2d_active[st];
            for (int f = 0; f < PV_FX_COUNT; ++f)
                fx_lift_v4(&g_h2d[st][f], &o->h2d[st][f]);
        }
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));

        g_cfg.printer           = stored.v4.printer;
        g_cfg.ap                = stored.v4.ap;
        memcpy(g_cfg.hostname, stored.v4.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v4.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v4.motor_manual;
        g_cfg.motor_manual_open = stored.v4.motor_manual_open;
        memcpy(g_cfg.device_name, stored.v4.device_name, sizeof(g_cfg.device_name));
        g_cfg.magic             = CFG_MAGIC;

        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v4 -> v7 (%u B -> %u B), closed colour = open colour",
                 (unsigned)size, (unsigned)sizeof(pv_cfg_t));
        pv_cfg_save();
        h2d_save_all();
        return;
    }

    // v3/v2 -> v4. Nine effects became eighteen, and PV_FX_COUNT is an array
    // dimension in the MIDDLE of pv_rgb_cfg_t, so this is the same trap v1
    // fell into: nothing after rgb.simple kept its offset and a prefix memcpy
    // would silently shift the printer binding, the AP credentials and the
    // hostname. Field by field, same as the v1 arm below.
    //
    // v3 and v2 share this arm because they share the rgb shape; the only
    // difference is that v2 predates device_name and leaves it at default.
    if (err == ESP_OK &&
        ((stored.v3.magic == CFG_MAGIC_V3 && size == sizeof(pv_cfg_v3_t)) ||
         (stored.v2.magic == CFG_MAGIC_V2 && size == sizeof(pv_cfg_v2_t)))) {
        bool has_name = (stored.v3.magic == CFG_MAGIC_V3);
        // Both shapes put rgb at the same offset, so one pointer serves.
        const pv_rgb_cfg_v3_t *o = &stored.v3.rgb;
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT_V3; ++i)
            fx_lift_v4(&g_cfg.rgb.simple[i], &o->simple[i]);
        for (int st = 0; st < PV_ST_COUNT; ++st) {
            g_cfg.rgb.h2d_active[st] = o->h2d_active[st];
            for (int f = 0; f < PV_FX_COUNT_V3; ++f)
                fx_lift_v4(&g_h2d[st][f], &o->h2d[st][f]);
        }
        memcpy(g_cfg.rgb.warnhot_current, o->warnhot_current,
               sizeof(g_cfg.rgb.warnhot_current));
        memcpy(g_cfg.rgb.warnhot_bg, o->warnhot_bg, sizeof(g_cfg.rgb.warnhot_bg));
        memcpy(g_cfg.rgb.warnhot_speed, o->warnhot_speed,
               sizeof(g_cfg.rgb.warnhot_speed));

        g_cfg.printer           = stored.v3.printer;
        g_cfg.ap                = stored.v3.ap;
        memcpy(g_cfg.hostname, stored.v3.hostname, sizeof(g_cfg.hostname));
        memcpy(g_cfg.language, stored.v3.language, sizeof(g_cfg.language));
        g_cfg.motor_manual      = stored.v3.motor_manual;
        g_cfg.motor_manual_open = stored.v3.motor_manual_open;
        if (has_name)
            memcpy(g_cfg.device_name, stored.v3.device_name,
                   sizeof(g_cfg.device_name));
        g_cfg.magic             = CFG_MAGIC;

        cfg_clamp_loaded();
        ESP_LOGW(TAG, "migrated config v%d -> v7 (%u B -> %u B), %d effects -> %d",
                 has_name ? 3 : 2, (unsigned)size, (unsigned)sizeof(pv_cfg_t),
                 PV_FX_COUNT_V3, PV_FX_COUNT);
        pv_cfg_save();
        h2d_save_all();
        return;
    }

    if (err == ESP_OK && size == sizeof(pv_cfg_v1_t) &&
        stored.v1.magic == CFG_MAGIC_V1) {
        // Seven effects becoming eighteen moved everything after rgb.simple,
        // so this copies field by field. g_cfg already holds factory
        // defaults, which means the new effect slots arrive correctly filled
        // and only the seven that existed get overwritten.
        const pv_rgb_cfg_v1_t *o = &stored.v1.rgb;
        g_cfg.rgb.light_on       = o->light_on;
        g_cfg.rgb.warning_sw     = o->warning_sw;
        g_cfg.rgb.follow_printer = o->follow_printer;
        g_cfg.rgb.follow_vent    = o->follow_vent;
        g_cfg.rgb.reverse        = o->reverse;
        g_cfg.rgb.light_mode     = o->light_mode;
        g_cfg.rgb.simple_current = o->simple_current;
        for (int i = 0; i < PV_FX_COUNT_V1; ++i)
            fx_lift_v4(&g_cfg.rgb.simple[i], &o->simple[i]);
        for (int st = 0; st < PV_ST_COUNT; ++st) {
            g_cfg.rgb.h2d_active[st] = o->h2d_active[st];
            for (int f = 0; f < PV_FX_COUNT_V1; ++f)
                fx_lift_v4(&g_h2d[st][f], &o->h2d[st][f]);
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
        ESP_LOGW(TAG, "migrated config v1 -> v7 (%u B -> %u B), %d effects -> %d",
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
    if (err == ESP_OK) {
        err = nvs_set_blob(h, CFG_KEY, &g_cfg, sizeof(g_cfg));

        // Measured on this device 2026-08-30: of the 378 entries in the three
        // NVS pages, one page is reserved for garbage collection and the Wi-Fi
        // stack's own keys take about 160 of the rest, cal_data alone being 62.
        // That leaves barely enough for ONE copy of this config, and
        // nvs_set_blob writes the new copy BEFORE releasing the old one, so it
        // needs room for two and fails.
        //
        // Releasing the old copy first turns a two-copy peak into a one-copy
        // peak, which fits. It is only done as a RETRY, never on the normal
        // path, because between the erase and the write there is a window in
        // which a power cut loses the config. Taking that small risk on the
        // path that is otherwise guaranteed to fail is the right trade; taking
        // it on every save would not be.
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "save failed (%d), releasing the old copy and retrying", err);
            esp_err_t e2 = nvs_erase_key(h, CFG_KEY);
            if (e2 == ESP_OK || e2 == ESP_ERR_NVS_NOT_FOUND) {
                nvs_commit(h);
                err = nvs_set_blob(h, CFG_KEY, &g_cfg, sizeof(g_cfg));
            }
        }
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGE(TAG, "nvs_open: %d", err);
    }

    // A failed save used to be one log line and nothing else. The device went
    // on working from RAM, looked healthy, and lost every setting at the next
    // reboot. Now it is remembered and reported in the state document, so the
    // UI can say so while there is still time to do something about it.
    //
    // The push happens on any CHANGE, not just on failure. Announcing only the
    // failure is how the warning banner outlived the problem: the flag cleared
    // on the next good save and nothing told the page, so it kept warning about
    // a condition that had already passed. A warning that does not withdraw
    // itself is worse than no warning, because the next real one is ignored.
    if (err != ESP_OK)
        ESP_LOGE(TAG, "CONFIG SAVE FAILED (%d)", err);
    save_failed_set(SAVE_CFG, err != ESP_OK);
}

void pv_factory_reset_and_reboot(void)
{
    // The factory manual: reset clears Wi-Fi, printer binding, lighting, and
    // preferences. Wipe the whole NVS partition (esp_wifi creds included).
    ESP_LOGW(TAG, "FACTORY RESET");
    nvs_flash_erase();
    esp_restart();
}
