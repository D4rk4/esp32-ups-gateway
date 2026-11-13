#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t=int;
constexpr esp_err_t ESP_OK=0,ESP_FAIL=-1;
enum esp_partition_type_t { ESP_PARTITION_TYPE_DATA=1 };
enum esp_partition_subtype_t { ESP_PARTITION_SUBTYPE_DATA_TEST=0x40 };
struct esp_partition_t { esp_partition_type_t type;esp_partition_subtype_t subtype;uint32_t address,size;char label[17]; };
extern bool host_partition_present,host_partition_fail_read,host_partition_fail_write;
extern esp_partition_t host_partition;
extern size_t host_partition_reads,host_partition_writes,host_partition_erases;
const esp_partition_t* esp_partition_find_first(esp_partition_type_t,esp_partition_subtype_t,const char*);
esp_err_t esp_partition_read(const esp_partition_t*,size_t,void*,size_t);
esp_err_t esp_partition_write(const esp_partition_t*,size_t,const void*,size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t*,size_t,size_t);
