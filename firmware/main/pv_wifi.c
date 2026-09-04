// Wi-Fi: STA with flash-stored credentials (they survive OTA from stock,
// since esp_wifi keeps them in its own NVS namespace), softAP per config,
// async scan feeding the factory wifi{scan,list} pushes, hostname + mDNS.
#include "pv.h"

#include <string.h>
#include <stdlib.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "esp_partition.h"
#include "cJSON.h"

static const char *TAG = "pv_wifi";

static esp_netif_t *s_sta_netif, *s_ap_netif;
static bool s_ever_connected;
// Stock's factory self test looks for this exact SSID among the 20 scan
// records, comparing the whole string.
#define TEST_AP_SSID "test1"
static bool s_saw_test_ap;
// Kept separate from g_live.wifi_scan on purpose: that field is part of the
// WebSocket schema and scan_done() returns it to 0 as soon as the list has
// been pushed, so it cannot carry the "done" verdict the self test needs.
static int s_test_scan;           // 0 idle, 1 scanning, 2 complete

bool pv_wifi_saw_test_ap(void)    { return s_saw_test_ap; }
int  pv_wifi_test_scan_state(void) { return s_test_scan; }

static const char *hostname_effective(void)
{
    // Factory default hostname is "PandaVent" (PandaVent.local), per the
    // stock image string table and the BIQU manual.
    return g_cfg.hostname[0] ? g_cfg.hostname : "PandaVent";
}

void pv_hostname_apply(void)
{
    const char *hn = hostname_effective();
    if (s_sta_netif) esp_netif_set_hostname(s_sta_netif, hn);
    static bool mdns_up;
    if (!mdns_up) {
        if (mdns_init() == ESP_OK) mdns_up = true;
    }
    if (mdns_up) {
        mdns_hostname_set(hn);
        mdns_instance_name_set(hn);
        static bool svc;
        if (!svc) { mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0); svc = true; }
    }
    ESP_LOGI(TAG, "hostname: %s", hn);
}

static void ap_ip_apply(void)
{
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton(g_cfg.ap.ip);
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip);
    esp_netif_dhcps_start(s_ap_netif);
}

void pv_ap_apply(void)
{
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%.31s", g_cfg.ap.ssid);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%.63s", g_cfg.ap.password);
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(g_cfg.ap.password) >= 8 ? WIFI_AUTH_WPA_WPA2_PSK
                                                    : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(g_cfg.ap.on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (g_cfg.ap.on) {
        esp_wifi_set_config(WIFI_IF_AP, &ap);
        ap_ip_apply();
    }
}

void pv_wifi_join(const char *ssid, const char *password)
{
    wifi_config_t sta = {0};
    snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", ssid);
    snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", password);
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &sta);   // persisted by esp_wifi
    snprintf(g_live.sta_ssid, sizeof(g_live.sta_ssid), "%s", ssid);
    snprintf(g_live.sta_password, sizeof(g_live.sta_password), "%s", password);
    g_live.sta_state = 2;
    s_ever_connected = false;
    esp_wifi_disconnect();
    esp_wifi_connect();
    pv_ws_push_state();
}

void pv_wifi_scan_start(void)
{
    g_live.wifi_scan = 1;
    s_test_scan = 1;
    s_saw_test_ap = false;
    pv_ws_push_state();
    // Scanning in APSTA takes the radio off the AP's channel. With ESP-IDF's
    // default dwell (120 ms active max, 13 channels) the hotspot is away for
    // over a second and a browser sitting on the setup page drops its
    // WebSocket, which the factory UI reports as "communication interrupted".
    // Bound the per-channel dwell and give the AP channel a longer slice
    // between hops so the client rides through the scan.
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = { .active = { .min = 20, .max = 60 } },
        .home_chan_dwell_time = 60,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, false /* async */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start: %d", err);
        g_live.wifi_scan = 0;
        s_test_scan = 0;
        pv_ws_push_state();
    }
}

