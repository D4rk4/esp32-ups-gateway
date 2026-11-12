#include "event_bus.hpp"
#include <cassert>
#include <iostream>
using namespace gateway;
int main(){
  EventBus bus;assert(bus.begin());
  bus.publish(EventType::LinkLost,3,"UPS link unavailable");
  bus.publish(EventType::ThresholdRaised,2,"Configured threshold exceeded");
  assert(bus.latest().size()==2&&bus.queued()==1);
  const auto sequence=bus.latest()[0].seq;
  bus.clearLog();assert(bus.latest().empty()&&bus.queued()==1);
  Event pending;assert(bus.popTrap(pending)&&pending.type==EventType::LinkLost&&pending.severity==3);
  assert(pending.message=="UPS link unavailable"&&!bus.popTrap(pending));
  for(size_t i=0;i<60;++i)bus.publish(EventType::ThresholdRaised,2,"threshold");
  assert(bus.latest().size()==48&&bus.latest()[0].seq>sequence&&bus.queued()==0);
  std::cout<<"event_bus platform boundary: clear preserves pending traps, thresholds have no trap, bounded journal passed\n";
}
