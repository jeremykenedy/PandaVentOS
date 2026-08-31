#pragma once
#include <stdint.h>
/* A clock the tests can move.
 *
 * The device's is monotonic microseconds since boot. This one starts at zero
 * and only advances when a test says so, which is the only way to reach a
 * timeout without actually waiting for it. */
extern int64_t fc_clock_us;
static inline int64_t esp_timer_get_time(void) { return fc_clock_us; }
