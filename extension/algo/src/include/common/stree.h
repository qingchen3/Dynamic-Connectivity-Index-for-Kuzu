#pragma once

#include "common/delete_diagnostics.h"

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>

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

    // Diagnostics describing the most recent deleteEdge() call.
    const DeleteDiagnostics& lastDeleteDiagnostics() const { return lastDeleteDiagnostics_; }

private:
    struct SNode {
        explicit SNode(node_key_t key) : key{key} {}

        node_key_t key;
        SNode* left = nullptr;
        SNode* right = nullptr;
        SNode* parent = nullptr;
        SNode* skip = nullptr;
        std::set<SNode*> children;
        std::set<SNode*> nte;
    };

    SNode* getOrCreateNode(node_key_t key);
    SNode* getNode(node_key_t key) const;
    static SNode* findRoot(SNode* node);
    static SNode* reroot(SNode* node);
    static std::pair<SNode*, SNode*> searchReplacement(SNode* startNode, DeleteDiagnostics& diag);

    void insertTreeEdge(node_key_t u, node_key_t v);
    void insertNonTreeEdge(node_key_t u, node_key_t v);
    void deleteTreeEdge(node_key_t parent, node_key_t child);
    void deleteNonTreeEdge(node_key_t u, node_key_t v);

private:
    std::unordered_map<node_key_t, std::unique_ptr<SNode>> nodes;
    DeleteDiagnostics lastDeleteDiagnostics_;
};

} // namespace algo_extension
} // namespace kuzu
