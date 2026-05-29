#include "common/stree_index.h"

#include "gtest/gtest.h"

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

TEST(DynamicConnectivityIndexTest, STreeIndexDeleteTreeEdgeDisconnectsWhenNoReplacementExists) {
    STreeIndex index;

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