#include "harness.hpp"
#include <esp_ota_ops.h>
#include <iostream>

namespace gateway { void checkFirmwareProbation(uint64_t now_ms,bool services_ready,bool ethernet_link,bool has_ip); }

namespace {
int failures=0;
void check(bool ok,const char* message){if(!ok){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
int code(const httpd_req_t& response){return std::stoi(response.status.substr(0,3));}
DynamicJsonDocument payload(const httpd_req_t& response){DynamicJsonDocument document(32768);const auto parsed=deserializeJson(document,response.response);check(!parsed,"HTTP JSON response parses");return document;}
gateway::Config configuration(){
  gateway::Config config;config.web_user="admin";config.web_password="admin-passphrase-123";config.operator_enabled=true;config.operator_password="operator-passphrase-123";
  config.readonly_enabled=true;config.readonly_password="viewer-passphrase-123";config.session_timeout_min=1;config.snmp_community="private-community";
  for(auto& target:config.traps)target.community="private-trap-community";
  config.controls_enabled=true;config.power_controls_enabled=true;config.syslog_enabled=true;config.syslog_host="192.0.2.20";return config;
}
struct Device {
  gateway::Config active{configuration()};gateway::ConfigStore store;gateway::EventBus events;
  gateway::UpsService ups{active,events};gateway::SnmpService snmp{active,ups,events,0x1234};
  gateway::ManagementService management{active,ups,events};gateway::WebService web{active,store,ups,snmp,events,management,0x1234};
  Device(){
    fixture::initial=active;fixture::now=1000;fixture::clock_step=0;fixture::writes=0;fixture::commands=0;fixture::cleared_history=0;fixture::syslog_tests=0;fixture::restarts=0;fixture::events.clear();fixture::control={};fixture::persistence={true,"",0,3825,0,0};fixture::fail_log_clear=fixture::fail_log_flush=fixture::fail_history_read=false;fixture::log_flushes=0;fixture::lifecycle.clear();
    fixture::ota_begins=fixture::ota_writes=fixture::ota_ends=fixture::ota_aborts=fixture::ota_switches=0;
    fixture::ota_available=true;fixture::ota_fail_begin=fixture::ota_fail_write=fixture::ota_fail_end=fixture::ota_fail_switch=fixture::ota_fail_verify=false;
    fixture::ota_limit=0x3f0000;fixture::ota_max_chunk=0;fixture::ota_verified_length=static_cast<size_t>(-1);fixture::ota_written.clear();
    fixture::ota_running_state=ESP_OTA_IMG_VALID;fixture::ota_running_second=false;fixture::ota_state_available=true;fixture::ota_mark_valid_fails=false;fixture::ota_mark_valid=fixture::ota_mark_invalid=0;
    fixture::snapshot={};fixture::snapshot.connected=true;fixture::snapshot.status_valid=true;fixture::snapshot.successful_polls=1;fixture::snapshot.last_success_ms=1000;fixture::snapshot.load_percent={37,true};
    check(store.begin()&&web.begin(),"WebService starts with enough URI slots");
  }
};
struct Login {std::string cookie,csrf;};
Login login(const std::string& username){
  const auto response=fixture::request(HTTP_POST,"/api/login","{\"username\":\""+username+"\",\"password\":\""+username+"-passphrase-123\"}");
  check(code(response)==200,"login returns success");const auto data=payload(response);
  check(data["authenticated"].as<bool>(),"login returns authenticated session");
  const auto it=response.response_headers.find("Set-Cookie");check(it!=response.response_headers.end(),"login issues cookie");
  if(it==response.response_headers.end())return {};
  check(it->second.find("HttpOnly")!=std::string::npos&&it->second.find("SameSite=Strict")!=std::string::npos,"session cookie prevents script reads and cross-site use");
  check(!data.containsKey("password")&&!data.containsKey("web_password"),"session never returns account passwords");
  return {it->second.substr(0,it->second.find(';')),data["csrf"].as<std::string>()};
}
void allocation_faults(){
  fixture::fail_error_allocations=1;
  DynamicJsonDocument failed(768);
  check(failed.capacity()==0&&fixture::fail_error_allocations==0,"768-byte JSON allocation fault is injected");
  DynamicJsonDocument recovered(768);
  check(recovered.capacity()>=768,"JSON allocation recovers after an injected failure");
}
void public_and_roles(){
  Device device;
  auto response=fixture::request(HTTP_GET,"/");check(code(response)==200&&response.type=="text/html; charset=utf-8","public shell loads without Basic authentication");
  check(response.response_headers["Content-Security-Policy"].find("img-src 'self' data:")!=std::string::npos,"reference data images are allowed by CSP");
  response=fixture::request(HTTP_GET,"/api/session");const auto anonymous=payload(response);
  check(code(response)==200&&!anonymous["authenticated"].as<bool>()&&!anonymous.containsKey("csrf")&&!anonymous.containsKey("username"),"anonymous session has no identity or token");
  check(code(fixture::request(HTTP_GET,"/api/status"))==401,"status requires login");
  fixture::fail_error_allocations=1;
  check(code(fixture::request(HTTP_GET,"/api/status"))==401,"an error-document allocation failure cannot recurse or replace an authorization denial");
  fixture::fail_error_allocations=0;
  const auto viewer=login("viewer"),op=login("operator"),admin=login("admin");
  for(const char* path:{"/api/status","/api/capabilities","/api/alarms","/api/history","/api/history.csv","/api/events","/api/events.csv","/api/time","/api/schedules","/api/notifications","/api/about"}){
    response=fixture::request(HTTP_GET,path,"",viewer.cookie);check(code(response)==200,"viewer can use every read-only management endpoint");
  }
  response=fixture::request(HTTP_GET,"/api/capabilities","",viewer.cookie);const auto capabilities=payload(response);
  check(capabilities["commands"].size()==8 && capabilities["driver"]["capabilities"].as<unsigned>()==255,"capabilities include all eight MegaTec commands");
  bool deep=false,timed=false;for(const auto item:capabilities["commands"].as<JsonArrayConst>()){
    const auto id=item["id"].as<std::string>();
    if(id=="test_deep")deep=item["capability"].as<unsigned>()==64&&!item["power"].as<bool>()&&item["supported"].as<bool>();
    if(id=="test_timed")timed=item["capability"].as<unsigned>()==128&&!item["power"].as<bool>()&&item["supported"].as<bool>();
  }
  check(deep&&timed,"deep and timed test capabilities are supported and non-power");
  response=fixture::request(HTTP_GET,"/api/about","",viewer.cookie);
  const auto about=payload(response);check(!about.containsKey("attribution")&&!about["protocol"].containsKey("compatibility"),"about omits removed attribution and compatibility notices");
  check(about["protocol"].containsKey("commands_acknowledged")&&!about["protocol"]["commands_acknowledged"].as<bool>(),"about retains command acknowledgement state");
  for(const auto* account:{&viewer,&op})for(const char* path:{"/api/config","/api/config/export"}){
    response=fixture::request(HTTP_GET,path,"",account->cookie);check(code(response)==403,"viewer/operator cannot read configuration or secret export");
    check(response.response.find("admin-passphrase-123")==std::string::npos,"denied export never leaks a secret");
  }
  for(const auto* account:{&viewer,&op})for(const char* path:{"/api/config","/api/trap-test","/api/syslog-test","/api/reboot","/api/logs/clear","/api/time","/api/reset-config"}){
    response=fixture::request(HTTP_POST,path,"{}",account->cookie,account->csrf);check(code(response)==403,"lesser roles cannot mutate administrator endpoints");
  }
  check(fixture::writes==0&&fixture::syslog_tests==0&&fixture::manual_utc==0,"denied administrator requests have no side effects");
  const std::string command="{\"command\":\"test_short\",\"confirm\":\"EXECUTE\"}";
  check(code(fixture::request(HTTP_POST,"/api/command",command,viewer.cookie,viewer.csrf))==403&&fixture::commands==0,"viewer command denial never reaches UPS backend");
  check(code(fixture::request(HTTP_POST,"/api/command",command,op.cookie,admin.csrf))==403&&fixture::commands==0,"another session's CSRF cannot submit a command");
  check(code(fixture::request(HTTP_POST,"/api/command",command,op.cookie,op.csrf))==202&&fixture::commands==1,"operator can enqueue permitted UPS command");
  check(code(fixture::request(HTTP_POST,"/api/command","{\"command\":\"output_off\",\"confirm\":\"EXECUTE\"}",op.cookie,op.csrf))==400&&fixture::commands==1,"power operation requires POWER before backend dispatch");
  check(code(fixture::request(HTTP_POST,"/api/command","{\"command\":\"output_off\",\"confirm\":\"POWER\",\"delay_minutes\":1,\"delay_seconds\":12}",op.cookie,op.csrf))==400&&fixture::commands==1,"shutdown units are mutually exclusive before dispatch");
  check(code(fixture::request(HTTP_POST,"/api/command","{\"command\":\"output_off\",\"confirm\":\"POWER\",\"delay_seconds\":0}",op.cookie,op.csrf))==400&&fixture::commands==1,"explicit zero seconds cannot become a one-minute shutdown");
  check(code(fixture::request(HTTP_POST,"/api/command","{\"command\":\"test_short\",\"confirm\":\"EXECUTE\",\"delay_seconds\":0}",op.cookie,op.csrf))==400&&fixture::commands==1,"explicit zero seconds cannot be used on a non-shutdown command");
  check(code(fixture::request(HTTP_POST,"/api/command","{\"command\":\"output_off\",\"confirm\":\"POWER\",\"delay_seconds\":12}",op.cookie,op.csrf))==202&&fixture::commands==2&&fixture::last_command==ups::Command::OutputOff&&fixture::last_args.delay_seconds==12,"shutdown seconds reach the UPS backend intact");
  check(code(fixture::request(HTTP_POST,"/api/command","{\"command\":\"test_deep\",\"confirm\":\"EXECUTE\"}",op.cookie,op.csrf))==202&&fixture::commands==3,"deep test uses EXECUTE confirmation");
  check(code(fixture::request(HTTP_POST,"/api/command","{\"command\":\"test_timed\",\"confirm\":\"POWER\"}",op.cookie,op.csrf))==400&&fixture::commands==3,"timed test rejects power confirmation");
  check(code(fixture::request(HTTP_POST,"/api/command","{\"command\":\"test_timed\",\"confirm\":\"EXECUTE\",\"delay_minutes\":99}",op.cookie,op.csrf))==202&&fixture::commands==4&&fixture::last_command==ups::Command::TestTimed&&fixture::last_args.delay_minutes==99&&fixture::last_args.delay_seconds==0,"timed test uses EXECUTE confirmation and preserves duration");
  for(const char* invalid:{"-1","1.5","\"12\"","true","65536","null"}){
    const auto body=std::string("{\"command\":\"output_off\",\"confirm\":\"POWER\",\"delay_seconds\":")+invalid+"}";
    check(code(fixture::request(HTTP_POST,"/api/command",body,op.cookie,op.csrf))==400&&fixture::commands==4,"seconds must be a bounded unsigned JSON integer");
  }
  check(code(fixture::request(HTTP_POST,"/api/config","{\"hostname\":\"changed\"}",admin.cookie,"wrong"))==403&&fixture::writes==0,"invalid admin CSRF never reaches config persistence");
  response=fixture::request(HTTP_GET,"/api/config/export","",admin.cookie);const auto exported=payload(response);
  check(code(response)==200&&exported["web_password"].as<std::string>()=="admin-passphrase-123","administrator export includes the actual saved credential");
  response=fixture::request(HTTP_GET,"/api/config","",admin.cookie);const auto redacted=payload(response);
  check(redacted["web_password"].as<std::string>().empty()&&redacted["web_password_set"].as<bool>(),"normal admin config response is redacted with presence flags");
  fixture::fail_config_allocations=1;
  response=fixture::request(HTTP_GET,"/api/config","",admin.cookie);
  check(code(response)==500&&!response.response.empty(),"failed large response allocation returns JSON error instead of empty HTTP success");
  check(fixture::fail_config_allocations==0,"large response allocation fault is injected");
  fixture::fail_config_allocations=0;
  check(code(fixture::request(HTTP_POST,"/api/logout","{}",viewer.cookie,op.csrf))==403,"logout needs its own session CSRF");
  check(code(fixture::request(HTTP_GET,"/api/status","",viewer.cookie))==200,"failed logout cannot destroy another session");
  response=fixture::request(HTTP_POST,"/api/logout","{}",viewer.cookie,viewer.csrf);check(code(response)==200&&response.response_headers["Set-Cookie"].find("Max-Age=0")!=std::string::npos,"logout expires browser cookie");
  check(code(fixture::request(HTTP_GET,"/api/status","",viewer.cookie))==401,"logout invalidates server session");
}
void config_and_body_bounds(){
  Device device;const auto admin=login("admin");
  const auto post=[&](const std::string& body){return fixture::request(HTTP_POST,"/api/config",body,admin.cookie,admin.csrf);};
  check(code(post("{\"poll_ms\":0}"))==400&&fixture::writes==0,"invalid config is atomic");
  check(code(post("{}{}"))==400&&fixture::writes==0,"a second JSON object cannot bypass config validation");
  check(code(post("{hostname:'unquoted'}"))==400&&fixture::writes==0,"JavaScript object extensions are rejected as JSON");
  check(code(post(std::string(12287,' ')+"{}"))==413&&fixture::writes==0,"config above 12288 bytes is rejected before saving");
  check(code(post(std::string(2100,' ')+"{\"hostname\":\"saved-host\",\"default_page\":\"events\",\"web_password\":\"\"}"))==200&&fixture::writes==1,"config bodies above 2KB remain accepted below 12KB");
  check(device.store.saved().hostname=="saved-host"&&device.active.hostname=="ups-gateway","saved config remains separate from active boot config");
  auto response=fixture::request(HTTP_GET,"/api/session","",admin.cookie);auto data=payload(response);
  check(data["display"]["default_page"].as<std::string>()=="home"&&data["session_timeout_min"].as<unsigned>()==1,"session reports active preferences until reboot");
  check(code(fixture::request(HTTP_POST,"/api/command",std::string(2049,' ')+"{}",admin.cookie,admin.csrf))==413&&fixture::commands==0,"small endpoints keep their 2048-byte bound");
  const std::string rule="{\"id\":1,\"enabled\":true,\"name\":\"weekly restart\",\"command\":\"output_restart\",\"delay_minutes\":1,\"restore_minutes\":3,\"mode\":\"weekly\",\"at_utc\":0,\"days_mask\":2,\"minute_of_day\":540}";
  const unsigned before=fixture::writes;
  check(code(post("{\"schedules\":["+rule+"]}"))==400&&fixture::writes==before&&device.store.saved().schedules.empty(),"unconfirmed power schedule never saves or changes state");
  check(code(post("{\"schedules\":["+rule+"],\"confirm\":\"EXECUTE\"}"))==400&&fixture::writes==before,"wrong transient confirmation cannot save power schedule");
  check(code(post("{\"schedules\":["+rule+"],\"confirm\":\"POWER\"}"))==200&&fixture::writes==before+1,"confirmed power schedule is persisted");
  response=fixture::request(HTTP_GET,"/api/config/export","",admin.cookie);data=payload(response);
  check(!data.containsKey("confirm")&&data["schedules"].size()==1,"transient POWER confirmation is absent from exports");
  check(code(post("{\"location\":\"room two\"}"))==200,"unrelated config edit does not require repeated power confirmation");
  check(code(post("{\"snmp_write_community\":\"test-write-community\"}"))==200&&device.store.saved().snmp_write_community=="test-write-community","administrator can configure write community");
  response=post("{\"snmp_write_community\":\"private-trap-community\",\"traps\":[{\"enabled\":true,\"ip\":\"192.0.2.20\"},{\"enabled\":false}]}");
  check(code(response)==400&&response.response.find("Enabled trap community must differ")!=std::string::npos&&device.store.saved().snmp_write_community=="test-write-community","enabled trap cannot disclose write community");
  response=fixture::request(HTTP_GET,"/api/config","",admin.cookie);data=payload(response);
  check(data["snmp_write_community"].as<std::string>().empty()&&data["snmp_write_community_set"].as<bool>(),"write community is redacted with presence flag");
  response=fixture::request(HTTP_GET,"/api/config/export","",admin.cookie);data=payload(response);
  check(data["snmp_write_community"].as<std::string>()=="test-write-community","secret export preserves write community");
  check(code(post("{\"location\":\"room three\"}"))==200&&device.store.saved().snmp_write_community=="test-write-community","omitted write community retains secret");
  check(code(post("{\"snmp_write_community\":\"\"}"))==200&&device.store.saved().snmp_write_community.empty(),"explicit empty write community disables SET");
  check(code(post("{\"snmp_write_community\":\"test-write-community\"}"))==200,"write community can be reconfigured");
  check(code(fixture::request(HTTP_POST,"/api/reset-config","{\"confirm\":\"RESET_CONFIG\",\"keep_network\":true}",admin.cookie,admin.csrf))==200&&device.store.saved().web_password=="admin-passphrase-123"&&device.store.saved().snmp_write_community.empty(),"functional reset preserves access credentials and disables SET");
}
void persistent_event_contract(){
  Device device;const auto admin=login("admin");fixture::events.clear();
  for(uint32_t seq=200;seq>0;--seq)fixture::events.push_back({seq,seq*1000,gateway::EventType::UpsFault,uint8_t(seq%50==0?3:1),seq%50==0?"match":"other",0});
  fixture::persistence={true,"",190,3825,10,2};
  auto response=fixture::request(HTTP_GET,"/api/events?min_severity=3&contains=match&limit=2","",admin.cookie);auto data=payload(response);
  check(code(response)==200&&data["events"].size()==2&&data["events"][0]["seq"].as<unsigned>()==200&&data["events"][1]["seq"].as<unsigned>()==150,"filters scan more than 48 raw records to fill page");
  check(data["next_before"].as<unsigned>()==150,"cursor is last examined event, not end of fetched chunk");
  check(data["persistence"]["available"].as<bool>()&&data["persistence"]["stored"].as<unsigned>()==190&&data["persistence"]["capacity"].as<unsigned>()==3825&&data["persistence"]["pending"].as<unsigned>()==10&&data["persistence"]["dropped"].as<unsigned>()==2&&data["persistence"]["flush_interval_s"].as<unsigned>()==30,"persistence metadata reflects EventBus status");
  response=fixture::request(HTTP_GET,"/api/events?min_severity=3&contains=match&limit=2&before=150","",admin.cookie);data=payload(response);
  check(data["events"].size()==2&&data["events"][0]["seq"].as<unsigned>()==100&&data["events"][1]["seq"].as<unsigned>()==50&&data["next_before"].as<unsigned>()==50,"older page is exclusive and continues across raw chunks");
  response=fixture::request(HTTP_GET,"/api/events?min_severity=3&contains=match&limit=2&before=50","",admin.cookie);data=payload(response);check(data["events"].size()==0&&data["next_before"].as<unsigned>()==0,"exhausted filtered history ends pagination");
  response=fixture::request(HTTP_GET,"/api/events.csv?min_severity=3&contains=match&limit=2&before=150","",admin.cookie);check(code(response)==200&&response.response.find("100,")!=std::string::npos&&response.response.find("150,")==std::string::npos,"CSV exports requested filtered page");
  for(const char* query:{"before=-1","before=abc","before=4294967296","before=1&before=2"})check(code(fixture::request(HTTP_GET,std::string("/api/events?")+query,"",admin.cookie))==400,"invalid or repeated cursor rejected");
  fixture::events={{2,0,gateway::EventType::Startup,1,"two",0},{1,0,gateway::EventType::Startup,1,"one",0},{UINT32_MAX,0,gateway::EventType::Startup,1,"max",0},{UINT32_MAX-1,0,gateway::EventType::Startup,1,"max-minus-one",0}};
  response=fixture::request(HTTP_GET,"/api/events?limit=2","",admin.cookie);data=payload(response);check(data["events"].size()==2&&data["next_before"].as<unsigned>()==1,"first page preserves wrap cursor");response=fixture::request(HTTP_GET,"/api/events?limit=2&before=1","",admin.cookie);data=payload(response);check(data["events"].size()==2&&data["events"][0]["seq"].as<uint32_t>()==UINT32_MAX&&data["events"][1]["seq"].as<uint32_t>()==UINT32_MAX-1&&data["next_before"].as<unsigned>()==0,"older page crosses sequence wrap without loss");
  fixture::fail_history_read=true;response=fixture::request(HTTP_GET,"/api/events?limit=1","",admin.cookie);data=payload(response);check(!data["persistence"]["available"].as<bool>()&&data["persistence"]["error"].as<std::string>()=="Synthetic history read failure","read-time storage failure appears in response metadata");fixture::fail_history_read=false;
  fixture::persistence={false,"Synthetic write failure",190,3825,10,2};response=fixture::request(HTTP_GET,"/api/events?limit=1","",admin.cookie);data=payload(response);check(!data["persistence"]["available"].as<bool>()&&data["persistence"]["error"].as<std::string>()=="Synthetic write failure","storage failure is visible without hiding RAM events");
  fixture::fail_log_clear=true;response=fixture::request(HTTP_POST,"/api/logs/clear","{\"target\":\"events\",\"confirm\":\"CLEAR_LOG\"}",admin.cookie,admin.csrf);check(code(response)==500&&response.response.find("Synthetic clear failure")!=std::string::npos&&!fixture::events.empty(),"failed durable clear returns error and preserves events");
  fixture::fail_log_clear=false;response=fixture::request(HTTP_POST,"/api/logs/clear","{\"target\":\"events\",\"confirm\":\"CLEAR_LOG\"}",admin.cookie,admin.csrf);check(code(response)==200&&fixture::events.empty(),"confirmed durable clear succeeds");
  response=fixture::request(HTTP_POST,"/api/reboot","{\"confirm\":\"REBOOT\"}",admin.cookie,admin.csrf);check(code(response)==200,"restart scheduled for flush test");fixture::fail_log_flush=true;fixture::now+=2000;device.web.tick();check(fixture::lifecycle==std::vector<std::string>{"flush","restart"},"restart attempts flush first and proceeds on failure");
}
void filters_status_and_handlers(){
  Device device;const auto admin=login("admin");
  fixture::events={{7,7000,gateway::EventType::UpsFault,3,"=1+1",1794000000},{6,6000,gateway::EventType::Startup,1,"normal message",0}};
  auto response=fixture::request(HTTP_GET,"/api/events?min_severity=2&contains=%2B&limit=1","",admin.cookie);auto data=payload(response);
  check(code(response)==200&&data["events"].size()==1&&data["events"][0]["seq"].as<unsigned>()==7&&data["capacity"].as<unsigned>()==48,"event query filters and limit affect streamed JSON");
  response=fixture::request(HTTP_GET,"/api/events.csv?min_severity=2","",admin.cookie);
  check(code(response)==200&&response.type=="text/csv; charset=utf-8"&&response.response.find("\"'=1+1\"")!=std::string::npos&&response.response.find("normal message")==std::string::npos,"CSV export applies filter and neutralizes formulas");
  check(code(fixture::request(HTTP_GET,"/api/events?limit=49","",admin.cookie))==400,"event limit above capacity is rejected");
  check(code(fixture::request(HTTP_GET,"/api/events?min_severity=9","",admin.cookie))==400,"invalid severity is rejected");
  check(code(fixture::request(HTTP_GET,"/api/events?limit=1&limit=48","",admin.cookie))==400,"duplicate query cannot override validation");
  check(code(fixture::request(HTTP_GET,"/api/%68istory?limit=7","",admin.cookie))==200&&fixture::history_limit==7,"decoded path and query dispatch to real history handler");
  check(code(fixture::request(HTTP_GET,"/api/history?limit=241","",admin.cookie))==400,"history capacity bounds are enforced");
  fixture::snapshot.input_fault_voltage={201,true};fixture::snapshot.nominal_voltage={230,true};fixture::snapshot.nominal_current={6,true};
  fixture::snapshot.nominal_battery_voltage={24,true};fixture::snapshot.nominal_frequency={50,true};fixture::snapshot.bypass_or_avr=true;fixture::snapshot.standby_topology=false;
  fixture::snapshot.manufacturer=std::string(15,'M');fixture::snapshot.model=std::string(10,'D');fixture::snapshot.firmware=std::string(10,'F');fixture::snapshot.raw_status=std::string(8,'1');fixture::snapshot.last_status_line=std::string(160,'S');device.active.hostname=std::string(32,'h');
  response=fixture::request(HTTP_GET,"/api/status","",admin.cookie);data=payload(response);
  check(code(response)==200&&data["ups"]["manufacturer"].as<std::string>()==fixture::snapshot.manufacturer&&data["ups"]["model"].as<std::string>()==fixture::snapshot.model&&data["ups"]["firmware"].as<std::string>()==fixture::snapshot.firmware&&data["ups"]["last_status_line"].as<std::string>()==fixture::snapshot.last_status_line&&data["hostname"].as<std::string>()==device.active.hostname&&data["traps"].containsKey("queued"),"full status document serializes with maximum driver identity and frame strings");
  check(code(response)==200&&data["ups"]["load_percent"].as<int>()==37&&data["thresholds"]["alarm_load_high"].as<int>()==90,"all read roles can render live load against active threshold");
  check(data["ups"]["stale_ms"].as<unsigned>()==device.active.stale_ms,"status exposes the actual active stale boundary for browser aging");
  const char* added[]={"input_fault_voltage","nominal_voltage","nominal_current","nominal_battery_voltage","nominal_frequency","bypass_or_avr","standby_topology"};
  for(const auto* key:added)check(data["ups"].containsKey(key),"status includes each added metric and flag");
  check(data["ups"]["input_fault_voltage"].as<int>()==201&&data["ups"]["nominal_voltage"].as<int>()==230&&data["ups"]["nominal_current"].as<int>()==6&&data["ups"]["nominal_battery_voltage"].as<int>()==24&&data["ups"]["nominal_frequency"].as<int>()==50,"fresh status exposes parsed metrics");
  check(data["ups"]["bypass_or_avr"].is<bool>()&&data["ups"]["bypass_or_avr"].as<bool>()&&data["ups"]["standby_topology"].is<bool>()&&!data["ups"]["standby_topology"].as<bool>(),"fresh status exposes both parsed flags");
  fixture::snapshot.nominal_voltage.valid=false;fixture::snapshot.nominal_current.valid=false;fixture::snapshot.nominal_battery_voltage.valid=false;fixture::snapshot.nominal_frequency.valid=false;
  response=fixture::request(HTTP_GET,"/api/status","",admin.cookie);data=payload(response);
  for(const auto* key:{"nominal_voltage","nominal_current","nominal_battery_voltage","nominal_frequency"})check(data["ups"].containsKey(key)&&data["ups"][key].isNull(),"unreported rating is present and null");
  fixture::snapshot.nominal_voltage.valid=true;fixture::snapshot.nominal_current.valid=true;fixture::snapshot.nominal_battery_voltage.valid=true;fixture::snapshot.nominal_frequency.valid=true;
  fixture::now=12000;response=fixture::request(HTTP_GET,"/api/status","",admin.cookie);data=payload(response);
  for(const auto* key:added)check(data["ups"].containsKey(key)&&data["ups"][key].isNull(),"stale added metric or flag is present and null");
  check(data["ups"]["load_percent"].isNull()&&data["ups"]["input_voltage"].isNull()&&!data["ups"]["connected"].as<bool>()&&data["ups"]["low_battery"].isNull(),"stale readings and flags become null even before UART owner updates connected");
  check(code(fixture::request(HTTP_POST,"/api/command","{\"command\":\"test_short\",\"confirm\":\"EXECUTE\"}",admin.cookie,admin.csrf))==409,"backend stale rejection is preserved as conflict");
  check(code(fixture::request(HTTP_POST,"/api/time","{\"utc\":1794000000}",admin.cookie,admin.csrf))==409&&fixture::manual_utc==0,"manual time cannot override enabled NTP");
  check(code(fixture::request(HTTP_POST,"/api/syslog-test","{}",admin.cookie,admin.csrf))==202&&fixture::syslog_tests==1,"syslog test is accepted as queued work");
  check(code(fixture::request(HTTP_POST,"/api/logs/clear","{\"target\":\"events\",\"confirm\":\"wrong\"}",admin.cookie,admin.csrf))==400&&fixture::events.size()==2,"log clear requires exact confirmation");
  check(code(fixture::request(HTTP_POST,"/api/logs/clear","{\"target\":\"events\",\"confirm\":\"CLEAR_LOG\"}",admin.cookie,admin.csrf))==200&&fixture::events.empty(),"confirmed event clear reaches journal");
  check(code(fixture::request(HTTP_POST,"/api/logs/clear","{\"target\":\"history\",\"confirm\":\"CLEAR_LOG\"}",admin.cookie,admin.csrf))==200&&fixture::cleared_history==1,"confirmed history clear reaches management service");
}
void expiry_boundary_and_local_reset(){
  Device device;const auto admin=login("admin");
  fixture::now=60999;check(code(fixture::request(HTTP_GET,"/api/status","",admin.cookie))==200,"background read just before deadline remains valid");
  fixture::now=61000;check(code(fixture::request(HTTP_GET,"/api/status","",admin.cookie))==401,"background polling never extends idle deadline");
  const auto next=login("admin");fixture::now=120999;fixture::clock_step=1;
  check(code(fixture::request(HTTP_GET,"/api/status","",next.cookie,"",true))==401,"expiry between lookup and activity touch cannot authorize a request");
  fixture::clock_step=0;const uint64_t logged_at=fixture::now;const auto boundary=login("admin");
  fixture::now=logged_at+59998;fixture::clock_step=1;const unsigned writes_before=fixture::writes;
  check(code(fixture::request(HTTP_POST,"/api/config","{\"hostname\":\"expired-write\"}",boundary.cookie,boundary.csrf))==401&&fixture::writes==writes_before,"expiry during CSRF/activity checks cannot persist configuration");
  fixture::clock_step=0;const auto local=login("admin");std::string password,error;
  check(device.web.resetPasswordLocal(password,error)&&password=="new-local-passphrase-123","local USB password reset remains functional");
  check(code(fixture::request(HTTP_GET,"/api/status","",local.cookie))==401,"local password reset invalidates existing RAM sessions");
}
std::string firmwareImage(){
  std::string image(9000,'\x5a');image[0]=char(0xe9);image[1]=1;image[12]=0;image[13]=0;
  image[24]=0x20;image[25]=0;image[26]=0x3f;image[27]=0x3f;
  const uint32_t segment_length=8800;for(unsigned i=0;i<4;++i)image[28+i]=char(segment_length>>(8*i));
  image[32]=char(0x32);image[33]=char(0x54);image[34]=char(0xcd);image[35]=char(0xab);return image;
}
void firmware_upload_contract(){
  Device device;const auto admin=login("admin"),op=login("operator");const auto image=firmwareImage();
  check(code(fixture::firmwareRequest(image,"", ""))==401&&fixture::ota_begins==0,"anonymous firmware upload is denied before flash");
  check(code(fixture::firmwareRequest(image,op.cookie,op.csrf))==403&&fixture::ota_begins==0,"operator firmware upload is denied before flash");
  check(code(fixture::firmwareRequest(image,admin.cookie,"wrong"))==403&&fixture::ota_begins==0,"invalid CSRF is denied before flash");
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf,"application/octet-stream",""))==400&&fixture::ota_begins==0,"firmware confirmation is mandatory before flash");
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf,"application/json"))==415&&fixture::ota_begins==0,"firmware media type is enforced before flash");
  check(code(fixture::firmwareRequest("",admin.cookie,admin.csrf))==413&&fixture::ota_begins==0,"empty firmware is rejected before flash");
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf,"application/octet-stream","UPDATE",fixture::ota_limit+1))==413&&fixture::ota_begins==0,"firmware larger than actual slot is rejected before flash");
  fixture::ota_available=false;
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==409&&fixture::ota_begins==0,"missing update partition is rejected before flash");
  fixture::ota_available=true;fixture::control.last_state="queued";
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==409&&fixture::ota_begins==0,"queued UPS command blocks upload");
  fixture::control={};auto bad=image;bad[0]=0;
  check(code(fixture::firmwareRequest(bad,admin.cookie,admin.csrf))==400&&fixture::ota_begins==0,"invalid image magic is rejected before flash");
  bad=image;bad[12]=5;
  check(code(fixture::firmwareRequest(bad,admin.cookie,admin.csrf))==400&&fixture::ota_begins==0,"wrong target chip is rejected before flash");
  bad=image;bad[32]=0;
  check(code(fixture::firmwareRequest(bad,admin.cookie,admin.csrf))==400&&fixture::ota_begins==0,"bootloader or missing app descriptor is rejected before flash");
  check(code(fixture::firmwareRequest(image.substr(0,16),admin.cookie,admin.csrf))==400&&fixture::ota_begins==0,"truncated header is rejected before flash");
  const auto response=fixture::firmwareRequest(image,admin.cookie,admin.csrf);const auto data=payload(response);
  check(code(response)==200&&data["ok"].as<bool>()&&data["rebooting"].as<bool>(),"validated firmware response schedules automatic reboot");
  check(fixture::ota_written==image&&fixture::ota_begins==1&&fixture::ota_ends==1&&fixture::ota_switches==1,"exact firmware bytes are finalized before boot slot switch");
  check(fixture::ota_max_chunk<=4096,"firmware writes are bounded to 4 KiB");
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==409&&fixture::ota_begins==1,"second upload is blocked while reboot is pending");
  fixture::now=2499;device.web.tick();check(fixture::restarts==0,"automatic reboot waits for response grace period");
  fixture::control.last_state="queued";fixture::now=2500;device.web.tick();check(fixture::restarts==0,"queued UPS command defers scheduled reboot");
  fixture::now=12499;device.web.tick();check(fixture::restarts==0,"queued UPS command delays reboot up to its bound");
  fixture::now=12500;device.web.tick();check(fixture::restarts==1,"scheduled reboot proceeds at bounded delay");
}
void firmware_failure_paths(){
  Device device;const auto admin=login("admin");const auto image=firmwareImage();
  fixture::ota_fail_begin=true;
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==500&&fixture::ota_aborts==0&&fixture::ota_switches==0,"failed OTA begin never switches boot slot");
  fixture::ota_fail_begin=false;fixture::ota_fail_write=true;
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==500&&fixture::ota_aborts==1&&fixture::ota_switches==0,"failed OTA write aborts before boot switch");
  fixture::ota_fail_write=false;
  check(code(fixture::firmwareRequest(image.substr(0,5000),admin.cookie,admin.csrf,"application/octet-stream","UPDATE",image.size()))==408&&fixture::ota_aborts==2&&fixture::ota_switches==0,"disconnect after flashing aborts before boot switch");
  fixture::ota_fail_end=true;
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==400&&fixture::ota_ends==1&&fixture::ota_switches==0,"failed OTA end never switches boot slot");
  fixture::ota_fail_end=false;fixture::ota_fail_verify=true;
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==400&&fixture::ota_switches==0,"failed image verification never switches boot slot");
  fixture::ota_fail_verify=false;fixture::ota_verified_length=image.size()-1;
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==400&&fixture::ota_switches==0,"image with trailing bytes never switches boot slot");
  fixture::ota_verified_length=static_cast<size_t>(-1);fixture::ota_fail_switch=true;
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==500&&fixture::ota_switches==1,"failed boot selection returns an error");
  fixture::now=200000;device.web.tick();check(fixture::restarts==0,"all failed firmware updates leave reboot unscheduled");
}
void pending_image_upload(){
  Device device;const auto admin=login("admin");const auto image=firmwareImage();
  auto info=payload(fixture::request(HTTP_GET,"/api/firmware","",admin.cookie));
  check(info["available"].as<bool>()&&info["reason"].as<std::string>().empty(),"available firmware info has an empty reason");
  fixture::ota_available=false;
  info=payload(fixture::request(HTTP_GET,"/api/firmware","",admin.cookie));
  check(!info["available"].as<bool>()&&info["reason"].as<std::string>().find("USB")!=std::string::npos,"legacy layout advertises USB migration");
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==409&&fixture::ota_begins==0,"legacy layout cannot receive OTA");
  fixture::ota_available=true;fixture::ota_running_state=ESP_OTA_IMG_PENDING_VERIFY;
  info=payload(fixture::request(HTTP_GET,"/api/firmware","",admin.cookie));
  check(!info["available"].as<bool>()&&info["reason"].as<std::string>().find("confirm")!=std::string::npos,"pending image reports unconfirmed firmware");
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==409&&fixture::ota_begins==0&&fixture::ota_switches==0,"unverified running image cannot overwrite its rollback slot");
  fixture::ota_state_available=false;
  info=payload(fixture::request(HTTP_GET,"/api/firmware","",admin.cookie));
  check(!info["available"].as<bool>()&&info["reason"].as<std::string>().find("rollback")!=std::string::npos,"missing running-image record reports no rollback record");
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==409&&fixture::ota_begins==0,"missing running-image record fails closed before flash");
  fixture::ota_state_available=true;fixture::ota_running_state=ESP_OTA_IMG_UNDEFINED;
  info=payload(fixture::request(HTTP_GET,"/api/firmware","",admin.cookie));
  check(!info["available"].as<bool>()&&info["reason"].as<std::string>().find("rollback")!=std::string::npos,"undefined image state reports missing confirmed rollback record");
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==409&&fixture::ota_begins==0&&fixture::ota_switches==0,"undefined image state blocks OTA before flash");
  fixture::ota_running_state=ESP_OTA_IMG_VALID;
  info=payload(fixture::request(HTTP_GET,"/api/firmware","",admin.cookie));
  check(info["available"].as<bool>()&&info["reason"].as<std::string>().empty(),"confirmed running image allows OTA");
  const auto response=fixture::firmwareRequest(image,admin.cookie,admin.csrf);
  check(code(response)==200&&fixture::ota_begins==1&&fixture::ota_switches==1,"confirmed running image can be updated");
}
void firmware_reboot_admission(){
  Device device;const auto admin=login("admin");const auto image=firmwareImage();
  check(code(fixture::request(HTTP_POST,"/api/reboot","{\"confirm\":\"REBOOT\"}",admin.cookie,admin.csrf))==200,"manual web restart is scheduled");
  check(code(fixture::firmwareRequest(image,admin.cookie,admin.csrf))==409&&fixture::ota_begins==0,"scheduled manual reboot blocks firmware flashing");
  fixture::control.last_state="queued";fixture::now=2500;device.web.tick();check(fixture::restarts==0,"manual web reboot waits for a queued command");
  fixture::control={};fixture::now=2501;device.web.tick();check(fixture::restarts==1,"manual web reboot proceeds when the queue clears");
}
void about_running_slot(){
  Device device;const auto admin=login("admin");fixture::ota_running_second=true;
  const auto response=fixture::request(HTTP_GET,"/api/about","",admin.cookie);const auto data=payload(response);
  check(code(response)==200&&data["storage"]["firmware_slot"].as<std::string>()=="ota_1"&&data["storage"]["firmware_offset"].as<unsigned>()==0x400000&&data["storage"]["firmware_size"].as<unsigned>()==0x3f0000,"about reports the actual running slot, offset and size");
}
void firmware_probation(){
  Device device;
  fixture::ota_state_available=false;
  gateway::checkFirmwareProbation(120000,false,false,false);
  check(fixture::ota_mark_invalid==0&&fixture::ota_mark_valid==0,"missing OTA state does not roll back a first USB installation");
  fixture::ota_state_available=true;fixture::ota_running_state=ESP_OTA_IMG_UNDEFINED;
  gateway::checkFirmwareProbation(120000,false,false,false);
  check(fixture::ota_mark_invalid==0&&fixture::ota_mark_valid==0,"undefined OTA state does not roll back a USB installation");
  fixture::ota_running_state=ESP_OTA_IMG_VALID;
  gateway::checkFirmwareProbation(120000,false,false,false);
  check(fixture::ota_mark_invalid==0&&fixture::ota_mark_valid==0,"already-valid image never enters probation");
  fixture::ota_running_state=ESP_OTA_IMG_PENDING_VERIFY;
  gateway::checkFirmwareProbation(3000,true,false,true);
  check(fixture::ota_mark_valid==0,"static IP alone cannot pass probation without a physical link");
  gateway::checkFirmwareProbation(5000,true,true,false);
  check(fixture::ota_mark_valid==0,"Ethernet link without an IP cannot pass probation");
  gateway::checkFirmwareProbation(7000,false,true,true);
  check(fixture::ota_mark_valid==0,"config and services must start before passing probation");
  gateway::checkFirmwareProbation(119999,true,true,true);
  check(fixture::ota_mark_valid==1&&fixture::ota_mark_invalid==0,"ready pending image is marked valid before deadline");
  fixture::ota_mark_valid=0;gateway::checkFirmwareProbation(119999,false,false,false);
  check(fixture::ota_mark_invalid==0,"pending image has a full 120-second probation");
  gateway::checkFirmwareProbation(120000,false,false,false);
  check(fixture::ota_mark_invalid==1&&fixture::ota_mark_valid==0,"pending image rolls back at deadline if startup is incomplete");
  fixture::ota_mark_invalid=0;gateway::checkFirmwareProbation(120000,true,true,true);
  check(fixture::ota_mark_invalid==1&&fixture::ota_mark_valid==0,"deadline takes precedence over late readiness");
}
void firmware_route_access(){
  Device device;
  const auto admin=login("admin"),op=login("operator"),viewer=login("viewer");
  check(code(fixture::request(HTTP_GET,"/api/firmware"))==401,"firmware information requires a session");
  check(code(fixture::request(HTTP_GET,"/api/firmware","",viewer.cookie))==403,"viewers cannot access firmware information");
  check(code(fixture::request(HTTP_GET,"/api/firmware","",op.cookie))==403,"operators cannot access firmware information");
  const auto response=fixture::request(HTTP_GET,"/api/firmware","",admin.cookie);
  check(code(response)==200,"administrators can read firmware information");
  if(code(response)==200){const auto data=payload(response);check(data["available"].is<bool>()&&data["max_image_bytes"].is<unsigned>()&&data["version"].as<std::string>()=="0.2.0","firmware information exposes availability, partition limit, and running version");}
  check(code(fixture::request(HTTP_POST,"/api/firmware","image",viewer.cookie,viewer.csrf))==403,"viewers cannot upload firmware");
  check(code(fixture::request(HTTP_POST,"/api/firmware","image",op.cookie,op.csrf))==403,"operators cannot upload firmware");
  check(code(fixture::request(HTTP_POST,"/api/firmware","image",admin.cookie))==403,"firmware upload requires CSRF");
}
}
int main(){allocation_faults();public_and_roles();config_and_body_bounds();persistent_event_contract();filters_status_and_handlers();expiry_boundary_and_local_reset();firmware_route_access();firmware_upload_contract();firmware_failure_paths();pending_image_upload();firmware_reboot_admission();about_running_slot();firmware_probation();if(failures)return 1;std::cout<<"HTTP adapter: production handlers passed authentication, permissions, CSRF, expiry, config/schedule admission, stale status, query/CSV and management actions\n";}
