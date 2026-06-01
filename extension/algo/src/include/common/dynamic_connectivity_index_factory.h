#pragma once

#include "common/dynamic_connectivity_index.h"

#include <memory>
#include <string>

namespace kuzu {
namespace algo_extension {

std::unique_ptr<DynamicConnectivityIndex> createDynamicConnectivityIndex(const std::string& method);

} // namespace algo_extension
} // namespace kuzu