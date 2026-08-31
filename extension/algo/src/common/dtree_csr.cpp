#include <set>
#include <cstdlib>
#include <climits>
#include <unordered_map>
#include <cstddef>
#include <utility>
#include <vector>
#include <queue>
#include <iostream>
#include <cassert>
#include <limits>
#include <stdexcept>

#include "common/assert.h"

#include "common/dtree_csr.h"

namespace kuzu {
namespace algo_extension {

namespace dtree_internal {
    DNode_CSR* reroot(DNode_CSR * n_w) {
        if (n_w->parent != nullptr) {
            DNode_CSR* ch = n_w;
            DNode_CSR* cur = ch->parent;
            n_w->parent = nullptr;
            while (cur != nullptr) {
                DNode_CSR* g = cur->parent;
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

    DNode_CSR* link(DNode_CSR* n_u, DNode_CSR* r_u, DNode_CSR* n_v) { //make n_u the parent of n_v
        n_v->parent = n_u;
        n_u->children.insert(n_v);

        DNode_CSR* c = n_u;
        DNode_CSR* new_root = nullptr;
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

    std::pair<DNode_CSR*, DNode_CSR*> unlink(DNode_CSR* n_v) {
        if (n_v->parent == nullptr) {
            return std::make_pair(n_v, n_v);
        }
        DNode_CSR* c = n_v;
        while (c->parent != nullptr) {
            c = c->parent;
            c->size -= n_v->size;
        }
        n_v->parent->children.erase(n_v);
        n_v->parent = nullptr;
        return std::make_pair(n_v, c);
    }

    std::pair<DNode_CSR*, int> find_root(DNode_CSR* node) {
        int dist = 0;
        while(node->parent != nullptr) {
            node = node->parent;
            dist++;
        }
        return std::make_pair(node, dist);
    }

    void insert_edge(int u, int v, std::unordered_map<int, DNode_CSR*> &Dtree) {

        if (Dtree.find(u) == Dtree.end()) {
            Dtree[u] = new DNode_CSR(u);
        }

        if (Dtree.find(v) == Dtree.end()) {
            Dtree[v] = new DNode_CSR(v);    
        }

        std::pair<DNode_CSR*, int> res_u = find_root(Dtree[u]);
        DNode_CSR* r_u = res_u.first;
        int dist_u = res_u.second;
        std::pair<DNode_CSR*, int> res_v = find_root(Dtree[v]);
        DNode_CSR* r_v = res_v.first;
        int dist_v = res_v.second;

        if(r_u->key != r_v->key) {
            insert_te(Dtree[u], Dtree[v], r_u, r_v);
        } else {
            if (Dtree[u]->parent != Dtree[v] && Dtree[v]->parent != Dtree[u])
                insert_nte(r_u, Dtree[u], dist_u, Dtree[v], dist_v);
        }
        //cal_size(Dtree);
    }

    void insert_nte(DNode_CSR* r, DNode_CSR* n_u, int dist_u, DNode_CSR* n_v, int dist_v) {
        // TODO: multi-edges:
        // if (n_u->nte.find(n_v) != n_u->nte.end() && n_v->nte.find(n_u) != n_v->nte.end()) return; 

        if (abs(dist_u - dist_v) < 2) {
            return;
        } else {
            DNode_CSR* shallow = nullptr;
            DNode_CSR* deep = nullptr;
            if(dist_u < dist_v) {
                deep = n_v;
                shallow = n_u;
            } else {
                deep = n_u;
                shallow = n_v;
            }
            int delta = abs(dist_u - dist_v) - 2;
            DNode_CSR* c = deep;
            for(int i = 0; i < delta; i++) c = c->parent;
            unlink(c);
            link(shallow, r, reroot(deep));
            return;
        }
    }

    // insert_te return value discarded (DTree.cpp:99)  
    // insert_te returns the (potentially new) root after rebalancing, but insert_edge ignores it. If the root changes, nothing tracks it. 
    DNode_CSR* insert_te(DNode_CSR* n_u, DNode_CSR* n_v, DNode_CSR* r_u, DNode_CSR* r_v) {
        if(r_v->size < r_u->size) {
            return link(n_u, r_u, reroot(n_v));
        } else {
            return link(n_v, r_v, reroot(n_u));
        }
    }

    std::pair<DNode_CSR*, DNode_CSR*> delete_te(
        DNode_CSR* n_u, 
        DNode_CSR* n_v,
        std::unordered_map<int, DNode_CSR*> &Dtree,
        const DynamicConnectivityIndex::NeighborProvider& getNeighbors) {
        // determine parent and child
        DNode_CSR* ch = nullptr;
        if(n_u->parent == n_v) {
            ch = n_u;
        } else if(n_v->parent == n_u) {
            ch = n_v;
        } else {
            // edge does not exist as a tree edge
            return std::make_pair(n_u, n_v);
        }

        DNode_CSR* root = nullptr;
        std::pair<DNode_CSR*, DNode_CSR*>res = unlink(ch);
        ch = res.first;
        root = res.second;

        DNode_CSR* r_s = nullptr;
        DNode_CSR* r_l = nullptr;

        if(ch->size < root->size) {
            r_s = ch;
            r_l = root;        
        } else {
            r_s = root;
            r_l = ch;
        }

        std::tuple<DNode_CSR*, DNode_CSR*, DNode_CSR*> res_bfs_sel = BFS_select(
            r_s, 
            Dtree, 
            getNeighbors);
        DNode_CSR* n_rs = std::get<0>(res_bfs_sel);
        DNode_CSR* n_rl = std::get<1>(res_bfs_sel);
        DNode_CSR* new_r = std::get<2>(res_bfs_sel);

        if(n_rs == nullptr && n_rl == nullptr) {
            if(new_r != nullptr) r_s = reroot(new_r);   
            return std::make_pair(r_s, r_l);
        } else {
            return std::make_pair(insert_te(n_rs, n_rl, r_s, r_l), nullptr);
        }
    }

    std::tuple<DNode_CSR*, DNode_CSR*, DNode_CSR*> BFS_select(
        DNode_CSR* r,
        std::unordered_map<int, DNode_CSR*> &Dtree,
        const DynamicConnectivityIndex::NeighborProvider& getNeighbors) {
        std::queue<DNode_CSR*> q;
        q.push(r);

        DNode_CSR* new_r = nullptr;
        int S = r->size;
        int min_dist = INT_MAX;

        DNode_CSR* n_rs = nullptr;
        DNode_CSR* n_rl = nullptr;

        while(!q.empty()) {
            std::queue<DNode_CSR*> new_q;
            while(!q.empty()) {
                DNode_CSR *current;
                current = q.front();
                q.pop();
                if (current->size> S / 2 && current->size < S && new_r == nullptr) new_r = current;
                
                for (auto ngbrKey : getNeighbors(current->key)) {
                    auto ngbrNode = Dtree[ngbrKey]; 
                    KU_ASSERT(ngbrNode != nullptr);

                    if (ngbrNode == current) { // handling of self-loops.
                        continue;
                    }

                    const bool isParent =
                        (ngbrNode == current->parent);

                    const bool isChild =
                        (current->children.find(ngbrNode) !=
                        current->children.end());

                    if (isParent || isChild) {
                        continue;
                    }

                    std::pair <DNode_CSR*, int> res_find_root = find_root(ngbrNode);
                    DNode_CSR* rt = res_find_root.first;
                    int dist = res_find_root.second;
                    if(rt->key == r->key) continue;

                    if(dist < min_dist) {
                        min_dist = dist;
                        n_rl = ngbrNode;
                        n_rs = current;
                    }

                }
                for (auto it : current->children) new_q.push(it);
            }
            q = new_q;
        }
        return std::make_tuple(n_rs, n_rl, new_r);
    }

    void delete_edge(
        int u, 
        int v, 
        std::unordered_map<int, DNode_CSR*> &Dtree,
        const DynamicConnectivityIndex::NeighborProvider& getNeighbors) {
        if(Dtree.find(u) == Dtree.end() || Dtree.find(v) == Dtree.end()) {
            return;
        }

        const bool isParent =
            (Dtree[u] == Dtree[v]->parent);

        const bool isChild =
            (Dtree[u]->children.find(Dtree[v]) !=
                Dtree[u]->children.end());

        if (isParent || isChild) {
            delete_te(Dtree[u], Dtree[v], Dtree, getNeighbors);
        }
    }

    int query(DNode_CSR* n_u, DNode_CSR* n_v) {
        DNode_CSR* d_u = nullptr;

        while (n_u->parent != nullptr) {
            d_u = n_u;
            n_u = n_u->parent;
        }
        if (d_u != nullptr && d_u->size > n_u->size / 2) n_u = reroot(d_u);

        DNode_CSR* d_v = nullptr;
        while (n_v->parent != nullptr) {
            d_v = n_v;
            n_v = n_v->parent;
        }
        if (d_v != nullptr && d_v->size > n_v->size / 2) n_v = reroot(d_v);

        return n_u->key == n_v->key;
    }

    int query_simple(DNode_CSR* n_u, DNode_CSR* n_v) {
        while (n_u->parent != nullptr) n_u = n_u->parent;
        while (n_v->parent != nullptr) n_v = n_v->parent;
        return n_u->key == n_v->key;
    }

    void cal_size(std::unordered_map<int, DNode_CSR*> &Dtree) {
        int total_size = 0;
        for(auto it = Dtree.begin(); it != Dtree.end(); it++) {
            size_t key_mem = sizeof(it->first);
            size_t ptr_mem = sizeof(it->second);

            total_size += key_mem + ptr_mem;
        }
    }

} // end of namespace dtree_internal

DTree_CSR::~DTree_CSR() {
    for (auto& entry : nodes) {
        delete entry.second;
    }
}

int DTree_CSR::toInternalKey(node_key_t key) {
    if (key < std::numeric_limits<int>::min() || key > std::numeric_limits<int>::max()) {
        throw std::runtime_error("DTree currently only supports node IDs within int range.");
    }
    return static_cast<int>(key);
}

void DTree_CSR::insertEdge(node_key_t u, node_key_t v) {
    dtree_internal::insert_edge(toInternalKey(u), toInternalKey(v), nodes);
}

void DTree_CSR::deleteEdge(
    node_key_t u,
    node_key_t v,
    const DynamicConnectivityIndex::NeighborProvider& getNeighbors) {
    dtree_internal::delete_edge(
        toInternalKey(u), 
        toInternalKey(v), 
        nodes,
        getNeighbors);
}

bool DTree_CSR::connected(node_key_t u, node_key_t v) const {
    auto uIt = nodes.find(toInternalKey(u));
    auto vIt = nodes.find(toInternalKey(v));
    if (uIt == nodes.end() || vIt == nodes.end()) {
        return false;
    }
    return dtree_internal::query_simple(uIt->second, vIt->second) != 0;
}

bool DTree_CSR::containsNode(node_key_t key) const {
    return nodes.find(toInternalKey(key)) != nodes.end();
}

uint64_t DTree_CSR::getNumNodes() const {
    return nodes.size();
}

} // end of namespace algo_extension
} // end of namespace kuzu
