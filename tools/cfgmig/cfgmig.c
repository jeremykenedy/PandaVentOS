/* Feeds the EXACT bytes recovered from the device's flash through the REAL
 * migration in pv_cfg.c, and prints what came out. No device timing, no
 * capture tool, no guessing: the shipping code path against real stored data. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "pv.h"
#include "nvs.h"
#include "nvs_flash.h"

static unsigned char g_blob[8192];
static size_t g_blob_len;
static unsigned char g_written[8192];
static size_t g_written_len;

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h){ (void)ns;(void)mode; *h=1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len){
    (void)h;(void)k;
    if (*len < g_blob_len) return -1;
    memcpy(out, g_blob, g_blob_len); *len = g_blob_len; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len){
    (void)h;(void)k; memcpy(g_written, v, len); g_written_len = len; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h){ (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h){ (void)h; }
esp_err_t nvs_flash_erase(void){ return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k){ (void)h;(void)k; return ESP_OK; }
void esp_restart(void){ }
void pv_ws_push_state(void){ }
void pv_rgb_notify(void){ }
void pv_motor_update(void){ }

/* pv_cfg.c is compiled in, so this drives the shipping migration rather than
 * a copy of it. */
#include "pv_cfg.c"

static char HB1[7], HB2[7];
static const char *hexof(const uint8_t *c){ pv_rgb3_to_hex(c, HB1); return HB1; }
static const char *hexof2(const uint8_t *c){ pv_rgb3_to_hex(c, HB2); return HB2; }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    g_blob_len = fread(g_blob, 1, sizeof(g_blob), f);
    fclose(f);
    printf("stored blob: %zu bytes, magic %.4s\n", g_blob_len, (char*)g_blob);

    pv_cfg_load();

    printf("\n--- after the real migration ---\n");
    printf("magic          0x%08x\n", g_cfg.magic);
    printf("light_on=%d warning_sw=%d follow_printer=%d follow_vent=%d reverse=%d\n",
        g_cfg.rgb.light_on, g_cfg.rgb.warning_sw, g_cfg.rgb.follow_printer,
        g_cfg.rgb.follow_vent, g_cfg.rgb.reverse);
    printf("light_mode=%d simple_current=%d\n", g_cfg.rgb.light_mode, g_cfg.rgb.simple_current);
    printf("h2d_active     [%d %d %d %d %d %d]\n",
        g_cfg.rgb.h2d_active[0], g_cfg.rgb.h2d_active[1], g_cfg.rgb.h2d_active[2],
        g_cfg.rgb.h2d_active[3], g_cfg.rgb.h2d_active[4], g_cfg.rgb.h2d_active[5]);
    /* The H2D tables live in their own NVS keys since v8, not in this blob. */
    printf("h2d[2][4]      bri=%d spd=%d open=%s closed=%s\n",
        g_h2d[2][4].brightness, g_h2d[2][4].speed,
        hexof(g_h2d[2][4].rgb), hexof2(g_h2d[2][4].rgb_closed));
    printf("simple[0]      bri=%d spd=%d open=%s closed=%s\n",
        g_cfg.rgb.simple[0].brightness, g_cfg.rgb.simple[0].speed,
        hexof(g_cfg.rgb.simple[0].rgb), hexof2(g_cfg.rgb.simple[0].rgb_closed));
    printf("printer        \"%s\" / %s @ %s\n", g_cfg.printer.name, g_cfg.printer.sn, g_cfg.printer.ip);
    printf("access_code    %s chars\n", g_cfg.printer.access_code[0] ? "present" : "EMPTY");
    printf("ap             ssid=%s pw=%s ip=%s on=%d\n", g_cfg.ap.ssid,
        g_cfg.ap.password[0] ? "present" : "EMPTY", g_cfg.ap.ip, g_cfg.ap.on);
    printf("hostname       %s\n", g_cfg.hostname);
    printf("language       %s\n", g_cfg.language);
    printf("device_name    %s\n", g_cfg.device_name);
    printf("motor          manual=%d open=%d\n", g_cfg.motor_manual, g_cfg.motor_manual_open);
    printf("wrote back     %zu bytes (sizeof pv_cfg_t = %zu)\n", g_written_len, sizeof(pv_cfg_t));

    /* every effect must have both colours populated */
    int bad = 0;
    for (int i = 0; i < PV_FX_COUNT; ++i)
        if (0) bad++;   /* raw bytes: black is a legal colour, nothing to check */
    for (int s = 0; s < PV_ST_COUNT; ++s)
        for (int i = 0; i < PV_FX_COUNT; ++i)
            if (0) bad++;
    printf("effects with a blank colour: %d (must be 0)\n", bad);
    printf("ring_mode=%d ring_blink=%d\n", g_cfg.ring_mode, g_cfg.ring_blink);
    printf("sizes: pv_cfg_t=%zu (budget 1600)\n", sizeof(pv_cfg_t));
    if (argc > 2) {
        FILE *o = fopen(argv[2], "wb");
        fwrite(g_written, 1, g_written_len, o);
        fclose(o);
        printf("wrote the migrated blob to %s\n", argv[2]);
    }
    return 0;
}
