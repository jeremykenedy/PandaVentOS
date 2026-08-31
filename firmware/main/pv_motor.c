// Vent motors + button, factory semantics:
//   AUTO (default): exhaust opens while printing, closes when idle; ring
//   LED off. MANUAL: single click toggles the vent, ring LED blinks; long
//   press (3 s) returns to AUTO. BOOT long press (3 s) = factory reset.
// Each of up to 4 groups: forward/reverse PWM with a 20 ms startup ramp and
// a hall sensor; travel ends when the hall reaches the target band.
#include "pv.h"

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pv_motor";

typedef struct {
    adc_channel_t hall;
    ledc_channel_t fwd_ch; int fwd_gpio;
    ledc_channel_t rev_ch; int rev_gpio;
} group_t;

// Recovered stock wiring.
static const group_t GROUPS[PV_MOTOR_GROUPS] = {
    { ADC_CHANNEL_2, LEDC_CHANNEL_4, 22, LEDC_CHANNEL_5, 21 },
    { ADC_CHANNEL_0, LEDC_CHANNEL_0, 25, LEDC_CHANNEL_1, 26 },
    { ADC_CHANNEL_1, LEDC_CHANNEL_2, 32, LEDC_CHANNEL_3, 33 },
    { ADC_CHANNEL_3, LEDC_CHANNEL_6, 23, LEDC_CHANNEL_7, 19 },
};

#define PWM_FREQ_HZ   30000
// From BIQU's motor_ledc_timer_init at 0x400deae8: speed_mode 1,
// duty_resolution 10 bits, freq_hz 30000 (literal 0x7530 at 0x400d0e1c).
#define PWM_RES       LEDC_TIMER_10_BIT
#define PWM_DUTY_MAX  1023
// 10 bit resolution means full scale is 1023, not 255. Getting this wrong
// would run the motors at a quarter power.
#define PWM_MAX       PWM_DUTY_MAX

// Startup ramp, recovered from BIQU's two drive helpers at 0x400de3d4 (fwd)
// and 0x400de47c (rev), which are mirror images of each other:
//
//     ledc_set_duty(1, opposite_ch, 0); ledc_update_duty(1, opposite_ch)
//     vTaskDelay(1)                                     ; 10 ms at a 100 Hz tick
//     if (ledc_get_duty(1, opposite_ch) != 0) return    ; interlock, 0x400de42a
//     esp_rom_delay_us(500)                             ; 0x400de42d, ets_delay_us
//     ledc_set_duty(1, drive_ch, 102); ledc_update_duty(1, drive_ch)
//     ledc_set_fade_with_time(1, drive_ch, 1023, 20)    ; 0x400de447..0x400de450
//     ledc_fade_start(1, drive_ch, LEDC_FADE_NO_WAIT)   ; 0x400de459
//
// So the motor does NOT go straight to full duty. It starts at 102 of 1023,
// about 10 percent, and ramps to full over 20 ms. Before that it drops the
// opposite channel, waits a tick, and refuses to drive at all unless the
// opposite channel reads back zero.
#define PWM_START_DUTY   102    // 0x400de436, movi a12, 102
#define PWM_FADE_MS      20     // 0x400de447, movi.n a13, 20
#define PWM_STOP_FADE_MS 10     // 0x400de358, movi.n a13, 10
#define PWM_DEADTIME_US  500    // 0x400de42d, movi a10, 0x1f4

// The motor task expresses the destination as a hall STATE, not a direction.
// Confirmed against the image: target 1 drives fwd_chan and target 2 drives
// rev_chan. See RE-NOTES.md, Motor section, for the chain through BIQU's own
// ledc_channel_config(&fwd_chan) / (&rev_chan) error strings.
#define HALL_OPEN       1
#define HALL_CLOSED     2

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;
static int s_groups = PV_MOTOR_GROUPS;
static volatile bool s_want_open, s_moving;

