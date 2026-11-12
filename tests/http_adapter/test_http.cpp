#include "harness.hpp"
#include <iostream>

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
    fixture::initial=active;fixture::now=1000;fixture::clock_step=0;fixture::writes=0;fixture::commands=0;fixture::cleared_history=0;fixture::syslog_tests=0;fixture::events.clear();
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
  check(code(fixture::request(HTTP_POST,"/api/reset-config","{\"confirm\":\"RESET_CONFIG\",\"keep_network\":true}",admin.cookie,admin.csrf))==200&&device.store.saved().web_password=="admin-passphrase-123","functional reset preserves access credentials");
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
}
int main(){allocation_faults();public_and_roles();config_and_body_bounds();filters_status_and_handlers();expiry_boundary_and_local_reset();if(failures)return 1;std::cout<<"HTTP adapter: production handlers passed authentication, permissions, CSRF, expiry, config/schedule admission, stale status, query/CSV and management actions\n";}
