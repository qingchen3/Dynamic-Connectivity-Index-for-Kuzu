#pragma once

#include "common/dynamic_connectivity_index.h"
#include "common/stree.h"

namespace kuzu {
namespace algo_extension {

class STreeIndex final : public DynamicConnectivityIndex {
public:
    STreeIndex() = default;
    ~STreeIndex() override = default;

    void insertEdge(node_key_t u, node_key_t v) override {
        stree.insertEdge(u, v);
    }

    void deleteEdge(node_key_t u, node_key_t v) override {
        stree.deleteEdge(u, v);
    }

    bool connected(node_key_t u, node_key_t v) const override {
        return stree.connected(u, v);
    }

    bool containsNode(node_key_t key) const override {
        return stree.containsNode(key);
    }

    uint64_t getNumNodes() const override {
        return stree.getNumNodes();
    }

    std::string getName() const override {
        return "stree";
    }

private:
    STree stree;
};

} // namespace algo_extension
} // namespace kuzu