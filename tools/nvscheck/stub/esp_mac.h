#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_MAC_WIFI_STA = 0, ESP_MAC_WIFI_SOFTAP = 1 } esp_mac_type_t;
static inline esp_err_t esp_read_mac(uint8_t *m, esp_mac_type_t t){(void)t;for(int i=0;i<6;i++)m[i]=0xAA;return ESP_OK;}
static inline esp_err_t esp_base_mac_addr_get(uint8_t *m){for(int i=0;i<6;i++)m[i]=0xAA;return ESP_OK;}
