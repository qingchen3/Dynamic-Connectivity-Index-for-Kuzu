#include "common/assert.h"
#include "common/stree_csr.h"
#include "common/exception/exception.h"

#include <queue>
#include <set>

namespace kuzu {
namespace algo_extension {

STree_CSR::SNode_CSR* STree_CSR::getOrCreateNode(node_key_t key) {
    auto [it, inserted] = nodes.try_emplace(key, nullptr);
    if (inserted) {
        it->second = std::make_unique<SNode_CSR>(key);
    }
    return it->second.get();
}

STree_CSR::SNode_CSR* STree_CSR::getNode(node_key_t key) const {
    auto it = nodes.find(key);
    return it == nodes.end() ? nullptr : it->second.get();
}

bool STree_CSR::containsNode(node_key_t key) const {
    return getNode(key) != nullptr;
}

uint64_t STree_CSR::getNumNodes() const {
    return nodes.size();
}

STree_CSR::SNode_CSR* STree_CSR::findRoot(SNode_CSR* node) {
    while (node->parent != nullptr) {
        node = node->skip != nullptr ? node->skip : node->parent;
    }
    return node;
}

void STree_CSR::insertEdge(node_key_t u, node_key_t v) {
    auto uNode = getOrCreateNode(u);
    auto vNode = getOrCreateNode(v);

    if (uNode->parent == vNode || vNode->parent == uNode) {
        return;
    }

    if (findRoot(uNode) != findRoot(vNode)) {
        insertTreeEdge(u, v);
    } 
}

void STree_CSR::insertTreeEdge(node_key_t u, node_key_t v) {
    auto ch = reroot(getOrCreateNode(u));
    auto vNode = getOrCreateNode(v);
    ch->parent = vNode;
    if (vNode->children.size() == 1) {
        auto childOfV = *vNode->children.begin();
        childOfV->skip = nullptr;
    }
    vNode->children.insert(ch);

    auto cur = ch;
    while (cur->children.size() == 1 && cur->skip == nullptr) {
        cur = *cur->children.begin();
    }

    auto par = cur->parent;
    while (par != nullptr && par->children.size() == 1) {
        if (par->parent != nullptr) {
            cur->skip = par->parent;
            par->skip = nullptr;
            if (par->parent->skip != nullptr) {
                return;
            }
        }
        cur = par->parent;
        par = cur != nullptr ? cur->parent : nullptr;
    }
}

STree_CSR::SNode_CSR* STree_CSR::reroot(SNode_CSR* node) {
    if (node->parent == nullptr) {
        node->skip = nullptr;
        return node;
    }

    if (node->children.size() == 1) {
        auto child = *node->children.begin();
        child->skip = nullptr;
    }

    auto ch = node;
    auto cur = ch->parent;
    node->parent = nullptr;
    node->skip = nullptr;
    while (cur != nullptr) {
        auto par = cur->parent;
        cur->parent = ch;
        cur->children.erase(ch);
        ch->children.insert(cur);
        cur->skip = nullptr;
        ch = cur;
        cur = par;
    }

    auto newCur = ch;
    auto newPar = newCur->parent;
    while (newPar != nullptr) {
        auto newGrandParent = newPar->parent;
        newCur->skip = nullptr;
        newPar->skip = nullptr;

        if (newPar->children.size() == 1) {
            if (newGrandParent != nullptr) {
                newCur->skip = newGrandParent;
                newCur = newGrandParent;
            } else {
                newCur = newPar;
            }
            newPar = newCur->parent;
        } else {
            newCur = newPar;
            newPar = newGrandParent;
        }
    }
    return node;
}

void STree_CSR::deleteEdge(
    node_key_t u,
    node_key_t v,
    const NeighborProvider& getNeighbors) {
    auto uNode = getNode(u);
    auto vNode = getNode(v);
    if (uNode == nullptr || vNode == nullptr) {
        return;
    }

    if (uNode->parent != vNode && vNode->parent != uNode) {
        return;
    } else if (uNode->parent == vNode) {
        deleteTreeEdge(v, u, getNeighbors);
        return;
    } else if (vNode->parent == uNode) {
        deleteTreeEdge(u, v, getNeighbors);
    } else {
        throw kuzu::common::Exception("An error occurs in STree_CSR::deleteEdge.");
    }
}

void STree_CSR::deleteTreeEdge(node_key_t parent, node_key_t child, const NeighborProvider& getNeighbors) {
    auto parentNode = getNode(parent);
    auto childNode = getNode(child);
    if (parentNode == nullptr || childNode == nullptr || childNode->parent != parentNode) {
        return;
    }

    parentNode->children.erase(childNode);
    if (parentNode->children.size() == 1) {
        auto temp = parentNode;
        while (temp->children.size() == 1) {
            temp = *temp->children.begin();
        }

        auto tempPar = temp->parent;
        while (tempPar != nullptr && tempPar->children.size() == 1) {
            if (tempPar->parent != nullptr) {
                temp->skip = tempPar->parent;
                tempPar->skip = nullptr;
                if (tempPar->parent->skip != nullptr) {
                    break;
                }
            }
            temp = tempPar->parent;
            tempPar = temp != nullptr ? temp->parent : nullptr;
        }
    }

    childNode->parent = nullptr;
    childNode->skip = nullptr;
    if (childNode->children.size() == 1) {
        auto grandChild = *childNode->children.begin();
        grandChild->skip = nullptr;
    }

    auto [connectedNode, nteNeighbor] = searchReplacement(childNode, getNeighbors);
    if (nteNeighbor != nullptr) {
        insertTreeEdge(connectedNode->key, nteNeighbor->key);
    }
}

std::pair<STree_CSR::SNode_CSR*, STree_CSR::SNode_CSR*> STree_CSR::searchReplacement(
    SNode_CSR* startNode,
    const NeighborProvider& getNeighbors) {
    if (startNode == nullptr) {
        return {nullptr, nullptr};
    }

    std::queue<SNode_CSR*> q;
    q.push(startNode);
    while (!q.empty()) {
        auto current = q.front();
        q.pop();

        for (auto ngbrKey : getNeighbors(current->key)) {
            auto ngbrNode = getNode(ngbrKey); 
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

            if (findRoot(ngbrNode) != startNode) {
                return {current, ngbrNode};
            }
        }

        for (auto child : current->children) {
            q.push(child);
        }
    }
    return {nullptr, nullptr};
}

bool STree_CSR::connected(node_key_t u, node_key_t v) const {
    auto uNode = getNode(u);
    auto vNode = getNode(v);
    if (uNode == nullptr || vNode == nullptr) {
        return false;
    }
    return findRoot(uNode) == findRoot(vNode);
}

} // namespace algo_extension
} // namespace kuzu
