// HTTP server: "/" serves the byte-exact factory web app (embedded gzip),
// "/ws" is the factory WebSocket (full state push on connect, dispatch on
// receive, broadcast on change), "/ota" accepts firmware uploads with the
// factory OTA-Type header. The updater accepts any valid app image for this
// partition layout, which is exactly what makes restoring the factory
// firmware a plain upload.
#include "pv.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pv_http";

extern const uint8_t index_gz_start[] asm("_binary_ui_html_gz_start");
extern const uint8_t index_gz_end[]   asm("_binary_ui_html_gz_end");

static httpd_handle_t s_server;
static volatile bool s_up;

// One tracked websocket per socket the server will accept. It was 4 against a
// pool of 8, so the server would happily accept clients it had no room to talk
// to. Eight ints is not a meaningful amount of RAM.
#define MAX_WS 8
static int s_ws_fd[MAX_WS];
static int s_ws_count;

// ---------- pages ----------

// DEPARTURE FROM STOCK, 2026-08-31. Send the page in chunks, and let a slow
// client take its time.
//
// This was one httpd_resp_send of the whole gzip. That call loops internally
// over httpd_send, and if ANY single socket write blocks longer than
// send_wait_timeout the whole response is abandoned part-sent, with no error
// the client can see: it just gets a short body. Measured on the device with
// the page at 273 KB: one fetch returned 217414 bytes, the next 5734, the next
// 11494, all of them "200 OK". A browser handed a page cut mid-script parses
// what arrived and silently loses every function after the cut, which looks
// exactly like the device having forgotten its settings.
//
// Chunking does not make the link faster; it makes the timeout mean the right
// thing. Each chunk restarts it, so the limit becomes "no progress for N
// seconds" instead of "the entire 273 KB inside N seconds", and a stall is
// reported rather than silently truncating. 8 KB is comfortably more than one
// TCP window and small enough that a chunk is one or two writes.
#define ROOT_CHUNK 8192

static esp_err_t root_get(httpd_req_t *req)
{
    const char *p = (const char *)index_gz_start;
    size_t left = (size_t)(index_gz_end - index_gz_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    while (left) {
        size_t n = left > ROOT_CHUNK ? ROOT_CHUNK : left;
        esp_err_t err = httpd_resp_send_chunk(req, p, n);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "page send stalled with %u of %u bytes left",
                     (unsigned)left, (unsigned)(index_gz_end - index_gz_start));
            return err;          // httpd closes the socket; no half-page is claimed complete
        }
        p += n;
        left -= n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);   // terminates the chunked body
}

// ---------- full-flash backup over the network ----------
//
// Streams the ENTIRE flash, byte for byte: bootloader, partition table,
// otadata, both app slots and NVS. The same image esptool produces over USB,
// and interchangeable with it, so vent-restore-golden.sh can write it back.
//
// This exists because a backup was the one job that still needed the cable,
// and needing the cable is how a device ends up with no backup at all. The
// running app can read its own flash: esp_flash_read goes to the SPI part
// directly rather than through the instruction cache, so the region holding
// this very code reads out correctly.
//
// It is a plain unauthenticated GET on the LAN, like everything else this
// server serves, and the image contains the Wi-Fi password, the printer serial
// and its access code in the NVS region. That is the same exposure as the
// password field on the settings page, but the file is the whole of it at
// once, so treat what comes out of here the way you treat the goldens folder.
static esp_err_t backup_get(httpd_req_t *req)
{
    uint32_t size = 0;
    if (esp_flash_get_size(NULL, &size) != ESP_OK || size == 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash size");
        return ESP_FAIL;
    }
    const size_t CHUNK = 8192;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"panda-vent-flash.bin\"");
    // Length up front so a client can tell a short read from a complete one.
    char len[16];
    snprintf(len, sizeof(len), "%u", (unsigned)size);
    httpd_resp_set_hdr(req, "X-Flash-Size", len);

    ESP_LOGW(TAG, "backup: streaming %u bytes of flash", (unsigned)size);
    esp_err_t err = ESP_OK;
    for (uint32_t off = 0; off < size; off += CHUNK) {
        size_t n = (size - off) < CHUNK ? (size - off) : CHUNK;
        if (esp_flash_read(NULL, buf, off, n) != ESP_OK) {
            ESP_LOGE(TAG, "backup: flash read failed at 0x%06x", (unsigned)off);
            err = ESP_FAIL;
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
            ESP_LOGW(TAG, "backup: client went away at 0x%06x", (unsigned)off);
            err = ESP_FAIL;
            break;
        }
        // Yield so the Wi-Fi and motor tasks are not starved for the whole
        // transfer. Every 512 KB is often enough to matter and rare enough
        // not to slow the read down.
        if ((off & 0x7FFFF) == 0) vTaskDelay(1);
    }
    free(buf);
    // Terminating chunk. On the error path this ends the body early, and the
    // client sees fewer bytes than X-Flash-Size promised.
    httpd_resp_send_chunk(req, NULL, 0);
    if (err == ESP_OK) ESP_LOGW(TAG, "backup: done");
    return err;
}

