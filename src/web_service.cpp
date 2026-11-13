#include "web_service.hpp"
#include "web_assets.hpp"
#include "driver_registry.hpp"
#include "config_json.hpp"
#include <Arduino.h>
#include <ETH.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_image_format.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace gateway {
namespace {
class Lock {
 public:explicit Lock(SemaphoreHandle_t mutex):mutex_(mutex){xSemaphoreTake(mutex_,portMAX_DELAY);}~Lock(){xSemaphoreGive(mutex_);}
 private:SemaphoreHandle_t mutex_;
};
const esp_partition_t* firmwarePartition(){
  const auto* running=esp_ota_get_running_partition();
  const auto* next=esp_ota_get_next_update_partition(nullptr);
  const auto* data=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,nullptr);
  if(!running||!next||!data||running->type!=ESP_PARTITION_TYPE_APP||next->type!=ESP_PARTITION_TYPE_APP||
     running->subtype<ESP_PARTITION_SUBTYPE_APP_OTA_0||running->subtype>ESP_PARTITION_SUBTYPE_APP_OTA_1||
     next->subtype<ESP_PARTITION_SUBTYPE_APP_OTA_0||next->subtype>ESP_PARTITION_SUBTYPE_APP_OTA_1||
     running->address==next->address||next->size==0)return nullptr;
  return next;
}
const char* firmwareUnavailableReason(const esp_partition_t* partition){
  if(!partition)return "OTA slots are unavailable; migrate by USB.";
  const auto* running=esp_ota_get_running_partition();esp_ota_img_states_t state;
  if(!running||esp_ota_get_state_partition(running,&state)!=ESP_OK||state==ESP_OTA_IMG_UNDEFINED)return "No confirmed rollback record is available; migrate by USB.";
  if(state!=ESP_OTA_IMG_VALID)return "Running firmware is not confirmed yet.";
  return "";
}
bool firmwareHeaderValid(const uint8_t* bytes,size_t size){
  constexpr size_t header_size=24,segment_size=8,app_desc_size=256;
  if(size<header_size+segment_size+app_desc_size||bytes[0]!=0xe9||bytes[1]==0||bytes[1]>16||bytes[12]!=0||bytes[13]!=0)return false;
  const uint32_t first_segment=uint32_t(bytes[28])|(uint32_t(bytes[29])<<8)|(uint32_t(bytes[30])<<16)|(uint32_t(bytes[31])<<24);
  return first_segment>=app_desc_size&&first_segment<=size-header_size-segment_size&&
         bytes[32]==0x32&&bytes[33]==0x54&&bytes[34]==0xcd&&bytes[35]==0xab;
}
uint64_t sessionClock(void*){return uptimeMs();}
bool sessionToken(void*,std::string& token){
  uint8_t random[32];esp_fill_random(random,sizeof(random));static constexpr char hex[]="0123456789abcdef";
  token.resize(sizeof(random)*2);
  for(size_t i=0;i<sizeof(random);++i){token[i*2]=hex[random[i]>>4];token[i*2+1]=hex[random[i]&15];}
  return true;
}
std::string hexId(uint32_t value){char out[9];snprintf(out,sizeof(out),"%08lx",static_cast<unsigned long>(value));return out;}
bool header(httpd_req_t* r,const char* name,std::string& value,size_t maximum){
  value.clear();const size_t size=httpd_req_get_hdr_value_len(r,name);if(!size||size>maximum)return false;
  value.resize(size+1);
  if(httpd_req_get_hdr_value_str(r,name,&value[0],value.size())!=ESP_OK){value.clear();return false;}
  value.resize(size);return true;
}
std::string peerKey(httpd_req_t* r){
  sockaddr_in address{};socklen_t length=sizeof(address);
  if(getpeername(httpd_req_to_sockfd(r),reinterpret_cast<sockaddr*>(&address),&length)!=0||address.sin_family!=AF_INET)return "unknown";
  return std::to_string(ntohl(address.sin_addr.s_addr));
}
const char* modeName(const ups::Snapshot& state,bool fresh){
  if(!fresh)return "unknown";
  switch(state.output_mode){case ups::OutputMode::Online:return "online";case ups::OutputMode::OnBattery:return "on_battery";case ups::OutputMode::Bypass:return "bypass";case ups::OutputMode::Off:return "off";default:return "unknown";}
}
void metric(JsonObject object,const char* key,const ups::Metric& value,bool fresh){if(fresh&&value.valid&&std::isfinite(value.value))object[key]=value.value;else object[key]=nullptr;}
void flag(JsonObject object,const char* key,bool value,bool fresh){if(fresh)object[key]=value;else object[key]=nullptr;}
bool onlyKeys(JsonObjectConst object,std::initializer_list<const char*> allowed){
  for(JsonPairConst pair:object){bool found=false;for(const char* key:allowed)found|=strcmp(pair.key().c_str(),key)==0;if(!found)return false;}
  return true;
}
bool onlyQuery(const auth::RequestTarget& target,std::initializer_list<const char*> allowed){
  for(size_t i=0;i<target.count;++i){bool found=false;for(const char* key:allowed)found|=target.query[i].key==key;if(!found)return false;}return true;
}
bool queryNumber(const auth::RequestTarget& target,const char* key,size_t low,size_t high,size_t fallback,size_t& value){
  const std::string* text=target.get(key);value=fallback;if(!text)return true;
  if(text->empty()||text->size()>10)return false;
  size_t parsed=0;for(char ch:*text){if(ch<'0'||ch>'9')return false;const size_t digit=size_t(ch-'0');if(digit>high||parsed>(high-digit)/10)return false;parsed=parsed*10+digit;}
  if(parsed<low||parsed>high)return false;
  value=parsed;return true;
}
void commonHeaders(httpd_req_t* r){
  httpd_resp_set_hdr(r,"Cache-Control","no-store");httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
  httpd_resp_set_hdr(r,"X-Frame-Options","DENY");httpd_resp_set_hdr(r,"Referrer-Policy","no-referrer");
}
void unsupported(JsonArray array){
  const char* ids[]={"snmp_v3","https","smtp","radius","ssh","telnet","ftp","wap"};
  const char* labels[]={"SNMPv3","HTTPS","SMTP","RADIUS","SSH","Telnet","FTP","WAP"};
  for(size_t i=0;i<8;++i){auto item=array.createNestedObject();item["id"]=ids[i];item["label"]=labels[i];item["supported"]=false;item["reason"]="Not provided by this firmware";}
}
void permissions(JsonObject object,auth::Role role){
  const bool admin=role==auth::Role::Admin;
  object["read"]=role!=auth::Role::None;object["command"]=role==auth::Role::Operator||admin;
  for(const char* key:{"configure","export_config","clear_logs","set_time","test_notifications","reboot","reset_config"})object[key]=admin;
}
void displayPreferences(JsonObject object,const Config& config){
  object["date_format"]=config.date_format;object["temperature_unit"]=config.temperature_unit;
  object["default_page"]=config.default_page;object["timezone"]=config.timezone;object["session_timeout_min"]=config.session_timeout_min;
  auto links=object.createNestedArray("quick_links");for(const auto& link:config.quick_links){auto item=links.createNestedObject();item["title"]=link.title;item["url"]=link.url;}
}
}