// Stock keeps three per-group arrays and the drive helpers consult all three
// before doing anything: moving at 0x3ffb6964, direction at 0x3ffb6960
// (1 = fwd_chan, 0 = rev_chan) and target hall state at 0x3ffb03f0.
static bool s_grp_moving[PV_MOTOR_GROUPS];
static bool s_grp_fwd[PV_MOTOR_GROUPS];
static int  s_grp_target[PV_MOTOR_GROUPS];
// Stock's four per-group fault bytes at 0x3ffb6958. 0x400de524 ORs them into
// the byte at 0x3ffb6954 once per motor-task pass (called at 0x400de7cc, right
// after the per-group loop), and the rgb task reads that byte through
// 0x400de550 to raise the red strobe.
//
// The predicate is RECOVERED, 2026-08-28. It is not a stall guard and it is
// not the >= 4 threshold. 0x400de695 is the in-progress path for group i; it
// runs only while the group's moving byte at 0x3ffb6964 is set, and only once
// its per-group timestamp at 0x3ffb6924 has aged past the same 199 literal the
// drive loop uses (0x400de6f4). At that re-check:
//
//   arrived, target == hall   0x400de720: stop, retry = 0, moving = 0,
//                             fault = 0
//   not yet                   0x400de745: fault = 1, then if the retry count
//                             at 0x3ffb6944 has reached 4 (blti a8, 4 at
//                             0x400de75a) the group is stopped and its moving
//                             byte cleared WITHOUT clearing fault
//
// So the flag is raised at the FIRST re-check that finds the group off target,
// not the fourth, and a group abandoned at four re-checks keeps its flag until
// the next command. The visible consequence is that any travel outlasting one
// re-check strobes the strip red while it runs. That is what stock does.
#define MOTOR_FAULT_GIVEUP 4
static bool s_grp_fault[PV_MOTOR_GROUPS];
static bool s_grp_seen[PV_MOTOR_GROUPS];   // a re-check has already happened
static int  s_grp_retry[PV_MOTOR_GROUPS];

bool pv_motor_fault_any(void)
{
    for (int i = 0; i < PV_MOTOR_GROUPS; ++i)
        if (s_grp_fault[i]) return true;
    return false;
}

// The guard, lifted straight out of both helpers. Stock checks all three
// pieces of state and RETURNS rather than re-issuing the drive sequence:
//
//   0x400de3ea  beqz.n a8, drive      ; not moving        -> drive
//   0x400de3f4  beqz.n a9, drive      ; wrong direction   -> drive
//   0x400de3ff  bnei   a9, 1, drive   ; wrong target      -> drive
//   0x400de402  j      return         ; already doing it  -> RETURN
//
// Kept as its own function so it can be compiled and exercised on the host
// without the rest of the driver. Do not inline it away.
static bool drive_needed(bool moving, bool fwd, int target,
                         bool open_dir, int want)
{
    return !(moving && fwd == open_dir && target == want);
}

// BIQU's stop helper, 0x400de31c. Fades the channel that is actually running
// down to zero over 10 ms and waits for it, then verifies BOTH channels read
// back zero and forces them if not.
static void group_stop(int i)
{
    const group_t *g = &GROUPS[i];
    ledc_channel_t act = s_grp_fwd[i] ? g->fwd_ch : g->rev_ch;

    if (ledc_get_duty(LEDC_LOW_SPEED_MODE, act) != 0) {
        ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, act, 0, PWM_STOP_FADE_MS);
        ledc_fade_start(LEDC_LOW_SPEED_MODE, act, LEDC_FADE_WAIT_DONE);
    }
    if (ledc_get_duty(LEDC_LOW_SPEED_MODE, g->fwd_ch) != 0 ||
        ledc_get_duty(LEDC_LOW_SPEED_MODE, g->rev_ch) != 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, g->fwd_ch, 0);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, g->rev_ch, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, g->fwd_ch);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, g->rev_ch);
    }
    s_grp_moving[i] = false;
}

// BIQU's drive helpers, 0x400de3d4 (fwd) and 0x400de47c (rev). One function
// here because the two are mirror images with the channels swapped.
static void group_drive(int i, bool open_dir)
{
    const group_t *g = &GROUPS[i];
    int want = open_dir ? HALL_OPEN : HALL_CLOSED;

    if (!drive_needed(s_grp_moving[i], s_grp_fwd[i], s_grp_target[i],
                      open_dir, want))
        return;                       // already travelling this way

    if (s_grp_moving[i]) group_stop(i);

    ledc_channel_t off = open_dir ? g->rev_ch : g->fwd_ch;
    ledc_channel_t on  = open_dir ? g->fwd_ch : g->rev_ch;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, off, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, off);
    vTaskDelay(1);
    if (ledc_get_duty(LEDC_LOW_SPEED_MODE, off) != 0)
        return;                       // opposite side has not let go
    esp_rom_delay_us(PWM_DEADTIME_US);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, on, PWM_START_DUTY);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, on);
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, on, PWM_MAX, PWM_FADE_MS);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, on, LEDC_FADE_NO_WAIT);

    s_grp_moving[i] = true;
    s_grp_fwd[i]    = open_dir;
    s_grp_target[i] = want;
}

