#include "common/dynamic_connectivity_index_factory.h"
#include "common/stree_index.h"
#include "common/dtree_index.h"
#include "common/dtree_lazy_nte_index.h"
#include "common/delete_diagnostics.h"

#include "index/native_dynamic_connectivity_index.h"

#include "gtest/gtest.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <stdexcept>

using namespace kuzu::algo_extension;

TEST(DynamicConnectivityIndexTest, STreeIndexInsertAndQueryConnectivity) {
    STreeIndex index;

    index.insertEdge(1, 2);
    index.insertEdge(2, 3);
    index.insertEdge(4, 5);

    EXPECT_EQ(index.getName(), "stree");
    EXPECT_TRUE(index.connected(1, 3));
    EXPECT_TRUE(index.connected(4, 5));
    EXPECT_FALSE(index.connected(1, 4));
    EXPECT_FALSE(index.connected(1, 99));
    EXPECT_EQ(index.getNumNodes(), 5);
}

TEST(DynamicConnectivityIndexTest, DTreeIndexInsertAndQueryConnectivity) {
    DTreeIndex index;

    index.insertEdge(1, 2);
    index.insertEdge(2, 3);
    index.insertEdge(4, 5);

    EXPECT_EQ(index.getName(), "dtree");
    EXPECT_TRUE(index.connected(1, 3));
    EXPECT_TRUE(index.connected(4, 5));
    EXPECT_FALSE(index.connected(1, 4));
    EXPECT_FALSE(index.connected(1, 99));
    EXPECT_EQ(index.getNumNodes(), 5);
}

TEST(DynamicConnectivityIndexTest, DTreeLazyNTESetAllocatedAndReleased) {
    auto index = createDynamicConnectivityIndex("dtree_lazy_nte");
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->getName(), "dtree_lazy_nte");

    index->insertEdge(1, 2);
    index->insertEdge(1, 3);
    const auto treeOnly = index->memoryFootprint();
    EXPECT_EQ(treeOnly.numNodes, 3u);
    EXPECT_EQ(treeOnly.numTreeEdges, 2u);
    EXPECT_EQ(treeOnly.numNonTreeEdges, 0u);

    index->insertEdge(2, 3);
    const auto withNonTreeEdge = index->memoryFootprint();
    EXPECT_EQ(withNonTreeEdge.numNonTreeEdges, 1u);
    // Each endpoint now owns a set object and one set node.
    EXPECT_EQ(withNonTreeEdge.bytesNodes - treeOnly.bytesNodes,
        2 * sizeof(std::set<dtree_lazy_nte_internal::DNodeLazyNTE*>));
    EXPECT_EQ(withNonTreeEdge.bytesEdges - treeOnly.bytesEdges,
        2 * memory_footprint_detail::setNodeBytes<dtree_lazy_nte_internal::DNodeLazyNTE*>());

    index->deleteEdge(2, 3, {});
    const auto afterDeletion = index->memoryFootprint();
    EXPECT_EQ(afterDeletion.numNonTreeEdges, 0u);
    EXPECT_EQ(afterDeletion.bytesNodes, treeOnly.bytesNodes);
    EXPECT_EQ(afterDeletion.bytesEdges, treeOnly.bytesEdges);
    EXPECT_TRUE(index->connected(2, 3));
}

TEST(DynamicConnectivityIndexTest, DTreeLazyNTEReplacementReleasesSets) {
    DTreeLazyNTEIndex index;
    index.insertEdge(1, 2);
    index.insertEdge(1, 3);
    index.insertEdge(2, 3);

    index.deleteEdge(1, 3);
    EXPECT_TRUE(index.connected(1, 3));
    EXPECT_EQ(index.lastDeleteDiagnostics().edgeKind, DeleteDiagnostics::EdgeKind::TREE);
    EXPECT_TRUE(index.lastDeleteDiagnostics().replacementFound);
    EXPECT_EQ(index.memoryFootprint().numNonTreeEdges, 0u);

    index.deleteEdge(2, 3);
    EXPECT_FALSE(index.connected(1, 3));
}

TEST(DynamicConnectivityIndexTest, STreeIndexDeleteTreeEdgeDisconnectsWhenNoReplacementExists) {
    STreeIndex index;

    index.insertEdge(1, 2);
    index.insertEdge(2, 3);

    EXPECT_TRUE(index.connected(1, 3));

    index.deleteEdge(2, 3);

    EXPECT_TRUE(index.connected(1, 2));
    EXPECT_FALSE(index.connected(1, 3));
}

TEST(DynamicConnectivityIndexTest, DTreeIndexDeleteTreeEdgeDisconnectsWhenNoReplacementExists) {
    DTreeIndex index;

    index.insertEdge(1, 2);
    index.insertEdge(2, 3);

    EXPECT_TRUE(index.connected(1, 3));

    index.deleteEdge(2, 3);

    EXPECT_TRUE(index.connected(1, 2));
    EXPECT_FALSE(index.connected(1, 3));
}


