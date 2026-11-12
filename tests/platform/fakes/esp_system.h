#pragma once
#include <cstddef>
void esp_fill_random(void* buffer, size_t length);
const char* esp_err_to_name(int error);