// ---------- websocket ----------

static void ws_track(int fd)
{
    for (int i = 0; i < s_ws_count; ++i)
        if (s_ws_fd[i] == fd) return;
    if (s_ws_count == MAX_WS) {
        // Table full: drop the OLDEST client, never the one that just arrived.
        //
        // Silently ignoring the newcomer, which is what this did before, gives
        // the worst symptom in the whole UI. The handshake succeeds, the
        // browser reports the socket OPEN, and then nothing ever arrives: no
        // state on connect, no updates. The page sits there empty and there is
        // nothing on screen to explain it, because the device is still busy
        // talking to tabs somebody left open on another machine.
        //
        // Measured on the running device with probe/ws-client-limit.py: six
        // clients connected, three were served, three got an open socket and
        // total silence.
        ESP_LOGW(TAG, "ws table full, evicting fd=%d to make room for fd=%d",
                 s_ws_fd[0], fd);
        for (int i = 1; i < MAX_WS; ++i) s_ws_fd[i - 1] = s_ws_fd[i];
        --s_ws_count;
    }
    s_ws_fd[s_ws_count++] = fd;
}

static void ws_untrack(int fd)
{
    for (int i = 0; i < s_ws_count; ++i) {
        if (s_ws_fd[i] == fd) {
            s_ws_fd[i] = s_ws_fd[--s_ws_count];
            return;
        }
    }
}

// DEPARTURE FROM STOCK. Untrack on EVERY socket close, not only when a send
// happens to fail.
//
// Without this the table only ever shed a client when a broadcast to it
// errored, so a browser tab closed cleanly, or a script that connects and
// disconnects in a loop, left its fd behind forever. Eight of those and the
// table is full of the dead: every new client evicts a ghost instead of being
// served, the handshake succeeds, and the page sits there with an open socket
// and no state document. Reproduced by running the probe harnesses back to
// back; the server kept answering plain HTTP the whole time, which is what
// makes the symptom so confusing.
//
// httpd calls close_fn for every session teardown, whatever the reason. An
// override owns the close() itself, which the default hook would have done.
static void ws_close_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ws_untrack(sockfd);
    close(sockfd);
}

typedef struct { char *json; } bcast_t;

static void bcast_work(void *arg)
{
    bcast_t *b = arg;
    httpd_ws_frame_t f = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)b->json,
        .len = strlen(b->json),
    };
    for (int i = 0; i < s_ws_count; ) {
        if (httpd_ws_send_frame_async(s_server, s_ws_fd[i], &f) != ESP_OK) {
            ws_untrack(s_ws_fd[i]);
        } else {
            ++i;
        }
    }
    free(b->json);
    free(b);
}

void pv_ws_broadcast(char *json_take_ownership)
{
    if (!json_take_ownership) return;
    if (!s_server || s_ws_count == 0) { free(json_take_ownership); return; }
    bcast_t *b = malloc(sizeof(*b));
    if (!b) { free(json_take_ownership); return; }
    b->json = json_take_ownership;
    if (httpd_queue_work(s_server, bcast_work, b) != ESP_OK) {
        free(b->json);
        free(b);
    }
}