WebService::WebService(const Config& config,ConfigStore& store,UpsService& ups,SnmpService& snmp,
                       EventBus& events,ManagementService& management,uint32_t boot)
    :active_(config),store_(store),ups_(ups),snmp_(snmp),events_(events),management_(management),boot_id_(boot),
     sessions_(uint64_t(config.session_timeout_min)*60000,{nullptr,sessionClock,sessionToken}){}
bool WebService::begin(){
  config_mutex_=xSemaphoreCreateMutex();auth_mutex_=xSemaphoreCreateMutex();if(!config_mutex_||!auth_mutex_)return false;
  httpd_config_t config=HTTPD_DEFAULT_CONFIG();
  config.stack_size=8192;config.max_uri_handlers=auth::routes().size();config.max_open_sockets=4;
  config.recv_wait_timeout=3;config.send_wait_timeout=3;config.lru_purge_enable=true;config.uri_match_fn=&WebService::matchUri;
  if(httpd_start(&server_,&config)!=ESP_OK)return false;
  for(const auto& route:auth::routes()){
    httpd_uri_t uri{};uri.uri=route.path;uri.method=route.method==auth::Method::Get?HTTP_GET:HTTP_POST;
    uri.handler=&WebService::dispatch;uri.user_ctx=this;
    if(httpd_register_uri_handler(server_,&uri)!=ESP_OK){httpd_stop(server_);server_=nullptr;return false;}
  }
  return true;
}
bool WebService::matchUri(const char* registered,const char* uri,size_t length){
  if(!registered||!uri||length>512)return false;
  auth::RequestTarget target;std::string why;
  return auth::parseTarget(std::string(uri,length),target,why)&&target.path==registered;
}
void WebService::tick(){
  const uint64_t when=reboot_at_.load(),now=uptimeMs();
  if(when&&now>=when&&(ups_.controlStatus().last_state!="queued"||now-when>=10000))ESP.restart();
}
bool WebService::resetPasswordLocal(std::string& password,std::string& why){
  bool ok=false;{Lock lock(config_mutex_);ok=store_.resetPassword(password,why);}
  if(ok){needs_reboot_=true;Lock lock(auth_mutex_);sessions_.invalidateAll();}return ok;
}
auth::Access WebService::authorizeRequest(httpd_req_t* r,const auth::Route& route,auth::Session& result){
  std::string cookie,csrf,activity;header(r,"Cookie",cookie,1024);header(r,"X-CSRF-Token",csrf,64);header(r,"X-Session-Activity",activity,1);
  const std::string token=auth::extractSessionCookie(cookie);Lock lock(auth_mutex_);
  const auto* session=sessions_.find(token);const bool csrf_ok=route.csrf&&sessions_.csrfValid(token,csrf);
  const auto access=auth::authorize(route,session,csrf_ok);
  if(access==auth::Access::Allowed&&session){
    if(route.method==auth::Method::Post||activity=="1")session=sessions_.find(token,true);
    if(!session&&route.minimum!=auth::Role::None)return auth::Access::Unauthenticated;
    if(session)result=*session;
  }
  return access;
}
bool WebService::readBody(httpd_req_t* r,size_t maximum,std::string& body){
  if(!r->content_len||r->content_len>maximum){error(r,"413 Payload Too Large","JSON request body is empty or exceeds the endpoint limit");return false;}
  std::string type;
  if(!header(r,"Content-Type",type,96)){error(r,"415 Unsupported Media Type","Content-Type: application/json is required");return false;}
  const size_t separator=type.find(';');if(separator!=std::string::npos)type.resize(separator);
  while(!type.empty()&&(type.back()==' '||type.back()=='\t'))type.pop_back();
  for(char& ch:type)if(ch>='A'&&ch<='Z')ch=char(ch-'A'+'a');
  if(type!="application/json"){error(r,"415 Unsupported Media Type","Content-Type: application/json is required");return false;}
  body.resize(r->content_len);size_t done=0;
  while(done<body.size()){
    const int received=httpd_req_recv(r,&body[done],body.size()-done);
    if(received<=0){error(r,"408 Request Timeout","Could not read the complete request body");return false;}done+=size_t(received);
  }
  if(!auth::isSingleJsonObject(body)){error(r,"400 Bad Request","Exactly one JSON object is required");return false;}
  return true;
}
esp_err_t WebService::json(httpd_req_t* r,const std::string& body,const char* response_status){
  if(body.empty())return error(r,"500 Internal Server Error","Could not allocate the JSON response");
  httpd_resp_set_status(r,response_status);httpd_resp_set_type(r,"application/json; charset=utf-8");commonHeaders(r);
  return httpd_resp_send(r,body.data(),body.size());
}
esp_err_t WebService::json(httpd_req_t* r,const JsonDocument& document,const char* response_status){
  if(document.overflowed())return error(r,"500 Internal Server Error","Response memory limit exceeded");
  std::string body;body.reserve(measureJson(document));serializeJson(document,body);return json(r,body,response_status);
}
esp_err_t WebService::error(httpd_req_t* r,const char* response_status,const std::string& why){
  // Keep the error document off the heap: an allocation failure in a normal
  // JSON response must not recursively try to allocate another error document.
  StaticJsonDocument<768> document;document["ok"]=false;document["error"]=why.substr(0,384);
  std::string body;serializeJson(document,body);
  httpd_resp_set_hdr(r,"Connection","close");json(r,body,response_status);
  // Some denials happen before receiving a body. Closing prevents unread bytes
  // from being interpreted as the next request on this socket.
  return ESP_FAIL;
}
esp_err_t WebService::firmwareInfo(httpd_req_t* r){
  const auto* partition=firmwarePartition();const char* reason=firmwareUnavailableReason(partition);
  StaticJsonDocument<256> document;document["available"]=!*reason;document["max_image_bytes"]=partition?partition->size:0;document["version"]=FIRMWARE_VERSION;document["reason"]=reason;
  return json(r,document);
}
esp_err_t WebService::firmwareUpload(httpd_req_t* r){
  if(reboot_at_.load())return error(r,"409 Conflict","A reboot is already scheduled");
  if(ups_.controlStatus().last_state=="queued")return error(r,"409 Conflict","Wait until the pending UPS command has been transmitted");
  const auto* partition=firmwarePartition();const char* reason=firmwareUnavailableReason(partition);
  if(*reason)return error(r,"409 Conflict",reason);
  if(!r->content_len||r->content_len>partition->size)return error(r,"413 Payload Too Large","Firmware image is empty or exceeds the update partition");
  std::string value;
  if(!header(r,"X-Firmware-Confirm",value,16)||value!="UPDATE")return error(r,"400 Bad Request","UPDATE confirmation is required");
  if(!header(r,"Content-Type",value,96))return error(r,"415 Unsupported Media Type","Content-Type: application/octet-stream is required");
  const auto separator=value.find(';');if(separator!=std::string::npos)value.resize(separator);
  while(!value.empty()&&(value.back()==' '||value.back()=='\t'))value.pop_back();
  for(char& ch:value)if(ch>='A'&&ch<='Z')ch=char(ch-'A'+'a');
  if(value!="application/octet-stream")return error(r,"415 Unsupported Media Type","Content-Type: application/octet-stream is required");
  constexpr size_t prefix_size=24+8+256;
  if(r->content_len<prefix_size)return error(r,"400 Bad Request","Firmware image header is incomplete");
  uint8_t buffer[4096];size_t received=0;
  while(received<prefix_size){
    const int count=httpd_req_recv(r,reinterpret_cast<char*>(buffer+received),prefix_size-received);
    if(count<=0)return error(r,"408 Request Timeout","Could not read the complete firmware image");
    received+=size_t(count);
  }
  if(!firmwareHeaderValid(buffer,r->content_len))return error(r,"400 Bad Request","Invalid ESP32 application image header");
  esp_ota_handle_t update;
  if(esp_ota_begin(partition,r->content_len,&update)!=ESP_OK)return error(r,"500 Internal Server Error","Could not begin firmware update");
  if(esp_ota_write(update,buffer,received)!=ESP_OK){esp_ota_abort(update);return error(r,"500 Internal Server Error","Could not write firmware image");}
  while(received<r->content_len){
    const size_t wanted=std::min(sizeof(buffer),r->content_len-received);
    const int count=httpd_req_recv(r,reinterpret_cast<char*>(buffer),wanted);
    if(count<=0){esp_ota_abort(update);return error(r,"408 Request Timeout","Could not read the complete firmware image");}
    if(esp_ota_write(update,buffer,size_t(count))!=ESP_OK){esp_ota_abort(update);return error(r,"500 Internal Server Error","Could not write firmware image");}
    received+=size_t(count);
  }
  if(esp_ota_end(update)!=ESP_OK)return error(r,"400 Bad Request","Firmware image integrity validation failed");
  esp_partition_pos_t position{};position.offset=partition->address;position.size=partition->size;
  esp_image_metadata_t metadata{};
  if(esp_image_verify(ESP_IMAGE_VERIFY_SILENT,&position,&metadata)!=ESP_OK||metadata.image_len!=r->content_len)return error(r,"400 Bad Request","Firmware image size or integrity validation failed");
  if(esp_ota_set_boot_partition(partition)!=ESP_OK)return error(r,"500 Internal Server Error","Could not select firmware boot partition");
  reboot_at_=uptimeMs()+1500;
  return json(r,"{\"ok\":true,\"rebooting\":true}");
}
esp_err_t WebService::dispatch(httpd_req_t* r){return static_cast<WebService*>(r->user_ctx)->handle(r);}

