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


// Render one effect into px[n]. tick advances at FPS. speed 0..100.
// ---------------------------------------------------------------------------
// EFFECT ENGINE, recovered from the stock image rather than invented.
//
// The stock firmware dispatches on the effect id to one function per effect
// (dispatcher at 0x400dc504; the seven targets are 0x400dcef4 Static,
// 0x400dd008 Breathing, 0x400dd1b0 Strobing, 0x400dd36c Wave, 0x400dd614
// Marquee, 0x400ddb00 Color_Cycle, 0x400ddc34 Rainbow). Each takes a pointer
// to its own 5 byte record { brightness, speed, r, g, b } at stride 5, fills
// both strips, calls the shared refresh at 0x400dce68, and then does its OWN
// vTaskDelay. There is no fixed frame rate: the frame period is part of the
// effect.
//
// Facts that apply to every effect, confirmed in the disassembly:
//   * channel scaling is NOT uniform. Static, Strobing, Color_Cycle and
//     Rainbow scale in integer: mull, then muluh against 0x51EB851F, then
//     srli 5. Breathing, Wave and Marquee reference 100.0f instead and
//     never touch that magic, so those three scale in float. The exact
//     float expression is not recovered, which is why chan() below stays
//     integer on purpose. See RE-NOTES.md, Universal section: for products
//     up to 25500 one plausible float form is bit-identical to the integer
//     divide and the other differs in 10 cases, so changing chan() without
//     pinning the form risks adding a divergence, not removing one.
//   * the strip byte order is GRB, written as [g][r][b]
//   * Warning Hot does not use a configured colour: safe is hardcoded pure
//     green 00FF00 and hazard is hardcoded pure red FF0000
//
// Timing, from the delay computation at the end of each effect function
// (the ms value is multiplied by 100 then divided by 1000, which is
// pdMS_TO_TICKS at a 100 Hz tick). Every effect except Static clamps its
// period at the bottom with a movi 9 / blt / movi 10 triple, so anything at
// or below 9 ms becomes 10. Wave is the one exception and clamps at 20.
//   Static       none, nothing moves
//   Breathing    frame = max(10, 150 - speed)    ms
//   Strobing     half period = 200 - speed       ms   (50% duty)
//   Wave         frame = max(20, 100 - speed)    ms
//   Marquee      frame = max(10, 70 - 0.6*speed) ms
//   Color_Cycle  frame = max(10, 150 - speed)    ms
//   Rainbow      frame = max(10, 100 - speed)    ms
//
// Rainbow's base is 100, not 150. It was assumed to match Breathing and
// Color_Cycle and was never checked against the image until 2026-08-27.
// The literal is movi a8, 0x64 at 0x400dde85.
//
// ALL SEVEN EFFECTS BELOW ARE THE FACTORY'S MATH, read out of the stock
// image. None of it is invented. Where a constant looks arbitrary it is
// because it IS arbitrary: it is what BIQU chose, and the address it came
// from is cited beside it.
// ---------------------------------------------------------------------------

// out = colour * brightness / 100. This is exactly what Static, Strobing,
// Color_Cycle and Rainbow do. Breathing, Wave and Marquee use a float form
// that is not recovered, so read the note above before changing this.
static inline uint8_t chan(uint8_t colour, uint8_t bright100)
{
    return (uint8_t)(((uint32_t)colour * (uint32_t)bright100) / 100u);
}

// The float path, for the three effects that use it. Stock's order is exact
// and it is not the same as scaling an already-divided byte: the INTEGER
// product first, then the factor, then the divide by 100.0f, then truncate.
//
//   Breathing 0x400dd125  mull / float.s / mul.s <factor> / divide / utrunc.s
//   Wave      0x400dd435  same shape, factor 0.3f for the background
//   Marquee   0x400dd73d  same shape, factor expf(-(d*d)/4.5)
//
// Applying the factor to chan()'s output instead loses the low bits of the
// product before the factor is applied. See RE-NOTES.md, Universal section.
static inline uint8_t chan_f(uint8_t colour, uint8_t bright100, float f)
{
    return (uint8_t)(((float)((uint32_t)colour * (uint32_t)bright100) * f)
                     / 100.0f);
}

