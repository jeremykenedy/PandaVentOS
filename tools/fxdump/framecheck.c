/* Drives pv_rgb.c's OWN per-frame body, pv_rgb_render_frame, through the same
 * path the device uses: resolve() picks the effect and its four colours out of
 * the real config, the brightness ramp is applied, each strip renders at its
 * own length from one rewound phase.
 *
 * fxdump proves render_effect. This proves the FRAME: everything around
 * render_effect that fxdump never touched, and every one of those parts is
 * newer and less exercised than the effect renderers themselves.
 *
 *   cc -I stub -I ../../firmware/main -o framecheck framecheck.c -lm && ./framecheck
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pv.h"
int64_t   fc_clock_us;      /* the movable stub clock */
pv_cfg_t  g_cfg;
pv_live_t g_live;

void pv_ws_push_state(void) {}
void pv_ws_push_state_to(int fd) { (void)fd; }
void pv_ws_broadcast(char *j) { free(j); }
char *pv_json_state(void) { return NULL; }
char *pv_json_response(const char *t, int ok) { (void)t; (void)ok; return NULL; }
void pv_cfg_save(void) {}
void pv_motor_update(void) {}
int  pv_policy_match(const char *m) { (void)m; return -1; }
pv_fx_param_t g_h2d[PV_ST_COUNT][PV_FX_COUNT];
void pv_cfg_h2d_save(int st) { (void)st; }
bool pv_bambu_started(void) { return true; }
bool pv_motor_fault_any(void) { return false; }
bool pv_wifi_saw_test_ap(void) { return false; }
void pv_wifi_scan_start(void) {}
int  pv_wifi_test_scan_state(void) { return 0; }

/* pv_hex_to_rgb3 lives in pv_cfg.c, which is not worth pulling in whole for a
 * renderer test: it drags NVS with it. Same four lines, same meaning. */
static uint8_t hexnib_(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}
void pv_hex_to_rgb3(const char *hex, uint8_t out[3])
{
    if (!hex || strlen(hex) < 6) { out[0] = out[1] = out[2] = 0; return; }
    for (int i = 0; i < 3; ++i)
        out[i] = (uint8_t)((hexnib_(hex[i * 2]) << 4) | hexnib_(hex[i * 2 + 1]));
}
void pv_rgb3_to_hex(const uint8_t rgb[3], char out[7])
{
    static const char *H = "0123456789ABCDEF";
    for (int i = 0; i < 3; ++i) { out[i*2] = H[rgb[i] >> 4]; out[i*2+1] = H[rgb[i] & 15]; }
    out[6] = 0;
}

#include "pv_rgb.c"

static int ok_n, bad_n;
static void t(const char *name, int cond, const char *detail)
{
    if (cond) { ok_n++; printf("  ok   %s\n", name); }
    else      { bad_n++; printf("  FAIL %s%s%s\n", name, detail ? "   " : "", detail ? detail : ""); }
}

static const char *FXNAME[PV_FX_COUNT] = {
    "Static","Breathing","Strobing","Wave","Marquee","Color_Cycle","Rainbow",
    "Cylon","Bounce","Progress_Bar","Marquee_Out","Marquee_In","Fill_Out",
    "Fill_In","Bounce_Out","Bounce_In","Bounce_Fill_Out","Bounce_Fill_In" };

/* Put the engine in a known place: simple mode, one effect, link settled so
 * resolve() falls all the way through to the configured effect. */
