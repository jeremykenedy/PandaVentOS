// RGB engine: two WS2812 strips over RMT, rendering the three factory modes
// (Simple / Advance-H2D / Warning Hot) with the seven factory effects.
// Semantics follow the factory manual; parameters (brightness/speed 0..100,
// color hex) follow the factory protocol. The stock transport is RMT (the
// stock binary links rmt_tx), so this uses RMT too.
#include "pv.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
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
// Stock keeps the link indicator's position in its OWN float at 0x3ffb6910,
// not in Marquee's. Sharing one would make the two interfere whenever the
// indicator comes up over a running Marquee.
static float s_link_pos;

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

    case PV_FX_OVERRIDE_RED:                 // 0x400ddeac, not an effect
        // Stock's warning override renderer. R=127 written straight to the
        // buffer at 0x400ddf29, no brightness scaling, then vTaskDelay(10)
        // at 0x400ddf4c. The delay argument is TICKS and the tick is 10 ms,
        // so the frame is 100 ms.
        for (int i = 0; i < n; ++i) px[i] = (rgb_t){127, 0, 0};
        return 100;

    case PV_FX_HOLD:                         // 0x400dcab0
        // Leave px exactly as the last frame left it.
        return 500;

    case PV_FX_FAULT_STROBE:                 // 0x400dd33c -> 0x400dd1b0
        // Not a renderer. Stock fills { brightness 100, speed 150, colour }
        // and tail calls Strobing, so the behaviour IS Strobing; only the
        // parameters are fixed. Expressed that way here so there is one
        // strobe implementation, not two that can drift.
        return render_effect(PV_FX_STROBING, color, 100, 150, reverse, px, n);

    case PV_FX_LINK_MARQUEE: {               // 0x400dd840
        // Marquee's Gaussian with no speed input and a fixed frame. The
        // cutoff, the sigma and the 0.3 step are the same literals Marquee
        // uses; only the period and the position global differ.
        for (int i = 0; i < n; ++i) {
            float dist = fabsf((float)i - s_link_pos);
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
        // 0x400dd9c9: dir (+/-1.0f at 0x400d0cd0 / 0x400d0cd4) times 0.3f,
        // then the same wrap as Marquee at 0x400dd9db.
        s_link_pos += reverse ? -0.3f : 0.3f;
        if (s_link_pos >= (float)n)      s_link_pos = 0.0f;
        else if (s_link_pos < 0.0f)      s_link_pos = (float)n - 1e-6f;
        return 50;                           // vTaskDelay(5) at 0x400dda16
    }

    case PV_FX_STATIC:                       // 0x400dcef4
        for (int i = 0; i < n; ++i) px[i] = base;
        // vTaskDelay(50) at 0x400dd001. Ticks, not ms: 50 ticks = 500 ms.
        return 500;

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
// ---------------------------------------------------------------------------
// THE INDICATOR LEVELS AHEAD OF THE CONFIGURED EFFECT
//
// Recovered 2026-08-28. The rgb task is stock's 0x400dcab8, created as
// xTaskCreatePinnedToCore(0x400dcab8, "rgb", ..., prio 15, tskNO_AFFINITY) at
// 0x400d8cc2. Its loop top is 0x400dcad6 and it tests FOUR things before it
// ever reaches the gate chain this file used to start at:
//
//   level 0  0x400dcae2  task notification, value 255 stops the task
//   level 1  0x400dcb2d  factory test mode
//   level 2  0x400dcc1c  motor fault      -> red strobe
//   level 3  0x400dcc44  printer link     -> yellow / blue marquee
//   level 4  0x400dcc84  the gate chain in resolve() below
//
// Only level 4 existed here before. The most visible consequence was at boot:
// the task's very first instruction, 0x400dcabd, arms the level 3 word to 2,
// so a stock vent shows a blue 50 ms marquee from power-on and leaves it when
// the link settles. The clone went straight to the configured effect.
// ---------------------------------------------------------------------------

// Level 1 state. 0x400dc980 is registered as the SHORT click handler for
// GPIO 0 at 0x400de965 (the LONG press, 0x400de938, is the factory reset the
// clone already implements), so these modes ship on every unit and are not
// jig-only. s_test_entered is stock's latch byte at 0x3ffb68d8: once set it
// is never cleared, so the vent stays in test mode until it is power cycled.
static int  s_test_mode;        // 0x3ffb68d4, cycles 0 -> 1 -> 2 -> 3 -> 1
static bool s_test_entered;     // 0x3ffb68d8

// Level 3 state, stock's word at 0x3ffb6900. Armed to 2 by the render task
// before its loop, then driven by 0x400d9840.
static int s_link_ind = 2;

// Test mode 2 cycles these once a second, from the table at DROM 0x3f417070.
static const rgb_t TEST_CYCLE[4] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255},
};

