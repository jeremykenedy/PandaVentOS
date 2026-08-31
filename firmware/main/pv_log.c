// NOT STOCK. A log you can read from the page.
//
// Every diagnosis in this project so far has needed a serial cable: the vent
// is on a shelf behind a printer, and reading why it did something meant
// unplugging it and carrying it to a laptop. Nothing about that is reasonable
// for someone who just wants to know why their vent opened.
//
// So the same lines ESP_LOG already writes to the UART are also kept here, in
// a fixed ring in RAM, and handed to the page on request.
//
// THE RULES THIS FILE EXISTS TO ENFORCE:
//
// 1. FIXED COST. One static ring, sized at compile time. It cannot grow, it
//    cannot fragment the heap, and it cannot fail to allocate at the moment
//    something is going wrong, which is exactly when a log matters.
//
// 2. NEVER BLOCK THE LOGGER. esp_log is called from every task including
//    ones holding locks. The hook takes a short mutex and copies bytes; it
//    never allocates, never calls back into esp_log, and never waits on
//    anything that could be held by a task waiting on it.
//
// 3. THE UART STILL GETS EVERYTHING. The hook chains to the previous vprintf
//    rather than replacing it, so a serial cable shows exactly what it always
//    did and this is purely additional.
#include "pv.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_LINES   64          // kept lines
#define LOG_LINE    128         // bytes each, including the terminator

typedef struct {
    int64_t  us;                // when, since boot
    char     text[LOG_LINE];
} log_line_t;

static log_line_t     s_ring[LOG_LINES];
static int            s_head;           // next slot to write
static uint32_t       s_seq;            // total lines ever written
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_chain;          // whatever was installed before us

// Colour codes and the trailing newline are for a terminal, not for a page.
static void scrub(char *p)
{
    char *w = p;
    for (char *r = p; *r; ++r) {
        if (*r == '\033') {                     // ESC [ ... m
            while (*r && *r != 'm') ++r;
            if (!*r) break;
            continue;
        }
        if (*r == '\r') continue;
        if (*r == '\n' && !*(r + 1)) continue;  // only the last one
        *w++ = *r;
    }
    *w = 0;
}

static int log_hook(const char *fmt, va_list ap)
{
    // The chain gets its own copy: vsnprintf consumes the va_list.
    va_list copy;
    va_copy(copy, ap);

    char buf[LOG_LINE];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) {
        scrub(buf);
        if (buf[0] && s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE) {
            // Zero timeout on purpose. A line is worth less than a stall: if
            // another task holds the ring for the microsecond this takes, the
            // line goes to the UART and not to the ring, and nothing waits.
            log_line_t *l = &s_ring[s_head];
            l->us = esp_timer_get_time();
            strlcpy(l->text, buf, sizeof l->text);
            s_head = (s_head + 1) % LOG_LINES;
            ++s_seq;
            xSemaphoreGive(s_lock);
        }
    }

    int r = s_chain ? s_chain(fmt, copy) : vprintf(fmt, copy);
    va_end(copy);
    return r;
}

void pv_log_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;                 // no ring, but the UART is untouched
    s_chain = esp_log_set_vprintf(log_hook);
}

int pv_log_read(pv_log_line_t *out, int max)
{
    if (!out || max <= 0 || !s_lock) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
    int have = (s_seq < LOG_LINES) ? (int)s_seq : LOG_LINES;
    if (have > max) have = max;
    // Oldest first, so the page reads top to bottom like a log should.
    int start = (s_head - have + LOG_LINES) % LOG_LINES;
    for (int i = 0; i < have; ++i) {
        const log_line_t *l = &s_ring[(start + i) % LOG_LINES];
        out[i].us = l->us;
        strlcpy(out[i].text, l->text, sizeof out[i].text);
    }
    xSemaphoreGive(s_lock);
    return have;
}

void pv_log_clear(void)
{
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    s_head = 0;
    s_seq = 0;
    xSemaphoreGive(s_lock);
}