esp_err_t WebService::sessionResponse(httpd_req_t* r,const auth::Session* session){
  DynamicJsonDocument document(6144);document["authenticated"]=session!=nullptr;document["firmware"]=FIRMWARE_VERSION;document["boot_id"]=hexId(boot_id_);
  if(session){
    document["username"]=session->username;document["role"]=auth::roleName(session->role);document["csrf"]=session->csrf;
    permissions(document.createNestedObject("permissions"),session->role);
    auto drivers=document.createNestedArray("drivers");
    for(const auto* driver:ups::drivers()){auto item=drivers.createNestedObject();item["id"]=driver->info().id;item["label"]=driver->info().label;item["capabilities"]=driver->info().capabilities;}
    displayPreferences(document.createNestedObject("display"),active_);displayPreferences(document.as<JsonObject>(),active_);
  }
  return json(r,document);
}
esp_err_t WebService::status(httpd_req_t* r){
  DynamicJsonDocument document(6144);
  document["uptime_s"]=uptimeMs()/1000;document["firmware"]=FIRMWARE_VERSION;document["hostname"]=active_.hostname;
  document["ip"]=ETH.localIP().toString();document["mac"]=ETH.macAddress();document["link_up"]=ETH.linkUp();
  document["free_heap"]=ESP.getFreeHeap();document["flash_bytes"]=ESP.getFlashChipSize();document["needs_reboot"]=needs_reboot_.load();
  {Lock lock(config_mutex_);document["config_ok"]=store_.writable();document["config_error"]=store_.error();}
  auto thresholds=document.createNestedObject("thresholds");thresholds["alarm_load_high"]=active_.alarm_load_high;thresholds["alarm_input_low"]=active_.alarm_input_low;thresholds["alarm_input_high"]=active_.alarm_input_high;
  thresholds["alarm_temperature_high"]=active_.alarm_temperature_high;thresholds["alarm_delay_s"]=active_.alarm_delay_s;
  const auto state=ups_.snapshot();const uint32_t age=uint32_t(millis()-state.last_success_ms);
  const bool fresh=state.connected&&state.status_valid&&state.successful_polls&&age<active_.stale_ms;
  auto ups=document.createNestedObject("ups");ups["connected"]=fresh;ups["status_valid"]=fresh;ups["stale_ms"]=active_.stale_ms;
  if(state.successful_polls)ups["age_ms"]=age;else ups["age_ms"]=nullptr;
  ups["driver"]=active_.driver;ups["manufacturer"]=state.manufacturer;ups["model"]=state.model;ups["firmware"]=state.firmware;ups["serial"]=state.serial;
  metric(ups,"input_voltage",state.input_voltage,fresh);metric(ups,"input_fault_voltage",state.input_fault_voltage,fresh);metric(ups,"output_voltage",state.output_voltage,fresh);metric(ups,"load_percent",state.load_percent,fresh);
  metric(ups,"input_frequency",state.input_frequency,fresh);metric(ups,"output_frequency",state.output_frequency,fresh);
  metric(ups,"battery_voltage_raw",state.battery_voltage_raw,fresh);metric(ups,"battery_voltage",state.battery_voltage,fresh);
  metric(ups,"battery_charge",state.battery_charge,fresh);metric(ups,"runtime_seconds",state.runtime_seconds,fresh);
  metric(ups,"ups_temperature",state.ups_temperature,fresh);metric(ups,"battery_temperature",state.battery_temperature,fresh);
  metric(ups,"nominal_voltage",state.nominal_voltage,fresh);metric(ups,"nominal_current",state.nominal_current,fresh);metric(ups,"nominal_battery_voltage",state.nominal_battery_voltage,fresh);metric(ups,"nominal_frequency",state.nominal_frequency,fresh);
  ups["output_mode"]=modeName(state,fresh);flag(ups,"mains_failed",state.mains_failed,fresh);flag(ups,"low_battery",state.low_battery,fresh);flag(ups,"ups_fault",state.ups_fault,fresh);
  flag(ups,"bypass_or_avr",state.bypass_or_avr,fresh);flag(ups,"standby_topology",state.standby_topology,fresh);
  flag(ups,"test_running",state.test_running,fresh);flag(ups,"shutdown_active",state.shutdown_active,fresh);flag(ups,"beeper_on",state.beeper_on,fresh);
  if(fresh){ups["raw_status"]=state.raw_status;ups["last_status_line"]=state.last_status_line;}else{ups["raw_status"]=nullptr;ups["last_status_line"]=nullptr;}
  ups["successful_polls"]=state.successful_polls;ups["errors"]=state.errors;
  const auto control=ups_.controlStatus();auto ctrl=document.createNestedObject("control");
  ctrl["enabled"]=active_.controls_enabled;ctrl["power_enabled"]=active_.power_controls_enabled;
  ctrl["last_id"]=control.last_id;ctrl["last_command"]=control.last_command;ctrl["last_state"]=control.last_state;ctrl["last_message"]=control.last_message;
  const auto traps=snmp_.stats();auto trap=document.createNestedObject("traps");trap["sent"]=traps.sent;trap["failed"]=traps.failed;trap["dropped"]=traps.dropped;trap["queued"]=traps.queued;
  return json(r,document);
}
esp_err_t WebService::eventsResponse(httpd_req_t* r,const auth::RequestTarget& target,bool csv){
  size_t severity=1,limit=48;
  if(!onlyQuery(target,{"min_severity","contains","limit"})||!queryNumber(target,"min_severity",1,3,1,severity)||!queryNumber(target,"limit",1,48,48,limit))return error(r,"400 Bad Request","Invalid event filter; severity is 1..3 and limit is 1..48");
  const std::string* contains=target.get("contains");const auto snapshot=events_.latest();
  commonHeaders(r);httpd_resp_set_type(r,csv?"text/csv; charset=utf-8":"application/json; charset=utf-8");
  if(csv)httpd_resp_set_hdr(r,"Content-Disposition","attachment; filename=ups-gateway-events.csv");
  const std::string prefix=csv?"seq,uptime_seconds,utc_unix_seconds,type,severity,message\r\n":"{\"boot_id\":\""+hexId(boot_id_)+"\",\"capacity\":"+std::to_string(events_.capacity())+",\"events\":[";
  if(httpd_resp_send_chunk(r,prefix.data(),prefix.size())!=ESP_OK)return ESP_FAIL;
  size_t count=0;DynamicJsonDocument row_document(768);
  for(const auto& event:snapshot){
    if(event.severity<severity||(contains&&!contains->empty()&&event.message.find(*contains)==std::string::npos&&std::string(eventName(event.type)).find(*contains)==std::string::npos))continue;
    if(count==limit)break;
    std::string row;
    if(csv){
      row=std::to_string(event.seq)+","+std::to_string(event.uptime_ms/1000)+","+(event.utc?std::to_string(event.utc):"")+","+auth::csvCell(eventName(event.type))+","+std::to_string(event.severity)+","+auth::csvCell(event.message)+"\r\n";
    }else{
      row_document.clear();row_document["seq"]=event.seq;row_document["uptime_s"]=event.uptime_ms/1000;
      if(event.utc)row_document["utc"]=event.utc;else row_document["utc"]=nullptr;
      row_document["type"]=eventName(event.type);row_document["severity"]=event.severity;row_document["message"]=event.message;
      if(row_document.overflowed())return ESP_FAIL;
      if(count)row=",";
      serializeJson(row_document,row);
    }
    if(httpd_resp_send_chunk(r,row.data(),row.size())!=ESP_OK)return ESP_FAIL;
    ++count;
  }
  if(!csv){const std::string suffix="],\"count\":"+std::to_string(count)+"}";if(httpd_resp_send_chunk(r,suffix.data(),suffix.size())!=ESP_OK)return ESP_FAIL;}
  return httpd_resp_send_chunk(r,nullptr,0);
}
esp_err_t WebService::capabilities(httpd_req_t* r){
  DynamicJsonDocument document(8192);document["product"]="UPS Gateway";document["firmware"]=FIRMWARE_VERSION;
  document["controls_enabled"]=active_.controls_enabled;document["power_controls_enabled"]=active_.power_controls_enabled;document.createNestedArray("outlet_groups");
  const auto* selected=ups_.driver();const uint32_t mask=selected?selected->info().capabilities:0;auto driver=document.createNestedObject("driver");
  driver["id"]=active_.driver;driver["label"]=selected?selected->info().label:"Unavailable";driver["capabilities"]=mask;
  const char* names[]={"status","commands","alarms","events","history","csv","schedules","time","syslog","snmp_v2c","traps","config","sessions"};
  const char* labels[]={"UPS status","UPS commands","Alarm status","Event journal","Data history","CSV export","Schedules","NTP / manual clock","UDP syslog","SNMPv2c","SNMP notifications","Configuration backup / import","Login sessions and roles"};
  auto features=document.createNestedArray("features");
  for(size_t i=0;i<13;++i){auto item=features.createNestedObject();item["id"]=names[i];item["label"]=labels[i];item["supported"]=true;item["detail"]=i==1?"Driver capability, fresh status and control opt-ins required":i==4?"240 samples in volatile RAM":i==6?"Saved rules apply after reboot; no overdue catch-up":i==8?"Bounded UDP queue; delivery is not confirmed":"Implemented by UPS Gateway";}
  auto commands=document.createNestedArray("commands");
  for(const auto& info:ups::kCommands){auto item=commands.createNestedObject();item["id"]=info.name;item["label"]=info.label;item["capability"]=info.capability;item["power"]=info.power;item["supported"]=(mask&info.capability)!=0;}
  unsupported(document.createNestedArray("unsupported_services"));return json(r,document);
}
esp_err_t WebService::about(httpd_req_t* r){
  DynamicJsonDocument document(6144);document["product"]="UPS Gateway";document["firmware"]=FIRMWARE_VERSION;document["boot_id"]=hexId(boot_id_);
  document["uptime_s"]=uptimeMs()/1000;document["needs_reboot"]=needs_reboot_.load();
  auto hardware=document.createNestedObject("hardware");hardware["board"]="Olimex ESP32-POE WROOM + MOD-RS232";hardware["chip"]=ESP.getChipModel();hardware["revision"]=ESP.getChipRevision();
  hardware["cpu_mhz"]=ESP.getCpuFreqMHz();hardware["flash_bytes"]=ESP.getFlashChipSize();hardware["mac"]=ETH.macAddress();
  auto memory=document.createNestedObject("memory");memory["free_heap"]=ESP.getFreeHeap();memory["min_free_heap"]=ESP.getMinFreeHeap();
  auto storage=document.createNestedObject("storage");storage["partition"]="appcfg";storage["offset"]=0xF00000;storage["size"]=0x10000;
  const auto* running=esp_ota_get_running_partition();
  storage["firmware_slot"]=running?running->label:"unknown";storage["firmware_offset"]=running?running->address:0;storage["firmware_size"]=running?running->size:0;storage["schema"]=active_.schema;
  {Lock lock(config_mutex_);storage["config_ok"]=store_.writable();storage["config_error"]=store_.error();}
  auto network=document.createNestedObject("network");network["ip"]=ETH.localIP().toString();network["link_up"]=ETH.linkUp();network["http_port"]=80;
  auto protocol=document.createNestedObject("protocol");protocol["driver"]=active_.driver;protocol["baud"]=active_.baud;protocol["tx_pin"]=UPS_UART_TX;protocol["rx_pin"]=UPS_UART_RX;
  protocol["commands_acknowledged"]=false;
  unsupported(document.createNestedArray("unsupported_services"));return json(r,document);
}