// Breathing, 0x400dd008.
//   phase += step;  step is +1.5 rising, -1.5 falling
//   if (phase >= 60) { phase = 60; step = -1.5; }
//   else if (phase <= 0) { phase = 0; step = +1.5; }
//   x = phase / 60
//   f = x*x*(3 - 2*x)                        <- smoothstep, not a sine
//   out = (colour * brightness / 100) * f
// 60/1.5 = 40 frames each way, so a full breath is 80 frames.
static float s_breath_phase = 0.0f;
static float s_breath_step  = 1.5f;

static float breath_factor(void)
{
    s_breath_phase += s_breath_step;
    if (s_breath_phase >= 60.0f)     { s_breath_phase = 60.0f; s_breath_step = -1.5f; }
    else if (s_breath_phase <= 0.0f) { s_breath_phase = 0.0f;  s_breath_step =  1.5f; }
    float x = s_breath_phase / 60.0f;
    return x * x * (3.0f - 2.0f * x);
}

// Strobing, 0x400dd1b0: full colour for one half period, dark for the next.
static bool s_strobe_on = true;

// Marquee, 0x400dd614: a travelling Gaussian, NOT a single lit pixel.
// Direction from the reverse switch. Frame period 70 - 0.6*speed ms, floor 10.
static float s_marquee_pos;

// Color_Cycle 0x400ddb00 and Rainbow 0x400ddc34 each keep a hue phase in a
// global, exactly as stock does (uint16 at 0x3ffb690c and int at 0x3ffb6908).
static uint16_t s_cycle_hue;
static int      s_rainbow_phase;
static float    s_wave_pos;         // Wave peak position, 0..n

// Stock converts with saturation = 100 and value = 100 (the call at
// 0x400dcd68 is always passed 100, 100), so this is the plain six sector
// full-brightness conversion.
static rgb_t hsv_full(uint16_t h)
{
    h %= 360;
    uint8_t seg = (uint8_t)(h / 60);
    uint8_t f = (uint8_t)(((h % 60) * 255) / 60);
    switch (seg) {
    case 0:  return (rgb_t){255, f, 0};
    case 1:  return (rgb_t){(uint8_t)(255 - f), 255, 0};
    case 2:  return (rgb_t){0, 255, f};
    case 3:  return (rgb_t){0, (uint8_t)(255 - f), 255};
    case 4:  return (rgb_t){f, 0, 255};
    default: return (rgb_t){255, 0, (uint8_t)(255 - f)};
    }
}

