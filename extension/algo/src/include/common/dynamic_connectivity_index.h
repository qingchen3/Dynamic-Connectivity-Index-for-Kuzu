#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <vector>

#include "common/delete_diagnostics.h"
#include "common/dc_index_memory_footprint.h"

namespace kuzu {
namespace algo_extension {

class DynamicConnectivityIndex {
public:
    using node_key_t = int64_t;
    using NeighborProvider =
        std::function<std::vector<node_key_t>(node_key_t)>;

    virtual ~DynamicConnectivityIndex() = default;

    virtual void insertEdge(node_key_t u, node_key_t v) = 0;
    
    virtual void deleteEdge(
        node_key_t u,
        node_key_t v,
        const NeighborProvider& getNeighbors) = 0;
        
    virtual bool connected(node_key_t u, node_key_t v) const = 0;

    // Whether connected() changes the forest. The D-tree query reroots a tree
    // toward its centroid, so it needs the forest to itself; the S-tree query
    // only reads, so queries can share it.
    virtual bool queryRestructures() const { return false; }

    // Appends key, parent(key), ..., root(key) to path, following tree edges
    // only. Never changes the index. A key the index has not seen is its own
    // root.
    virtual void rootPath(node_key_t key, std::vector<node_key_t>& path) const = 0;

    // Same answer as connected(), but never restructures the forest, so
    // concurrent readers can share the index.
    virtual bool connectedReadOnly(node_key_t u, node_key_t v) const {
        if (!containsNode(u) || !containsNode(v)) {
            return false;
        }
        std::vector<node_key_t> pathU;
        std::vector<node_key_t> pathV;
        rootPath(u, pathU);
        rootPath(v, pathV);
        return pathU.back() == pathV.back();
    }

    virtual bool containsNode(node_key_t key) const = 0;

    virtual uint64_t getNumNodes() const = 0;

    virtual std::string getName() const = 0;

    virtual IndexMemoryFootprint memoryFootprint() const = 0;

    // Whether this index instruments deletions and exposes per-deletion
    // diagnostics via lastDeleteDiagnostics().
    virtual bool supportsDeleteDiagnostics() const { return false; }

    // Diagnostics describing the most recent deleteEdge() call. Only meaningful
    // when supportsDeleteDiagnostics() returns true.
    virtual DeleteDiagnostics lastDeleteDiagnostics() const { return {}; }
};

} // namespace algo_extension
} // namespace kuzu
