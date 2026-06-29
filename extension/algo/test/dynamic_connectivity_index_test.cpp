#include "common/dynamic_connectivity_index_factory.h"
#include "common/stree_index.h"
#include "common/dtree_index.h"

#include "index/native_dynamic_connectivity_index.h"

#include "gtest/gtest.h"

#include <memory>
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



namespace {

std::unique_ptr<kuzu::algo_extension::NativeDynamicConnectivityIndex>
createNativeIndex(const std::string& indexName, const std::string& method) {
    kuzu::storage::IndexInfo indexInfo{
        indexName,
        "DYNAMIC_CONNECTIVITY",
        1 /* owner node table ID */,
        std::vector<kuzu::common::column_id_t>{},
        std::vector<kuzu::common::PhysicalTypeID>{},
        false /* isPrimary */,
        false /* isBuiltin */};

    return std::make_unique<
        kuzu::algo_extension::NativeDynamicConnectivityIndex>(
        std::move(indexInfo),
        std::make_unique<kuzu::storage::IndexStorageInfo>(),
        2 /* source relationship table ID */,
        method);
}

} // namespace

TEST(NativeDynamicConnectivityIndexTest, DelegatesToSTree) {
    auto index = createNativeIndex("dc_stree", "stree");

    index->insertEdge(1, 2);
    index->insertEdge(2, 3);

    EXPECT_TRUE(index->connected(1, 3));
    EXPECT_EQ(index->getMethod(), "stree");
    EXPECT_EQ(index->getSourceRelTableID(), 2);

    index->deleteEdge(2, 3);
    EXPECT_FALSE(index->connected(1, 3));
}

TEST(NativeDynamicConnectivityIndexTest, DelegatesToDTree) {
    auto index = createNativeIndex("dc_dtree", "dtree");

    index->insertEdge(10, 20);
    index->insertEdge(20, 30);

    EXPECT_TRUE(index->connected(10, 30));
    EXPECT_EQ(index->getMethod(), "dtree");
    EXPECT_EQ(index->getSourceRelTableID(), 2);

    index->deleteEdge(20, 30);
    EXPECT_FALSE(index->connected(10, 30));
}