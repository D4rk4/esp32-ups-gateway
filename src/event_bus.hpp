#pragma once
#include "events.hpp"
#include <array>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
namespace gateway {
uint64_t uptimeMs();
class EventBus {
 public:
  bool begin();
  void publish(EventType type,uint8_t severity=1,const std::string& message="",uint64_t time_ms=0);
  std::vector<Event> latest() const;
  void clearLog();
  static constexpr size_t capacity() {return 48;}
  bool popTrap(Event& event);
  uint32_t dropped() const { return dropped_.load(); }
  uint32_t queued() const;
  void countDropped(){++dropped_;}
 private:
  struct Queued { uint32_t seq;uint64_t time_ms;EventType type;uint8_t severity;char text[192];uint64_t utc; };
  mutable SemaphoreHandle_t mutex_{nullptr};
  QueueHandle_t queue_{nullptr};
  std::array<Event,48> log_{};
  size_t head_{0},count_{0};uint32_t sequence_{0};
  std::atomic<uint32_t> dropped_{0};
};
}
