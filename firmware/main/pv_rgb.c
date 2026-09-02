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
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static const char *TAG = "pv_rgb";

#define FPS 30

typedef struct { uint8_t r, g, b; } rgb_t;

// ---------------------------------------------------------------------------
// OUTPUT LAYER, rewritten 2026-08-29 to match stock exactly.
//
// The managed led_strip component is gone. Stock does NOT use it: its own
// error strings name ./main/rgb/app_rgb.c calling rmt_new_tx_channel,
// rmt_new_led_strip_encoder and rmt_enable directly. That was the one layer of
// this firmware never cloned, and it is the layer that produced ESP_OK on
// every transmit with no light on the wire.
//
// Every constant below is read out of the shipping image, not from an example.
//
//   channel config, built on the stack in rgb_init at 0x400dc46f:
//       gpio_num          per-strip table at DRAM 0x3ffb031c field +16 = 14, 4
//       clk_src           4 = SOC_MOD_CLK_APB = RMT_CLK_SRC_DEFAULT
//       resolution_hz     0x400d0c30 = 10000000
//       mem_block_symbols 64
//       trans_queue_depth 4
//       intr_priority     0, flags 0 (so invert_out is NOT set)
//
//   bit timings, doubles split across the literal pool and recombined from
//   the (low, high) pairs the code actually loads into a12/a13:
//       T_short = (0x400d0cec, 0x400d0d58) = 0.3 us -> 3 ticks at 10 MHz
//       T_long  = (0x400d03fc, 0x400d0d60) = 0.9 us -> 9 ticks
//       divisor = (0x400d0cc4, 0x400d0d5c) = 1e6, i.e. ticks = res * T / 1e6
//   giving, at 0x400de105 / 0x400de143 / 0x400de181 / 0x400de1bc:
//       bit0 = { 3 ticks high, 9 ticks low }
//       bit1 = { 9 ticks high, 3 ticks low }
//
//   reset code, computed at 0x400de254 as (resolution / 1000000) * 25 per
//   half, both halves low: 250 + 250 ticks = 50 us at 10 MHz.
//
// An earlier note in this file said 0.6 us. That was wrong: it paired
// 0x400d0cec with the following pool word instead of the high half the code
// actually loads. The correct value is 0.3.
#define RMT_RES_HZ        10000000u
#define WS_T_SHORT_TICKS  3       // 0.3 us
#define WS_T_LONG_TICKS   9       // 0.9 us
#define WS_RESET_TICKS    250     // per half, both halves low -> 50 us total

typedef struct {
    rmt_encoder_t      base;
    rmt_encoder_t     *bytes;
    rmt_encoder_t     *copy;
    rmt_symbol_word_t  reset_code;
    int                state;
} ws_encoder_t;

static size_t ws_encode(rmt_encoder_t *enc, rmt_channel_handle_t chan,
                        const void *data, size_t len, rmt_encode_state_t *ret)
{
    ws_encoder_t *e = __containerof(enc, ws_encoder_t, base);
    rmt_encode_state_t sess = RMT_ENCODING_RESET;
    rmt_encode_state_t out  = RMT_ENCODING_RESET;
    size_t n = 0;

    if (e->state == 0) {
        n += e->bytes->encode(e->bytes, chan, data, len, &sess);
        if (sess & RMT_ENCODING_COMPLETE) e->state = 1;
        if (sess & RMT_ENCODING_MEM_FULL) {
            out |= RMT_ENCODING_MEM_FULL;
            *ret = out;
            return n;
        }
    }
    if (e->state == 1) {
        n += e->copy->encode(e->copy, chan, &e->reset_code,
                             sizeof(e->reset_code), &sess);
        if (sess & RMT_ENCODING_COMPLETE) {
            e->state = 0;
            out |= RMT_ENCODING_COMPLETE;
        }
        if (sess & RMT_ENCODING_MEM_FULL) out |= RMT_ENCODING_MEM_FULL;
    }
    *ret = out;
    return n;
}

static esp_err_t ws_del(rmt_encoder_t *enc)
{
    ws_encoder_t *e = __containerof(enc, ws_encoder_t, base);
    rmt_del_encoder(e->bytes);
    rmt_del_encoder(e->copy);
    free(e);
    return ESP_OK;
}

static esp_err_t ws_reset(rmt_encoder_t *enc)
{
    ws_encoder_t *e = __containerof(enc, ws_encoder_t, base);
    rmt_encoder_reset(e->bytes);
    rmt_encoder_reset(e->copy);
    e->state = 0;
    return ESP_OK;
}

static esp_err_t ws_encoder_new(rmt_encoder_handle_t *out)
{
    ws_encoder_t *e = calloc(1, sizeof(ws_encoder_t));
    if (!e) return ESP_ERR_NO_MEM;
    e->base.encode = ws_encode;
    e->base.del    = ws_del;
    e->base.reset  = ws_reset;

    rmt_bytes_encoder_config_t bc = {
        .bit0 = { .level0 = 1, .duration0 = WS_T_SHORT_TICKS,
                  .level1 = 0, .duration1 = WS_T_LONG_TICKS  },
        .bit1 = { .level0 = 1, .duration0 = WS_T_LONG_TICKS,
                  .level1 = 0, .duration1 = WS_T_SHORT_TICKS },
        .flags.msb_first = 1,          // 0x400de1f2 sets the msb_first bit
    };
    esp_err_t err = rmt_new_bytes_encoder(&bc, &e->bytes);   // 0x400f2834
    if (err != ESP_OK) { free(e); return err; }
    rmt_copy_encoder_config_t cc = {};
    err = rmt_new_copy_encoder(&cc, &e->copy);
    if (err != ESP_OK) { rmt_del_encoder(e->bytes); free(e); return err; }

    e->reset_code = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = WS_RESET_TICKS,
        .level1 = 0, .duration1 = WS_RESET_TICKS,
    };
    *out = &e->base;
    return ESP_OK;
}

static rmt_channel_handle_t s_chan[PV_STRIP_COUNT_MAX];
static rmt_encoder_handle_t s_enc[PV_STRIP_COUNT_MAX];
// GRB on the wire, which is how stock lays its buffer out ([g][r][b]).
static uint8_t s_buf[PV_STRIP_COUNT_MAX][PV_LEDS_PER_STRIP * 3];
static int s_strips;
static SemaphoreHandle_t s_lock;

static esp_err_t strip_push(int i, const rgb_t *px, int n)
{
    for (int k = 0; k < n; ++k) {
        s_buf[i][3 * k + 0] = px[k].g;
        s_buf[i][3 * k + 1] = px[k].r;
        s_buf[i][3 * k + 2] = px[k].b;
    }
    rmt_transmit_config_t tc = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_chan[i], s_enc[i], s_buf[i], (size_t)n * 3, &tc);
    if (err != ESP_OK) return err;
    // 500 ms, not 100.
    //
    // Sixteen WS2812 pixels is 384 bits at 1.25 us: the transfer itself takes
    // under half a millisecond, so this wait normally returns at once and the
    // number here only decides how long to tolerate the RMT peripheral being
    // starved. It gets starved: serving the 340 KB page reads it out of flash,
    // and every flash read disables the instruction cache, which delays any
    // ISR not resident in IRAM. At 100 ms the device logged an rmt flush
    // timeout and counted a dropped frame each time somebody loaded the page.
    //
    // A frame arriving 200 ms late is invisible. A frame skipped is a visible
    // stutter, and a counter that ticks up on every page load makes the one
    // statistic that would reveal a genuinely dead strip useless. The only
    // thing this guards against is the peripheral hanging outright, and half a
    // second is still three orders of magnitude longer than the transfer.
    return rmt_tx_wait_all_done(s_chan[i], pdMS_TO_TICKS(500));
}


