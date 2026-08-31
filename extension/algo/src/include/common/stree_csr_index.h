#pragma once

#include "common/dynamic_connectivity_index.h"
#include "common/stree_csr.h"
#include "stree_csr.h"

namespace kuzu {
namespace algo_extension {

class STreeCSRIndex final : public DynamicConnectivityIndex {
public:
    STreeCSRIndex() = default;
    ~STreeCSRIndex() override = default;

    void insertEdge(node_key_t u, node_key_t v) override {
        stree_csr.insertEdge(u, v);
    }

    void deleteEdge(
        node_key_t u,
        node_key_t v,
        const NeighborProvider& getNeighbors) override {

        stree_csr.deleteEdge(u, v, getNeighbors);
    }

    bool connected(node_key_t u, node_key_t v) const override {
        return stree_csr.connected(u, v);
    }

    bool containsNode(node_key_t key) const override {
        return stree_csr.containsNode(key);
    }

    uint64_t getNumNodes() const override {
        return stree_csr.getNumNodes();
    }

    std::string getName() const override {
        return "stree_csr";
    }

private:
    STree_CSR stree_csr;
};

} // namespace algo_extension
} // namespace kuzu