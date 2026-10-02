#include <set>
#include <cstdlib>
#include <climits>
#include <unordered_map>
#include "common/dtree_lazy_nte.h"
#include <cstddef>
#include <utility>
#include <vector>
#include <queue>
#include <iostream>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace kuzu {
namespace algo_extension {

namespace dtree_lazy_nte_internal {

    DNodeLazyNTE* reroot(DNodeLazyNTE * n_w) {
        if (n_w->parent != nullptr) {
            DNodeLazyNTE* ch = n_w;
            DNodeLazyNTE* cur = ch->parent;
            n_w->parent = nullptr;
            while (cur != nullptr) {
                DNodeLazyNTE* g = cur->parent;
                cur->parent = ch;
                cur->children.erase(ch);
                ch->children.insert(cur);
                ch = cur;
                cur = g;
            }
            while (ch->parent != nullptr) {
                ch->size -= ch->parent->size;
                ch->parent->size += ch->size;
                ch = ch->parent;
            }
        }
        return n_w;
    }

    DNodeLazyNTE* link(DNodeLazyNTE* n_u, DNodeLazyNTE* r_u, DNodeLazyNTE* n_v) {
        n_v->parent = n_u;
        n_u->children.insert(n_v);

        DNodeLazyNTE* c = n_u;
        DNodeLazyNTE* new_root = nullptr;
        while (c != nullptr) {
            c->size += n_v->size;
            if(c->size > (r_u->size + n_v->size) / 2 && new_root == nullptr && c->parent != nullptr) {
                new_root = c;
            }
            c = c->parent;
        }

        if (new_root != nullptr) r_u = reroot(new_root);

        return r_u;
    }

    std::pair<DNodeLazyNTE*, DNodeLazyNTE*> unlink(DNodeLazyNTE* n_v) {
        if (n_v->parent == nullptr) {
            return std::make_pair(n_v, n_v);
        }
        DNodeLazyNTE* c = n_v;
        while (c->parent != nullptr) {
            c = c->parent;
            c->size -= n_v->size;
        }
        n_v->parent->children.erase(n_v);
        n_v->parent = nullptr;
        return std::make_pair(n_v, c);
    }

    std::pair<DNodeLazyNTE*, int> find_root(DNodeLazyNTE* node) {
        int dist = 0;
        while(node->parent != nullptr) {
            node = node->parent;
            dist++;
        }
        return std::make_pair(node, dist);
    }

    void insert_edge(int u, int v, std::unordered_map<int, DNodeLazyNTE*> &Dtree) {

        if (Dtree.find(u) == Dtree.end()) {
            Dtree[u] = new DNodeLazyNTE(u);
        }

        if (Dtree.find(v) == Dtree.end()) {
            Dtree[v] = new DNodeLazyNTE(v);
        }

        std::pair<DNodeLazyNTE*, int> res_u = find_root(Dtree[u]);
        DNodeLazyNTE* r_u = res_u.first;
        int dist_u = res_u.second;
        std::pair<DNodeLazyNTE*, int> res_v = find_root(Dtree[v]);
        DNodeLazyNTE* r_v = res_v.first;
        int dist_v = res_v.second;

        if(r_u->key != r_v->key) {
            insert_te(Dtree[u], Dtree[v], r_u, r_v);
        } else {
            if (Dtree[u]->parent != Dtree[v] && Dtree[v]->parent != Dtree[u])
                insert_nte(r_u, Dtree[u], dist_u, Dtree[v], dist_v);
        }
        //cal_size(Dtree);
    }

    void insert_nte(DNodeLazyNTE* r, DNodeLazyNTE* n_u, int dist_u, DNodeLazyNTE* n_v, int dist_v) {
        if (n_u->hasNonTreeNeighbor(n_v) && n_v->hasNonTreeNeighbor(n_u)) return;

        if (abs(dist_u - dist_v) < 2) {
            n_u->addNonTreeNeighbor(n_v);
            n_v->addNonTreeNeighbor(n_u);
            return;
        } else {
            DNodeLazyNTE* shallow = nullptr;
            DNodeLazyNTE* deep = nullptr;
            if(dist_u < dist_v) {
                deep = n_v;
                shallow = n_u;
            } else {
                deep = n_u;
                shallow = n_v;
            }
            int delta = abs(dist_u - dist_v) - 2;
            DNodeLazyNTE* c = deep;
            for(int i = 0; i < delta; i++) c = c->parent;
            c->parent->addNonTreeNeighbor(c);
            c->addNonTreeNeighbor(c->parent);

            unlink(c);
            link(shallow, r, reroot(deep));
            return;
        }
    }

