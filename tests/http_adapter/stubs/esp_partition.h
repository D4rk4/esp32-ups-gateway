#pragma once
#include <cstdint>
enum esp_partition_type_t { ESP_PARTITION_TYPE_APP=0, ESP_PARTITION_TYPE_DATA=1 };
enum esp_partition_subtype_t { ESP_PARTITION_SUBTYPE_APP_OTA_0=0x10, ESP_PARTITION_SUBTYPE_APP_OTA_1=0x11, ESP_PARTITION_SUBTYPE_DATA_OTA=0 };
struct esp_partition_t { esp_partition_type_t type;esp_partition_subtype_t subtype;uint32_t address,size;char label[17]; };
