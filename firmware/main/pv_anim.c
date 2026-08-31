// NOT STOCK. An animation held in RAM, and nothing else.
//
// See the block comment in pv.h for why RAM rather than flash. This file is
// deliberately the whole of it: a buffer, a lock, and three ways to touch it.
// There is no decoder, no file format and no persistence, because each of
// those would be a place for a device with 80 KB of free heap to fall over.
#include "pv.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "pv_anim";

static uint8_t *s_rgb;
static int s_frames, s_pixels;
static SemaphoreHandle_t s_lock;

static void anim_lock_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

// The render task calls pv_anim_frame on every frame. A mutex it can block on
// would let an upload stall the strip, so the readers take it with a zero
// timeout and fall back to reporting nothing loaded. A single dark frame
// during an upload is not worth a stutter in the render loop.
static bool take(TickType_t wait)
{
    anim_lock_init();
    return s_lock && xSemaphoreTake(s_lock, wait) == pdTRUE;
}

static void give(void) { if (s_lock) xSemaphoreGive(s_lock); }

bool pv_anim_set(const uint8_t *rgb, int frames, int pixels)
{
    if (!rgb || frames <= 0 || pixels <= 0) return false;
    if (pixels > PV_ANIM_PIXELS) {
        ESP_LOGW(TAG, "%d pixels per frame, max %d", pixels, PV_ANIM_PIXELS);
        return false;
    }
    size_t need = (size_t)frames * pixels * 3;
    if (need > PV_ANIM_MAX_BYTES) {
        ESP_LOGW(TAG, "%u bytes, max %u", (unsigned)need, (unsigned)PV_ANIM_MAX_BYTES);
        return false;
    }
    // Ask for the new block BEFORE freeing the old one, so a device that
    // cannot fit the new animation keeps the one it was playing rather than
    // ending up with neither.
    uint8_t *buf = malloc(need);
    if (!buf) {
        ESP_LOGW(TAG, "no room for %u bytes (largest free block %u)",
                 (unsigned)need,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return false;
    }
    memcpy(buf, rgb, need);
    if (!take(portMAX_DELAY)) { free(buf); return false; }
    free(s_rgb);
    s_rgb = buf; s_frames = frames; s_pixels = pixels;
    give();
    // Start the new animation at its first row rather than at whatever index
    // the previous one had reached, which on a short upload would mean the
    // first thing anyone sees is the middle of their own animation.
    pv_rgb_anim_rewind();
    ESP_LOGI(TAG, "loaded %d frames of %d pixels (%u bytes)",
             frames, pixels, (unsigned)need);
    return true;
}

void pv_anim_clear(void)
{
    if (!take(portMAX_DELAY)) return;
    free(s_rgb);
    s_rgb = NULL; s_frames = 0; s_pixels = 0;
    give();
    pv_rgb_anim_rewind();
    ESP_LOGI(TAG, "cleared");
}

void pv_anim_info(pv_anim_info_t *out)
{
    if (!out) return;
    if (!take(pdMS_TO_TICKS(20))) {
        // Being asked while an upload holds the lock. Report what is there
        // rather than blocking the caller, which may be the HTTP task.
        out->frames = s_frames; out->pixels = s_pixels;
        out->bytes = s_frames * s_pixels * 3;
        return;
    }
    out->frames = s_frames; out->pixels = s_pixels;
    out->bytes  = s_frames * s_pixels * 3;
    give();
}

// COPIES the frame out rather than handing back a pointer into the buffer.
//
// A pointer would be returned with the lock already released, and the very
// next thing an upload does is free that buffer: the renderer would then be
// reading freed heap on a device where the heap is under real pressure. The
// copy is 96 bytes.
int pv_anim_copy(int i, uint8_t *dst, int max_pixels)
{
    if (!dst || max_pixels <= 0) return 0;
    if (!take(0)) return 0;             // see take(): never block the renderer
    int n = 0;
    if (s_rgb && s_frames > 0 && s_pixels > 0) {
        if (i < 0) i = 0;
        i %= s_frames;
        n = s_pixels < max_pixels ? s_pixels : max_pixels;
        memcpy(dst, s_rgb + (size_t)i * s_pixels * 3, (size_t)n * 3);
    }
    give();
    return n;
}
