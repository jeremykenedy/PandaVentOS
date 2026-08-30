#pragma once
#include "rmt_tx.h"
#include <stddef.h>
typedef enum { RMT_ENCODING_RESET = 0, RMT_ENCODING_COMPLETE = 1, RMT_ENCODING_MEM_FULL = 2 } rmt_encode_state_t;
typedef struct rmt_encoder_t {
    size_t (*encode)(struct rmt_encoder_t *, rmt_channel_handle_t, const void *, size_t, rmt_encode_state_t *);
    esp_err_t (*reset)(struct rmt_encoder_t *);
    esp_err_t (*del)(struct rmt_encoder_t *);
} rmt_encoder_t;
static inline esp_err_t rmt_encoder_reset(rmt_encoder_handle_t e){(void)e;return ESP_OK;}

/* IDF provides these in sys/param.h and the encoder API. */
#ifndef __containerof
#define __containerof(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
static inline esp_err_t rmt_del_encoder(rmt_encoder_handle_t e){(void)e;return ESP_OK;}
static inline esp_err_t rmt_new_copy_encoder(const void *c, rmt_encoder_handle_t *h){(void)c;(void)h;return ESP_OK;}
static inline esp_err_t rmt_new_bytes_encoder(const void *c, rmt_encoder_handle_t *h){(void)c;(void)h;return ESP_OK;}
typedef struct { int dummy; } rmt_copy_encoder_config_t;
typedef struct {
    rmt_symbol_word_t bit0;
    rmt_symbol_word_t bit1;
    struct { unsigned msb_first : 1; } flags;
} rmt_bytes_encoder_config_t;
