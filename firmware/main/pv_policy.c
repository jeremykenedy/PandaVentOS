// Material-aware vent policy.
//
// THIS IS NOT PART OF THE STOCK CLONE. Stock has no idea what filament is
// loaded; its AUTO rule is one line, "open while PRINTING or PAUSED, closed
// otherwise" (pv_motor_update, from the stock state machine).
// Everything here sits on top of that and is off-by-switch, so turning the
// master switch off restores stock behaviour exactly.
//
// Modelled on DragonVent's dv_policy, whose rules and thresholds these are:
//   during a print   a sealing material closes the vent, a venting material
//                    opens it, an unknown material leaves stock alone
//   after a print    bed-temperature hysteresis holds the vent open until the
//                    residual chamber heat is gone (45 C open, 35 C close)
//
// Stored in its own NVS key rather than appended to pv_cfg_t on purpose: the
// config blob is a fixed-size struct guarded by a magic, so growing it would
// have thrown away every existing setting on the first boot after the update.

#include "pv.h"

#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pv_policy";

#define POL_NS    "pv"
#define POL_KEY   "pol"
#define POL_MAGIC 0x5056504FU   // 'PVPO'

// The filament families, in match order. First rule whose name matches the
// front of the reported material wins, so PETG must be tested before PET. Same
// list and the same order as DragonVent's DEFAULT_FILAMENT_RULES.
const char *const pv_material_name[PV_MAT_COUNT] = {
    "PLA", "PETG", "PET", "TPU", "ABS", "ASA", "PC", "PA", "HIPS",
};
const bool pv_material_seal[PV_MAT_COUNT] = {
    false, false, false, false, true, true, true, true, true,
};

pv_policy_cfg_t g_pol;

void pv_policy_defaults(pv_policy_cfg_t *p)
{
    memset(p, 0, sizeof(*p));
    p->magic       = POL_MAGIC;
    p->enable      = true;                  // DragonVent's policy is always on
    p->rule_on     = (1u << PV_MAT_COUNT) - 1;   // all nine rules active
    p->heat_hold   = true;
    p->bed_open_c  = PV_BED_OPEN_C_DEFAULT;
    p->bed_close_c = PV_BED_CLOSE_C_DEFAULT;
}

static void clamp_loaded(void)
{
    g_pol.rule_on &= (1u << PV_MAT_COUNT) - 1;
    if (g_pol.bed_open_c  < 0)   g_pol.bed_open_c  = 0;
    if (g_pol.bed_open_c  > 120) g_pol.bed_open_c  = 120;
    if (g_pol.bed_close_c < 0)   g_pol.bed_close_c = 0;
    if (g_pol.bed_close_c > 120) g_pol.bed_close_c = 120;
    // The band has to have a direction or the hysteresis degenerates into a
    // single threshold and the vent chatters around it.
    if (g_pol.bed_close_c >= g_pol.bed_open_c) {
        g_pol.bed_close_c = g_pol.bed_open_c > 0 ? g_pol.bed_open_c - 1 : 0;
    }
}

void pv_policy_load(void)
{
    pv_policy_defaults(&g_pol);
    nvs_handle_t h;
    if (nvs_open(POL_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved policy, defaults");
        return;
    }
    pv_policy_cfg_t stored;
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_blob(h, POL_KEY, &stored, &size);
    nvs_close(h);
    if (err == ESP_OK && size == sizeof(stored) && stored.magic == POL_MAGIC) {
        g_pol = stored;
        clamp_loaded();
        ESP_LOGI(TAG, "policy loaded: enable=%d rules=0x%03x hold=%d %d/%d C",
                 g_pol.enable, g_pol.rule_on, g_pol.heat_hold,
                 g_pol.bed_open_c, g_pol.bed_close_c);
    } else {
        ESP_LOGI(TAG, "policy defaults (err=%d size=%u)", err, (unsigned)size);
    }
}

void pv_policy_save(void)
{
    clamp_loaded();
    nvs_handle_t h;
    if (nvs_open(POL_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, POL_KEY, &g_pol, sizeof(g_pol));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "policy save failed: %d", err);
}

// Index of the first enabled rule matching the front of the reported material,
// or -1. Case-insensitive, so "pla" and "PLA+" both hit PLA.
//
// A plain prefix test is WRONG, and the printer proves it. Bambu Studio ships
// 33 distinct filament_type values, which is exactly what lands in tray_type,
// and one of them is "PCTG". A prefix test hands PCTG to the PC rule and seals
// the chamber for a PETG-family copolyester that wants venting.
//
// So the family name has to end on a boundary: end of string, or a character
// that is not a letter. Checked against all 33 real values, this changes PCTG
// and nothing else. The separators that actually occur are '-' (PLA-CF,
// ABS-GF, ASA-AERO, TPU-AMS), a digit (PA6-CF), and in sub-brand strings a
// space or '+' (PLA Basic, PLA Silk+). A letter continuing the token means a
// different material, not a variant of this one.
int pv_policy_match(const char *material)
{
    if (!material || !material[0]) return -1;
    char up[24];
    size_t i = 0;
    for (; material[i] && i < sizeof(up) - 1; ++i) {
        up[i] = (char)toupper((unsigned char)material[i]);
    }
    up[i] = '\0';
    for (int r = 0; r < PV_MAT_COUNT; ++r) {
        if (!(g_pol.rule_on & (1u << r))) continue;
        size_t n = strlen(pv_material_name[r]);
        if (strncmp(up, pv_material_name[r], n) != 0) continue;
        char next = up[n];
        if (next == '\0' || !isalpha((unsigned char)next)) return r;
    }
    return -1;
}

// The whole decision. `stock_open` is what pv_motor_update would have done on
// its own; returning it unchanged is how every "no opinion" path falls back to
// stock. `hold` is the vent's current position, used for the hysteresis band.
bool pv_policy_decide(bool stock_open, bool hold)
{
    if (!g_pol.enable) return stock_open;

    bool printing = (g_live.device_state == PV_ST_PRINTING ||
                     g_live.device_state == PV_ST_PAUSED);

    if (printing) {
        int r = pv_policy_match(g_live.material);
        if (r < 0) return stock_open;              // unknown filament
        return !pv_material_seal[r];               // seal = closed, vent = open
    }

    if (!g_pol.heat_hold) return stock_open;

    // Idle, finished or errored. Stock would close immediately; hold the vent
    // open while the bed is still dumping heat into the chamber.
    //
    // bed_temp is an int and starts at 0 before the printer has ever reported,
    // which reads as cold and would be indistinguishable from a cold bed. That
    // is the same answer stock gives (closed), so no guard is needed.
    if (g_live.bed_temp > g_pol.bed_open_c)  return true;
    if (g_live.bed_temp < g_pol.bed_close_c) return stock_open;
    return hold;                                    // inside the band
}
