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

static void drive_task(void *arg)
{
    bool open_dir = (bool)(uintptr_t)arg;
    s_moving = true;
    ESP_LOGI(TAG, "vent -> %s", open_dir ? "OPEN" : "CLOSED");

    int last[PV_MOTOR_GROUPS] = {0}, flat[PV_MOTOR_GROUPS] = {0};
    bool done[PV_MOTOR_GROUPS] = {0};
    // Track BIQU's four-band hall STATE, not a raw millivolt level. Their
    // hall_get_state is a position classifier, so travel is finished when the
    // reported state stops changing.
    //
    // NOT YET RECOVERED: how BIQU's motor.c consumes hall_get_state to decide
    // that a group has finished. The classifier below is theirs exactly; the
    // "state stopped changing" rule around it is still this firmware's own.
    for (int i = 0; i < s_groups; ++i)
        last[i] = hall_state_from_raw(hall_raw(&GROUPS[i]));

    TickType_t start = xTaskGetTickCount();
    for (;;) {
        TickType_t el = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
        uint32_t d = el >= RAMP_MS ? PWM_MAX : PWM_MAX * el / RAMP_MS;   // soft start
        int running = 0;
        for (int i = 0; i < s_groups; ++i) {
            if (done[i]) continue;
            duty(&GROUPS[i], open_dir, d);
            int mv = hall_state_from_raw(hall_raw(&GROUPS[i]));
            if (el > RAMP_MS) {
                if (abs(mv - last[i]) < 12) {
                    if (++flat[i] >= 8) {   // ~400 ms plateau = end of travel
                        duty(&GROUPS[i], open_dir, 0);
                        done[i] = 1;
                    }
                } else {
                    flat[i] = 0;
                }
            }
            last[i] = mv;
            if (!done[i]) ++running;
        }
        if (!running || el >= TRAVEL_MS_MAX) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    stop_all();
    g_live.vent_open = open_dir;
    s_moving = false;
    pv_rgb_notify();
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
    int user_held = 0, boot_held = 0, blink = 0;
    bool user_armed = false, boot_armed = false;

    for (;; vTaskDelay(pdMS_TO_TICKS(50))) {
        // Ring LED: off in AUTO, blink in MANUAL (stock behavior).
        if (g_cfg.motor_manual) {
            if (++blink >= 10) blink = 0;
            gpio_set_level(PV_PIN_BUTTON_LED, blink < 5);
        } else {
            gpio_set_level(PV_PIN_BUTTON_LED, 0);
        }

        bool user_down = gpio_get_level(PV_PIN_USER_BUTTON) == 0;
        bool boot_down = gpio_get_level(PV_PIN_BOOT_BUTTON) == 0;

        if (!user_down) user_armed = true;      // seen released: real button
        if (!boot_down) boot_armed = true;

        if (user_armed) {
            if (user_down) {
                ++user_held;
                if (user_held == 60) {          // 3 s: back to AUTO
                    ESP_LOGI(TAG, "long press: AUTO");
                    pv_motor_set_auto(true);
                }
            } else {
                if (user_held > 0 && user_held < 60) {
                    ESP_LOGI(TAG, "click: manual toggle");
                    pv_motor_manual_toggle();
                }
                user_held = 0;
            }
        }

        if (boot_armed) {
            if (boot_down) {
                if (++boot_held == 60) pv_factory_reset_and_reboot();
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
