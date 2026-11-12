#include "driver_registry.hpp"
#include "megatec_q1.hpp"

namespace ups {
const std::vector<const UpsDriver*>& drivers() {
  static const MegaTecQ1Driver megatec;
  static const std::vector<const UpsDriver*> registry{&megatec};
  return registry;
}
const UpsDriver* findDriver(const std::string& id) {
  for (const auto* driver : drivers()) if (id == driver->info().id) return driver;
  return nullptr;
}
}
