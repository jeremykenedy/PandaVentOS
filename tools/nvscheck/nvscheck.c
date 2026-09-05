/* Drives pv_wifi.c's OWN stock-Wi-Fi salvage against real NVS partition
 * images taken off real devices.
 *
 *   cc -I stub -I ../../firmware/main -o nvscheck nvscheck.c && ./nvscheck
 *
 * pv_wifi.c is #included, not reimplemented, so what runs here is the code
 * the device runs. The images are 12 KB NVS partitions carved out of full
 * 4 MB flash dumps: one from a vent still on BIQU's firmware, one from a vent
 * already converted, and one stock image with the Wi-Fi fields populated.
 *
 * This exists because the path cannot be exercised on hardware any more:
 * salvage only runs on a vent booting our firmware over a full stock NVS for
 * the first time, and both vents here have already made that transition.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pv.h"

const uint8_t *nvs_test_image;
uint32_t       nvs_test_size;

/* everything pv_wifi.c reaches for that this test does not exercise */
pv_cfg_t  g_cfg;
pv_live_t g_live;
void pv_ws_push_state(void) {}
void pv_ws_broadcast(char *j) { (void)j; }
char *pv_json_state(void) { return NULL; }
void pv_cfg_save(void) {}
void pv_bambu_rebind(void) {}
void pv_motor_update(void) {}

#include "pv_wifi.c"

static int pass, fail;
static void t(const char *what, int ok, const char *got) {
    if (ok) { pass++; printf("  ok   %s\n", what); }
    else    { fail++; printf("  FAIL %s  (got %s)\n", what, got ? got : "-"); }
}

static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static uint8_t buf[0x3000];
    memset(buf, 0xFF, sizeof buf);
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    nvs_test_image = buf; nvs_test_size = (uint32_t)sizeof buf;
    return n > 0;
}

int main(int argc, char **argv) {
    const char *stock  = argc > 1 ? argv[1] : "nvs-stock.bin";
    const char *pop    = argc > 2 ? argv[2] : "nvs-stock-populated.bin";
    const char *ours   = argc > 3 ? argv[3] : "nvs-ours.bin";
    char ssid[33], pass_[64];

    printf("A stock vent that was never given Wi-Fi\n");
    if (!load(stock)) { printf("  (image missing: %s)\n", stock); return 2; }
    t("nothing to salvage, and it says so",
      pv_wifi_salvage_stock(ssid, sizeof ssid, pass_, sizeof pass_) == false, ssid);

    printf("\nA stock vent that WAS on a network\n");
    if (!load(pop)) { printf("  (image missing: %s)\n", pop); return 2; }
    int got = pv_wifi_salvage_stock(ssid, sizeof ssid, pass_, sizeof pass_);
    t("the credentials are found", got, "not found");
    t("the ssid is the one the vent was on", got && !strcmp(ssid, "example-iot"), ssid);
    t("the password comes back whole",
      got && !strcmp(pass_, "not-a-real-password"), pass_);

    printf("\nA vent already converted (stock's namespace is long gone)\n");
    if (!load(ours)) { printf("  (image missing: %s)\n", ours); return 2; }
    t("nothing to salvage, and it does not invent any",
      pv_wifi_salvage_stock(ssid, sizeof ssid, pass_, sizeof pass_) == false, ssid);

    printf("\nAn empty partition\n");
    { static uint8_t blank[0x3000]; memset(blank, 0xFF, sizeof blank);
      nvs_test_image = blank; nvs_test_size = sizeof blank; }
    t("does not walk off the end of a blank partition",
      pv_wifi_salvage_stock(ssid, sizeof ssid, pass_, sizeof pass_) == false, ssid);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
