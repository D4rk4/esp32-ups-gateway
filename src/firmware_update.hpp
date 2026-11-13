#pragma once
#include <cstdint>
namespace gateway {
constexpr uint64_t kFirmwareProbationMs=120000;
void checkFirmwareProbation(uint64_t now_ms,bool services_ready,bool ethernet_link,bool has_ip);
}
