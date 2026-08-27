// Vent motors + button, factory semantics:
//   AUTO (default): exhaust opens while printing, closes when idle; ring
//   LED off. MANUAL: single click toggles the vent, ring LED blinks; long
//   press (3 s) returns to AUTO. BOOT long press (3 s) = factory reset.
// Each of up to 4 groups: forward/reverse PWM with gradual startup and a
// hall sensor; travel ends on hall plateau or timeout.
#include "pv.h"

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
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
// From BIQU's motor_ledc_timer_init at 0x400deaf0: speed_mode 1,
// duty_resolution 10 bits, freq_hz 30000 (literal 0x7530 at 0x400d0e24).
#define PWM_RES       LEDC_TIMER_10_BIT
#define PWM_DUTY_MAX  1023
// 10 bit resolution means full scale is 1023, not 255. Getting this wrong
// would run the motors at a quarter power.
#define PWM_MAX       PWM_DUTY_MAX
#define RAMP_MS       400
#define TRAVEL_MS_MAX 5000

static adc_oneshot_unit_handle_t s_adc;
static int s_groups = PV_MOTOR_GROUPS;
static volatile bool s_want_open, s_moving;

static void duty(const group_t *g, bool open_dir, uint32_t d)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, open_dir ? g->fwd_ch : g->rev_ch, d);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, open_dir ? g->fwd_ch : g->rev_ch);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, open_dir ? g->rev_ch : g->fwd_ch, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, open_dir ? g->rev_ch : g->fwd_ch);
}

// BIQU's hall_get_state, 0x400deb2c. Not a plateau detector: a four band
// classifier over the raw ADC value, with the bands hardcoded in the image.
//
//   raw == 0                      -> 0   (nothing on the channel)
//   1360 <= raw <= 1680           -> 2
//    640 <= raw <=  960           -> 1
//   raw  >  2450                  -> 4
//   anything else                 -> 3
//
// The magnitudes come straight from the compare sequence: raw - 1360 <= 320,
// raw - 640 <= 320, then raw - 2080 > 370.
static int hall_state_from_raw(int raw)
{
    if (raw == 0) return 0;
    if ((unsigned)(raw - 1360) <= 320u) return 2;
    if ((unsigned)(raw -  640) <= 320u) return 1;
    if (raw - 2080 > 370) return 4;
    return 3;
}

static int hall_raw(const group_t *g)
{
    int raw = 0;
    if (s_adc) adc_oneshot_read(s_adc, g->hall, &raw);
    return raw;
}


static void stop_all(void)
{
    for (int i = 0; i < s_groups; ++i) duty(&GROUPS[i], true, 0);
}

// BIQU's motor loop, from the task at 0x400de564.
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
// (0x400de324).
//
// A travel timeout is kept here purely as a stall guard, because a jammed
// vent that never reaches its band would otherwise drive its motor forever.
// Stock appears to accept that risk; this firmware does not.

#define MOTOR_TICK_MS   200

// Button timings, all from BIQU's button_task at 0x400defa4.
#define PV_BTN_POLL_MS           10     // vTaskDelay(1) at 0x400df082
#define PV_BTN_LONG_MS           2999   // literal 0x400d0950
#define PV_BTN_CLICK_SETTLE_MS   300    // 0x12c at 0x400df062
#define HALL_OPEN       1
#define HALL_CLOSED     2

