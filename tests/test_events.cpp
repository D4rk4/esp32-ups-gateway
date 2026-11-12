#include "events.hpp"
#include <cassert>
#include <iostream>
using namespace gateway;
bool has(const std::vector<Event>& es,EventType t){for(const auto&e:es)if(e.type==t)return true;return false;}
int main(){
  EventEngine e; ups::Snapshot s; s.connected=true;s.status_valid=true;
  auto events=e.observe(s,1000);assert(has(events,EventType::LinkEstablished));assert(!has(events,EventType::PowerRestored));
  assert(e.observe(s,2000).empty());
  s.mains_failed=true;events=e.observe(s,3000);assert(has(events,EventType::OnBattery));assert(e.observe(s,4000).empty());
  s.low_battery=true;assert(has(e.observe(s,5000),EventType::LowBattery));
  s.connected=false;assert(has(e.observe(s,6000),EventType::LinkLost));assert(e.observe(s,7000).empty());
  s.connected=true;s.mains_failed=false;s.low_battery=false;events=e.observe(s,8000);
  assert(has(events,EventType::LinkEstablished));assert(!has(events,EventType::PowerRestored));assert(!has(events,EventType::LowBatteryCleared));
  s.test_running=true;assert(has(e.observe(s,9000),EventType::TestStarted));
  s.test_running=false;events=e.observe(s,10000);assert(has(events,EventType::TestEnded));assert(powerNetTrapNumber(EventType::TestEnded)==0);
  s.mains_failed=true;e.observe(s,11000);s.mains_failed=false;assert(has(e.observe(s,12000),EventType::PowerRestored));
  assert(powerNetTrapNumber(EventType::UpsFault)==0);assert(powerNetTrapNumber(EventType::OnBattery)==5);
  EventEngine initial; s.connected=false;assert(has(initial.observe(s,10000),EventType::LinkLost));
  std::cout<<"events: all assertions passed\n";
}
