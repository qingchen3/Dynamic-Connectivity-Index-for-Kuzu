#pragma once

#include <cstdint>

namespace kuzu {
namespace algo_extension {

struct DeleteDiagnostics {
    enum class EdgeKind { NONE, TREE, NON_TREE };

    EdgeKind edgeKind = EdgeKind::NONE;
    bool replacementSearchTriggered = false;
    bool replacementFound = false;
    uint64_t replacementCandidatesScanned = 0;

    uint64_t replacementSearchElapsedNs = 0;   // Whole replacement search
    uint64_t getNeighborsElapsedNs = 0;         // Time inside getNeighbors calls
    uint64_t getNeighborsCallCount = 0;         // Number of callback calls
    uint64_t getNeighborsReturnedIdCount = 0;   // Sum of returned vector sizes
};

} // namespace algo_extension
} // namespace kuzu
