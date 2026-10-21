#include "common/assert.h"
#include "common/stree_csr.h"
#include "common/exception/exception.h"

#include <chrono>
#include <queue>
#include <set>

#include "common/dc_runtime_profile.h"
namespace dcprof = kuzu::common::dc_profile;

namespace kuzu {
namespace algo_extension {

STree_CSR::SNode_CSR* STree_CSR::getOrCreateNode(node_key_t key) {
    dcprof::Scope dcPhase{dcprof::Phase::NodeLookup};

    auto [it, inserted] = nodes.try_emplace(key, nullptr);
    if (inserted) {
        it->second = std::make_unique<SNode_CSR>(key);
    }
    return it->second.get();
}

STree_CSR::SNode_CSR* STree_CSR::getNode(node_key_t key) const {
    dcprof::Scope dcPhase{dcprof::Phase::NodeLookup};

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
    dcprof::Scope dcPhase{dcprof::Phase::RootWalk};

    while (node->parent != nullptr) {
        dcprof::count(dcprof::Counter::RootSteps);
        node = node->skip != nullptr ? node->skip : node->parent;
    }
    return node;
}

void STree_CSR::insertEdge(node_key_t u, node_key_t v) {
    dcprof::Event dcEvent{"index_insert", "stree_csr", u, v};
    dcprof::Scope dcClassify{dcprof::Phase::Classify};

    auto uNode = getOrCreateNode(u);
    auto vNode = getOrCreateNode(v);

    if (uNode->parent == vNode || vNode->parent == uNode) {
        dcprof::kind("duplicate");
        return;
    }

    dcprof::kind("non_tree");
    if (findRoot(uNode) != findRoot(vNode)) {
        dcClassify.stop();
        dcprof::kind("tree");
        insertTreeEdge(u, v);
    } 
}

void STree_CSR::insertTreeEdge(node_key_t u, node_key_t v) {
    dcprof::Scope dcPhase{dcprof::Phase::Link};

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
    dcprof::Scope dcPhase{dcprof::Phase::Reroot};

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
    dcprof::Event dcEvent{"index_delete", "stree_csr", u, v};
    dcprof::Scope dcClassify{dcprof::Phase::Classify};

    lastDeleteDiagnostics_ = DeleteDiagnostics{};
    auto uNode = getNode(u);
    auto vNode = getNode(v);
    if (uNode == nullptr || vNode == nullptr) {
        return;
    }

    if (uNode->parent != vNode && vNode->parent != uNode) {
        dcprof::kind("non_tree");
        lastDeleteDiagnostics_.edgeKind = DeleteDiagnostics::EdgeKind::NON_TREE;
        return;
    } else if (uNode->parent == vNode) {
        dcClassify.stop();
        deleteTreeEdge(v, u, getNeighbors);
        return;
    } else if (vNode->parent == uNode) {
        dcClassify.stop();
        deleteTreeEdge(u, v, getNeighbors);
    } else {
        throw kuzu::common::Exception("An error occurs in STree_CSR::deleteEdge.");
    }
}

void STree_CSR::deleteTreeEdge(node_key_t parent, node_key_t child, const NeighborProvider& getNeighbors) {
    dcprof::Scope dcClassify{dcprof::Phase::Classify};

    auto parentNode = getNode(parent);
    auto childNode = getNode(child);
    if (parentNode == nullptr || childNode == nullptr || childNode->parent != parentNode) {
        return;
    }
    lastDeleteDiagnostics_.edgeKind = DeleteDiagnostics::EdgeKind::TREE;
    lastDeleteDiagnostics_.replacementSearchTriggered = true;

    dcClassify.stop();
    dcprof::kind("tree");
    dcprof::Scope dcCut{dcprof::Phase::Cut};
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

    dcCut.stop();
    const auto searchStart = std::chrono::steady_clock::now();
    auto [connectedNode, nteNeighbor] = searchReplacement(childNode, getNeighbors);
    lastDeleteDiagnostics_.replacementSearchElapsedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - searchStart).count();
    if (nteNeighbor != nullptr) {
        dcprof::Scope dcInstall{dcprof::Phase::ReplacementInstall};
        dcprof::count(dcprof::Counter::ReplacementFound);
        lastDeleteDiagnostics_.replacementFound = true;
        insertTreeEdge(connectedNode->key, nteNeighbor->key);
    }
}

std::pair<STree_CSR::SNode_CSR*, STree_CSR::SNode_CSR*> STree_CSR::searchReplacement(
    SNode_CSR* startNode,
    const NeighborProvider& getNeighbors) {
    dcprof::Scope dcPhase{dcprof::Phase::Search};
    dcprof::count(dcprof::Counter::Searches);

    if (startNode == nullptr) {
        return {nullptr, nullptr};
    }

    std::queue<SNode_CSR*> q;
    q.push(startNode);
    while (!q.empty()) {
        auto current = q.front();
        q.pop();
        dcprof::count(dcprof::Counter::SearchVertices);

        const auto getNeighborsStart = std::chrono::steady_clock::now();
        dcprof::Scope dcProvider{dcprof::Phase::Provider};
        auto neighbors = getNeighbors(current->key);
        dcProvider.stop();
        dcprof::count(dcprof::Counter::NeighborIds, neighbors.size());
        lastDeleteDiagnostics_.getNeighborsElapsedNs +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - getNeighborsStart).count();
        ++lastDeleteDiagnostics_.getNeighborsCallCount;
        lastDeleteDiagnostics_.getNeighborsReturnedIdCount += neighbors.size();

        for (auto ngbrKey : neighbors) {
            dcprof::Scope dcCandidate{dcprof::Phase::Candidate};
            dcprof::count(dcprof::Counter::Candidates);
            ++lastDeleteDiagnostics_.replacementCandidatesScanned;
            auto ngbrNode = getNode(ngbrKey); 
            if (ngbrNode == nullptr) {
                throw kuzu::common::Exception(
                    "Graph neighbor is absent from STree_CSR");
            }

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

IndexMemoryFootprint STree_CSR::memoryFootprint() const {
    IndexMemoryFootprint result;
    result.numNodes = nodes.size();

    result.bytesNodes =
        sizeof(*this) +
        memory_footprint_detail::mapBucketBytes<
            node_key_t, std::unique_ptr<SNode_CSR>>(
            nodes.bucket_count(), nodes.size()) +
        nodes.size() *
            (memory_footprint_detail::mapNodeBytes<
                 node_key_t, std::unique_ptr<SNode_CSR>>() +
             sizeof(SNode_CSR));

    const auto setNodeBytes =
        memory_footprint_detail::setNodeBytes<SNode_CSR*>();

    for (const auto& [key, node] : nodes) {
        result.numTreeEdges += node->children.size();
    }

    result.bytesEdges = result.numTreeEdges * setNodeBytes;
    return result;
}

} // namespace algo_extension
} // namespace kuzu
