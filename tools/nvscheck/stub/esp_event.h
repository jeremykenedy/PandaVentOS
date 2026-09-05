#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
#define ESP_EVENT_ANY_ID -1
static inline esp_err_t esp_event_loop_create_default(void){return ESP_OK;}
static inline esp_err_t esp_event_handler_instance_register(esp_event_base_t b,int32_t id,void(*h)(void*,esp_event_base_t,int32_t,void*),void*a,esp_event_handler_instance_t*i){(void)b;(void)id;(void)h;(void)a;(void)i;return ESP_OK;}
static inline esp_err_t esp_event_handler_register(esp_event_base_t b,int32_t id,void(*h)(void*,esp_event_base_t,int32_t,void*),void*a){(void)b;(void)id;(void)h;(void)a;return ESP_OK;}
