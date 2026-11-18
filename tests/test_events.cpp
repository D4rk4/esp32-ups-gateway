#include "events.hpp"
#include <cassert>
#include <iostream>
using namespace gateway;
bool has(const std::vector<Event>& es,EventType t){for(const auto&e:es)if(e.type==t)return true;return false;}
int main(){
  for(int i=static_cast<int>(EventType::LinkLost);i<=static_cast<int>(EventType::ExtendedQuery);++i){const std::string message=eventMessage(static_cast<EventType>(i));assert(!message.empty()&&message.size()<=180);for(const unsigned char ch:message)assert(ch<128);}
  assert(std::string(eventMessage(EventType::OnBattery))=="UPS is on battery");
  assert(std::string(eventMessage(EventType::LinkLost))=="Communication with UPS lost");
  EventEngine e; ups::Snapshot s; s.connected=true;s.status_valid=true;
  auto events=e.observe(s,1000);assert(has(events,EventType::LinkEstablished));assert(!has(events,EventType::PowerRestored));
  assert(e.observe(s,2000).empty());
  s.mains_failed=true;events=e.observe(s,3000);assert(has(events,EventType::OnBattery));assert(e.observe(s,4000).empty());
  s.low_battery=true;assert(has(e.observe(s,5000),EventType::LowBattery));
  s.connected=false;assert(has(e.observe(s,6000),EventType::LinkLost));assert(e.observe(s,7000).empty());
  s.connected=true;s.mains_failed=false;s.low_battery=false;events=e.observe(s,8000);
  assert(has(events,EventType::LinkEstablished));assert(!has(events,EventType::PowerRestored));assert(!has(events,EventType::LowBatteryCleared));
  // A self-test with utility present is not a transfer: no on-battery event, no PowerNet trap.
  s.test_running=true;events=e.observe(s,9000);assert(has(events,EventType::TestStarted));
  assert(!has(events,EventType::OnBattery)&&!has(events,EventType::PowerRestored)&&powerNetTrapNumber(EventType::TestStarted)==0);
  s.test_running=false;events=e.observe(s,10000);assert(has(events,EventType::TestEnded));assert(powerNetTrapNumber(EventType::TestEnded)==0);
  assert(!has(events,EventType::OnBattery)&&!has(events,EventType::PowerRestored));
  // A real outage during a test is still reported as an outage.
  s.test_running=true;e.observe(s,10500);s.mains_failed=true;assert(has(e.observe(s,10600),EventType::OnBattery));
  s.mains_failed=false;s.test_running=false;e.observe(s,10700);
  s.mains_failed=true;e.observe(s,11000);s.mains_failed=false;assert(has(e.observe(s,12000),EventType::PowerRestored));
  assert(powerNetTrapNumber(EventType::UpsFault)==0);assert(powerNetTrapNumber(EventType::OnBattery)==5);
  EventEngine initial; s.connected=false;assert(has(initial.observe(s,10000),EventType::LinkLost));
  std::cout<<"events: all assertions passed\n";
}
