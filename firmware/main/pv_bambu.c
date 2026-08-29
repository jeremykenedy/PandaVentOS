// Bambu Lab LAN link: MQTT over TLS (self-signed per-device cert, so no
// verification — LAN only), device/<sn>/report subscription, one pushall on
// connect. SSDP discovery for the factory printer{scan,list} flow.
#include "pv.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "cJSON.h"

static const char *TAG = "pv_bambu";

static esp_mqtt_client_handle_t s_client;
static char s_report_topic[80], s_request_topic[80];
static bool s_started;
static volatile TickType_t s_last_report;   // 0 until the bound SN answers
static volatile TickType_t s_connected_at;
static void relocate_start(void);           // defined with the scan code

static const char PUSHALL[] =
    "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\","
    "\"version\":1,\"push_target\":1}}";

// ---------- report parsing ----------

static void handle_report(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    cJSON *print = cJSON_GetObjectItemCaseSensitive(root, "print");
    if (print) {
        cJSON *gs = cJSON_GetObjectItemCaseSensitive(print, "gcode_state");
        if (cJSON_IsString(gs)) {
            const char *st = gs->valuestring;
            int ds = g_live.device_state;
            if (!strcmp(st, "IDLE") || !strcmp(st, "INIT")) ds = PV_ST_IDLE;
            else if (!strcmp(st, "PREPARE") || !strcmp(st, "SLICING")) ds = PV_ST_PREPARE;
            else if (!strcmp(st, "RUNNING")) ds = PV_ST_PRINTING;
            else if (!strcmp(st, "PAUSE")) ds = PV_ST_PAUSED;
            else if (!strcmp(st, "FINISH")) ds = PV_ST_COMPLETE;
            else if (!strcmp(st, "FAILED")) ds = PV_ST_ERROR;
            if (ds != g_live.device_state) {
                g_live.device_state = ds;
                ESP_LOGI(TAG, "printer state -> %d (%s)", ds, st);
                pv_rgb_notify();
                pv_motor_update();
            }
        }
        cJSON *bed = cJSON_GetObjectItemCaseSensitive(print, "bed_temper");
        if (cJSON_IsNumber(bed)) g_live.bed_temp = (int)bed->valuedouble;
        cJSON *noz = cJSON_GetObjectItemCaseSensitive(print, "nozzle_temper");
        if (cJSON_IsNumber(noz)) g_live.nozzle_temp = (int)noz->valuedouble;

        // The printer's own chamber light, for "Follow Printer Light".
        // Bambu reports it as print.lights_report:
        //   [ { "node": "chamber_light", "mode": "on" | "off" }, ... ]
        cJSON *lights = cJSON_GetObjectItemCaseSensitive(print, "lights_report");
        if (cJSON_IsArray(lights)) {
            cJSON *e;
            cJSON_ArrayForEach(e, lights) {
                cJSON *node = cJSON_GetObjectItemCaseSensitive(e, "node");
                cJSON *mode = cJSON_GetObjectItemCaseSensitive(e, "mode");
                if (cJSON_IsString(node) && cJSON_IsString(mode) &&
                    !strcmp(node->valuestring, "chamber_light")) {
                    bool on = !strcmp(mode->valuestring, "on");
                    if (on != g_live.printer_light) {
                        g_live.printer_light = on;
                        ESP_LOGI(TAG, "printer chamber light -> %s",
                                 on ? "on" : "off");
                        pv_rgb_notify();
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        // The broker accepted "bblp" + the access code, so the address and
        // the code are both right. Whether the SERIAL is right is not known
        // until something arrives on device/<sn>/report; sn_watch_task
        // decides that.
        g_live.printer_state = 3;
        s_last_report = 0;
        s_connected_at = xTaskGetTickCount();
        esp_mqtt_client_subscribe(s_client, s_report_topic, 0);
        esp_mqtt_client_publish(s_client, s_request_topic, PUSHALL, 0, 0, 0);
        pv_ws_push_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (g_live.printer_state == 3) g_live.printer_state = 2;
        pv_ws_push_state();
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle &&
            ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            g_live.printer_state = 6;   // access code rejected
        } else if (ev->error_handle &&
                   ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            g_live.printer_state = 4;   // no route / ip error
            // Factory behaviour: do not just show the error. Re-run discovery
            // for the same serial and follow the printer to its new address.
            relocate_start();
        }
        pv_ws_push_state();
        break;
    case MQTT_EVENT_DATA:
        // Reports up to ~20 KB arrive fragmented; only parse single-part
        // messages and the first fragment carrying the JSON start (state and
        // temperature keys sit early in Bambu reports).
        if (ev->current_data_offset == 0 && ev->topic_len &&
            strncmp(ev->topic, s_report_topic, ev->topic_len) == 0) {
            if (ev->total_data_len == ev->data_len) {
                s_last_report = xTaskGetTickCount();
                handle_report(ev->data, ev->data_len);
            } else {
                // Fragment: parse best-effort by truncating to last full JSON
                // not possible; skip. pushall arrives once, deltas are small.
            }
        }
        break;
    default:
        break;
    }
}

void pv_bambu_disconnect(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    g_live.printer_state = 0;
    g_live.device_state = PV_ST_IDLE;
    // Deliberately NOT reset. Stock's report array is written only by the
    // parser at 0x400d9244 and nothing in the image ever clears it: not on
    // disconnect, not on unbind, and a report that omits a key leaves the
    // slot alone (the loop just continues, 0x400d92bd). So stock keeps the
    // last temperature it ever saw, and stays hot on a dropped printer.
    // Resetting here made the vent go green where stock stays red.
    g_live.printer_light = false;
    pv_rgb_notify();
}

void pv_bambu_rebind(void)
{
    if (!s_started) return;
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    if (!g_cfg.printer.sn[0] || !g_cfg.printer.ip[0] || !g_cfg.printer.access_code[0]) {
        g_live.printer_state = g_cfg.printer.sn[0] ? 1 : 0;
        return;
    }
    snprintf(s_report_topic, sizeof(s_report_topic), "device/%s/report", g_cfg.printer.sn);
    snprintf(s_request_topic, sizeof(s_request_topic), "device/%s/request", g_cfg.printer.sn);
    char uri[64];
    snprintf(uri, sizeof(uri), "mqtts://%s:8883", g_cfg.printer.ip);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = "bblp",
        .credentials.authentication.password = g_cfg.printer.access_code,
        // A P-series pushall report measures ~19 KB; the buffer must hold it
        // whole or the connect-time state snapshot is lost to fragmentation.
        .buffer.size = 24576,
        .buffer.out_size = 1024,
        .network.timeout_ms = 8000,
        .session.keepalive = 30,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { g_live.printer_state = 7; return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    g_live.printer_state = 2;
    esp_mqtt_client_start(s_client);
    pv_ws_push_state();
}

// A wrong serial still connects: the broker authenticates the access code,
// not the topic. The only symptom is silence on device/<sn>/report. The
// factory app distinguishes SN errors (state 5) from access code errors
// (state 6) and shows different text, so the firmware has to tell them apart.
#define PV_SN_ANSWER_TIMEOUT_MS 20000

static void sn_watch_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (g_live.printer_state != 3) continue;
        if (s_last_report) continue;                 // the serial answered
        TickType_t since = xTaskGetTickCount() - s_connected_at;
        if (since > pdMS_TO_TICKS(PV_SN_ANSWER_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "no report on device/%s/report in %d s: wrong serial",
                     g_cfg.printer.sn, PV_SN_ANSWER_TIMEOUT_MS / 1000);
            g_live.printer_state = 5;                // sn error
            pv_ws_push_state();
        }
    }
}

void pv_bambu_start(void)
{
    s_started = true;
    xTaskCreate(sn_watch_task, "pv_snwatch", 3072, NULL, 3, NULL);
    // The actual connect happens when STA gets an IP (pv_wifi calls rebind).
}

// ---------- SSDP discovery (factory printer scan) ----------

#define SSDP_GROUP "239.255.255.250"
#define SSDP_PORT  2021

typedef struct { char name[32]; char sn[24]; char ip[16]; } found_t;

static int ssdp_collect(found_t *found, int max)
{
    int nfound = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in addr = { .sin_family = AF_INET,
            .sin_port = htons(SSDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
        bind(sock, (struct sockaddr *)&addr, sizeof(addr));
        struct ip_mreq mreq = {0};
        mreq.imr_multiaddr.s_addr = inet_addr(SSDP_GROUP);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

        const char *msearch =
            "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:2021\r\n"
            "MAN: \"ssdp:discover\"\r\nMX: 3\r\n"
            "ST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";
        struct sockaddr_in dst = { .sin_family = AF_INET,
            .sin_port = htons(SSDP_PORT), .sin_addr.s_addr = inet_addr(SSDP_GROUP) };
        sendto(sock, msearch, strlen(msearch), 0, (struct sockaddr *)&dst, sizeof(dst));

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(4000);
        char buf[1024];
        while (xTaskGetTickCount() < end && nfound < max) {
            struct sockaddr_in src; socklen_t sl = sizeof(src);
            int r = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &sl);
            if (r <= 0) continue;
            buf[r] = '\0';
            found_t f = {0};
            inet_ntoa_r(src.sin_addr, f.ip, sizeof(f.ip));
            for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
                if (!strncasecmp(line, "USN:", 4)) {
                    const char *v = line + 4; while (*v == ' ') ++v;
                    snprintf(f.sn, sizeof(f.sn), "%s", v);
                } else if (!strncasecmp(line, "DevName.bambu.com:", 18)) {
                    const char *v = line + 18; while (*v == ' ') ++v;
                    snprintf(f.name, sizeof(f.name), "%s", v);
                } else if (!strncasecmp(line, "Location:", 9)) {
                    const char *v = line + 9; while (*v == ' ') ++v;
                    if (*v) snprintf(f.ip, sizeof(f.ip), "%s", v);
                }
            }
            if (!f.sn[0]) continue;
            bool dup = false;
            for (int i = 0; i < nfound; ++i)
                if (!strcmp(found[i].sn, f.sn)) dup = true;
            if (!dup) {
                if (!f.name[0]) snprintf(f.name, sizeof(f.name), "%s", f.sn);
                found[nfound++] = f;
            }
        }
        close(sock);
    }
    return nfound;
}

// The plain user-initiated scan: collect and publish the list.
static void scan_task(void *arg)
{
    found_t found[6];
    int nfound = ssdp_collect(found, 6);

    cJSON *root = cJSON_CreateObject();
    cJSON *pr = cJSON_AddObjectToObject(root, "printer");
    cJSON_AddNumberToObject(pr, "scan", 2);
    cJSON *list = cJSON_AddArrayToObject(pr, "list");
    for (int i = 0; i < nfound; ++i) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", found[i].name);
        cJSON_AddStringToObject(e, "sn", found[i].sn);
        cJSON_AddStringToObject(e, "ip", found[i].ip);
        cJSON_AddStringToObject(e, "access_code", "");
        cJSON_AddItemToArray(list, e);
    }
    g_live.printer_scan = 0;
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    pv_ws_broadcast(out);
    ESP_LOGI(TAG, "ssdp scan done, %d printer(s)", nfound);
    vTaskDelete(NULL);
}