// BIQU's hall_get_state, 0x400deb24. Not a plateau detector: a four band
// classifier over the raw ADC value, with the bands hardcoded in the image.
//
//   raw == 0                      -> 0   (nothing on the channel)
//   1360 <= raw <= 1680           -> 2
//    640 <= raw <=  960           -> 1
//   2080 <= raw <= 2450           -> 3
//   anything else                 -> 4
//
// All three range tests are the same unsigned idiom. The third one is BLTU,
// not a signed compare:
//
//     movi   a8, -2080
//     add.n  a2, a2, a8      ; a2 = raw - 2080
//     movi   a8, 370
//     bltu   a8, a2, +       ; 370 <u (raw-2080)  ->  return 4
//     movi.n a2, 3           ; else               ->  return 3
//
// Because it is unsigned, any raw below 2080 that misses both bands wraps to
// a huge value and returns 4, not 3. This was written as a signed compare
// with the last two bands inverted until 2026-08-27, which was wrong for raw
// in 1..639, 961..1359 and 1681..2079.
static int hall_state_from_raw(int raw)
{
    if (raw == 0) return 0;
    if ((unsigned)(raw - 1360) <= 320u) return 2;
    if ((unsigned)(raw -  640) <= 320u) return 1;
    if ((unsigned)(raw - 2080) <= 370u) return 3;
    return 4;
}

// Stock does NOT classify raw ADC counts. At 0x400deb45 it takes the raw
// reading, passes it through adc_cali_raw_to_voltage (call8 0x40115404) and
// classifies the resulting MILLIVOLTS at 0x400deb51. The bands 640..960,
// 1360..1680 and 2080..2450 are volts, not counts. Classifying counts against
// them is a unit error that makes every target unreachable, which is what
// drove the motors against their stops on 2026-08-30.
static int hall_raw(const group_t *g)
{
    int raw = 0;
    if (!s_adc) return 0;
    if (adc_oneshot_read(s_adc, g->hall, &raw) != ESP_OK) return 0;
    if (!s_cali) return raw;          // uncalibrated: caller sees counts
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return raw;
    return mv;
}

// Raw counts, for the kit-detect line and the safe-build probe.
__attribute__((unused)) static int hall_raw_counts(const group_t *g)
{
    int raw = 0;
    if (s_adc) adc_oneshot_read(s_adc, g->hall, &raw);
    return raw;
}


static void stop_all(void)
{
    for (int i = 0; i < s_groups; ++i) group_stop(i);
}

// BIQU's motor loop, from the task at 0x400de55c.
//
// The design is much simpler than a ramp-and-plateau. The vent target is
// expressed as a HALL STATE, not a direction and a duration:
//
//     open   -> hall state 1   (raw 640..960)
//     closed -> hall state 2   (raw 1360..1680)
//
// The task polls on a 200 ms tick. For each motor group it reads
// hall_get_state and compares it against the target:
//
//     state != target  ->  drive toward it, mark the group moving
//     state == target  ->  if it was moving, stop it and clear the flag
//
// That is the whole stop rule. There is no soft-start ramp in the travel
// path and no travel timeout: the hall sensor's own end-position bands ARE
// the limit switches. Stopping goes through ledc_stop with a 10 ms fade
// (0x400de31c).
//
// There is no travel timeout, because stock has none. A jammed vent that
// never reaches its band will drive its motor until the target changes.
// That is stock's behaviour and this is a clone, so it is ours. A stall
// guard was carried here until 2026-08-28 and has been removed: a
// deliberate departure is still a departure.

// 200 ms. Stock's literal is 199, as movi a9, 199 then bgeu a9, elapsed,
// skip, at 0x400de5a5. It acts once elapsed passes 199.
#define MOTOR_TICK_MS   200

// Button timings, all from BIQU's button_task at 0x400def9c.
#define PV_BTN_POLL_MS           10     // vTaskDelay(1) at 0x400df07a
#define PV_BTN_LONG_MS           2999   // literal 0x400d0948
#define PV_BTN_CLICK_SETTLE_MS   300    // 0x12c at 0x400df05a

