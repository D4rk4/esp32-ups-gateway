#pragma once
#include "FreeRTOS.h"
using SemaphoreHandle_t=void*;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t,uint32_t){return pdTRUE;}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t){return pdTRUE;}
