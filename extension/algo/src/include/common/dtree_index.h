#pragma once

#include "common/dtree.h"
#include "common/dynamic_connectivity_index.h"

namespace kuzu {
namespace algo_extension {

class DTreeIndex final : public DynamicConnectivityIndex {
public:
    DTreeIndex() = default;
    ~DTreeIndex() override = default;

    void insertEdge(node_key_t u, node_key_t v) override {
        dtree.insertEdge(u, v);
    }

    void deleteEdge(node_key_t u, node_key_t v) override {
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
        return "dtree";
    }

private:
    DTree dtree;
};

} // namespace algo_extension
} // namespace kuzu