#include "driver_registry.hpp"
#include "megatec_q1.hpp"
#include "ups_extended_probe.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <tuple>
#include <vector>

using namespace ups;

// Full field-wise comparison makes atomicity assertions independent of struct padding.
static std::string fingerprint(const Snapshot& s) {
  std::ostringstream o;
  o << s.connected << ',' << s.last_success_ms << ',' << s.successful_polls << ',' << s.errors
    << s.manufacturer << '|' << s.model << '|' << s.firmware << '|' << s.serial;
  for (const Metric* m : {&s.input_voltage, &s.input_fault_voltage, &s.output_voltage,
       &s.load_percent, &s.input_frequency, &s.output_frequency, &s.battery_voltage_raw,
       &s.battery_voltage, &s.battery_charge, &s.runtime_seconds, &s.ups_temperature,
       &s.battery_temperature, &s.nominal_voltage, &s.nominal_current,
       &s.nominal_battery_voltage, &s.nominal_frequency}) o << '|' << m->value << ':' << m->valid;
  o << '|' << s.status_valid << ':' << static_cast<int>(s.output_mode)
    << s.mains_failed << s.low_battery << s.bypass_or_avr << s.ups_fault
    << s.standby_topology << s.test_running << s.shutdown_active << s.beeper_on
    << '|' << s.raw_status << '|' << s.last_status_line;
  return o.str();
}
static void metric(const Metric& m, float v) { assert(m.valid && std::fabs(m.value-v)<0.001f); }