// Fills px and returns the number of MILLISECONDS to wait before the next
// frame, matching the stock per-effect delay.
static uint32_t render_effect(int fx, rgb_t color, uint8_t bright100,
                              uint8_t speed, bool reverse, rgb_t *px, int n)
{
    rgb_t base = { chan(color.r, bright100),
                   chan(color.g, bright100),
                   chan(color.b, bright100) };

    switch (fx) {

    case PV_FX_STATIC:                       // 0x400dcef4
        for (int i = 0; i < n; ++i) px[i] = base;
        return 100;                          // static: nothing moves

    case PV_FX_BREATHING: {                  // 0x400dd008
        // chan_f, not base * f: stock's order at 0x400dd125 is the integer
        // product colour * brightness, converted once, then the smoothstep
        // factor, then the divide by 100. Scaling the already-divided byte
        // differed in 22.84 percent of cases, the worst of the three float
        // effects.
        float f = breath_factor();
        rgb_t c = { chan_f(color.r, bright100, f),
                    chan_f(color.g, bright100, f),
                    chan_f(color.b, bright100, f) };
        for (int i = 0; i < n; ++i) px[i] = c;
        uint32_t ms = 150u - (speed > 150 ? 150 : speed);
        return ms < 10 ? 10 : ms;
    }

    case PV_FX_STROBING: {                   // 0x400dd1b0
        rgb_t c = s_strobe_on ? base : (rgb_t){0, 0, 0};
        for (int i = 0; i < n; ++i) px[i] = c;
        s_strobe_on = !s_strobe_on;
        return 200u - (speed > 200 ? 200 : speed);
    }

    case PV_FX_MARQUEE: {                    // 0x400dd614
        // Recovered 2026-08-28. A travelling Gaussian, not one lit pixel,
        // using the same circular distance metric as Wave:
        //
        //   d = fminf(|i - pos|, n - |i - pos|)   fminf at 0x400dd70b, the
        //                                         same one Wave calls
        //   if 5.0 < d  the pixel is dark         (olt.s at 0x400dd717)
        //   else        f = expf(-(d*d) / 4.5)    (4.5f, expf at 0x40171d90)
        //   out = colour * brightness * f / 100   (0x400dd73d, chan_f order)
        //
        // The position is a FLOAT global advancing +/-0.3 px per frame (0.3f
        // at 0x400d0cd8, the same literal Wave uses for its background),
        // wrapping to 0 at n and to n-1e-6 below zero (0x400dd7d0). Reverse
        // negates the step; it does not mirror the index. The old code lit a
        // single pixel and stepped a whole pixel per frame, so it ran 3.33x
        // too fast and looked nothing like stock.
        for (int i = 0; i < n; ++i) {
            float dist = fabsf((float)i - s_marquee_pos);
            float d = dist < (n - dist) ? dist : (n - dist);   // fminf
            if (d > 5.0f) {
                px[i] = (rgb_t){0, 0, 0};
                continue;
            }
            float f = expf(-(d * d) / 4.5f);
            px[i].r = chan_f(color.r, bright100, f);
            px[i].g = chan_f(color.g, bright100, f);
            px[i].b = chan_f(color.b, bright100, f);
        }
        s_marquee_pos += reverse ? -0.3f : 0.3f;
        if (s_marquee_pos >= (float)n)      s_marquee_pos = 0.0f;
        else if (s_marquee_pos < 0.0f)      s_marquee_pos = (float)n - 1e-6f;
        // stock does this in double then truncates: 0x400dd658 onward
        int ms = (int)(70.0 - (double)speed * 0.6);
        return ms < 10 ? 10 : (uint32_t)ms;
    }

    // Wave 0x400dd36c, Color_Cycle 0x400ddb00, Rainbow 0x400ddc34.
    case PV_FX_WAVE: {                       // 0x400dd36c
        // A bright peak sliding along a dim background.
        //
        //   background = colour * brightness * 0.3 / 100   (literal 0.3 at
        //                                                   0x400d0cd8)
        //   full       = colour * brightness / 100
        //   d          = circular distance from the peak, min(|i - pos|,
        //                n - |i - pos|), via the fminf call at 0x400d0ce8
        //   if d >= 6   the pixel stays at background   (half width 6.0f
        //                                                at 0x400d0ce0)
        //   else        f = (1 - d/6)^2                 quadratic falloff
        //               out = bg + (full - bg) * f
        //
        // The peak position is a float global that advances by +/-0.5 pixels
        // per frame (0.5f at 0x400d0ce4, sign from the reverse switch) and
        // wraps around 0..n. Frame period max(20, 100 - speed) ms.
        // Stock's order, 0x400dd435 onward: the integer product colour *
        // brightness, converted to float ONCE, then the 0.3 factor for the
        // background, then the divide by 100. Scaling an already-divided
        // byte by 3/10 is not the same number.
        rgb_t full = { chan_f(color.r, bright100, 1.0f),
                       chan_f(color.g, bright100, 1.0f),
                       chan_f(color.b, bright100, 1.0f) };
        rgb_t bg   = { chan_f(color.r, bright100, 0.3f),
                       chan_f(color.g, bright100, 0.3f),
                       chan_f(color.b, bright100, 0.3f) };

        for (int i = 0; i < n; ++i) {
            float dist = s_wave_pos - (float)i;
            if (dist < 0) dist = -dist;
            float d = dist < (n - dist) ? dist : (n - dist);   // fminf
            if (d > 6.0f) {                 // olt.s 6.0, d at 0x400dd51a
                px[i] = bg;
                continue;
            }
            float t = 1.0f - d / 6.0f;
            float f = t * t;
            px[i].r = (uint8_t)(bg.r + (uint8_t)((full.r - bg.r) * f));
            px[i].g = (uint8_t)(bg.g + (uint8_t)((full.g - bg.g) * f));
            px[i].b = (uint8_t)(bg.b + (uint8_t)((full.b - bg.b) * f));
        }

        s_wave_pos += reverse ? -0.5f : 0.5f;
        if (s_wave_pos >= (float)n) s_wave_pos -= (float)n;
        else if (s_wave_pos < 0.0f) s_wave_pos += (float)n;

        uint32_t ms = 100u - (speed > 100 ? 100 : speed);
        return ms < 20 ? 20 : ms;
    }

    case PV_FX_COLOR_CYCLE: {                // 0x400ddb00
        // hue lives in a uint16 global, HSV at full saturation and value,
        // uniform across the whole strip. hue += 2 each frame, and wraps to
        // 0 once it passes 359 (0x167). Frame period 150 - speed ms.
        rgb_t c = hsv_full(s_cycle_hue);
        rgb_t o = { chan(c.r, bright100), chan(c.g, bright100),
                    chan(c.b, bright100) };
        for (int i = 0; i < n; ++i) px[i] = o;
        s_cycle_hue += 2;
        if (s_cycle_hue > 359) s_cycle_hue = 0;
        uint32_t ms = 150u - (speed > 150 ? 150 : speed);
        return ms < 10 ? 10 : ms;
    }

    case PV_FX_RAINBOW:                      // 0x400ddc34
    default: {
        // hue = (i * 360 / n + phase) mod 360, so a FULL spectrum is spread
        // across the strip, scrolling by phase. HSV at full saturation and
        // value. phase advances by +/-5 per frame, sign taken from the
        // reverse switch (sext a4, a4, 7 then addx4 a4, a4, a4 at
        // 0x400dde44, so the step is +5 or -5 and never 0: both directions
        // do scroll).
        //
        // The detail worth keeping: stock does not light every pixel at the
        // configured brightness. It subtracts ((i + 2) mod 7) from it, with
        // an unsigned-underflow guard that clamps to 0. That is a 7 pixel
        // sawtooth shimmer laid over the spectrum, and it is why the stock
        // rainbow has visible texture rather than a flat wash.
        for (int i = 0; i < n; ++i) {
            int hue = ((i * 360) / n) + s_rainbow_phase;
            hue %= 360;
            if (hue < 0) hue += 360;
            rgb_t c = hsv_full((uint16_t)hue);

            uint8_t dip = (uint8_t)((i + 2) % 7);
            uint8_t b = (uint8_t)(bright100 - dip);
            if (bright100 < b) b = 0;            // stock's underflow guard

            px[i].r = chan(c.r, b);
            px[i].g = chan(c.g, b);
            px[i].b = chan(c.b, b);
        }
        // Sign confirmed at 0x400ddc83: stock normalises the flag to +1 when
        // set and 255 (= -1 after sext a4,a4,7) when clear, then multiplies
        // by 5 with addx4. So reverse OFF DECREMENTS the phase. Since
        // hue(i) = i*360/n + phase, a decreasing phase moves the pattern
        // toward increasing i, the same way Wave and Marquee travel with
        // reverse off. This was inverted until 2026-08-28.
        s_rainbow_phase += reverse ? +5 : -5;
        if (s_rainbow_phase >= 360)  s_rainbow_phase -= 360;
        if (s_rainbow_phase <= -360) s_rainbow_phase += 360;
        // Base 100, not 150. movi a8, 0x64 at 0x400dde85, floored at 10.
        uint32_t ms = 100u - (speed > 100 ? 100 : speed);
        return ms < 10 ? 10 : ms;
    }
    }
}

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
        // 0x400dc5d7 onward, exactly:
        //     l32i a8, a8, 180      bed_temper
        //     movi.n a9, 50
        //     blt  a9, a8, HOT      50 < bed     -> hot
        //     l32i a8, a8, 176      nozzle_temper
        //     bge  a9, a8, SAFE     50 >= nozzle -> safe, else hot
        // Strict >, on either temperature, on the truncated integer, and NO
        // hysteresis. This carried >= and 2 C of hysteresis until 2026-08-28,
        // which read hot across 50.0..50.9 where stock reads safe, and held
        // hot down to 48 on the way back.
        bool hot = (PV_WARN_HOT_C < g_live.bed_temp)
                || (PV_WARN_HOT_C < g_live.nozzle_temp);

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
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int fx; rgb_t color; uint8_t bright, speed;
        uint32_t wait_ms = 50;
        if (resolve(&fx, &color, &bright, &speed)) {
            wait_ms = render_effect(fx, color, bright, speed,
                                    g_cfg.rgb.reverse, px, PV_LEDS_PER_STRIP);
        } else {
            memset(px, 0, sizeof(px));
        }
        for (int s = 0; s < s_strips; ++s) {
            for (int i = 0; i < PV_LEDS_PER_STRIP; ++i)
                led_strip_set_pixel(s_strip[s], i, px[i].r, px[i].g, px[i].b);
            led_strip_refresh(s_strip[s]);
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));   // stock: the effect owns its frame period
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
