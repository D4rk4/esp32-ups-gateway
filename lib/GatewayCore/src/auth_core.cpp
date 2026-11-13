#include "auth_core.hpp"
#include "config_model.hpp"
#include <algorithm>
#include <limits>
namespace gateway { namespace auth {
namespace {
constexpr uint64_t kFailureWindow=60000;
bool fixedEqual(std::string_view a,std::string_view b){
  if(a.size()>128||b.size()>128)return false;
  unsigned difference=unsigned(a.size()^b.size());
  for(size_t i=0;i<128;++i)difference|=unsigned(i<a.size()?uint8_t(a[i]):0)^unsigned(i<b.size()?uint8_t(b[i]):0);
  return difference==0;
}
bool validToken(std::string_view value){
  if(value.size()!=64)return false;
  for(char ch:value)if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')))return false;
  return true;
}
uint32_t secondsLeft(uint64_t elapsed){return uint32_t((kFailureWindow-elapsed+999)/1000);}
bool space(char c){return c==' '||c=='\t'||c=='\r'||c=='\n';}
int hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
bool decode(std::string_view raw,std::string& result,bool query,size_t limit){
  result.clear();
  for(size_t i=0;i<raw.size();++i){
    unsigned char ch=static_cast<unsigned char>(raw[i]);
    if(ch=='%'){
      if(i+2>=raw.size()||hex(raw[i+1])<0||hex(raw[i+2])<0)return false;
      ch=static_cast<unsigned char>((hex(raw[i+1])<<4)|hex(raw[i+2]));i+=2;
    }else if(ch=='+'&&query)ch=' ';
    if(ch<0x20||ch==0x7f||result.size()>=limit)return false;
    result.push_back(char(ch));
  }
  return true;
}
// ArduinoJson deliberately accepts some JavaScript extensions. The HTTP
// contract accepts standard JSON only, before passing it to that parser.
class StrictJson {
 public:
  explicit StrictJson(const std::string& text):text_(text){}
  bool object(){skip();if(at_==text_.size()||text_[at_]!='{'||!value(0))return false;skip();return at_==text_.size();}
 private:
  void skip(){while(at_<text_.size()&&space(text_[at_]))++at_;}
  bool take(char ch){skip();if(at_==text_.size()||text_[at_]!=ch)return false;++at_;return true;}
  bool unicode(uint32_t& code){
    code=0;for(unsigned i=0;i<4;++i){if(at_==text_.size()||hex(text_[at_])<0)return false;code=(code<<4)|uint32_t(hex(text_[at_++]));}return code!=0;
  }
  bool string(){
    if(!take('"'))return false;
    while(at_<text_.size()){
      const auto ch=static_cast<unsigned char>(text_[at_++]);if(ch=='"')return true;if(ch<0x20)return false;
      if(ch=='\\'){
        if(at_==text_.size())return false;
        const char escape=text_[at_++];
        if(escape=='u'){
          uint32_t code;if(!unicode(code))return false;
          if(code>=0xd800&&code<=0xdbff){
            if(at_+2>text_.size()||text_[at_]!='\\'||text_[at_+1]!='u')return false;
            at_+=2;
            if(!unicode(code)||code<0xdc00||code>0xdfff)return false;
          }else if(code>=0xdc00&&code<=0xdfff)return false;
        }else if(escape!='"'&&escape!='\\'&&escape!='/'&&escape!='b'&&escape!='f'&&escape!='n'&&escape!='r'&&escape!='t')return false;
      }else if(ch>=0x80){
        unsigned count=0;uint32_t code=0,minimum=0;
        if(ch>=0xc2&&ch<=0xdf){count=1;code=ch&31;minimum=0x80;}
        else if(ch>=0xe0&&ch<=0xef){count=2;code=ch&15;minimum=0x800;}
        else if(ch>=0xf0&&ch<=0xf4){count=3;code=ch&7;minimum=0x10000;}
        else return false;
        for(unsigned i=0;i<count;++i){if(at_==text_.size())return false;const auto next=static_cast<unsigned char>(text_[at_++]);if((next&0xc0)!=0x80)return false;code=(code<<6)|(next&63);}
        if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
      }
    }
    return false;
  }
  bool number(){
    if(at_<text_.size()&&text_[at_]=='-')++at_;
    if(at_==text_.size())return false;
    if(text_[at_]=='0')++at_;
    else{if(text_[at_]<'1'||text_[at_]>'9')return false;while(at_<text_.size()&&text_[at_]>='0'&&text_[at_]<='9')++at_;}
    if(at_<text_.size()&&text_[at_]=='.'){++at_;const size_t digits=at_;while(at_<text_.size()&&text_[at_]>='0'&&text_[at_]<='9')++at_;if(at_==digits)return false;}
    if(at_<text_.size()&&(text_[at_]=='e'||text_[at_]=='E')){++at_;if(at_<text_.size()&&(text_[at_]=='+'||text_[at_]=='-'))++at_;const size_t digits=at_;while(at_<text_.size()&&text_[at_]>='0'&&text_[at_]<='9')++at_;if(at_==digits)return false;}
    return true;
  }
  bool value(unsigned depth){
    skip();if(depth>16||at_==text_.size())return false;const char ch=text_[at_];
    if(ch=='"')return string();
    if(ch=='{'||ch=='['){
      ++at_;skip();const char end=ch=='{'?'}':']';if(take(end))return true;
      for(;;){
        if(ch=='{'&&(!string()||!take(':')))return false;
        if(!value(depth+1))return false;
        if(take(end))return true;
        if(!take(','))return false;
      }
    }
    for(const char* literal:{"true","false","null"}){
      const std::string_view word(literal);if(std::string_view(text_).substr(at_,word.size())==word){at_+=word.size();return true;}
    }
    return number();
  }
  const std::string& text_;size_t at_{0};
};
}
const char* roleName(Role role){
  switch(role){case Role::Admin:return "admin";case Role::Operator:return "operator";case Role::Viewer:return "viewer";default:return "none";}
}
SessionStore::SessionStore(uint64_t timeout,Callbacks callbacks):timeout_ms_(std::max<uint64_t>(1000,timeout)),callbacks_(callbacks){}
uint64_t SessionStore::now()const{return callbacks_.clock?callbacks_.clock(callbacks_.context):0;}
void SessionStore::expire(uint64_t timestamp){
  for(auto& session:sessions_)if(session.role!=Role::None&&(timestamp<session.last_activity_ms||timestamp-session.last_activity_ms>=timeout_ms_))session={};
}
uint32_t SessionStore::retryAfter(const std::string& peer,uint64_t timestamp){
  if(global_failures_&&(timestamp<global_since_||timestamp-global_since_>=kFailureWindow))global_failures_=0;
  uint32_t retry=global_failures_>=20?secondsLeft(timestamp-global_since_):0;
  bool available=false,known=false;uint32_t earliest=60;
  for(auto& failure:failures_){
    if(failure.count&&(timestamp<failure.since||timestamp-failure.since>=kFailureWindow))failure={};
    if(!failure.count){available=true;continue;}
    const uint32_t left=secondsLeft(timestamp-failure.since);earliest=std::min(earliest,left);
    if(failure.peer==peer){known=true;if(failure.count>=5)retry=std::max(retry,left);}
  }
  if(!known&&!available)retry=std::max(retry,earliest);
  return retry;
}
void SessionStore::recordFailure(const std::string& peer,uint64_t timestamp){
  if(!global_failures_)global_since_=timestamp;
  if(global_failures_<20)++global_failures_;
  Failure* target=nullptr;
  for(auto& failure:failures_)if(failure.count&&failure.peer==peer){target=&failure;break;}
  if(!target)for(auto& failure:failures_)if(!failure.count){target=&failure;break;}
  if(target){if(!target->count){target->peer=peer;target->since=timestamp;}if(target->count<5)++target->count;}
}
LoginResult SessionStore::login(const std::array<Account,3>& accounts,std::string_view username,
                                std::string_view password,const std::string& peer,Session& result){
  result={};const uint64_t timestamp=now();expire(timestamp);
  const std::string key=peer.empty()||peer.size()>64?"unknown":peer;
  const uint32_t retry=retryAfter(key,timestamp);if(retry)return {LoginState::Throttled,retry};
  const Account* matched=nullptr;
  for(const auto& account:accounts){
    const bool name_ok=fixedEqual(username,account.username),password_ok=fixedEqual(password,account.password);
    if(account.enabled&&account.role!=Role::None&&!account.password.empty()&&name_ok&&password_ok)matched=&account;
  }
  if(username.empty()||username.size()>32||password.empty()||password.size()>64||!matched){recordFailure(key,timestamp);return {LoginState::InvalidCredentials,0};}
  Session* slot=nullptr;for(auto& session:sessions_)if(session.role==Role::None){slot=&session;break;}
  if(!slot)return {LoginState::Capacity,0};
  auto uniqueToken=[&](std::string& out,const std::string& other){
    for(unsigned attempt=0;attempt<4;++attempt){
      if(!callbacks_.token||!callbacks_.token(callbacks_.context,out)||!validToken(out))return false;
      bool unique=out!=other;
      for(const auto& session:sessions_)if(session.role!=Role::None&&(out==session.token||out==session.csrf))unique=false;
      if(unique)return true;
    }
    return false;
  };
  Session next;if(!uniqueToken(next.token,"")||!uniqueToken(next.csrf,next.token))return {LoginState::TokenFailure,0};
  next.username=std::string(matched->username);next.role=matched->role;next.last_activity_ms=timestamp;
  *slot=std::move(next);result=*slot;
  for(auto& failure:failures_)if(failure.peer==key)failure={};
  return {LoginState::Ok,0};
}
const Session* SessionStore::find(const std::string& token,bool activity){
  const uint64_t timestamp=now();expire(timestamp);if(!validToken(token))return nullptr;
  for(auto& session:sessions_)if(session.role!=Role::None&&fixedEqual(session.token,token)){
    if(activity)session.last_activity_ms=timestamp;
    return &session;
  }
  return nullptr;
}
bool SessionStore::csrfValid(const std::string& token,const std::string& csrf){
  const auto* session=find(token);return session&&validToken(csrf)&&fixedEqual(session->csrf,csrf);
}
void SessionStore::logout(const std::string& token){for(auto& session:sessions_)if(!token.empty()&&fixedEqual(session.token,token))session={};}
void SessionStore::invalidateAll(){for(auto& session:sessions_)session={};}
const std::array<Route,27>& routes(){
  static const std::array<Route,27> value{{
    {Method::Get,"/",Endpoint::Root,Role::None,false,0},
    {Method::Get,"/api/session",Endpoint::Session,Role::None,false,0},
    {Method::Get,"/api/status",Endpoint::Status,Role::Viewer,false,0},
    {Method::Get,"/api/config",Endpoint::Config,Role::Admin,false,0},
    {Method::Get,"/api/config/export",Endpoint::ConfigExport,Role::Admin,false,0},
    {Method::Get,"/api/firmware",Endpoint::Firmware,Role::Admin,false,0},
    {Method::Get,"/api/events",Endpoint::Events,Role::Viewer,false,0},
    {Method::Get,"/api/events.csv",Endpoint::EventsCsv,Role::Viewer,false,0},
    {Method::Get,"/api/capabilities",Endpoint::Capabilities,Role::Viewer,false,0},
    {Method::Get,"/api/alarms",Endpoint::Alarms,Role::Viewer,false,0},
    {Method::Get,"/api/history",Endpoint::History,Role::Viewer,false,0},
    {Method::Get,"/api/history.csv",Endpoint::HistoryCsv,Role::Viewer,false,0},
    {Method::Get,"/api/time",Endpoint::Time,Role::Viewer,false,0},
    {Method::Get,"/api/schedules",Endpoint::Schedules,Role::Viewer,false,0},
    {Method::Get,"/api/notifications",Endpoint::Notifications,Role::Viewer,false,0},
    {Method::Get,"/api/about",Endpoint::About,Role::Viewer,false,0},
    {Method::Post,"/api/login",Endpoint::Login,Role::None,false,2048},
    {Method::Post,"/api/logout",Endpoint::Logout,Role::Viewer,true,2048},
    {Method::Post,"/api/config",Endpoint::Config,Role::Admin,true,12288},
    {Method::Post,"/api/firmware",Endpoint::Firmware,Role::Admin,true,0},
    {Method::Post,"/api/command",Endpoint::Command,Role::Operator,true,2048},
    {Method::Post,"/api/trap-test",Endpoint::TrapTest,Role::Admin,true,2048},
    {Method::Post,"/api/syslog-test",Endpoint::SyslogTest,Role::Admin,true,2048},
    {Method::Post,"/api/reboot",Endpoint::Reboot,Role::Admin,true,2048},
    {Method::Post,"/api/logs/clear",Endpoint::LogsClear,Role::Admin,true,2048},
    {Method::Post,"/api/time",Endpoint::Time,Role::Admin,true,2048},
    {Method::Post,"/api/reset-config",Endpoint::ResetConfig,Role::Admin,true,2048}
  }};return value;
}
const Route* routeFor(Method method,const std::string& path){for(const auto& route:routes())if(route.method==method&&path==route.path)return &route;return nullptr;}
Access authorize(const Route& route,const Session* session,bool csrf_valid){
  if(route.minimum==Role::None)return Access::Allowed;
  if(!session||session->role==Role::None)return Access::Unauthenticated;
  if(static_cast<unsigned>(session->role)<static_cast<unsigned>(route.minimum))return Access::Forbidden;
  if(route.csrf&&!csrf_valid)return Access::InvalidCsrf;
  return Access::Allowed;
}
const std::string* RequestTarget::get(const std::string& key)const{for(size_t i=0;i<count;++i)if(query[i].key==key)return &query[i].value;return nullptr;}
bool parseTarget(const std::string& uri,RequestTarget& result,std::string& error){
  result={};error.clear();
  auto fail=[&](){result={};error="Malformed or oversized request target";return false;};
  if(uri.empty()||uri.front()!='/'||uri.size()>512||uri.find('#')!=std::string::npos)return fail();
  const size_t q=uri.find('?');
  if(!decode(std::string_view(uri).substr(0,q),result.path,false,128))return fail();
  if(result.path.find('?')!=std::string::npos||result.path.find('#')!=std::string::npos)return fail();
  if(q==std::string::npos||q+1==uri.size())return true;
  size_t start=q+1;
  while(start<uri.size()){
    if(result.count>=result.query.size())return fail();
    const size_t end=uri.find('&',start),length=(end==std::string::npos?uri.size():end)-start;
    const std::string_view pair(uri.data()+start,length);const size_t equal=pair.find('=');
    Query item;
    if(equal==std::string::npos||!decode(pair.substr(0,equal),item.key,true,32)||item.key.empty()||!decode(pair.substr(equal+1),item.value,true,192)||result.get(item.key))return fail();
    result.query[result.count++]=std::move(item);
    if(end==std::string::npos)break;
    start=end+1;if(start==uri.size())return fail();
  }
  return true;
}
bool isSingleJsonObject(const std::string& body){
  return body.size()<=12288&&StrictJson(body).object();
}
std::string extractSessionCookie(const std::string& cookie){
  if(cookie.size()>1024)return {};
  std::string token;bool found=false;size_t start=0;
  while(start<cookie.size()){
    const size_t end=cookie.find(';',start);size_t last=end==std::string::npos?cookie.size():end;
    while(start<last&&(cookie[start]==' '||cookie[start]=='\t'))++start;
    while(last>start&&(cookie[last-1]==' '||cookie[last-1]=='\t'))--last;
    const std::string_view pair(cookie.data()+start,last-start);const size_t equal=pair.find('=');
    if(equal!=std::string::npos&&pair.substr(0,equal)=="ups_session"){
      if(found||!validToken(pair.substr(equal+1)))return {};
      found=true;token=std::string(pair.substr(equal+1));
    }
    if(end==std::string::npos)break;
    start=end+1;
  }
  return token;
}
std::string csvCell(std::string_view value){
  std::string out="\"";size_t first=0;while(first<value.size()&&space(value[first]))++first;
  if((first<value.size()&&(value[first]=='='||value[first]=='+'||value[first]=='-'||value[first]=='@'))||(!value.empty()&&(value.front()=='\t'||value.front()=='\r'||value.front()=='\n')))out+='\'';
  for(char c:value){if(c=='"')out+='"';out+=c;}
  out+='"';return out;
}
bool powerConfirmationRequired(const Config& saved,const Config& candidate){
  for(const auto& rule:candidate.schedules){
    if(!rule.enabled||(rule.command!="output_off"&&rule.command!="output_restart"&&rule.command!="shutdown_cancel"))continue;
    if((!saved.controls_enabled&&candidate.controls_enabled)||(!saved.power_controls_enabled&&candidate.power_controls_enabled)||saved.driver!=candidate.driver||(rule.mode=="weekly"&&saved.timezone!=candidate.timezone))return true;
    const ScheduleRule* previous=nullptr;for(const auto& old:saved.schedules)if(old.id==rule.id){previous=&old;break;}
    if(!previous||!previous->enabled||previous->name!=rule.name||previous->command!=rule.command||previous->delay_minutes!=rule.delay_minutes||previous->restore_minutes!=rule.restore_minutes||previous->mode!=rule.mode||previous->at_utc!=rule.at_utc||previous->days_mask!=rule.days_mask||previous->minute_of_day!=rule.minute_of_day)return true;
  }
  return false;
}
}}
