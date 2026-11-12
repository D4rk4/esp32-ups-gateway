#include "auth_core.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <string>

using namespace gateway::auth;
namespace {
int failed=0;
void check(bool ok,const char* name){if(!ok){++failed;std::cerr<<"FAIL: "<<name<<'\n';}}
struct Fake {
  uint64_t now{1000};uint32_t counter{0};bool fail_tokens{false};
  static uint64_t clock(void* p){return static_cast<Fake*>(p)->now;}
  static bool token(void* p,std::string& out){
    auto& f=*static_cast<Fake*>(p);if(f.fail_tokens)return false;
    char tail[9];std::snprintf(tail,sizeof(tail),"%08x",++f.counter);
    out=std::string(56,'a')+tail;return true;
  }
  Callbacks callbacks(){return {this,clock,token};}
};
const std::array<Account,3> accounts{{
  {true,Role::Admin,"admin","admin-passphrase-123"},
  {true,Role::Operator,"operator","operator-passphrase-123"},
  {true,Role::Viewer,"viewer","viewer-passphrase-123"}
}};
void expiry_and_logout(){
  Fake f;SessionStore store(60000,f.callbacks());Session session;
  const auto first=store.login(accounts,"admin","admin-passphrase-123","192.0.2.1",session);
  check(first.state==LoginState::Ok,"valid credentials create a session");
  check(session.token.size()==64&&session.csrf.size()==64&&session.token!=session.csrf,"independent cookie and CSRF tokens");
  f.now=60999;check(store.find(session.token)!=nullptr,"session survives before idle deadline");
  f.now=61000;check(store.find(session.token)==nullptr,"background reads do not extend idle deadline");
  check(!store.csrfValid(session.token,session.csrf),"expired CSRF is rejected");
  Session touched;store.login(accounts,"viewer","viewer-passphrase-123","192.0.2.1",touched);
  f.now=90000;check(store.find(touched.token,true)!=nullptr,"user activity touches a valid session");
  f.now=120999;check(store.find(touched.token)!=nullptr,"touch extends idle lifetime");
  check(store.csrfValid(touched.token,touched.csrf),"matching per-session CSRF is accepted");
  check(!store.csrfValid(touched.token,session.csrf),"another session's CSRF is rejected");
  check(!store.csrfValid(touched.token,touched.csrf+"0"),"CSRF suffix is rejected");
  store.logout(touched.token);check(store.find(touched.token)==nullptr,"logout destroys session");
  check(!store.csrfValid(touched.token,touched.csrf),"logout destroys CSRF authorization");
}
void roles_credentials_and_capacity(){
  Fake f;SessionStore store(60000,f.callbacks());Session result;
  check(store.login(accounts,"operator","operator-passphrase-123","peer1",result).state==LoginState::Ok&&result.role==Role::Operator,"operator identity cannot become admin");
  auto disabled=accounts;disabled[2].enabled=false;
  check(store.login(disabled,"viewer","viewer-passphrase-123","peer2",result).state==LoginState::InvalidCredentials,"disabled account is rejected");
  check(store.login(accounts,"admin","ADMIN-passphrase-123","peer3",result).state==LoginState::InvalidCredentials,"password is case-sensitive");
  check(store.login(accounts,std::string(33,'a'),"admin-passphrase-123","peer4",result).state==LoginState::InvalidCredentials,"oversized login name is rejected");
  SessionStore bounded(60000,f.callbacks());std::array<Session,SessionStore::kCapacity> sessions;
  for(auto& s:sessions)check(bounded.login(accounts,"admin","admin-passphrase-123","peer",s).state==LoginState::Ok,"free session slot is usable");
  check(bounded.login(accounts,"admin","admin-passphrase-123","peer",result).state==LoginState::Capacity,"session table cannot grow without limit");
  check(bounded.find(sessions.front().token)!=nullptr,"capacity exhaustion never evicts another login");
  bounded.logout(sessions.front().token);
  check(bounded.login(accounts,"viewer","viewer-passphrase-123","peer",result).state==LoginState::Ok,"logout frees bounded slot");
  bounded.invalidateAll();check(bounded.find(result.token)==nullptr,"local credential reset can invalidate sessions");
  f.fail_tokens=true;SessionStore bad_random(60000,f.callbacks());
  check(bad_random.login(accounts,"admin","admin-passphrase-123","peer",result).state==LoginState::TokenFailure,"randomness failure cannot create a predictable session");
}
void throttle(){
  Fake f;SessionStore store(60000,f.callbacks());Session result;
  for(int i=0;i<5;++i)check(store.login(accounts,"admin","wrong","192.0.2.1",result).state==LoginState::InvalidCredentials,"first bounded login failures are recorded");
  auto blocked=store.login(accounts,"admin","admin-passphrase-123","192.0.2.1",result);
  check(blocked.state==LoginState::Throttled&&blocked.retry_after_s==60,"sixth attempt is blocked even with valid credentials");
  check(store.login(accounts,"viewer","viewer-passphrase-123","192.0.2.2",result).state==LoginState::Ok,"single-peer throttle does not block other peers");
  f.now=60999;blocked=store.login(accounts,"admin","wrong","192.0.2.1",result);
  check(blocked.state==LoginState::Throttled&&blocked.retry_after_s==1,"throttle boundary rounds retry interval up");
  f.now=61000;check(store.login(accounts,"admin","admin-passphrase-123","192.0.2.1",result).state==LoginState::Ok,"throttle expires at window boundary");
  Fake f2;SessionStore bounded(60000,f2.callbacks());
  for(size_t i=0;i<SessionStore::kThrottleCapacity;++i)check(bounded.login(accounts,"admin","wrong","peer"+std::to_string(i),result).state==LoginState::InvalidCredentials,"new peer uses one bounded throttle slot");
  check(bounded.login(accounts,"admin","wrong","new-peer",result).state==LoginState::Throttled,"new peers cannot evict live throttle records");
  for(int i=0;i<5;++i)check(bounded.login(accounts,"admin","wrong","new-peer-"+std::to_string(i),result).state==LoginState::Throttled,"peer rotation cannot grow or bypass the bounded throttle");
  f2.now=61000;check(bounded.login(accounts,"admin","admin-passphrase-123","new-peer",result).state==LoginState::Ok,"expired throttle slots are reclaimed");
  Fake f3;SessionStore global(60000,f3.callbacks());
  for(int p=0;p<4;++p)for(int i=0;i<5;++i)global.login(accounts,"admin","wrong","peer"+std::to_string(p),result);
  check(global.login(accounts,"admin","admin-passphrase-123","peer-new",result).state==LoginState::Throttled,"global failure ceiling blocks distributed guesses");
}
}
int main(){expiry_and_logout();roles_credentials_and_capacity();throttle();if(failed)return 1;std::cout<<"auth: session expiry, CSRF, roles, bounded sessions and throttle passed\n";}