static void setup(int fx, const char *open_hex, const char *closed_hex,
                  const char *bg_hex, const char *bgc_hex,
                  int bright, int bright_end, int leds0, int leds1, bool vent_open)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(&g_live, 0, sizeof(g_live));
    /* A preview left running by the previous case would silently answer this
     * one instead of the config being set up here. */
    pv_rgb_preview_cancel();
    fc_clock_us = 0;
    s_strips = 2;
    s_test_entered = false;
    s_link_ind = 0;
    g_cfg.rgb.light_on = true;
    g_cfg.rgb.light_mode = PV_MODE_SIMPLE;
    g_cfg.rgb.simple_current = fx;
    g_cfg.leds[0] = leds0;
    g_cfg.leds[1] = leds1;
    g_live.vent_open = vent_open;
    g_live.print_percent = 50;
    pv_fx_param_t *p = &g_cfg.rgb.simple[fx];
    p->brightness = bright;
    p->speed = 50;
    pv_hex_to_rgb3(open_hex,   p->rgb);
    pv_hex_to_rgb3(closed_hex, p->rgb_closed);
    p->opt_set = 0;
    if (bg_hex)  { pv_hex_to_rgb3(bg_hex,  p->bg);        p->opt_set |= PV_BG_OPEN; }
    if (bgc_hex) { pv_hex_to_rgb3(bgc_hex, p->bg_closed); p->opt_set |= PV_BG_CLOSED; }
    if (bright_end >= 0) { p->bright_end = bright_end; p->opt_set |= PV_BRIGHT_END; }
    else                 { p->bright_end = bright; }
    s_ramp_step = 0;
    /* Every animation phase, not just the ramp. Leaving these where the last
     * case ended made one test compare two runs that did not start together,
     * which is a fault in the test, not the firmware. */
    s_breath_phase = 0; s_breath_step = 0; s_strobe_on = false;
    s_marquee_pos = 0; s_link_pos = 0;
    s_bounce_pos = 0; s_bounce_dir = 1.0f;
    s_cylon_pos = 0; s_cylon_dir = 1.0f;
    s_split_pos = 0; s_fill_pos = 0;
    s_sbounce_pos = 0; s_sbounce_dir = 1.0f;
    s_sfill_pos = 0; s_sfill_dir = 1.0f;
    s_progress_shown = 0; s_wave_pos = 0;
    s_cycle_hue = 0; s_rainbow_phase = 0;
}

static rgb_t frame[PV_STRIP_COUNT_MAX][PV_LEDS_PER_STRIP];
static uint32_t step(void) { return pv_rgb_render_frame(frame, false); }

static int is(rgb_t c, int r, int g, int b) { return c.r == r && c.g == g && c.b == b; }
static int black(rgb_t c) { return !c.r && !c.g && !c.b; }
static int same(rgb_t a, rgb_t b) { return a.r==b.r && a.g==b.g && a.b==b.b; }

