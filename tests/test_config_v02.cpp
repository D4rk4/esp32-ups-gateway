#include "config_json.hpp"
#include <ArduinoJson.h>
#include <cassert>
#include <iostream>
using namespace gateway;
static Config validBase() {
  Config c; c.web_user="admin"; c.web_password="admin-password-123456"; c.snmp_community="private-community";
  for(auto& t:c.traps)t.community="trap-community";
  return c;
}
static std::string json(const JsonDocument& d){std::string s;serializeJson(d,s);return s;}
int main() {
  const auto base=validBase(); Config out=base; std::string error;
  // A real legacy export must migrate in RAM without changing any secrets.
  DynamicJsonDocument legacy(24576); assert(!deserializeJson(legacy,configJson(base,true)));
  legacy["schema"]=1;
  const char* extras[]={"operator_enabled","operator_user","operator_password","readonly_enabled","readonly_user","readonly_password","session_timeout_min","ntp_enabled","ntp_server","timezone","date_format","temperature_unit","history_interval_s","default_page","quick_links","alarm_load_high","alarm_temperature_high","alarm_input_low","alarm_input_high","alarm_delay_s","syslog_enabled","syslog_host","syslog_port","syslog_min_severity","schedules"};
  for(const auto* key:extras)legacy.remove(key);
  assert(mergeConfigJson(Config{},json(legacy),out,error,true));
  assert(out.schema==2);
  assert(out.web_user=="admin"&&out.web_password=="admin-password-123456"&&out.snmp_community=="private-community");
  assert(out.traps[0].community=="trap-community");
  DynamicJsonDocument full(24576); assert(!deserializeJson(full,configJson(out,true)));
  assert(full["session_timeout_min"].as<int>()==15);
  assert(full["quick_links"].size()==3&&full["schedules"].size()==0);
  assert(full["operator_password"].as<std::string>().empty());
  assert(mergeConfigJson(Config{},configJson(out,true),out,error,true));
  const auto before=configJson(out,true);
  const char* invalid[]={
    "{\"schema\":3}","{\"schema\":2,\"operator_enabled\":true}",
    "{\"operator_enabled\":true,\"operator_password\":\"short\"}",
    "{\"operator_enabled\":true,\"operator_user\":\"admin\",\"operator_password\":\"123456789012\"}",
    "{\"readonly_user\":\"bad name\"}","{\"session_timeout_min\":0}",
    "{\"ntp_server\":\"host name\"}","{\"timezone\":\"a\\u0001b\"}",
    "{\"history_interval_s\":9}","{\"alarm_input_low\":270}",
    "{\"quick_links\":[{\"title\":\"x\",\"url\":\"javascript:alert(1)\"}]}",
    "{\"syslog_enabled\":true,\"syslog_host\":\"\"}","{\"syslog_port\":65536}",
    "{\"schedules\":[{\"id\":1,\"enabled\":true,\"name\":\"unsafe\",\"command\":\"output_off\",\"delay_minutes\":0,\"restore_minutes\":3,\"mode\":\"once\",\"at_utc\":1794000000,\"days_mask\":0,\"minute_of_day\":0}]}"
  };
  for(const auto* input:invalid){out=validBase();const auto unchanged=configJson(out,true);assert(!mergeConfigJson(base,input,out,error));assert(configJson(out,true)==unchanged);}
  assert(mergeConfigJson(base,"{\"operator_enabled\":true,\"operator_password\":\"operator-secret-123\",\"readonly_enabled\":true,\"readonly_password\":\"viewer-secret-123\"}",out,error));
  Config saved=out;
  assert(mergeConfigJson(saved,"{\"operator_password\":\"\",\"readonly_password\":\"\"}",out,error));
  assert(configJson(out,true)==configJson(saved,true));
  DynamicJsonDocument publicDoc(24576);assert(!deserializeJson(publicDoc,configJson(saved,false)));
  assert(publicDoc["operator_password_set"].as<bool>()&&publicDoc["readonly_password_set"].as<bool>());
  assert(configJson(saved,false).find("operator-secret-123")==std::string::npos);
  assert(mergeConfigJson(base,"{\"schedules\":[{\"id\":1,\"enabled\":false,\"name\":\"Weekly test\",\"command\":\"test_short\",\"delay_minutes\":1,\"restore_minutes\":3,\"mode\":\"weekly\",\"at_utc\":0,\"days_mask\":2,\"minute_of_day\":540}]}",out,error));
  DynamicJsonDocument duplicate(24576);assert(!deserializeJson(duplicate,configJson(out,true)));
  duplicate["schedules"].as<JsonArray>().add(duplicate["schedules"][0]);
  assert(!mergeConfigJson(base,json(duplicate),out,error));
  full.remove("timezone");assert(!mergeConfigJson(base,json(full),out,error,true));
  assert(!mergeConfigJson(base,std::string(12289,' '),out,error));
  std::cout<<"config_v02: migration, strict schema, secret retention, invalid atomicity, schedules passed\n";
}