static void drive_task(void *arg)
{
    bool open_dir = (bool)(uintptr_t)arg;
    int target = open_dir ? HALL_OPEN : HALL_CLOSED;
    s_moving = true;
    ESP_LOGI(TAG, "vent -> %s (hall target %d)", open_dir ? "OPEN" : "CLOSED", target);

    bool moving[PV_MOTOR_GROUPS] = {0};
    // SAFETY DEPARTURE FROM STOCK. Stock re-issues the drive on the tick after
    // it gives up, so a group whose target is unreachable is driven on/off at
    // 200 ms forever. On 2026-08-30 that ground the mechanism audibly and the
    // unit had to be unplugged. Once a group is abandoned it stays abandoned
    // for the rest of this travel. Recorded in RE-NOTES.md.
    bool abandoned[PV_MOTOR_GROUPS] = {0};

    for (;;) {
        int running = 0;

        for (int i = 0; i < s_groups; ++i) {
            if (abandoned[i]) continue;
            int st = hall_state_from_raw(hall_raw(&GROUPS[i]));
            if (st != target) {
                // group_drive holds stock's guard: on every tick after the
                // first it returns immediately rather than re-issuing, so the
                // 20 ms ramp is never stamped on.
                group_drive(i, open_dir);
                moving[i] = true;
                ++running;
                if (s_grp_seen[i]) {
                    // DEPARTURE FROM STOCK, 2026-08-30. Stock sets the fault
                    // flag HERE, at the first re-check that finds the group
                    // still moving (0x400de745), which is every travel longer
                    // than one 200 ms tick. Since the rgb task reads that flag
                    // to raise the red strobe, stock flashes the strip red for
                    // the whole of every normal open and close.
                    //
                    // A vent that is travelling is not faulted, and painting it
                    // red teaches people to ignore the one colour that is
                    // supposed to mean something. The flag is now raised only
                    // when the group is actually given up on, which is the case
                    // that genuinely needs attention. Everything downstream is
                    // untouched: a stuck vent still strobes red, and still
                    // stays red until the next command.
                    if (++s_grp_retry[i] >= MOTOR_FAULT_GIVEUP) {
                        // 0x400de75d: stop and clear moving, leaving fault set.
                        s_grp_fault[i] = true;
                        group_stop(i);
                        moving[i] = false;
                        s_grp_seen[i] = false;
                        abandoned[i] = true;
                        ESP_LOGW(TAG, "group %d abandoned off target %d", i, target);
                    }
                } else {
                    s_grp_seen[i] = true;
                }
            } else {
                // 0x400de720 clears retry, the moving byte at 0x3ffb6964 and
                // the fault byte together when the group reaches its band.
                s_grp_retry[i] = 0;
                s_grp_fault[i] = false;
                s_grp_seen[i] = false;
                if (moving[i]) {
                    group_stop(i);
                    moving[i] = false;
                    ESP_LOGI(TAG, "group %d reached hall state %d", i, st);
                }
            }
        }

        if (!running) break;
        vTaskDelay(pdMS_TO_TICKS(MOTOR_TICK_MS));
    }

    stop_all();
    g_live.vent_open = open_dir;
    s_moving = false;

    // The vent's position is part of the state document now (vent_policy
    // reports it), and this is the only place it changes. Without this push
    // the UI, and anything testing the policy from outside, reads the position
    // the vent had BEFORE this travel and stays one move behind forever.
    pv_ws_push_state();

    vTaskDelete(NULL);
}

static void vent_go(bool open_dir)
{
#if PV_SAFE_NO_MOTORS
    ESP_LOGW(TAG, "SAFE BUILD: drive to %s refused", open_dir ? "OPEN" : "CLOSED");
    return;
#endif
    if (s_moving || g_live.vent_open == open_dir) {
        if (!s_moving) return;
    }
    if (s_moving) return;   // let the current travel finish
    // 4096, not 3072: the task now builds and pushes the state document on
    // arrival, and the sys_evt overflow earlier in this project is a standing
    // reminder of what a tight stack costs.
    xTaskCreate(drive_task, "pv_drive", 4096, (void *)(uintptr_t)open_dir, 5, NULL);
}

