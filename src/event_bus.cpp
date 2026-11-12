#include "event_bus.hpp"
#include <esp_timer.h>
#include <cstring>
#include <ctime>
namespace gateway {
uint64_t uptimeMs(){return uint64_t(esp_timer_get_time())/1000u;}
bool EventBus::begin(){mutex_=xSemaphoreCreateMutex();queue_=xQueueCreate(16,sizeof(Queued));return mutex_&&queue_;}
void EventBus::publish(EventType type,uint8_t severity,const std::string& message,uint64_t time_ms){
  if(!mutex_)return;
  Event e;e.uptime_ms=time_ms?time_ms:uptimeMs();e.type=type;e.severity=severity;
  const time_t now=time(nullptr);if(now>=1577836800&&uint64_t(now)<=4102444799ULL)e.utc=uint64_t(now);
  e.message=(message.empty()?std::string(eventMessage(type)):message).substr(0,180);
  xSemaphoreTake(mutex_,portMAX_DELAY);e.seq=++sequence_;log_[head_]=e;head_=(head_+1)%log_.size();if(count_<log_.size())++count_;xSemaphoreGive(mutex_);
  if(powerNetTrapNumber(type)||type==EventType::Startup||type==EventType::TestTrap){
    Queued q{};q.seq=e.seq;q.time_ms=e.uptime_ms;q.type=type;q.severity=severity;
    q.utc=e.utc;
    std::memcpy(q.text,e.message.c_str(),e.message.size());
    if(!queue_||xQueueSend(queue_,&q,0)!=pdTRUE)++dropped_;
  }
}
std::vector<Event> EventBus::latest()const{
  std::vector<Event> out;if(!mutex_)return out;
  xSemaphoreTake(mutex_,portMAX_DELAY);out.reserve(count_);for(size_t i=0;i<count_;++i)out.push_back(log_[(head_+log_.size()-1-i)%log_.size()]);xSemaphoreGive(mutex_);return out;
}
void EventBus::clearLog(){if(!mutex_)return;xSemaphoreTake(mutex_,portMAX_DELAY);for(auto& e:log_)e={};head_=0;count_=0;xSemaphoreGive(mutex_);}
bool EventBus::popTrap(Event& e){Queued q{};if(!queue_||xQueueReceive(queue_,&q,0)!=pdTRUE)return false;e={q.seq,q.time_ms,q.type,q.severity,q.text,q.utc};return true;}
uint32_t EventBus::queued()const{return queue_?uxQueueMessagesWaiting(queue_):0;}
}
