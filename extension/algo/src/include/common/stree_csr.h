#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

#include "common/dynamic_connectivity_index.h"

namespace kuzu {
namespace algo_extension {

class STree_CSR {
public:
    using node_key_t = int64_t;
    
    using NeighborProvider = DynamicConnectivityIndex::NeighborProvider;

    STree_CSR() = default;
    STree_CSR(const STree_CSR&) = delete;
    STree_CSR& operator=(const STree_CSR&) = delete;
    STree_CSR(STree_CSR&&) = delete;
    STree_CSR& operator=(STree_CSR&&) = delete;
    ~STree_CSR() = default;

    void insertEdge(node_key_t u, node_key_t v);
    void deleteEdge(
        node_key_t u,
        node_key_t v,
        const NeighborProvider& getNeighbors);
    bool connected(node_key_t u, node_key_t v) const;
    bool containsNode(node_key_t key) const;
    uint64_t getNumNodes() const;

private:
    struct SNode_CSR {
        explicit SNode_CSR(node_key_t key) : key{key} {}

        node_key_t key;
        SNode_CSR* parent = nullptr;
        SNode_CSR* skip = nullptr;
        std::set<SNode_CSR*> children;
    };

    SNode_CSR* getOrCreateNode(node_key_t key);
    SNode_CSR* getNode(node_key_t key) const;
    static SNode_CSR* findRoot(SNode_CSR* node);
    static SNode_CSR* reroot(SNode_CSR* node);
    std::pair<SNode_CSR*, SNode_CSR*> searchReplacement(
        SNode_CSR* startNode,
        const NeighborProvider& getNeighbors);

    void insertTreeEdge(node_key_t u, node_key_t v);
    void deleteTreeEdge(
        node_key_t parent, 
        node_key_t child,
        const NeighborProvider& getNeighbors);

private:
    std::unordered_map<node_key_t, std::unique_ptr<SNode_CSR>> nodes;
};

} // namespace algo_extension
} // namespace kuzu
