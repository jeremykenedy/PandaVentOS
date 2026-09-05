#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
static inline esp_err_t esp_netif_init(void){return ESP_OK;}
static inline esp_netif_t *esp_netif_create_default_wifi_sta(void){return 0;}
static inline esp_netif_t *esp_netif_create_default_wifi_ap(void){return 0;}
static inline esp_err_t esp_netif_set_hostname(esp_netif_t *n,const char *h){(void)n;(void)h;return ESP_OK;}
static inline esp_err_t esp_netif_dhcps_stop(esp_netif_t *n){(void)n;return ESP_OK;}
static inline esp_err_t esp_netif_dhcps_start(esp_netif_t *n){(void)n;return ESP_OK;}
static inline esp_err_t esp_netif_set_ip_info(esp_netif_t *n,esp_netif_ip_info_t *i){(void)n;(void)i;return ESP_OK;}
static inline uint32_t esp_ip4addr_aton(const char *s){(void)s;return 0;}
#define IP2STR(a) 0,0,0,0
#define IPSTR "%d.%d.%d.%d"
