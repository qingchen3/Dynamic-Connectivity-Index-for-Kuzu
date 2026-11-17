#pragma once

#include "common/delete_diagnostics.h"
#if __has_include("common/dc_index_memory_footprint.h")
#include "common/dc_index_memory_footprint.h"
#else
#include "common/index_memory_footprint.h"
#endif

#include <cstdint>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace kuzu {
namespace algo_extension {

namespace dtree_lazy_nte_internal {

struct DNodeLazyNTE {
    explicit DNodeLazyNTE(int key) : key{key} {}

    int key;
    int size = 1;
    DNodeLazyNTE* parent = nullptr;
    std::set<DNodeLazyNTE*> children;
    std::unique_ptr<std::set<DNodeLazyNTE*>> nte;

    bool hasNonTreeNeighbor(DNodeLazyNTE* neighbor) const {
        return nte && nte->contains(neighbor);
    }

    void addNonTreeNeighbor(DNodeLazyNTE* neighbor) {
        if (!nte) {
            nte = std::make_unique<std::set<DNodeLazyNTE*>>();
        }
        nte->insert(neighbor);
    }

    void removeNonTreeNeighbor(DNodeLazyNTE* neighbor) {
        if (!nte) {
            return;
        }
        nte->erase(neighbor);
        if (nte->empty()) {
            nte.reset();
        }
    }
};

void insert_edge(int u, int v, std::unordered_map<int, DNodeLazyNTE*>& Dtree);
void delete_edge(int u, int v, std::unordered_map<int, DNodeLazyNTE*>& Dtree, DeleteDiagnostics& diag);

std::pair<DNodeLazyNTE*, DNodeLazyNTE*> unlink(DNodeLazyNTE* n_v);
std::pair<DNodeLazyNTE*, int> find_root(DNodeLazyNTE* node);
void insert_nte(DNodeLazyNTE* r, DNodeLazyNTE* n_u, int dist_u, DNodeLazyNTE* n_v, int dist_v);
DNodeLazyNTE* insert_te(DNodeLazyNTE* n_u, DNodeLazyNTE* n_v, DNodeLazyNTE* r_u, DNodeLazyNTE* r_v);
void delete_nte(DNodeLazyNTE* n_u, DNodeLazyNTE* n_v);
std::pair<DNodeLazyNTE*, DNodeLazyNTE*> delete_te(DNodeLazyNTE* n_u, DNodeLazyNTE* n_v, DeleteDiagnostics& diag);
std::tuple<DNodeLazyNTE*, DNodeLazyNTE*, DNodeLazyNTE*> BFS_select(DNodeLazyNTE* r, DeleteDiagnostics& diag);
void cal_size(std::unordered_map<int, DNodeLazyNTE*>& Dtree);

int query(DNodeLazyNTE* n_u, DNodeLazyNTE* n_v);
int query_simple(DNodeLazyNTE* n_u, DNodeLazyNTE* n_v);

} // namespace dtree_lazy_nte_internal

class DTreeLazyNTE {
public:
    using node_key_t = int64_t;

    DTreeLazyNTE() = default;
    DTreeLazyNTE(const DTreeLazyNTE&) = delete;
    DTreeLazyNTE& operator=(const DTreeLazyNTE&) = delete;
    DTreeLazyNTE(DTreeLazyNTE&&) = delete;
    DTreeLazyNTE& operator=(DTreeLazyNTE&&) = delete;
    ~DTreeLazyNTE();

    void insertEdge(node_key_t u, node_key_t v);
    void deleteEdge(node_key_t u, node_key_t v);
    bool connected(node_key_t u, node_key_t v) const;
    bool containsNode(node_key_t key) const;
    uint64_t getNumNodes() const;
    IndexMemoryFootprint memoryFootprint() const;

    // Diagnostics describing the most recent deleteEdge() call.
    const DeleteDiagnostics& lastDeleteDiagnostics() const { return lastDeleteDiagnostics_; }

private:
    static int toInternalKey(node_key_t key);

private:
    std::unordered_map<int, dtree_lazy_nte_internal::DNodeLazyNTE*> nodes;
    DeleteDiagnostics lastDeleteDiagnostics_;
};

} // namespace algo_extension
} // namespace kuzu
