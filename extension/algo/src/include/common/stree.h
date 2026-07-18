#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

namespace kuzu {
namespace algo_extension {

class STree {
public:
    using node_key_t = int64_t;

    STree() = default;
    STree(const STree&) = delete;
    STree& operator=(const STree&) = delete;
    STree(STree&&) = delete;
    STree& operator=(STree&&) = delete;
    ~STree() = default;

    void insertEdge(node_key_t u, node_key_t v);
    void deleteEdge(node_key_t u, node_key_t v);
    bool connected(node_key_t u, node_key_t v) const;
    bool containsNode(node_key_t key) const;
    uint64_t getNumNodes() const;

private:
    struct SNode {
        explicit SNode(node_key_t key) : key{key} {}

        node_key_t key;
        SNode* parent = nullptr;
        SNode* skip = nullptr;
        std::set<SNode*> children;
        std::set<SNode*> nte;
    };

    SNode* getOrCreateNode(node_key_t key);
    SNode* getNode(node_key_t key) const;
    static SNode* findRoot(SNode* node);
    static SNode* reroot(SNode* node);
    static std::pair<SNode*, SNode*> searchReplacement(SNode* startNode);

    void insertTreeEdge(node_key_t u, node_key_t v);
    void insertNonTreeEdge(node_key_t u, node_key_t v);
    void deleteTreeEdge(node_key_t parent, node_key_t child);
    void deleteNonTreeEdge(node_key_t u, node_key_t v);

private:
    std::unordered_map<node_key_t, std::unique_ptr<SNode>> nodes;
};

} // namespace algo_extension
} // namespace kuzu
