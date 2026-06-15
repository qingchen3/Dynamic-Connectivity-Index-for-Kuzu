#include "common/dynamic_connectivity_index_factory.h"
#include "common/stree_index.h"
#include "common/dtree_index.h"

#include "gtest/gtest.h"

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
