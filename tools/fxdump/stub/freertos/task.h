#pragma once
#include "FreeRTOS.h"
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline int xTaskCreate(void (*f)(void *), const char *n, uint32_t s,
                              void *p, uint32_t pr, TaskHandle_t *h)
{ (void)f;(void)n;(void)s;(void)p;(void)pr;(void)h; return 1; }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
static inline void xTaskNotifyGive(TaskHandle_t h) { (void)h; }
static inline uint32_t ulTaskNotifyTake(int c, TickType_t t) { (void)c;(void)t; return 0; }

typedef enum { eNoAction = 0, eSetBits, eIncrement, eSetValueWithOverwrite,
               eSetValueWithoutOverwrite } eNotifyAction;
static inline int xTaskNotify(TaskHandle_t h, uint32_t v, eNotifyAction a)
{ (void)h;(void)v;(void)a; return 1; }
static inline int xTaskNotifyWait(uint32_t clr_entry, uint32_t clr_exit,
                                  uint32_t *val, TickType_t t)
{ (void)clr_entry;(void)clr_exit;(void)t; if (val) *val = 0; return 0; }
