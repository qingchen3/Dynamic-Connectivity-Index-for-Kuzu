#include <set>
#include <cstdlib>
#include <climits>
#include <unordered_map>
#include "common/dtree.h"
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

namespace dtree_internal {

    DNode* reroot(DNode * n_w) {
        if (n_w->parent != nullptr) {
            DNode* ch = n_w;
            DNode* cur = ch->parent;
            n_w->parent = nullptr;
            while (cur != nullptr) {
                DNode* g = cur->parent;
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


    DNode* link(DNode* n_u, DNode* r_u, DNode* n_v) {
        n_v->parent = n_u;
        n_u->children.insert(n_v);

        DNode* c = n_u;
        DNode* new_root = nullptr;
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


    std::pair<DNode*, DNode*> unlink(DNode* n_v) {
        if (n_v->parent == nullptr) {
            return std::make_pair(n_v, n_v);
        }
        DNode* c = n_v;
        while (c->parent != nullptr) {
            c = c->parent;
            c->size -= n_v->size;
        }
        n_v->parent->children.erase(n_v);
        n_v->parent = nullptr;
        return std::make_pair(n_v, c);
    }


    std::pair<DNode*, int> find_root(DNode* node) {
        int dist = 0;
        while(node->parent != nullptr) {
            node = node->parent;
            dist++;
        }
        return std::make_pair(node, dist);
    }


    void insert_edge(int u, int v, std::unordered_map<int, DNode*> &Dtree) {

        if (Dtree.find(u) == Dtree.end()) {
            Dtree[u] = new DNode(u);
        }

        if (Dtree.find(v) == Dtree.end()) {
            Dtree[v] = new DNode(v);    
        }

        std::pair<DNode*, int> res_u = find_root(Dtree[u]);
        DNode* r_u = res_u.first;
        int dist_u = res_u.second;
        std::pair<DNode*, int> res_v = find_root(Dtree[v]);
        DNode* r_v = res_v.first;
        int dist_v = res_v.second;

        if(r_u->key != r_v->key) {
            insert_te(Dtree[u], Dtree[v], r_u, r_v);
        } else {
            if (Dtree[u]->parent != Dtree[v] && Dtree[v]->parent != Dtree[u])
                insert_nte(r_u, Dtree[u], dist_u, Dtree[v], dist_v);
        }
        //cal_size(Dtree);
    }

    void insert_nte(DNode* r, DNode* n_u, int dist_u, DNode* n_v, int dist_v) {
        if (n_u->nte.find(n_v) != n_u->nte.end() && n_v->nte.find(n_u) != n_v->nte.end()) return;

        if (abs(dist_u - dist_v) < 2) {
            n_u->nte.insert(n_v);
            n_v->nte.insert(n_u);
            return;
        } else {
            DNode* shallow = nullptr;
            DNode* deep = nullptr;
            if(dist_u < dist_v) {
                deep = n_v;
                shallow = n_u;
            } else {
                deep = n_u;
                shallow = n_v;
            }
            int delta = abs(dist_u - dist_v) - 2;
            DNode* c = deep;
            for(int i = 0; i < delta; i++) c = c->parent;
            c->parent->nte.insert(c);
            c->nte.insert(c->parent);

            unlink(c);
            link(shallow, r, reroot(deep));
            return;
        }
    }

    //  insert_te return value discarded (DTree.cpp:99)  
    // insert_te returns the (potentially new) root after rebalancing, but insert_edge ignores it. If the root changes, nothing tracks it. 
    DNode* insert_te(DNode* n_u, DNode* n_v, DNode* r_u, DNode* r_v) {
        if(r_v->size > r_u->size) {
            return link(n_u, r_u, reroot(n_v));
        } else {
            return link(n_v, r_v, reroot(n_u));
        }
    }


    void delete_nte(DNode* n_u, DNode* n_v) {
        n_u->nte.erase(n_v);
        n_v->nte.erase(n_u);
    }


    std::pair<DNode*, DNode*> delete_te(DNode* n_u, DNode* n_v, DeleteDiagnostics& diag) {
        // determine parent and child
        DNode* ch = nullptr;
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

        DNode* root = nullptr;
        std::pair<DNode*, DNode*>res = unlink(ch);
        ch = res.first;
        root = res.second;

        DNode* r_s = nullptr;
        DNode* r_l = nullptr;

        if(ch->size < root->size) {
            r_s = ch;
            r_l = root;        
        } else {
            r_s = root;
            r_l = ch;
        }

        std::tuple<DNode*, DNode*, DNode*> res_bfs_sel = BFS_select(r_s, diag);
        DNode* n_rs = std::get<0>(res_bfs_sel);
        DNode* n_rl = std::get<1>(res_bfs_sel);
        DNode* new_r = std::get<2>(res_bfs_sel);

        if(n_rs == nullptr && n_rl == nullptr) {
            if(new_r != nullptr) r_s = reroot(new_r);   
            return std::make_pair(r_s, r_l);
        } else {
            diag.replacementFound = true;
            n_rs->nte.erase(n_rl);
            n_rl->nte.erase(n_rs);
            return std::make_pair(insert_te(n_rs, n_rl, r_s, r_l), nullptr);
        }
    }


    std::tuple<DNode*, DNode*, DNode*> BFS_select(DNode* r, DeleteDiagnostics& diag) {
        std::queue<DNode*> q;
        q.push(r);

        DNode* new_r = nullptr;
        int S = r->size;
        int min_dist = INT_MAX;

        DNode* n_rs = nullptr;
        DNode* n_rl = nullptr;

        while(!q.empty()) {
            std::queue<DNode*> new_q;
            while(!q.empty()) {
                DNode *node;
                node = q.front();
                q.pop();
                if (node->size> S / 2 && node->size < S && new_r == nullptr) new_r = node;

                if (!node->nte.empty()) {
                    for (auto it : node->nte) {
                        diag.replacementCandidatesScanned++;
                        std::pair <DNode*, int> res_find_root = find_root(it);
                        DNode* rt = res_find_root.first;
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


    void delete_edge(int u, int v, std::unordered_map<int, DNode*> &Dtree, DeleteDiagnostics& diag) {
        if(Dtree.find(u) == Dtree.end() || Dtree.find(v) == Dtree.end()) {
            return;
        }
        if(Dtree[v]->nte.find(Dtree[u]) != Dtree[v]->nte.end() || Dtree[u]->nte.find(Dtree[v]) != Dtree[u]->nte.end()) {
            diag.edgeKind = DeleteDiagnostics::EdgeKind::NON_TREE;
            delete_nte(Dtree[u], Dtree[v]);
        } else {
            delete_te(Dtree[u], Dtree[v], diag);
        }
    }


    int query(DNode* n_u, DNode* n_v) {
        DNode* d_u = nullptr;

        while (n_u->parent != nullptr) {
            d_u = n_u;
            n_u = n_u->parent;
        }
        if (d_u != nullptr && d_u->size > n_u->size / 2) n_u = reroot(d_u);

        DNode* d_v = nullptr;
        while (n_v->parent != nullptr) {
            d_v = n_v;
            n_v = n_v->parent;
        }
        if (d_v != nullptr && d_v->size > n_v->size / 2) n_v = reroot(d_v);

        return n_u->key == n_v->key;
    }


    int query_simple(DNode* n_u, DNode* n_v) {
        while (n_u->parent != nullptr) n_u = n_u->parent;
        while (n_v->parent != nullptr) n_v = n_v->parent;
        return n_u->key == n_v->key;
    }


    void cal_size(std::unordered_map<int, DNode*> &Dtree) {
        int total_size = 0;
        for(auto it = Dtree.begin(); it != Dtree.end(); it++) {
            size_t key_mem = sizeof(it->first);
            size_t ptr_mem = sizeof(it->second);

            total_size += key_mem + ptr_mem;
        }
        //std::cout << total_size << std::endl;
    }

} // end of namespace dtree_internal

DTree::~DTree() {
    for (auto& entry : nodes) {
        delete entry.second;
    }
}

int DTree::toInternalKey(node_key_t key) {
    if (key < std::numeric_limits<int>::min() || key > std::numeric_limits<int>::max()) {
        throw std::runtime_error("DTree currently only supports node IDs within int range.");
    }
    return static_cast<int>(key);
}

void DTree::insertEdge(node_key_t u, node_key_t v) {
    dtree_internal::insert_edge(toInternalKey(u), toInternalKey(v), nodes);
}

void DTree::deleteEdge(node_key_t u, node_key_t v) {
    lastDeleteDiagnostics_ = DeleteDiagnostics{};
    dtree_internal::delete_edge(toInternalKey(u), toInternalKey(v), nodes, lastDeleteDiagnostics_);
}

bool DTree::connected(node_key_t u, node_key_t v) const {
    auto uIt = nodes.find(toInternalKey(u));
    auto vIt = nodes.find(toInternalKey(v));
    if (uIt == nodes.end() || vIt == nodes.end()) {
        return false;
    }
    return dtree_internal::query_simple(uIt->second, vIt->second) != 0;
}

bool DTree::containsNode(node_key_t key) const {
    return nodes.find(toInternalKey(key)) != nodes.end();
}

uint64_t DTree::getNumNodes() const {
    return nodes.size();
}

} // end of namespace algo_extension
} // end of namespace kuzu