void pv_rgb_test_cycle(void)
{
    if (s_test_mode == 0) {
        // 0x400dc995: latch, then ask for a scan, then mode 1.
        s_test_entered = true;
        pv_wifi_scan_start();
        s_test_mode = 1;
    } else if (s_test_mode == 1) {
        s_test_mode = 2;            // 0x400dc9af
    } else if (s_test_mode == 2) {
        s_test_mode = 3;            // 0x400dc9bc
    } else {
        // 0x400dc9c8: wrapping back to 1 re-requests the scan, the same way
        // entering from 0 does.
        pv_wifi_scan_start();
        s_test_mode = 1;            // 0x400dc9ce
    }
    ESP_LOGI(TAG, "==================current_mode is %d", s_test_mode);
}

// Stock's level 3 evaluation, 0x400d986b onward.
//
// RECOVERED: the link half. The word at 0x3ffb4a7c takes exactly 2, 3, 4, 5,
// 6, 7 across every writer in the image (0x400d9632, 0x400d9584, 0x400d964e,
// 0x400d9645, 0x400d95f4, 0x400d960a, 0x400d965f, 0x400d95bc), which is the
// factory printer.state enum. 0x400d986b raises the indicator for 2 and 4..7,
// so yellow means trying to reach the printer, or failing to.
//
// NOT RECOVERED, and previously claimed closed IN ERROR: the fallback. When
// the link half is false stock consults the word at +44 of the printer block
// (0x3ffb56a4), 1 -> yellow, 2 -> blue, 3 -> normal. On 2026-08-28 this was
// reported here as a constant 3 on the strength of a scan that found no
// writers. That scan was broken: it split objdump output on tabs and took the
// last field, which is the OPERAND list, so its "is this a store" test could
// never match and it returned zero hits by construction. Re-run correctly on
// 2026-08-29 the field has THREE writers: 0x400d9854 (init, 3), 0x400d9985
// (0), and 0x400d9056, which stores its caller's argument and is reached with
// 1 from at least 0x400d9905, 0x400d9acd and 0x400d9ae6.
//
// The conditions behind those three "= 1" sites are not yet traced, so this
// function implements the link half only. That means the clone misses some
// yellow the vent shows. It does NOT affect the boot blue, which comes from
// the 2 the render task arms at 0x400dcabd.
static void link_indicator_update(void)
{
    int ps = g_live.printer_state;
    s_link_ind = (ps == 2 || (ps >= 4 && ps <= 7)) ? 1 : 0;
}

