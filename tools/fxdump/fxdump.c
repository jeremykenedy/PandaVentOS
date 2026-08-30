/* Renders the FIRMWARE'S OWN effects natively and prints the frames, so an
 * animation can be inspected without a strip in front of you.
 *
 * Same principle as tools/uicmp: this does not carry a copy of the renderer,
 * it #includes firmware/main/pv_rgb.c and drives render_effect directly. What
 * you are looking at is the shipping source, not a description of it.
 *
 *   cc -I stub -I ../../firmware/main -o fxdump fxdump.c -lm
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
bool pv_bambu_started(void) { return false; }
bool pv_motor_fault_any(void) { return false; }
bool pv_wifi_saw_test_ap(void) { return false; }
void pv_wifi_scan_start(void) {}
int  pv_wifi_test_scan_state(void) { return 0; }

#include "pv_rgb.c"

static const char *NAMES[PV_FX_COUNT] = {
    "Static", "Breathing", "Strobing", "Wave",
    "Marquee", "Color_Cycle", "Rainbow", "Cylon", "Bounce",
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

    rgb_t color = { 0xFF, 0x37, 0x00 };   /* the factory default, FF3700 */
    rgb_t px[N];
    memset(px, 0, sizeof(px));

    if (ppm) {
        /* A filmstrip: one row per frame, so the shape of the motion is
         * visible as a whole rather than one frame at a time. */
        const int SCALE = 12;
        printf("P6\n%d %d\n255\n", N * SCALE, frames * SCALE);
        for (int f = 0; f < frames; ++f) {
            render_effect(fx, color, 100, 50, false, px, N);
            for (int sy = 0; sy < SCALE; ++sy)
                for (int i = 0; i < N; ++i)
                    for (int sx = 0; sx < SCALE; ++sx)
                        fwrite(&px[i], 1, 3, stdout);
        }
        return 0;
    }

    printf("effect %d (%s), %d frames, brightness 100, speed 50, %d pixels\n",
           fx, fx >= 0 && fx < PV_FX_COUNT ? NAMES[fx] : "?", frames, N);
    printf("     +%.*s+\n", N, "----------------------------------------");
    uint32_t total = 0;
    for (int f = 0; f < frames; ++f) {
        uint32_t ms = render_effect(fx, color, 100, 50, false, px, N);
        total += ms;
        printf("%3d  |", f);
        row_ascii(px, N);
        printf("|  %u ms\n", ms);
    }
    printf("     +%.*s+\n", N, "----------------------------------------");
    printf("%d frames in %u ms, %.1f fps\n", frames, total,
           total ? frames * 1000.0 / total : 0.0);
    return 0;
}
