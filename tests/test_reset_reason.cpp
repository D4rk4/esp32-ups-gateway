#include "reset_reason.hpp"
#include "events.hpp"
#include <cassert>
#include <iostream>
using namespace gateway;
namespace {
void expect(int code,const char* reason,uint8_t severity){
  const StartupReport report=startupReport(code);
  assert(report.message==std::string(eventMessage(EventType::Startup))+"; reset reason: "+reason);
  assert(report.severity==severity);
  assert(report.message.size()<=180);
}
}
int main(){
  // Codes mirror esp_reset_reason_t: 0 unknown, 1 power-on, 2 external pin, 3 software, 4 panic,
  // 5 interrupt WDT, 6 task WDT, 7 other WDT, 8 deep sleep, 9 brownout, 10 SDIO.
  expect(1,"power-on",1);
  expect(2,"external reset",1);
  expect(3,"software restart",1);
  expect(8,"deep sleep",1);
  expect(10,"SDIO reset",1);
  // Warning severity (2) is reserved for resets that indicate a fault.
  expect(4,"panic",2);
  expect(5,"interrupt watchdog",2);
  expect(6,"task watchdog",2);
  expect(7,"other watchdog",2);
  expect(9,"brownout",2);
  // Unknown or out-of-range codes never claim a fault and never index past the table.
  expect(0,"unknown",1);expect(11,"unknown",1);expect(-1,"unknown",1);expect(1000000,"unknown",1);
  std::cout<<"reset reason: all assertions passed\n";
}
