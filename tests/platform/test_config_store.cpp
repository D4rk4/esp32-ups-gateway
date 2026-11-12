#include "config_store.hpp"
#include "config_json.hpp"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cassert>
#include <cstring>
#include <iostream>

namespace {
constexpr esp_err_t failure = 2;
enum class Fault { None, Init, ReadOpen, Query, Read, Parse, WriteOpen, Write, Commit, Serialize };
Fault fault = Fault::None;
std::string disk;
gateway::Config stored;
unsigned writes = 0, randomCalls = 0;
void setup(Fault value, bool existing) {
  fault = value; disk = existing ? "stored" : ""; writes = 0; randomCalls = 0;
  stored = gateway::Config{}; stored.web_user = "existing"; stored.web_password = "existing-password";
}
}

void esp_fill_random(void* buffer, size_t length) {
  std::memset(buffer, static_cast<int>(++randomCalls % 62), length);
}
const char* esp_err_to_name(int) { return "test error"; }
esp_err_t nvs_flash_init_partition(const char*) { return fault == Fault::Init ? failure : ESP_OK; }
esp_err_t nvs_open_from_partition(const char*, const char*, int mode, nvs_handle_t* handle) {
  if (mode == NVS_READONLY) {
    if (fault == Fault::ReadOpen) return failure;
    if (disk.empty()) return ESP_ERR_NVS_NOT_FOUND;
  } else if (fault == Fault::WriteOpen) return failure;
  *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void* data, size_t* size) {
  if (!data && fault == Fault::Query) return failure;
  if (data && fault == Fault::Read) return failure;
  if (disk.empty()) return ESP_ERR_NVS_NOT_FOUND;
  if (data) std::memcpy(data, disk.data(), disk.size());
  *size = disk.size(); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) {
  ++writes; return fault == Fault::Write ? failure : ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) { return fault == Fault::Commit ? failure : ESP_OK; }
void nvs_close(nvs_handle_t) {}

namespace gateway {
std::string configJson(const Config& config, bool) {
  if (fault == Fault::Serialize) return {};
  stored = config; return "stored";
}
bool mergeConfigJson(const Config&, const std::string&, Config& out, std::string& error, bool) {
  if (fault == Fault::Parse) { error = "invalid saved record"; return false; }
  out = stored; return true;
}
}

int main() {
  for (const auto issue : {Fault::Init, Fault::ReadOpen, Fault::Query, Fault::Read, Fault::Parse,
                           Fault::WriteOpen, Fault::Write, Fault::Commit, Fault::Serialize}) {
    const bool existing = issue == Fault::ReadOpen || issue == Fault::Query || issue == Fault::Read || issue == Fault::Parse;
    setup(issue, existing);
    gateway::ConfigStore store;
    assert(!store.begin() && !store.writable() && store.generatedCredentials());
    assert(store.saved().web_password.size() == 24 && store.saved().web_password != "ups");
    if (existing) assert(disk == "stored" && writes == 0);
  }
  setup(Fault::None, false);
  gateway::ConfigStore fresh;
  assert(fresh.begin() && fresh.writable() && fresh.generatedCredentials());
  assert(fresh.saved().web_user == "ups" && fresh.saved().web_password == "ups");
  assert(fresh.saved().snmp_community.size() == 24 && fresh.saved().snmp_community != fresh.saved().web_password);
  assert(writes == 1);
  setup(Fault::None, true);
  gateway::ConfigStore existing;
  assert(existing.begin() && existing.writable() && !existing.generatedCredentials());
  assert(existing.saved().web_user == "existing" && existing.saved().web_password == "existing-password");
  assert(writes == 0);
  std::string error;
  assert(existing.resetDefaults(false, error));
  assert(existing.saved().web_user == "existing" && existing.saved().web_password == "existing-password");
  std::cout << "config store: random fallback, factory persistence and existing config passed\n";
}
