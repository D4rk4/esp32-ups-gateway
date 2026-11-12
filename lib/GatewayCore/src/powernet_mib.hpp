#pragma once
#include "snmp.hpp"
#include "ups_model.hpp"
#include "config_model.hpp"
namespace gateway {
class PowerNetMib final : public snmp::Provider {
 public:
  PowerNetMib(const Config&, const ups::Snapshot&, uint32_t uptime_ticks,const std::string& firmware);
  bool get(const snmp::Oid&,snmp::Value&) const override;
  bool next(const snmp::Oid&,snmp::Oid&,snmp::Value&) const override;
 private:
  std::vector<snmp::VarBind> entries_;
};
}
