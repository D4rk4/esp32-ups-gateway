#include "event_bus.hpp"
#include <esp_timer.h>
#include <algorithm>
#include <cstring>
#include <ctime>

namespace gateway {
namespace {
constexpr size_t kBatchSize=16;
constexpr uint64_t kFlushIntervalMs=30000;
bool newer(uint32_t candidate,uint32_t reference){return candidate!=reference&&uint32_t(candidate-reference)<0x80000000u;}
}
uint64_t uptimeMs(){return uint64_t(esp_timer_get_time())/1000u;}
EventBus::Queued EventBus::pack(const Event& event,uint64_t queued_ms){
  Queued queued{};queued.seq=event.seq;queued.time_ms=event.uptime_ms;queued.type=event.type;
  queued.severity=event.severity;queued.text_length=static_cast<uint8_t>(event.message.size());
  std::memcpy(queued.text,event.message.data(),event.message.size());queued.utc=event.utc;queued.queued_ms=queued_ms;return queued;
}
Event EventBus::unpack(const Queued& queued){
  Event event;event.seq=queued.seq;event.uptime_ms=queued.time_ms;event.type=queued.type;
  event.severity=queued.severity;event.message.assign(queued.text,queued.text_length);event.utc=queued.utc;return event;
}
bool EventBus::begin(){
  std::string error;const bool available=storage_.begin(error);
  return start(available?&storage_:nullptr,error);
}
bool EventBus::begin(PersistentLogStorage& storage){return start(&storage,"");}
bool EventBus::start(PersistentLogStorage* storage,const std::string& error){
  mutex_=xSemaphoreCreateMutex();storage_mutex_=xSemaphoreCreateMutex();queue_=xQueueCreate(16,sizeof(Queued));
  if(!mutex_||!storage_mutex_||!queue_)return false;
  if(!storage){persistence_error_=error;return true;}
  persistent_=std::make_unique<PersistentLog>(*storage);
  if(!persistent_->begin()){
    persistence_error_="Persistent log is unreadable or corrupt";return true;
  }
  sequence_=persistent_->latestSequence();
  std::vector<Event> recovered;
  if(!persistent_->recent(recovered,capacity())){
    persistence_error_="Persistent log read failed during recovery";return true;
  }
  for(auto it=recovered.rbegin();it!=recovered.rend();++it){
    log_[head_]=*it;head_=(head_+1)%log_.size();++count_;
  }
  // Seed from the recovered ring first; only search the deeper log when the ring
  // has no OnBattery, so a display-only search never stops journaling. A failed
  // deep search is "not known", not a fault.
  for(const auto& event:recovered)if(event.type==EventType::OnBattery){last_transfer_=event;has_transfer_=true;break;}
  if(!has_transfer_)has_transfer_=persistent_->newest(EventType::OnBattery,last_transfer_);
  return true;
}
void EventBus::publish(EventType type,uint8_t severity,const std::string& message,uint64_t time_ms){
  if(!mutex_)return;
  const uint64_t now_ms=uptimeMs();Event event;event.uptime_ms=time_ms?time_ms:now_ms;event.type=type;event.severity=severity;
  const time_t now=time(nullptr);if(now>=1577836800&&uint64_t(now)<=4102444799ULL)event.utc=uint64_t(now);
  event.message=(message.empty()?std::string(eventMessage(type)):message).substr(0,180);
  xSemaphoreTake(mutex_,portMAX_DELAY);
  if(++sequence_==0)++sequence_;event.seq=sequence_;
  log_[head_]=event;head_=(head_+1)%log_.size();if(count_<log_.size())++count_;
  if(type==EventType::OnBattery){has_transfer_=true;last_transfer_=event;}
  const Queued queued=pack(event,now_ms);
  if(pending_count_<pending_.size()){
    pending_[(pending_head_+pending_count_)%pending_.size()]=queued;
    if(!pending_count_)pending_since_=now_ms;
    ++pending_count_;
  }else ++persistence_dropped_;
  xSemaphoreGive(mutex_);
  if(powerNetTrapNumber(type)||type==EventType::Startup||type==EventType::TestTrap){
    if(!queue_||xQueueSend(queue_,&queued,0)!=pdTRUE)++dropped_;
  }
}
bool EventBus::lastTransfer(Event& out)const{
  if(!mutex_)return false;
  xSemaphoreTake(mutex_,portMAX_DELAY);const bool ok=has_transfer_;if(ok)out=last_transfer_;xSemaphoreGive(mutex_);return ok;
}
std::vector<Event> EventBus::latest()const{
  std::vector<Event> out;if(!mutex_)return out;
  xSemaphoreTake(mutex_,portMAX_DELAY);out.reserve(count_);
  for(size_t i=0;i<count_;++i)out.push_back(log_[(head_+log_.size()-1-i)%log_.size()]);
  xSemaphoreGive(mutex_);return out;
}
std::vector<Event> EventBus::history(uint32_t before_seq,size_t limit){
  std::vector<Event> out;if(!mutex_||!storage_mutex_||!limit)return out;
  limit=std::min(limit,capacity());
  xSemaphoreTake(storage_mutex_,portMAX_DELAY);
  if(persistent_&&!persistent_->recent(out,limit,before_seq!=0,before_seq)&&persistence_error_.empty()){
    persistence_error_="Persistent log history read failed";
  }
  out.reserve(limit+capacity()+pending_.size());
  xSemaphoreTake(mutex_,portMAX_DELAY);
  for(size_t i=0;i<count_;++i){
    const Event& event=log_[(head_+log_.size()-1-i)%log_.size()];
    if(!before_seq||newer(before_seq,event.seq))out.push_back(event);
  }
  for(size_t i=0;i<pending_count_;++i){
    const Queued& queued=pending_[(pending_head_+i)%pending_.size()];
    if(!before_seq||newer(before_seq,queued.seq))out.push_back(unpack(queued));
  }
  xSemaphoreGive(mutex_);xSemaphoreGive(storage_mutex_);
  std::sort(out.begin(),out.end(),[](const Event& a,const Event& b){return newer(a.seq,b.seq);});
  out.erase(std::unique(out.begin(),out.end(),[](const Event& a,const Event& b){return a.seq==b.seq;}),out.end());
  if(out.size()>limit)out.resize(limit);return out;
}
PersistenceStatus EventBus::persistenceStatus()const{
  PersistenceStatus status;if(!mutex_||!storage_mutex_){status.error="Event system not initialized";return status;}
  xSemaphoreTake(storage_mutex_,portMAX_DELAY);
  status.ok=persistent_&&persistent_->healthy();status.error=persistence_error_;
  if(persistent_){status.stored=persistent_->count();status.capacity=persistent_->capacity();}
  if(!status.ok&&status.error.empty())status.error="Persistent log is unavailable";
  xSemaphoreTake(mutex_,portMAX_DELAY);status.pending=pending_count_;status.dropped=persistence_dropped_;xSemaphoreGive(mutex_);
  xSemaphoreGive(storage_mutex_);return status;
}
bool EventBus::clearLog(std::string& error){
  if(!mutex_||!storage_mutex_){error="Event system not initialized";return false;}
  xSemaphoreTake(storage_mutex_,portMAX_DELAY);
  if(!persistent_||!persistent_->healthy()){
    error=persistence_error_.empty()?"Persistent log is unavailable":persistence_error_;xSemaphoreGive(storage_mutex_);return false;
  }
  xSemaphoreTake(mutex_,portMAX_DELAY);const uint32_t cutoff=sequence_;xSemaphoreGive(mutex_);
  if(!persistent_->clear(cutoff)){
    persistence_error_="Persistent log clear failed";error=persistence_error_;xSemaphoreGive(storage_mutex_);return false;
  }
  xSemaphoreTake(mutex_,portMAX_DELAY);
  if(has_transfer_&&!newer(last_transfer_.seq,cutoff)){has_transfer_=false;last_transfer_={};}// one published during the clear survives
  while(count_){
    const size_t oldest=(head_+log_.size()-count_)%log_.size();
    if(newer(log_[oldest].seq,cutoff))break;
    log_[oldest]={};--count_;
  }
  while(pending_count_&&!newer(pending_[pending_head_].seq,cutoff)){
    pending_head_=(pending_head_+1)%pending_.size();--pending_count_;
  }
  if(pending_count_)pending_since_=pending_[pending_head_].queued_ms;else pending_since_=0;
  xSemaphoreGive(mutex_);persistence_error_.clear();error.clear();xSemaphoreGive(storage_mutex_);return true;
}
bool EventBus::flushPending(std::string& error){
  if(!mutex_||!storage_mutex_){error="Event system not initialized";return false;}
  xSemaphoreTake(storage_mutex_,portMAX_DELAY);
  if(!persistent_||!persistent_->healthy()){
    error=persistence_error_.empty()?"Persistent log is unavailable":persistence_error_;xSemaphoreGive(storage_mutex_);return false;
  }
  std::vector<Event> batch;
  xSemaphoreTake(mutex_,portMAX_DELAY);batch.reserve(pending_count_);
  for(size_t i=0;i<pending_count_;++i)batch.push_back(unpack(pending_[(pending_head_+i)%pending_.size()]));
  xSemaphoreGive(mutex_);
  if(!batch.empty()&&!persistent_->append(batch)){
    persistence_error_="Persistent log flush failed";error=persistence_error_;xSemaphoreGive(storage_mutex_);return false;
  }
  xSemaphoreTake(mutex_,portMAX_DELAY);
  pending_head_=(pending_head_+batch.size())%pending_.size();pending_count_-=batch.size();
  if(pending_count_)pending_since_=pending_[pending_head_].queued_ms;else pending_since_=0;
  xSemaphoreGive(mutex_);persistence_error_.clear();error.clear();xSemaphoreGive(storage_mutex_);return true;
}
uint32_t EventBus::latestSequence()const{
  if(!mutex_)return 0;xSemaphoreTake(mutex_,portMAX_DELAY);const uint32_t seq=sequence_;xSemaphoreGive(mutex_);return seq;
}
void EventBus::tick(){
  if(!mutex_)return;
  const uint64_t now=uptimeMs();xSemaphoreTake(mutex_,portMAX_DELAY);
  const bool due=pending_count_&&(pending_count_>=kBatchSize||now-pending_since_>=kFlushIntervalMs);
  xSemaphoreGive(mutex_);if(due){std::string error;flushPending(error);}
}
bool EventBus::popTrap(Event& event){Queued queued{};if(!queue_||xQueueReceive(queue_,&queued,0)!=pdTRUE)return false;event=unpack(queued);return true;}
uint32_t EventBus::queued()const{return queue_?uxQueueMessagesWaiting(queue_):0;}
}