void pv_motor_update(void)
{
    if (g_cfg.motor_manual) return;
    // Stock's whole AUTO rule, one line: open while printing or paused.
    bool open_dir = (g_live.device_state == PV_ST_PRINTING ||
                     g_live.device_state == PV_ST_PAUSED);
    // The material-aware policy sits on top and can override it. With its
    // master switch off it returns stock_open unchanged, so stock behaviour is
    // bit-for-bit what it was.
    bool want = pv_policy_decide(open_dir, g_live.vent_open);
    // Log the DECISION CHANGING, not the decision holding.
    //
    // This runs about once a second. Logging every time the policy disagreed
    // with stock wrote the same line once a second for the life of the print,
    // which filled the sixty-four line ring in a minute: the log viewer could
    // only ever show one repeated sentence, and any error that mattered had
    // already been pushed off the end. What is worth recording is the moment
    // the answer changes, and the inputs it changed on.
    {
        static bool s_seen;
        static bool s_last_override, s_last_want, s_last_stock;
        bool override_now = (want != open_dir);
        if (!s_seen || override_now != s_last_override
            || want != s_last_want || open_dir != s_last_stock) {
            if (override_now) {
                int r = pv_policy_match(g_live.material);
                ESP_LOGI(TAG, "policy overrides vent: %s -> %s (mat=%s rule=%s bed=%d)",
                         open_dir ? "OPEN" : "CLOSED", want ? "OPEN" : "CLOSED",
                         g_live.material[0] ? g_live.material : "?",
                         r >= 0 ? pv_material_name[r] : "none", g_live.bed_temp);
            } else if (s_seen && s_last_override) {
                ESP_LOGI(TAG, "policy no longer overrides vent: %s (mat=%s bed=%d)",
                         want ? "OPEN" : "CLOSED",
                         g_live.material[0] ? g_live.material : "?", g_live.bed_temp);
            }
            s_seen = true;
            s_last_override = override_now;
            s_last_want = want;
            s_last_stock = open_dir;
        }
    }
    vent_go(want);
}

void pv_motor_set_auto(bool auto_mode)
{
    g_cfg.motor_manual = !auto_mode;
    pv_cfg_save();
    if (auto_mode) pv_motor_update();
}

// NOT STOCK. Re-seat both endstops and report what the sensor read at each.
// See the block comment on pv_motor_calibrate_start in pv.h for why this is
// a measurement rather than a calibration.
static pv_cal_t s_cal;
static portMUX_TYPE s_cal_lock = portMUX_INITIALIZER_UNLOCKED;

static void cal_set(const pv_cal_t *v)
{
    portENTER_CRITICAL(&s_cal_lock);
    s_cal = *v;
    portEXIT_CRITICAL(&s_cal_lock);
    pv_ws_push_state();
}

void pv_motor_calibrate_get(pv_cal_t *out)
{
    portENTER_CRITICAL(&s_cal_lock);
    *out = s_cal;
    portEXIT_CRITICAL(&s_cal_lock);
}

// Wait for the drive task to finish, or give up. Stock has no travel timeout
// and neither does drive_task, but THIS caller has to have one: a jammed vent
// would otherwise leave the calibration marked as running for ever, and a
// progress bar that never ends is worse than an honest failure.
#define CAL_TRAVEL_MS   20000

