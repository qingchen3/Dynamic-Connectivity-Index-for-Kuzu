#pragma once

#include "common/dtree_csr.h"
#include "common/dynamic_connectivity_index.h"

namespace kuzu {
namespace algo_extension {

class DTreeCSRIndex final : public DynamicConnectivityIndex {
public:
    DTreeCSRIndex() = default;
    ~DTreeCSRIndex() override = default;

    void insertEdge(node_key_t u, node_key_t v) override {
        dtree_csr.insertEdge(u, v);
    }

    void deleteEdge(
        node_key_t u,
        node_key_t v,
        const NeighborProvider& getNeighbors) override {
        dtree_csr.deleteEdge(u, v, getNeighbors);
    }

    bool connected(node_key_t u, node_key_t v) const override {
        return dtree_csr.connected(u, v);
    }

    bool containsNode(node_key_t key) const override {
        return dtree_csr.containsNode(key);
    }

    uint64_t getNumNodes() const override {
        return dtree_csr.getNumNodes();
    }

    std::string getName() const override {
        return "dtree_csr";
    }

private:
    DTree_CSR dtree_csr;
};

} // namespace algo_extension
} // namespace kuzu