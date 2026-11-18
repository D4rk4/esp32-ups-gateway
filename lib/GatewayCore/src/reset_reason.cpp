#include "reset_reason.hpp"
#include "events.hpp"
namespace gateway {
namespace {
struct Reason { const char* text; uint8_t severity; };
// Index = esp_reset_reason_t. Severity 2 (warning) marks resets that indicate a fault.
constexpr Reason kReasons[] = {
  {"unknown", 1},            // ESP_RST_UNKNOWN
  {"power-on", 1},           // ESP_RST_POWERON
  {"external reset", 1},     // ESP_RST_EXT
  {"software restart", 1},   // ESP_RST_SW
  {"panic", 2},              // ESP_RST_PANIC
  {"interrupt watchdog", 2}, // ESP_RST_INT_WDT
  {"task watchdog", 2},      // ESP_RST_TASK_WDT
  {"other watchdog", 2},     // ESP_RST_WDT
  {"deep sleep", 1},         // ESP_RST_DEEPSLEEP
  {"brownout", 2},           // ESP_RST_BROWNOUT
  {"SDIO reset", 1},         // ESP_RST_SDIO
};
constexpr int kReasonCount = int(sizeof(kReasons) / sizeof(kReasons[0]));
}
StartupReport startupReport(int resetCode) {
  const Reason& reason = kReasons[resetCode >= 0 && resetCode < kReasonCount ? resetCode : 0];
  return {reason.severity, std::string(eventMessage(EventType::Startup)) + "; reset reason: " + reason.text};
}
}
