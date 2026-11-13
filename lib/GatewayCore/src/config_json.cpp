#include "config_json.hpp"
#include "driver_registry.hpp"
#include <ArduinoJson.h>
#include <cmath>
#include <utility>

namespace gateway {
namespace {
constexpr size_t kMaxJson = kMaxConfigJson;
constexpr size_t kDocumentCapacity = kConfigDocumentCapacity;

bool fieldError(std::string& error, const char* key, const char* detail) {
  error = std::string("Field ") + key + ": " + detail;
  return false;
}
// ArduinoJson stops reading after the first document. Reject trailing data as
// well, including a second document; quoted braces and escapes remain valid.
bool oneDocument(const std::string& text) {
  size_t depth = 0;
  bool quoted = false, escaped = false, started = false, finished = false;
  for (const unsigned char c : text) {
    if (finished) {
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
      continue;
    }
    if (!started) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
      if (c != '{') return false;
      started = true;
    }
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') {
      if (!depth) return false;
      if (--depth == 0) finished = true;
    }
  }
  return finished && !quoted;
}

bool stringField(JsonObjectConst object, const char* key, std::string& target,
                 bool secret, bool importing, std::string& error) {
  if (!object.containsKey(key)) {
    return !importing || fieldError(error, key, "missing from saved configuration");
  }
  const auto value = object[key];
  if (!value.is<const char*>()) return fieldError(error, key, "expected a string");
  const JsonString string = value.as<JsonString>();
  const std::string parsed(string.c_str(), string.size());
  if (secret && parsed.empty()) {
    if (importing) return fieldError(error, key, "secret must not be empty");
    return true; // A blank field in the redacted edit form retains its secret.
  }
  // Keep the exact byte length: embedded NUL must not silently truncate strings.
  for (const unsigned char c : parsed)
    if (c < 32 || c == 127) return fieldError(error, key, "control characters are not allowed");
  target = parsed;
  return true;
}
bool boolField(JsonObjectConst object, const char* key, bool& target,
               bool importing, std::string& error) {
  if (!object.containsKey(key)) return !importing || fieldError(error, key, "missing");
  if (!object[key].is<bool>()) return fieldError(error, key, "expected true or false");
  target = object[key].as<bool>();
  return true;
}
bool uintField(JsonObjectConst object, const char* key, uint32_t& target,
               bool importing, std::string& error) {
  if (!object.containsKey(key)) return !importing || fieldError(error, key, "missing");
  if (!object[key].is<uint32_t>()) return fieldError(error, key, "expected an integer from 0–4294967295");
  target = object[key].as<uint32_t>();
  return true;
}
bool secretValid(const std::string& secret) {
  if (secret.empty() || secret.size() > 64) return false;
  for (const unsigned char c : secret) if (c < 32 || c == 127) return false;
  return true;
}
}

