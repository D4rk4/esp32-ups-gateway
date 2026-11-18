#include "management_core.hpp"
#include "driver_registry.hpp"
#include <ArduinoJson.h>
#include <cassert>
#include <cmath>
#include <iostream>
using namespace gateway;
static Config base(){Config c;c.controls_enabled=true;c.power_controls_enabled=true;c.history_interval_s=10;return c;}
static ups::Snapshot fresh(uint64_t now){ups::Snapshot s;s.connected=true;s.status_valid=true;s.last_success_ms=uint32_t(now);s.input_voltage={230,true};s.load_percent={95,true};return s;}
static ScheduleRule once(uint64_t at){ScheduleRule r;r.id=1;r.enabled=true;r.name="test";r.at_utc=at;return r;}
int main(){
  Config c=base();DataHistory history;
  auto s=fresh(1000);history.sample(c,s,1000,0);
  auto rows=history.latest();assert(rows.size()==1&&rows[0].utc==0&&rows[0].connected);
  assert((rows[0].valid_mask&1)!=0&&(rows[0].valid_mask&(1u<<6))==0);
  history.sample(c,s,11000,1794000010);rows=history.latest();assert(rows.size()==2&&!rows.back().connected&&rows.back().valid_mask==0);
  DynamicJsonDocument doc(4096);assert(!deserializeJson(doc,history.json(10)));
  assert(doc["samples"][0]["battery_charge"].isNull()&&doc["samples"][0]["utc"].isNull());
  assert(doc["samples"][1]["input_voltage"].isNull());
  assert(history.csv().find("utc_seconds,uptime_seconds")!=std::string::npos);
  for(uint64_t i=2;i<242;++i)history.sample(c,fresh(i*10000+1000),i*10000+1000,1794000000+i*10);
  rows=history.latest();assert(rows.size()==240&&rows.front().seq==3&&rows.back().seq==242);
  history.clear();assert(history.latest().empty());
  AlarmTracker alarms;c.alarm_delay_s=5;s=fresh(1000);
  assert(alarms.update(c,s,1000).empty());assert(alarms.active().empty());
  s.last_success_ms=5999;alarms.update(c,s,5999);assert(alarms.active().empty());
  s.last_success_ms=6000;auto changes=alarms.update(c,s,6000);assert(changes.size()==1&&changes[0].severity==2&&powerNetTrapNumber(changes[0].type)==0);
  assert(alarms.active().size()==1&&alarms.active()[0].id=="load_high");
  s.load_percent={0,false};changes=alarms.update(c,s,6001);assert(changes.size()==1&&changes[0].severity==1&&alarms.active().empty());
  s.input_voltage={0,false};s.load_percent={NAN,true};alarms.update(c,s,6002);assert(alarms.active().empty());
  alarms.update(c,s,16000);assert(alarms.active().size()==1&&alarms.active()[0].id=="link");
  {// a self-test with utility present raises no on-battery alarm; a real outage still does
    AlarmTracker testing;auto probe=fresh(1000);probe.load_percent={10,true};probe.output_mode=ups::OutputMode::BatteryTest;probe.test_running=true;
    testing.update(c,probe,1000);assert(testing.active().empty());
    probe.mains_failed=true;probe.output_mode=ups::OutputMode::OnBattery;testing.update(c,probe,1001);assert(testing.active().size()==1&&testing.active()[0].id=="on_battery");
  }
  // Durable claim precedes the actual enqueue; replay, overdue and failed saves never enqueue.
  const uint64_t t=1794000000; c=base();c.schedules={once(t+60)};
  ScheduleJournal durable;int sends=0,saves=0;bool fail_claim=false;
  ScheduleEngine::Save save=[&](const ScheduleJournal& j,std::string& error){++saves;if(fail_claim&&!j.records.empty()&&j.records[0].last_claim){error="injected NVS failure";return false;}durable=j;return true;};
  ScheduleEngine::Enqueue send=[&](ups::Command command,const ups::CommandArgs&,uint32_t& id,std::string&){assert(command==ups::Command::TestShort);assert(durable.records[0].last_claim==t+60&&durable.records[0].last_state=="claimed");id=7;++sends;return true;};
  const auto* driver=ups::findDriver("megatec_q1");LocalScheduleTime local;
  ScheduleEngine engine(c);engine.begin({},true);engine.tick(t,0,local,driver,fresh(0),save,send);
  engine.tick(t+59,59000,local,driver,fresh(59000),save,send);engine.tick(t+60,60000,local,driver,fresh(60000),save,send);
  assert(sends==1&&saves>=3&&engine.runtime()[0].last_state=="queued");
  engine.tick(t+60,60100,local,driver,fresh(60100),save,send);assert(sends==1);
  engine.observeCommand(8,"queued","A later manual command",save);
  assert(engine.runtime()[0].last_state=="uncertain");
  ScheduleEngine reboot(c);reboot.begin(durable,true);reboot.tick(t+60,0,local,driver,fresh(0),save,send);
  reboot.tick(t-60,1000,local,driver,fresh(1000),save,send);reboot.tick(t+60,121000,local,driver,fresh(121000),save,send);assert(sends==1);
  ScheduleEngine failed(c);failed.begin({},true);fail_claim=true;failed.tick(t,0,local,driver,fresh(0),save,send);failed.tick(t+59,59000,local,driver,fresh(59000),save,send);failed.tick(t+60,60000,local,driver,fresh(60000),save,send);assert(sends==1);fail_claim=false;
  ScheduleEngine jumped(c);jumped.begin({},true);jumped.tick(t,0,local,driver,fresh(0),save,send);jumped.tick(t+65,1000,local,driver,fresh(1000),save,send);assert(sends==1);
  ScheduleEngine stale(c);stale.begin({},true);stale.tick(t,0,local,driver,fresh(0),save,send);stale.tick(t+59,59000,local,driver,fresh(59000),save,send);stale.tick(t+60,60000,local,driver,fresh(0),save,send);assert(sends==1&&stale.runtime()[0].last_state=="skipped");
  c.schedules[0].command="output_off";c.power_controls_enabled=false;
  std::string reason;assert(!scheduledCommandReady(c,c.schedules[0],driver,fresh(0),0,reason));
  c.schedules[0].command="test_deep";assert(scheduledCommandReady(c,c.schedules[0],driver,fresh(0),0,reason));
  c.schedules[0].command="test_timed";c.schedules[0].delay_minutes=99;assert(scheduledCommandReady(c,c.schedules[0],driver,fresh(0),0,reason));
  for(const uint16_t duration:{uint16_t(1),uint16_t(99)}){
    c.schedules[0].delay_minutes=duration;int timed_sends=0;ups::Command sent_command=ups::Command::TestShort;ups::CommandArgs sent_args;
    const ScheduleEngine::Enqueue timed_send=[&](ups::Command command,const ups::CommandArgs& args,uint32_t& id,std::string&){sent_command=command;sent_args=args;id=9;++timed_sends;return true;};
    ScheduleEngine timed_engine(c);timed_engine.begin({},true);timed_engine.tick(t,0,local,driver,fresh(0),save,timed_send);
    timed_engine.tick(t+59,59000,local,driver,fresh(59000),save,timed_send);timed_engine.tick(t+60,60000,local,driver,fresh(60000),save,timed_send);
    assert(timed_sends==1&&sent_command==ups::Command::TestTimed&&sent_args.delay_minutes==duration&&sent_args.restore_minutes==3&&sent_args.delay_seconds==0);
  }
  c.schedules[0].delay_minutes=100;assert(!scheduledCommandReady(c,c.schedules[0],driver,fresh(0),0,reason));
  const auto message=rfc3164Message(c,{1,1000,EventType::Startup,3,"a\nb\rc"},"Nov 11 01:02:03");
  assert(message.find("<10>")==0&&message.find('\n')==std::string::npos&&message.find('\r')==std::string::npos&&message.size()<=480);
  // A corrupted journal may never turn into permission to replay a schedule.
  ScheduleJournal loaded;assert(parseScheduleJournal(scheduleJournalJson(durable),loaded,reason));
  const auto saved_journal=scheduleJournalJson(loaded);
  assert(!parseScheduleJournal(saved_journal+" trailing",loaded,reason));
  assert(scheduleJournalJson(loaded)==saved_journal);
  // Weekly schedules follow local weekdays, and a repeated DST hour is one occurrence.
  c=base();ScheduleRule weekly;weekly.id=2;weekly.enabled=true;weekly.mode="weekly";weekly.days_mask=2;weekly.minute_of_day=540;c.schedules={weekly};
  int weekly_sends=0;ScheduleJournal weekly_durable;
  const ScheduleEngine::Save week_save=[&](const ScheduleJournal& j,std::string&){weekly_durable=j;return true;};
  const ScheduleEngine::Enqueue week_send=[&](ups::Command,const ups::CommandArgs&,uint32_t& id,std::string&){assert(weekly_durable.records[0].last_local_day==20261109);++weekly_sends;id=8;return true;};
  ScheduleEngine week(c);week.begin({},true);week.tick(t,0,{true,1,539,0,20261109},driver,fresh(0),week_save,week_send);
  week.tick(t+59,59000,{true,1,539,59,20261109},driver,fresh(59000),week_save,week_send);
  week.tick(t+60,60000,{true,1,540,0,20261109},driver,fresh(60000),week_save,week_send);assert(weekly_sends==1);
  week.observeCommand(8,"sent_unconfirmed","Command transmitted; execution is unconfirmed",week_save);
  assert(week.runtime()[0].last_state=="sent_unconfirmed");
  ScheduleEngine repeat(c);repeat.begin(weekly_durable,true);repeat.tick(t+3600,0,{true,1,539,0,20261109},driver,fresh(0),week_save,week_send);
  repeat.tick(t+3659,59000,{true,1,539,59,20261109},driver,fresh(59000),week_save,week_send);
  repeat.tick(t+3660,60000,{true,1,540,0,20261109},driver,fresh(60000),week_save,week_send);assert(weekly_sends==1);
  assert(filterEvents({{1,0,EventType::OnBattery,2,"battery"},{2,0,EventType::UpsFault,3,"fault"}},3,"",48).size()==1);
  // An idle weekly rule must not wear NVS with one write per minute.
  c=base();weekly.days_mask=1;c.schedules={weekly};int idle_writes=0;
  const ScheduleEngine::Save idle_save=[&](const ScheduleJournal&,std::string&){++idle_writes;return true;};
  ScheduleEngine idle(c);idle.begin({},true);
  for(uint64_t second=0;second<=86400;++second)idle.tick(t+second,second*1000,{true,uint8_t(2+second/86400),uint16_t((second/60)%1440),uint8_t(second%60),uint32_t(20261110+second/86400)},driver,fresh(second*1000),idle_save,week_send);
  assert(idle_writes==1);
  // The RAM high-water still prevents a backward jump from replaying idle time.
  idle.tick(t+100,86401000,{true,2,1,40,20261110},driver,fresh(86401000),idle_save,week_send);
  assert(idle_writes==2&&idle.journal().guarded_until>=t+86400&&!idle.runtime()[0].armed);
  {// ups.last_transfer: null without an event; whole boot-uptime seconds; unset clock (utc 0) reads as null, never 0
    auto render=[](const Event* event){DynamicJsonDocument d(256);writeLastTransfer(d.to<JsonObject>(),event);std::string out;serializeJson(d,out);return out;};
    Event e;e.type=EventType::OnBattery;
    assert(render(nullptr)=="{\"last_transfer\":null}");
    e.uptime_ms=1999;e.utc=1794000010;assert(render(&e)=="{\"last_transfer\":{\"uptime_s\":1,\"utc\":1794000010}}");
    e.uptime_ms=120000;e.utc=0;assert(render(&e)=="{\"last_transfer\":{\"uptime_s\":120,\"utc\":null}}");
    e.uptime_ms=5000000000000ULL;e.utc=kLastValidUtc;assert(render(&e)=="{\"last_transfer\":{\"uptime_s\":5000000000,\"utc\":4102444799}}");
  }
  std::cout<<"management: missing/stale history, ring bound, threshold delay, durable scheduling and RFC3164 passed\n";
}
