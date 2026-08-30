#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct { uint32_t duration0:15, level0:1, duration1:15, level1:1; } rmt_symbol_word_t;
typedef void *rmt_channel_handle_t;
typedef void *rmt_encoder_handle_t;
typedef struct { int loop_count; } rmt_transmit_config_t;
typedef struct { int gpio_num, clk_src, mem_block_symbols, trans_queue_depth; uint32_t resolution_hz; } rmt_tx_channel_config_t;
#define RMT_CLK_SRC_DEFAULT 0
static inline esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *c, rmt_channel_handle_t *h){(void)c;(void)h;return ESP_OK;}
static inline esp_err_t rmt_enable(rmt_channel_handle_t h){(void)h;return ESP_OK;}
static inline esp_err_t rmt_disable(rmt_channel_handle_t h){(void)h;return ESP_OK;}
static inline esp_err_t rmt_del_channel(rmt_channel_handle_t h){(void)h;return ESP_OK;}
static inline esp_err_t rmt_transmit(rmt_channel_handle_t h, rmt_encoder_handle_t e, const void *p, size_t n, const rmt_transmit_config_t *c){(void)h;(void)e;(void)p;(void)n;(void)c;return ESP_OK;}
static inline esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t h, int t){(void)h;(void)t;return ESP_OK;}
