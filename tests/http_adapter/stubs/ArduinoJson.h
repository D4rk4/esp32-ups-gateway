#pragma once

#include <ArduinoJson.hpp>

using namespace ArduinoJson;

namespace fixture {
void* jsonAllocate(size_t size);
}

struct HttpTestJsonAllocator {
  void* allocate(size_t size) { return fixture::jsonAllocate(size); }
  void deallocate(void* ptr) { free(ptr); }
  void* reallocate(void* ptr, size_t size) { return realloc(ptr,size); }
};

using HttpTestJsonDocument = ArduinoJson::BasicJsonDocument<HttpTestJsonAllocator>;
// Replaces GNU ld --wrap=malloc; only DynamicJsonDocument allocations use fixture::jsonAllocate.
#define DynamicJsonDocument HttpTestJsonDocument