TEST(DynamicConnectivityIndexTest, STreeIndexDeleteTreeEdgeUsesNonTreeReplacement) {
    STreeIndex index;

    index.insertEdge(1, 2);
    index.insertEdge(2, 3);
    index.insertEdge(1, 3);

    EXPECT_TRUE(index.connected(1, 3));

    index.deleteEdge(2, 3);

    EXPECT_TRUE(index.connected(1, 3));

    index.deleteEdge(1, 3);

    EXPECT_TRUE(index.connected(1, 2));
    EXPECT_FALSE(index.connected(1, 3));
}

TEST(DynamicConnectivityIndexTest, DTreeIndexDeleteTreeEdgeUsesNonTreeReplacement) {
    DTreeIndex index;

    index.insertEdge(1, 2);
    index.insertEdge(2, 3);
    index.insertEdge(1, 3);

    EXPECT_TRUE(index.connected(1, 3));

    index.deleteEdge(2, 3);

    EXPECT_TRUE(index.connected(1, 3));
}

TEST(DynamicConnectivityIndexFactoryTest, CreatesSTreeIndexByName) {
    auto index = createDynamicConnectivityIndex("stree");

    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->getName(), "stree");

    index->insertEdge(1, 2);
    index->insertEdge(2, 3);

    EXPECT_TRUE(index->connected(1, 3));
    EXPECT_FALSE(index->connected(1, 4));
}

TEST(DynamicConnectivityIndexFactoryTest, CreatesDTreeIndexByName) {
    auto index = createDynamicConnectivityIndex("dtree");

    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->getName(), "dtree");

    index->insertEdge(1, 2);
    index->insertEdge(2, 3);

    EXPECT_TRUE(index->connected(1, 3));
    EXPECT_FALSE(index->connected(1, 4));
}

TEST(DynamicConnectivityIndexFactoryTest, SupportsCaseInsensitiveMethodNames) {
    auto streeIndex = createDynamicConnectivityIndex("STree");
    auto dtreeIndex = createDynamicConnectivityIndex("DTree");

    ASSERT_NE(streeIndex, nullptr);
    ASSERT_NE(dtreeIndex, nullptr);

    EXPECT_EQ(streeIndex->getName(), "stree");
    EXPECT_EQ(dtreeIndex->getName(), "dtree");
}

TEST(DynamicConnectivityIndexFactoryTest, ThrowsOnUnknownMethodName) {
    EXPECT_THROW(createDynamicConnectivityIndex("unknown"), std::runtime_error);
}

TEST(DynamicConnectivityIndexTest, STreeIndexDeleteDiagnostics) {
    STreeIndex index;

    EXPECT_TRUE(index.supportsDeleteDiagnostics());

    // Deleting a missing edge should be reported as a no-op deletion.
    index.deleteEdge(100, 200);
    {
        auto diag = index.lastDeleteDiagnostics();
        EXPECT_EQ(diag.edgeKind, DeleteDiagnostics::EdgeKind::NONE);
        EXPECT_FALSE(diag.replacementSearchTriggered);
        EXPECT_FALSE(diag.replacementFound);
        EXPECT_EQ(diag.replacementCandidatesScanned, 0u);
    }

    // Deleting a tree edge with no replacement available.
    index.insertEdge(1, 2);
    index.deleteEdge(1, 2);
    {
        auto diag = index.lastDeleteDiagnostics();
        EXPECT_EQ(diag.edgeKind, DeleteDiagnostics::EdgeKind::TREE);
        EXPECT_TRUE(diag.replacementSearchTriggered);
        EXPECT_FALSE(diag.replacementFound);
        EXPECT_EQ(diag.replacementCandidatesScanned, 0u);
        EXPECT_FALSE(index.connected(1, 2));
    }

    // Deleting a non-tree edge should not trigger replacement search.
    index.insertEdge(1, 2);
    index.insertEdge(2, 3);
    index.insertEdge(1, 3);
    index.deleteEdge(1, 3);
    {
        auto diag = index.lastDeleteDiagnostics();
        EXPECT_EQ(diag.edgeKind, DeleteDiagnostics::EdgeKind::NON_TREE);
        EXPECT_FALSE(diag.replacementSearchTriggered);
        EXPECT_FALSE(diag.replacementFound);
        EXPECT_EQ(diag.replacementCandidatesScanned, 0u);
    }

    // Re-add the non-tree edge, then delete a tree edge so replacement search
    // reconnects the component through the non-tree edge.
    index.insertEdge(1, 3);
    index.deleteEdge(2, 3);
    {
        auto diag = index.lastDeleteDiagnostics();
        EXPECT_EQ(diag.edgeKind, DeleteDiagnostics::EdgeKind::TREE);
        EXPECT_TRUE(diag.replacementSearchTriggered);
        EXPECT_TRUE(diag.replacementFound);
        EXPECT_GE(diag.replacementCandidatesScanned, 1u);
        EXPECT_TRUE(index.connected(2, 3));
    }
}

