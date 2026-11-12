#include "harness.hpp"
#include "config_json.hpp"
#include "driver_registry.hpp"
#include <algorithm>
#include <cstring>

namespace fixture {
gateway::Config initial;
uint64_t now=1000,clock_step=0,manual_utc=0;
unsigned writes=0,commands=0,cleared_history=0,syslog_tests=0,restarts=0;
unsigned fail_error_allocations=0;
unsigned fail_config_allocations=0;
size_t history_limit=0;
ups::Snapshot snapshot;
std::vector<gateway::Event> events;
httpd_config_t server_config;
std::vector<httpd_uri_t> registered;
httpd_req_t request(httpd_method_t method,const std::string& target,const std::string& body,const std::string& cookie,const std::string& csrf,bool activity){
  httpd_req_t request;request.uri=target.c_str();request.method=method;request.body=body;request.content_len=body.size();
  if(!body.empty())request.headers["Content-Type"]="application/json";
  if(!cookie.empty())request.headers["Cookie"]=cookie;
  if(!csrf.empty())request.headers["X-CSRF-Token"]=csrf;
  if(activity)request.headers["X-Session-Activity"]="1";
  const size_t path_length=target.find('?')==std::string::npos?target.size():target.find('?');
  for(const auto& route:registered)if(route.method==method&&server_config.uri_match_fn(route.uri,target.c_str(),path_length)){
    request.user_ctx=route.user_ctx;route.handler(&request);request.uri=nullptr;return request;
  }
  request.status="404 Not Found";request.uri=nullptr;return request;
}
}
void* fixture::jsonAllocate(size_t size){
  if(size==768&&fail_error_allocations){--fail_error_allocations;return nullptr;}
  if(size==gateway::kConfigDocumentCapacity&&fail_config_allocations){--fail_config_allocations;return nullptr;}
  return malloc(size);
}
namespace {
void sendHeaders(httpd_req_t* r){if(!r->sent_headers){for(const auto& h:r->pending_headers)r->response_headers[h.first]=h.second;r->sent_headers=true;}}
}
esp_err_t httpd_start(httpd_handle_t* handle,const httpd_config_t* config){*handle=reinterpret_cast<void*>(1);fixture::server_config=*config;fixture::registered.clear();return ESP_OK;}
esp_err_t httpd_stop(httpd_handle_t){return ESP_OK;}
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t* route){if(fixture::registered.size()>=fixture::server_config.max_uri_handlers)return ESP_FAIL;fixture::registered.push_back(*route);return ESP_OK;}
size_t httpd_req_get_hdr_value_len(httpd_req_t* r,const char* name){const auto it=r->headers.find(name);return it==r->headers.end()?0:it->second.size();}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r,const char* name,char* value,size_t capacity){const auto it=r->headers.find(name);if(it==r->headers.end()||it->second.size()+1>capacity)return ESP_FAIL;std::memcpy(value,it->second.c_str(),it->second.size()+1);return ESP_OK;}
int httpd_req_to_sockfd(httpd_req_t*){return -1;}
int httpd_req_recv(httpd_req_t* r,char* buffer,size_t maximum){const size_t count=std::min(maximum,r->body.size()-r->offset);std::memcpy(buffer,r->body.data()+r->offset,count);r->offset+=count;return static_cast<int>(count);}
esp_err_t httpd_resp_set_status(httpd_req_t* r,const char* status){r->status=status;return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t* r,const char* type){r->type=type;return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t* r,const char* key,const char* value){for(auto& h:r->pending_headers)if(std::strcmp(h.first,key)==0){h.second=value;return ESP_OK;}r->pending_headers.emplace_back(key,value);return ESP_OK;}
esp_err_t httpd_resp_send(httpd_req_t* r,const char* data,size_t size){sendHeaders(r);r->response.assign(data,size);return ESP_OK;}
esp_err_t httpd_resp_send_chunk(httpd_req_t* r,const char* data,size_t size){sendHeaders(r);if(size)r->response.append(data,size);return ESP_OK;}
void esp_fill_random(void* data,size_t size){static uint32_t token=0;++token;auto* bytes=static_cast<uint8_t*>(data);for(size_t i=0;i<size;++i)bytes[i]=uint8_t((token>>((i%4)*8))^(i*33));}
uint32_t millis(){return uint32_t(fixture::now);}
void FakeESP::restart()const{++fixture::restarts;}
namespace gateway {
uint64_t uptimeMs(){const uint64_t value=fixture::now;fixture::now+=fixture::clock_step;return value;}
bool ConfigStore::begin(){saved_=fixture::initial;writable_=true;return true;}
const Config& ConfigStore::saved()const{return saved_;}
bool ConfigStore::writable()const{return writable_;}
const std::string& ConfigStore::error()const{return error_;}
std::string ConfigStore::toJson(bool secrets)const{return configJson(saved_,secrets);}
bool ConfigStore::mergeAndSave(const std::string& body,std::string& why){Config next;if(!mergeConfigJson(saved_,body,next,why))return false;saved_=std::move(next);++fixture::writes;return true;}
bool ConfigStore::resetDefaults(bool keep,std::string& why){why.clear();saved_=defaultsPreservingAccess(saved_,keep);++fixture::writes;return true;}
bool ConfigStore::resetPassword(std::string& password,std::string& why){why.clear();saved_.web_password="new-local-passphrase-123";password=saved_.web_password;++fixture::writes;return true;}
UpsService::UpsService(const Config& config,EventBus& events):config_(config),events_(events),driver_(ups::findDriver(config.driver)){}
ups::Snapshot UpsService::snapshot()const{return fixture::snapshot;}
ControlStatus UpsService::controlStatus()const{return {};}
bool UpsService::enqueue(ups::Command,const ups::CommandArgs&,uint32_t& id,std::string& why){
  ++fixture::commands;
  if(!fixture::snapshot.connected||!fixture::snapshot.status_valid||uint32_t(fixture::now-fixture::snapshot.last_success_ms)>=config_.stale_ms){why="Fresh status required by backend";return false;}
  id=fixture::commands;return true;
}
SnmpService::SnmpService(const Config& c,UpsService& u,EventBus& e,uint32_t boot):config_(c),ups_(u),events_(e),boot_id_(boot){}
SnmpStats SnmpService::stats()const{return {};}
std::vector<Event> EventBus::latest()const{return fixture::events;}
void EventBus::publish(EventType type,uint8_t severity,const std::string& message,uint64_t time){fixture::events.push_back({uint32_t(fixture::events.size()+1),time,type,severity,message});}
void EventBus::clearLog(){fixture::events.clear();}
ManagementService::ManagementService(const Config& c,UpsService& u,EventBus& e):config_(c),ups_(u),events_(e),scheduler_(c){}
std::string ManagementService::alarmsJson()const{return alarms_.json();}
std::string ManagementService::historyJson(size_t limit)const{fixture::history_limit=limit;return history_.json(config_.history_interval_s,limit);}
std::string ManagementService::historyCsv()const{return history_.csv();}
std::string ManagementService::timeJson()const{return "{\"valid\":false,\"utc\":null,\"ntp_enabled\":true,\"timezone\":\"UTC0\",\"ntp_server\":\"pool.ntp.org\"}";}
std::string ManagementService::schedulesJson()const{return "{\"rules\":[],\"clock_valid\":false}";}
std::string ManagementService::notificationsJson()const{return "{\"syslog\":{\"enabled\":false,\"delivery_confirmed\":false}}";}
bool ManagementService::setTime(uint64_t value,std::string& why){if(config_.ntp_enabled){why="Disable NTP before setting time";return false;}fixture::manual_utc=value;return true;}
void ManagementService::clearHistory(){++fixture::cleared_history;}
bool ManagementService::testSyslog(std::string& why){if(!config_.syslog_enabled){why="Syslog disabled";return false;}++fixture::syslog_tests;return true;}
}