static bool cal_travel(bool open_dir)
{
    pv_motor_set_mode(open_dir ? PV_VENT_OPEN : PV_VENT_CLOSED);
    // Give the drive task a tick to pick the travel up before watching for it
    // to end, or an already-seated vent reads as "finished" before it started.
    vTaskDelay(pdMS_TO_TICKS(300));
    for (int waited = 0; waited < CAL_TRAVEL_MS; waited += 100) {
        if (!s_moving) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

// The average across the groups, because there is one vent and the owner is
// being shown one number. A group that reads nothing at all is left out: on a
// single-group unit the second channel reads zero, and averaging that in would
// halve a perfectly good reading.
static int cal_read_mv(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < s_groups; ++i) {
        int mv = hall_raw(&GROUPS[i]);
        if (mv <= 0) continue;
        sum += mv; ++n;
    }
    return n ? (sum / n) : 0;
}

static void cal_task(void *arg)
{
    int restore = (int)(uintptr_t)arg;
    pv_cal_t c = { .state = PV_CAL_RUNNING, .step = 0 };
    cal_set(&c);

    bool ok = cal_travel(false);                 // closed first
    vTaskDelay(pdMS_TO_TICKS(400));              // let it settle on the stop
    c.closed_mv = cal_read_mv();
    c.closed_ok = ok && hall_state_from_raw(c.closed_mv) == HALL_CLOSED;
    c.step = 1;
    cal_set(&c);

    ok = cal_travel(true);                       // then open
    vTaskDelay(pdMS_TO_TICKS(400));
    c.open_mv = cal_read_mv();
    c.open_ok = ok && hall_state_from_raw(c.open_mv) == HALL_OPEN;
    c.step = 2;
    cal_set(&c);

    // Put the vent back under whatever was driving it before, rather than
    // leaving it held open because a diagnostic ran.
    pv_motor_set_mode(restore);

    c.state = (c.closed_ok && c.open_ok) ? PV_CAL_DONE : PV_CAL_FAILED;
    c.at_s  = (int)(esp_timer_get_time() / 1000000);
    cal_set(&c);
    ESP_LOGI(TAG, "endstops: closed %d mV %s, open %d mV %s",
             c.closed_mv, c.closed_ok ? "ok" : "OFF BAND",
             c.open_mv,   c.open_ok   ? "ok" : "OFF BAND");
    vTaskDelete(NULL);
}

bool pv_motor_calibrate_start(void)
{
#if PV_SAFE_NO_MOTORS
    ESP_LOGW(TAG, "SAFE BUILD: endstop check refused");
    return false;
#else
    pv_cal_t now;
    pv_motor_calibrate_get(&now);
    if (now.state == PV_CAL_RUNNING) return false;
    int restore = pv_motor_get_mode();
    return xTaskCreate(cal_task, "pv_cal", 3072,
                       (void *)(uintptr_t)restore, 4, NULL) == pdPASS;
#endif
}

// NOT STOCK. The button gives you a manual toggle and nothing else; there was
// no way to say "hold it open" or "go back to automatic" from the web UI. This
// is the three-way the Control Panel drives.
//   PV_VENT_AUTO   follow the printer, which is the factory behaviour
//   PV_VENT_OPEN   manual, held open
//   PV_VENT_CLOSED manual, held closed
void pv_motor_set_mode(int mode)
{
    switch (mode) {
    case PV_VENT_OPEN:
    case PV_VENT_CLOSED: {
        bool open_dir = (mode == PV_VENT_OPEN);
        g_cfg.motor_manual = true;
        g_cfg.motor_manual_open = open_dir;
        pv_cfg_save();
        vent_go(open_dir);
        break;
    }
    case PV_VENT_AUTO:
    default:
        g_cfg.motor_manual = false;
        pv_cfg_save();
        pv_motor_update();
        break;
    }
}

int pv_motor_get_mode(void)
{
    if (!g_cfg.motor_manual) return PV_VENT_AUTO;
    return g_cfg.motor_manual_open ? PV_VENT_OPEN : PV_VENT_CLOSED;
}

void pv_motor_manual_toggle(void)
{
    g_cfg.motor_manual = true;
    g_cfg.motor_manual_open = !g_live.vent_open;
    pv_cfg_save();
    vent_go(g_cfg.motor_manual_open);
}

// ---------- button + ring LED ----------

// NOT STOCK. One place that decides what the ring LED is doing, so the modes
// cannot drift apart from each other or from the blink toggle.
//
// blink is the task's own 0..49 counter; the first half of it is the lit half.
static bool ring_level(int blink)
{
    // Stock's rule, and the fallback the one-sided modes defer to.
    bool stock = g_cfg.motor_manual
               ? (g_cfg.ring_blink ? (blink < 25) : true)
               : false;

    switch (g_cfg.ring_mode) {
    case PV_RING_ALWAYS_ON:  return true;
    case PV_RING_ALWAYS_OFF: return false;
    // Each of these owns ONE side of the vent's travel and hands the other
    // side back to the stock rule. That is what keeps them distinct: were both
    // sides forced, the two would be the same setting under two names.
    case PV_RING_ON_OPEN:    return g_live.vent_open ? true  : stock;
    case PV_RING_OFF_CLOSED: return g_live.vent_open ? stock : false;
    case PV_RING_AUTO:
    default:                 return stock;
    }
}

static void button_task(void *arg)
{
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PV_PIN_USER_BUTTON) | (1ULL << PV_PIN_BOOT_BUTTON),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << PV_PIN_BUTTON_LED, .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);

    // GPIO 12 is MTDI, a boot strapping pin. Boards commonly fit an external
    // pull-down on it, which reads identically to "button held down forever".
    // Acting on a level would then fire a phantom long-press on every boot and
    // write NVS for a press that never happened. So: require the line to be
    // observed RELEASED once before any press counts, and act on edges only.
    // BIQU's button_task, 0x400def9c. Their timings, read from the image:
    //
    //   poll interval        10 ms   (vTaskDelay(1) at 0x400df07a)
    //   long press fires at  > 2999 ms  (literal 0x400d0948 = 0xbb7)
    //   a single click is dispatched only after the line has been released
    //   for more than 300 ms (0x12c at 0x400df05a), so a double tap does not
    //   register as two separate clicks
    //
    // They also debounce by re-reading the level after a yield before
    // accepting an edge.
    int user_held = 0, boot_held = 0, blink = 0;
    int user_released_ms = 0, boot_released_ms = 0;
    bool user_pending_click = false, boot_pending_click = false;
    bool user_armed = false, boot_armed = false;

    for (;; vTaskDelay(pdMS_TO_TICKS(PV_BTN_POLL_MS))) {
        // Ring LED. The stock rule is off in AUTO and blinking in MANUAL;
        // everything else here is an addition, and PV_RING_AUTO with
        // ring_blink set is stock bit for bit.
        if (++blink >= 50) blink = 0;              // 500 ms period at 10 ms poll
        gpio_set_level(PV_PIN_BUTTON_LED, ring_level(blink));

        bool user_down = gpio_get_level(PV_PIN_USER_BUTTON) == 0;
        bool boot_down = gpio_get_level(PV_PIN_BOOT_BUTTON) == 0;

        if (!user_down) user_armed = true;      // seen released: real button
        if (!boot_down) boot_armed = true;

        if (user_armed) {
            if (user_down) {
                user_held += PV_BTN_POLL_MS;
                user_released_ms = 0;
                if (user_held > PV_BTN_LONG_MS && !user_pending_click) {
                    ESP_LOGI(TAG, "long press (%d ms): AUTO", user_held);
                    pv_motor_set_auto(true);
                    user_pending_click = false;
                    user_held = -1000000;        // one shot per hold
                }
            } else {
                if (user_held > 0) {             // just released from a short press
                    user_pending_click = true;
                    user_released_ms = 0;
                }
                user_held = 0;
                // Stock waits out a 300 ms quiet window before treating the
                // release as a completed single click.
                if (user_pending_click) {
                    user_released_ms += PV_BTN_POLL_MS;
                    if (user_released_ms > PV_BTN_CLICK_SETTLE_MS) {
                        ESP_LOGI(TAG, "click: manual toggle");
                        pv_motor_manual_toggle();
                        user_pending_click = false;
                        user_released_ms = 0;
                    }
                }
            }
        }

        if (boot_armed) {
            if (boot_down) {
                boot_held += PV_BTN_POLL_MS;
                boot_released_ms = 0;
                if (boot_held > PV_BTN_LONG_MS) {
                    // ctx[8], 0x400de938, dispatched at 0x400df035 once the
                    // hold passes the 0xbb7 literal.
                    boot_held = -1000000;
                    boot_pending_click = false;
                    pv_factory_reset_and_reboot();
                }
            } else {
                if (boot_held > 0) {
                    boot_pending_click = true;
                    boot_released_ms = 0;
                }
                boot_held = 0;
                // ctx[4], 0x400dc980, dispatched at 0x400df06x after the same
                // 300 ms quiet window the user button uses.
                if (boot_pending_click) {
                    boot_released_ms += PV_BTN_POLL_MS;
                    if (boot_released_ms > PV_BTN_CLICK_SETTLE_MS) {
                        ESP_LOGI(TAG, "boot click: test mode");
                        pv_rgb_test_cycle();
                        boot_pending_click = false;
                        boot_released_ms = 0;
                    }
                }
            }
        }
    }
}

