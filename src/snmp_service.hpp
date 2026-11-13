#pragma once
#include <atomic>
#include <cstdint>
#include "snmp.hpp"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace gateway {
struct Config;
class UpsService;
class EventBus;

// sent/failed count trap attempts per configured target. sent indicates local
// UDP acceptance, with no delivery acknowledgement. dropped/queued are events.
struct SnmpStats {
  uint32_t sent{0}, failed{0}, dropped{0}, queued{0};
};

class SnmpService {
 public:
  SnmpService(const Config&, UpsService&, EventBus&, uint32_t boot_id);
  bool begin();
  SnmpStats stats() const;

 private:
  static void taskEntry(void*);
  void run();

  const Config& config_;
  UpsService& ups_;
  EventBus& events_;
  const uint32_t boot_id_;
  snmp::ReplayGuard replay_;
  TaskHandle_t task_{nullptr};
  std::atomic<uint32_t> sent_{0}, failed_{0};
};
}  // namespace gateway
