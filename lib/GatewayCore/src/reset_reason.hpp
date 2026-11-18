#pragma once
#include <cstdint>
#include <string>
namespace gateway {
// Text and severity for the boot event. `resetCode` is esp_reset_reason_t; the numeric values
// are asserted against the SDK enum in src/main.cpp so this stays platform independent.
struct StartupReport { uint8_t severity; std::string message; };
StartupReport startupReport(int resetCode);
}
