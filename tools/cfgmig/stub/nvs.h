#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k);
#define ESP_ERR_NVS_NOT_FOUND (-4361)
#define ESP_ERR_NVS_NO_FREE_PAGES (-4362)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (-4363)
void nvs_close(nvs_handle_t h);
