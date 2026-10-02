#pragma once

#include "common/dtree_lazy_nte.h"
#include "common/dynamic_connectivity_index.h"

namespace kuzu::algo_extension {

class DTreeLazyNTEIndex final : public DynamicConnectivityIndex {
public:
    void insertEdge(node_key_t u, node_key_t v) override {
        dtree.insertEdge(u, v);
    }

    void deleteEdge(node_key_t u, node_key_t v) {
        dtree.deleteEdge(u, v);
    }

    void deleteEdge(node_key_t u, node_key_t v, const NeighborProvider& getNeighbors) override {
        (void)getNeighbors;
        dtree.deleteEdge(u, v);
    }

    bool connected(node_key_t u, node_key_t v) const override {
        return dtree.connected(u, v);
    }

    bool containsNode(node_key_t key) const override {
        return dtree.containsNode(key);
    }

    uint64_t getNumNodes() const override {
        return dtree.getNumNodes();
    }

    std::string getName() const override {
        return "dtree_lazy_nte";
    }

    IndexMemoryFootprint memoryFootprint() const override {
        auto result = dtree.memoryFootprint();
        result.bytesNodes += sizeof(*this) - sizeof(dtree);
        return result;
    }

    bool supportsDeleteDiagnostics() const override {
        return true;
    }

    DeleteDiagnostics lastDeleteDiagnostics() const override {
        return dtree.lastDeleteDiagnostics();
    }

private:
    DTreeLazyNTE dtree;
};

} // namespace kuzu::algo_extension
