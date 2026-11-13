#pragma once
#include "events.hpp"
#include "persistent_log.hpp"
#include "persistent_log_store.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
namespace gateway {
uint64_t uptimeMs();
struct PersistenceStatus {
  bool ok{false};std::string error;size_t stored{0},capacity{0},pending{0};uint32_t dropped{0};
};
class EventBus {
 public:
  bool begin();
  bool begin(PersistentLogStorage& storage);
  void publish(EventType type,uint8_t severity=1,const std::string& message="",uint64_t time_ms=0);
  std::vector<Event> latest() const;
  std::vector<Event> history(uint32_t before_seq=0,size_t limit=48);
  PersistenceStatus persistenceStatus() const;
  bool clearLog(std::string& error);
  bool flushPending(std::string& error);
  uint32_t latestSequence() const;
  void tick();
  static constexpr size_t capacity() {return 48;}
  bool popTrap(Event& event);
  uint32_t dropped() const { return dropped_.load(); }
  uint32_t queued() const;
  void countDropped(){++dropped_;}
 private:
  struct Queued {uint32_t seq;uint64_t time_ms;EventType type;uint8_t severity;uint8_t text_length;char text[181];uint64_t utc,queued_ms;};
  static Queued pack(const Event& event,uint64_t queued_ms=0);
  static Event unpack(const Queued& queued);
  bool start(PersistentLogStorage* storage,const std::string& error);
  mutable SemaphoreHandle_t mutex_{nullptr},storage_mutex_{nullptr};
  QueueHandle_t queue_{nullptr};
  std::array<Event,48> log_{};
  std::array<Queued,48> pending_{};
  size_t head_{0},count_{0},pending_head_{0},pending_count_{0};uint32_t sequence_{0},persistence_dropped_{0};
  uint64_t pending_since_{0};
  PersistentLogStore storage_;
  std::unique_ptr<PersistentLog> persistent_;
  std::string persistence_error_;
  std::atomic<uint32_t> dropped_{0};
};
}
