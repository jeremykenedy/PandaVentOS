#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2

#ifndef ESP_ERROR_CHECK
#define ESP_ERROR_CHECK(x) do { (void)(x); } while (0)
#endif
