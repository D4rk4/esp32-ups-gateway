#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace snmp {
using Oid = std::vector<uint32_t>;
enum class Type : uint8_t { Integer=0x02, OctetString=0x04, Null=0x05, ObjectId=0x06, IpAddress=0x40, Counter32=0x41, Gauge32=0x42, TimeTicks=0x43, Opaque=0x44, Counter64=0x46, NoSuchObject=0x80, NoSuchInstance=0x81, EndOfMibView=0x82 };
struct Value { Type type{Type::Null}; int32_t integer{0}; uint32_t unsigned_value{0}; std::string text; Oid oid; };
struct VarBind { Oid oid; Value value; };
class Provider {
 public:
  virtual ~Provider()=default;
  virtual bool get(const Oid&,Value&) const=0;
  virtual bool next(const Oid&,Oid&,Value&) const=0;
};
class Writer {
 public:
  virtual ~Writer()=default;
  // Validate all bindings before acting. error_index is 1-based on error, 0 on success.
  virtual int32_t set(const std::vector<VarBind>&,size_t& error_index)=0;
};
// One recent writer-invoked request; hits do not extend the 10-second window.
class ReplayGuard {
 public:
  template<class Process>
  bool respond(uint32_t address,uint16_t port,const uint8_t* request,size_t size,uint64_t now_ms,Process process,std::vector<uint8_t>& response) {
    if(lookup(address,port,request,size,now_ms,response))return true;
    bool invoked=false;
    if(!process(response,invoked))return false;
    if(invoked)store(address,port,request,size,response,now_ms);
    return true;
  }
  bool lookup(uint32_t address,uint16_t port,const uint8_t* request,size_t size,uint64_t now_ms,std::vector<uint8_t>& response) const;
  bool store(uint32_t address,uint16_t port,const uint8_t* request,size_t size,const std::vector<uint8_t>& response,uint64_t now_ms);
 private:
  static constexpr size_t kCapacity=1400;
  std::array<uint8_t,kCapacity> request_{},response_{};
  size_t request_size_{0},response_size_{0};
  uint64_t stored_ms_{0};uint32_t address_{0};uint16_t port_{0};bool valid_{false};
};
// Text OIDs may start with one dot; failures leave the OID empty.
bool parseOid(const std::string&,Oid&);
std::string oidString(const Oid&);
// SNMPv2c only. Malformed, unsupported or unauthenticated input returns false
// with empty output. Responses are at most 1400 bytes. Provider::next must
// return a lexicographically later OID; violations become endOfMibView.
bool processRequest(const uint8_t* data,size_t size,const std::string& read_community,const std::string& write_community,const Provider&,Writer*,std::vector<uint8_t>& output);
inline bool processRequest(const uint8_t* data,size_t size,const std::string& community,const Provider& provider,std::vector<uint8_t>& output) {
  return processRequest(data,size,community,{},provider,nullptr,output);
}
// Unacknowledged SNMPv2 trap: sysUpTime.0, snmpTrapOID.0, then extra bindings.
// Failure leaves output empty.
bool encodeTrap(const std::string& community,uint32_t uptime_ticks,const Oid& trap_oid,const std::vector<VarBind>& extra,std::vector<uint8_t>& output);
}  // namespace snmp
