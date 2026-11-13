#pragma once
#include "FreeRTOS.h"
#include <cassert>
#include <memory>
#include <vector>
struct HostMutex {bool locked=false;};
using SemaphoreHandle_t=HostMutex*;
inline std::vector<std::unique_ptr<HostMutex>>& hostMutexes(){static std::vector<std::unique_ptr<HostMutex>> mutexes;return mutexes;}
inline SemaphoreHandle_t xSemaphoreCreateMutex(){hostMutexes().emplace_back(new HostMutex{});return hostMutexes().back().get();}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex,uint32_t){assert(mutex&&!mutex->locked);mutex->locked=true;return pdTRUE;}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex){assert(mutex&&mutex->locked);mutex->locked=false;return pdTRUE;}
