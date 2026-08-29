// HTTP server: "/" serves the byte-exact factory web app (embedded gzip),
// "/ws" is the factory WebSocket (full state push on connect, dispatch on
// receive, broadcast on change), "/ota" accepts firmware uploads with the
// factory OTA-Type header. The updater accepts any valid app image for this
// partition layout, which is exactly what makes restoring the factory
// firmware a plain upload.
#include "pv.h"

#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pv_http";

extern const uint8_t index_gz_start[] asm("_binary_factory_index_html_gz_start");
extern const uint8_t index_gz_end[]   asm("_binary_factory_index_html_gz_end");

static httpd_handle_t s_server;
static volatile bool s_up;

#define MAX_WS 4
static int s_ws_fd[MAX_WS];
static int s_ws_count;

// ---------- pages ----------

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)index_gz_start,
                           index_gz_end - index_gz_start);
}

// ---------- websocket ----------

static void ws_track(int fd)
{
    for (int i = 0; i < s_ws_count; ++i)
        if (s_ws_fd[i] == fd) return;
    if (s_ws_count < MAX_WS) s_ws_fd[s_ws_count++] = fd;
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

void pv_ws_push_state(void)
{
    pv_ws_broadcast(pv_json_state());
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {   // handshake completed
        int fd = httpd_req_to_sockfd(req);
        ws_track(fd);
        ESP_LOGI(TAG, "ws client fd=%d (%d total)", fd, s_ws_count);
        pv_ws_push_state();          // factory pushes full state on connect
        return ESP_OK;
    }
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.len == 0 || f.len > 4096) return ESP_OK;
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

static void ota_reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }

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

    // Stock stops its render task before an OTA: notification 255 at
    // 0x400dcae5, all-off through 0x400ddf98, then the task returns, so the
    // strip goes dark and the RMT channels are released for the duration.
    pv_rgb_stop();

    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(dst, req->content_len, &h);
    char *buf = err == ESP_OK ? malloc(4096) : NULL;
    if (err == ESP_OK && !buf) { esp_ota_abort(h); err = ESP_ERR_NO_MEM; }
    size_t remaining = req->content_len;
    while (err == ESP_OK && remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > 4096 ? 4096 : remaining);
        if (r <= 0) { err = ESP_FAIL; break; }
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
    }
    return ESP_OK;
}

// ---------- server ----------

esp_err_t pv_http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 8;
    cfg.stack_size = 8192;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    static const httpd_uri_t ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler,
        .is_websocket = true,
    };
    static const httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &ws);
    httpd_register_uri_handler(s_server, &ota);
    s_up = true;
    ESP_LOGI(TAG, "http up");
    return ESP_OK;
}

// Used by the boot-probation health check: an image only counts as good once
// it is actually answering requests, not merely holding an IP.
bool pv_http_is_up(void) { return s_up; }