TEST(DynamicConnectivityIndexTest, DTreeIndexDeleteDiagnostics) {
    {
        DTreeIndex index;

        EXPECT_TRUE(index.supportsDeleteDiagnostics());

        // Deleting a missing edge should be reported as a no-op deletion.
        index.deleteEdge(100, 200);
        auto diag = index.lastDeleteDiagnostics();

        EXPECT_EQ(diag.edgeKind, DeleteDiagnostics::EdgeKind::NONE);
        EXPECT_FALSE(diag.replacementSearchTriggered);
        EXPECT_FALSE(diag.replacementFound);
        EXPECT_EQ(diag.replacementCandidatesScanned, 0u);
    }

    {
        DTreeIndex index;

        // Deleting a tree edge with no replacement available.
        index.insertEdge(1, 2);
        index.deleteEdge(1, 2);

        auto diag = index.lastDeleteDiagnostics();

        EXPECT_EQ(diag.edgeKind, DeleteDiagnostics::EdgeKind::TREE);
        EXPECT_TRUE(diag.replacementSearchTriggered);
        EXPECT_FALSE(diag.replacementFound);
        EXPECT_EQ(diag.replacementCandidatesScanned, 0u);
        EXPECT_FALSE(index.connected(1, 2));
    }

    {
        DTreeIndex index;

        // Insert two tree edges from node 1, then a non-tree edge between
        // their children. DTree may otherwise rearrange a path on insertion.
        index.insertEdge(1, 2);
        index.insertEdge(1, 3);
        index.insertEdge(2, 3);
        index.deleteEdge(2, 3);

        auto diag = index.lastDeleteDiagnostics();

        EXPECT_EQ(diag.edgeKind, DeleteDiagnostics::EdgeKind::NON_TREE);
        EXPECT_FALSE(diag.replacementSearchTriggered);
        EXPECT_FALSE(diag.replacementFound);
        EXPECT_EQ(diag.replacementCandidatesScanned, 0u);
    }

    {
        DTreeIndex index;

        // Delete a tree edge so replacement search reconnects the component
        // through an existing non-tree edge.
        index.insertEdge(1, 2);
        index.insertEdge(1, 3);
        index.insertEdge(2, 3);
        index.deleteEdge(1, 3);

        auto diag = index.lastDeleteDiagnostics();

        EXPECT_EQ(diag.edgeKind, DeleteDiagnostics::EdgeKind::TREE);
        EXPECT_TRUE(diag.replacementSearchTriggered);
        EXPECT_TRUE(diag.replacementFound);
        EXPECT_GE(diag.replacementCandidatesScanned, 1u);
        EXPECT_TRUE(index.connected(1, 3));
    }
}

TEST(DynamicConnectivityIndexTest, FourBackendsExposeReplacementSearchDiagnostics) {
    const std::vector<std::pair<int64_t, int64_t>> edges{{1, 2}, {1, 3}, {2, 3}};
    for (const std::string& method : {"dtree", "dtree_csr", "stree", "stree_csr"}) {
        SCOPED_TRACE(method);
        bool foundTreeReplacement = false;
        for (const auto& cut : edges) {
            auto index = createDynamicConnectivityIndex(method);
            ASSERT_TRUE(index->supportsDeleteDiagnostics());
            for (const auto& [u, v] : edges) {
                index->insertEdge(u, v);
            }

            // The provider sees the graph AFTER the base edge has been deleted.
            auto getNeighbors = [&edges, cut](int64_t node) {
                std::vector<int64_t> neighbors;
                for (const auto& [u, v] : edges) {
                    if ((u == cut.first && v == cut.second) ||
                        (v == cut.first && u == cut.second)) {
                        continue;
                    }
                    if (u == node) neighbors.push_back(v);
                    if (v == node) neighbors.push_back(u);
                }
                return neighbors;
            };

            index->deleteEdge(cut.first, cut.second, getNeighbors);
            const auto diag = index->lastDeleteDiagnostics();
            if (!diag.replacementSearchTriggered) {
                continue; // This cut was a non-tree edge in this backend.
            }

            EXPECT_EQ(diag.edgeKind, DeleteDiagnostics::EdgeKind::TREE);
            EXPECT_TRUE(diag.replacementFound);
            EXPECT_GE(diag.replacementCandidatesScanned, 1u);
            EXPECT_GE(diag.replacementSearchElapsedNs, diag.getNeighborsElapsedNs);
            EXPECT_TRUE(index->connected(cut.first, cut.second));

            if (method == "dtree_csr" || method == "stree_csr") {
                EXPECT_GT(diag.getNeighborsCallCount, 0u);
                EXPECT_GE(diag.getNeighborsReturnedIdCount,
                    diag.replacementCandidatesScanned);
            } else {
                EXPECT_EQ(diag.getNeighborsCallCount, 0u);
                EXPECT_EQ(diag.getNeighborsElapsedNs, 0u);
                EXPECT_EQ(diag.getNeighborsReturnedIdCount, 0u);
            }
            foundTreeReplacement = true;
            break;
        }
        EXPECT_TRUE(foundTreeReplacement);
    }
}
