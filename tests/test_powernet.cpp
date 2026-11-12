#include "powernet_mib.hpp"
#include <cassert>
#include <iostream>
snmp::Oid oid(const char*s){snmp::Oid o;assert(snmp::parseOid(s,o));return o;}
int main(){
  gateway::Config c;ups::Snapshot s;s.connected=true;s.status_valid=true;s.output_mode=ups::OutputMode::Online;
  s.input_voltage={230.4f,true};s.input_frequency={49.9f,true};s.output_voltage={229.8f,true};s.load_percent={32.5f,true};s.ups_temperature={35.0f,true};
  gateway::PowerNetMib m(c,s,0xffffffffu,"0.1.0");snmp::Value v;
  assert(m.get(oid("1.3.6.1.2.1.1.3.0"),v)&&v.type==snmp::Type::TimeTicks&&v.unsigned_value==0xffffffffu);
  assert(m.get(oid("1.3.6.1.4.1.318.1.1.1.3.3.1.0"),v)&&v.type==snmp::Type::Gauge32&&v.unsigned_value==2304);
  assert(m.get(oid("1.3.6.1.4.1.318.1.1.1.4.3.3.0"),v)&&v.unsigned_value==325);
  assert(m.get(oid("1.3.6.1.4.1.318.1.1.1.2.2.2.0"),v)&&v.unsigned_value==35);
  assert(!m.get(oid("1.3.6.1.4.1.318.1.1.1.2.2.1.0"),v)); // no fabricated capacity
  assert(!m.get(oid("1.3.6.1.4.1.318.1.1.1.2.2.3.0"),v)); // no fabricated runtime
  assert(!m.get(oid("1.3.6.1.4.1.318.1.1.1.4.2.2.0"),v)); // input frequency isn't output frequency
  assert(m.get(oid("1.3.6.1.4.1.318.1.1.1.8.1.0"),v)&&v.integer==1);
  snmp::Oid current,next;size_t count=0;while(m.next(current,next,v)){assert(current<next);current=next;assert(++count<100);}assert(count>15);
  s.connected=false;gateway::PowerNetMib stale(c,s,100,"0.1.0");
  assert(stale.get(oid("1.3.6.1.4.1.318.1.1.1.8.1.0"),v)&&v.integer==2);
  assert(stale.get(oid("1.3.6.1.4.1.318.1.1.1.4.1.1.0"),v)&&v.integer==1);
  assert(!stale.get(oid("1.3.6.1.4.1.318.1.1.1.3.3.1.0"),v));
  std::cout<<"PowerNet mapping: all assertions passed\n";
}
