#pragma once
#include <cstdint>
#include <string>

namespace ups {
struct Metric { float value{0}; bool valid{false}; };
enum class OutputMode { Unknown, Online, OnBattery, Bypass, Off, BatteryTest };
struct Snapshot {
  bool connected{false};
  uint32_t last_success_ms{0};
  uint32_t successful_polls{0};
  uint32_t errors{0};
  std::string manufacturer, model, firmware, serial;
  Metric input_voltage, input_fault_voltage, output_voltage, load_percent;
  Metric input_frequency, output_frequency, battery_voltage_raw;
  Metric battery_voltage, battery_charge, runtime_seconds;
  Metric ups_temperature, battery_temperature;
  Metric nominal_voltage, nominal_current, nominal_battery_voltage, nominal_frequency;
  bool status_valid{false};
  OutputMode output_mode{OutputMode::Unknown};
  bool mains_failed{false}, low_battery{false}, bypass_or_avr{false};
  bool ups_fault{false}, standby_topology{false}, test_running{false};
  bool shutdown_active{false}, beeper_on{false};
  std::string raw_status, last_status_line;
};
enum class Command { BeeperToggle, TestShort, TestCancel, ShutdownCancel, OutputOff, OutputRestart, TestDeep, TestTimed };
struct CommandArgs { uint16_t delay_minutes{1}; uint16_t restore_minutes{3}; uint16_t delay_seconds{0}; };
enum Capability : uint32_t { CapBeeper=1u, CapTest=2u, CapTestCancel=4u, CapShutdownCancel=8u, CapOutputOff=16u, CapOutputRestart=32u, CapTestDeep=64u, CapTestTimed=128u };
struct CommandInfo { Command command; const char* name; const char* label; uint32_t capability; bool power; };
inline constexpr CommandInfo kCommands[] = {
  {Command::BeeperToggle,"beeper_toggle","Toggle beeper",CapBeeper,false},
  {Command::TestShort,"test_short","Short battery test",CapTest,false},
  {Command::TestCancel,"test_cancel","Cancel battery test",CapTestCancel,false},
  {Command::ShutdownCancel,"shutdown_cancel","Cancel shutdown",CapShutdownCancel,true},
  {Command::OutputOff,"output_off","Turn output off",CapOutputOff,true},
  {Command::OutputRestart,"output_restart","Restart output",CapOutputRestart,true},
  {Command::TestDeep,"test_deep","Deep battery test",CapTestDeep,false},
  {Command::TestTimed,"test_timed","Timed battery test",CapTestTimed,false}
};
inline const CommandInfo* findCommand(const std::string& name) {
  for (const auto& info : kCommands) if (name == info.name) return &info;
  return nullptr;
}
inline const CommandInfo* findCommand(Command command) {
  for (const auto& info : kCommands) if (command == info.command) return &info;
  return nullptr;
}
enum class Parity { None, Even, Odd };
struct DriverInfo {
  const char* id; const char* label; uint32_t default_baud; uint32_t capabilities;
  Parity parity{Parity::None};
  uint8_t stop_bits{1};
};
}

