#pragma once

#include <cstdint>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "common/dynamic_connectivity_index.h"

namespace kuzu {
namespace algo_extension {

using NeighborProvider = DynamicConnectivityIndex::NeighborProvider;

namespace dtree_internal {

struct DNode_CSR {
    explicit DNode_CSR(int key) : key{key} {}

    int key;
    int size = 1;
    DNode_CSR* parent = nullptr;
    std::set<DNode_CSR*> children;
};

void insert_edge(int u, int v, std::unordered_map<int, DNode_CSR*>& Dtree);
void delete_edge(
    int u, 
    int v, 
    std::unordered_map<int, DNode_CSR*>& Dtree,
    const NeighborProvider& getNeighbors);

std::pair<DNode_CSR*, DNode_CSR*> unlink(DNode_CSR* n_v);
std::pair<DNode_CSR*, int> find_root(DNode_CSR* node);
void insert_nte(DNode_CSR* r, DNode_CSR* n_u, int dist_u, DNode_CSR* n_v, int dist_v);
DNode_CSR* insert_te(DNode_CSR* n_u, DNode_CSR* n_v, DNode_CSR* r_u, DNode_CSR* r_v);

std::pair<DNode_CSR*, DNode_CSR*> delete_te(
    DNode_CSR* n_u, 
    DNode_CSR* n_v,
    const NeighborProvider& getNeighbors);

std::tuple<DNode_CSR*, DNode_CSR*, DNode_CSR*> BFS_select(
    DNode_CSR* r,
    std::unordered_map<int, DNode_CSR*>& Dtree,
    const NeighborProvider& getNeighbors);

void cal_size(std::unordered_map<int, DNode_CSR*>& Dtree);

int query(DNode_CSR* n_u, DNode_CSR* n_v);
int query_simple(DNode_CSR* n_u, DNode_CSR* n_v);

} // namespace dtree_internal

class DTree_CSR {
public:
    using node_key_t = int64_t;
    
    //using NeighborProvider = DynamicConnectivityIndex::NeighborProvider;

    DTree_CSR() = default;
    DTree_CSR(const DTree_CSR&) = delete;
    DTree_CSR& operator=(const DTree_CSR&) = delete;
    DTree_CSR(DTree_CSR&&) = delete;
    DTree_CSR& operator=(DTree_CSR&&) = delete;
    ~DTree_CSR();

    void insertEdge(node_key_t u, node_key_t v);
    void deleteEdge(
        node_key_t u,
        node_key_t v,
        const NeighborProvider& getNeighbors);
    bool connected(node_key_t u, node_key_t v) const;
    bool containsNode(node_key_t key) const;
    uint64_t getNumNodes() const;

private:
    static int toInternalKey(node_key_t key);

private:
    std::unordered_map<int, dtree_internal::DNode_CSR*> nodes;
};

} // namespace algo_extension
} // namespace kuzu