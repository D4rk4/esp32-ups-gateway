#include "config_store.hpp"
#include "config_json.hpp"
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <utility>

namespace gateway {
namespace {
constexpr const char* kPartition = "appcfg";
constexpr const char* kNamespace = "gateway";
constexpr const char* kKey = "config";
constexpr size_t kMaxBlob = kMaxConfigJson;
struct Handle {
  nvs_handle_t value{};
  ~Handle() { if (value) nvs_close(value); }
};
std::string nvsError(const char* operation, esp_err_t code) {
  return std::string(operation) + ": " + esp_err_to_name(code) +
    ". Исходная конфигурация не сбрасывается автоматически";
}
std::string randomSecret() {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string result; result.reserve(24);
  while (result.size() < 24) {
    uint8_t bytes[32]; esp_fill_random(bytes,sizeof(bytes));
    for (const auto byte : bytes) {
      if (byte >= 248) continue; // Rejection sampling keeps all 62 characters equally likely.
      result.push_back(alphabet[byte % 62]);
      if (result.size() == 24) break;
    }
  }
  return result;
}
Config freshDefaults() {
  Config result;
  do { result.snmp_community = randomSecret(); } while (result.snmp_community == result.web_password);
  for (auto& target : result.traps) target.community = result.snmp_community;
  return result;
}
}

bool ConfigStore::begin() {
  writable_ = false; generated_credentials_ = true; error_.clear();
  saved_ = freshDefaults();
  do { saved_.web_password = randomSecret(); } while (saved_.web_password == saved_.snmp_community);
  const auto initialized = nvs_flash_init_partition(kPartition);
  if (initialized != ESP_OK) { error_ = nvsError("Не удалось открыть раздел appcfg", initialized); return false; }
  bool missing = false;
  {
    Handle handle;
    const auto opened = nvs_open_from_partition(kPartition,kNamespace,NVS_READONLY,&handle.value);
    if (opened == ESP_ERR_NVS_NOT_FOUND) missing = true;
    else if (opened != ESP_OK) { error_ = nvsError("Не удалось прочитать пространство gateway",opened); return false; }
    else {
      size_t size = 0;
      const auto queried = nvs_get_blob(handle.value,kKey,nullptr,&size);
      if (queried == ESP_ERR_NVS_NOT_FOUND) missing = true;
      else if (queried != ESP_OK) { error_ = nvsError("Не удалось прочитать размер конфигурации",queried); return false; }
      else {
        if (size == 0 || size > kMaxBlob) { error_ = "Сохранённая конфигурация имеет недопустимый размер; запись заблокирована"; return false; }
        std::string blob(size,'\0');
        const auto read = nvs_get_blob(handle.value,kKey,&blob[0],&size);
        if (read != ESP_OK) { error_ = nvsError("Не удалось прочитать конфигурацию",read); return false; }
        blob.resize(size); Config loaded;
        if (!mergeConfigJson(Config{},blob,loaded,error_,true)) {
          error_ = "Сохранённая конфигурация отклонена; запись заблокирована: " + error_;
          return false;
        }
        saved_ = std::move(loaded); generated_credentials_ = false; writable_ = true; return true;
      }
    }
  }
  if (missing) {
    // This is the sole automatic write path: the key did not exist. Existing
    // unreadable, corrupt or unsupported records are never replaced on boot.
    Config factory = saved_;
    factory.web_password = Config{}.web_password;
    if (!persist(factory,error_)) return false;
    saved_ = std::move(factory); writable_ = true; return true;
  }
  error_ = "Не удалось определить состояние конфигурации"; return false;
}
const Config& ConfigStore::saved() const { return saved_; }
bool ConfigStore::writable() const { return writable_; }
bool ConfigStore::generatedCredentials() const { return generated_credentials_; }
const std::string& ConfigStore::error() const { return error_; }
std::string ConfigStore::toJson(bool include_secrets) const { return configJson(saved_,include_secrets); }

bool ConfigStore::persist(const Config& candidate, std::string& error) {
  // Validate the exact serialized record before touching flash. This also
  // requires schema, every field, all credentials, and a registered driver.
  const auto blob = configJson(candidate,true); Config verified;
  if (blob.empty() || blob.size() > kMaxBlob) { error = "Не удалось сформировать JSON-конфигурацию"; return false; }
  if (!mergeConfigJson(Config{},blob,verified,error,true)) return false;
  Handle handle;
  auto result = nvs_open_from_partition(kPartition,kNamespace,NVS_READWRITE,&handle.value);
  if (result != ESP_OK) { error = nvsError("Не удалось открыть конфигурацию для записи",result); return false; }
  result = nvs_set_blob(handle.value,kKey,blob.data(),blob.size());
  if (result != ESP_OK) { error = nvsError("Не удалось записать конфигурацию",result); return false; }
  result = nvs_commit(handle.value);
  if (result != ESP_OK) { error = nvsError("Не удалось подтвердить запись конфигурации",result); return false; }
  error.clear(); return true;
}
bool ConfigStore::mergeAndSave(const std::string& body, std::string& error) {
  error.clear();
  if (!writable_) { error = error_.empty() ? "Запись конфигурации недоступна" : error_; return false; }
  Config candidate;
  if (!mergeConfigJson(saved_,body,candidate,error)) { error_ = error; return false; }
  const auto previous = configJson(saved_,true);
  const auto next = configJson(candidate,true);
  if (previous.empty() || next.empty()) { error = "Недостаточно памяти для конфигурации"; error_ = error; return false; }
  if (previous == next) { error_.clear(); return true; }
  if (!persist(candidate,error)) { error_ = error; return false; }
  saved_ = std::move(candidate); error_.clear(); return true;
}
bool ConfigStore::resetPassword(std::string& generated, std::string& error) {
  generated.clear(); error.clear();
  if (!writable_) { error = error_.empty() ? "Запись конфигурации недоступна" : error_; return false; }
  Config candidate = saved_;
  do { candidate.web_password = randomSecret(); } while (candidate.web_password == saved_.web_password || candidate.web_password == candidate.snmp_community);
  if (!persist(candidate,error)) { error_ = error; return false; }
  saved_ = std::move(candidate); generated = saved_.web_password; error_.clear(); return true;
}
bool ConfigStore::resetDefaults(bool keep_network,std::string& error) {
  error.clear();
  if(!writable_){error=error_.empty()?"Запись конфигурации недоступна":error_;return false;}
  Config candidate=defaultsPreservingAccess(saved_,keep_network);
  if(!persist(candidate,error)){error_=error;return false;}
  saved_=std::move(candidate);error_.clear();return true;
}
}
