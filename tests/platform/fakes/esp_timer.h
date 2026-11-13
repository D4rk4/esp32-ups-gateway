#pragma once
#include <cstdint>
inline int64_t host_time_us=1234000;
inline int64_t esp_timer_get_time(){return host_time_us;}
