/* Host stub. The test supplies a real NVS partition image and this hands it
   to the code under test, so pv_wifi.c's salvage walks genuine on-flash
   bytes rather than a reimplementation of them. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_DATA_NVS = 2 } esp_partition_subtype_t;
typedef struct { esp_partition_type_t type; esp_partition_subtype_t subtype;
                 uint32_t address; uint32_t size; char label[17]; } esp_partition_t;
extern const uint8_t *nvs_test_image;      /* set by the test */
extern uint32_t       nvs_test_size;
static inline const esp_partition_t *esp_partition_find_first(
        esp_partition_type_t t, esp_partition_subtype_t st, const char *label) {
    (void)t; (void)st; (void)label;
    static esp_partition_t p;
    if (!nvs_test_image) return 0;
    p.type = ESP_PARTITION_TYPE_DATA; p.subtype = ESP_PARTITION_SUBTYPE_DATA_NVS;
    p.address = 0x9000; p.size = nvs_test_size;
    return &p;
}
static inline esp_err_t esp_partition_read(const esp_partition_t *p, size_t off,
                                           void *dst, size_t n) {
    if (!p || off + n > nvs_test_size) return ESP_FAIL;
    memcpy(dst, nvs_test_image + off, n);
    return ESP_OK;
}
