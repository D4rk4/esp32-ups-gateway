#pragma once
#include "ups_driver.hpp"
#include <vector>

namespace ups {
const UpsDriver* findDriver(const std::string& id);
const std::vector<const UpsDriver*>& drivers();
}