static bool resolve(int *fx, rgb_t *color, uint8_t *bright, uint8_t *speed)
{
    const pv_rgb_cfg_t *r = &g_cfg.rgb;

    // ---- Level 1: factory test mode, gate at 0x400dcb2d ----
    if (s_test_entered) {
        if (s_test_mode == 1) {
            // 0x400dc9e8. A radio self test: Static at brightness 100, blue
            // while the scan runs, then green if an AP named "test1" is in
            // range and red if it is not. With no verdict yet stock delays
            // 500 ms at 0x400dcab0 and draws nothing at all.
            *bright = 100; *speed = 0; *fx = PV_FX_STATIC;
            if (pv_wifi_test_scan_state() == 1) {
                *color = (rgb_t){0, 0, 255};
            } else if (pv_wifi_test_scan_state() == 2) {
                *color = pv_wifi_saw_test_ap() ? (rgb_t){0, 255, 0}
                                               : (rgb_t){255, 0, 0};
            } else {
                *fx = PV_FX_HOLD;
                *color = (rgb_t){0, 0, 0};
            }
            return true;
        }
        if (s_test_mode == 2) {
            // 0x400dcb4a. One colour per second, red green blue white, timed
            // off esp_timer_get_time against 999999 us at 0x400d0c88 and
            // indexed by a counter masked to two bits at 0x400dcb7a.
            static int64_t last_us;
            static uint8_t idx;
            int64_t now = esp_timer_get_time();
            if (now - last_us > 999999) { last_us = now; idx = (idx + 1) & 3; }
            *fx = PV_FX_STATIC; *bright = 100; *speed = 0;
            *color = TEST_CYCLE[idx];
            return true;
        }
        // mode 3 falls through to level 2, then straight to green, never to
        // the gate chain. 0x400dcbc9.
    }

    // ---- Level 2: motor fault, gate at 0x400dcc1c (0x400dcbcf in mode 3) ----
    if (pv_motor_fault_any()) {
        *fx = PV_FX_FAULT_STROBE;
        *color = (rgb_t){255, 0, 0};
        *bright = 100; *speed = 0;
        return true;
    }

    if (s_test_entered) {
        // Test mode 3 tail, 0x400dcbf8: green link marquee, no gates.
        *fx = PV_FX_LINK_MARQUEE;
        *color = (rgb_t){0, 255, 0};
        *bright = 100; *speed = 0;
        return true;
    }

    // ---- Level 3: printer link, gate at 0x400dcc44 ----
    // The 2 armed at task start stands until the link layer has run its first
    // evaluation, which in stock is inside the bambu init at 0x400d9840. That
    // is what puts the blue marquee on the strip from power-on.
    if (pv_bambu_started()) link_indicator_update();
    if (s_link_ind != 0) {
        *fx = PV_FX_LINK_MARQUEE;
        // 0x400dcc4d: 1 is yellow. 0x400dcc60: anything else is blue.
        *color = (s_link_ind == 1) ? (rgb_t){255, 255, 0} : (rgb_t){0, 0, 255};
        *bright = 100; *speed = 0;
        return true;
    }

    // ---- Level 4 ----
    // Gate order is stock's, 0x400dcc87 through 0x400dcd08, outermost first:
    // total_switch, then warning_overide, then follow_printer, then
    // follow_vent. warning_overide used to be evaluated LAST here, which
    // meant a printer in error with follow_printer on and the chamber light
    // off showed nothing at all where stock shows red.
    //
    // None of these gates consult a connection or bind state. Stock reads
    // report bytes directly and has no notion of "bound" at this point, so
    // the bound check that used to guard two of them is gone.

    // 1. total_switch, switch array +3, gate at 0x400dcc87.
    if (!r->light_on) return false;

    // 2. warning_overide, switch array +4, gate at 0x400dcc90, and only when
    //    the printer state byte reads ERROR (0x400dcc98 tests == 1).
    if (r->warning_sw && g_live.device_state == PV_ST_ERROR) {
        if (r->light_mode != PV_MODE_H2D) {
            // 0x400dcc9d: mode != 1 routes to 0x400ddeac, solid red 127.
            *fx = PV_FX_OVERRIDE_RED;
            *color = (rgb_t){127, 0, 0};
            *bright = 100; *speed = 0;
            return true;
        }
        // mode == 1 routes to 0x400dc59c, the ordinary H2D renderer, which
        // lands on h2d[ERROR]. Fall through to the mode switch and let it.
    } else {
        // 3. follow_printer, +1, gate at 0x400dccd0, qualified by the
        //    printer's own chamber-light byte at 0x3ffb5578+188.
        if (r->follow_printer) {
            if (!g_live.printer_light) return false;
        }
        // 4. follow_vent, +2, gate at 0x400dcd08, qualified by the vent-open
        //    byte at 0x3ffb5678+19. Only reached when follow_printer is off.
        else if (r->follow_vent) {
            if (!g_live.vent_open) return false;
        }
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

static TaskHandle_t s_render;

static void strip_blank(void)
{
    for (int s = 0; s < s_strips; ++s) {
        for (int i = 0; i < PV_LEDS_PER_STRIP; ++i)
            led_strip_set_pixel(s_strip[s], i, 0, 0, 0);
        led_strip_refresh(s_strip[s]);
    }
}

void pv_rgb_stop(void)
{
    // 0x400dcae5 tests the notification for 255. Anything else is ignored and
    // the frame proceeds normally.
    TaskHandle_t t = s_render;
    if (t) xTaskNotify(t, 255, eSetValueWithOverwrite);
}

static void render_task(void *arg)
{
    rgb_t px[PV_LEDS_PER_STRIP];
    memset(px, 0, sizeof(px));
    // 0x400dcabd, the task's first act: arm the link indicator to 2 so the
    // strip is blue from power-on until the link settles.
    s_link_ind = 2;
    for (;;) {
        // ---- Level 0: 0x400dcae2, a NON-BLOCKING poll (xTicksToWait 0) ----
        uint32_t note = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &note, 0) == pdTRUE && note == 255) {
            // 0x400dcaf0 through 0x400dcb08: brightness to 0, everything off,
            // then the task RETURNS. Stock does this so an OTA leaves the
            // strip dark and the RMT channels released.
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strip_blank();
            s_render = NULL;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "render task stopped");
            vTaskDelete(NULL);
            return;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int fx = PV_FX_STATIC; rgb_t color; uint8_t bright, speed;
        uint32_t wait_ms = 50;
        if (resolve(&fx, &color, &bright, &speed)) {
            wait_ms = render_effect(fx, color, bright, speed,
                                    g_cfg.rgb.reverse, px, PV_LEDS_PER_STRIP);
        } else {
            memset(px, 0, sizeof(px));
        }
        // PV_FX_HOLD is stock's 0x400dcab0: it returns without reaching the
        // shared refresh at 0x400dce68, so no RMT transaction is queued at
        // all and the WS2812s simply hold their latched frame. Re-pushing an
        // identical buffer would look the same but is a different instruction
        // path and costs a transfer per frame, so skip it outright.
        if (fx != PV_FX_HOLD) {
            for (int s = 0; s < s_strips; ++s) {
                for (int i = 0; i < PV_LEDS_PER_STRIP; ++i)
                    led_strip_set_pixel(s_strip[s], i, px[i].r, px[i].g, px[i].b);
                led_strip_refresh(s_strip[s]);
            }
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
    // Stock's rgb task runs at priority 15, not 4 (0x400d8cb6 passes 15 to
    // xTaskCreatePinnedToCore at 0x400d8cc2).
    if (s_strips)
        xTaskCreate(render_task, "pv_rgb", 4096, NULL, 15, &s_render);
}
