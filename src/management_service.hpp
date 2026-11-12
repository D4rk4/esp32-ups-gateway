#pragma once
#include "config_model.hpp"
#include "event_bus.hpp"
#include "management_core.hpp"
#include "ups_service.hpp"
#include <atomic>
#include <freertos/task.h>

namespace gateway {
// All configuration is the immutable active boot copy. HTTP calls only read
// bounded snapshots or queue work; UDP/DNS belongs to a separate worker.
class ManagementService {
 public:
  ManagementService(const Config&,UpsService&,EventBus&);
  bool begin();
  void tick();
  std::string alarmsJson() const;
  std::string historyJson(size_t limit=240) const;
  std::string historyCsv() const;
  std::string timeJson() const;
  std::string schedulesJson() const;
  std::string notificationsJson() const;
  bool setTime(uint64_t utc_seconds,std::string& error);
  void clearHistory();
  bool testSyslog(std::string& error);
 private:
  struct SyslogPacket {uint16_t length;char text[481];};
  bool readJournal(ScheduleJournal&,std::string& error);
  bool saveJournal(const ScheduleJournal&,std::string& error);
  bool queueSyslog(const Event&);
  static void syslogEntry(void*);
  void syslogRun();
  void syslogState(const char* state,const std::string& message);
  const Config& config_;UpsService& ups_;EventBus& events_;
  mutable SemaphoreHandle_t mutex_{nullptr};
  DataHistory history_;AlarmTracker alarms_;ScheduleEngine scheduler_;
  uint64_t last_tick_ms_{0};bool ticked_{false};
  uint32_t last_syslog_seq_{0};
  QueueHandle_t syslog_queue_{nullptr};TaskHandle_t syslog_task_{nullptr};
  std::atomic<uint32_t> syslog_sent_{0},syslog_failed_{0},syslog_dropped_{0};
  std::string syslog_last_state_{"disabled"},syslog_last_message_;
};
}
