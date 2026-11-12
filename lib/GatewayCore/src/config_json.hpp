#pragma once
#include "config_model.hpp"
#include <string>
namespace gateway {
constexpr size_t kMaxConfigJson = 12288;
constexpr size_t kConfigDocumentCapacity = 24576;
bool mergeConfigJson(const Config& base, const std::string& json, Config& out,
                     std::string& error, bool importing = false);
std::string configJson(const Config& config, bool secrets);
}
