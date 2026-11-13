#pragma once
#include "snmp.hpp"
#include "ups_model.hpp"
#include "config_model.hpp"
namespace gateway {
struct PowerNetControlAction { bool pending{false}; ups::Command command{}; ups::CommandArgs args{}; size_t index{0}; };
// Returns an SNMP error status; validates the complete request before exposing an action.
int32_t mapPowerNetSet(const std::vector<snmp::VarBind>&,const ups::Snapshot&,bool fresh,PowerNetControlAction&,size_t& error_index);
class PowerNetMib final : public snmp::Provider {
 public:
  PowerNetMib(const Config&, const ups::Snapshot&, uint32_t uptime_ticks,const std::string& firmware);
  PowerNetMib(const Config&, const ups::Snapshot&, uint32_t uptime_ticks,const std::string& firmware,uint32_t driver_capabilities);
  bool get(const snmp::Oid&,snmp::Value&) const override;
  bool next(const snmp::Oid&,snmp::Oid&,snmp::Value&) const override;
 private:
  std::vector<snmp::VarBind> entries_;
};
}