#if PV_SAFE_NO_MOTORS
// SAFE BUILD ONLY. Reads the hall ADC and reports raw plus classified state so
// the real travel range can be measured by hand. Reads only; drives nothing.
static void hall_probe_task(void *arg)
{
    for (;;) {
        char line[160];
        int o = 0;
        for (int i = 0; i < 2; ++i) {
            int cnt = hall_raw_counts(&GROUPS[i]);
            int mv  = hall_raw(&GROUPS[i]);
            int st  = hall_state_from_raw(mv);
            const char *n = st == 0 ? "zero" : st == 1 ? "OPEN" :
                            st == 2 ? "CLOSED" : st == 3 ? "mid3" : "none4";
            o += snprintf(line + o, sizeof(line) - o,
                          "g%d cnt=%4d mv=%4d st=%d(%s)  ", i, cnt, mv, st, n);
        }
        ESP_LOGW(TAG, "HALL %s", line);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
#endif

void pv_motor_start(void)
{
    // Every failure here is logged and survived. A vent that cannot drive its
    // motors is a degraded vent; a vent that aborts on boot is a brick.
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&u, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc unit init failed (%s); motors disabled",
                 esp_err_to_name(err));
        s_adc = NULL;
        s_groups = 0;
        xTaskCreate(button_task, "pv_btn", 3072, NULL, 3, NULL);
        return;
    }
    adc_oneshot_chan_cfg_t cc = { .bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12 };
    for (int i = 0; i < PV_MOTOR_GROUPS; ++i)
        adc_oneshot_config_channel(s_adc, GROUPS[i].hall, &cc);

    // Stock builds a line-fitting calibration scheme
    // (adc_cali_create_scheme_line_fitting, string at file offset 0x2a934)
    // and converts every hall reading to millivolts before classifying it.
    // Without this the hall bands are compared against raw counts and no
    // target is ever reachable.
    adc_cali_line_fitting_config_t lf = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t ce = adc_cali_create_scheme_line_fitting(&lf, &s_cali);
    if (ce != ESP_OK) {
        ESP_LOGE(TAG, "adc cali failed (%s); hall readings will be raw counts",
                 esp_err_to_name(ce));
        s_cali = NULL;
    }
    // Kit auto-detect line (GPIO35): ~1900-2400 raw = 4 motors, ~1100-1700 = 2.
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_7, &cc);
    int det = 0;
    adc_oneshot_read(s_adc, ADC_CHANNEL_7, &det);   // raw counts, not calibrated
    (void)det;
    if (det >= 1100 && det < 1800) s_groups = 2;
    ESP_LOGI(TAG, "config-detect raw=%d -> %d motor group(s)", det, s_groups);

    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = PWM_RES, .freq_hz = PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&t);
    if (err == ESP_OK) {
        // Stock installs the fade service at the end of
        // motor_ledc_timer_init: call8 0x400f5080 with a10 = 0, i.e.
        // ledc_fade_func_install(0). Without it ledc_fade_start returns
        // ESP_ERR_INVALID_STATE and the startup ramp silently never happens.
        esp_err_t fe = ledc_fade_func_install(0);
        if (fe != ESP_OK)
            ESP_LOGE(TAG, "ledc fade service failed (%s); no startup ramp",
                     esp_err_to_name(fe));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc timer failed (%s); motors disabled",
                 esp_err_to_name(err));
        s_groups = 0;
        xTaskCreate(button_task, "pv_btn", 3072, NULL, 3, NULL);
        return;
    }
#if PV_SAFE_NO_MOTORS
    ESP_LOGW(TAG, "SAFE BUILD: motor channels not configured, motors inert");
    s_groups = 0;
#endif
    for (int i = 0; i < s_groups; ++i) {
        ledc_channel_config_t f = {
            .gpio_num = GROUPS[i].fwd_gpio, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = GROUPS[i].fwd_ch, .timer_sel = LEDC_TIMER_0, .duty = 0,
        };
        ledc_channel_config(&f);
        ledc_channel_config_t r = {
            .gpio_num = GROUPS[i].rev_gpio, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = GROUPS[i].rev_ch, .timer_sel = LEDC_TIMER_0, .duty = 0,
        };
        ledc_channel_config(&r);
    }

    if (g_cfg.motor_manual) {
        vent_go(g_cfg.motor_manual_open);
    }
    xTaskCreate(button_task, "pv_btn", 3072, NULL, 3, NULL);
#if PV_SAFE_NO_MOTORS
    xTaskCreate(hall_probe_task, "pv_hall", 3072, NULL, 3, NULL);
#endif
}
