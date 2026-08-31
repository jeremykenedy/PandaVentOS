#pragma once
/* Host stub. pv_anim.c reports the largest free block when a malloc fails,
   which is a diagnostic for the device's fragmented heap and has no meaning
   on a machine with virtual memory. */
#include <stddef.h>
#define MALLOC_CAP_8BIT 0
static inline size_t heap_caps_get_largest_free_block(int caps)
{
    (void)caps;
    return 0;
}
