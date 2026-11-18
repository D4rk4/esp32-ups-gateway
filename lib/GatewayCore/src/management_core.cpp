#include "management_core.hpp"
#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace gateway {
namespace {
const char* metricNames[]={"input_voltage","output_voltage","load_percent","input_frequency","ups_temperature","battery_voltage","battery_charge","runtime_seconds"};
bool validUtc(uint64_t t){return t>=kFirstValidUtc&&t<=kLastValidUtc;}
bool singleObject(const std::string& text){
  size_t depth=0;bool started=false,finished=false,quoted=false,escaped=false;
  for(unsigned char ch:text){
    if(!started||finished){if(ch==' '||ch=='\t'||ch=='\r'||ch=='\n')continue;if(finished||ch!='{')return false;started=true;}
    if(quoted){if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='"')quoted=false;}
    else if(ch=='"')quoted=true;
    else if(ch=='{'||ch=='[')++depth;
    else if(ch=='}'||ch==']'){if(!depth)return false;if(--depth==0)finished=true;}
  }return finished&&!quoted;
}
std::string number(float value){char out[32];std::snprintf(out,sizeof(out),"%.6g",double(value));return out;}
uint32_t capability(ups::Command c){const auto* info=ups::findCommand(c);return info?info->capability:0;}
ScheduleRecord* recordFor(ScheduleJournal& journal,uint32_t id){
  for(auto& r:journal.records)if(r.id==id)return &r;
  if(journal.records.size()>=8)return nullptr;
  journal.records.push_back({id,0,0,{},{}});return &journal.records.back();
}
const ScheduleRecord* recordFor(const ScheduleJournal& journal,uint32_t id){for(const auto& r:journal.records)if(r.id==id)return &r;return nullptr;}
void writeRule(JsonObject o,const ScheduleRule& r){
  o["id"]=r.id;o["enabled"]=r.enabled;o["name"]=r.name;o["command"]=r.command;
  o["delay_minutes"]=r.delay_minutes;o["restore_minutes"]=r.restore_minutes;
  o["mode"]=r.mode;o["at_utc"]=r.at_utc;o["days_mask"]=r.days_mask;o["minute_of_day"]=r.minute_of_day;
}
}
bool freshSnapshot(const ups::Snapshot& s,uint64_t now,uint32_t stale_ms){
  return s.connected&&s.status_valid&&uint32_t(uint32_t(now)-s.last_success_ms)<stale_ms;
}
void writeLastTransfer(JsonObject ups,const Event* e){
  if(!e){ups["last_transfer"]=nullptr;return;}
  auto o=ups.createNestedObject("last_transfer");o["uptime_s"]=e->uptime_ms/1000;
  if(e->utc)o["utc"]=e->utc;else o["utc"]=nullptr;
}
bool parseScheduledCommand(const std::string& name,ups::Command& out){const auto* info=ups::findCommand(name);if(!info)return false;out=info->command;return true;}
bool scheduledCommandReady(const Config& c,const ScheduleRule& r,const ups::UpsDriver* driver,const ups::Snapshot& s,uint64_t now,std::string& reason){
  reason.clear();ups::Command command{};ups::EncodedCommand encoded;
  if(!c.controls_enabled){reason="UPS controls are disabled";return false;}
  if(!parseScheduledCommand(r.command,command)||!driver||!(driver->info().capabilities&capability(command))){reason="Command is not supported by the active driver";return false;}
  if(r.delay_minutes>65535||r.restore_minutes>65535||!driver->encodeCommand(command,{uint16_t(r.delay_minutes),uint16_t(r.restore_minutes)},encoded,reason)||encoded.bytes.empty()){
    if(reason.empty())reason="Driver rejected command arguments";
    return false;
  }
  if(encoded.changes_output&&!c.power_controls_enabled){reason="Power controls are disabled";return false;}
  if(!freshSnapshot(s,now,c.stale_ms)){reason="A fresh connected UPS status is required";return false;}
  return true;
}
void DataHistory::sample(const Config& c,const ups::Snapshot& s,uint64_t now,uint64_t utc){
  if(sampled_&&now>=last_sample_ms_&&now-last_sample_ms_<uint64_t(c.history_interval_s)*1000)return;
  sampled_=true;last_sample_ms_=now;HistorySample row;
  row.seq=++sequence_;row.uptime_s=now/1000;row.utc=validUtc(utc)?utc:0;row.connected=freshSnapshot(s,now,c.stale_ms);
  const ups::Metric* metrics[]={&s.input_voltage,&s.output_voltage,&s.load_percent,&s.input_frequency,&s.ups_temperature,&s.battery_voltage,&s.battery_charge,&s.runtime_seconds};
  if(row.connected)for(size_t i=0;i<8;++i)if(metrics[i]->valid&&std::isfinite(metrics[i]->value)){row.values[i]=metrics[i]->value;row.valid_mask|=uint16_t(1u<<i);}
  samples_[head_]=row;head_=(head_+1)%capacity;if(count_<capacity)++count_;
}
void DataHistory::clear(){head_=0;count_=0;sampled_=false;}
std::vector<HistorySample> DataHistory::latest(size_t limit)const{
  const size_t n=std::min(count_,limit);std::vector<HistorySample> out;out.reserve(n);
  for(size_t i=0;i<n;++i)out.push_back(samples_[(head_+capacity-n+i)%capacity]);
  return out;
}
std::string DataHistory::json(uint32_t interval,size_t limit)const{
  // Serialize one bounded row at a time instead of a large DOM on the ESP32 heap.
  std::string out="{\"samples\":[";const size_t n=std::min(count_,limit);out.reserve(n*300+100);
  for(size_t i=0;i<n;++i){
    if(i)out+=',';
    const auto& row=samples_[(head_+capacity-n+i)%capacity];StaticJsonDocument<768> d;
    d["seq"]=row.seq;d["uptime_s"]=row.uptime_s;if(row.utc)d["utc"]=row.utc;else d["utc"]=nullptr;
    d["connected"]=row.connected;for(size_t k=0;k<8;++k){if(row.valid_mask&(1u<<k))d[metricNames[k]]=row.values[k];else d[metricNames[k]]=nullptr;}
    serializeJson(d,out);
  }
  out+="],\"capacity\":240,\"interval_s\":"+std::to_string(interval)+",\"volatile\":true}";return out;
}
std::string DataHistory::csv()const{
  std::string out="seq,utc_seconds,uptime_seconds,connected,input_voltage_V,output_voltage_V,load_percent,input_frequency_Hz,ups_temperature_C,battery_voltage_V,battery_charge_percent,runtime_seconds\r\n";
  out.reserve(count_*130+200);
  for(size_t i=0;i<count_;++i){const auto& s=samples_[(head_+capacity-count_+i)%capacity];
    out+=std::to_string(s.seq)+","+(s.utc?std::to_string(s.utc):"")+","+std::to_string(s.uptime_s)+","+(s.connected?"true":"false");
    for(size_t k=0;k<8;++k){out+=',';if(s.valid_mask&(1u<<k))out+=number(s.values[k]);}out+="\r\n";
  }return out;
}
std::vector<Event> AlarmTracker::update(const Config& c,const ups::Snapshot& s,uint64_t now){
  std::vector<Event> changes;std::vector<Alarm> next;const bool fresh=freshSnapshot(s,now,c.stale_ms);
  auto add=[&](const char* id,uint8_t severity,const std::string& message,uint64_t since){
    for(const auto& previous:active_)if(previous.id==id){since=previous.since_uptime_s;break;}
    next.push_back({id,severity,message,since});
  };
  if(!fresh)add("link",3,"Fresh UPS status is unavailable",now/1000);
  else{
    if(s.mains_failed)add("on_battery",2,"UPS is operating on battery",now/1000);
    if(s.low_battery)add("low_battery",3,"UPS reports low battery",now/1000);
    if(s.ups_fault)add("ups_fault",3,"UPS reports a fault",now/1000);
    if(s.shutdown_active)add("shutdown_pending",2,"UPS reports an active shutdown sequence",now/1000);
  }
  const ups::Metric* metrics[]={&s.load_percent,&s.ups_temperature,&s.input_voltage,&s.input_voltage};
  const float thresholds[]={float(c.alarm_load_high),float(c.alarm_temperature_high),float(c.alarm_input_low),float(c.alarm_input_high)};
  const char* ids[]={"load_high","temperature_high","input_low","input_high"};
  const char* messages[]={"Load exceeds the configured threshold","Temperature exceeds the configured threshold","Input voltage is below the configured threshold","Input voltage exceeds the configured threshold"};
  for(size_t i=0;i<thresholds_.size();++i){auto& state=thresholds_[i];const bool available=fresh&&metrics[i]->valid&&std::isfinite(metrics[i]->value);
    const bool exceeded=available&&(i==2?metrics[i]->value<thresholds[i]:metrics[i]->value>thresholds[i]);
    if(exceeded){
      if(!state.pending){state.pending=true;state.since_ms=now;}
      if(!state.active&&now-state.since_ms>=uint64_t(c.alarm_delay_s)*1000){state.active=true;changes.push_back({0,now,EventType::ThresholdRaised,2,messages[i]});}
      if(state.active)add(ids[i],2,messages[i],state.since_ms/1000);
    }else{
      if(state.active)changes.push_back({0,now,EventType::ThresholdCleared,1,std::string(messages[i])+(available?": cleared":": no fresh measurement")});
      state={};
    }
  }
  active_=std::move(next);return changes;
}
std::string AlarmTracker::json()const{
  DynamicJsonDocument d(4096);auto a=d.createNestedArray("alarms");unsigned critical=0,warning=0;
  for(const auto& alarm:active_){auto o=a.createNestedObject();o["id"]=alarm.id;o["severity"]=alarm.severity;o["message"]=alarm.message;o["since_uptime_s"]=alarm.since_uptime_s;if(alarm.severity==3)++critical;else if(alarm.severity==2)++warning;}
  d["critical"]=critical;d["warning"]=warning;std::string out;serializeJson(d,out);return out;
}
std::vector<Event> filterEvents(const std::vector<Event>& events,uint8_t min_severity,const std::string& contains,size_t limit){
  std::vector<Event> out;for(const auto& e:events){if(out.size()>=limit)break;if(e.severity>=min_severity&&(contains.empty()||e.message.find(contains)!=std::string::npos||std::string(eventName(e.type)).find(contains)!=std::string::npos))out.push_back(e);}return out;
}
std::string rfc3164Message(const Config& c,const Event& e,const std::string& timestamp){
  // user facility (1); gateway severity remains 1=info, 2=warning, 3=critical.
  const unsigned severity=e.severity>=3?2:e.severity==2?4:6;
  std::string out="<"+std::to_string(8+severity)+">"+timestamp+" "+c.hostname+" ups-gateway: "+eventName(e.type)+" "+e.message;
  for(auto& ch:out)if(static_cast<unsigned char>(ch)<32||ch==127)ch=' ';
  if(out.size()>480)out.resize(480);
  return out;
}
std::string scheduleJournalJson(const ScheduleJournal& journal){
  DynamicJsonDocument d(8192);d["version"]=1;d["guarded_until"]=journal.guarded_until;auto records=d.createNestedArray("records");
  for(const auto& r:journal.records){auto o=records.createNestedObject();o["id"]=r.id;o["last_claim"]=r.last_claim;o["last_local_day"]=r.last_local_day;o["last_state"]=r.last_state;o["last_message"]=r.last_message;}
  if(d.overflowed())return {};
  std::string out;serializeJson(d,out);return out;
}
bool parseScheduleJournal(const std::string& text,ScheduleJournal& out,std::string& error){
  error.clear();auto fail=[&](){error="Invalid schedule journal; automatic overwrite is disabled";return false;};
  if(text.empty()||text.size()>4096||!singleObject(text))return fail();
  DynamicJsonDocument d(8192);if(deserializeJson(d,text)||d.overflowed()||!d.is<JsonObject>())return fail();
  if(!d["version"].is<uint32_t>()||d["version"].as<uint32_t>()!=1||!d["guarded_until"].is<uint64_t>()||!d["records"].is<JsonArrayConst>())return fail();
  ScheduleJournal candidate;candidate.guarded_until=d["guarded_until"].as<uint64_t>();
  if(candidate.guarded_until>kLastValidUtc+59||d["records"].size()>8)return fail();
  for(const auto item:d["records"].as<JsonArrayConst>()){
    if(!item.is<JsonObjectConst>()||!item["id"].is<uint32_t>()||!item["last_claim"].is<uint64_t>()||!item["last_local_day"].is<uint32_t>()||!item["last_state"].is<const char*>()||!item["last_message"].is<const char*>())return fail();
    ScheduleRecord r;r.id=item["id"].as<uint32_t>();r.last_claim=item["last_claim"].as<uint64_t>();r.last_local_day=item["last_local_day"].as<uint32_t>();
    const auto state=item["last_state"].as<JsonString>(),message=item["last_message"].as<JsonString>();r.last_state.assign(state.c_str(),state.size());r.last_message.assign(message.c_str(),message.size());
    if(!r.id||r.id>65535||r.last_claim>candidate.guarded_until||r.last_local_day>99991231||r.last_state.size()>24||r.last_message.size()>160)return fail();
    for(const auto& old:candidate.records)if(old.id==r.id)return fail();
    for(const auto* s:{&r.last_state,&r.last_message})for(unsigned char ch:*s)if(ch<32||ch==127)return fail();
    candidate.records.push_back(std::move(r));
  }
  out=std::move(candidate);return true;
}
void ScheduleEngine::begin(const ScheduleJournal& journal,bool writable){
  journal_=journal;writable_=writable;observed_=false;blocked_until_=journal.guarded_until;high_water_=journal.guarded_until;runtime_.assign(config_.schedules.size(),{});
  std::vector<ScheduleRecord> records;
  for(const auto& rule:config_.schedules){const auto* old=recordFor(static_cast<const ScheduleJournal&>(journal_),rule.id);if(old)records.push_back(*old);else records.push_back({rule.id,0,0,{},{}});}
  journal_.records=std::move(records);
  for(size_t i=0;i<runtime_.size();++i){const auto& r=journal_.records[i];auto& run=runtime_[i];run.last_claim=r.last_claim;run.last_state=r.last_state;run.last_message=r.last_message;
    if(run.last_state=="claimed"||run.last_state=="queued"){run.last_state="uncertain";run.last_message="Previous execution was claimed; result after restart is unknown";}
  }
}
bool ScheduleEngine::saveGuard(uint64_t until,const Save& save){
  if(until<=journal_.guarded_until)return writable_;
  ScheduleJournal candidate=journal_;candidate.guarded_until=until;std::string error;
  if(!writable_||!save(candidate,error)){journal_error_=error.empty()?"Schedule journal is unavailable":error;return false;}
  journal_=std::move(candidate);journal_error_.clear();return true;
}
void ScheduleEngine::refresh(uint64_t utc,uint64_t now,const LocalScheduleTime& local,const ups::UpsDriver* driver,const ups::Snapshot& s){
  for(size_t i=0;i<config_.schedules.size();++i){const auto& r=config_.schedules[i];auto& run=runtime_[i];run.armed=false;
    if(!r.enabled)run.reason="Disabled";
    else if(!validUtc(utc))run.reason="Clock is not valid";
    else if(!writable_||!journal_error_.empty())run.reason=journal_error_.empty()?"Schedule journal is unavailable":journal_error_;
    else if(utc<=blocked_until_)run.reason="Waiting beyond the boot or clock-correction safety window";
    else if(r.mode=="once"&&run.last_claim>=r.at_utc)run.reason="This occurrence is already claimed";
    else if(r.mode=="once"&&utc>r.at_utc)run.reason="Past occurrence; catch-up is disabled";
    else if(r.mode=="weekly"&&!local.valid)run.reason="Local clock is unavailable";
    else if(!scheduledCommandReady(config_,r,driver,s,now,run.reason)){}
    else{run.armed=true;run.reason="Armed for the next future occurrence";}
  }
}
void ScheduleEngine::tick(uint64_t utc,uint64_t now,const LocalScheduleTime& local,const ups::UpsDriver* driver,const ups::Snapshot& s,const Save& save,const Enqueue& enqueue){
  if(runtime_.size()!=config_.schedules.size())begin(journal_,writable_);
  const bool enabled=std::any_of(config_.schedules.begin(),config_.schedules.end(),[](const ScheduleRule& r){return r.enabled;});
  if(!validUtc(utc)){observed_=false;refresh(utc,now,local,driver,s);return;}
  const uint64_t minute_end=utc-utc%60+59;
  high_water_=std::max(high_water_,minute_end);
  if(!observed_){
    observed_=true;previous_utc_=utc;previous_uptime_ms_=now;blocked_until_=high_water_;
    if(enabled&&!saveGuard(blocked_until_,save))blocked_until_=std::max(blocked_until_,minute_end);
    refresh(utc,now,local,driver,s);return;
  }
  const uint64_t previous=previous_utc_;const uint64_t elapsed=now>=previous_uptime_ms_?(now-previous_uptime_ms_)/1000:0;
  const bool discontinuity=utc<previous||now<previous_uptime_ms_||utc-previous>=60||utc-previous>elapsed+2;
  previous_utc_=utc;previous_uptime_ms_=now;
  if(discontinuity){blocked_until_=high_water_;if(enabled)saveGuard(blocked_until_,save);refresh(utc,now,local,driver,s);return;}
  // Normal idle minutes only advance RAM high-water. Boot, discontinuities,
  // and each claimed/skipped occurrence checkpoint it before any command.
  // Retry failed discontinuity checkpoints no more than once per minute.
  if(enabled&&!journal_error_.empty()&&writable_&&utc>blocked_until_&&!saveGuard(high_water_,save)){blocked_until_=high_water_;refresh(utc,now,local,driver,s);return;}
  refresh(utc,now,local,driver,s);
  if(!enabled||utc<=blocked_until_||!writable_||!journal_error_.empty())return;
  for(size_t i=0;i<config_.schedules.size();++i){const auto& r=config_.schedules[i];if(!r.enabled)continue;
    uint64_t occurrence=0;uint32_t local_day=0;
    if(r.mode=="once")occurrence=r.at_utc;
    else if(local.valid&&local.weekday<7&&(r.days_mask&(1u<<local.weekday))&&local.minute_of_day==r.minute_of_day){occurrence=utc-local.second;local_day=local.day_key;}
    if(!occurrence||occurrence<=previous||occurrence>utc||utc-occurrence>=60)continue;
    const auto* old=recordFor(static_cast<const ScheduleJournal&>(journal_),r.id);
    if(old&&(old->last_claim>=occurrence||(local_day&&old->last_local_day>=local_day)))continue;
    auto& run=runtime_[i];std::string reason;const bool ready=scheduledCommandReady(config_,r,driver,s,now,reason);
    ScheduleJournal claimed=journal_;claimed.guarded_until=std::max(claimed.guarded_until,high_water_);auto* record=recordFor(claimed,r.id);if(!record){run.reason="Schedule journal is full";continue;}
    record->last_claim=occurrence;record->last_local_day=local_day;record->last_state=ready?"claimed":"skipped";
    record->last_message=ready?"Claim persisted before enqueue":reason;
    std::string error;
    if(!save(claimed,error)){run.armed=false;run.last_state="failed";run.last_message="Execution not sent: "+error;run.reason="Execution claim could not be persisted";continue;}
    journal_=std::move(claimed);run.last_claim=occurrence;run.last_state=ready?"claimed":"skipped";run.last_message=recordFor(static_cast<const ScheduleJournal&>(journal_),r.id)->last_message;run.armed=false;
    if(!ready){run.reason=reason;continue;}
    ups::Command command{};parseScheduledCommand(r.command,command);uint32_t id=0;
    // Platform enqueue checks active driver, permissions and freshness again.
    const bool accepted=enqueue(command,{uint16_t(r.delay_minutes),uint16_t(r.restore_minutes)},id,error);
    run.command_id=accepted?id:0;run.last_state=accepted?"queued":"failed";run.last_message=accepted?"Queued; UPS execution is not confirmed":error;run.reason=accepted?"Occurrence claimed and queued":error;
    ScheduleJournal result=journal_;auto* updated=recordFor(result,r.id);updated->last_state=run.last_state;updated->last_message=run.last_message.substr(0,160);
    if(save(result,error))journal_=std::move(result); // Failure leaves the durable claim.
  }
}
void ScheduleEngine::observeCommand(uint32_t id,const std::string& state,const std::string& message,const Save& save){
  if(!id)return;
  for(size_t i=0;i<runtime_.size();++i){auto& run=runtime_[i];if(!run.command_id)continue;
    if(run.command_id==id){if(state=="queued"||run.last_state==state)continue;run.last_state=state;run.last_message=message;}
    else if(int32_t(id-run.command_id)>0){run.last_state="uncertain";run.last_message="A later command replaced status; prior UPS execution is unconfirmed";}
    else continue;
    run.command_id=0;ScheduleJournal candidate=journal_;auto* r=recordFor(candidate,config_.schedules[i].id);
    if(r){r->last_state=run.last_state;r->last_message=run.last_message.substr(0,160);std::string error;if(save(candidate,error))journal_=std::move(candidate);}
  }
}
std::string ScheduleEngine::json(bool valid)const{
  DynamicJsonDocument d(12288);d["clock_valid"]=valid;auto rules=d.createNestedArray("rules");
  for(size_t i=0;i<config_.schedules.size();++i){auto o=rules.createNestedObject();writeRule(o,config_.schedules[i]);
    if(i<runtime_.size()){const auto& run=runtime_[i];o["armed"]=run.armed;o["reason"]=run.reason;if(run.last_claim)o["last_claim"]=run.last_claim;else o["last_claim"]=nullptr;o["last_state"]=run.last_state;o["last_message"]=run.last_message;}
  }
  std::string out;serializeJson(d,out);return out;
}
}
