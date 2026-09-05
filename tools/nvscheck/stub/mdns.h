#pragma once
#include "esp_err.h"
static inline esp_err_t mdns_init(void){return ESP_OK;}
static inline esp_err_t mdns_hostname_set(const char *h){(void)h;return ESP_OK;}
static inline esp_err_t mdns_instance_name_set(const char *h){(void)h;return ESP_OK;}
static inline esp_err_t mdns_service_add(const char *a,const char *b,const char *c,uint16_t p,void *t,size_t n){(void)a;(void)b;(void)c;(void)p;(void)t;(void)n;return ESP_OK;}