static void drive_task(void *arg)
{
    bool open_dir = (bool)(uintptr_t)arg;
    int target = open_dir ? HALL_OPEN : HALL_CLOSED;
    s_moving = true;
    ESP_LOGI(TAG, "vent -> %s (hall target %d)", open_dir ? "OPEN" : "CLOSED", target);

    bool moving[PV_MOTOR_GROUPS] = {0};
    TickType_t start = xTaskGetTickCount();

    for (;;) {
        TickType_t el = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
        int running = 0;

        for (int i = 0; i < s_groups; ++i) {
            int st = hall_state_from_raw(hall_raw(&GROUPS[i]));
            if (st != target) {
                duty(&GROUPS[i], open_dir, PWM_MAX);
                moving[i] = true;
                ++running;
            } else if (moving[i]) {
                duty(&GROUPS[i], open_dir, 0);
                moving[i] = false;
                ESP_LOGI(TAG, "group %d reached hall state %d", i, st);
            }
        }

        if (!running) break;
        if (el >= TRAVEL_MS_MAX) {            // stall guard, not stock
            ESP_LOGW(TAG, "travel timeout with %d group(s) short of target",
                     running);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(MOTOR_TICK_MS));
    }

    stop_all();
    g_live.vent_open = open_dir;
    s_moving = false;

    vTaskDelete(NULL);
}

static void vent_go(bool open_dir)
{
    if (s_moving || g_live.vent_open == open_dir) {
        if (!s_moving) return;
    }
    if (s_moving) return;   // let the current travel finish
    xTaskCreate(drive_task, "pv_drive", 3072, (void *)(uintptr_t)open_dir, 5, NULL);
}

void pv_motor_update(void)
{
    if (g_cfg.motor_manual) return;
    bool open_dir = (g_live.device_state == PV_ST_PRINTING ||
                     g_live.device_state == PV_ST_PAUSED);
    vent_go(open_dir);
}

void pv_motor_set_auto(bool auto_mode)
{
    g_cfg.motor_manual = !auto_mode;
    pv_cfg_save();
    if (auto_mode) pv_motor_update();
}

void pv_motor_manual_toggle(void)
{
    g_cfg.motor_manual = true;
    g_cfg.motor_manual_open = !g_live.vent_open;
    pv_cfg_save();
    vent_go(g_cfg.motor_manual_open);
}

// ---------- button + ring LED ----------

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
    // BIQU's button_task, 0x400defa4. Their timings, read from the image:
    //
    //   poll interval        10 ms   (vTaskDelay(1) at 0x400df082)
    //   long press fires at  > 2999 ms  (literal 0x400d0950 = 0xbb7)
    //   a single click is dispatched only after the line has been released
    //   for more than 300 ms (0x12c at 0x400df062), so a double tap does not
    //   register as two separate clicks
    //
    // They also debounce by re-reading the level after a yield before
    // accepting an edge.
    int user_held = 0, boot_held = 0, blink = 0;
    int user_released_ms = 0;
    bool user_pending_click = false;
    bool user_armed = false, boot_armed = false;

    for (;; vTaskDelay(pdMS_TO_TICKS(PV_BTN_POLL_MS))) {
        // Ring LED: off in AUTO, blink in MANUAL (stock behavior).
        if (g_cfg.motor_manual) {
            if (++blink >= 50) blink = 0;          // 500 ms period at 10 ms poll
            gpio_set_level(PV_PIN_BUTTON_LED, blink < 25);
        } else {
            gpio_set_level(PV_PIN_BUTTON_LED, 0);
        }

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
                if (boot_held > PV_BTN_LONG_MS) {
                    boot_held = -1000000;
                    pv_factory_reset_and_reboot();
                }
            } else {
                boot_held = 0;
            }
        }
    }
}

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
    // Kit auto-detect line (GPIO35): ~1900-2400 raw = 4 motors, ~1100-1700 = 2.
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_7, &cc);
    int det = 0;
    adc_oneshot_read(s_adc, ADC_CHANNEL_7, &det);
    (void)det;
    if (det >= 1100 && det < 1800) s_groups = 2;
    ESP_LOGI(TAG, "config-detect raw=%d -> %d motor group(s)", det, s_groups);

    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = PWM_RES, .freq_hz = PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc timer failed (%s); motors disabled",
                 esp_err_to_name(err));
        s_groups = 0;
        xTaskCreate(button_task, "pv_btn", 3072, NULL, 3, NULL);
        return;
    }
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
}
