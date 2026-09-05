/* Host stub. pv_wifi.c is compiled whole so the salvage under test is the
   shipping function; none of these are called by the test. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP = 1 } wifi_interface_t;
typedef enum { WIFI_MODE_NULL=0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
typedef enum { WIFI_AUTH_OPEN=0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK,
               WIFI_AUTH_WPA_WPA2_PSK } wifi_auth_mode_t;
typedef enum { WIFI_ALL_CHANNEL_SCAN=0, WIFI_FAST_SCAN } wifi_scan_method_t;
typedef enum { WIFI_CONNECT_AP_BY_SIGNAL=0 } wifi_sort_method_t;
typedef enum { WIFI_SCAN_TYPE_ACTIVE=0 } wifi_scan_type_t;
typedef enum { WIFI_PS_NONE=0, WIFI_PS_MIN_MODEM } wifi_ps_type_t;
typedef enum { WIFI_STORAGE_FLASH=0, WIFI_STORAGE_RAM } wifi_storage_t;
typedef struct { uint8_t ssid[33]; uint8_t ssid_len; uint8_t password[65]; uint8_t channel; wifi_auth_mode_t authmode;
                 uint8_t ssid_hidden; uint8_t max_connection; uint16_t beacon_interval;
                 wifi_scan_method_t scan_method; wifi_sort_method_t sort_method; } wifi_ap_cfg_stub_t;
typedef union { struct { uint8_t ssid[33]; uint8_t password[65]; wifi_scan_method_t scan_method;
                         wifi_sort_method_t sort_method; uint8_t channel; } sta;
                wifi_ap_cfg_stub_t ap; } wifi_config_t;
typedef struct { uint8_t ssid[33]; int8_t rssi; uint8_t primary; } wifi_ap_record_t;
typedef struct { struct { uint32_t min, max; } active; } wifi_active_scan_time_t;
typedef struct { bool show_hidden; wifi_scan_type_t scan_type; wifi_active_scan_time_t scan_time;
                 uint32_t home_chan_dwell_time; } wifi_scan_config_t;
typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() (wifi_init_config_t){0}
#define WIFI_EVENT ((esp_event_base_t)"WIFI_EVENT")
#define IP_EVENT ((esp_event_base_t)"IP_EVENT")
#define WIFI_EVENT_STA_START 0
#define WIFI_EVENT_STA_DISCONNECTED 1
#define WIFI_EVENT_SCAN_DONE 2
#define IP_EVENT_STA_GOT_IP 3
static inline esp_err_t esp_wifi_init(const wifi_init_config_t *c){(void)c;return ESP_OK;}
static inline esp_err_t esp_wifi_set_mode(wifi_mode_t m){(void)m;return ESP_OK;}
static inline esp_err_t esp_wifi_start(void){return ESP_OK;}
static inline esp_err_t esp_wifi_stop(void){return ESP_OK;}
static inline esp_err_t esp_wifi_connect(void){return ESP_OK;}
static inline esp_err_t esp_wifi_disconnect(void){return ESP_OK;}
static inline esp_err_t esp_wifi_set_ps(wifi_ps_type_t p){(void)p;return ESP_OK;}
static inline esp_err_t esp_wifi_set_storage(wifi_storage_t s){(void)s;return ESP_OK;}
static inline esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c){(void)i;(void)c;return ESP_OK;}
static inline esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c){(void)i;(void)c;return ESP_OK;}
static inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c, bool b){(void)c;(void)b;return ESP_OK;}
static inline esp_err_t esp_wifi_scan_get_ap_num(uint16_t *n){*n=0;return ESP_OK;}
static inline esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *r){(void)r;*n=0;return ESP_OK;}
static inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *r){(void)r;return ESP_FAIL;}
typedef struct { uint8_t ssid[33]; uint8_t reason; } wifi_event_sta_disconnected_t;
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_NO_AP_FOUND 201