    DNodeLazyNTE* insert_te(DNodeLazyNTE* n_u, DNodeLazyNTE* n_v, DNodeLazyNTE* r_u, DNodeLazyNTE* r_v) {
        if(r_v->size < r_u->size) {
            return link(n_u, r_u, reroot(n_v));
        } else {
            return link(n_v, r_v, reroot(n_u));
        }
    }

    void delete_nte(DNodeLazyNTE* n_u, DNodeLazyNTE* n_v) {
        n_u->removeNonTreeNeighbor(n_v);
        n_v->removeNonTreeNeighbor(n_u);
    }

    std::pair<DNodeLazyNTE*, DNodeLazyNTE*> delete_te(DNodeLazyNTE* n_u, DNodeLazyNTE* n_v, DeleteDiagnostics& diag) {
        // determine parent and child
        DNodeLazyNTE* ch = nullptr;
        if(n_u->parent == n_v) {
            ch = n_u;
        } else if(n_v->parent == n_u) {
            ch = n_v;
        } else {
            // edge does not exist as a tree edge
            return std::make_pair(n_u, n_v);
        }

        diag.edgeKind = DeleteDiagnostics::EdgeKind::TREE;
        diag.replacementSearchTriggered = true;

        DNodeLazyNTE* root = nullptr;
        std::pair<DNodeLazyNTE*, DNodeLazyNTE*>res = unlink(ch);
        ch = res.first;
        root = res.second;

        DNodeLazyNTE* r_s = nullptr;
        DNodeLazyNTE* r_l = nullptr;

        if(ch->size < root->size) {
            r_s = ch;
            r_l = root;
        } else {
            r_s = root;
            r_l = ch;
        }

        std::tuple<DNodeLazyNTE*, DNodeLazyNTE*, DNodeLazyNTE*> res_bfs_sel = BFS_select(r_s, diag);
        DNodeLazyNTE* n_rs = std::get<0>(res_bfs_sel);
        DNodeLazyNTE* n_rl = std::get<1>(res_bfs_sel);
        DNodeLazyNTE* new_r = std::get<2>(res_bfs_sel);

        if(n_rs == nullptr && n_rl == nullptr) {
            if(new_r != nullptr) r_s = reroot(new_r);
            return std::make_pair(r_s, r_l);
        } else {
            diag.replacementFound = true;
            n_rs->removeNonTreeNeighbor(n_rl);
            n_rl->removeNonTreeNeighbor(n_rs);
            return std::make_pair(insert_te(n_rs, n_rl, r_s, r_l), nullptr);
        }
    }

    std::tuple<DNodeLazyNTE*, DNodeLazyNTE*, DNodeLazyNTE*> BFS_select(DNodeLazyNTE* r, DeleteDiagnostics& diag) {
        std::queue<DNodeLazyNTE*> q;
        q.push(r);

        DNodeLazyNTE* new_r = nullptr;
        int S = r->size;
        int min_dist = INT_MAX;

        DNodeLazyNTE* n_rs = nullptr;
        DNodeLazyNTE* n_rl = nullptr;

        while(!q.empty()) {
            std::queue<DNodeLazyNTE*> new_q;
            while(!q.empty()) {
                DNodeLazyNTE *node;
                node = q.front();
                q.pop();
                if (node->size> S / 2 && node->size < S && new_r == nullptr) new_r = node;

                if (node->nte) {
                    for (auto it : *node->nte) {
                        diag.replacementCandidatesScanned++;
                        std::pair <DNodeLazyNTE*, int> res_find_root = find_root(it);
                        DNodeLazyNTE* rt = res_find_root.first;
                        int dist = res_find_root.second;
                        if(rt->key == r->key) continue;

                        if(dist < min_dist) {
                            min_dist = dist;
                            n_rl = it;
                            n_rs = node;
                        }
                    }
                }
                for (auto it : node->children) new_q.push(it);
            }
            q = new_q;
        }
        return std::make_tuple(n_rs, n_rl, new_r);
    }