esp_err_t WebService::handle(httpd_req_t* r){
  auth::RequestTarget target;std::string why;if(!auth::parseTarget(r->uri,target,why))return error(r,"400 Bad Request",why);
  if(r->method!=HTTP_GET&&r->method!=HTTP_POST)return error(r,"404 Not Found","Unknown route");
  const auto method=r->method==HTTP_GET?auth::Method::Get:auth::Method::Post;const auto* route=auth::routeFor(method,target.path);
  if(!route)return error(r,"404 Not Found","Unknown route");
  if(method==auth::Method::Get&&r->content_len)return error(r,"400 Bad Request","GET requests cannot contain a body");
  if(route->id!=auth::Endpoint::Events&&route->id!=auth::Endpoint::EventsCsv&&route->id!=auth::Endpoint::History&&target.count)return error(r,"400 Bad Request","This endpoint does not accept query parameters");
  auth::Session session;const auto access=authorizeRequest(r,*route,session);
  if(access==auth::Access::Unauthenticated)return error(r,"401 Unauthorized","Login required or session expired");
  if(access==auth::Access::Forbidden)return error(r,"403 Forbidden","This account does not have permission for this operation");
  if(access==auth::Access::InvalidCsrf)return error(r,"403 Forbidden","Invalid CSRF token; log in again if the session expired");
  if(method==auth::Method::Get){
    switch(route->id){
      case auth::Endpoint::Root:
        httpd_resp_set_type(r,"text/html; charset=utf-8");httpd_resp_set_hdr(r,"Content-Encoding","gzip");commonHeaders(r);
        httpd_resp_set_hdr(r,"Content-Security-Policy","default-src 'self'; img-src 'self' data:; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
        return httpd_resp_send(r,reinterpret_cast<const char*>(WEB_GZIP),WEB_GZIP_SIZE);
      case auth::Endpoint::Session:return sessionResponse(r,session.role==auth::Role::None?nullptr:&session);
      case auth::Endpoint::Status:return status(r);
      case auth::Endpoint::Config:case auth::Endpoint::ConfigExport:{
        Lock lock(config_mutex_);const bool secrets=route->id==auth::Endpoint::ConfigExport;
        if(secrets)httpd_resp_set_hdr(r,"Content-Disposition","attachment; filename=ups-gateway-config.json");
        return json(r,store_.toJson(secrets));
      }
      case auth::Endpoint::Events:case auth::Endpoint::EventsCsv:return eventsResponse(r,target,route->id==auth::Endpoint::EventsCsv);
      case auth::Endpoint::Capabilities:return capabilities(r);
      case auth::Endpoint::Alarms:return json(r,management_.alarmsJson());
      case auth::Endpoint::History:{
        size_t limit;if(!onlyQuery(target,{"limit"})||!queryNumber(target,"limit",1,240,240,limit))return error(r,"400 Bad Request","History limit must be 1..240");
        return json(r,management_.historyJson(limit));
      }
      case auth::Endpoint::HistoryCsv:{
        const std::string csv=management_.historyCsv();httpd_resp_set_type(r,"text/csv; charset=utf-8");commonHeaders(r);
        httpd_resp_set_hdr(r,"Content-Disposition","attachment; filename=ups-gateway-history.csv");return httpd_resp_send(r,csv.data(),csv.size());
      }
      case auth::Endpoint::Time:return json(r,management_.timeJson());
      case auth::Endpoint::Schedules:return json(r,management_.schedulesJson());
      case auth::Endpoint::Notifications:return json(r,management_.notificationsJson());
      case auth::Endpoint::About:return about(r);
      case auth::Endpoint::Firmware:return firmwareInfo(r);
      default:return error(r,"404 Not Found","Unknown route");
    }
  }
  if(route->id==auth::Endpoint::Firmware)return firmwareUpload(r);
  std::string body;if(!readBody(r,route->max_body,body))return ESP_FAIL;
  if(route->id==auth::Endpoint::Config){
    Lock lock(config_mutex_);Config candidate;
    // Preview validation releases its large JSON document before persistence.
    // The filtered document below contains only the transient confirmation.
    if(!mergeConfigJson(store_.saved(),body,candidate,why))return error(r,"400 Bad Request",why);
    StaticJsonDocument<64> filter;filter["confirm"]=true;DynamicJsonDocument confirmation(256);
    const auto parsed=deserializeJson(confirmation,body,DeserializationOption::Filter(filter));
    if(parsed||confirmation.overflowed()||(confirmation.containsKey("confirm")&&(!confirmation["confirm"].is<const char*>()||confirmation["confirm"].as<std::string>()!="POWER")))return error(r,"400 Bad Request","Transient confirm must be the string POWER");
    if(auth::powerConfirmationRequired(store_.saved(),candidate)&&confirmation["confirm"].as<std::string>()!="POWER")return error(r,"400 Bad Request","Enabling or changing a power schedule requires explicit POWER confirmation");
    // ConfigStore serializes known fields only; confirm is never persisted.
    if(!store_.mergeAndSave(body,why))return error(r,"400 Bad Request",why);
    needs_reboot_=true;return json(r,"{\"ok\":true,\"reboot_required\":true}");
  }
  DynamicJsonDocument document(4096);const auto parsed=deserializeJson(document,body);
  if(parsed||!document.is<JsonObject>())return error(r,"400 Bad Request","Invalid JSON object or request memory limit exceeded");
  const auto object=document.as<JsonObjectConst>();
  switch(route->id){
    case auth::Endpoint::Login:{
      if(!onlyKeys(object,{"username","password"})||!document["username"].is<const char*>()||!document["password"].is<const char*>())return error(r,"400 Bad Request","Username and password are required");
      const std::array<auth::Account,3> accounts{{
        {true,auth::Role::Admin,active_.web_user,active_.web_password},
        {active_.operator_enabled,auth::Role::Operator,active_.operator_user,active_.operator_password},
        {active_.readonly_enabled,auth::Role::Viewer,active_.readonly_user,active_.readonly_password}
      }};
      const JsonString username=document["username"].as<JsonString>(),password=document["password"].as<JsonString>();auth::LoginResult result;
      {Lock lock(auth_mutex_);result=sessions_.login(accounts,std::string_view(username.c_str(),username.size()),std::string_view(password.c_str(),password.size()),peerKey(r),session);}
      if(result.state==auth::LoginState::Throttled){const std::string retry=std::to_string(result.retry_after_s);httpd_resp_set_hdr(r,"Retry-After",retry.c_str());return error(r,"429 Too Many Requests","Too many login attempts; retry after the indicated delay");}
      if(result.state==auth::LoginState::InvalidCredentials)return error(r,"401 Unauthorized","Invalid username or password");
      if(result.state==auth::LoginState::Capacity)return error(r,"503 Service Unavailable","Session limit reached; log out another session or wait for its idle timeout");
      if(result.state!=auth::LoginState::Ok)return error(r,"503 Service Unavailable","Could not create a secure session");
      const std::string cookie="ups_session="+session.token+"; Path=/; HttpOnly; SameSite=Strict";httpd_resp_set_hdr(r,"Set-Cookie",cookie.c_str());
      return sessionResponse(r,&session);
    }
    case auth::Endpoint::Logout:
      if(!onlyKeys(object,{}))return error(r,"400 Bad Request","Logout expects an empty JSON object");
      {Lock lock(auth_mutex_);sessions_.logout(session.token);}
      httpd_resp_set_hdr(r,"Set-Cookie","ups_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");return json(r,"{\"ok\":true,\"authenticated\":false}");
    case auth::Endpoint::Reboot:
      if(!onlyKeys(object,{"confirm"})||!document["confirm"].is<const char*>()||document["confirm"].as<std::string>()!="REBOOT")return error(r,"400 Bad Request","REBOOT confirmation is required");
      if(ups_.controlStatus().last_state=="queued")return error(r,"409 Conflict","Wait until the pending UPS command has been transmitted");
      reboot_at_=uptimeMs()+1500;return json(r,"{\"ok\":true}");
    case auth::Endpoint::TrapTest:{
      if(!onlyKeys(object,{}))return error(r,"400 Bad Request","Trap test expects an empty JSON object");
      bool enabled=false;for(const auto& target:active_.traps)enabled|=target.enabled;
      if(!enabled)return error(r,"400 Bad Request","Configure a trap recipient and reboot first");
      events_.publish(EventType::TestTrap);return json(r,"{\"ok\":true,\"state\":\"queued\",\"delivery_confirmed\":false}","202 Accepted");
    }
    case auth::Endpoint::SyslogTest:
      if(!onlyKeys(object,{}))return error(r,"400 Bad Request","Syslog test expects an empty JSON object");
      if(!management_.testSyslog(why))return error(r,"409 Conflict",why);
      return json(r,"{\"ok\":true,\"state\":\"queued\",\"delivery_confirmed\":false}","202 Accepted");
    case auth::Endpoint::Time:
      if(!onlyKeys(object,{"utc"})||!document["utc"].is<uint64_t>())return error(r,"400 Bad Request","UTC must be an integer Unix timestamp");
      if(!management_.setTime(document["utc"].as<uint64_t>(),why))return error(r,active_.ntp_enabled?"409 Conflict":"400 Bad Request",why);
      return json(r,"{\"ok\":true}");
    case auth::Endpoint::LogsClear:{
      if(!onlyKeys(object,{"target","confirm"})||!document["target"].is<const char*>()||!document["confirm"].is<const char*>()||document["confirm"].as<std::string>()!="CLEAR_LOG")return error(r,"400 Bad Request","Target and CLEAR_LOG confirmation are required");
      const std::string target=document["target"].as<std::string>();
      if(target=="events")events_.clearLog();else if(target=="history")management_.clearHistory();else return error(r,"400 Bad Request","Target must be events or history");
      return json(r,"{\"ok\":true}");
    }
    case auth::Endpoint::ResetConfig:{
      if(!onlyKeys(object,{"confirm","keep_network"})||!document["confirm"].is<const char*>()||document["confirm"].as<std::string>()!="RESET_CONFIG"||!document["keep_network"].is<bool>())return error(r,"400 Bad Request","RESET_CONFIG confirmation and keep_network boolean are required");
      const bool keep=document["keep_network"].as<bool>();{Lock lock(config_mutex_);if(!store_.resetDefaults(keep,why))return error(r,"400 Bad Request",why);}
      needs_reboot_=true;return json(r,"{\"ok\":true,\"reboot_required\":true,\"access_preserved\":true}");
    }
    case auth::Endpoint::Command:{
      if(!onlyKeys(object,{"command","confirm","delay_minutes","delay_seconds","restore_minutes"})||!document["command"].is<const char*>()||!document["confirm"].is<const char*>())return error(r,"400 Bad Request","Command and confirmation are required");
      const auto* command=ups::findCommand(document["command"].as<std::string>());
      if(!command)return error(r,"400 Bad Request","Unknown UPS command");
      if(document["confirm"].as<std::string>()!=(command->power?"POWER":"EXECUTE"))return error(r,"400 Bad Request","Command confirmation does not match the operation");
      if(!active_.controls_enabled||(command->power&&!active_.power_controls_enabled))return error(r,"403 Forbidden","This category of UPS controls is disabled");
      if(document.containsKey("delay_minutes")&&document.containsKey("delay_seconds"))return error(r,"400 Bad Request","Choose either minutes or seconds");
      ups::CommandArgs args;
      for(const char* key:{"delay_minutes","delay_seconds","restore_minutes"})if(document.containsKey(key)&&(!document[key].is<uint32_t>()||document[key].as<uint32_t>()>9999))return error(r,"400 Bad Request","Invalid command delay");
      if(document.containsKey("delay_seconds")&&document["delay_seconds"].as<uint32_t>()==0)return error(r,"400 Bad Request","Seconds delay must be non-zero");
      if(document.containsKey("delay_minutes"))args.delay_minutes=document["delay_minutes"].as<uint16_t>();
      if(document.containsKey("delay_seconds"))args.delay_seconds=document["delay_seconds"].as<uint16_t>();
      if(document.containsKey("restore_minutes"))args.restore_minutes=document["restore_minutes"].as<uint16_t>();
      uint32_t id=0;if(!ups_.enqueue(command->command,args,id,why))return error(r,"409 Conflict",why);
      DynamicJsonDocument result(256);result["ok"]=true;result["id"]=id;result["state"]="queued";return json(r,result,"202 Accepted");
    }
    default:return error(r,"404 Not Found","Unknown route");
  }
}
}
