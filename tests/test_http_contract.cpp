#include "auth_core.hpp"
#include "config_model.hpp"
#include <iostream>
#include <string>
using namespace gateway::auth;
namespace {
int failed=0;
void check(bool ok,const char* name){if(!ok){++failed;std::cerr<<"FAIL: "<<name<<'\n';}}
void role_and_csrf_gates(){
  Session admin,op,viewer;admin.role=Role::Admin;op.role=Role::Operator;viewer.role=Role::Viewer;
  for(const auto& route:routes()){
    if(route.method==Method::Post&&route.id!=Endpoint::Login){
      check(authorize(route,nullptr,true)==Access::Unauthenticated,"every non-login POST requires a live session");
      check(authorize(route,&admin,false)==Access::InvalidCsrf,"every non-login POST requires CSRF even for an administrator");
    }
    if(route.minimum==Role::None){check(authorize(route,nullptr,false)==Access::Allowed,"public route is usable without a session");continue;}
    check(authorize(route,nullptr,true)==Access::Unauthenticated,"all protected routes reject absent session");
    if(route.minimum==Role::Admin){
      check(authorize(route,&viewer,true)==Access::Forbidden,"viewer cannot read config/export or mutate admin routes");
      check(authorize(route,&op,true)==Access::Forbidden,"operator cannot read config/export or mutate admin routes");
    }
    if(route.minimum==Role::Operator)check(authorize(route,&viewer,true)==Access::Forbidden,"viewer cannot submit UPS commands");
    if(route.csrf){
      check(authorize(route,&admin,false)==Access::InvalidCsrf,"admin mutation requires CSRF");
      check(authorize(route,&admin,true)==Access::Allowed,"admin with CSRF may use mutation endpoint");
    }else check(authorize(route,&admin,false)==Access::Allowed,"authorized GET does not require CSRF");
  }
  const auto* command=routeFor(Method::Post,"/api/command");
  check(command&&authorize(*command,&op,true)==Access::Allowed,"operator may request an independently validated UPS command");
  check(command&&authorize(*command,&op,false)==Access::InvalidCsrf,"operator command also requires CSRF");
  check(routeFor(Method::Get,"/api/config/export")&&routeFor(Method::Get,"/api/config/export")->minimum==Role::Admin,"secret export has the admin gate");
  check(routeFor(Method::Post,"/api/config")->max_body==12288,"configuration permits bounded schema2 import");
  for(const auto& route:routes())if(route.method==Method::Post&&route.id!=Endpoint::Config)check(route.max_body<=2048,"non-config requests keep the small body limit");
  check(routeFor(Method::Post,"/api/status")==nullptr&&routeFor(Method::Get,"/api/reset-config")==nullptr,"wrong methods cannot call endpoints");
}
void target_and_payload_validation(){
  RequestTarget t;std::string error;
  check(parseTarget("/api/events?min_severity=2&contains=on%20battery%2B&limit=7",t,error)&&t.path=="/api/events"&&*t.get("contains")=="on battery+","query is stripped and decoded once");
  check(parseTarget("/api/%68istory?limit=240",t,error)&&t.path=="/api/history","encoded route characters decode for dispatch");
  check(!parseTarget("/api/events?limit=4&limit=48",t,error),"ambiguous duplicate query is rejected");
  check(!parseTarget("/api/events?contains=%0d%0aX-Header",t,error),"decoded query controls are rejected");
  check(!parseTarget("/api/events?contains=%GG",t,error),"malformed percent escape is rejected");
  check(!parseTarget("/api/events?contains="+std::string(193,'a'),t,error),"decoded query values are bounded");
  check(isSingleJsonObject(" \n{\"nested\":{\"x\":1},\"text\":\"}\\\"\"} \r\n"),"nested object and quoted braces are accepted");
  check(!isSingleJsonObject("{}{}")&&!isSingleJsonObject("{}false"),"trailing JSON or garbage cannot be ignored");
  check(!isSingleJsonObject("[]")&&!isSingleJsonObject("null"),"only one root object is accepted");
  check(!isSingleJsonObject("{\"a\":1"),"truncated object is rejected");
  check(!isSingleJsonObject(std::string("{}\0{}",5)),"embedded NUL cannot hide a second object");
  check(!isSingleJsonObject("{username:'admin'}"),"non-JSON unquoted keys and single quotes are rejected");
  check(!isSingleJsonObject("{\"a\":1,}")&&!isSingleJsonObject("{\"a\":[1,]}"),"trailing commas are rejected before the permissive framework parser");
  check(!isSingleJsonObject("{\"a\":01}")&&!isSingleJsonObject("{\"a\":NaN}")&&!isSingleJsonObject("{\"a\":.5}"),"only JSON number syntax is accepted");
  check(!isSingleJsonObject("{\"a\":\"\\x41\"}")&&!isSingleJsonObject("{\"a\\u0000extra\":1}"),"invalid escapes and hidden NUL keys are rejected");
  check(isSingleJsonObject("{\"text\":\"\\u0410\\n\",\"a\":[true,false,null,-1.25e+2,0]}"),"strict JSON retains strings arrays booleans null and numeric exponents");
  check(extractSessionCookie("theme=x; ups_session="+std::string(64,'a'))==std::string(64,'a'),"session cookie is selected by exact name");
  check(extractSessionCookie("ups_session="+std::string(64,'a')+"; ups_session="+std::string(64,'b')).empty(),"ambiguous duplicate session cookies are rejected");
  check(extractSessionCookie("ups_session="+std::string(65,'a')).empty(),"oversized session cookie is rejected");
  check(extractSessionCookie("ups_session="+std::string(63,'a')+";").empty(),"malformed session token is rejected");
  check(csvCell("=HYPERLINK(\"https://example.test\")")=="\"'=HYPERLINK(\"\"https://example.test\"\")\"","CSV formula text is neutralized and quoted");
  check(csvCell("message,with\"quote\"")=="\"message,with\"\"quote\"\"\"","CSV commas and quotes round-trip");
}
void power_schedule_confirmation(){
  gateway::Config saved,candidate;gateway::ScheduleRule rule;
  rule.id=4;rule.enabled=true;rule.command="output_restart";rule.mode="weekly";rule.days_mask=2;rule.minute_of_day=540;
  candidate.schedules={rule};
  check(powerConfirmationRequired(saved,candidate),"creating enabled power schedule needs explicit POWER confirmation");
  saved=candidate;check(!powerConfirmationRequired(saved,candidate),"unchanged power schedule does not burden unrelated edits");
  candidate.hostname="different-host";check(!powerConfirmationRequired(saved,candidate),"identity-only save keeps existing power authorization");
  candidate.schedules[0].minute_of_day=541;check(powerConfirmationRequired(saved,candidate),"changing power schedule timing needs confirmation");
  candidate=saved;candidate.schedules[0].enabled=false;check(!powerConfirmationRequired(saved,candidate),"disabling a power schedule is permitted without POWER");
  candidate=saved;candidate.controls_enabled=true;check(powerConfirmationRequired(saved,candidate),"new control opt-in cannot silently arm retained power schedules");
  candidate=saved;candidate.power_controls_enabled=true;check(powerConfirmationRequired(saved,candidate),"new power opt-in cannot silently arm retained power schedules");
  candidate=saved;candidate.timezone="CET-1";check(powerConfirmationRequired(saved,candidate),"timezone changes require review of weekly power schedules");
  candidate=saved;candidate.driver="different-driver";check(powerConfirmationRequired(saved,candidate),"driver change requires review of enabled power schedules");
  candidate=saved;candidate.schedules[0].command="test_short";check(!powerConfirmationRequired(saved,candidate),"non-power schedules use normal administrator authorization");
}
}
int main(){role_and_csrf_gates();target_and_payload_validation();power_schedule_confirmation();if(failed)return 1;std::cout<<"http contract: route permissions, CSRF, query/body bounds, cookie parsing, CSV and power schedule confirmation passed\n";}
