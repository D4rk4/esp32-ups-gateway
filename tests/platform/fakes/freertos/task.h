#pragma once
#include "FreeRTOS.h"
inline unsigned host_task_yields=0;
inline void vTaskDelay(uint32_t ticks){if(ticks)++host_task_yields;}