// Runs on the esp_event task (sys_evt), whose stack is small. A
// wifi_ap_record_t is over 100 bytes, so twenty of them on the stack is more
// than 2 KB and overflows it: the device panicked and rebooted on every scan,
// which the factory UI reported as "communication interrupted" and which made
// the hotspot setup page unusable after a factory reset. Heap-allocate.
static void scan_done(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!recs) {
        ESP_LOGE(TAG, "scan: out of memory for %u records", n);
        s_test_scan = 2;
        g_live.wifi_scan = 0;
        pv_ws_push_state();
        return;
    }
    uint16_t got = n;
    esp_wifi_scan_get_ap_records(&got, recs);

    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddNumberToObject(wifi, "scan", 2);
    cJSON *list = cJSON_AddArrayToObject(wifi, "list");
    for (int i = 0; i < got; ++i) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", recs[i].rssi);
        cJSON_AddItemToArray(list, e);
    }
    for (int i = 0; i < got; ++i) {
        if (strcmp((const char *)recs[i].ssid, TEST_AP_SSID) == 0) {
            s_saw_test_ap = true;
            break;
        }
    }
    s_test_scan = 2;
    g_live.wifi_scan = 0;
    free(recs);
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    pv_ws_broadcast(out);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_config_t cur;
        if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0]) {
            snprintf(g_live.sta_ssid, sizeof(g_live.sta_ssid), "%s", (char *)cur.sta.ssid);
            snprintf(g_live.sta_password, sizeof(g_live.sta_password), "%s", (char *)cur.sta.password);
            g_live.sta_state = 2;
            esp_wifi_connect();
        } else {
            g_live.sta_state = 1;   // no ssid: UI routes to first-use flow
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (d->reason == WIFI_REASON_AUTH_FAIL ||
            d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
            g_live.sta_state = 5;    // password error
        } else if (g_live.sta_state != 5) {
            g_live.sta_state = s_ever_connected ? 4 : 2;
        }
        g_live.sta_ip[0] = '\0';
        if (g_live.sta_state != 5) esp_wifi_connect();
        pv_ws_push_state();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_done();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(g_live.sta_ip, sizeof(g_live.sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        g_live.sta_state = 3;
        s_ever_connected = true;
        ESP_LOGI(TAG, "sta ip %s", g_live.sta_ip);
        pv_hostname_apply();
        pv_ws_push_state();
        pv_bambu_rebind();
    }
}

/* Salvage stock's Wi-Fi from a raw NVS partition, before anything erases it.
 *
 * A vent arriving from stock has a FULL NVS, so nvs_flash_init() answers
 * ESP_ERR_NVS_NO_FREE_PAGES and the only way forward is nvs_flash_erase().
 * That erase takes stock's whole app_nvs namespace with it -- including the
 * Wi-Fi -- and it happens long before migrate_stock_wifi() gets to look, so
 * the migration always found an empty store and the vent came up on its own
 * hotspot instead of the network it had been sitting on a minute earlier.
 *
 * NVS cannot be opened at that point, so this reads the partition directly
 * and walks the on-flash format: 4 KB pages, a 32-byte header, a 2-bit-per
 * -entry state bitmap, then 126 thirty-two-byte entries. A namespace entry
 * (type 0x01, ns 0) maps a name to an index; a blob-data entry (type 0x42)
 * carries its length in the first two data bytes and its payload in the
 * entries that follow. Only the two fields that matter are taken. */
static char s_salv_ssid[33];
static char s_salv_pass[64];

bool pv_wifi_salvage_stock(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (ssid) ssid[0] = '\0';
    if (pass) pass[0] = '\0';
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (!part) return false;

    uint8_t *page = malloc(4096);
    if (!page) return false;

    bool got = false;
    uint8_t want_ns = 0xFF;

    /* Two passes: the namespace entry can live on a later page than the blob. */
    for (int pass_no = 0; pass_no < 2 && !got; ++pass_no) {
        for (size_t off = 0; off + 4096 <= part->size; off += 4096) {
            if (esp_partition_read(part, off, page, 4096) != ESP_OK) continue;
            uint32_t state;
            memcpy(&state, page, 4);
            if (state == 0xFFFFFFFFU) continue;            /* never written */
            const uint8_t *bm = page + 32;
            for (int e = 0; e < 126; ) {
                int st = (bm[e / 4] >> ((e % 4) * 2)) & 3;
                const uint8_t *ent = page + 64 + e * 32;
                if (st != 2) { ++e; continue; }             /* 2 = written */
                uint8_t ns = ent[0], type = ent[1], span = ent[2];
                const char *key = (const char *)(ent + 8);
                if (pass_no == 0 && type == 0x01 && ns == 0 &&
                    !strncmp(key, "app_nvs", 16)) {
                    want_ns = ent[24];
                } else if (pass_no == 1 && type == 0x42 && ns == want_ns &&
                           !strncmp(key, "wifi_info", 16)) {
                    uint16_t sz;
                    memcpy(&sz, ent + 24, 2);
                    const uint8_t *blob = ent + 32;         /* payload follows */
                    size_t avail = (size_t)(page + 4096 - blob);
                    if (sz > avail) sz = avail;
                    /* ssid[33] at 0, password[64] at 33 -- the offsets the AP
                       fields at 97 and 130 confirm on a real stock image. */
                    if (sz >= 97 && blob[0]) {
                        snprintf(s_salv_ssid, sizeof s_salv_ssid, "%.32s", (const char *)blob);
                        snprintf(s_salv_pass, sizeof s_salv_pass, "%.63s", (const char *)blob + 33);
                        if (ssid) snprintf(ssid, ssid_len, "%s", s_salv_ssid);
                        if (pass) snprintf(pass, pass_len, "%s", s_salv_pass);
                        got = true;
                    }
                }
                e += span > 0 ? span : 1;
                if (got) break;
            }
            if (got) break;
        }
        if (pass_no == 0 && want_ns == 0xFF) break;         /* no app_nvs at all */
    }
    free(page);
    if (got) ESP_LOGW(TAG, "salvaged stock Wi-Fi from raw NVS (ssid '%s')", s_salv_ssid);
    return got;
}

// Stock keeps its Wi-Fi in NVS namespace "app_nvs", blob "wifi_info":
// ssid[33]@0, password[64]@33, ap_ssid[33]@97 (layout recovered from stock
// and proven on this hardware). When this firmware is installed OVER stock,
// esp_wifi's own store is empty, so lift the factory credentials once.
static void migrate_stock_wifi(void)
{
    wifi_config_t cur = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0])
        return;   // already provisioned in our own store

    /* app_nvs is gone on any vent whose NVS had to be erased to boot -- which
       is every vent arriving from stock. The pair salvaged out of the raw
       partition before that erase is the only copy left. */
    if (s_salv_ssid[0]) {
        wifi_config_t sv = {0};
        snprintf((char *)sv.sta.ssid, sizeof(sv.sta.ssid), "%.31s", s_salv_ssid);
        snprintf((char *)sv.sta.password, sizeof(sv.sta.password), "%.63s", s_salv_pass);
        sv.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sv.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        esp_wifi_set_config(WIFI_IF_STA, &sv);
        ESP_LOGW(TAG, "restored the salvaged stock Wi-Fi (ssid '%s')", s_salv_ssid);
        return;
    }

    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t blob[228] = {0};
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, "wifi_info", blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len < 97 || !blob[0]) return;

    char ssid[33] = {0}, pass[64] = {0};
    memcpy(ssid, blob, 32);
    memcpy(pass, blob + 33, 63);
    wifi_config_t sta = {0};
    snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%.31s", ssid);
    snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%.63s", pass);
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &sta);   // persists (storage = flash)
    ESP_LOGW(TAG, "migrated stock Wi-Fi (ssid '%s')", ssid);

    if (len >= 130 && blob[97] && g_cfg.ap.ssid[0] == '\0') {
        char ap_ssid[33] = {0};
        memcpy(ap_ssid, blob + 97, 32);
        snprintf(g_cfg.ap.ssid, sizeof(g_cfg.ap.ssid), "%s", ap_ssid);
        pv_cfg_save();
        ESP_LOGW(TAG, "migrated stock AP ssid '%s'", g_cfg.ap.ssid);
    }
}

