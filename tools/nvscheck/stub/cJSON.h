/* Host stub. pv_wifi.c builds a scan-result document with these; the salvage
   under test never runs that path, so they only have to compile and link. */
#pragma once
#include <stddef.h>
typedef struct cJSON cJSON;
static inline cJSON *cJSON_CreateObject(void){return (cJSON*)0;}
static inline cJSON *cJSON_AddObjectToObject(cJSON*o,const char*k){(void)o;(void)k;return (cJSON*)0;}
static inline cJSON *cJSON_AddArrayToObject(cJSON*o,const char*k){(void)o;(void)k;return (cJSON*)0;}
static inline cJSON *cJSON_CreateObject_(void){return (cJSON*)0;}
static inline void cJSON_AddStringToObject(cJSON*o,const char*k,const char*v){(void)o;(void)k;(void)v;}
static inline void cJSON_AddNumberToObject(cJSON*o,const char*k,double v){(void)o;(void)k;(void)v;}
static inline void cJSON_AddItemToArray(cJSON*a,cJSON*i){(void)a;(void)i;}
static inline void cJSON_Delete(cJSON*o){(void)o;}
static inline char *cJSON_Print(cJSON*o){(void)o;return (char*)0;}
static inline char *cJSON_PrintUnformatted(cJSON*o){(void)o;return (char*)0;}
