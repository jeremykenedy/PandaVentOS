#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define pdFALSE 0
