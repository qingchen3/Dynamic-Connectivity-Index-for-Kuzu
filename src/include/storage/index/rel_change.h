#pragma once

#include "common/types/types.h"

namespace kuzu::storage {

enum class RelChangeType : unit8_t {
    INSERT,
    DELETE
};

struct RelChange {
    RelChangeType type;
    common::nodeID_t src;
    common::nodeID_t dst;
    common::internalID_t relID;
};

} // namespace kuzu::storage