// Stock builds the softAP ssid at run time from the STA (base) MAC:
// "Panda_Vent_" + six uppercase hex bytes. Proven on a live unit: STA MAC
// AA:BB:CC:DD:EE:10 advertises "Panda_Vent_AABBCCDDEE10" (the SOFTAP MAC
// would be ...11, so stock reads ESP_MAC_WIFI_STA, not ESP_MAC_WIFI_SOFTAP).
static void ap_ssid_default(void)
{
    if (g_cfg.ap.ssid[0]) return;
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return;
    snprintf(g_cfg.ap.ssid, sizeof(g_cfg.ap.ssid),
             "Panda_Vent_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    pv_cfg_save();
    ESP_LOGI(TAG, "default ap ssid %s", g_cfg.ap.ssid);
}

void pv_wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());          // unsurvivable
    esp_event_loop_create_default();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t werr = esp_wifi_init(&init);
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s -- device will be unreachable",
                 esp_err_to_name(werr));
        return;   // never abort: the health task will roll this image back
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);

    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    migrate_stock_wifi();       // may supply the stock AP ssid
    ap_ssid_default();          // otherwise derive it exactly like stock
    esp_wifi_set_mode(g_cfg.ap.on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (g_cfg.ap.on) pv_ap_apply();
    werr = esp_wifi_start();
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(werr));
        return;
    }
    pv_hostname_apply();
}