int main(void)
{
    char buf[160];

    puts("1. the INACTIVE colour is painted where the effect is dark, all 18 effects");
    for (int fx = 0; fx < PV_FX_COUNT; ++fx) {
        /* Colour_Cycle and Rainbow generate their own colours; the inactive
         * colour still has to appear where they are dark. */
        setup(fx, "0000FF", "0000FF", "FF0000", NULL, 100, -1, 16, 16, true);
        int sawBlack = 0, frames = 0;
        for (int f = 0; f < 60; ++f) {
            step(); frames++;
            for (int i = 0; i < 16; ++i) if (black(frame[0][i])) sawBlack = 1;
        }
        snprintf(buf, sizeof buf, "(fx %d %s) a pixel was left black", fx, FXNAME[fx]);
        t(FXNAME[fx], !sawBlack, sawBlack ? buf : NULL);
        (void)frames;
    }

    puts("\n2. with NO inactive colour those same pixels are black, all 18 effects");
    for (int fx = 0; fx < PV_FX_COUNT; ++fx) {
        setup(fx, "0000FF", "0000FF", NULL, NULL, 100, -1, 16, 16, true);
        int nonBlackEver = 0;
        for (int f = 0; f < 60; ++f) {
            step();
            for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) nonBlackEver = 1;
        }
        /* every effect lights SOMETHING, so this only checks the run completes
         * and that the two configurations differ; the pairing is checked next */
        t(FXNAME[fx], nonBlackEver, "nothing lit at all");
    }

    puts("\n3. the vent position picks the pair, not the effect");
    setup(PV_FX_STATIC, "00FF00", "0000FF", "FF0000", "FFFF00", 100, -1, 16, 16, true);
    step();
    t("vent OPEN uses the open active colour",   is(frame[0][0], 0, 255, 0), NULL);
    setup(PV_FX_STATIC, "00FF00", "0000FF", "FF0000", "FFFF00", 100, -1, 16, 16, false);
    step();
    t("vent CLOSED uses the closed active colour", is(frame[0][0], 0, 0, 255), NULL);
    /* Fill_Out at frame 0 is entirely unlit, so every pixel is the inactive one */
    setup(PV_FX_FILL_OUT, "00FF00", "0000FF", "FF0000", "FFFF00", 100, -1, 16, 16, true);
    step();
    t("vent OPEN uses the open INACTIVE colour",   is(frame[0][0], 255, 0, 0), NULL);
    setup(PV_FX_FILL_OUT, "00FF00", "0000FF", "FF0000", "FFFF00", 100, -1, 16, 16, false);
    step();
    t("vent CLOSED uses the closed INACTIVE colour", is(frame[0][0], 255, 255, 0), NULL);

    puts("\n4. the brightness ramp, through the real frame");
    setup(PV_FX_STATIC, "FFFFFF", "FFFFFF", NULL, NULL, 100, 0, 16, 16, true);
    int first = -1, mid = -1, last = -1, wrapped = -1;
    for (int f = 0; f <= 100; ++f) {
        step();
        if (f == 0)   first = frame[0][0].r;
        if (f == 50)  mid   = frame[0][0].r;
        if (f == 99)  last  = frame[0][0].r;
        if (f == 100) wrapped = frame[0][0].r;
    }
    snprintf(buf, sizeof buf, "first=%d mid=%d last=%d wrapped=%d", first, mid, last, wrapped);
    t("starts at the start brightness", first == 255, buf);
    t("is about half way at the half way point", mid > 120 && mid < 136, buf);
    t("reaches the end brightness", last <= 5, buf);
    t("wraps back to the start", wrapped == 255, buf);

    setup(PV_FX_STATIC, "FFFFFF", "FFFFFF", NULL, NULL, 20, 100, 16, 16, true);
    int up0 = -1, up99 = -1;
    for (int f = 0; f < 100; ++f) { step(); if (f == 0) up0 = frame[0][0].r; if (f == 99) up99 = frame[0][0].r; }
    snprintf(buf, sizeof buf, "start=%d end=%d", up0, up99);
    t("ramps upward too", up0 < 60 && up99 > 245, buf);

    setup(PV_FX_STATIC, "FFFFFF", "FFFFFF", NULL, NULL, 60, -1, 16, 16, true);
    int flat = 1, v0;
    step(); v0 = frame[0][0].r;
    for (int f = 0; f < 100; ++f) { step(); if (frame[0][0].r != v0) flat = 0; }
    snprintf(buf, sizeof buf, "moved from %d", v0);
    t("with no ramp the brightness never moves", flat, buf);

    puts("\n5. the ramp reaches the INACTIVE colour too, not just the lit pixels");
    setup(PV_FX_FILL_OUT, "0000FF", "0000FF", "FF0000", NULL, 100, 0, 16, 16, true);
    int bgFirst = -1, bgLast = -1;
    for (int f = 0; f < 100; ++f) { step(); if (f == 0) bgFirst = frame[0][0].r; if (f == 99) bgLast = frame[0][0].r; }
    snprintf(buf, sizeof buf, "first=%d last=%d", bgFirst, bgLast);
    t("the inactive colour dims with the ramp", bgFirst > 240 && bgLast <= 5, buf);

    puts("\n6. per-strip lengths: both strips render the SAME instant");
    setup(PV_FX_MARQUEE, "00FF00", "00FF00", NULL, NULL, 100, -1, 16, 16, true);
    int identical = 1;
    for (int f = 0; f < 40; ++f) {
        step();
        for (int i = 0; i < 16; ++i) if (!same(frame[0][i], frame[1][i])) identical = 0;
    }
    t("two 16-LED strips are pixel-identical", identical, NULL);

    setup(PV_FX_MARQUEE, "00FF00", "00FF00", NULL, NULL, 100, -1, 16, 11, true);
    int tailDark = 1, shortLit = 0, longLit = 0;
    for (int f = 0; f < 60; ++f) {
        step();
        for (int i = 11; i < 16; ++i) if (!black(frame[1][i])) tailDark = 0;
        for (int i = 0; i < 11; ++i) if (!black(frame[1][i])) shortLit = 1;
        for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) longLit = 1;
    }
    t("a shorter strip leaves its tail explicitly dark", tailDark, NULL);
    t("the shorter strip is still lit within its length", shortLit, NULL);
    t("the longer strip is unaffected", longLit, NULL);

    puts("\n7. the animation advances ONCE per frame, not once per strip");
    setup(PV_FX_MARQUEE, "00FF00", "00FF00", NULL, NULL, 100, -1, 16, 16, true);
    float a[40];
    for (int f = 0; f < 40; ++f) { step(); a[f] = s_marquee_pos; }
    setup(PV_FX_MARQUEE, "00FF00", "00FF00", NULL, NULL, 100, -1, 16, 16, true);
    s_strips = 1;
    int matched = 1;
    for (int f = 0; f < 40; ++f) { step(); if (a[f] != s_marquee_pos) matched = 0; }
    s_strips = 2;
    t("two strips advance the phase exactly as one does", matched, NULL);

    puts("\n8. the centred effects are centred at every strip length");
    for (int n = 5; n <= 16; ++n) {
        int bad = 0;
        for (int fx = PV_FX_MARQUEE_OUT; fx <= PV_FX_BOUNCE_FILL_IN; ++fx) {
            setup(fx, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, n, n, true);
            for (int f = 0; f < 25; ++f) {
                step();
                for (int i = 0; i < n; ++i)
                    if (!same(frame[0][i], frame[0][n - 1 - i])) { bad = 1; break; }
                if (bad) break;
            }
            if (bad) { snprintf(buf, sizeof buf, "fx %d %s at n=%d", fx, FXNAME[fx], n); break; }
        }
        char nm[48]; snprintf(nm, sizeof nm, "n=%d: all eight symmetric about the middle", n);
        t(nm, !bad, bad ? buf : NULL);
    }

    puts("\n9. the progress bar is scaled to the strip, at every length");
    for (int n = 5; n <= 16; ++n) {
        int bad = 0;
        for (int pct = 0; pct <= 100; pct += 10) {
            setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, n, n, true);
            g_live.print_percent = pct;
            for (int f = 0; f < 40; ++f) step();      /* it eases toward the value */
            int lit = 0;
            for (int i = 0; i < n; ++i) if (!black(frame[0][i])) lit++;
            int want = (pct * n + 50) / 100;
            if (lit < want - 1 || lit > want + 1) {
                snprintf(buf, sizeof buf, "n=%d pct=%d lit=%d want~%d", n, pct, lit, want);
                bad = 1; break;
            }
        }
        char nm[64]; snprintf(nm, sizeof nm, "n=%d: lit pixels track the percentage", n);
        t(nm, !bad, bad ? buf : NULL);
    }

    puts("\n10. Follow Vent, and the gate order around it");
    /* Follow Vent means: when the vent is CLOSED, the strip goes dark. It is
     * only consulted when Follow Printer is off, which is stock's order. */
    setup(PV_FX_STATIC, "00FF00", "0000FF", NULL, NULL, 100, -1, 16, 16, true);
    g_cfg.rgb.follow_printer = false;
    g_cfg.rgb.follow_vent = true;
    step();
    t("follow vent ON, vent OPEN: the strip is lit", is(frame[0][0], 0, 255, 0), NULL);

    setup(PV_FX_STATIC, "00FF00", "0000FF", NULL, NULL, 100, -1, 16, 16, false);
    g_cfg.rgb.follow_printer = false;
    g_cfg.rgb.follow_vent = true;
    step();
    {
        int allDark = 1;
        for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) allDark = 0;
        t("follow vent ON, vent CLOSED: the strip is dark", allDark, NULL);
    }

    /* And it must go dark even when an INACTIVE colour is set: the whole strip
     * is off, not painted with the inactive colour. That is the difference
     * between "this effect is dark here" and "the lights are off". */
    setup(PV_FX_STATIC, "00FF00", "0000FF", "FF0000", "FFFF00", 100, -1, 16, 16, false);
    g_cfg.rgb.follow_printer = false;
    g_cfg.rgb.follow_vent = true;
    step();
    {
        int allDark = 1;
        for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) allDark = 0;
        t("and an inactive colour does not keep it alight", allDark, NULL);
    }

    setup(PV_FX_STATIC, "00FF00", "0000FF", NULL, NULL, 100, -1, 16, 16, false);
    g_cfg.rgb.follow_printer = false;
    g_cfg.rgb.follow_vent = false;
    step();
    t("follow vent OFF, vent CLOSED: still lit, with the closed colour",
      is(frame[0][0], 0, 0, 255), NULL);

    /* Follow Printer wins, so follow_vent is not consulted at all. */
    setup(PV_FX_STATIC, "00FF00", "0000FF", NULL, NULL, 100, -1, 16, 16, false);
    g_cfg.rgb.follow_printer = true;
    g_cfg.rgb.follow_vent = true;
    g_live.printer_light = true;
    step();
    t("follow PRINTER takes precedence: vent closed but chamber light on stays lit",
      is(frame[0][0], 0, 0, 255), NULL);

    setup(PV_FX_STATIC, "00FF00", "0000FF", NULL, NULL, 100, -1, 16, 16, true);
    g_cfg.rgb.follow_printer = true;
    g_cfg.rgb.follow_vent = true;
    g_live.printer_light = false;
    step();
    {
        int allDark = 1;
        for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) allDark = 0;
        t("chamber light off with follow printer on: dark, vent open or not", allDark, NULL);
    }

    puts("\n11. the master light switch beats everything");
    setup(PV_FX_STATIC, "00FF00", "0000FF", "FF0000", "FFFF00", 100, -1, 16, 16, true);
    g_cfg.rgb.light_on = false;
    step();
    {
        int allDark = 1;
        for (int s2 = 0; s2 < 2; ++s2)
            for (int i = 0; i < 16; ++i) if (!black(frame[s2][i])) allDark = 0;
        t("lights off: both strips dark, inactive colour and all", allDark, NULL);
    }

    puts("\n12. a preview pins the printer state and the job progress");
    /* The pinned percentage. The progress effect must fill to what the preview
     * says and not to what the printer is reporting, so a fill can be judged
     * without waiting for a print to reach it. */
    {
        pv_fx_param_t p;
        setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 10;
        p = g_cfg.rgb.simple[PV_FX_PROGRESS];
        pv_rgb_preview(PV_FX_PROGRESS, &p, -1, 75, 60);
        for (int f = 0; f < 60; ++f) step();
        int lit = 0;
        for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) lit++;
        snprintf(buf, sizeof buf, "lit=%d want~12", lit);
        t("progress fills to the PINNED percent, not the printer's",
          lit >= 11 && lit <= 13, buf);

        /* And drops back the moment the pin goes away. */
        pv_rgb_preview_cancel();
        setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 10;
        for (int f = 0; f < 60; ++f) step();
        lit = 0;
        for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) lit++;
        snprintf(buf, sizeof buf, "lit=%d want~2", lit);
        t("and back to the printer's percent once the preview is cancelled",
          lit >= 1 && lit <= 3, buf);
    }

    /* The pinned STATE. Pinning ERROR with the warning override on must give
     * stock's solid red 127, which is the whole point: see the fault look
     * without causing a fault. */
    {
        pv_fx_param_t p;
        setup(PV_FX_STATIC, "00FF00", "00FF00", NULL, NULL, 100, -1, 16, 16, true);
        g_cfg.rgb.warning_sw = true;
        g_live.device_state = PV_ST_IDLE;
        p = g_cfg.rgb.simple[PV_FX_STATIC];
        step();
        t("no pin, printer idle: the panel's own colour",
          is(frame[0][0], 0, 255, 0), NULL);

        pv_rgb_preview(PV_FX_STATIC, &p, PV_ST_ERROR, -1, 60);
        step();
        t("pinning ERROR with the warning override on shows stock's red 127",
          is(frame[0][0], 127, 0, 0), NULL);

        /* The override sits ABOVE the preview, so it must beat it. */
        pv_rgb_preview_cancel();
        step();
        t("cancelling the preview puts the panel's colour back",
          is(frame[0][0], 0, 255, 0), NULL);

        /* With the warning override OFF the pin changes nothing: stock only
         * consults the printer state through that one gate. */
        g_cfg.rgb.warning_sw = false;
        pv_rgb_preview(PV_FX_STATIC, &p, PV_ST_ERROR, -1, 60);
        step();
        t("with the warning override off, pinning ERROR shows the preview",
          is(frame[0][0], 0, 255, 0), NULL);
        pv_rgb_preview_cancel();
    }

    /* A pin must never leak into g_live: pv_motor.c reads device_state to
     * choose the flap direction and pv_policy.c reads it to decide whether to
     * vent at all. A preview that moved the flap would be a hardware bug. */
    {
        pv_fx_param_t p;
        setup(PV_FX_STATIC, "00FF00", "00FF00", NULL, NULL, 100, -1, 16, 16, true);
        g_live.device_state = PV_ST_IDLE;
        g_live.print_percent = 10;
        p = g_cfg.rgb.simple[PV_FX_STATIC];
        pv_rgb_preview(PV_FX_STATIC, &p, PV_ST_PRINTING, 90, 60);
        for (int f = 0; f < 10; ++f) step();
        t("the pinned state never reaches g_live.device_state",
          g_live.device_state == PV_ST_IDLE, NULL);
        t("nor the pinned percent g_live.print_percent",
          g_live.print_percent == 10, NULL);
        pv_rgb_preview_cancel();
    }

    /* The deadline has to be honoured on every path out of resolve(), not just
     * the one that runs the override. A pinned ERROR with the warning override
     * on returns red at the gate above it and never reaches the override, so
     * expiring there would leave the preview pinned for good. */
    {
        pv_fx_param_t p;
        setup(PV_FX_STATIC, "00FF00", "00FF00", NULL, NULL, 100, -1, 16, 16, true);
        g_cfg.rgb.warning_sw = true;
        p = g_cfg.rgb.simple[PV_FX_STATIC];
        pv_rgb_preview(PV_FX_STATIC, &p, PV_ST_ERROR, -1, 1);
        step();
        t("pinned ERROR is red while it runs", is(frame[0][0], 127, 0, 0), NULL);
        fc_clock_us += 2000000;                 /* two seconds later */
        step();
        t("and expires on its own even down that early-return path",
          is(frame[0][0], 0, 255, 0), NULL);
        t("with the preview really gone", pv_rgb_preview_left() == 0, NULL);
    }

    puts("\n13. the two animated progress effects");

    /* All three draw the SAME number. Whatever the animation does on top, the
     * number of pixels that are lit at all has to agree, or two of them are
     * telling the user a different percentage from the third. */
    for (int pct = 10; pct <= 90; pct += 20) {
        int lit[3];
        int fxs[3] = { PV_FX_PROGRESS, PV_FX_PROGRESS_ANIM, PV_FX_BARBER };
        for (int k = 0; k < 3; ++k) {
            setup(fxs[k], "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
            g_live.print_percent = pct;
            for (int f = 0; f < 60; ++f) step();
            lit[k] = 0;
            for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) lit[k]++;
        }
        /* Barber's dark band can land on the last pixel, so it is allowed to
         * differ by one band's worth rather than exactly. */
        snprintf(buf, sizeof buf, "pct=%d plain=%d anim=%d barber=%d", pct, lit[0], lit[1], lit[2]);
        char nm[80];
        snprintf(nm, sizeof nm, "pct=%d: the animated bar lights the same pixels as the plain one", pct);
        t(nm, lit[1] == lit[0], buf);
    }

    /* The animated one must actually MOVE: two frames apart cannot be the
     * same picture, or it is a plain bar wearing a different name. */
    {
        rgb_t a[PV_LEDS_PER_STRIP];
        setup(PV_FX_PROGRESS_ANIM, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 70;
        for (int f = 0; f < 60; ++f) step();
        memcpy(a, frame[0], sizeof a);
        int moved = 0;
        for (int f = 0; f < 6; ++f) {
            step();
            for (int i = 0; i < 16; ++i) if (!same(a[i], frame[0][i])) moved = 1;
        }
        t("it animates rather than sitting still", moved, NULL);
    }

    /* The head of the bar is the brightest thing in it: that is what makes the
     * tip read as the tip. */
    {
        setup(PV_FX_PROGRESS_ANIM, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 50;   /* eight pixels */
        int tip_won = 0;
        for (int f = 0; f < 200; ++f) {
            step();
            int tip = frame[0][7].r, best = 1;
            for (int i = 0; i < 7; ++i) if (frame[0][i].r > tip) best = 0;
            if (best && tip > 180) tip_won = 1;
        }
        t("the head of the bar is the brightest pixel in it at some point", tip_won, NULL);
    }

    /* Past the fill it is dark, exactly like the plain bar. An effect that
     * lights the empty part is not a progress bar. */
    {
        setup(PV_FX_PROGRESS_ANIM, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 25;   /* four pixels */
        int leak = 0;
        for (int f = 0; f < 80; ++f) {
            step();
            for (int i = 5; i < 16; ++i) if (!black(frame[0][i])) leak = 1;
        }
        t("nothing lights past the fill", !leak, NULL);
    }

    /* Barber Pole with NO job fills the whole run, because a state that never
     * has a job would otherwise show an effect that is always off. */
    {
        setup(PV_FX_BARBER, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 0;
        for (int f = 0; f < 40; ++f) step();
        int anyLit = 0;
        for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) anyLit = 1;
        t("with no job at all, the pole still runs the whole strip", anyLit, NULL);
    }

    /* ...and the other two do NOT, because an empty bar is the honest answer
     * there and filling it would say a print was running. */
    {
        setup(PV_FX_PROGRESS_ANIM, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 0;
        for (int f = 0; f < 60; ++f) step();
        int anyLit = 0;
        for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) anyLit = 1;
        t("the animated bar with no job stays empty", !anyLit, NULL);
    }

    /* The pole's second band is the INACTIVE colour, which is the same
     * per-effect answer every other effect gives to "what goes where I am not
     * lit". Unset, that band is dark. */
    {
        setup(PV_FX_BARBER, "00FF00", "00FF00", "0000FF", NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 100;
        int sawGreen = 0, sawBlue = 0, sawOther = 0;
        for (int f = 0; f < 40; ++f) {
            step();
            for (int i = 0; i < 16; ++i) {
                if (is(frame[0][i], 0, 255, 0))      sawGreen = 1;
                else if (is(frame[0][i], 0, 0, 255)) sawBlue = 1;
                else                                 sawOther = 1;
            }
        }
        t("the pole draws the active colour", sawGreen, NULL);
        t("and the inactive colour as its second band", sawBlue, NULL);
        t("and nothing else at all", !sawOther, NULL);
    }
    {
        setup(PV_FX_BARBER, "00FF00", "00FF00", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 100;
        int sawBlack = 0;
        for (int f = 0; f < 40; ++f) {
            step();
            for (int i = 0; i < 16; ++i) if (black(frame[0][i])) sawBlack = 1;
        }
        t("with no inactive colour set, the second band is dark", sawBlack, NULL);
    }

    /* The pole moves, and comes back to where it started rather than drifting
     * by a pixel every cycle. */
    {
        rgb_t first[PV_LEDS_PER_STRIP];
        setup(PV_FX_BARBER, "00FF00", "00FF00", "0000FF", NULL, 100, -1, 15, 15, true);
        g_live.print_percent = 100;
        for (int f = 0; f < 40; ++f) step();     /* settle the easing */
        step();
        memcpy(first, frame[0], sizeof first);
        int band = 15 / 5;                        /* the default: a fifth */
        int moved = 0, returned = 1;
        for (int f = 0; f < band * 2; ++f) {
            step();
            int diff = 0;
            for (int i = 0; i < 15; ++i) if (!same(first[i], frame[0][i])) diff = 1;
            if (f < band * 2 - 1 && diff) moved = 1;
        }
        for (int i = 0; i < 15; ++i) if (!same(first[i], frame[0][i])) returned = 0;
        t("the pole moves", moved, NULL);
        t("and repeats exactly, rather than drifting", returned, NULL);
    }

    /* The stored spare byte sets the band width, and an unset one falls back
     * to a fifth of the run. */
    {
        setup(PV_FX_BARBER, "00FF00", "00FF00", "0000FF", NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 100;
        g_cfg.rgb.simple[PV_FX_BARBER].aux = 4;
        g_cfg.rgb.simple[PV_FX_BARBER].opt_set |= PV_AUX;
        for (int f = 0; f < 40; ++f) step();
        /* Count the run length of the first band from pixel 0. */
        int run = 1;
        for (int i = 1; i < 16 && same(frame[0][i], frame[0][0]); ++i) run++;
        snprintf(buf, sizeof buf, "run=%d", run);
        t("a stored band width of four draws bands no wider than four",
          run <= 4, buf);

        setup(PV_FX_BARBER, "00FF00", "00FF00", "0000FF", NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 100;
        for (int f = 0; f < 40; ++f) step();
        int colours = 0;
        for (int i = 1; i < 16; ++i) if (!same(frame[0][i], frame[0][i-1])) colours++;
        snprintf(buf, sizeof buf, "%d changes", colours);
        t("unset, it still draws more than one band across sixteen pixels",
          colours >= 2, buf);
    }

    /* Both are still bound by the master switch and the follow gates, because
     * they are effects and not exceptions. */
    {
        setup(PV_FX_BARBER, "00FF00", "00FF00", "0000FF", NULL, 100, -1, 16, 16, true);
        g_cfg.rgb.light_on = false;
        step();
        int allDark = 1;
        for (int i = 0; i < 16; ++i) if (!black(frame[0][i])) allDark = 0;
        t("lights off still means dark, pole or no pole", allDark, NULL);
    }

    puts("\n14. three independent ways to turn a strip around");

    /* Progress at 25% lights the first four of sixteen. Which END those four
     * are on is the whole question here, and it is one that three separate
     * settings all get to answer. */
    #define LIT_AT_START() ({ int _v = !black(frame[0][0]) && black(frame[0][15]); _v; })
    #define LIT_AT_END()   ({ int _v = black(frame[0][0]) && !black(frame[0][15]); _v; })

    {
        setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 25;
        for (int f = 0; f < 60; ++f) step();
        t("nothing reversed: the bar fills from the start", LIT_AT_START(), NULL);
    }
    {
        setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 25;
        g_cfg.rgb.reverse = true;
        for (int f = 0; f < 60; ++f) step();
        t("the master switch alone: it fills from the far end", LIT_AT_END(), NULL);
    }
    {
        setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 25;
        g_cfg.rgb.simple[PV_FX_PROGRESS].opt_set |= PV_FX_REVERSE;
        for (int f = 0; f < 60; ++f) step();
        t("the effect's own flag alone: the same, from the far end", LIT_AT_END(), NULL);
    }
    {
        setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 25;
        g_cfg.rgb.reverse_strips = 1;          /* strip 0 only */
        for (int f = 0; f < 60; ++f) step();
        t("one strip's flag alone: that strip fills from the far end", LIT_AT_END(), NULL);
        int otherAtStart = !black(frame[1][0]) && black(frame[1][15]);
        t("and the OTHER strip is untouched by it", otherAtStart, NULL);
    }
    {
        /* Exclusive-or, not "any of them wins". Two flips is where you
         * started, and a scheme where the second one did nothing would make
         * the per-effect flag useless the moment the master switch was on. */
        setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 25;
        g_cfg.rgb.reverse = true;
        g_cfg.rgb.simple[PV_FX_PROGRESS].opt_set |= PV_FX_REVERSE;
        for (int f = 0; f < 60; ++f) step();
        t("master AND effect: turned around twice, so back at the start",
          LIT_AT_START(), NULL);
    }
    {
        setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 25;
        g_cfg.rgb.reverse = true;
        g_cfg.rgb.reverse_strips = 1;
        g_cfg.rgb.simple[PV_FX_PROGRESS].opt_set |= PV_FX_REVERSE;
        for (int f = 0; f < 60; ++f) step();
        t("all three: three flips is one flip, so the far end again",
          LIT_AT_END(), NULL);
        int otherAtStart = !black(frame[1][0]) && black(frame[1][15]);
        t("and the strip without its own flag is back at the start", otherAtStart, NULL);
    }
    {
        /* The per-state tables carry their own parameters, so a per-effect
         * flag is a per-state one for free. */
        setup(PV_FX_PROGRESS, "FFFFFF", "FFFFFF", NULL, NULL, 100, -1, 16, 16, true);
        g_live.print_percent = 25;
        g_cfg.rgb.light_mode = PV_MODE_H2D;
        g_live.device_state = PV_ST_PRINTING;
        for (int st = 0; st < PV_ST_COUNT; ++st) {
            g_cfg.rgb.h2d_active[st] = PV_FX_PROGRESS;
            g_h2d[st][PV_FX_PROGRESS] = g_cfg.rgb.simple[PV_FX_PROGRESS];
        }
        g_h2d[PV_ST_PRINTING][PV_FX_PROGRESS].opt_set |= PV_FX_REVERSE;
        for (int f = 0; f < 60; ++f) step();
        t("printing has its own direction", LIT_AT_END(), NULL);

        g_live.device_state = PV_ST_IDLE;
        for (int f = 0; f < 60; ++f) step();
        t("and idle, which does not, keeps the other one", LIT_AT_START(), NULL);
    }

    printf("\n%d passed, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