// NOT STOCK. The state goes out in parts, built one at a time INSIDE the
// server task, so only one part exists at once and none of it is copied.
//
// One queued work item sends all of them. Queuing eight would multiply the
// server's work queue by eight on every slider move, and the queue is not
// deep. Building inside the send is free: this runs on the HTTP task, which is
// where the frames go out anyway, and that task is single threaded, so the
// static buffer cannot be overwritten between building a part and sending it.
static void state_send(int only_fd)
{
    for (int part = 0; part < PV_STATE_PARTS; ++part) {
        const char *json = pv_json_state_part(part);
        if (!json) continue;
        httpd_ws_frame_t f = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = strlen(json),
        };
        if (only_fd >= 0) {
            if (httpd_ws_send_frame_async(s_server, only_fd, &f) != ESP_OK) {
                ESP_LOGW(TAG, "state part %d failed for fd=%d", part, only_fd);
                ws_untrack(only_fd);
                return;
            }
        } else {
            // ws_untrack compacts the table, so the index only advances on a
            // socket that survived. Walking it with a plain for-loop skipped
            // the client that was moved into the slot just vacated.
            for (int i = 0; i < s_ws_count; ) {
                if (httpd_ws_send_frame_async(s_server, s_ws_fd[i], &f) != ESP_OK)
                    ws_untrack(s_ws_fd[i]);
                else
                    ++i;
            }
        }
    }
}

static void state_work(void *arg)
{
    state_send((int)(intptr_t)arg);
}

void pv_ws_push_state(void)
{
    if (!s_server || s_ws_count == 0) return;
    httpd_queue_work(s_server, state_work, (void *)(intptr_t)-1);
}

// NOT STOCK. The retry machinery that used to sit here is gone with the single
// document it retried. A part that fails to send now drops that client from
// the table, and the page asks for the state itself when it does not arrive,
// which is the same recovery by a shorter road.

// The state document for ONE client, sent after the handler that asked for it
// has returned. See the comment at the handshake for why it is not sent inline.
void pv_ws_push_state_to(int fd)
{
    if (!s_server) return;
    if (httpd_queue_work(s_server, state_work, (void *)(intptr_t)fd) != ESP_OK)
        ESP_LOGW(TAG, "connect push: work queue full for fd=%d", fd);
}


static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {   // handshake completed
        int fd = httpd_req_to_sockfd(req);
        ws_track(fd);
        ESP_LOGI(TAG, "ws client fd=%d (%d total)", fd, s_ws_count);
        // DEPARTURE FROM STOCK. Push the state document to THIS client only,
        // and do it from queued work rather than from inside this handler.
        //
        // Two separate mistakes were made here and both are worth naming.
        // Broadcasting on connect made the one thing the new client needs
        // compete with a send to every other client, and when that queue was
        // busy the connect push was simply lost: an open socket, an empty
        // page, no error anywhere. Sending inline from this handler instead
        // was worse, not better: the handshake response has not been flushed
        // when the handler runs, so writing a frame here races the 101 and
        // almost always loses (measured: 2 of 25 connects got their state).
        // Queued work runs after the handler returns, on a socket that is
        // fully a websocket by then, and it addresses one fd instead of N.
        pv_ws_push_state_to(fd);
        return ESP_OK;
    }
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    // Shipping cap stays exactly 4096. The test build takes whole captured
    // printer reports through this path, which do not fit in that.
#if PV_POLICY_TEST_HOOK
#define PV_WS_MAX_FRAME 16384
#else
#define PV_WS_MAX_FRAME 4096
#endif
    if (f.len == 0 || f.len > PV_WS_MAX_FRAME) return ESP_OK;
    f.payload = malloc(f.len + 1);
    if (!f.payload) return ESP_ERR_NO_MEM;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err == ESP_OK && f.type == HTTPD_WS_TYPE_TEXT) {
        f.payload[f.len] = '\0';
        pv_apply_message((const char *)f.payload, f.len);
    } else if (f.type == HTTPD_WS_TYPE_CLOSE) {
        ws_untrack(httpd_req_to_sockfd(req));
    }
    free(f.payload);
    return ESP_OK;
}

