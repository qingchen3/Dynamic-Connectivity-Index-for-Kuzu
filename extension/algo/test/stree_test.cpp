#include "common/stree.h"

#include "gtest/gtest.h"

using namespace kuzu::algo_extension;

TEST(STreeTest, InsertAndQueryConnectivity) {
    STree tree;

    tree.insertEdge(1, 2);
    tree.insertEdge(2, 3);
    tree.insertEdge(4, 5);

    EXPECT_TRUE(tree.connected(1, 3));
    EXPECT_TRUE(tree.connected(4, 5));
    EXPECT_FALSE(tree.connected(1, 4));
    EXPECT_FALSE(tree.connected(1, 99));
    EXPECT_EQ(tree.getNumNodes(), 5);
}

TEST(STreeTest, DeleteTreeEdgeDisconnectsWhenNoReplacementExists) {
    STree tree;

    tree.insertEdge(1, 2);
    tree.insertEdge(2, 3);
    tree.deleteEdge(2, 3);

    EXPECT_TRUE(tree.connected(1, 2));
    EXPECT_FALSE(tree.connected(1, 3));
}

TEST(STreeTest, DeleteTreeEdgeUsesNonTreeReplacement) {
    STree tree;

    tree.insertEdge(1, 2);
    tree.insertEdge(2, 3);
    tree.insertEdge(1, 3);
    tree.deleteEdge(2, 3);

    EXPECT_TRUE(tree.connected(1, 3));

    tree.deleteEdge(1, 3);

    EXPECT_TRUE(tree.connected(1, 2));
    EXPECT_FALSE(tree.connected(1, 3));
}