int main() {
  MegaTecQ1Driver driver;
  assert(driver.maxReplyLength()==160);
  assert(driver.info().parity==Parity::None && driver.info().stop_bits==1);
  const DriverInfo oldAggregate{"future", "Future driver",9600,0};
  assert(oldAggregate.parity==Parity::None && oldAggregate.stop_bits==1);
  for (const auto kind : {PollKind::Status,PollKind::Rating,PollKind::Identity}) {
    assert(!driver.frameComplete(kind,"",0));
    assert(!driver.frameComplete(kind,"partial",1000));
    assert(!driver.frameComplete(kind,std::string("\0\x01\x02",3),1000));
    assert(driver.frameComplete(kind,"reply\r",0));
    assert(driver.frameComplete(kind,"reply\n",0));
    assert(driver.frameComplete(kind,"reply\r\n",0));
  }
  const std::string fixture="(208.4 140.0 208.4 034 59.9 2.05 35.0 00110000";
  Snapshot s;
  s.last_success_ms=123; s.successful_polls=7; s.errors=2; s.serial="retained";
  s.battery_charge={72,true}; s.runtime_seconds={300,true}; s.output_frequency={50,true};
  assert(driver.parseReply(PollKind::Status, fixture+"\r\n",s)==ParseResult::Accepted);
  metric(s.input_voltage,208.4f); metric(s.input_fault_voltage,140.0f);
  metric(s.output_voltage,208.4f); metric(s.load_percent,34); metric(s.input_frequency,59.9f);
  metric(s.battery_voltage_raw,2.05f); metric(s.ups_temperature,35);
  // 00110000: mains present, battery normal, bypass, fault, online topology, no test/shutdown/beep.
  assert(s.status_valid && !s.mains_failed && !s.low_battery && s.bypass_or_avr && s.ups_fault);
  assert(!s.standby_topology && !s.test_running && !s.shutdown_active && !s.beeper_on);
  assert(s.output_mode==OutputMode::Bypass && s.raw_status=="00110000" && s.last_status_line==fixture);
  assert(!s.connected && s.last_success_ms==123 && s.successful_polls==7 && s.errors==2);
  metric(s.battery_charge,72); metric(s.runtime_seconds,300); metric(s.output_frequency,50);
  assert(!s.battery_voltage.valid && !s.battery_temperature.valid && s.serial=="retained");

  for (const auto& entry : std::vector<std::pair<std::string,OutputMode>>{
      {"00000000",OutputMode::Online},{"10000000",OutputMode::OnBattery},
      {"00101000",OutputMode::Online},{"00000010",OutputMode::Online},
      {"11111111",OutputMode::OnBattery}}) {
    assert(driver.parseReply(PollKind::Status,"(230.0 230.0 230.0 050 50.0 24.0 30.0 "+entry.first,s)==ParseResult::Accepted);
    assert(s.output_mode==entry.second);
  }
  assert(s.mains_failed && s.low_battery && s.bypass_or_avr && s.ups_fault && s.standby_topology && s.test_running && s.shutdown_active && s.beeper_on);
  const std::vector<std::string> individualBits{
      "10000000","01000000","00100000","00010000","00001000","00000100","00000010","00000001"};
  for (std::size_t i=0; i<individualBits.size(); ++i) {
    assert(driver.parseReply(PollKind::Status,"(230 230 230 50 50 24 30 "+individualBits[i],s)==ParseResult::Accepted);
    const bool actual[]{s.mains_failed,s.low_battery,s.bypass_or_avr,s.ups_fault,s.standby_topology,s.test_running,s.shutdown_active,s.beeper_on};
    for (std::size_t j=0; j<8; ++j) assert(actual[j] == (i == j));
  }
  assert(driver.parseReply(PollKind::Status,"(@ @@@.@ @ @@@ @ @@@@ @ 00000000",s)==ParseResult::Accepted);
  assert(!s.input_voltage.valid && !s.input_fault_voltage.valid && !s.output_voltage.valid && !s.load_percent.valid);
  assert(!s.input_frequency.valid && !s.battery_voltage_raw.valid && !s.ups_temperature.valid && s.status_valid);

  const std::string prior=fingerprint(s);
  for (const std::string& line : std::vector<std::string>{"", "garbage",fixture+" extra",fixture.substr(0,fixture.size()-1),
      "(nan 140 208 34 59.9 2.05 35 00110000", "(inf 140 208 34 59.9 2.05 35 00110000",
      "(1e2 140 208 34 59.9 2.05 35 00110000", "(-1 140 208 34 59.9 2.05 35 00110000",
      "(1000 140 208 34 59.9 2.05 35 00110000", "(208 140 208 34 100 2.05 35 00110000",
      "(208 140 208 34 59.9 2.05 35 0011000x", "(208 140 208 34 59.9 2.05 35 @@@@@@@@",
      "(208 140 208 34 59.9 2.05 35 00110000\rtrailer",std::string(161,'x')}) {
    assert(driver.parseReply(PollKind::Status,line,s)==ParseResult::Invalid);
    assert(fingerprint(s)==prior);
  }
  for (const auto kind : {PollKind::Status,PollKind::Rating,PollKind::Identity}) {
    for (const auto* echo : {"Q1\r","F\r","I\r","NAK\r","(NAK\r","\r","\n","\r\n"}) {
      assert(driver.parseReply(kind,echo,s)==ParseResult::Ignored); assert(fingerprint(s)==prior);
    }
  }

  assert(driver.parseReply(PollKind::Rating,"#230.0 010 24.00 50.0\r",s)==ParseResult::Accepted);
  metric(s.nominal_voltage,230); metric(s.nominal_current,10); metric(s.nominal_battery_voltage,24); metric(s.nominal_frequency,50);
  assert(driver.parseReply(PollKind::Rating,"#@ @@@ @@@@ @",s)==ParseResult::Accepted);
  assert(!s.nominal_voltage.valid && !s.nominal_current.valid && !s.nominal_battery_voltage.valid && !s.nominal_frequency.valid);
  const std::string id="#ACME           "+std::string(" ")+"MODEL-1   "+" "+"FW1       ";
  assert(id.size()==38);
  assert(driver.parseReply(PollKind::Identity,id+"\r",s)==ParseResult::Accepted);
  assert(s.manufacturer=="ACME" && s.model=="MODEL-1" && s.firmware=="FW1" && s.serial=="retained");
  assert(driver.parseReply(PollKind::Identity,"#"+std::string(37,' '),s)==ParseResult::Accepted);
  assert(s.manufacturer.empty() && s.model.empty() && s.firmware.empty() && s.serial=="retained");
  const std::string shortId="!"+std::string("ACME      ")+" "+"MODEL-1   "+" "+"FW1       ";
  assert(shortId.size()==33);
  assert(driver.parseReply(PollKind::Identity,shortId+"\r",s)==ParseResult::Accepted);
  assert(s.manufacturer=="ACME" && s.model=="MODEL-1" && s.firmware=="FW1" && s.serial=="retained");
  const std::string actualId="!"+std::string(15,' ')+" "+std::string(10,' ')+" "+"V142B000D0";
  assert(actualId.size()==38 && driver.parseReply(PollKind::Identity,actualId,s)==ParseResult::Accepted);
  assert(s.manufacturer.empty() && s.model.empty() && s.firmware=="V142B000D0" && s.serial=="retained");
  const std::string serialId=id+" "+"SN-123"+std::string(15,' ');
  assert(serialId.size()==60 && driver.parseReply(PollKind::Identity,serialId+"\r",s)==ParseResult::Accepted);
  assert(s.manufacturer=="ACME" && s.model=="MODEL-1" && s.firmware=="FW1" && s.serial=="SN-123");
  auto bangSerialId=serialId;bangSerialId[0]='!';
  assert(driver.parseReply(PollKind::Identity,bangSerialId,s)==ParseResult::Accepted && s.serial=="SN-123");
  auto hashShortId=shortId;hashShortId[0]='#';
  assert(driver.parseReply(PollKind::Identity,hashShortId,s)==ParseResult::Accepted && s.serial=="SN-123");
  const auto prior2=fingerprint(s);
  for (const auto& identity : {shortId,id,serialId}) {
    for (const auto separator : (identity.size()==33 ? std::vector<size_t>{11,22} :
                               identity.size()==38 ? std::vector<size_t>{16,27} : std::vector<size_t>{16,27,38})) {
      auto bad=identity;bad[separator]='x';
      assert(driver.parseReply(PollKind::Identity,bad,s)==ParseResult::Invalid && fingerprint(s)==prior2);
    }
    auto bad=identity;bad[0]='?';
    assert(driver.parseReply(PollKind::Identity,bad,s)==ParseResult::Invalid && fingerprint(s)==prior2);
  }
  for (const auto& bad : std::vector<std::pair<PollKind,std::string>>{
      {PollKind::Rating,"#230 10 24 50 extra"},{PollKind::Rating,"#230 -1 24 50"},
      {PollKind::Rating,"#230 10 nan 50"},{PollKind::Identity,id.substr(0,37)},
      {PollKind::Identity,id+" "},{PollKind::Identity,"#"+std::string(15,'x')+"x"+std::string(21,' ')}}) {
    assert(driver.parseReply(bad.first,bad.second,s)==ParseResult::Invalid); assert(fingerprint(s)==prior2);
  }
  std::string bytes="stale";
  assert(driver.buildPoll(PollKind::Status,bytes) && bytes=="Q1\r");
  assert(driver.buildPoll(PollKind::Rating,bytes) && bytes=="F\r");
  assert(driver.buildPoll(PollKind::Identity,bytes) && bytes=="I\r");
  assert(!driver.buildPoll(static_cast<PollKind>(999),bytes));
  EncodedCommand out; std::string error;
  for (const auto& cmd : std::vector<std::tuple<Command,std::string,bool>>{
      {Command::BeeperToggle,"Q\r",false},{Command::TestShort,"T\r",false},
      {Command::TestCancel,"CT\r",false},{Command::ShutdownCancel,"C\r",true},
      {Command::OutputOff,"S01\r",true},{Command::OutputRestart,"S01R0003\r",true}}) {
    assert(driver.encodeCommand(std::get<0>(cmd),{},out,error));
    assert(out.bytes==std::get<1>(cmd) && out.changes_output==std::get<2>(cmd) && out.settle_ms==500 && error.empty());
  }
  assert((driver.info().capabilities & (CapTestDeep|CapTestTimed))==(CapTestDeep|CapTestTimed));
  assert(driver.encodeCommand(Command::TestDeep,{},out,error) && out.bytes=="TL\r" && !out.changes_output);
  for (const auto duration : {uint16_t(1),uint16_t(99)}) {
    assert(driver.encodeCommand(Command::TestTimed,{duration,3},out,error));
    assert(out.bytes==(duration==1 ? "T01\r" : "T99\r") && !out.changes_output);
  }
  for (const auto duration : {uint16_t(0),uint16_t(100)})
    assert(!driver.encodeCommand(Command::TestTimed,{duration,3},out,error) && !error.empty() && out.bytes.empty());
  for (uint16_t seconds=12;seconds<=54;seconds+=6) {
    assert(driver.encodeCommand(Command::OutputOff,{1,3,seconds},out,error));
    assert(out.bytes==std::string("S.")+char('0'+seconds/6)+"\r" && out.changes_output);
  }
  assert(driver.encodeCommand(Command::OutputRestart,{1,3,12},out,error) && out.bytes=="S.2R0003\r" && out.changes_output);
  for (const auto seconds : {uint16_t(6),uint16_t(13),uint16_t(60)}) {
    for (const auto command : {Command::OutputOff,Command::OutputRestart})
      assert(!driver.encodeCommand(command,{1,3,seconds},out,error) && !error.empty() && out.bytes.empty());
  }
  for (const auto command : {Command::BeeperToggle,Command::TestShort,Command::TestDeep,Command::TestTimed,Command::TestCancel,Command::ShutdownCancel})
    assert(!driver.encodeCommand(command,{1,3,12},out,error) && !error.empty() && out.bytes.empty());
  assert(driver.encodeCommand(Command::OutputRestart,{10,9999},out,error) && out.bytes=="S10R9999\r");
  assert(driver.encodeCommand(Command::OutputOff,{10,3},out,error) && out.bytes=="S10\r");
  for (const auto cmd : {Command::OutputOff,Command::OutputRestart}) {
    for (const uint16_t delay : {uint16_t(0),uint16_t(11),uint16_t(65535)}) {
      assert(!driver.encodeCommand(cmd,{delay,3},out,error) && !error.empty());
      assert(out.bytes.empty());
    }
  }
  for (const uint16_t restore : {uint16_t(0),uint16_t(1),uint16_t(2),uint16_t(10000),uint16_t(65535)})
    assert(!driver.encodeCommand(Command::OutputRestart,{1,restore},out,error) && out.bytes.empty());
  assert(!driver.encodeCommand(static_cast<Command>(999),{},out,error) && out.bytes.empty());
  assert(std::string(driver.info().id)=="megatec_q1" && driver.info().default_baud==2400);
  assert(std::string(driver.info().label)=="MegaTec Q1");
  assert(drivers().size()==1 && findDriver("megatec_q1")==drivers()[0] && !findDriver("unknown"));
  // ---- Extended queries BT (runtime) and BP (charge): strict parsing ----
  {
    MegaTecQ1Driver driver;
    Snapshot base;
    base.battery_charge = {0, false};
    base.runtime_seconds = {0, false};

    // BT valid: wire "#  42 0.120\r", total length 12 incl CR -> runtime = 42 min * 60 s; charge untouched
    {
      const std::string bt = "#  42 0.120\r";
      assert(bt.size() == 12);
      Snapshot s = base;
      assert(driver.parseReply(PollKind::ExtendedRuntime, bt, s) == ParseResult::Accepted);
      metric(s.runtime_seconds, 42.0f * 60.0f);
      assert(!s.battery_charge.valid);
    }
    // BT valid with leading zeros / spaces
    {
      Snapshot s = base;
      assert(driver.parseReply(PollKind::ExtendedRuntime, "#   007 5.5\r", s) == ParseResult::Accepted);
      metric(s.runtime_seconds, 7.0f * 60.0f);
    }
    // BT strict length: too long or missing CR is Invalid
    {
      Snapshot s = base;
      assert(driver.parseReply(PollKind::ExtendedRuntime, "#  42 0.120\rX", s) == ParseResult::Invalid);
      assert(driver.parseReply(PollKind::ExtendedRuntime, "#  42 0.120", s) == ParseResult::Invalid);
      assert(!s.runtime_seconds.valid);
    }
    // BT missing '#' prefix is Invalid
    {
      Snapshot s = base;
      assert(driver.parseReply(PollKind::ExtendedRuntime, "  42 0.12\r", s) == ParseResult::Invalid);
      assert(!s.runtime_seconds.valid);
    }
    // BT non-digits in the minutes field is Invalid
    {
      Snapshot s = base;
      assert(driver.parseReply(PollKind::ExtendedRuntime, "#  1x 0.12\r", s) == ParseResult::Invalid);
      assert(!s.runtime_seconds.valid);
    }
    // BT malformed reply must not mutate existing metrics
    {
      Snapshot good = base;
      good.runtime_seconds = {12345, true};
      good.battery_charge = {77.5f, true};
      assert(driver.parseReply(PollKind::ExtendedRuntime, "BAD\r\n", good) == ParseResult::Invalid);
      metric(good.runtime_seconds, 12345);
      metric(good.battery_charge, 77.5f);
    }

    // BP valid: "#1000\r", total length 6 incl CR -> charge = 1000 * 0.1 = 100.0
    {
      const std::string bp = "#1000\r";
      assert(bp.size() == 6);
      Snapshot s = base;
      assert(driver.parseReply(PollKind::ExtendedCharge, bp, s) == ParseResult::Accepted);
      metric(s.battery_charge, 100.0f);
      assert(!s.runtime_seconds.valid);
    }
    // BP boundary: 99.9 and 100.0 accepted
    {
      Snapshot s = base;
      assert(driver.parseReply(PollKind::ExtendedCharge, "#0999\r", s) == ParseResult::Accepted);
      metric(s.battery_charge, 99.9f);
      Snapshot s2 = base;
      assert(driver.parseReply(PollKind::ExtendedCharge, "#1000\r", s2) == ParseResult::Accepted);
      metric(s2.battery_charge, 100.0f);
    }
    // BP above 100.0 rejected (also length 7)
    {
      Snapshot s = base;
      assert(driver.parseReply(PollKind::ExtendedCharge, "#9999\r", s) == ParseResult::Invalid);
      assert(driver.parseReply(PollKind::ExtendedCharge, "#10001\r", s) == ParseResult::Invalid);
      assert(driver.parseReply(PollKind::ExtendedCharge, "#10000\r", s) == ParseResult::Invalid);
      assert(!s.battery_charge.valid);
    }
    // BP missing '#' / non-digits / missing CR is Invalid
    {
      Snapshot s = base;
      assert(driver.parseReply(PollKind::ExtendedCharge, "1000\r", s) == ParseResult::Invalid);
      assert(driver.parseReply(PollKind::ExtendedCharge, "#1x00\r", s) == ParseResult::Invalid);
      assert(driver.parseReply(PollKind::ExtendedCharge, "#1000", s) == ParseResult::Invalid);
      assert(!s.battery_charge.valid);
    }
  }

  // ---- ExtendedProbe: self-disable after 3 consecutive failures ----
  {
    ups::ExtendedProbe runtime(600000, 3);
    assert(runtime.supports());
    runtime.recordFailure();  // 1
    assert(runtime.supports());
    runtime.recordFailure();  // 2
    assert(runtime.supports());
    runtime.recordFailure();  // 3 -> disabled until reboot
    assert(!runtime.supports());
    // once disabled: never due, not re-enabled by a later success
    assert(!runtime.due(0));
    runtime.recordSuccess();
    assert(!runtime.supports());
    assert(!runtime.due(999999999));
    runtime.reset();          // simulates reboot
    assert(runtime.supports());
    assert(runtime.due(0));

    // rate gate: after an attempt, not due until kProbePeriodMs elapses
    ups::ExtendedProbe charge(1000, 3);
    assert(charge.due(0));
    charge.markSent(0);
    assert(!charge.due(500));
    assert(charge.due(30000));

    // two failures still supported; a success clears failures
    charge.recordFailure();
    charge.recordFailure();
    assert(charge.supports());
    assert(charge.failures() == 2);
    charge.recordSuccess();
    assert(charge.failures() == 0);
  }

  std::cout << "Driver tests: all assertions passed\n";
}
