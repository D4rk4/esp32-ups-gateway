#include "events.hpp"
namespace gateway {
const char* eventName(EventType t) {
  switch(t){
    case EventType::LinkLost:return "communication_lost";case EventType::LinkEstablished:return "communication_established";
    case EventType::OnBattery:return "on_battery";case EventType::PowerRestored:return "power_restored";
    case EventType::LowBattery:return "low_battery";case EventType::LowBatteryCleared:return "low_battery_cleared";
    case EventType::UpsFault:return "ups_fault";case EventType::UpsFaultCleared:return "ups_fault_cleared";
    case EventType::TestStarted:return "test_started";case EventType::TestEnded:return "test_ended";
    case EventType::ShutdownPending:return "shutdown_pending";case EventType::ShutdownCancelled:return "shutdown_cancelled";
    case EventType::CommandQueued:return "command_queued";case EventType::CommandSent:return "command_sent";
    case EventType::CommandFailed:return "command_failed";case EventType::TestTrap:return "test_trap";
    case EventType::Startup:return "startup";
    case EventType::ThresholdRaised:return "threshold_raised";case EventType::ThresholdCleared:return "threshold_cleared";
    case EventType::ScheduleResult:return "schedule_result";case EventType::ClockChanged:return "clock_changed";
    case EventType::SyslogTest:return "syslog_test";
  }return "unknown";
}
const char* eventMessage(EventType t) {
  switch(t){
    case EventType::LinkLost:return "Communication with UPS lost";
    case EventType::LinkEstablished:return "Communication with UPS established";
    case EventType::OnBattery:return "UPS is on battery";
    case EventType::PowerRestored:return "Utility power restored";
    case EventType::LowBattery:return "Low battery";
    case EventType::LowBatteryCleared:return "Low battery condition cleared";
    case EventType::UpsFault:return "UPS reports a general fault";
    case EventType::UpsFaultCleared:return "General fault condition cleared";
    case EventType::TestStarted:return "UPS test in progress";
    case EventType::TestEnded:return "Test ended; the protocol does not report the result";
    case EventType::ShutdownPending:return "UPS reports a shutdown in progress";
    case EventType::ShutdownCancelled:return "Shutdown condition cleared";
    case EventType::CommandQueued:return "Command queued";
    case EventType::CommandSent:return "Command sent; execution not confirmed";
    case EventType::CommandFailed:return "Command failed";
    case EventType::TestTrap:return "SNMP trap channel test; UPS status unchanged";
    case EventType::Startup:return "Gateway started";
    case EventType::ThresholdRaised:return "Configured threshold exceeded";
    case EventType::ThresholdCleared:return "Configured threshold cleared";
    case EventType::ScheduleResult:return "Schedule result";
    case EventType::ClockChanged:return "Gateway time changed";
    case EventType::SyslogTest:return "UDP syslog test; delivery not confirmed";
  }return "";
}
uint32_t powerNetTrapNumber(EventType t) {
  switch(t){case EventType::LinkLost:return 1;case EventType::LinkEstablished:return 8;
    case EventType::OnBattery:return 5;case EventType::PowerRestored:return 9;
    case EventType::LowBattery:return 7;case EventType::LowBatteryCleared:return 11;
    default:return 0;}
}
std::vector<Event> EventEngine::observe(const ups::Snapshot& s,uint64_t now){
  std::vector<Event> out;
  auto add=[&](EventType t,uint8_t severity=1){out.push_back({0,now,t,severity,eventMessage(t)});};
  if(!initialized_||link_!=s.connected){add(s.connected?EventType::LinkEstablished:EventType::LinkLost,s.connected?1:3);baseline_=false;}
  initialized_=true;link_=s.connected;
  if(!s.connected||!s.status_valid){baseline_=false;return out;}
  if(!baseline_){
    if(s.mains_failed)add(EventType::OnBattery,2);
    if(s.low_battery)add(EventType::LowBattery,3);
    if(s.ups_fault)add(EventType::UpsFault,3);
    if(s.test_running)add(EventType::TestStarted);
    if(s.shutdown_active)add(EventType::ShutdownPending,2);
  }else{
    if(s.mains_failed!=battery_)add(s.mains_failed?EventType::OnBattery:EventType::PowerRestored,s.mains_failed?2:1);
    if(s.low_battery!=low_)add(s.low_battery?EventType::LowBattery:EventType::LowBatteryCleared,s.low_battery?3:1);
    if(s.ups_fault!=fault_)add(s.ups_fault?EventType::UpsFault:EventType::UpsFaultCleared,s.ups_fault?3:1);
    if(s.test_running!=testing_)add(s.test_running?EventType::TestStarted:EventType::TestEnded);
    if(s.shutdown_active!=shutting_)add(s.shutdown_active?EventType::ShutdownPending:EventType::ShutdownCancelled,s.shutdown_active?2:1);
  }
  baseline_=true;battery_=s.mains_failed;low_=s.low_battery;fault_=s.ups_fault;testing_=s.test_running;shutting_=s.shutdown_active;
  return out;
}
}

