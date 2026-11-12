#include "config_model.hpp"
#include <cmath>
#include <cctype>
#include <ctime>
#include <limits>
#include <algorithm>
namespace gateway {
uint64_t supportedClockMaxUtc(){return std::min<uint64_t>(4102444799ULL,static_cast<uint64_t>(std::numeric_limits<time_t>::max()));}
bool parseIPv4(const std::string& s, uint32_t& value) {
  if(s.empty() || s.size()>15) return false;
  uint32_t result=0; size_t pos=0;
  for(int part=0;part<4;part++) {
    const size_t start=pos; unsigned v=0;
    while(pos<s.size()&&s[pos]>='0'&&s[pos]<='9') { v=v*10+unsigned(s[pos++]-'0'); if(v>255)return false; }
    if(pos==start || (pos-start>1&&s[start]=='0'))return false;
    result=(result<<8)|v;
    if(part<3) { if(pos==s.size()||s[pos++]!='.')return false; }
  }
  if(pos!=s.size())return false;
  value=result;return true;
}
static bool unicast(uint32_t a) { return (a>>24)>0&&(a>>24)<224&&(a>>24)!=127; }
static bool textOk(const std::string& s,size_t lo,size_t hi) {
  if(s.size()<lo||s.size()>hi)return false;
  for(unsigned char c:s)if(c<32||c==127)return false;
  return true;
}
static bool usernameOk(const std::string& s) {
  if(!textOk(s,1,32))return false;
  for(unsigned char ch:s)if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.'))return false;
  return true;
}
static bool hostOk(const std::string& s) {
  if(s.empty()||s.size()>63||s.front()=='.'||s.back()=='.')return false;
  bool start=true; unsigned char last=0;
  for(unsigned char ch:s){
    if(ch=='.'){if(start||last=='-')return false;start=true;}
    else {if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='-')||(start&&ch=='-'))return false;start=false;}
    last=ch;
  }
  return !start&&last!='-';
}
bool validateConfig(const Config& c, std::string& e) {
  e.clear();
  auto fail=[&](const char* message){ e=message;return false; };
  if(c.schema!=2)return fail("Неподдерживаемая версия конфигурации");
  if(!textOk(c.hostname,1,32)||c.hostname.front()=='-'||c.hostname.back()=='-')return fail("Некорректное имя шлюза");
  for(unsigned char ch:c.hostname)if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='-'))return fail("Имя шлюза: латиница, цифры и дефис");
  if(!textOk(c.location,0,96)||!textOk(c.contact,0,96))return fail("Контакт/расположение: максимум 96 байт без управляющих символов");
  if(!textOk(c.driver,1,32))return fail("Некорректный идентификатор драйвера");
  bool baudOk=false;for(auto b:{1200u,2400u,4800u,9600u,19200u,38400u,57600u,115200u})if(c.baud==b)baudOk=true;
  if(!baudOk)return fail("Неподдерживаемая скорость UART");
  if(c.poll_ms<500||c.poll_ms>60000)return fail("Период опроса: 500–60000 мс");
  if(c.response_timeout_ms<200||c.response_timeout_ms>5000)return fail("Таймаут ответа: 200–5000 мс");
  if(c.stale_ms<c.poll_ms+c.response_timeout_ms||c.stale_ms>300000)return fail("Таймаут связи должен быть не меньше периода опроса плюс таймаут ответа, максимум 300000 мс");
  if(!std::isfinite(c.battery_multiplier)||c.battery_multiplier<0||c.battery_multiplier>1000)return fail("Множитель батареи: 0–1000; 0 отключает пересчёт");
  if(c.power_controls_enabled&&!c.controls_enabled)return fail("Сначала разрешите управление ИБП");
  if(!usernameOk(c.web_user)||!usernameOk(c.operator_user)||!usernameOk(c.readonly_user))return fail("Имя пользователя: 1–32 ASCII буквы, цифры, точка, дефис или подчёркивание");
  if(!(c.web_user=="ups"&&c.web_password=="ups")&&!textOk(c.web_password,12,64))return fail("Пароль: 12–64 байта без управляющих символов");
  if(!textOk(c.operator_password,c.operator_enabled?12:0,64)||!textOk(c.readonly_password,c.readonly_enabled?12:0,64))return fail("Включённому пользователю нужен пароль 12–64 байта");
  if((c.operator_enabled&&c.operator_user==c.web_user)||(c.readonly_enabled&&c.readonly_user==c.web_user)||(c.operator_enabled&&c.readonly_enabled&&c.operator_user==c.readonly_user))return fail("Имена включённых пользователей должны различаться");
  if(!textOk(c.snmp_community,1,64))return fail("SNMP community: 1–64 байта");
  uint32_t address=0,mask=0,gw=0,dns=0;
  if(!c.snmp_manager_ip.empty()&&(!parseIPv4(c.snmp_manager_ip,address)||!unicast(address)))return fail("Некорректный IPv4 SNMP-менеджера");
  if(!c.dhcp) {
    if(!parseIPv4(c.ip,address)||!unicast(address))return fail("Некорректный статический IPv4");
    if(!parseIPv4(c.netmask,mask)||mask==0||((~mask)&((~mask)+1u))!=0)return fail("Некорректная маска подсети");
    const uint32_t host=address&~mask;
    if(~mask>1&&(host==0||host==~mask))return fail("Нельзя использовать адрес сети или broadcast");
    if(!parseIPv4(c.gateway,gw)||(gw!=0&&(!unicast(gw)||(gw&mask)!=(address&mask))))return fail("Шлюз должен находиться в той же подсети, либо 0.0.0.0");
    if(!parseIPv4(c.dns,dns)||(dns!=0&&!unicast(dns)))return fail("Некорректный DNS IPv4");
  }
  for(const auto& t:c.traps)if(t.enabled){
    if(!parseIPv4(t.ip,address)||!unicast(address)||t.port==0)return fail("Для trap нужен unicast IPv4 и порт 1–65535");
    if(!textOk(t.community,1,64))return fail("Trap community: 1–64 байта");
  }
  if(c.session_timeout_min<1||c.session_timeout_min>240)return fail("Таймаут сеанса: 1–240 минут");
  if(!hostOk(c.ntp_server))return fail("NTP сервер: имя узла или IPv4, 1–63 ASCII байта");
  if(!textOk(c.timezone,1,96))return fail("Часовой пояс: POSIX TZ, 1–96 ASCII байт");
  for(unsigned char ch:c.timezone)if(ch>126)return fail("Часовой пояс должен быть ASCII POSIX TZ");
  if(c.date_format!="YYYY-MM-DD"&&c.date_format!="DD.MM.YYYY"&&c.date_format!="MM/DD/YYYY")return fail("Неизвестный формат даты");
  if(c.temperature_unit!="C"&&c.temperature_unit!="F")return fail("Единица температуры: C или F");
  if(c.history_interval_s<10||c.history_interval_s>3600)return fail("Период журнала данных: 10–3600 секунд");
  if(c.default_page!="home"&&c.default_page!="status"&&c.default_page!="events"&&c.default_page!="data")return fail("Неизвестная начальная страница");
  for(const auto& link:c.quick_links){
    if(!textOk(link.title,0,32)||!textOk(link.url,0,192))return fail("Быстрая ссылка: название до 32 байт, URL до 192 байт");
    if(link.title.empty()!=link.url.empty())return fail("Укажите одновременно название и URL ссылки");
    if(!link.url.empty()){
      size_t offset=link.url.compare(0,7,"http://")==0?7:link.url.compare(0,8,"https://")==0?8:0;
      if(!offset||offset==link.url.size())return fail("Быстрая ссылка должна быть HTTP(S) URL");
      for(unsigned char ch:link.url)if(ch<=32||ch==127)return fail("URL не может содержать пробелы");
      if(link.url[offset]=='/'||link.url[offset]=='?'||link.url[offset]=='#')return fail("URL должен содержать имя узла");
    }
  }
  if(c.alarm_load_high<1||c.alarm_load_high>200||c.alarm_temperature_high<1||c.alarm_temperature_high>150||c.alarm_input_low<1||c.alarm_input_low>300||c.alarm_input_high<2||c.alarm_input_high>400||c.alarm_input_low>=c.alarm_input_high||c.alarm_delay_s>300)return fail("Порог тревоги или задержка вне допустимого диапазона");
  if((c.syslog_enabled&&!hostOk(c.syslog_host))||(!c.syslog_host.empty()&&!hostOk(c.syslog_host)))return fail("Syslog сервер: имя узла или IPv4, 1–63 ASCII байта");
  if(c.syslog_port<1||c.syslog_port>65535||c.syslog_min_severity<1||c.syslog_min_severity>3)return fail("Syslog порт/важность вне допустимого диапазона");
  if(c.schedules.size()>8)return fail("Допускается не более 8 расписаний");
  for(size_t i=0;i<c.schedules.size();++i){
    const auto& r=c.schedules[i];
    if(!r.id||r.id>65535||!textOk(r.name,0,48))return fail("Расписание: id 1–65535, название до 48 байт");
    for(size_t j=0;j<i;++j)if(c.schedules[j].id==r.id)return fail("ID расписаний должны различаться");
    if(r.command!="beeper_toggle"&&r.command!="test_short"&&r.command!="test_cancel"&&r.command!="shutdown_cancel"&&r.command!="output_off"&&r.command!="output_restart")return fail("Неизвестная команда расписания");
    if(r.delay_minutes>65535||r.restore_minutes>65535)return fail("Задержки расписания вне диапазона драйвера");
    if((r.command=="output_off"||r.command=="output_restart")&&(r.delay_minutes<1||r.delay_minutes>10))return fail("Задержка отключения: 1–10 минут");
    if(r.command=="output_restart"&&(r.restore_minutes<3||r.restore_minutes>9999))return fail("Задержка восстановления: 3–9999 минут");
    if(r.mode!="once"&&r.mode!="weekly")return fail("Режим расписания: once или weekly");
    if(r.at_utc>supportedClockMaxUtc()||(r.mode=="once"&&r.at_utc<1577836800ULL))return fail("UTC расписания должен поддерживаться системными часами и быть не раньше 2020 года");
    if(r.days_mask>127||r.minute_of_day>1439||(r.mode=="weekly"&&!r.days_mask))return fail("Недельное расписание требует дни и минуту суток 0–1439");
  }
  return true;
}
Config defaultsPreservingAccess(const Config& old,bool keep_network) {
  Config c;
  c.web_user=old.web_user;c.web_password=old.web_password;
  c.operator_enabled=old.operator_enabled;c.operator_user=old.operator_user;c.operator_password=old.operator_password;
  c.readonly_enabled=old.readonly_enabled;c.readonly_user=old.readonly_user;c.readonly_password=old.readonly_password;
  c.snmp_community=old.snmp_community;c.snmp_manager_ip=old.snmp_manager_ip;
  for(size_t i=0;i<c.traps.size();++i)c.traps[i].community=old.traps[i].community;
  if(keep_network){c.dhcp=old.dhcp;c.ip=old.ip;c.netmask=old.netmask;c.gateway=old.gateway;c.dns=old.dns;}
  return c;
}
}

