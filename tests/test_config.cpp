#include "config_model.hpp"
#include "auth_core.hpp"
#include <cassert>
#include <iostream>
#include <limits>
using namespace gateway;
Config base() { Config c; c.web_password="a-long-test-password"; c.snmp_community="read-test"; for(auto& t:c.traps)t.community="trap-test"; return c; }
uint64_t clockForTest(void*) { return 1000; }
bool tokenForTest(void* context,std::string& token) { token=std::string(63,'a')+char('1'+(*static_cast<int*>(context))++); return true; }
int main(){
  uint32_t ip=0; assert(parseIPv4("192.168.1.20",ip)&&ip==0xc0a80114u);
  for(const auto* s:{"1.2.3","256.2.3.4","1.2.3.4.5","1.2.3.-1","1.2.3.04"," 1.2.3.4","1.2.3.4x"})assert(!parseIPv4(s,ip));
  std::string err; auto c=base(); assert(validateConfig(c,err));
  Config factory; factory.snmp_community="read-test"; for(auto& t:factory.traps)t.community="trap-test";
  assert(factory.web_user=="ups"&&factory.web_password=="ups"&&validateConfig(factory,err));
  const std::array<auth::Account,3> factoryAccounts{{
    {true,auth::Role::Admin,factory.web_user,factory.web_password},
    {factory.operator_enabled,auth::Role::Operator,factory.operator_user,factory.operator_password},
    {factory.readonly_enabled,auth::Role::Viewer,factory.readonly_user,factory.readonly_password}
  }};
  int issued=0; auth::SessionStore sessions(60000,{&issued,clockForTest,tokenForTest}); auth::Session session;
  assert(sessions.login(factoryAccounts,"ups","ups","192.0.2.1",session).state==auth::LoginState::Ok);
  assert(session.role==auth::Role::Admin);
  assert(sessions.login(factoryAccounts,"ups","wrong","192.0.2.1",session).state==auth::LoginState::InvalidCredentials);
  factory.location="server room"; assert(validateConfig(factory,err));
  factory.web_user="admin"; assert(!validateConfig(factory,err));
  factory.web_user="ups"; factory.web_password="short"; assert(!validateConfig(factory,err));
  factory.web_password="12345678901"; assert(!validateConfig(factory,err));
  factory.web_password="123456789012"; assert(validateConfig(factory,err));
  c=base(); c.operator_enabled=false; c.operator_password=std::string(65,'x'); std::string longPasswordError; const bool longPasswordValid=validateConfig(c,longPasswordError);
  c=base(); c.operator_enabled=false; c.operator_password="a\tb"; std::string controlPasswordError; const bool controlPasswordValid=validateConfig(c,controlPasswordError);
  assert(!longPasswordValid&&!controlPasswordValid);
  const std::string passwordError="Operator and read-only passwords: up to 64 bytes without control characters; enabled accounts need at least 12 bytes";
  const bool longPasswordMessageCorrect=longPasswordError==passwordError, controlPasswordMessageCorrect=controlPasswordError==passwordError;
  assert(longPasswordMessageCorrect&&controlPasswordMessageCorrect);
  c=base(); c.stale_ms=2500; assert(!validateConfig(c,err));
  c=base(); c.battery_multiplier=std::numeric_limits<float>::quiet_NaN(); assert(!validateConfig(c,err));
  c=base(); c.web_password="short"; assert(!validateConfig(c,err));
  c=base(); c.hostname="bad host"; assert(!validateConfig(c,err)); assert(err=="Gateway name: letters, digits and hyphens only");
  c=base(); c.dhcp=false; c.netmask="255.0.255.0"; assert(!validateConfig(c,err));
  c=base(); c.dhcp=false; c.ip="192.168.1.0"; assert(!validateConfig(c,err));
  c=base(); c.dhcp=false; c.gateway="192.168.2.1"; assert(!validateConfig(c,err));
  c=base(); c.traps[0].enabled=true; c.traps[0].ip="224.0.0.1"; assert(!validateConfig(c,err));
  c.traps[0].ip="192.168.1.2"; assert(validateConfig(c,err));
  c=base(); c.schema=3; assert(!validateConfig(c,err));
  c=base(); c.power_controls_enabled=true; assert(!validateConfig(c,err));
  c=base();c.operator_enabled=true;c.operator_password="operator-secret-123";c.readonly_enabled=true;c.readonly_password="viewer-secret-123";c.dhcp=false;c.ip="10.0.0.3";c.netmask="255.255.255.0";c.gateway="10.0.0.1";c.dns="10.0.0.1";c.controls_enabled=true;
  auto reset=defaultsPreservingAccess(c,true);assert(reset.web_password==c.web_password&&reset.operator_enabled&&reset.readonly_enabled&&reset.operator_password==c.operator_password&&reset.readonly_password==c.readonly_password);
  assert(reset.snmp_community==c.snmp_community&&reset.traps[0].community==c.traps[0].community&&reset.ip=="10.0.0.3"&&!reset.controls_enabled);
  reset=defaultsPreservingAccess(c,false);assert(reset.dhcp&&reset.ip=="192.168.1.50"&&reset.web_password==c.web_password);assert(validateConfig(reset,err));
  std::cout<<"config: all assertions passed\n";
}
