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
  // upsBasicOutputStatus is derived from the mode only; a self-test is its own value, and the
  // raw test_running flag no longer overrides the mode (test plus utility failure is on battery).
  struct ModeCase{ups::OutputMode mode;bool test_running;int expected;};
  for(const auto& row:{ModeCase{ups::OutputMode::BatteryTest,true,15},ModeCase{ups::OutputMode::OnBattery,true,3},
      ModeCase{ups::OutputMode::OnBattery,false,3},ModeCase{ups::OutputMode::Online,false,2},
      ModeCase{ups::OutputMode::Bypass,false,9},ModeCase{ups::OutputMode::Off,false,7},ModeCase{ups::OutputMode::Unknown,false,1}}){
    ups::Snapshot probe=s;probe.output_mode=row.mode;probe.test_running=row.test_running;gateway::PowerNetMib mode_mib(c,probe,0,"v");
    assert(mode_mib.get(oid("1.3.6.1.4.1.318.1.1.1.4.1.1.0"),v)&&v.type==snmp::Type::Integer&&v.integer==row.expected);
  }
  snmp::Oid current,next;size_t count=0;while(m.next(current,next,v)){assert(current<next);current=next;assert(++count<100);}assert(count>15);
  s.connected=false;gateway::PowerNetMib stale(c,s,100,"0.1.0");
  assert(stale.get(oid("1.3.6.1.4.1.318.1.1.1.8.1.0"),v)&&v.integer==2);
  assert(stale.get(oid("1.3.6.1.4.1.318.1.1.1.4.1.1.0"),v)&&v.integer==1);
  assert(!stale.get(oid("1.3.6.1.4.1.318.1.1.1.3.3.1.0"),v));
  const std::string root="1.3.6.1.4.1.318.1.1.1.";
  const auto control=[&](const char* suffix,int value){snmp::Value x;x.type=snmp::Type::Integer;x.integer=value;snmp::Oid name;assert(snmp::parseOid(root+suffix,name));return snmp::VarBind{name,x};};
  struct Mapping {const char* suffix;int value;ups::Command command;uint16_t minutes,restore,seconds;};
  const Mapping mappings[]={
    {"6.2.1.0",2,ups::Command::OutputOff,1,3,12},{"6.2.1.0",3,ups::Command::OutputOff,1,3,0},
    {"6.1.1.0",2,ups::Command::OutputRestart,1,3,0},
    {"6.2.2.0",2,ups::Command::OutputRestart,1,3,12},{"6.2.2.0",3,ups::Command::OutputRestart,1,3,0},
    {"6.2.5.0",2,ups::Command::BeeperToggle,1,3,0},{"6.2.6.0",2,ups::Command::ShutdownCancel,1,3,0},
    {"7.2.2.0",2,ups::Command::TestShort,1,3,0},{"7.2.5.0",2,ups::Command::TestDeep,1,3,0},{"7.2.5.0",3,ups::Command::TestCancel,1,3,0}};
  s.connected=true;s.status_valid=true;s.mains_failed=true;
  for(const auto& mapping:mappings){gateway::PowerNetControlAction action;size_t index=99;assert(gateway::mapPowerNetSet({control(mapping.suffix,mapping.value)},s,true,action,index)==0&&index==0&&action.pending);assert(action.command==mapping.command&&action.args.delay_minutes==mapping.minutes&&action.args.restore_minutes==mapping.restore&&action.args.delay_seconds==mapping.seconds);}
  for(const auto* suffix:{"6.2.1.0","6.1.1.0","6.2.2.0","6.2.5.0","6.2.6.0","7.2.2.0","7.2.5.0"}){gateway::PowerNetControlAction action;size_t index=0;assert(gateway::mapPowerNetSet({control(suffix,1)},s,true,action,index)==0&&!action.pending);assert(gateway::mapPowerNetSet({control(suffix,9)},s,true,action,index)==10&&index==1);}
  gateway::PowerNetControlAction action;size_t index=0;s.mains_failed=false;
  assert(gateway::mapPowerNetSet({control("6.1.1.0",2)},s,true,action,index)==10&&index==1&&!action.pending);
  s.mains_failed=true;assert(gateway::mapPowerNetSet({control("6.1.1.0",2)},s,false,action,index)==10&&index==1&&!action.pending);
  auto nonInteger=control("6.2.1.0",2);nonInteger.value.type=snmp::Type::OctetString;
  assert(gateway::mapPowerNetSet({nonInteger},s,true,action,index)==7&&index==1);
  assert(gateway::mapPowerNetSet({control("5.2.9.0",2)},s,true,action,index)==17&&index==1);
  assert(gateway::mapPowerNetSet({control("99.1.0",2)},s,true,action,index)==17&&index==1);
  assert(gateway::mapPowerNetSet({control("6.2.1.0",2),control("7.2.2.0",2)},s,true,action,index)==12&&index==2&&!action.pending);
  c.snmp_write_community="write";c.controls_enabled=true;c.power_controls_enabled=true;
  gateway::PowerNetMib all(c,s,0,"v");for(const auto* suffix:{"6.2.1.0","6.1.1.0","6.2.2.0","6.2.5.0","6.2.6.0","7.2.2.0","7.2.5.0"})assert(all.get(control(suffix,1).oid,v)&&v.type==snmp::Type::Integer&&v.integer==1);
  assert(all.next(control("6.2.0.0",1).oid,next,v)&&next==control("6.2.1.0",1).oid&&v.integer==1);
  assert(all.next(control("6.2.1.0",1).oid,next,v)&&next==control("6.2.2.0",1).oid&&v.integer==1);
  assert(all.get(control("5.2.10.0",1).oid,v)&&v.type==snmp::Type::TimeTicks&&v.unsigned_value==6000);
  assert(all.get(control("5.2.9.0",1).oid,v)&&v.type==snmp::Type::TimeTicks&&v.unsigned_value==18000);
  c.power_controls_enabled=false;gateway::PowerNetMib noPower(c,s,0,"v");assert(!noPower.get(control("6.2.1.0",1).oid,v));assert(noPower.get(control("7.2.2.0",1).oid,v));assert(!noPower.get(control("5.2.9.0",1).oid,v));
  assert(noPower.next(control("6.2.0.0",1).oid,next,v)&&next==control("6.2.5.0",1).oid);
  // Partial driver capabilities must expose only the supported controls and their delays.
  c.power_controls_enabled=true;
  gateway::PowerNetMib offOnly(c,s,0,"v",ups::CapOutputOff);
  assert(offOnly.get(control("6.2.1.0",1).oid,v));assert(!offOnly.get(control("6.2.2.0",1).oid,v));
  assert(offOnly.get(control("5.2.10.0",1).oid,v));assert(!offOnly.get(control("5.2.9.0",1).oid,v));
  gateway::PowerNetMib restartOnly(c,s,0,"v",ups::CapOutputRestart);
  assert(!restartOnly.get(control("6.2.1.0",1).oid,v));assert(restartOnly.get(control("6.2.2.0",1).oid,v));
  assert(!restartOnly.get(control("5.2.10.0",1).oid,v));assert(restartOnly.get(control("5.2.9.0",1).oid,v));
  gateway::PowerNetMib shortOnly(c,s,0,"v",ups::CapTest);
  assert(shortOnly.get(control("7.2.2.0",1).oid,v));assert(!shortOnly.get(control("7.2.5.0",1).oid,v));
  gateway::PowerNetMib deepOnly(c,s,0,"v",ups::CapTestDeep);
  assert(!deepOnly.get(control("7.2.5.0",1).oid,v));
  gateway::PowerNetMib deepAndCancel(c,s,0,"v",ups::CapTestDeep|ups::CapTestCancel);
  assert(deepAndCancel.get(control("7.2.5.0",1).oid,v));
  c.controls_enabled=false;gateway::PowerNetMib noControls(c,s,0,"v");assert(!noControls.get(control("7.2.2.0",1).oid,v));
  c.controls_enabled=true;c.power_controls_enabled=true;c.snmp_write_community.clear();gateway::PowerNetMib noWrite(c,s,0,"v");assert(!noWrite.get(control("6.2.1.0",1).oid,v));
  c.snmp_write_community="write";c.driver="missing";gateway::PowerNetMib noDriver(c,s,0,"v");assert(!noDriver.get(control("6.2.1.0",1).oid,v)&&!noDriver.get(control("7.2.2.0",1).oid,v));
  std::cout<<"PowerNet mapping: all assertions passed\n";
}
