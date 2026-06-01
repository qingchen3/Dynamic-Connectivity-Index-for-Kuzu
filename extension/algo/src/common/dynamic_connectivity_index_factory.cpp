#include "common/dynamic_connectivity_index_factory.h"

#include "common/dtree_index.h"
#include "common/stree_index.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace kuzu {
namespace algo_extension {

std::unique_ptr<DynamicConnectivityIndex> createDynamicConnectivityIndex(const std::string& method) {
    std::string normalizedMethod = method;
    std::transform(normalizedMethod.begin(), normalizedMethod.end(), normalizedMethod.begin(),
        [](unsigned char c) { return std::tolower(c); });

    if (normalizedMethod == "stree") {
        return std::make_unique<STreeIndex>();
    }

    if (normalizedMethod == "dtree") {
        return std::make_unique<DTreeIndex>();
    }

    throw std::runtime_error("Unknown dynamic connectivity index method: " + method);
}

} // namespace algo_extension
} // namespace kuzu