// ---------- OTA ----------

// Consecutive empty reads tolerated before an upload is called dead. At the
// twenty second socket timeout that is two minutes of silence.
#define OTA_MAX_TIMEOUTS 6

static void ota_reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }

// NOT STOCK. An animation, uploaded into RAM.
//
//   POST /anim   body: [u16 frames][u16 pixels] then frames*pixels*3 RGB bytes
//   POST /anim   with an empty body clears whatever is loaded
//
// Little-endian, because that is what a DataView in the browser writes without
// a byte-swap and what the ESP32 reads without one either.
//
// Raw RGB rather than an image. Decoding a PNG needs a decoder and a frame
// buffer on a device with eighty kilobytes of heap free; the page already has
// a canvas, which decodes every format the browser knows and scales the rows
// at the same time, so the work happens where the memory is. It also means
// the preview on screen is made of the same bytes that get sent.
//
// The whole body is read into one heap block before anything is committed, so
// an upload that dies half way leaves the animation that was playing alone.
static esp_err_t anim_post(httpd_req_t *req)
{
    if (req->content_len == 0) {
        pv_anim_clear();
        httpd_resp_send(req, "", 0);
        pv_ws_broadcast(pv_json_response("anim", 1));
        pv_ws_push_state();
        return ESP_OK;
    }
    size_t max = 4 + (size_t)PV_ANIM_MAX_BYTES;
    if (req->content_len < 4 || (size_t)req->content_len > max) {
        ESP_LOGW(TAG, "anim: %d bytes, allowed 4..%u",
                 (int)req->content_len, (unsigned)max);
        httpd_resp_send(req, "", 0);
        pv_ws_broadcast(pv_json_response("anim", 0));
        return ESP_OK;
    }
    uint8_t *body = malloc(req->content_len);
    if (!body) {
        ESP_LOGW(TAG, "anim: no room for %d bytes", (int)req->content_len);
        httpd_resp_send(req, "", 0);
        pv_ws_broadcast(pv_json_response("anim", 0));
        return ESP_OK;
    }
    size_t got = 0;
    int timeouts = 0;
    bool bad = false;
    while (got < (size_t)req->content_len) {
        int r = httpd_req_recv(req, (char *)body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            // Same reasoning as the OTA path: a timeout means the socket has
            // nothing ready yet, not that the upload failed.
            if (++timeouts > OTA_MAX_TIMEOUTS) { bad = true; break; }
            continue;
        }
        if (r <= 0) { bad = true; break; }
        timeouts = 0;
        got += r;
    }
    bool ok = false;
    if (!bad && got >= 4) {
        int frames = body[0] | (body[1] << 8);
        int pixels = body[2] | (body[3] << 8);
        size_t need = (size_t)frames * pixels * 3;
        if (need == got - 4)
            ok = pv_anim_set(body + 4, frames, pixels);
        else
            ESP_LOGW(TAG, "anim: header says %d x %d = %u bytes, body has %u",
                     frames, pixels, (unsigned)need, (unsigned)(got - 4));
    }
    free(body);
    httpd_resp_send(req, "", 0);
    pv_ws_broadcast(pv_json_response("anim", ok ? 1 : 0));
    pv_ws_push_state();
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    char ota_type[24] = "ota_fw";
    httpd_req_get_hdr_value_str(req, "OTA-Type", ota_type, sizeof(ota_type));

    if (strcmp(ota_type, "ota_fw") != 0) {
        // The vent has no display; gif/image OTA types belong to the Status
        // family. Report the factory "unknown" response and drain nothing.
        httpd_resp_send(req, "", 0);
        pv_ws_broadcast(pv_json_response("ota_unknown", 0));
        return ESP_OK;
    }

    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst || req->content_len < 1024 || req->content_len > dst->size) {
        httpd_resp_send(req, "", 0);
        pv_ws_broadcast(pv_json_response("ota_fw", 0));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "ota_fw: %d bytes -> %s", (int)req->content_len, dst->label);

    // Stock stops its render task before an OTA: the task is notified, the
    // strip is driven all-off, and the task returns, so the strip goes dark
    // and the RMT channels are released for the duration.
    pv_rgb_stop();

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(dst, req->content_len, &h);
    if (err != ESP_OK) {
        // esp_ota_begin leaves the handle untouched when it fails, so there is
        // nothing to abort and h holds whatever was on the stack.
        ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(err));
        pv_rgb_resume();
        httpd_resp_send(req, "", 0);
        pv_ws_broadcast(pv_json_response("ota_fw", 0));
        return ESP_OK;
    }
    char *buf = malloc(4096);
    if (!buf) { esp_ota_abort(h); h = 0; err = ESP_ERR_NO_MEM; }
    size_t remaining = req->content_len;
    // DEPARTURE FROM STOCK, 2026-08-31. A read TIMEOUT is not a failed upload.
    //
    // This treated every non-positive return as fatal, and httpd_req_recv
    // returns HTTPD_SOCK_ERR_TIMEOUT when the socket simply has nothing ready
    // yet. One five-second pause anywhere in a 1.4 MB upload therefore aborted
    // the whole thing. Worse, the abort is invisible from the uploading end:
    // the handler still answers 200 with the stock empty body, so curl reports
    // success and the device quietly keeps running the old image. Three
    // consecutive flashes were lost that way before anyone checked what the
    // device was actually serving.
    //
    // A timeout now just means "wait and read again", and only a real socket
    // error, or a long run of nothing at all, gives up. ESP-IDF's own OTA
    // example does the same.
    int timeouts = 0;
    while (err == ESP_OK && remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > 4096 ? 4096 : remaining);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > OTA_MAX_TIMEOUTS) {
                ESP_LOGE(TAG, "ota: no data for too long, %u bytes short",
                         (unsigned)remaining);
                err = ESP_FAIL;
            }
            continue;
        }
        if (r <= 0) {
            ESP_LOGE(TAG, "ota: socket error %d with %u bytes to go",
                     r, (unsigned)remaining);
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(h, buf, r);
        remaining -= r;
    }
    free(buf);
    if (err == ESP_OK) err = esp_ota_end(h);         // verifies the image
    else esp_ota_abort(h);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(dst);

    httpd_resp_send(req, "", 0);
    pv_ws_broadcast(pv_json_response("ota_fw", err == ESP_OK ? 1 : 0));
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "ota ok, rebooting");
        xTaskCreate(ota_reboot_task, "pv_ota_rst", 2048, NULL, 5, NULL);
    } else {
        ESP_LOGE(TAG, "ota failed: %d", err);
        // No reboot on this path, so nothing else would ever restart the
        // renderer that was stopped before the write began. The strip stayed
        // dark until the vent was unplugged.
        pv_rgb_resume();
    }
    return ESP_OK;
}

