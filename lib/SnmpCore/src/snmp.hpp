#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace snmp {
using Oid = std::vector<uint32_t>;
enum class Type : uint8_t { Integer=0x02, OctetString=0x04, Null=0x05, ObjectId=0x06, Counter32=0x41, Gauge32=0x42, TimeTicks=0x43, NoSuchObject=0x80, NoSuchInstance=0x81, EndOfMibView=0x82 };
struct Value { Type type{Type::Null}; int32_t integer{0}; uint32_t unsigned_value{0}; std::string text; Oid oid; };
struct VarBind { Oid oid; Value value; };
class Provider {
 public:
  virtual ~Provider()=default;
  virtual bool get(const Oid&,Value&) const=0;
  virtual bool next(const Oid&,Oid&,Value&) const=0;
};
// Text OIDs may start with one dot; failures leave the OID empty.
bool parseOid(const std::string&,Oid&);
std::string oidString(const Oid&);
// SNMPv2c only. Malformed, unsupported or unauthenticated input returns false
// with empty output. Responses are at most 1400 bytes. Provider::next must
// return a lexicographically later OID; violations become endOfMibView.
bool processRequest(const uint8_t* data,size_t size,const std::string& community,const Provider&,std::vector<uint8_t>& output);
// Unacknowledged SNMPv2 trap: sysUpTime.0, snmpTrapOID.0, then extra bindings.
// Failure leaves output empty.
bool encodeTrap(const std::string& community,uint32_t uptime_ticks,const Oid& trap_oid,const std::vector<VarBind>& extra,std::vector<uint8_t>& output);
}  // namespace snmp
