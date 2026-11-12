#include "config_json.hpp"
#include <ArduinoJson.h>
#include <cassert>
#include <iostream>
using namespace gateway;
static Config base() {
  Config c; c.web_user="admin"; c.web_password="password-at-least-twenty"; c.snmp_community="unique-snmp-community";
  for(auto& t:c.traps)t.community=c.snmp_community;
  return c;
}
int main() {
  auto c=base(); Config out=base(); std::string e;
  assert(mergeConfigJson(c,"{\"hostname\":\"ups-office\",\"poll_ms\":3000}",out,e));
  assert(out.hostname=="ups-office"&&out.poll_ms==3000&&c.hostname=="ups-gateway");
  assert(mergeConfigJson(c,"{\"web_password\":\"\",\"snmp_community\":\"\",\"traps\":[{\"community\":\"\"},{}]}",out,e));
  assert(out.web_password==c.web_password&&out.snmp_community==c.snmp_community&&out.traps[0].community==c.traps[0].community);
  const auto publicJson=configJson(c,false);
  assert(publicJson.find(c.web_password)==std::string::npos&&publicJson.find(c.snmp_community)==std::string::npos);
  DynamicJsonDocument redacted(8192); assert(!deserializeJson(redacted,publicJson));
  assert(redacted["web_password"].as<std::string>().empty()&&redacted["web_password_set"].as<bool>());
  assert(redacted["traps"][0]["community_set"].as<bool>());
  assert(mergeConfigJson(c,publicJson,out,e)&&configJson(out,true)==configJson(c,true));
  assert(mergeConfigJson(Config{},configJson(c,true),out,e,true));
  assert(configJson(out,true)==configJson(c,true));
  Config factory; factory.snmp_community="private-community"; for(auto& target:factory.traps)target.community=factory.snmp_community;
  assert(mergeConfigJson(Config{},configJson(factory,true),out,e,true));
  assert(out.web_user=="ups"&&out.web_password=="ups");
  assert(mergeConfigJson(out,"{\"location\":\"server room\",\"web_password\":\"\"}",out,e));
  assert(out.location=="server room"&&out.web_user=="ups"&&out.web_password=="ups");
  assert(!mergeConfigJson(out,"{\"web_user\":\"admin\"}",c,e));
  assert(!mergeConfigJson(out,"{\"web_password\":\"short\"}",c,e));
  const char* invalid[]={
    "[]","null","123","{} trailing","{}{}","{\"poll_ms\":\"2000\"}","{\"poll_ms\":true}",
    "{\"poll_ms\":2000.5}","{\"poll_ms\":-1}","{\"poll_ms\":4294969296}","{\"dhcp\":1}",
    "{\"dhcp\":\"false\"}","{\"hostname\":null}","{\"hostname\":[]}","{\"hostname\":12}",
    "{\"ip\":\"1234567890123456\"}","{\"ip\":\"a\\nb\"}",
    "{\"traps\":[{\"ip\":\"a\\tb\"},{}]}",
    "{\"hostname\":\"a\\u0000b\"}","{\"schema\":3}","{\"schema\":\"1\"}","{\"schema\":1.0}",
    "{\"driver\":\"missing\"}","{\"battery_multiplier\":true}","{\"battery_multiplier\":\"5\"}",
    "{\"battery_multiplier\":-1}","{\"battery_multiplier\":1001}","{\"battery_multiplier\":1e100}",
    "{\"traps\":[]}","{\"traps\":[{},null]}","{\"traps\":[{},{} ,{}]}",
    "{\"traps\":[{\"port\":65536},{}]}","{\"traps\":[{\"port\":-1},{}]}",
    "{\"traps\":[{\"port\":4294967458},{}]}","{\"traps\":[{\"port\":\"162\"},{}]}",
    "{\"traps\":[{\"port\":0},{}]}","{\"web_password\":\"short\"}",
    "{\"traps\":[{\"enabled\":true,\"ip\":\"224.1.1.1\"},{}]}"
  };
  for(const auto* json:invalid) {
    out=c; auto before=configJson(out,true);
    if(mergeConfigJson(c,json,out,e)){std::cerr<<"Unexpected acceptance: "<<json<<"\n";return 1;}
    assert(!e.empty()); assert(configJson(out,true)==before);
  }
  assert(!mergeConfigJson(c,std::string(12289,' '),out,e));
  assert(!mergeConfigJson(c,"{}",out,e,true));
  auto incomplete=configJson(c,true); DynamicJsonDocument d(8192); assert(!deserializeJson(d,incomplete));
  d.remove("schema"); incomplete.clear();serializeJson(d,incomplete);assert(!mergeConfigJson(c,incomplete,out,e,true));
  d.clear();assert(!deserializeJson(d,configJson(c,true)));d["web_password"]="";incomplete.clear();serializeJson(d,incomplete);
  assert(!mergeConfigJson(c,incomplete,out,e,true));
  d.clear();assert(!deserializeJson(d,configJson(c,true)));d["traps"][0]["community"]="";incomplete.clear();serializeJson(d,incomplete);
  assert(!mergeConfigJson(c,incomplete,out,e,true));
  assert(mergeConfigJson(c,"{\"battery_multiplier\":2.5,\"traps\":[{\"port\":65535},{}]}",out,e));
  assert(out.battery_multiplier==2.5&&out.traps[0].port==65535);
  std::cout<<"config_json: all assertions passed (strict types, ranges, secrets, import, rollback)\n";
}