// ---------- server ----------

/* ── The captive portal ──────────────────────────────────────────────────
 *
 * Joining the vent's hotspot on a phone should open its page by itself, the
 * way the factory firmware does. Two pieces are needed and neither existed.
 *
 * A phone decides it is "behind a portal" by fetching a known URL and seeing
 * something other than the expected answer: iOS asks captive.apple.com for
 * hotspot-detect.html and wants a body saying Success, Android asks for
 * generate_204 and wants an empty 204, Windows wants "Microsoft NCSI". Each
 * of those is a different host, so the vent has to be the DNS server for the
 * hotspot and answer every name with its own address -- then all three probes
 * arrive here, hit no registered path, and fall through to the wildcard
 * handler below, which redirects. That redirect is what opens the sheet.
 *
 * The DNS half only ever answers queries that arrived from the hotspot's own
 * subnet. A vent that is also on the house network must not start answering
 * DNS for anything else on it. */

static esp_err_t portal_get(httpd_req_t *req)
{
    // Anything that is not a path this firmware serves is a probe. Send it to
    // the page; every OS treats the redirect as "there is a portal here".
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PV_AP_PORTAL_IP "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

#define DNS_PORT 53

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "captive dns: socket failed"); vTaskDelete(NULL); return; }
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&me, sizeof me) < 0) {
        ESP_LOGE(TAG, "captive dns: bind failed");
        close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "captive dns up on :%d", DNS_PORT);

    uint8_t buf[256];
    for (;;) {
        struct sockaddr_in from; socklen_t flen = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;

        // Only for clients on the hotspot. Never answer for the house network.
        uint32_t a = ntohl(from.sin_addr.s_addr);
        if ((a & 0xFFFFFF00u) != (PV_AP_PORTAL_NET & 0xFFFFFF00u)) continue;

        uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
        if ((buf[2] & 0x80) || qd != 1) continue;      // a reply, or not one question

        // Walk the single question to find where it ends.
        int p = 12;
        while (p < n && buf[p]) { p += buf[p] + 1; if (p > 200) break; }
        p += 5;                                        // the root label + qtype + qclass
        if (p > n || p + 16 > (int)sizeof buf) continue;

        buf[2] = 0x81; buf[3] = 0x80;                  // response, recursion available
        buf[6] = 0; buf[7] = 1;                        // one answer
        buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;

        uint8_t *w = buf + p;
        *w++ = 0xC0; *w++ = 0x0C;                      // name: pointer back to the question
        *w++ = 0x00; *w++ = 0x01;                      // type A
        *w++ = 0x00; *w++ = 0x01;                      // class IN
        *w++ = 0; *w++ = 0; *w++ = 0; *w++ = 60;       // ttl 60s
        *w++ = 0x00; *w++ = 0x04;                      // 4 bytes of address
        uint32_t ip = htonl(PV_AP_PORTAL_NET | 1u);    // the hotspot's own address
        memcpy(w, &ip, 4); w += 4;

        sendto(sock, buf, (int)(w - buf), 0, (struct sockaddr *)&from, flen);
    }
}

