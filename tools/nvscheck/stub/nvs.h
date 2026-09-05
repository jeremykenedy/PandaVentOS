#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char *ns,nvs_open_mode_t m,nvs_handle_t *h){(void)ns;(void)m;(void)h;return ESP_FAIL;}
static inline esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *o,size_t *l){(void)h;(void)k;(void)o;(void)l;return ESP_FAIL;}
static inline void nvs_close(nvs_handle_t h){(void)h;}
