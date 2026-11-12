#pragma once
#include "ups_model.hpp"
#include <cstdint>
#include <vector>
#include <string>
namespace gateway {
enum class EventType { LinkLost, LinkEstablished, OnBattery, PowerRestored, LowBattery, LowBatteryCleared, UpsFault, UpsFaultCleared, TestStarted, TestEnded, ShutdownPending, ShutdownCancelled, CommandQueued, CommandSent, CommandFailed, TestTrap, Startup, ThresholdRaised, ThresholdCleared, ScheduleResult, ClockChanged, SyslogTest };
// Keep the original aggregate order: UTC is appended and zero means unknown.
struct Event { uint32_t seq{0}; uint64_t uptime_ms{0}; EventType type{EventType::Startup}; uint8_t severity{1}; std::string message; uint64_t utc{0}; };
const char* eventName(EventType);
const char* eventMessage(EventType);
uint32_t powerNetTrapNumber(EventType);
class EventEngine {
 public:
  std::vector<Event> observe(const ups::Snapshot& state,uint64_t now);
 private:
  bool initialized_{false}, link_{false}, baseline_{false};
  bool battery_{false}, low_{false}, fault_{false}, testing_{false}, shutting_{false};
};
}