bool mergeConfigJson(const Config& base, const std::string& json, Config& out,
                     std::string& error, bool importing) {
  error.clear();
  if (json.empty() || json.size() > kMaxJson) {
    error = "Configuration must contain 1–12288 bytes of JSON"; return false;
  }
  if (!oneDocument(json)) { error = "Expected one JSON object without extra data"; return false; }
  DynamicJsonDocument document(kDocumentCapacity);
  const auto parseError = deserializeJson(document, json);
  if (parseError || document.overflowed()) {
    error = std::string("JSON error: ") + (parseError ? parseError.c_str() : "out of memory"); return false;
  }
  if (!document.is<JsonObject>()) { error = "Expected a JSON object"; return false; }
  const JsonObjectConst object = document.as<JsonObjectConst>();
  uint32_t source_schema=base.schema;
  if(!uintField(object,"schema",source_schema,importing,error))return false;
  if(source_schema!=1&&source_schema!=2){error="Unsupported configuration version";return false;}
  Config candidate = importing&&source_schema==1 ? Config{} : base;
  candidate.schema=2;
#define UINT_FIELD(key) if (!uintField(object, #key, candidate.key, importing, error)) return false
#define STRING_FIELD(key) if (!stringField(object, #key, candidate.key, false, importing, error)) return false
#define SECRET_FIELD(key) if (!stringField(object, #key, candidate.key, true, importing, error)) return false
#define BOOL_FIELD(key) if (!boolField(object, #key, candidate.key, importing, error)) return false
  STRING_FIELD(hostname); STRING_FIELD(location); STRING_FIELD(contact); STRING_FIELD(driver);
  UINT_FIELD(baud); UINT_FIELD(poll_ms); UINT_FIELD(response_timeout_ms); UINT_FIELD(stale_ms);
  BOOL_FIELD(controls_enabled); BOOL_FIELD(power_controls_enabled); BOOL_FIELD(dhcp);
  STRING_FIELD(ip); STRING_FIELD(netmask); STRING_FIELD(gateway); STRING_FIELD(dns);
  BOOL_FIELD(snmp_enabled); SECRET_FIELD(snmp_community); STRING_FIELD(snmp_manager_ip);
  // Older persisted configurations have no write secret; missing means disabled.
  if (importing && !object.containsKey("snmp_write_community")) candidate.snmp_write_community.clear();
  else if (!stringField(object,"snmp_write_community",candidate.snmp_write_community,false,false,error)) return false;
  STRING_FIELD(web_user); SECRET_FIELD(web_password);
#undef UINT_FIELD
#undef STRING_FIELD
#undef SECRET_FIELD
#undef BOOL_FIELD
  const bool require_new=importing&&source_schema==2;
#define UINT_FIELD(key) if (!uintField(object,#key,candidate.key,require_new,error)) return false
#define STRING_FIELD(key) if (!stringField(object,#key,candidate.key,false,require_new,error)) return false
#define BOOL_FIELD(key) if (!boolField(object,#key,candidate.key,require_new,error)) return false
  BOOL_FIELD(operator_enabled);STRING_FIELD(operator_user);
  BOOL_FIELD(readonly_enabled);STRING_FIELD(readonly_user);
  // Disabled-account passwords are valid empty imports, while a blank edit
  // retains the stored secret. validateConfig enforces enabled-account strength.
  if(!stringField(object,"operator_password",candidate.operator_password,!importing,require_new,error)||
     !stringField(object,"readonly_password",candidate.readonly_password,!importing,require_new,error))return false;
  UINT_FIELD(session_timeout_min);BOOL_FIELD(ntp_enabled);STRING_FIELD(ntp_server);STRING_FIELD(timezone);
  STRING_FIELD(date_format);STRING_FIELD(temperature_unit);UINT_FIELD(history_interval_s);STRING_FIELD(default_page);
  UINT_FIELD(alarm_load_high);UINT_FIELD(alarm_temperature_high);UINT_FIELD(alarm_input_low);UINT_FIELD(alarm_input_high);UINT_FIELD(alarm_delay_s);
  BOOL_FIELD(syslog_enabled);STRING_FIELD(syslog_host);UINT_FIELD(syslog_port);UINT_FIELD(syslog_min_severity);
#undef UINT_FIELD
#undef STRING_FIELD
#undef BOOL_FIELD
  if(object.containsKey("quick_links")){
    if(!object["quick_links"].is<JsonArrayConst>()||object["quick_links"].size()>3)return fieldError(error,"quick_links","expected an array of up to three links");
    candidate.quick_links={}; size_t i=0;
    for(const auto item:object["quick_links"].as<JsonArrayConst>()){
      if(!item.is<JsonObjectConst>())return fieldError(error,"quick_links","expected objects");
      const auto link=item.as<JsonObjectConst>();auto& target=candidate.quick_links[i++];
      if(!stringField(link,"title",target.title,false,true,error)||!stringField(link,"url",target.url,false,true,error))return false;
    }
  }else if(require_new)return fieldError(error,"quick_links","missing");
  if(object.containsKey("schedules")){
    if(!object["schedules"].is<JsonArrayConst>()||object["schedules"].size()>8)return fieldError(error,"schedules","expected an array of up to eight schedules");
    candidate.schedules.clear();
    for(const auto item:object["schedules"].as<JsonArrayConst>()){
      if(!item.is<JsonObjectConst>())return fieldError(error,"schedules","expected objects");
      const auto rule=item.as<JsonObjectConst>();ScheduleRule r;
      if(!uintField(rule,"id",r.id,true,error)||!boolField(rule,"enabled",r.enabled,true,error)||
         !stringField(rule,"name",r.name,false,true,error)||!stringField(rule,"command",r.command,false,true,error)||
         !uintField(rule,"delay_minutes",r.delay_minutes,true,error)||!uintField(rule,"restore_minutes",r.restore_minutes,true,error)||
         !stringField(rule,"mode",r.mode,false,true,error)||!uintField(rule,"days_mask",r.days_mask,true,error)||
         !uintField(rule,"minute_of_day",r.minute_of_day,true,error))return false;
      if(!rule["at_utc"].is<uint64_t>())return fieldError(error,"at_utc","expected an integer UTC time");
      r.at_utc=rule["at_utc"].as<uint64_t>();candidate.schedules.push_back(std::move(r));
    }
  }else if(require_new)return fieldError(error,"schedules","missing");
  if (object.containsKey("battery_multiplier")) {
    const auto value = object["battery_multiplier"];
    if (!value.is<double>()) return fieldError(error, "battery_multiplier", "expected a number");
    const double multiplier = value.as<double>();
    if (!std::isfinite(multiplier) || multiplier < 0 || multiplier > 1000)
      return fieldError(error, "battery_multiplier", "allowed range is 0–1000");
    candidate.battery_multiplier = static_cast<float>(multiplier);
  } else if (importing) return fieldError(error, "battery_multiplier", "missing");
  if (object.containsKey("traps")) {
    if (!object["traps"].is<JsonArrayConst>()) return fieldError(error, "traps", "expected an array of two objects");
    const auto traps = object["traps"].as<JsonArrayConst>();
    if (traps.size() != candidate.traps.size()) return fieldError(error, "traps", "exactly two objects are required");
    for (size_t i=0; i<candidate.traps.size(); ++i) {
      if (!traps[i].is<JsonObjectConst>()) return fieldError(error, "traps", "each element must be an object");
      const auto trap = traps[i].as<JsonObjectConst>(); auto& target = candidate.traps[i];
      uint32_t port = target.port;
      if (!boolField(trap,"enabled",target.enabled,importing,error) ||
          !stringField(trap,"ip",target.ip,false,importing,error) ||
          !uintField(trap,"port",port,importing,error) ||
          !stringField(trap,"community",target.community,true,importing,error)) return false;
      if (port < 1 || port > 65535) return fieldError(error,"traps.port","allowed range is 1–65535");
      target.port = static_cast<uint16_t>(port);
    }
  } else if (importing) return fieldError(error,"traps","missing");
  // Disabled targets still need intact credentials for a complete persisted record.
  for (const auto& trap : candidate.traps) {
    if (!secretValid(trap.community)) return fieldError(error,"traps.community","requires 1–64 bytes without control characters");
    if (trap.ip.size()>15) return fieldError(error,"traps.ip","maximum 15 bytes");
  }
  for (const auto* address : {&candidate.ip, &candidate.netmask, &candidate.gateway, &candidate.dns, &candidate.snmp_manager_ip})
    if (address->size() > 15) return fieldError(error,"IPv4","maximum 15 bytes");
  if (!validateConfig(candidate,error)) return false;
  const auto* driver=ups::findDriver(candidate.driver);
  if (!driver) return fieldError(error,"driver","unknown UPS driver");
  for(const auto& rule:candidate.schedules){
    const auto* command=ups::findCommand(rule.command);
    if(!command)return fieldError(error,"schedules.command","unknown UPS command");
    ups::EncodedCommand encoded;
    if(!driver->encodeCommand(command->command,{static_cast<uint16_t>(rule.delay_minutes),static_cast<uint16_t>(rule.restore_minutes)},encoded,error))return false;
  }
  out = std::move(candidate); return true;
}