    void delete_edge(int u, int v, std::unordered_map<int, DNodeLazyNTE*> &Dtree, DeleteDiagnostics& diag) {
        if(Dtree.find(u) == Dtree.end() || Dtree.find(v) == Dtree.end()) {
            return;
        }
        if (Dtree[v]->hasNonTreeNeighbor(Dtree[u]) ||
            Dtree[u]->hasNonTreeNeighbor(Dtree[v])) {
            diag.edgeKind = DeleteDiagnostics::EdgeKind::NON_TREE;
            delete_nte(Dtree[u], Dtree[v]);
        } else {
            delete_te(Dtree[u], Dtree[v], diag);
        }
    }

    int query(DNodeLazyNTE* n_u, DNodeLazyNTE* n_v) {
        DNodeLazyNTE* d_u = nullptr;

        while (n_u->parent != nullptr) {
            d_u = n_u;
            n_u = n_u->parent;
        }
        if (d_u != nullptr && d_u->size > n_u->size / 2) n_u = reroot(d_u);

        DNodeLazyNTE* d_v = nullptr;
        while (n_v->parent != nullptr) {
            d_v = n_v;
            n_v = n_v->parent;
        }
        if (d_v != nullptr && d_v->size > n_v->size / 2) n_v = reroot(d_v);

        return find_root(n_u).first->key == find_root(n_v).first->key;
    }

    void cal_size(std::unordered_map<int, DNodeLazyNTE*> &Dtree) {
        int total_size = 0;
        for(auto it = Dtree.begin(); it != Dtree.end(); it++) {
            size_t key_mem = sizeof(it->first);
            size_t ptr_mem = sizeof(it->second);

            total_size += key_mem + ptr_mem;
        }
        //std::cout << total_size << std::endl;
    }

} // end of namespace dtree_lazy_nte_internal

DTreeLazyNTE::~DTreeLazyNTE() {
    for (auto& entry : nodes) {
        delete entry.second;
    }
}

int DTreeLazyNTE::toInternalKey(node_key_t key) {
    if (key < std::numeric_limits<int>::min() || key > std::numeric_limits<int>::max()) {
        throw std::runtime_error("DTreeLazyNTE currently only supports node IDs within int range.");
    }
    return static_cast<int>(key);
}

void DTreeLazyNTE::insertEdge(node_key_t u, node_key_t v) {
    dtree_lazy_nte_internal::insert_edge(toInternalKey(u), toInternalKey(v), nodes);
}

void DTreeLazyNTE::deleteEdge(node_key_t u, node_key_t v) {
    lastDeleteDiagnostics_ = DeleteDiagnostics{};
    dtree_lazy_nte_internal::delete_edge(toInternalKey(u), toInternalKey(v), nodes, lastDeleteDiagnostics_);
}

bool DTreeLazyNTE::connected(node_key_t u, node_key_t v) const {
    auto uIt = nodes.find(toInternalKey(u));
    auto vIt = nodes.find(toInternalKey(v));
    if (uIt == nodes.end() || vIt == nodes.end()) {
        return false;
    }
    return dtree_lazy_nte_internal::query(uIt->second, vIt->second) != 0;
}

bool DTreeLazyNTE::containsNode(node_key_t key) const {
    return nodes.find(toInternalKey(key)) != nodes.end();
}

uint64_t DTreeLazyNTE::getNumNodes() const {
    return nodes.size();
}

IndexMemoryFootprint DTreeLazyNTE::memoryFootprint() const {
    using Node = dtree_lazy_nte_internal::DNodeLazyNTE;
    IndexMemoryFootprint result;
    result.numNodes = nodes.size();
    result.bytesNodes = sizeof(*this) +
        memory_footprint_detail::mapBucketBytes<int, Node*>(nodes.bucket_count(), nodes.size()) +
        nodes.size() * (memory_footprint_detail::mapNodeBytes<int, Node*>() + sizeof(Node));
    const auto setNode = memory_footprint_detail::setNodeBytes<Node*>();
    for (const auto& [key, node] : nodes) {
        result.numTreeEdges += node->children.size();
        if (node->nte) {
            // The set object is allocated separately from Node; its entries
            // are counted with the other set nodes in bytesEdges below.
            result.bytesNodes += sizeof(*node->nte);
            result.numNonTreeEdges += node->nte->size();
        }
    }
    result.bytesEdges = (result.numTreeEdges + result.numNonTreeEdges) * setNode;
    result.numNonTreeEdges /= 2; // Each undirected edge occurs in both endpoint sets.
    return result;
}

} // end of namespace algo_extension
} // end of namespace kuzu