// ---------------------------------------------------------------------------
// IP-change recovery. When a bound printer stops answering at its stored
// address the factory does not just sit on an error: it re-runs discovery
// looking for the SAME serial, and reports the outcome through printer.scan.
// The factory app has a dialog for each result, which is how the state
// numbering is known:
//
//   3  ip_change_scanning   "..." (spinner)
//   4  sn not matched       "No printer with the same SN code was scanned,
//                            rescanning."
//   5  ip not changed       "The IP has not changed, reconnecting. (Or please
//                            go to the AP menu to confirm if the Hotspot IP
//                            conflicts with the IP range of your own router)"
//   6  new ip applied       "The IP has changed, reconnecting with the new IP."
// ---------------------------------------------------------------------------

static bool s_relocating;

static void scan_publish(int scan_state)
{
    g_live.printer_scan = scan_state;
    pv_ws_push_state();
}

static void relocate_task(void *arg)
{
    found_t found[6];
    char want[24];
    snprintf(want, sizeof(want), "%s", g_cfg.printer.sn);

    for (int attempt = 0; attempt < 3 && want[0]; ++attempt) {
        scan_publish(3);                       // ip_change_scanning
        int n = ssdp_collect(found, 6);

        const found_t *hit = NULL;
        for (int i = 0; i < n; ++i)
            if (!strcmp(found[i].sn, want)) { hit = &found[i]; break; }

        if (!hit) {
            ESP_LOGW(TAG, "relocate: sn %s not seen (attempt %d)", want, attempt + 1);
            scan_publish(4);                   // sn not matched, will rescan
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        if (!strcmp(hit->ip, g_cfg.printer.ip)) {
            ESP_LOGW(TAG, "relocate: sn %s still at %s", want, hit->ip);
            scan_publish(5);                   // ip not changed
            vTaskDelay(pdMS_TO_TICKS(500));
            g_live.printer_scan = 0;
            pv_bambu_rebind();                 // "reconnecting"
            break;
        }

        ESP_LOGW(TAG, "relocate: sn %s moved %s -> %s", want,
                 g_cfg.printer.ip, hit->ip);
        snprintf(g_cfg.printer.ip, sizeof(g_cfg.printer.ip), "%s", hit->ip);
        pv_cfg_save();
        scan_publish(6);                       // new ip applied
        vTaskDelay(pdMS_TO_TICKS(500));
        g_live.printer_scan = 0;
        pv_bambu_rebind();                     // "reconnecting with the new IP"
        break;
    }

    g_live.printer_scan = 0;
    s_relocating = false;
    pv_ws_push_state();
    vTaskDelete(NULL);
}

// Called when a bound printer looks unreachable at its stored address.
static void relocate_start(void)
{
    if (s_relocating || !g_cfg.printer.sn[0]) return;
    s_relocating = true;
    xTaskCreate(relocate_task, "pv_reloc", 4096, NULL, 4, NULL);
}

void pv_bambu_scan_start(void)
{
    if (g_live.printer_scan == 1) return;
    g_live.printer_scan = 1;
    pv_ws_push_state();
    xTaskCreate(scan_task, "pv_ssdp", 4096, NULL, 4, NULL);
}
