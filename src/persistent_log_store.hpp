#pragma once
#include "persistent_log.hpp"
#include <esp_partition.h>
#include <string>

namespace gateway {
class PersistentLogStore : public PersistentLogStorage {
 public:
  bool begin(std::string& error);
  size_t size() const override;
  bool read(size_t offset,void* destination,size_t length) override;
  bool write(size_t offset,const void* source,size_t length) override;
  bool erase(size_t offset,size_t length) override;
 private:
  const esp_partition_t* partition_{nullptr};
};
}
