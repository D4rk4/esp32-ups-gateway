#include "powernet_mib.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace gateway {
// Source: APC-authored PowerNet-MIB, https://raw.githubusercontent.com/librenms/librenms/master/mibs/apc/PowerNet-MIB
// Only a measured, typed subset is implemented. No private enterprise number is claimed.
PowerNetMib::PowerNetMib(const Config& c,const ups::Snapshot& s,uint32_t uptime,const std::string& firmware){
  const std::string root="1.3.6.1.4.1.318.1.1.1.";
  auto add=[&](const std::string& oid,snmp::Value value){snmp::Oid o;if(snmp::parseOid(oid,o))entries_.push_back({std::move(o),std::move(value)});};
  auto text=[&](const std::string& oid,const std::string& text){snmp::Value v;v.type=snmp::Type::OctetString;v.text=text;add(oid,v);};
  auto integer=[&](const std::string& oid,int32_t x){snmp::Value v;v.type=snmp::Type::Integer;v.integer=x;add(oid,v);};
  auto unsignedValue=[&](const std::string& oid,uint32_t x,snmp::Type type=snmp::Type::Gauge32){snmp::Value v;v.type=type;v.unsigned_value=x;add(oid,v);};
  auto metric=[&](const char* suffix,const ups::Metric& m,float scale=1.0f,bool signed_value=false,bool clamp_zero=false){
    if(!s.connected||!m.valid||!std::isfinite(m.value))return;
    double x=double(m.value)*scale;if(clamp_zero&&x<0)x=0;
    if(x<0||x>double(std::numeric_limits<int32_t>::max()))return;
    const uint32_t rounded=uint32_t(std::llround(x));
    if(signed_value)integer(root+suffix,int32_t(rounded));else unsignedValue(root+suffix,rounded);
  };
  text("1.3.6.1.2.1.1.1.0","ESP32 UPS Gateway/"+firmware+"; driver="+c.driver+"; PowerNet subset");
  snmp::Value identity;identity.type=snmp::Type::ObjectId;identity.oid={0,0};add("1.3.6.1.2.1.1.2.0",identity);
  unsignedValue("1.3.6.1.2.1.1.3.0",uptime,snmp::Type::TimeTicks);
  text("1.3.6.1.2.1.1.4.0",c.contact);text("1.3.6.1.2.1.1.5.0",c.hostname);text("1.3.6.1.2.1.1.6.0",c.location);integer("1.3.6.1.2.1.1.7.0",72);
  text(root+"1.1.1.0",s.model.empty()?"UPS via "+c.driver:s.model);
  text(root+"1.1.2.0",c.hostname);
  if(!s.firmware.empty())text(root+"1.2.1.0",s.firmware);
  if(!s.serial.empty())text(root+"1.2.3.0",s.serial);
  integer(root+"8.1.0",s.connected?1:2);
  integer(root+"2.1.1.0",(!s.connected||!s.status_valid)?1:(s.low_battery?3:2));
  int32_t mode=1;
  if(s.connected&&s.status_valid){
    if(s.test_running)mode=15;
    else switch(s.output_mode){case ups::OutputMode::Online:mode=2;break;case ups::OutputMode::OnBattery:mode=3;break;case ups::OutputMode::Bypass:mode=9;break;case ups::OutputMode::Off:mode=7;break;default:break;}
  }
  integer(root+"4.1.1.0",mode);
  metric("3.2.1.0",s.input_voltage);metric("3.3.1.0",s.input_voltage,10);
  metric("3.2.4.0",s.input_frequency);metric("3.3.4.0",s.input_frequency,10);
  metric("4.2.1.0",s.output_voltage);metric("4.3.1.0",s.output_voltage,10);
  metric("4.2.3.0",s.load_percent);metric("4.3.3.0",s.load_percent,10);
  metric("4.2.2.0",s.output_frequency);metric("4.3.2.0",s.output_frequency,10);
  // Historical object names contain 'Battery'; their MIB descriptions explicitly say internal UPS temperature.
  metric("2.2.2.0",s.ups_temperature,1,false,true);metric("2.3.2.0",s.ups_temperature,10,false,true);
  metric("2.2.8.0",s.battery_voltage,1,true);metric("2.3.4.0",s.battery_voltage,10,true);
  metric("2.2.1.0",s.battery_charge);
  if(s.connected&&s.runtime_seconds.valid&&std::isfinite(s.runtime_seconds.value)&&s.runtime_seconds.value>=0&&double(s.runtime_seconds.value)*100<4294967295.0)
    unsignedValue(root+"2.2.3.0",uint32_t(std::llround(double(s.runtime_seconds.value)*100)),snmp::Type::TimeTicks);
  text("1.3.6.1.4.1.318.2.3.3.0","");
  std::sort(entries_.begin(),entries_.end(),[](const snmp::VarBind&a,const snmp::VarBind&b){return a.oid<b.oid;});
}
bool PowerNetMib::get(const snmp::Oid& oid,snmp::Value& value)const{
  auto it=std::lower_bound(entries_.begin(),entries_.end(),oid,[](const snmp::VarBind&v,const snmp::Oid&o){return v.oid<o;});
  if(it==entries_.end()||it->oid!=oid)return false;
  value=it->value;
  return true;
}
bool PowerNetMib::next(const snmp::Oid& oid,snmp::Oid& next,snmp::Value& value)const{
  auto it=std::upper_bound(entries_.begin(),entries_.end(),oid,[](const snmp::Oid&o,const snmp::VarBind&v){return o<v.oid;});
  if(it==entries_.end())return false;
  next=it->oid;value=it->value;
  return true;
}
}