static rgb_t hex_to_rgb(const char *hex)
{
    unsigned v = 0;
    sscanf(hex, "%6x", &v);
    return (rgb_t){ (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF };
}


// Render one effect into px[n]. speed 0..100.
// ---------------------------------------------------------------------------
// EFFECT ENGINE.
//
// One case per effect. Each fills px[0..n-1] for this instant, advances its
// own animation phase once per call, and returns the number of MILLISECONDS
// to wait before the next frame. The frame period is not fixed: it is the
// effect's, so a fast effect and a slow one can share the one render task.
//
// The seven stock effects (ids 0..6) are a clean-room reimplementation written
// to the behavioural specification in private/SPEC/effects-math.md and pinned
// by tools/fxdump/framecheck.c; the fifteen that follow are original. Every
// effect reaches the INACTIVE colour through mix3()/s_fx_bg, so the single
// rule "unlit -> inactive colour, else black" holds everywhere without any
// effect special-casing it.
//
// Channel scaling is integer: out = colour * brightness / 100, in chan(). The
// effects that ease within a frame use chan_f(), which keeps the integer
// colour*brightness product intact and applies the float factor before the
// divide, so an effect at full brightness with no easing is byte-exact.
//
// Every effect takes its animation rate from one shared speed curve,
// fx_period(), so a given speed setting means the same liveliness on all of
// them.
// ---------------------------------------------------------------------------

// out = colour * brightness / 100, integer. Used directly by the effects that
// do not ease within a frame.
static inline uint8_t chan(uint8_t colour, uint8_t bright100)
{
    return (uint8_t)(((uint32_t)colour * (uint32_t)bright100) / 100u);
}

// The float path, for effects that ease within a frame. The order matters and
// is not the same as scaling an already-divided byte: the INTEGER product
// colour*brightness first, then the float factor, then the divide by 100.
// Applying the factor to chan()'s output instead would lose the low bits of
// the product before the factor is applied.
static inline uint8_t chan_f(uint8_t colour, uint8_t bright100, float f)
{
    return (uint8_t)(((float)((uint32_t)colour * (uint32_t)bright100) * f)
                     / 100.0f);
}

// NOT STOCK. The INACTIVE colour.
//
// Every effect used to paint black where it was not lit. Now it paints this,
// and black is simply the value it holds when no inactive colour is set. That
// is why one formula gives every one of the eighteen effects the feature.
//
// The active term is chan_f UNCHANGED, so with an unset (black) inactive
// colour the output is bit-identical to stock: the second term is zero and the
// first is the exact expression stock uses, in stock's order. See the note
// above chan_f about why that order matters.
//
//   out = chan_f(active, bright, f) + chan_f(inactive, bright, 1 - f)
//
// The two factors sum to one, so the result cannot exceed a full-brightness
// channel and needs no clamp.
static rgb_t s_fx_bg;            // set once per frame by render_effect

// The print percentage the effects should use. Defined further down, next to
// the preview state it consults; declared here because the progress renderer
// above needs it.
static int fx_percent(void);

static inline rgb_t mix3(rgb_t active, uint8_t bright100, float f)
{
    rgb_t o;
    o.r = (uint8_t)(chan_f(active.r, bright100, f) + chan_f(s_fx_bg.r, bright100, 1.0f - f));
    o.g = (uint8_t)(chan_f(active.g, bright100, f) + chan_f(s_fx_bg.g, bright100, 1.0f - f));
    o.b = (uint8_t)(chan_f(active.b, bright100, f) + chan_f(s_fx_bg.b, bright100, 1.0f - f));
    return o;
}

// Breathing keeps a triangle phase in [0,1] and a signed per-frame step; the
// case eases it into a smooth swell. s_breath_step == 0 means "not yet armed",
// so framecheck can zero both and have the effect start from the bottom.
static float s_breath_phase;
static float s_breath_step;

// Strobing: the whole strip on for one half of the cycle, off the other.
static bool s_strobe_on = true;

// Marquee: a single lit block that walks the run and wraps.
static float s_marquee_pos;
// The link indicator keeps its own position float, separate from Marquee's, so
// the two do not interfere when the indicator comes up over a running Marquee.
static float s_link_pos;

// ADDITIONS, not stock. Cylon and Bounce both travel end to end and turn
// around, so each needs a direction alongside its position. Stock's effects
// all wrap instead, which is why none of them carry one.
static float s_bounce_pos;
static float s_bounce_dir = 1.0f;
static float s_cylon_pos;
static float s_cylon_dir  = 1.0f;

// The centre-referenced family, added 2026-08-30. Each pair (out/in) shares
// one phase global, because only one of a pair can be on screen at a time and
// sharing means switching between them in the UI does not jump.
//
// Every one of them works in HALF-STRIP coordinates: h = n/2 rounded up, and
// the position runs 0..h-1 from the centre outward. The inward variants are
// the same number read from the other end. Doing it this way rather than with
// two independent counters is what guarantees the two halves stay symmetric
// on an odd pixel count, which 16 is not but a future strip might be.
static float s_split_pos;          // Center marquee pair, 0..h-1
static float s_fill_pos;           // Center fill pair, 0..h
static float s_sbounce_pos;        // Center bounce pair
static float s_sbounce_dir = 1.0f;
static float s_sfill_pos;          // Center bounce-fill pair
static float s_sfill_dir  = 1.0f;
static float s_progress_shown;     // Progress bar, eased toward the real value
// NOT STOCK. The two animated progress effects.
static int   s_chase_pos;          // the sweeping head, in pixels
static int   s_anim_breath;        // frames, for the breathing tip
static int   s_barber_pos;         // the pole's offset, in pixels
// NOT STOCK. Which row of the uploaded animation is showing. A phase
// like every other one here, so it advances once per FRAME rather than
// once per strip, and both runs draw the same row.
static int   s_anim_frame;
// The band width the pole is currently drawing at. Published the same way
// s_fx_bg is: resolve() knows the effect's parameters, render_effect does not.
static int   s_fx_band = 3;

// Color_Cycle and Rainbow each keep a hue phase in a global.
// NOT STOCK. Rendering the SAME frame for two strips of different lengths.
//
// The strips can have different LED counts, so each needs its own render pass
// with its own n. Every effect advances its phase as a side effect of
// rendering, so a naive second pass would step the animation twice per frame
// and run it at double speed. These two functions snapshot the phase before
// the first strip and restore it before each subsequent one, so every strip
// sees the same instant and the phase advances once per frame.
typedef struct {
    float breath_phase, breath_step;
    bool  strobe_on;
    float marquee_pos, link_pos;
    float bounce_pos, bounce_dir, cylon_pos, cylon_dir;
    float split_pos, fill_pos;
    float sbounce_pos, sbounce_dir, sfill_pos, sfill_dir;
    float progress_shown, wave_pos;
    int   chase_pos, anim_breath, barber_pos;
    int   anim_frame;
    int   ramp_step;
    float cycle_hue;
    float rainbow_phase;
} pv_fx_phase_t;

static float    s_cycle_hue;
static float    s_rainbow_phase;
static float    s_wave_pos;         // Wave peak position, 0..n

// NOT STOCK. The optional brightness ramp.
//
// An effect can hold a second brightness. When it does, the brightness sweeps
// from the first to the second across a cycle and starts over, instead of
// sitting at one value. The sweep is one sawtooth, 0 -> 1, advanced once per
// rendered frame by 1/PV_RAMP_STEPS; the frame period is whatever the effect
// asked for, so the sweep speeds up and slows down with the speed slider along
// with everything else on the strip. Static has no cycle of its own and gets
// the same sweep, which is the point: it is how you get a slow fade out of an
// effect that does not move.
#define PV_RAMP_STEPS 100
static int s_ramp_step;          // 0 .. PV_RAMP_STEPS-1, an exact position

// One step of the ramp, and the brightness to render this frame with.
// bright_end < 0 means no ramp: the brightness is returned untouched and the
// sweep is parked at its start, so switching a ramp on begins at `bright`
// rather than wherever a previous effect happened to leave it.
//
// A function rather than inline code in the frame loop so tools/fxdump can
// drive the SHIPPING implementation instead of a copy of it.
static uint8_t ramp_apply(uint8_t bright, int bright_end)
{
    if (bright_end < 0) { s_ramp_step = 0; return bright; }
    // An integer step, not an accumulated float. Adding 0.01f a hundred times
    // lands at 0.99999997, so the sweep missed its wrap by a frame and the
    // error grew with every cycle.
    float f = (float)s_ramp_step / (float)PV_RAMP_STEPS;
    float b = (float)bright + ((float)bright_end - (float)bright) * f;
    if (b < 0.0f) b = 0.0f; else if (b > 100.0f) b = 100.0f;
    if (++s_ramp_step >= PV_RAMP_STEPS) s_ramp_step = 0;
    return (uint8_t)(b + 0.5f);
}

void pv_rgb_anim_rewind(void) { s_anim_frame = 0; }

static void fx_phase_save(pv_fx_phase_t *o)
{
    o->breath_phase = s_breath_phase; o->breath_step = s_breath_step;
    o->strobe_on = s_strobe_on;
    o->marquee_pos = s_marquee_pos;   o->link_pos = s_link_pos;
    o->bounce_pos = s_bounce_pos;     o->bounce_dir = s_bounce_dir;
    o->cylon_pos = s_cylon_pos;       o->cylon_dir = s_cylon_dir;
    o->split_pos = s_split_pos;       o->fill_pos = s_fill_pos;
    o->sbounce_pos = s_sbounce_pos;   o->sbounce_dir = s_sbounce_dir;
    o->sfill_pos = s_sfill_pos;       o->sfill_dir = s_sfill_dir;
    o->progress_shown = s_progress_shown; o->wave_pos = s_wave_pos;
    o->chase_pos = s_chase_pos; o->anim_breath = s_anim_breath;
    o->barber_pos = s_barber_pos;   o->anim_frame = s_anim_frame;
    o->ramp_step = s_ramp_step;
    o->cycle_hue = s_cycle_hue;       o->rainbow_phase = s_rainbow_phase;
}

static void fx_phase_restore(const pv_fx_phase_t *o)
{
    s_breath_phase = o->breath_phase; s_breath_step = o->breath_step;
    s_strobe_on = o->strobe_on;
    s_marquee_pos = o->marquee_pos;   s_link_pos = o->link_pos;
    s_bounce_pos = o->bounce_pos;     s_bounce_dir = o->bounce_dir;
    s_cylon_pos = o->cylon_pos;       s_cylon_dir = o->cylon_dir;
    s_split_pos = o->split_pos;       s_fill_pos = o->fill_pos;
    s_sbounce_pos = o->sbounce_pos;   s_sbounce_dir = o->sbounce_dir;
    s_sfill_pos = o->sfill_pos;       s_sfill_dir = o->sfill_dir;
    s_progress_shown = o->progress_shown; s_wave_pos = o->wave_pos;
    s_chase_pos = o->chase_pos; s_anim_breath = o->anim_breath;
    s_barber_pos = o->barber_pos;   s_anim_frame = o->anim_frame;
    s_ramp_step = o->ramp_step;
    s_cycle_hue = o->cycle_hue;       s_rainbow_phase = o->rainbow_phase;
}

// Hue -> RGB at full saturation and value, the ordinary textbook conversion.
// The colour-cycle and rainbow effects sweep the hue wheel and always want a
// fully saturated, full-brightness colour, so this is the special case S=V=1
// of the standard six-sector HSV->RGB: the hue's 360 degrees split into six
// 60-degree sectors, one channel rising and one falling linearly across each.
static rgb_t hsv_full(uint16_t h)
{
    h %= 360;
    uint8_t sector = (uint8_t)(h / 60);
    uint8_t rise = (uint8_t)(((h % 60) * 255) / 60);   // 0..255 across a sector
    uint8_t fall = (uint8_t)(255 - rise);
    switch (sector) {
    case 0:  return (rgb_t){255, rise, 0};
    case 1:  return (rgb_t){fall, 255, 0};
    case 2:  return (rgb_t){0, 255, rise};
    case 3:  return (rgb_t){0, fall, 255};
    case 4:  return (rgb_t){rise, 0, 255};
    default: return (rgb_t){255, 0, fall};
    }
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// One speed curve for every effect. speed 0..100 maps to a frame interval,
// geometrically: each equal step in speed multiplies the frame RATE by a
// constant factor, because liveliness reads as a ratio and not a difference,
// so 0->5 feels like the same change as 95->100. The fast end is floored at
// 16 ms (~60 Hz) so the quickest setting stays smooth rather than strobing,
// and the slow end is 500 ms so speed 0 is a slow pulse rather than a stall.
static uint32_t fx_period(uint8_t speed)
{
    if (speed > 100) speed = 100;
    const float fast_ms = 16.0f;     // speed 100
    const float slow_ms = 500.0f;    // speed 0
    float t  = (float)(100 - speed) / 100.0f;
    float ms = fast_ms * powf(slow_ms / fast_ms, t);
    return (uint32_t)(ms + 0.5f);
}

// Open curve choices for the animated stock effects. framecheck pins none of
// these numbers; they are shape and rate, free to retune.
#define BREATH_FLOOR   0.12f    // dimmest point of the breath (never black)
#define BREATH_DELTA   0.02f    // triangle step: 50 frames bottom -> top
#define WAVE_CYCLES    2.0f     // sine humps across the whole run
#define WAVE_DELTA     0.05f    // wavelengths advanced per frame
#define MARQUEE_WFRAC  4        // lit block width = n / MARQUEE_WFRAC (>= 1)
#define MARQUEE_DELTA  0.5f     // pixels the block advances per frame
#define CYCLE_DELTA    3.0f     // degrees of hue advanced per frame
#define RAINBOW_DELTA  3.0f     // degrees the rainbow scrolls per frame

// Fills px and returns the number of MILLISECONDS to wait before the next
// frame, which is fx_period(speed) for every effect.
static uint32_t render_effect(int fx, rgb_t color, rgb_t bg, uint8_t bright100,
                              uint8_t speed, bool reverse, rgb_t *px, int n)
{
    // Published for mix3, which every effect goes through: this is the colour a
    // pixel falls to when it is not lit.
    s_fx_bg = bg;

    switch (fx) {

    case PV_FX_OVERRIDE_RED:                 // the warning override, not an effect
        // Solid R=127 written straight to the buffer with no brightness
        // scaling, at a fixed 100 ms frame.
        for (int i = 0; i < n; ++i) px[i] = (rgb_t){127, 0, 0};
        return 100;

    case PV_FX_HOLD:
        // Leave px exactly as the last frame left it.
        return 500;

    case PV_FX_FAULT_STROBE:
        // The motor-fault indicator: Strobing at fixed full brightness, so it
        // reuses the one strobe implementation for its pixels. Its rate is not
        // the speed curve's to set, so it holds a steady ~10 Hz.
        render_effect(PV_FX_STROBING, color, bg, 100, 150, reverse, px, n);
        return 50;

    case PV_FX_LINK_MARQUEE: {               // the printer-link indicator
        // A travelling Gaussian blob with no speed input and a fixed frame: a
        // 5-pixel cutoff, a Gaussian falloff, and a 0.3 px/frame step, on its
        // own position global so it does not disturb a configured effect.
        for (int i = 0; i < n; ++i) {
            float dist = fabsf((float)i - s_link_pos);
            float d = dist < (n - dist) ? dist : (n - dist);   // fminf
            if (d > 5.0f) {
                px[i] = mix3(color, bright100, 0.0f);
                continue;
            }
            float f = expf(-(d * d) / 4.5f);
            px[i] = mix3(color, bright100, f);
        }
        // Step +/-0.3 px per frame, wrapping like the blob effects.
        s_link_pos += reverse ? -0.3f : 0.3f;
        if (s_link_pos >= (float)n)      s_link_pos = 0.0f;
        else if (s_link_pos < 0.0f)      s_link_pos = (float)n - 1e-6f;
        return 50;
    }

    // 0 - Static: every pixel is the active colour, scaled. No motion and no
    //     time term, so with a fixed brightness the frame never changes.
    //     chan() is used directly so full brightness is byte-exact.
    case PV_FX_STATIC:
        for (int i = 0; i < n; ++i) {
            px[i].r = chan(color.r, bright100);
            px[i].g = chan(color.g, bright100);
            px[i].b = chan(color.b, bright100);
        }
        return fx_period(speed);

    // 1 - Breathing: the whole strip is one colour whose intensity eases up and
    //     down together, a triangle in s_breath_phase cosine-eased into a smooth
    //     swell. Routed through mix3, so the trough is the INACTIVE colour when
    //     one is set and a floored dim ACTIVE colour (never black) when not.
    case PV_FX_BREATHING: {
        if (s_breath_step == 0.0f) s_breath_step = BREATH_DELTA;      // arm
        s_breath_phase += s_breath_step;
        if (s_breath_phase >= 1.0f)      { s_breath_phase = 1.0f; s_breath_step = -BREATH_DELTA; }
        else if (s_breath_phase <= 0.0f) { s_breath_phase = 0.0f; s_breath_step =  BREATH_DELTA; }
        float eased = 0.5f - 0.5f * cosf((float)M_PI * s_breath_phase);
        // With an inactive colour set the trough reaches it (floor 0, so mix3
        // lands on s_fx_bg); with none set the trough floors at a dim active
        // colour rather than going black.
        bool  has_bg = s_fx_bg.r || s_fx_bg.g || s_fx_bg.b;
        float envlo  = has_bg ? 0.0f : BREATH_FLOOR;
        float env    = envlo + (1.0f - envlo) * eased;
        for (int i = 0; i < n; ++i) px[i] = mix3(color, bright100, env);
        return fx_period(speed);
    }

    // 2 - Strobing: the whole strip hard on, then hard off, in phase. The off
    //     half is mix3(...,0) == the inactive colour when set and black when
    //     not; one half per frame, so the strobe rate rides fx_period.
    case PV_FX_STROBING: {
        s_strobe_on = !s_strobe_on;
        float f = s_strobe_on ? 1.0f : 0.0f;
        for (int i = 0; i < n; ++i) px[i] = mix3(color, bright100, f);
        return fx_period(speed);
    }

    // 4 - Marquee: a single lit block that walks the run and wraps. Inside the
    //     block is active, outside is inactive/black. One block, not a tiled
    //     pattern, is what makes a joined pair show the light in one place at a
    //     time. s_marquee_pos advances once per call; the pipeline's rewind
    //     makes that one advance per frame and keeps equal-length strips equal.
    case PV_FX_MARQUEE: {
        int w = n / MARQUEE_WFRAC; if (w < 1) w = 1;
        for (int i = 0; i < n; ++i) {
            int ri = reverse ? (n - 1 - i) : i;
            float rel = (float)ri - s_marquee_pos;
            while (rel < 0.0f)      rel += (float)n;
            while (rel >= (float)n) rel -= (float)n;
            float f = (rel < (float)w) ? 1.0f : 0.0f;
            px[i] = mix3(color, bright100, f);
        }
        s_marquee_pos += MARQUEE_DELTA;
        if (s_marquee_pos >= (float)n) s_marquee_pos -= (float)n;
        return fx_period(speed);
    }

    // 3 - Wave: a sinusoidal brightness pattern that travels along the run.
    //     Each pixel blends inactive (trough) toward active (crest) through
    //     mix3, so with an inactive colour it is never black, and without one
    //     the troughs are black while the crests light. Direction flips with
    //     reverse.
    case PV_FX_WAVE: {
        float dir = reverse ? -1.0f : 1.0f;
        for (int i = 0; i < n; ++i) {
            float cyc = (float)i * WAVE_CYCLES / (float)n;
            float f   = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * (cyc - dir * s_wave_pos));
            px[i] = mix3(color, bright100, f);
        }
        s_wave_pos += WAVE_DELTA;
        if (s_wave_pos >= 1.0f) s_wave_pos -= 1.0f;
        return fx_period(speed);
    }

    // 5 - Color Cycle: the whole strip is one hue, walking the wheel. Generates
    //     its own colour; hsv_full is always full value, so at least one
    //     channel is 255 and the strip is never black. Scaled by brightness.
    case PV_FX_COLOR_CYCLE: {
        rgb_t h = hsv_full((uint16_t)s_cycle_hue % 360);
        rgb_t o = { chan(h.r, bright100), chan(h.g, bright100), chan(h.b, bright100) };
        for (int i = 0; i < n; ++i) px[i] = o;
        s_cycle_hue += CYCLE_DELTA;
        if (s_cycle_hue >= 360.0f) s_cycle_hue -= 360.0f;
        return fx_period(speed);
    }

    // -----------------------------------------------------------------
    // ADDITIONS. Everything above reproduces a routine in the stock binary
    // and cites its address. These two have no address to cite: they are
    // ours. They are written to sit alongside the stock seven rather than
    // to look like them, and they reuse the stock timing so that a given
    // speed setting means the same thing across every effect.
    // -----------------------------------------------------------------

    case PV_FX_BOUNCE: {
        // Marquee's travelling Gaussian, reflecting at the ends instead of
        // wrapping. Same sigma, same five pixel cutoff, same frame period, so
        // at any speed the blob crosses the strip at exactly the marquee's
        // pace and only the turn at each end is different.
        //
        // Distance is LINEAR here, not circular. Marquee measures
        // min(|i-p|, n-|i-p|) because it wraps; a bouncing blob that did that
        // would bleed off one end and glow faintly at the other just before
        // it turns, which reads as a bug.
        //
        // The reverse switch mirrors the strip rather than negating the step.
        // Negating it would fight the turnaround logic and stick the blob at
        // an end. Mirroring is also the more useful behaviour on a two strip
        // kit, where it decides which end the blob starts from.
        float p = reverse ? (float)(n - 1) - s_bounce_pos : s_bounce_pos;
        for (int i = 0; i < n; ++i) {
            float d = fabsf((float)i - p);
            if (d > 5.0f) {
                px[i] = mix3(color, bright100, 0.0f);
                continue;
            }
            float f = expf(-(d * d) / 4.5f);
            px[i] = mix3(color, bright100, f);
        }
        s_bounce_pos += s_bounce_dir * 0.3f;
        // Turn ON the last pixel, not past it, so the blob visibly reaches
        // both ends and dwells there for one frame rather than several.
        if (s_bounce_pos >= (float)(n - 1)) {
            s_bounce_pos = (float)(n - 1);
            s_bounce_dir = -1.0f;
        } else if (s_bounce_pos <= 0.0f) {
            s_bounce_pos = 0.0f;
            s_bounce_dir = 1.0f;
        }
        return fx_period(speed);
    }

    case PV_FX_CYLON: {
        // A bright head sweeping end to end with a tail fading out behind it.
        //
        // The tail is the whole point, and it is what separates this from
        // Bounce. Bounce's blob is symmetric, so a still photograph of it
        // cannot tell you which way it is going. This one is asymmetric, so
        // it reads as motion even frozen, and the asymmetry flips when the
        // head turns around, which is the effect people picture when they say
        // Cylon.
        //
        // Ahead of the head: a short Gaussian, so the leading edge stays
        // crisp. Behind it: a linear fade over TAIL pixels, squared, because
        // a linear ramp in duty cycle looks top heavy to the eye.
        const float TAIL = 5.0f;
        float head = reverse ? (float)(n - 1) - s_cylon_pos : s_cylon_pos;
        float dir  = reverse ? -s_cylon_dir : s_cylon_dir;
        for (int i = 0; i < n; ++i) {
            float rel = ((float)i - head) * dir;      // >0 ahead, <0 behind
            float f;
            if (rel >= 0.0f) {
                f = rel > 1.5f ? 0.0f : expf(-(rel * rel) / 0.9f);
            } else {
                float back = -rel;
                f = back > TAIL ? 0.0f : 1.0f - back / TAIL;
                f = f * f;
            }
            px[i] = mix3(color, bright100, f);
        }
        // Slightly faster than Bounce at the same speed. The tail already
        // conveys motion, so the head can move without smearing.
        s_cylon_pos += s_cylon_dir * 0.35f;
        if (s_cylon_pos >= (float)(n - 1)) {
            s_cylon_pos = (float)(n - 1);
            s_cylon_dir = -1.0f;
        } else if (s_cylon_pos <= 0.0f) {
            s_cylon_pos = 0.0f;
            s_cylon_dir = 1.0f;
        }
        return fx_period(speed);
    }

    // -----------------------------------------------------------------
    // ADDITIONS 2026-08-30. Nine effects, none of them stock.
    //
    // Shared vocabulary for the eight centre-referenced ones:
    //   h        half the strip, rounded up: pixel h-1 is the last one on the
    //            first half, pixel n-h the first one on the second
    //   depth    how far from the ORIGIN a pixel sits, in pixels
    //   origin   the middle for the *_OUT effects, the two ends for *_IN
    //
    // depth_of() converts a pixel index into that distance once, so every
    // effect below is written as a function of depth alone and an out/in pair
    // differs by one subtraction. Anything else drifts: writing the inward
    // variant as its own loop is how you end up with the two halves a pixel
    // out of step at one speed and not another.
    // -----------------------------------------------------------------
    // NOT STOCK. The bed temperature, as a colour.
    //
    // The whole strip is one colour, mixed between the effect's INACTIVE
    // colour at the cold end and its ACTIVE colour at the hot end. Not a bar:
    // a bar says "how far through", and this says "how hot", and drawing the
    // second as the first is how a glance gets the wrong answer.
    //
    // Below the cold point it is the cold colour exactly, above the hot point
    // the hot colour exactly, so the ends are readable rather than asymptotic.
    // With no temperature reported at all it holds the cold colour, because
    // "not reported" is not "cold" but it is the honest thing to show.
    case PV_FX_TEMP_GRADIENT: {
        int lo = g_cfg.rgb.grad_min_c ? g_cfg.rgb.grad_min_c : PV_GRAD_MIN_C_DEFAULT;
        int hi = g_cfg.rgb.grad_max_c ? g_cfg.rgb.grad_max_c : PV_GRAD_MAX_C_DEFAULT;
        if (hi <= lo) hi = lo + 1;         // a zero-width range divides by zero
        int tC = g_live.bed_temp;
        float f = (tC < lo) ? 0.0f : (tC >= hi) ? 1.0f
                : (float)(tC - lo) / (float)(hi - lo);
        for (int i = 0; i < n; ++i) px[i] = mix3(color, bright100, f);
        // The gradient does not animate, but it re-samples on the shared speed
        // curve so the slider still governs how quickly it follows the bed.
        return fx_period(speed);
    }

    // NOT STOCK. Whatever was uploaded.
    //
    // One row of the uploaded image per frame, scaled by the effect's own
    // brightness so it sits under the same control as everything else. The
    // upload is in RAM and is gone after a reboot, so the common case for
    // this effect is "nothing loaded": that renders as Static in the effect's
    // own colour rather than as darkness, because a strip that goes black
    // when you pick an effect reads as a fault, and this is not one.
    //
    // A frame shorter than the strip leaves the tail on the effect's colour
    // rather than dark, for the same reason.
    case PV_FX_ANIM: {
        uint8_t row[PV_ANIM_PIXELS * 3];
        int got = pv_anim_copy(s_anim_frame, row, n < PV_ANIM_PIXELS ? n : PV_ANIM_PIXELS);
        for (int i = 0; i < n; ++i) {
            if (i < got) {
                // The uploaded pixel, through the effect's own brightness, so
                // it obeys the same slider as everything else on this strip.
                px[i].r = chan_f(row[i * 3 + 0], bright100, 1.0f);
                px[i].g = chan_f(row[i * 3 + 1], bright100, 1.0f);
                px[i].b = chan_f(row[i * 3 + 2], bright100, 1.0f);
            } else {
                // Nothing loaded, or a frame shorter than the strip. Static in
                // the effect's own colour, not black: a strip that goes dark
                // when an effect is picked reads as a fault, and this is not
                // one.
                px[i] = mix3(color, bright100, 1.0f);
            }
        }
        // pv_anim_copy wraps the index itself, so this can count up forever
        // without needing to know how many frames are loaded. It is reset when
        // an animation is replaced, so a new upload starts at its first row
        // rather than wherever the last one had got to.
        ++s_anim_frame;
        // One shared speed curve, like every other effect.
        return fx_period(speed);
    }

    // NOT STOCK. Three ways to draw the same number.
    //
    //   PROGRESS       a plain bar. Stock's, and the honest default.
    //   PROGRESS_ANIM  the bar, plus a chase sweeping it and a breathing tip,
    //                  so a print in progress reads as one from across a room.
    //   BARBER         a two-colour pole crawling through the bar.
    //
    // They share the fill, the easing and the reversal, because they ARE the
    // same number: writing them as three loops is how two of them end up
    // rounding a percentage differently from the third.
    case PV_FX_PROGRESS_ANIM:
    case PV_FX_BARBER:
    case PV_FX_PROGRESS: {
        // The strip as a fuel gauge. pixels_lit = percent * n / 100, and the
        // pixel straddling the boundary is lit PARTIALLY, so a 16 pixel strip
        // still resolves single percent steps instead of stepping in chunks
        // of 6.25%.
        //
        // The shown value chases the reported one instead of snapping to it.
        // mc_percent arrives once a second at best and jumps whole points at
        // a time on a fast print; easing turns that into a crawl. The rate is
        // per frame, and the frame period is speed-dependent, so a slower
        // speed setting also means a lazier catch-up, which is what someone
        // reaching for the speed slider on a progress bar is asking for.
        // NOT STOCK. A preview may pin the percentage, so the fill can be
        // judged at a chosen point instead of waiting for a print to reach it.
        float target = (float)fx_percent();
        float delta  = target - s_progress_shown;
        // Snap on a big jump. A new print starting at 0 after the last one
        // finished at 100 should not spend five seconds draining.
        if (delta > 25.0f || delta < -25.0f) s_progress_shown = target;
        else                                 s_progress_shown += delta * 0.15f;

        float lit = s_progress_shown * (float)n / 100.0f;

        // Barber Pole with no job at all fills the whole run, so it still
        // reads as a working light on a state that never has one. The other
        // two do not: an empty bar IS the answer there, and filling it would
        // say a print was running when none is.
        if (fx == PV_FX_BARBER && lit < 0.5f) lit = (float)n;

        int litn = (int)(lit + 0.5f);
        int w = s_fx_band; if (w < 1) w = 1; if (w > n) w = n;

        for (int i = 0; i < n; ++i) {
            // Reverse fills from the far end, which is the only sensible
            // mirror for a bar: the same amount of light, other side.
            int   idx  = reverse ? (n - 1 - i) : i;
            float head = lit - (float)i;              // >=1 full, <=0 dark
            float f    = head >= 1.0f ? 1.0f : (head <= 0.0f ? 0.0f : head);

            if (fx == PV_FX_PROGRESS || f <= 0.0f) {
                px[idx] = mix3(color, bright100, f);
                continue;
            }

            if (fx == PV_FX_BARBER) {
                // Bands of the ACTIVE colour and the INACTIVE one. The
                // inactive colour is already this effect's answer to "what
                // goes where I am not lit", and in a pole that is exactly
                // what the other band is; unset, the other band is dark and
                // it reads as a plain moving stripe.
                int   band = ((i + s_barber_pos) / w) & 1;
                rgb_t c    = mix3(color, bright100, band ? 0.0f : 1.0f);
                px[idx] = (f >= 1.0f) ? c
                        : (rgb_t){ (uint8_t)(c.r * f),
                                   (uint8_t)(c.g * f),
                                   (uint8_t)(c.b * f) };
                continue;
            }

            // PROGRESS_ANIM. The fill sits at seven tenths so the two things
            // moving over it have somewhere to go.
            float lv = 0.70f;
            if (litn > 1) {
                int d = i - s_chase_pos; if (d < 0) d = -d;
                if (d < 3) {
                    float boost = 0.70f + (float)(3 - d) * 0.10f;
                    lv = boost > 1.0f ? 1.0f : boost;
                }
            }
            if (i == litn - 1) {
                // The head of the bar breathes, so the tip reads as alive
                // rather than as the place the light happened to stop.
                float ph = (float)(s_anim_breath & 63) / 63.0f;
                float w2 = ph < 0.5f ? (ph * 2.0f) : ((1.0f - ph) * 2.0f);
                lv = 0.65f + w2 * 0.35f;
            }
            if (lv > f) lv = f;         // never brighter than the fill allows
            px[idx] = mix3(color, bright100, lv);
        }

        // The phases advance once per FRAME, not once per pixel, and not once
        // per strip: fx_phase_save/restore rewinds them for each strip's pass
        // so both runs draw the same instant.
        if (fx == PV_FX_PROGRESS_ANIM) {
            s_chase_pos = (s_chase_pos + 1) % (litn < 1 ? 1 : litn);
            ++s_anim_breath;
        } else if (fx == PV_FX_BARBER) {
            // Two bands is one full repeat, so the pole returns to where it
            // started instead of drifting a pixel every cycle.
            s_barber_pos = (s_barber_pos + 1) % (w * 2);
        }

        // All three take their rate from the one shared speed curve.
        return fx_period(speed);
    }

    case PV_FX_MARQUEE_OUT:
    case PV_FX_MARQUEE_IN: {
        // Marquee's travelling Gaussian, mirrored about the centre so there
        // are two of them. Same sigma and same five pixel cutoff as Marquee
        // and Bounce, so all three read as the same light at the same speed.
        //
        // The blob wraps rather than turning around: it leaves one end and
        // reappears at the origin, which is what "marquee" means here and is
        // what separates this pair from the BOUNCE_* pair below.
        int   h   = (n + 1) / 2;
        bool  out = (fx == PV_FX_MARQUEE_OUT);
        float p   = s_split_pos;                     // 0..h-1 from the origin
        for (int i = 0; i < n; ++i) {
            int   half  = i < h ? (h - 1 - i) : (i - (n - h));
            float depth = out ? (float)half : (float)(h - 1 - half);
            float d     = fabsf(depth - p);
            if (d > 5.0f) { px[i] = mix3(color, bright100, 0.0f); continue; }
            float f = expf(-(d * d) / 4.5f);
            px[i] = mix3(color, bright100, f);
        }
        s_split_pos += reverse ? -0.3f : 0.3f;
        if (s_split_pos >= (float)h)   s_split_pos = 0.0f;
        else if (s_split_pos < 0.0f)   s_split_pos = (float)h - 1e-6f;
        return fx_period(speed);
    }

    case PV_FX_FILL_OUT:
    case PV_FX_FILL_IN: {
        // Solid, not a blob: every pixel from the origin out to the head is
        // fully lit and stays lit until the bar completes and restarts.
        //
        // The head pixel is fractional for the same reason the progress bar's
        // is. Without it a 16 pixel strip fills in 8 visible steps per half
        // and the motion looks like a stutter rather than a sweep.
        int  h   = (n + 1) / 2;
        bool out = (fx == PV_FX_FILL_OUT);
        for (int i = 0; i < n; ++i) {
            int   half  = i < h ? (h - 1 - i) : (i - (n - h));
            float depth = out ? (float)half : (float)(h - 1 - half);
            float head  = s_fill_pos - depth;
            float f     = head >= 1.0f ? 1.0f : (head <= 0.0f ? 0.0f : head);
            px[i] = mix3(color, bright100, f);
        }
        s_fill_pos += reverse ? -0.3f : 0.3f;
        // Overshoot by one so the strip is seen FULL for a moment before it
        // resets. Wrapping exactly at h blanks it the instant it completes,
        // and the eye reads that as a dropped frame.
        if (s_fill_pos >= (float)h + 1.0f) s_fill_pos = 0.0f;
        else if (s_fill_pos < 0.0f)        s_fill_pos = (float)h + 1.0f;
        return fx_period(speed);
    }

    case PV_FX_BOUNCE_OUT:
    case PV_FX_BOUNCE_IN: {
        // The MARQUEE_* pair with a reflection instead of a wrap. Out starts
        // at the middle and turns at the ends; In starts at the ends and
        // turns at the middle. Reverse mirrors the strip rather than negating
        // the step, for the reason spelled out on PV_FX_BOUNCE: negating it
        // fights the turnaround and parks the blob at a limit.
        int   h   = (n + 1) / 2;
        bool  out = (fx == PV_FX_BOUNCE_OUT);
        float p   = reverse ? (float)(h - 1) - s_sbounce_pos : s_sbounce_pos;
        for (int i = 0; i < n; ++i) {
            int   half  = i < h ? (h - 1 - i) : (i - (n - h));
            float depth = out ? (float)half : (float)(h - 1 - half);
            float d     = fabsf(depth - p);
            if (d > 5.0f) { px[i] = mix3(color, bright100, 0.0f); continue; }
            float f = expf(-(d * d) / 4.5f);
            px[i] = mix3(color, bright100, f);
        }
        s_sbounce_pos += s_sbounce_dir * 0.3f;
        if (s_sbounce_pos >= (float)(h - 1)) {
            s_sbounce_pos = (float)(h - 1);
            s_sbounce_dir = -1.0f;
        } else if (s_sbounce_pos <= 0.0f) {
            s_sbounce_pos = 0.0f;
            s_sbounce_dir = 1.0f;
        }
        return fx_period(speed);
    }

    case PV_FX_BOUNCE_FILL_OUT:
    case PV_FX_BOUNCE_FILL_IN: {
        // The FILL_* pair that unfills instead of resetting. Filling to full
        // and then dropping to black is a hard cut; retreating the way it came
        // is continuous, and it is the difference the name is asking for.
        //
        // The limits are 0 and h, not h-1: the bar has to reach COMPLETELY
        // full before it turns, and full means the head is past the last
        // pixel, not on it.
        int  h   = (n + 1) / 2;
        bool out = (fx == PV_FX_BOUNCE_FILL_OUT);
        float base = reverse ? (float)h - s_sfill_pos : s_sfill_pos;
        for (int i = 0; i < n; ++i) {
            int   half  = i < h ? (h - 1 - i) : (i - (n - h));
            float depth = out ? (float)half : (float)(h - 1 - half);
            float head  = base - depth;
            float f     = head >= 1.0f ? 1.0f : (head <= 0.0f ? 0.0f : head);
            px[i] = mix3(color, bright100, f);
        }
        s_sfill_pos += s_sfill_dir * 0.3f;
        if (s_sfill_pos >= (float)h) {
            s_sfill_pos = (float)h;
            s_sfill_dir = -1.0f;
        } else if (s_sfill_pos <= 0.0f) {
            s_sfill_pos = 0.0f;
            s_sfill_dir = 1.0f;
        }
        return fx_period(speed);
    }

    // 6 - Rainbow: the hue wheel spread across the run, scrolling over time.
    //     One full wheel maps across n, so it stays a complete rainbow at any
    //     strip length; hsv_full is never black. Spatial order flips with
    //     reverse. Also the default, so an unknown id still shows something.
    case PV_FX_RAINBOW:
    default: {
        for (int i = 0; i < n; ++i) {
            int ri = reverse ? (n - 1 - i) : i;
            float hue = s_rainbow_phase + (360.0f * (float)ri) / (float)n;
            rgb_t h = hsv_full((uint16_t)hue % 360);
            px[i].r = chan(h.r, bright100);
            px[i].g = chan(h.g, bright100);
            px[i].b = chan(h.b, bright100);
        }
        s_rainbow_phase += RAINBOW_DELTA;
        if (s_rainbow_phase >= 360.0f) s_rainbow_phase -= 360.0f;
        return fx_period(speed);
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

// NOT STOCK. Every effect carries an open colour and a closed colour; this is
// the one place that decides between them. Reading g_live.vent_open rather
// than the configured target means the strip follows the vent's ACTUAL
// position, so it changes when the flap finishes moving, not when the command
// is issued.
static rgb_t fx_colour(const pv_fx_param_t *p)
{
    const uint8_t *c = g_live.vent_open ? p->rgb : p->rgb_closed;
    return (rgb_t){ c[0], c[1], c[2] };
}

// The INACTIVE colour for the vent's current position, or black when the user
// has not set one, which is what every effect painted before this existed.
static rgb_t fx_bg(const pv_fx_param_t *p)
{
    uint8_t bit = g_live.vent_open ? PV_BG_OPEN : PV_BG_CLOSED;
    if (!(p->opt_set & bit)) return (rgb_t){0, 0, 0};
    const uint8_t *c = g_live.vent_open ? p->bg : p->bg_closed;
    return (rgb_t){ c[0], c[1], c[2] };
}

// NOT STOCK. The live preview. See pv.h for why it bypasses the gates.
static pv_preview_t s_preview;

void pv_rgb_preview(int fx, const pv_fx_param_t *p, int state, int percent, int seconds)
{
    if (!p) return;
    if (fx < 0 || fx >= PV_FX_COUNT) fx = PV_FX_STATIC;
    if (seconds < 1) seconds = 1;
    if (seconds > PV_PREVIEW_MAX_S) seconds = PV_PREVIEW_MAX_S;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_preview.fx = fx;
    s_preview.p = *p;
    if (s_preview.p.brightness > 100) s_preview.p.brightness = 100;
    if (s_preview.p.speed > 100) s_preview.p.speed = 100;
    if (s_preview.p.bright_end > 100) s_preview.p.bright_end = 100;
    s_preview.state = (int8_t)((state >= 0 && state < PV_ST_COUNT) ? state : -1);
    s_preview.percent = (int8_t)((percent >= 0 && percent <= 100) ? percent : -1);
    s_preview.until_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    s_preview.active = true;
    // Start the ramp and the animation from the beginning, so what is being
    // judged is the effect from its start rather than mid-sweep.
    s_ramp_step = 0;
    xSemaphoreGive(s_lock);
}

void pv_rgb_preview_cancel(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_preview.active = false;
    xSemaphoreGive(s_lock);
}

// The print percentage the effects should use: the preview's, when it has
// pinned one, otherwise whatever the printer is actually reporting.
static int fx_percent(void)
{
    if (s_preview.active && s_preview.percent >= 0) return s_preview.percent;
    return g_live.print_percent;
}

// NOT STOCK. The printer state the LIGHT should answer to. A preview may pin
// one so the look of a state can be judged without waiting for the printer to
// reach it, which for the error state means without causing an error.
//
// Deliberately read-side and local to this renderer. g_live.device_state
// itself is never written, because pv_motor.c reads it to choose the flap
// direction and pv_policy.c reads it to decide whether to vent at all: a
// preview must change what the strip shows and nothing else.
static int fx_state(void)
{
    if (s_preview.active && s_preview.state >= 0) return s_preview.state;
    return g_live.device_state;
}

// NOT STOCK. Retire a preview whose time is up.
//
// Called at the very top of resolve() rather than at the override, because
// several gates above the override return early: a pinned ERROR state with
// the warning override on answers red at gate 2 and never reaches it. Expiring
// there too would leave the preview pinned for good.
static void preview_tick(void)
{
    if (s_preview.active && esp_timer_get_time() >= s_preview.until_us) {
        s_preview.active = false;
        pv_ws_push_state();          // let the UI drop its countdown
    }
}

// What a running preview has pinned, or -1 where it has pinned nothing. Only
// meaningful while pv_rgb_preview_left() is non-zero.
int pv_rgb_preview_state(void)
{
    return s_preview.active ? s_preview.state : -1;
}

int pv_rgb_preview_percent(void)
{
    return s_preview.active ? s_preview.percent : -1;
}

int pv_rgb_preview_left(void)
{
    if (!s_preview.active) return 0;
    int64_t left = s_preview.until_us - esp_timer_get_time();
    if (left <= 0) return 0;
    return (int)((left + 999999) / 1000000);
}

// NOT STOCK. The band width a Barber Pole should draw at: the stored aux when
// one is set, otherwise a fifth of the run, which puts about two and a half
// bands on a sixteen pixel strip and keeps the same look on a shorter one.
static int fx_band(const pv_fx_param_t *p, int n)
{
    int w = (p && (p->opt_set & PV_AUX)) ? (int)p->aux : 0;
    if (w <= 0) w = n / 5;
    if (w < 1)  w = 1;
    if (w > n)  w = n;
    return w;
}

// -1 means "no ramp": one brightness for the whole cycle, as before.
static int fx_bright_end(const pv_fx_param_t *p)
{
    return (p->opt_set & PV_BRIGHT_END) ? (int)p->bright_end : -1;
}

static bool resolve(int *fx, rgb_t *color, uint8_t *bright, uint8_t *speed,
                    rgb_t *bg, int *bright_end, int *band, bool *flip, int n)
{
    // Every path that is not a configured effect runs at one brightness.
    *bright_end = -1;
    const pv_rgb_cfg_t *r = &g_cfg.rgb;
    // Every path that does not come from a configured effect keeps the stock
    // behaviour of going dark where it is not lit.
    *bg = (rgb_t){0, 0, 0};

    preview_tick();      // NOT STOCK, and above every early return by design

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
    if (r->warning_sw && fx_state() == PV_ST_ERROR) {
        if (r->light_mode != PV_MODE_H2D) {
            // 0x400dcc9d: mode != 1 routes to 0x400ddeac, solid red 127.
            //
            // NOT STOCK: unless the owner has said otherwise. Red is the one
            // colour a red-green colourblind owner cannot pick out, and a
            // fault that does not move is a fault that gets walked past. With
            // err_set clear this is stock's override byte for byte, including
            // the 127 that is written without brightness scaling.
            if (r->err_set) {
                *fx = r->err_strobe ? PV_FX_STROBING : PV_FX_STATIC;
                *color = (rgb_t){ r->err_rgb[0], r->err_rgb[1], r->err_rgb[2] };
                *bg = (rgb_t){0, 0, 0};
                *bright = r->err_bright > 100 ? 100 : r->err_bright;
                *speed = 50;
                *band = 3;
                *flip = false;
                return true;
            }
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

    // 4b. NOT STOCK. A live preview replaces the whole answer from here down.
    //
    // It sits BELOW the fault and test gates, which are safety and diagnostics
    // and must never be masked, and ABOVE the master switch and the follow
    // gates, which would otherwise make a preview show nothing at all. See the
    // comment on pv_preview_t.
    if (s_preview.active) {
        const pv_fx_param_t *p = &s_preview.p;
        *fx = s_preview.fx;
        *color = fx_colour(p);
        *bg = fx_bg(p);
        *bright = p->brightness;
        *speed = p->speed;
        *bright_end = fx_bright_end(p);
        return true;
    }

    // 5. The selected light mode.
    switch (r->light_mode) {
    case PV_MODE_H2D: {
        // The live state, not fx_state(): a pinned state cannot reach here,
        // because the preview override above returns before it. Pinning is
        // felt at the warning gate, which sits above that override, and in
        // fx_percent(), which the progress effects read.
        int st = g_live.device_state;
        if (st < 0 || st >= PV_ST_COUNT) st = PV_ST_IDLE;
        int e = r->h2d_active[st];
        if (e < 0 || e >= PV_FX_COUNT) e = PV_FX_STATIC;
        const pv_fx_param_t *p = &g_h2d[st][e];
        *fx = e; *color = fx_colour(p); *bg = fx_bg(p);
        *bright = p->brightness; *speed = p->speed;
        *bright_end = fx_bright_end(p);
        *band = fx_band(p, n);
        *flip = (p->opt_set & PV_FX_REVERSE) != 0;
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
        // NOT STOCK: the threshold is a setting now. Zero means "never set",
        // and never set is stock's fifty, so an untouched device compares the
        // same number stock does, the same way.
        int hot_c = g_cfg.rgb.warn_hot_c ? g_cfg.rgb.warn_hot_c : PV_WARN_HOT_C;
        bool hot = (hot_c < g_live.bed_temp)
                || (hot_c < g_live.nozzle_temp);

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
        *fx = e; *color = fx_colour(p); *bg = fx_bg(p);
        *bright = p->brightness; *speed = p->speed;
        *bright_end = fx_bright_end(p);
        *band = fx_band(p, n);
        *flip = (p->opt_set & PV_FX_REVERSE) != 0;
        return true;
    }
    }
}

static TaskHandle_t s_render;

static void strip_blank(void)
{
    rgb_t off[PV_LEDS_PER_STRIP];
    memset(off, 0, sizeof(off));
    for (int s = 0; s < s_strips; ++s) strip_push(s, off, PV_LEDS_PER_STRIP);
}

void pv_rgb_stop(void)
{
    // 0x400dcae5 tests the notification for 255. Anything else is ignored and
    // the frame proceeds normally.
    TaskHandle_t t = s_render;
    if (t) xTaskNotify(t, 255, eSetValueWithOverwrite);
}

// NOT STOCK. One frame, lifted out of the task so it can be driven from a
// host test.
//
// The renderer had been verified by calling render_effect directly, which is
// only part of what a frame is: it skipped resolve, the brightness ramp, the
// per-strip lengths and the phase rewind between strips. Those are exactly the
// parts most likely to be wrong, and none of them were covered. This is the
// SHIPPING body, not a copy of it. render_task calls it and does nothing else;
// tools/fxdump drives it with push = false and reads the pixels back.
//
// out, when given, receives PV_STRIP_COUNT_MAX rows of PV_LEDS_PER_STRIP
// pixels. Returns the frame period the effect asked for.
// NOT STOCK. What the renderer is actually doing.
//
// A strip that looks wrong and a strip that is not being drawn at all look the
// same from across a room, and until now the only way to tell them apart was a
// serial cable. These are counted where the work happens and reported on the
// Status page: frames drawn, the effect being drawn, the interval it asked
// for, and how many RMT pushes were refused by the driver.
static uint32_t s_frames;
static uint32_t s_push_fail;
static int64_t  s_frames_since_us;
static uint32_t s_frames_at_mark;
static int      s_last_fx = -1;
static uint32_t s_last_wait;

void pv_rgb_stats(pv_rgb_stats_t *o)
{
    if (!o) return;
    o->frames = s_frames;
    o->push_failed = s_push_fail;
    o->effect = s_last_fx;
    o->interval_ms = s_last_wait;
    // Frames per second over the window since the last time anyone asked,
    // which is what makes it a rate rather than a lifetime average that can
    // never move again.
    int64_t now = esp_timer_get_time();
    int64_t span = now - s_frames_since_us;
    o->fps = (s_frames_since_us && span > 200000)
           ? (int)(((int64_t)(s_frames - s_frames_at_mark) * 1000000 + span / 2) / span)
           : -1;
    if (span > 200000) { s_frames_since_us = now; s_frames_at_mark = s_frames; }
    else if (!s_frames_since_us) { s_frames_since_us = now; s_frames_at_mark = s_frames; }
}

uint32_t pv_rgb_render_frame(rgb_t out[][PV_LEDS_PER_STRIP], bool push)
{
    static rgb_t px[PV_LEDS_PER_STRIP];
        int fx = PV_FX_STATIC; rgb_t color, bg; uint8_t bright, speed;
    int bright_end = -1;
    // The band width the Barber Pole should use. Resolved once for the whole
    // frame off the FIRST strip's length, so both runs draw the same pole
    // rather than two different ones when they are different lengths.
    int band = 3;
    // NOT STOCK. Whether THIS effect runs the other way round, which is a
    // separate answer from the master switch and from the per-strip flags.
    bool fx_flip = false;
    uint32_t wait_ms = 50;
    pv_fx_phase_t phase;
    int n0 = g_cfg.leds[0];
    if (n0 < 1 || n0 > PV_LEDS_PER_STRIP) n0 = PV_LEDS_PER_STRIP;
    bool on = resolve(&fx, &color, &bright, &speed, &bg, &bright_end, &band, &fx_flip, n0);
    s_fx_band = band;
    // NOT STOCK. Fold the brightness ramp into the value the effect is
    // given, so every effect inherits it without knowing it exists and an
    // unset ramp is bit-identical to what it rendered before.
    bright = ramp_apply(bright, on ? bright_end : -1);
    if (!on) memset(px, 0, sizeof(px));
    else     fx_phase_save(&phase);
    // PV_FX_HOLD is stock's 0x400dcab0: it returns without reaching the
    // shared refresh at 0x400dce68, so no RMT transaction is queued at
    // all and the WS2812s simply hold their latched frame. Re-pushing an
    // identical buffer would look the same but is a different instruction
    // path and costs a transfer per frame, so skip it outright.
    // NOT STOCK. One run, or two.
    //
    // The strips are separate outputs and every effect renders on each of them
    // from its own start, so a marquee runs twice, side by side, which is what
    // stock does and is not always what the hardware looks like. Contiguous
    // renders ONE strip of leds[0] + leds[1] pixels and hands each output its
    // own slice, so the light travels the whole length once.
    static rgb_t joined[PV_LEDS_PER_STRIP * PV_STRIP_COUNT_MAX];
    int total = 0;
    bool joinup = g_cfg.rgb.contiguous && s_strips > 1;
    if (joinup) {
        for (int s2 = 0; s2 < s_strips; ++s2) {
            int n2 = g_cfg.leds[s2];
            if (n2 < 1 || n2 > PV_LEDS_PER_STRIP) n2 = PV_LEDS_PER_STRIP;
            total += n2;
        }
        if (on) {
            fx_phase_restore(&phase);
            // The master flip only. A per-strip flag has no meaning for one
            // run that happens to be delivered down two wires, and applying
            // one of them to half of a joined effect would tear it in two.
            bool rev = g_cfg.rgb.reverse ^ fx_flip;
            wait_ms = render_effect(fx, color, bg, bright, speed, rev, joined, total);
        } else {
            memset(joined, 0, sizeof(joined));
        }
    }

    if (fx != PV_FX_HOLD) {
        int taken = 0;
        for (int s = 0; s < s_strips; ++s) {
            int n = g_cfg.leds[s];
            if (n < 1 || n > PV_LEDS_PER_STRIP) n = PV_LEDS_PER_STRIP;
            if (joinup) {
                // This output's slice of the one long strip.
                for (int i = 0; i < n; ++i) px[i] = joined[taken + i];
                for (int i = n; i < PV_LEDS_PER_STRIP; ++i) px[i] = (rgb_t){0, 0, 0};
                taken += n;
            } else if (on) {
                // Same instant for every strip: rewind the phase, then let
                // this strip's pass advance it. The last pass leaves the
                // phase advanced exactly once for the frame.
                fx_phase_restore(&phase);
                // Three independent flips, combined by exclusive-or:
                //
                //   the master switch     everything turns around
                //   this strip's flag     one run is mounted backwards
                //   this effect's flag    this look reads better the other way
                //
                // Exclusive-or is the only composition that lets all three
                // mean the same thing. Two of them set is back where you
                // started, which is what "turn it around, twice" means.
                bool rev = g_cfg.rgb.reverse
                         ^ ((g_cfg.rgb.reverse_strips >> s) & 1)
                         ^ fx_flip;
                wait_ms = render_effect(fx, color, bg, bright, speed,
                                        rev, px, n);
                // Anything past the configured length is explicitly dark.
                for (int i = n; i < PV_LEDS_PER_STRIP; ++i)
                    px[i] = (rgb_t){0, 0, 0};
            }
            // ALWAYS push the full sixteen, whatever the configured count.
            //
            // Pushing only n was a real fault, found on the hardware: a
            // WS2812 latches its last frame and holds it, so a run that is
            // physically longer than the configured count keeps its final
            // LEDs lit at whatever colour they happened to be showing when
            // the count changed, forever. On Jeremy's vent that froze a
            // whole light bar while the others followed the effect.
            //
            // The count cannot be measured (WS2812 is write-only), so it
            // is a number a person types, and a wrong number must not be
            // able to strand pixels. Sending sixteen always costs nothing:
            // on a shorter run the surplus bytes are simply not received.
            // Now a count that is too small only darkens the tail, which
            // is visible, obviously wrong, and instantly reversible.
            if (out) memcpy(out[s], px, sizeof(rgb_t) * PV_LEDS_PER_STRIP);
                if (push && strip_push(s, px, PV_LEDS_PER_STRIP) != ESP_OK)
                    ++s_push_fail;
        }
    }
    ++s_frames;
    s_last_fx = fx;
    s_last_wait = wait_ms;
    return wait_ms;
}

static void render_task(void *arg)
{
    // The pixel buffer moved into pv_rgb_render_frame with the rest of the
    // frame body; the task holds no state of its own now.
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
        uint32_t wait_ms = pv_rgb_render_frame(NULL, true);
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
        // Stock's three calls, in stock's order: rmt_new_tx_channel,
        // rmt_new_led_strip_encoder, rmt_enable (0x400f3010, 0x400de088,
        // 0x400f2730). Nothing else is called per strip, and stock calls
        // nothing we do not.
        rmt_tx_channel_config_t tx = {
            .gpio_num          = pins[i],
            .clk_src           = RMT_CLK_SRC_DEFAULT,   // stock's 4 = APB
            .resolution_hz     = RMT_RES_HZ,
            .mem_block_symbols = 64,
            .trans_queue_depth = 4,
        };
        esp_err_t e1 = rmt_new_tx_channel(&tx, &s_chan[i]);
        esp_err_t e2 = e1 == ESP_OK ? ws_encoder_new(&s_enc[i]) : e1;
        esp_err_t e3 = e2 == ESP_OK ? rmt_enable(s_chan[i])     : e2;
        if (e3 == ESP_OK) {
            ++s_strips;
        } else {
            ESP_LOGW(TAG, "strip %d init failed: chan=%s enc=%s enable=%s", i,
                     esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
            break;
        }
    }
    ESP_LOGI(TAG, "%d strip(s) up", s_strips);
    // Stock's rgb task runs at priority 15, not 4 (0x400d8cb6 passes 15 to
    // xTaskCreatePinnedToCore at 0x400d8cc2).
    if (s_strips)
        xTaskCreate(render_task, "pv_rgb", 4096, NULL, 15, &s_render);
}
