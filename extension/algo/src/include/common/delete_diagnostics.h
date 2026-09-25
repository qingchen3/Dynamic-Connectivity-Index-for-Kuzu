#pragma once

#include <cstdint>

namespace kuzu {
namespace algo_extension {

// Per-deletion diagnostic information collected by a dynamic connectivity index.
//
// A fresh instance describes the most recent deleteEdge() call. Implementations
// that do not instrument deletions can leave the defaults, which describe a
// no-op deletion.

struct DeleteDiagnostics {
    enum class EdgeKind { NONE, TREE, NON_TREE };

    EdgeKind edgeKind = EdgeKind::NONE;
    bool replacementSearchTriggered = false;
    bool replacementFound = false;
    uint64_t replacementCandidatesScanned = 0;
};

} // namespace algo_extension
} // namespace kuzu
