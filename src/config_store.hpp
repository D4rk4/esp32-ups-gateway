#pragma once
#include "config_model.hpp"
#include <string>
namespace gateway {
// Saved configuration is separate from the application's immutable boot copy.
// Serialize concurrent HTTP/USB access externally. Saved changes apply after reboot.
class ConfigStore {
 public:
  bool begin();
  const Config& saved() const;
  bool writable() const;
  bool generatedCredentials() const;
  const std::string& error() const;
  std::string toJson(bool include_secrets = false) const;
  bool mergeAndSave(const std::string& body, std::string& error);
  bool resetPassword(std::string& generated, std::string& error);
  bool resetDefaults(bool keep_network, std::string& error);
 private:
  bool persist(const Config& candidate, std::string& error);
  Config saved_;
  bool writable_{false};
  bool generated_credentials_{false};
  std::string error_;
};
}
