/* Renders the FIRMWARE'S OWN effects natively and prints the frames, so an
 * animation can be inspected without a strip in front of you.
 *
 * Same principle as tools/uicmp: this does not carry a copy of the renderer,
 * it #includes firmware/main/pv_rgb.c and drives render_effect directly. What
 * you are looking at is the shipping source, not a description of it.
 *
 *   cc -I stub -I ../../firmware/main -o fxdump \
 *      fxdump.c ../../firmware/main/pv_anim.c -lm
 *   ./fxdump 8 40        # effect 8 (Bounce), 40 frames
 *   ./fxdump 7 40 ppm    # effect 7 (Cylon) as a PPM filmstrip on stdout
 *
 * render_effect is static, which is the point: including the .c file is how a
 * static function gets tested without weakening it to non-static in shipping
 * code just to make it reachable from a test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pv_rgb.c owns these; the firmware defines them in pv_cfg.c. */
#include "pv.h"
int64_t   fc_clock_us;      /* the movable stub clock */
pv_cfg_t  g_cfg;
pv_live_t g_live;

/* Everything pv_rgb.c calls that lives in another translation unit. */
void pv_ws_push_state(void) {}
void pv_ws_broadcast(char *j) { free(j); }
char *pv_json_state(void) { return NULL; }
char *pv_json_response(const char *t, int ok) { (void)t; (void)ok; return NULL; }
void pv_cfg_save(void) {}
void pv_motor_update(void) {}
int  pv_policy_match(const char *m) { (void)m; return -1; }
pv_fx_param_t g_h2d[PV_ST_COUNT][PV_FX_COUNT];
void pv_cfg_h2d_save(int st) { (void)st; }
bool pv_bambu_started(void) { return false; }
bool pv_motor_fault_any(void) { return false; }
bool pv_wifi_saw_test_ap(void) { return false; }
void pv_wifi_scan_start(void) {}
int  pv_wifi_test_scan_state(void) { return 0; }

#include "pv_rgb.c"

static const char *NAMES[PV_FX_COUNT] = {
    "Static", "Breathing", "Strobing", "Wave",
    "Marquee", "Color_Cycle", "Rainbow", "Cylon", "Bounce",
    "Progress_Bar",
    "Marquee_Out", "Marquee_In", "Fill_Out", "Fill_In",
    "Bounce_Out", "Bounce_In", "Bounce_Fill_Out", "Bounce_Fill_In",
};

#define N PV_LEDS_PER_STRIP

/* One row of the strip as text. Nine levels is enough to read a fade. */
static void row_ascii(const rgb_t *px, int n)
{
    static const char RAMP[] = " .:-=+*#%@";
    for (int i = 0; i < n; ++i) {
        int v = px[i].r;
        if (px[i].g > v) v = px[i].g;
        if (px[i].b > v) v = px[i].b;
        int k = v * 9 / 255;
        putchar(RAMP[k]);
    }
}

int main(int argc, char **argv)
{
    int fx = argc > 1 ? atoi(argv[1]) : PV_FX_BOUNCE;
    int frames = argc > 2 ? atoi(argv[2]) : 40;
    int ppm = argc > 3 && strcmp(argv[3], "ppm") == 0;
    /* Progress Bar reads the live print percentage, so it needs one to read.
     * PVPCT also lets a sweep be scripted from the shell. */
    int rev = getenv("PVREV") && atoi(getenv("PVREV"));
    if (getenv("PVPCT")) g_live.print_percent = atoi(getenv("PVPCT"));

    rgb_t color = { 0xFF, 0x37, 0x00 };   /* the factory default, FF3700 */
    /* PVN sets the strip length, PVBG the inactive colour as RRGGBB.
     * PVB is the starting brightness and PVBE the ramp's end brightness; with
     * PVBE set every frame goes through the SHIPPING ramp_apply, so what is
     * printed is the firmware's own answer, not a re-implementation of it. */
    int bright0 = getenv("PVB") ? atoi(getenv("PVB")) : 100;
    int bend    = getenv("PVBE") ? atoi(getenv("PVBE")) : -1;
    int NN = getenv("PVN") ? atoi(getenv("PVN")) : N;
    if (NN < 1 || NN > N) NN = N;
    rgb_t bg = {0,0,0};
    if (getenv("PVBG")) {
        unsigned v = (unsigned)strtoul(getenv("PVBG"), NULL, 16);
        bg.r = (v>>16)&255; bg.g = (v>>8)&255; bg.b = v&255;
    }
    rgb_t px[N];
    memset(px, 0, sizeof(px));

    if (ppm) {
        /* A filmstrip: one row per frame, so the shape of the motion is
         * visible as a whole rather than one frame at a time. */
        const int SCALE = 12;
        printf("P6\n%d %d\n255\n", N * SCALE, frames * SCALE);
        for (int f = 0; f < frames; ++f) {
            render_effect(fx, color, bg, ramp_apply((uint8_t)bright0, bend), 50, rev, px, NN);
            for (int sy = 0; sy < SCALE; ++sy)
                for (int i = 0; i < N; ++i)
                    for (int sx = 0; sx < SCALE; ++sx)
                        fwrite(&px[i], 1, 3, stdout);
        }
        return 0;
    }

    if (bend >= 0)
        printf("effect %d (%s), %d frames, brightness %d -> %d over %g frames, speed 50, %d pixels\n",
               fx, fx >= 0 && fx < PV_FX_COUNT ? NAMES[fx] : "?", frames,
               bright0, bend, (double)PV_RAMP_STEPS, NN);
    else
        printf("effect %d (%s), %d frames, brightness %d, speed 50, %d pixels\n",
               fx, fx >= 0 && fx < PV_FX_COUNT ? NAMES[fx] : "?", frames, bright0, NN);
    printf("     +%.*s+\n", NN, "----------------------------------------");
    uint32_t total = 0;
    for (int f = 0; f < frames; ++f) {
        uint8_t bnow = ramp_apply((uint8_t)bright0, bend);
        uint32_t ms = render_effect(fx, color, bg, bnow, 50, rev, px, NN);
        total += ms;
        printf("%3d %3d%%  |", f, bnow);
        row_ascii(px, NN);
        printf("|  %u ms\n", ms);
    }
    printf("     +%.*s+\n", NN, "----------------------------------------");
    printf("%d frames in %u ms, %.1f fps\n", frames, total,
           total ? frames * 1000.0 / total : 0.0);
    return 0;
}
