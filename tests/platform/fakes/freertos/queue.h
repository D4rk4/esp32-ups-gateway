#pragma once
#include "FreeRTOS.h"
#include <deque>
#include <vector>
#include <cstring>
#include <memory>
struct HostQueue{size_t capacity,size;std::deque<std::vector<unsigned char>> values;};
using QueueHandle_t=HostQueue*;
inline std::vector<std::unique_ptr<HostQueue>>& hostQueues(){static std::vector<std::unique_ptr<HostQueue>> queues;return queues;}
inline QueueHandle_t xQueueCreate(size_t capacity,size_t size){hostQueues().emplace_back(new HostQueue{capacity,size,{}});return hostQueues().back().get();}
inline BaseType_t xQueueSend(QueueHandle_t q,const void* value,uint32_t){if(q->values.size()>=q->capacity)return pdFALSE;const auto* p=static_cast<const unsigned char*>(value);q->values.emplace_back(p,p+q->size);return pdTRUE;}
inline BaseType_t xQueueReceive(QueueHandle_t q,void* out,uint32_t){if(q->values.empty())return pdFALSE;std::memcpy(out,q->values.front().data(),q->size);q->values.pop_front();return pdTRUE;}
inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q){return UBaseType_t(q->values.size());}
