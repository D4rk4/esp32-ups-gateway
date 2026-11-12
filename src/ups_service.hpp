#pragma once
#include "config_model.hpp"
#include "event_bus.hpp"
#include "ups_driver.hpp"
#include <freertos/task.h>
#include <string>

namespace gateway {
struct ControlStatus {
  uint32_t last_id{0};
  std::string last_command, last_state, last_message;
};

// Config and EventBus must outlive this service. One worker exclusively owns
// Serial2; HTTP/SNMP callers only copy snapshots or enqueue typed commands.
class UpsService {
 public:
  UpsService(const Config& config, EventBus& events);
  bool begin();
  ups::Snapshot snapshot() const;
  ControlStatus controlStatus() const;
  bool enqueue(ups::Command command, const ups::CommandArgs& args,
               uint32_t& id, std::string& error);
  const ups::UpsDriver* driver() const { return driver_; }

 private:
  struct Request {
    ups::Command command;
    ups::CommandArgs args;
    uint32_t id;
  };
  static void taskEntry(void* context);
  void run();
  bool validateCommandLocked(ups::Command command, const ups::CommandArgs& args,
                             uint32_t now, ups::EncodedCommand& encoded,
                             std::string& error) const;
  void finishCommand(const Request& request, bool sent, const std::string& message);

  const Config& config_;
  EventBus& events_;
  const ups::UpsDriver* driver_{nullptr};
  mutable SemaphoreHandle_t mutex_{nullptr};
  QueueHandle_t queue_{nullptr};
  TaskHandle_t task_{nullptr};
  ups::Snapshot state_;
  ControlStatus control_;
  bool command_pending_{false};
  uint32_t next_id_{0}, ready_command_id_{0};
};
}