esp_err_t pv_http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 8;
    cfg.stack_size = 8192;
    cfg.close_fn = ws_close_fn;      // see ws_close_fn: stops the table filling with ghosts
    // The page is a quarter of a megabyte off an ESP32 over Wi-Fi. The default
    // five seconds is a plausible amount of time for one write to a phone on a
    // weak signal to take, and hitting it truncated the page. Per CHUNK now,
    // so this is "no progress at all for twenty seconds", not a deadline for
    // the whole transfer.
    // Without this, "/*" is matched as a literal path and the probes 404
    // instead of redirecting. The specific handlers are registered first and
    // the first match wins, so the wildcard only ever sees what they did not.
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.send_wait_timeout = 20;
    cfg.recv_wait_timeout = 20;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    static const httpd_uri_t anim = { .uri = "/anim", .method = HTTP_POST, .handler = anim_post };
    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    static const httpd_uri_t ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler,
        .is_websocket = true,
    };
    static const httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    static const httpd_uri_t backup = {
        .uri = "/backup", .method = HTTP_GET, .handler = backup_get,
    };
    httpd_register_uri_handler(s_server, &anim);
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &ws);
    httpd_register_uri_handler(s_server, &ota);
    httpd_register_uri_handler(s_server, &backup);

    // Last, so it only ever catches what the handlers above did not: the
    // connectivity probes, which is what opens the portal on a phone.
    static const httpd_uri_t portal = {
        .uri = "/*", .method = HTTP_GET, .handler = portal_get,
    };
    httpd_register_uri_handler(s_server, &portal);

    static bool dns_started;
    if (!dns_started) { dns_started = true; xTaskCreate(dns_task, "pv_dns", 3072, NULL, 5, NULL); }

    s_up = true;
    ESP_LOGI(TAG, "http up");
    return ESP_OK;
}

// Used by the boot-probation health check: an image only counts as good once
// it is actually answering requests, not merely holding an IP.
bool pv_http_is_up(void) { return s_up; }
