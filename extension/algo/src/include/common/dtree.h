#pragma once

#include <cstdint>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace kuzu {
namespace algo_extension {

namespace dtree_internal {

struct DNode {
    explicit DNode(int key) : key{key} {}

    int key;
    int size = 1;
    DNode* left = nullptr;
    DNode* right = nullptr;
    DNode* parent = nullptr;
    std::set<DNode*> children;
    std::set<DNode*> nte;
};

void insert_edge(int u, int v, std::unordered_map<int, DNode*>& Dtree);
void delete_edge(int u, int v, std::unordered_map<int, DNode*>& Dtree);

std::pair<DNode*, DNode*> unlink(DNode* n_v);
std::pair<DNode*, int> find_root(DNode* node);
void insert_nte(DNode* r, DNode* n_u, int dist_u, DNode* n_v, int dist_v);
DNode* insert_te(DNode* n_u, DNode* n_v, DNode* r_u, DNode* r_v);
void delete_nte(DNode* n_u, DNode* n_v);
std::pair<DNode*, DNode*> delete_te(DNode* n_u, DNode* n_v);
std::tuple<DNode*, DNode*, DNode*> BFS_select(DNode* r);
void cal_size(std::unordered_map<int, DNode*>& Dtree);

int query(DNode* n_u, DNode* n_v);
int query_simple(DNode* n_u, DNode* n_v);

} // namespace dtree_internal

class DTree {
public:
    using node_key_t = int64_t;

    DTree() = default;
    DTree(const DTree&) = delete;
    DTree& operator=(const DTree&) = delete;
    DTree(DTree&&) = delete;
    DTree& operator=(DTree&&) = delete;
    ~DTree();

    void insertEdge(node_key_t u, node_key_t v);
    void deleteEdge(node_key_t u, node_key_t v);
    bool connected(node_key_t u, node_key_t v) const;
    bool containsNode(node_key_t key) const;
    uint64_t getNumNodes() const;

private:
    static int toInternalKey(node_key_t key);

private:
    std::unordered_map<int, dtree_internal::DNode*> nodes;
};

} // namespace algo_extension
} // namespace kuzu