// RGB engine: two WS2812 strips over RMT, rendering the three factory modes
// (Simple / Advance-H2D / Warning Hot) with the seven factory effects.
// Semantics follow the factory manual; parameters (brightness/speed 0..100,
// color hex) follow the factory protocol. The stock transport is RMT (the
// stock binary links rmt_tx), so this uses RMT too.
#include "pv.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "pv_rgb";

#define FPS 30

static led_strip_handle_t s_strip[PV_STRIP_COUNT_MAX];
static int s_strips;
static SemaphoreHandle_t s_lock;

typedef struct { uint8_t r, g, b; } rgb_t;

static rgb_t hex_to_rgb(const char *hex)
{
    unsigned v = 0;
    sscanf(hex, "%6x", &v);
    return (rgb_t){ (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF };
}

static uint8_t scale(uint8_t c, uint32_t num, uint32_t den)
{
    return (uint8_t)((uint32_t)c * num / den);
}

static rgb_t hue_rgb(uint16_t h)   // h 0..359
{
    uint8_t seg = h / 60, rem = (h % 60) * 255 / 60;
    switch (seg) {
    case 0: return (rgb_t){255, rem, 0};
    case 1: return (rgb_t){255 - rem, 255, 0};
    case 2: return (rgb_t){0, 255, rem};
    case 3: return (rgb_t){0, 255 - rem, 255};
    case 4: return (rgb_t){rem, 0, 255};
    default: return (rgb_t){255, 0, 255 - rem};
    }
}

// Render one effect into px[n]. tick advances at FPS. speed 0..100.
static void render_effect(int fx, rgb_t color, uint8_t bright100, uint8_t speed,
                          uint32_t tick, bool reverse, rgb_t *px, int n)
{
    uint32_t phase = tick * (1 + speed / 10);   // effect clock
    for (int i = 0; i < n; ++i) {
        int pos = reverse ? (n - 1 - i) : i;
        rgb_t c = color;
        uint32_t lvl = 255;
        switch (fx) {
        case PV_FX_STATIC:
            break;
        case PV_FX_BREATHING: {
            uint32_t p = phase % 256;
            lvl = p < 128 ? 40 + p * 215 / 127 : 40 + (255 - p) * 215 / 127;
            break;
        }
        case PV_FX_STROBING:
            lvl = ((phase / 8) & 1) ? 255 : 0;
            break;
        case PV_FX_WAVE: {
            uint32_t x = (phase * 4 + pos * 512 / n) % 512;
            lvl = x < 256 ? x : 511 - x;
            break;
        }
        case PV_FX_MARQUEE:
            lvl = (((pos + phase / 4) % 3) == 0) ? 255 : 0;
            break;
        case PV_FX_COLOR_CYCLE:
            c = hue_rgb((phase * 2) % 360);
            break;
        case PV_FX_RAINBOW:
            c = hue_rgb((uint16_t)((phase * 2 + pos * 360 / n) % 360));
            break;
        default:
            break;
        }
        px[i].r = scale(scale(c.r, lvl, 255), bright100, 100);
        px[i].g = scale(scale(c.g, lvl, 255), bright100, 100);
        px[i].b = scale(scale(c.b, lvl, 255), bright100, 100);
    }
}

// Decide what to render this frame from config + live state.
// Decide what to render this frame. The priority order below is not
// invented: every rule is a sentence from the factory app's own help text.
//
//   Light Switch      "Click Switch to turn on and off the RGB Light effects.
//                      Note: Overrides all other light settings if 'off'."
//   Follow Printer    "Automatically turns RGB effect ON and OFF following
//                      the printers stock light."
//   Follow Vent       "Set to turn on RGB lights when vent is open and off
//                      when vent is closed. Lower priority than Follow
//                      Printer."
//   Warning OverRide  "Click switch to override Red flashing warning light
//                      when printer is in error state."   <-- ERROR STATE,
//                      not temperature. Warning Hot Mode is the separate
//                      temperature feature.
//   Warning Hot Mode  safe below 50 C, hazard above.
//
// Returns false when the strips should be dark.
static bool resolve(int *fx, rgb_t *color, uint8_t *bright, uint8_t *speed)
{
    const pv_rgb_cfg_t *r = &g_cfg.rgb;

    // 1. Master switch overrides everything else.
    if (!r->light_on) return false;

    // 2. Follow Printer Light: gate on the printer's own chamber light.
    //    Only meaningful while actually bound to a printer.
    bool bound = g_live.printer_state == 3;
    if (r->follow_printer && bound) {
        if (!g_live.printer_light) return false;
    }
    // 3. Follow Vent, explicitly lower priority than Follow Printer.
    else if (r->follow_vent) {
        if (!g_live.vent_open) return false;
    }

    // 4. Warning override: printer in error state forces red flashing,
    //    whatever mode is selected.
    if (r->warning_sw && bound && g_live.device_state == PV_ST_ERROR) {
        *fx = PV_FX_STROBING;
        *color = (rgb_t){255, 0, 0};
        *bright = r->h2d[PV_ST_ERROR][PV_FX_STROBING].brightness;
        *speed = r->h2d[PV_ST_ERROR][PV_FX_STROBING].speed;
        return true;
    }

    // 5. The selected light mode.
    switch (r->light_mode) {
    case PV_MODE_H2D: {
        int st = g_live.device_state;
        if (st < 0 || st >= PV_ST_COUNT) st = PV_ST_IDLE;
        int e = r->h2d_active[st];
        if (e < 0 || e >= PV_FX_COUNT) e = PV_FX_STATIC;
        const pv_fx_param_t *p = &r->h2d[st][e];
        *fx = e; *color = hex_to_rgb(p->color);
        *bright = p->brightness; *speed = p->speed;
        return true;
    }
    case PV_MODE_WARNING: {
        // Factory boundary is 50 C on the printer's MAXIMUM temperature:
        // "The maximum temperature of the printer is below 50 C, with no risk
        // of burns" / "...above 50 C, which poses a risk of burns!".
        // 2 C of hysteresis so a printer sitting on the line does not flicker.
        float tmax = g_live.bed_temp > g_live.nozzle_temp
                   ? g_live.bed_temp : g_live.nozzle_temp;
        static bool hot;
        if (tmax >= PV_WARN_HOT_C)            hot = true;
        else if (tmax < PV_WARN_HOT_C - 2.0f) hot = false;

        int lvl = hot ? 1 : 0;
        // Each level offers Static or Strobing only.
        int sel = r->warnhot_current[lvl] ? PV_FX_STROBING : PV_FX_STATIC;
        *fx = sel;
        *color = hot ? (rgb_t){255, 0, 0} : (rgb_t){0, 255, 0};
        *bright = r->warnhot_bg[lvl][r->warnhot_current[lvl]];
        *speed = r->warnhot_speed[lvl][r->warnhot_current[lvl]];
        return true;
    }
    default: {   // PV_MODE_SIMPLE
        int e = r->simple_current;
        if (e < 0 || e >= PV_FX_COUNT) e = PV_FX_STATIC;
        const pv_fx_param_t *p = &r->simple[e];
        *fx = e; *color = hex_to_rgb(p->color);
        *bright = p->brightness; *speed = p->speed;
        return true;
    }
    }
}

static void render_task(void *arg)
{
    rgb_t px[PV_LEDS_PER_STRIP];
    for (uint32_t tick = 0;; ++tick) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int fx; rgb_t color; uint8_t bright, speed;
        if (resolve(&fx, &color, &bright, &speed)) {
            render_effect(fx, color, bright, speed, tick, g_cfg.rgb.reverse,
                          px, PV_LEDS_PER_STRIP);
        } else {
            memset(px, 0, sizeof(px));
        }
        for (int s = 0; s < s_strips; ++s) {
            for (int i = 0; i < PV_LEDS_PER_STRIP; ++i)
                led_strip_set_pixel(s_strip[s], i, px[i].r, px[i].g, px[i].b);
            led_strip_refresh(s_strip[s]);
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(1000 / FPS));
    }
}

void pv_rgb_notify(void)
{
    // Rendering reads config each frame; nothing to do beyond existing.
}

void pv_rgb_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    static const int pins[PV_STRIP_COUNT_MAX] = { PV_PIN_STRIP0, PV_PIN_STRIP1 };
    for (int i = 0; i < PV_STRIP_COUNT_MAX; ++i) {
        led_strip_config_t sc = {
            .strip_gpio_num = pins[i],
            .max_leds = PV_LEDS_PER_STRIP,
            .led_pixel_format = LED_PIXEL_FORMAT_GRB,
            .led_model = LED_MODEL_WS2812,
        };
        led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
        if (led_strip_new_rmt_device(&sc, &rc, &s_strip[i]) == ESP_OK) {
            ++s_strips;
        } else {
            ESP_LOGW(TAG, "strip %d init failed", i);
            break;
        }
    }
    ESP_LOGI(TAG, "%d strip(s) up", s_strips);
    if (s_strips)
        xTaskCreate(render_task, "pv_rgb", 4096, NULL, 4, NULL);
}
