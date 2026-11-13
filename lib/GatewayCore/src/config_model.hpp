#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace gateway {
struct TrapTarget { bool enabled{false}; std::string ip; uint16_t port{162}; std::string community; };
struct QuickLink { std::string title, url; };
struct ScheduleRule {
  uint32_t id{1}; bool enabled{false}; std::string name;
  std::string command{"test_short"};
  uint32_t delay_minutes{1}, restore_minutes{3};
  std::string mode{"once"}; uint64_t at_utc{0};
  uint32_t days_mask{0}, minute_of_day{0};
};
struct Config {
  uint32_t schema{2};
  std::string hostname{"ups-gateway"}, location, contact;
  std::string driver{"megatec_q1"};
  uint32_t baud{2400}, poll_ms{2000}, response_timeout_ms{1200}, stale_ms{10000};
  float battery_multiplier{0};
  bool controls_enabled{false}, power_controls_enabled{false};
  bool dhcp{true};
  std::string ip{"192.168.1.50"}, netmask{"255.255.255.0"}, gateway{"192.168.1.1"}, dns{"192.168.1.1"};
  bool snmp_enabled{true};
  std::string snmp_community, snmp_write_community, snmp_manager_ip;
  std::string web_user{"ups"}, web_password{"ups"};
  std::array<TrapTarget,2> traps;
  bool operator_enabled{false}, readonly_enabled{false};
  std::string operator_user{"operator"}, operator_password;
  std::string readonly_user{"viewer"}, readonly_password;
  uint32_t session_timeout_min{15};
  bool ntp_enabled{true};
  std::string ntp_server{"pool.ntp.org"}, timezone{"UTC0"};
  std::string date_format{"YYYY-MM-DD"}, temperature_unit{"C"};
  uint32_t history_interval_s{60};
  std::string default_page{"home"};
  std::array<QuickLink,3> quick_links;
  uint32_t alarm_load_high{90}, alarm_temperature_high{50};
  uint32_t alarm_input_low{180}, alarm_input_high{260}, alarm_delay_s{5};
  bool syslog_enabled{false};
  std::string syslog_host;
  uint32_t syslog_port{514}, syslog_min_severity{1};
  std::vector<ScheduleRule> schedules;
};
bool parseIPv4(const std::string& text, uint32_t& value);
bool validateConfig(const Config& config, std::string& error);
uint64_t supportedClockMaxUtc();
Config defaultsPreservingAccess(const Config& current, bool keep_network);
}

