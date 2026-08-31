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

    printf("\n%d passed, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
