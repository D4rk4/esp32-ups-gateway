#pragma once
#include "config_model.hpp"
#include "events.hpp"
#include "ups_driver.hpp"
#include <ArduinoJson.h>
#include <array>
#include <functional>

namespace gateway {
constexpr uint64_t kFirstValidUtc=1577836800ULL;
constexpr uint64_t kLastValidUtc=4102444799ULL;
bool freshSnapshot(const ups::Snapshot&,uint64_t uptime_ms,uint32_t stale_ms);
// Writes "last_transfer": null, or {"uptime_s","utc"} (utc null when the event's clock was unset).
void writeLastTransfer(JsonObject ups,const Event* newest_on_battery);
bool parseScheduledCommand(const std::string&,ups::Command&);
bool scheduledCommandReady(const Config&,const ScheduleRule&,const ups::UpsDriver*,
                           const ups::Snapshot&,uint64_t uptime_ms,std::string& reason);
struct HistorySample {
  uint32_t seq{0}; uint64_t uptime_s{0},utc{0}; bool connected{false};
  std::array<float,8> values{}; uint16_t valid_mask{0};
};
class DataHistory {
 public:
  static constexpr size_t capacity=240;
  void sample(const Config&,const ups::Snapshot&,uint64_t uptime_ms,uint64_t utc);
  void clear();
  std::vector<HistorySample> latest(size_t limit=capacity) const;
  std::string json(uint32_t interval_s,size_t limit=capacity) const;
  std::string csv() const;
 private:
  std::array<HistorySample,capacity> samples_{};
  size_t head_{0},count_{0}; uint32_t sequence_{0};
  uint64_t last_sample_ms_{0}; bool sampled_{false};
};
struct Alarm { std::string id; uint8_t severity{2}; std::string message; uint64_t since_uptime_s{0}; };
class AlarmTracker {
 public:
  std::vector<Event> update(const Config&,const ups::Snapshot&,uint64_t uptime_ms);
  const std::vector<Alarm>& active() const { return active_; }
  std::string json() const;
 private:
  struct Threshold {bool pending{false},active{false};uint64_t since_ms{0};};
  std::array<Threshold,4> thresholds_{};
  std::vector<Alarm> active_;
};
std::vector<Event> filterEvents(const std::vector<Event>&,uint8_t min_severity,const std::string& contains,size_t limit);
std::string rfc3164Message(const Config&,const Event&,const std::string& timestamp);

struct LocalScheduleTime { bool valid{false};uint8_t weekday{0};uint16_t minute_of_day{0};uint8_t second{0};uint32_t day_key{0}; };
struct ScheduleRecord {
  uint32_t id{0};uint64_t last_claim{0};uint32_t last_local_day{0};
  std::string last_state,last_message;
};
struct ScheduleJournal {uint64_t guarded_until{0};std::vector<ScheduleRecord> records;};
std::string scheduleJournalJson(const ScheduleJournal&);
bool parseScheduleJournal(const std::string&,ScheduleJournal&,std::string& error);
struct ScheduleRuntime {
  bool armed{false};std::string reason;uint64_t last_claim{0};std::string last_state,last_message;
  uint32_t command_id{0};
};
class ScheduleEngine {
 public:
  using Save=std::function<bool(const ScheduleJournal&,std::string&)>;
  using Enqueue=std::function<bool(ups::Command,const ups::CommandArgs&,uint32_t&,std::string&)>;
  explicit ScheduleEngine(const Config& config):config_(config){}
  void begin(const ScheduleJournal& journal,bool writable);
  void tick(uint64_t utc,uint64_t uptime_ms,const LocalScheduleTime&,const ups::UpsDriver*,
            const ups::Snapshot&,const Save&,const Enqueue&);
  void observeCommand(uint32_t id,const std::string& state,const std::string& message,const Save&);
  std::string json(bool clock_valid) const;
  const ScheduleJournal& journal() const {return journal_;}
  const std::vector<ScheduleRuntime>& runtime() const {return runtime_;}
 private:
  bool saveGuard(uint64_t until,const Save& save);
  void refresh(uint64_t utc,uint64_t uptime_ms,const LocalScheduleTime&,const ups::UpsDriver*,const ups::Snapshot&);
  const Config& config_;ScheduleJournal journal_;std::vector<ScheduleRuntime> runtime_;
  bool writable_{false},observed_{false};uint64_t previous_utc_{0},previous_uptime_ms_{0},blocked_until_{0},high_water_{0};
  std::string journal_error_;
};
}
