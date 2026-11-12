#include "management_service.hpp"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <utility>

namespace gateway {
namespace {
constexpr const char* kPartition="appcfg";
constexpr const char* kNamespace="management";
constexpr const char* kJournalKey="schedules";
struct NvsHandle{nvs_handle_t value{};~NvsHandle(){if(value)nvs_close(value);}};
struct Lock{SemaphoreHandle_t mutex;explicit Lock(SemaphoreHandle_t m):mutex(m){if(mutex)xSemaphoreTake(mutex,portMAX_DELAY);}~Lock(){if(mutex)xSemaphoreGive(mutex);}};
uint64_t currentUtc(){const time_t now=time(nullptr);return now>=time_t(kFirstValidUtc)&&uint64_t(now)<=kLastValidUtc?uint64_t(now):0;}
LocalScheduleTime localSchedule(uint64_t utc){
  LocalScheduleTime out;if(!utc)return out;const time_t now=static_cast<time_t>(utc);struct tm local{};
  if(!localtime_r(&now,&local))return out;
  out.valid=true;out.weekday=uint8_t(local.tm_wday);out.minute_of_day=uint16_t(local.tm_hour*60+local.tm_min);out.second=uint8_t(local.tm_sec);
  out.day_key=uint32_t(local.tm_year+1900)*10000+uint32_t(local.tm_mon+1)*100+uint32_t(local.tm_mday);return out;
}
std::string nvsError(const char* message,esp_err_t code){return std::string(message)+": "+esp_err_to_name(code);}
}
ManagementService::ManagementService(const Config& c,UpsService& ups,EventBus& events):config_(c),ups_(ups),events_(events),scheduler_(c){}
bool ManagementService::begin(){
  if(mutex_)return true;
  mutex_=xSemaphoreCreateMutex();if(!mutex_)return false;
  setenv("TZ",config_.timezone.c_str(),1);tzset();
  if(config_.ntp_enabled)configTzTime(config_.timezone.c_str(),config_.ntp_server.c_str());
  ScheduleJournal journal;std::string error;const bool journal_ok=readJournal(journal,error);scheduler_.begin(journal,journal_ok);
  if(!journal_ok&&!config_.schedules.empty())events_.publish(EventType::ScheduleResult,3,error);
  if(config_.syslog_enabled){
    syslog_queue_=xQueueCreate(8,sizeof(SyslogPacket));
    if(!syslog_queue_||xTaskCreate(syslogEntry,"gateway-syslog",4096,this,1,&syslog_task_)!=pdPASS){syslog_last_state_="failed";syslog_last_message_="Syslog worker allocation failed";return false;}
    syslog_last_state_="ready";syslog_last_message_="UDP delivery is not confirmed";
  }
  tick();return true;
}
bool ManagementService::readJournal(ScheduleJournal& journal,std::string& error){
  const auto initialized=nvs_flash_init_partition(kPartition);
  if(initialized!=ESP_OK){error=nvsError("Schedule partition unavailable",initialized);return false;}
  NvsHandle handle;const auto opened=nvs_open_from_partition(kPartition,kNamespace,NVS_READWRITE,&handle.value);
  if(opened!=ESP_OK){error=nvsError("Schedule journal unavailable",opened);return false;}
  size_t length=0;const auto queried=nvs_get_blob(handle.value,kJournalKey,nullptr,&length);
  if(queried==ESP_ERR_NVS_NOT_FOUND){journal={};return true;}
  if(queried!=ESP_OK||!length||length>4096){error="Schedule journal is unreadable; automatic reset is disabled";return false;}
  std::string text(length,'\0');const auto read=nvs_get_blob(handle.value,kJournalKey,&text[0],&length);
  if(read!=ESP_OK){error=nvsError("Schedule journal read failed",read);return false;}
  text.resize(length);return parseScheduleJournal(text,journal,error);
}
bool ManagementService::saveJournal(const ScheduleJournal& journal,std::string& error){
  const auto text=scheduleJournalJson(journal);ScheduleJournal checked;
  if(text.empty()||!parseScheduleJournal(text,checked,error))return false;
  NvsHandle handle;auto result=nvs_open_from_partition(kPartition,kNamespace,NVS_READWRITE,&handle.value);
  if(result!=ESP_OK){error=nvsError("Schedule journal open failed",result);return false;}
  result=nvs_set_blob(handle.value,kJournalKey,text.data(),text.size());
  if(result==ESP_OK)result=nvs_commit(handle.value);
  if(result!=ESP_OK){error=nvsError("Schedule claim persistence failed",result);return false;}
  error.clear();return true;
}
void ManagementService::tick(){
  if(!mutex_)return;
  const uint64_t now=uptimeMs();if(ticked_&&now-last_tick_ms_<200)return;ticked_=true;last_tick_ms_=now;
  const uint64_t utc=currentUtc();const auto local=localSchedule(utc);const auto snapshot=ups_.snapshot();const auto control=ups_.controlStatus();
  std::vector<Event> transitions;std::vector<ScheduleRuntime> before,after;
  {
    Lock lock(mutex_);history_.sample(config_,snapshot,now,utc);transitions=alarms_.update(config_,snapshot,now);before=scheduler_.runtime();
    const ScheduleEngine::Save save=[this](const ScheduleJournal& j,std::string& e){return saveJournal(j,e);};
    scheduler_.observeCommand(control.last_id,control.last_state,control.last_message,save);
    scheduler_.tick(utc,now,local,ups_.driver(),snapshot,save,[this](ups::Command c,const ups::CommandArgs& args,uint32_t& id,std::string& e){return ups_.enqueue(c,args,id,e);});
    after=scheduler_.runtime();
  }
  for(const auto& e:transitions)events_.publish(e.type,e.severity,e.message,e.uptime_ms);
  for(size_t i=0;i<after.size();++i)if(i>=before.size()||after[i].last_state!=before[i].last_state||after[i].last_claim!=before[i].last_claim){
    if(!after[i].last_state.empty())events_.publish(EventType::ScheduleResult,(after[i].last_state=="failed"||after[i].last_state=="skipped"||after[i].last_state=="uncertain")?2:1,"Schedule "+std::to_string(config_.schedules[i].id)+" "+after[i].last_state+": "+after[i].last_message);
  }
  if(config_.syslog_enabled){
    const auto logs=events_.latest();
    for(auto it=logs.rbegin();it!=logs.rend();++it){
      if(int32_t(it->seq-last_syslog_seq_)<=0)continue;
      if(last_syslog_seq_&&uint32_t(it->seq-last_syslog_seq_)>1)syslog_dropped_+=uint32_t(it->seq-last_syslog_seq_)-1;
      last_syslog_seq_=it->seq;
      if(it->severity>=config_.syslog_min_severity&&!queueSyslog(*it))++syslog_dropped_;
    }
  }
}
std::string ManagementService::alarmsJson()const{Lock lock(mutex_);return alarms_.json();}
std::string ManagementService::historyJson(size_t limit)const{Lock lock(mutex_);return history_.json(config_.history_interval_s,std::min(limit,DataHistory::capacity));}
std::string ManagementService::historyCsv()const{Lock lock(mutex_);return history_.csv();}
std::string ManagementService::timeJson()const{
  const uint64_t utc=currentUtc();DynamicJsonDocument d(1024);d["valid"]=utc!=0;if(utc)d["utc"]=utc;else d["utc"]=nullptr;
  char display[32]{};if(utc){const time_t now=static_cast<time_t>(utc);struct tm local{};if(localtime_r(&now,&local))strftime(display,sizeof(display),"%Y-%m-%d %H:%M:%S",&local);}
  d["local"]=display;d["timezone"]=config_.timezone;d["ntp_enabled"]=config_.ntp_enabled;d["ntp_server"]=config_.ntp_server;
  d["min_utc"]=kFirstValidUtc;d["max_utc"]=supportedClockMaxUtc();std::string out;serializeJson(d,out);return out;
}
std::string ManagementService::schedulesJson()const{Lock lock(mutex_);return scheduler_.json(currentUtc()!=0);}
std::string ManagementService::notificationsJson()const{
  Lock lock(mutex_);DynamicJsonDocument d(2048);auto s=d.createNestedObject("syslog");s["enabled"]=config_.syslog_enabled;s["host"]=config_.syslog_host;s["port"]=config_.syslog_port;s["min_severity"]=config_.syslog_min_severity;
  s["last_state"]=syslog_last_state_;s["last_message"]=syslog_last_message_;s["sent"]=syslog_sent_.load();s["failed"]=syslog_failed_.load();s["dropped"]=syslog_dropped_.load();s["queued"]=syslog_queue_?uxQueueMessagesWaiting(syslog_queue_):0;s["delivery_confirmed"]=false;
  std::string out;serializeJson(d,out);return out;
}
bool ManagementService::setTime(uint64_t utc,std::string& error){
  error.clear();if(config_.ntp_enabled){error="Disable NTP and reboot before setting time manually";return false;}
  if(utc<kFirstValidUtc||utc>supportedClockMaxUtc()){error="UTC is outside the supported system-clock range";return false;}
  {Lock lock(mutex_);struct timeval tv{};tv.tv_sec=static_cast<time_t>(utc);if(settimeofday(&tv,nullptr)!=0){error="System clock could not be set";return false;}}
  events_.publish(EventType::ClockChanged,1,"System clock set manually; scheduler correction guard applies");return true;
}
void ManagementService::clearHistory(){Lock lock(mutex_);history_.clear();}
bool ManagementService::queueSyslog(const Event& event){
  if(!syslog_queue_||!syslog_task_)return false;
  char timestamp[24]="Jan  1 00:00:00";Event copy=event;
  if(event.utc){const time_t now=static_cast<time_t>(event.utc);struct tm local{};if(localtime_r(&now,&local))strftime(timestamp,sizeof(timestamp),"%b %e %H:%M:%S",&local);}
  else copy.message="[clock unavailable; uptime_s="+std::to_string(event.uptime_ms/1000)+"] "+copy.message;
  const auto message=rfc3164Message(config_,copy,timestamp);SyslogPacket packet{};packet.length=uint16_t(message.size());std::memcpy(packet.text,message.data(),message.size());
  return xQueueSend(syslog_queue_,&packet,0)==pdTRUE;
}
bool ManagementService::testSyslog(std::string& error){
  error.clear();if(!config_.syslog_enabled){error="Syslog is disabled in the active configuration";return false;}
  Event event{0,uptimeMs(),EventType::SyslogTest,1,eventMessage(EventType::SyslogTest),currentUtc()};
  if(!queueSyslog(event)){error="Syslog queue is full or unavailable";++syslog_dropped_;return false;}
  syslogState("queued","Test queued; UDP delivery is not confirmed");return true;
}
void ManagementService::syslogState(const char* state,const std::string& message){Lock lock(mutex_);syslog_last_state_=state;syslog_last_message_=message.substr(0,160);}
void ManagementService::syslogEntry(void* context){static_cast<ManagementService*>(context)->syslogRun();}
void ManagementService::syslogRun(){
  sockaddr_in destination{};destination.sin_family=AF_INET;destination.sin_port=htons(uint16_t(config_.syslog_port));
  bool resolved=false;uint64_t resolved_ms=0,last_dns_attempt=0;bool attempted=false;int socket_fd=-1;
  for(;;){SyslogPacket packet{};if(xQueueReceive(syslog_queue_,&packet,portMAX_DELAY)!=pdTRUE)continue;
    const uint64_t now=uptimeMs();
    if(!resolved||now-resolved_ms>=900000){
      if(!attempted||now-last_dns_attempt>=60000){
        attempted=true;last_dns_attempt=now;addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_DGRAM;addrinfo* result=nullptr;
        // DNS can wait in this dedicated worker; it never holds the UART or HTTP lock.
        const int status=getaddrinfo(config_.syslog_host.c_str(),nullptr,&hints,&result);
        if(status==0&&result&&result->ai_addrlen>=sizeof(sockaddr_in)){destination.sin_addr=reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;resolved=true;resolved_ms=now;}
        else resolved=false;
        if(result)freeaddrinfo(result);
      }
    }
    if(!resolved){++syslog_failed_;syslogState("failed","Syslog host resolution failed; retry is rate limited");continue;}
    if(socket_fd<0){socket_fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(socket_fd>=0){struct timeval timeout{0,200000};setsockopt(socket_fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));}}
    if(socket_fd<0){++syslog_failed_;syslogState("failed","UDP socket is unavailable");continue;}
    const int sent=sendto(socket_fd,packet.text,packet.length,MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&destination),sizeof(destination));
    if(sent==packet.length){++syslog_sent_;syslogState("sent_unconfirmed","UDP datagram submitted; remote delivery is not confirmed");}
    else{++syslog_failed_;syslogState("failed","UDP datagram could not be submitted");close(socket_fd);socket_fd=-1;}
  }
}
}
