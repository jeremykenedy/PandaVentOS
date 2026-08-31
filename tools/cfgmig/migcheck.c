/* Drives the REAL migration in pv_cfg.c with blobs in every stored layout,
 * and checks that what comes out the far side is what went in.
 *
 * This exists because of what happened at v4: PV_FX_COUNT is an array
 * dimension in the MIDDLE of the config struct, so adding an effect does not
 * append to the blob, it moves everything after rgb.simple. A device that had
 * been configured for months would have come back from the update with every
 * setting at the factory value and one line in a serial log nobody was
 * watching. There is no way to notice that from the outside until it is too
 * late, so it is checked from the inside, here, before it ships.
 *
 *   cc -I stub -I ../../firmware/main -o migcheck migcheck.c && ./migcheck
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "pv.h"
#include "nvs.h"
#include "nvs_flash.h"

/* ---- a tiny key -> blob store, because the H2D tables have their own keys ---- */
#define MAXK 12
static struct { char key[16]; unsigned char b[4096]; size_t len; bool have; } S[MAXK];

static int slot(const char *k)
{
    for (int i = 0; i < MAXK; ++i) if (S[i].have && !strcmp(S[i].key, k)) return i;
    return -1;
}
static void put(const char *k, const void *v, size_t len)
{
    int i = slot(k);
    if (i < 0) for (i = 0; i < MAXK; ++i) if (!S[i].have) break;
    snprintf(S[i].key, sizeof S[i].key, "%s", k);
    memcpy(S[i].b, v, len); S[i].len = len; S[i].have = true;
}
static void wipe(void) { memset(S, 0, sizeof S); }

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h){ (void)ns;(void)mode; *h=1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len){
    (void)h;
    int i = slot(k);
    if (i < 0) return -1;
    if (*len < S[i].len) return -1;
    memcpy(out, S[i].b, S[i].len); *len = S[i].len; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len){
    (void)h; put(k, v, len); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h){ (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h){ (void)h; }
esp_err_t nvs_flash_erase(void){ return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k){
    (void)h; int i = slot(k); if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    S[i].have = false; return ESP_OK;
}
void esp_restart(void){ }
void pv_ws_push_state(void){ }
void pv_rgb_notify(void){ }
void pv_motor_update(void){ }

/* pv_cfg.c is compiled INTO this test, so the frozen legacy shapes it keeps
 * private are the ones the test builds its blobs from. A test that redeclared
 * them would be testing its own copy. */
#include "pv_cfg.c"

static int ok_n, bad_n;
static void t(const char *n, int c, const char *g)
{
    if (c) { ok_n++; printf("  ok   %s\n", n); }
    else   { bad_n++; printf("  FAIL %s%s%s\n", n, g ? "   got: " : "", g ? g : ""); }
}

static char B[160];

int main(void)
{
    printf("sizeof(pv_cfg_t) = %zu, budget %d\n", sizeof(pv_cfg_t), PV_CFG_MAX_BYTES);
    printf("sizeof(pv_h2d_blob_t) = %zu, %d effects\n\n",
           sizeof(pv_h2d_blob_t), PV_FX_COUNT);

    puts("1. a v9 device keeps everything it had");
    wipe();
    {
        pv_cfg_v9_t o;
        memset(&o, 0, sizeof o);
        o.magic = CFG_MAGIC_V9;
        o.rgb.light_on = true; o.rgb.warning_sw = false;
        o.rgb.follow_printer = true; o.rgb.follow_vent = false; o.rgb.reverse = true;
        o.rgb.light_mode = PV_MODE_H2D;
        o.rgb.simple_current = 11;
        for (int i = 0; i < PV_FX_COUNT_V9; ++i) {
            o.rgb.simple[i].brightness = (uint8_t)(10 + i);
            o.rgb.simple[i].speed      = (uint8_t)(90 - i);
            o.rgb.simple[i].rgb[0] = (uint8_t)i; o.rgb.simple[i].rgb[1] = 0x10; o.rgb.simple[i].rgb[2] = 0x20;
            o.rgb.simple[i].rgb_closed[0] = 0x30; o.rgb.simple[i].rgb_closed[1] = (uint8_t)i; o.rgb.simple[i].rgb_closed[2] = 0x40;
            o.rgb.simple[i].bg[0] = 0xAA; o.rgb.simple[i].bg[1] = 0xBB; o.rgb.simple[i].bg[2] = (uint8_t)i;
            o.rgb.simple[i].bright_end = (uint8_t)(50 + i);
            o.rgb.simple[i].opt_set = PV_BG_OPEN | PV_BRIGHT_END;
        }
        for (int s = 0; s < PV_ST_COUNT; ++s) o.rgb.h2d_active[s] = (uint8_t)(s + 2);
        o.rgb.warnhot_current[0] = 1; o.rgb.warnhot_current[1] = 0;
        o.rgb.warnhot_bg[0][1] = 77;  o.rgb.warnhot_speed[1][0] = 33;
        snprintf(o.printer.name, sizeof o.printer.name, "%s", "MyPrinter");
        snprintf(o.printer.sn, sizeof o.printer.sn, "%s", "SN123456789");
        snprintf(o.printer.ip, sizeof o.printer.ip, "%s", "10.0.0.9");
        snprintf(o.hostname, sizeof o.hostname, "%s", "somehost");
        snprintf(o.language, sizeof o.language, "%s", "de");
        snprintf(o.device_name, sizeof o.device_name, "%s", "Left Unit");
        o.motor_manual = true; o.motor_manual_open = true;
        o.ring_mode = 3; o.ring_blink = true;
        o.leds[0] = 16; o.leds[1] = 11;
        put(CFG_KEY, &o, sizeof o);

        pv_h2d_blob_v9_t hb;
        for (int st = 0; st < PV_ST_COUNT; ++st) {
            memset(&hb, 0, sizeof hb);
            hb.magic = H2D_MAGIC_V9;
            for (int f = 0; f < PV_FX_COUNT_V9; ++f) {
                hb.fx[f].brightness = (uint8_t)(st * 10 + f);
                hb.fx[f].speed = (uint8_t)(f * 3);
                hb.fx[f].rgb[0] = (uint8_t)st; hb.fx[f].rgb[1] = (uint8_t)f; hb.fx[f].rgb[2] = 0x55;
                hb.fx[f].bg_closed[0] = 0x66; hb.fx[f].bg_closed[1] = (uint8_t)st; hb.fx[f].bg_closed[2] = (uint8_t)f;
                hb.fx[f].bright_end = (uint8_t)(f + 1);
                hb.fx[f].opt_set = PV_BG_CLOSED | PV_BRIGHT_END;
            }
            char k[8]; h2d_key(st, k);
            put(k, &hb, sizeof hb);
        }
    }
    pv_cfg_load();

    t("the magic moved to the current layout", g_cfg.magic == CFG_MAGIC, NULL);
    t("every switch survived",
      g_cfg.rgb.light_on && !g_cfg.rgb.warning_sw && g_cfg.rgb.follow_printer
      && !g_cfg.rgb.follow_vent && g_cfg.rgb.reverse, NULL);
    t("the light mode survived", g_cfg.rgb.light_mode == PV_MODE_H2D, NULL);
    t("the selected simple effect survived", g_cfg.rgb.simple_current == 11, NULL);
    {
        int bad = -1;
        for (int i = 0; i < PV_FX_COUNT_V9; ++i) {
            const pv_fx_param_t *p = &g_cfg.rgb.simple[i];
            if (p->brightness != 10 + i || p->speed != 90 - i ||
                p->rgb[0] != i || p->rgb[1] != 0x10 || p->rgb[2] != 0x20 ||
                p->rgb_closed[0] != 0x30 || p->rgb_closed[1] != i || p->rgb_closed[2] != 0x40 ||
                p->bg[0] != 0xAA || p->bg[1] != 0xBB || p->bg[2] != i ||
                p->bright_end != 50 + i ||
                p->opt_set != (PV_BG_OPEN | PV_BRIGHT_END)) { bad = i; break; }
        }
        snprintf(B, sizeof B, "effect %d", bad);
        t("all eighteen simple effects kept every byte they had", bad < 0, bad < 0 ? NULL : B);
    }
    {
        int bad = -1;
        for (int st = 0; st < PV_ST_COUNT; ++st)
            for (int f = 0; f < PV_FX_COUNT_V9 && bad < 0; ++f) {
                const pv_fx_param_t *p = &g_h2d[st][f];
                if (p->brightness != st * 10 + f || p->speed != f * 3 ||
                    p->rgb[0] != st || p->rgb[1] != f || p->rgb[2] != 0x55 ||
                    p->bg_closed[0] != 0x66 || p->bg_closed[1] != st || p->bg_closed[2] != f ||
                    p->bright_end != f + 1 ||
                    p->opt_set != (PV_BG_CLOSED | PV_BRIGHT_END)) { bad = st * 100 + f; }
            }
        snprintf(B, sizeof B, "state %d effect %d", bad / 100, bad % 100);
        t("all six H2D tables kept every byte they had", bad < 0, bad < 0 ? NULL : B);
    }
    t("the per-state active effects survived",
      g_cfg.rgb.h2d_active[0] == 2 && g_cfg.rgb.h2d_active[5] == 7, NULL);
    t("the warning-hot settings survived",
      g_cfg.rgb.warnhot_current[0] == 1 && g_cfg.rgb.warnhot_bg[0][1] == 77
      && g_cfg.rgb.warnhot_speed[1][0] == 33, NULL);
    t("the printer survived", !strcmp(g_cfg.printer.name, "MyPrinter")
      && !strcmp(g_cfg.printer.sn, "SN123456789")
      && !strcmp(g_cfg.printer.ip, "10.0.0.9"), g_cfg.printer.name);
    t("the hostname, language and device name survived",
      !strcmp(g_cfg.hostname, "somehost") && !strcmp(g_cfg.language, "de")
      && !strcmp(g_cfg.device_name, "Left Unit"), g_cfg.device_name);
    t("the manual vent position survived",
      g_cfg.motor_manual && g_cfg.motor_manual_open, NULL);
    t("the ring settings survived", g_cfg.ring_mode == 3 && g_cfg.ring_blink, NULL);
    snprintf(B, sizeof B, "%d, %d", g_cfg.leds[0], g_cfg.leds[1]);
    t("the LED counts survived", g_cfg.leds[0] == 16 && g_cfg.leds[1] == 11, B);

    puts("\n2. what is NEW arrives new, not as leftover bytes");
    {
        int aux_set = 0;
        for (int i = 0; i < PV_FX_COUNT_V9; ++i)
            if (g_cfg.rgb.simple[i].opt_set & PV_AUX) aux_set++;
        snprintf(B, sizeof B, "%d effects", aux_set);
        t("no migrated effect claims a spare byte it never had", aux_set == 0, B);
    }
    t("the two new effects exist", PV_FX_COUNT == 20, NULL);
    t("and carry a real brightness rather than zero",
      g_cfg.rgb.simple[PV_FX_PROGRESS_ANIM].brightness > 0
      && g_cfg.rgb.simple[PV_FX_BARBER].brightness > 0, NULL);
    t("and a real colour rather than black",
      (g_cfg.rgb.simple[PV_FX_BARBER].rgb[0] |
       g_cfg.rgb.simple[PV_FX_BARBER].rgb[1] |
       g_cfg.rgb.simple[PV_FX_BARBER].rgb[2]) != 0, NULL);
    {
        int blank = 0;
        for (int st = 0; st < PV_ST_COUNT; ++st)
            for (int f = PV_FX_COUNT_V9; f < PV_FX_COUNT; ++f)
                if (!(g_h2d[st][f].rgb[0] | g_h2d[st][f].rgb[1] | g_h2d[st][f].rgb[2]))
                    blank++;
        snprintf(B, sizeof B, "%d of %d", blank, PV_ST_COUNT * (PV_FX_COUNT - PV_FX_COUNT_V9));
        t("the new H2D entries are not black in any state", blank == 0, B);
    }

    puts("\n3. it was written back in the new shape, so it migrates once");
    t("the config was rewritten", slot(CFG_KEY) >= 0
      && S[slot(CFG_KEY)].len == sizeof(pv_cfg_t), NULL);
    {
        char k[8]; h2d_key(3, k);
        int i = slot(k);
        snprintf(B, sizeof B, "%zu vs %zu", i >= 0 ? S[i].len : 0, sizeof(pv_h2d_blob_t));
        t("and so was every H2D table", i >= 0 && S[i].len == sizeof(pv_h2d_blob_t), B);
    }
    {
        /* Load again from what was just written: the second boot must take the
         * fast path and change nothing. */
        pv_fx_param_t before = g_cfg.rgb.simple[7];
        pv_fx_param_t bh2d   = g_h2d[2][9];
        pv_cfg_load();
        t("a second load reads it straight back, unchanged",
          !memcmp(&before, &g_cfg.rgb.simple[7], sizeof before)
          && !memcmp(&bh2d, &g_h2d[2][9], sizeof bh2d), NULL);
    }

    puts("\n4. a v8 device, two layouts back, also survives");
    wipe();
    {
        pv_cfg_v8_t o;
        memset(&o, 0, sizeof o);
        o.magic = CFG_MAGIC_V8;
        o.rgb.light_on = true;
        o.rgb.simple_current = 5;
        for (int i = 0; i < PV_FX_COUNT_V9; ++i) {
            o.rgb.simple[i].brightness = (uint8_t)(20 + i);
            o.rgb.simple[i].rgb[0] = 0xFF; o.rgb.simple[i].rgb[1] = (uint8_t)i;
            o.rgb.simple[i].opt_set = PV_BG_OPEN;
        }
        snprintf(o.device_name, sizeof o.device_name, "%s", "Older Unit");
        o.leds[0] = 16; o.leds[1] = 16;
        put(CFG_KEY, &o, sizeof o);
    }
    pv_cfg_load();
    t("its colours and brightnesses came across",
      g_cfg.rgb.simple[3].brightness == 23 && g_cfg.rgb.simple[3].rgb[0] == 0xFF
      && g_cfg.rgb.simple[3].rgb[1] == 3, NULL);
    t("its name came across", !strcmp(g_cfg.device_name, "Older Unit"), g_cfg.device_name);
    t("the ramp it never had is not claimed to be set",
      !(g_cfg.rgb.simple[3].opt_set & PV_BRIGHT_END), NULL);
    t("nor the spare byte", !(g_cfg.rgb.simple[3].opt_set & PV_AUX), NULL);
    t("and it is now the current layout", g_cfg.magic == CFG_MAGIC, NULL);

    puts("\n5. rubbish is refused rather than half-read");
    wipe();
    {
        unsigned char junk[400];
        for (size_t i = 0; i < sizeof junk; ++i) junk[i] = (unsigned char)(i * 7);
        put(CFG_KEY, junk, sizeof junk);
    }
    pv_cfg_load();
    t("an unrecognised blob falls back to the factory config",
      g_cfg.magic == CFG_MAGIC && g_cfg.rgb.light_on, NULL);
    t("with sane LED counts rather than zero",
      g_cfg.leds[0] > 0 && g_cfg.leds[1] > 0, NULL);

    printf("\n%d passed, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