std::string configJson(const Config& config, bool secrets) {
  DynamicJsonDocument document(kDocumentCapacity);
  if (document.capacity() == 0) return {};
#define FIELD(key) document[#key] = config.key
  FIELD(schema); FIELD(hostname); FIELD(location); FIELD(contact); FIELD(driver);
  FIELD(baud); FIELD(poll_ms); FIELD(response_timeout_ms); FIELD(stale_ms); FIELD(battery_multiplier);
  FIELD(controls_enabled); FIELD(power_controls_enabled); FIELD(dhcp);
  FIELD(ip); FIELD(netmask); FIELD(gateway); FIELD(dns); FIELD(snmp_enabled); FIELD(snmp_manager_ip); FIELD(web_user);
  FIELD(operator_enabled);FIELD(operator_user);FIELD(readonly_enabled);FIELD(readonly_user);FIELD(session_timeout_min);
  FIELD(ntp_enabled);FIELD(ntp_server);FIELD(timezone);FIELD(date_format);FIELD(temperature_unit);FIELD(history_interval_s);FIELD(default_page);
  FIELD(alarm_load_high);FIELD(alarm_temperature_high);FIELD(alarm_input_low);FIELD(alarm_input_high);FIELD(alarm_delay_s);
  FIELD(syslog_enabled);FIELD(syslog_host);FIELD(syslog_port);FIELD(syslog_min_severity);
#undef FIELD
  document["web_password"] = secrets ? config.web_password : std::string();
  document["snmp_community"] = secrets ? config.snmp_community : std::string();
  document["snmp_write_community"] = secrets ? config.snmp_write_community : std::string();
  document["operator_password"] = secrets ? config.operator_password : std::string();
  document["readonly_password"] = secrets ? config.readonly_password : std::string();
  if (!secrets) {
    document["web_password_set"] = !config.web_password.empty();
    document["snmp_community_set"] = !config.snmp_community.empty();
    document["snmp_write_community_set"] = !config.snmp_write_community.empty();
    document["operator_password_set"] = !config.operator_password.empty();
    document["readonly_password_set"] = !config.readonly_password.empty();
  }
  auto traps = document.createNestedArray("traps");
  for (const auto& target : config.traps) {
    auto object = traps.createNestedObject(); object["enabled"] = target.enabled;
    object["ip"] = target.ip; object["port"] = target.port;
    object["community"] = secrets ? target.community : std::string();
    if (!secrets) object["community_set"] = !target.community.empty();
  }
  auto links=document.createNestedArray("quick_links");
  for(const auto& link:config.quick_links){auto o=links.createNestedObject();o["title"]=link.title;o["url"]=link.url;}
  auto schedules=document.createNestedArray("schedules");
  for(const auto& r:config.schedules){auto o=schedules.createNestedObject();
    o["id"]=r.id;o["enabled"]=r.enabled;o["name"]=r.name;o["command"]=r.command;o["delay_minutes"]=r.delay_minutes;o["restore_minutes"]=r.restore_minutes;
    o["mode"]=r.mode;o["at_utc"]=r.at_utc;o["days_mask"]=r.days_mask;o["minute_of_day"]=r.minute_of_day;
  }
  if (document.overflowed()) return {};
  std::string result; serializeJson(document,result); return result;
}
}
