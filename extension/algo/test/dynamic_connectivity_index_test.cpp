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

constexpr kuzu::common::table_id_t TEST_NODE_TABLE_ID = 1;
constexpr kuzu::common::table_id_t TEST_REL_TABLE_ID = 2;

std::unique_ptr<kuzu::algo_extension::NativeDynamicConnectivityIndex>
createNativeIndex(const std::string& indexName, const std::string& method) {
    kuzu::storage::IndexInfo indexInfo{
        indexName,
        "DYNAMIC_CONNECTIVITY",
        TEST_NODE_TABLE_ID,
        std::vector<kuzu::common::column_id_t>{},
        std::vector<kuzu::common::PhysicalTypeID>{},
        false /* isPrimary */,
        false /* isBuiltin */};

    return std::make_unique<
        kuzu::algo_extension::NativeDynamicConnectivityIndex>(
        std::move(indexInfo),
        std::make_unique<kuzu::storage::IndexStorageInfo>(),
        TEST_REL_TABLE_ID,
        method);
}

kuzu::common::nodeID_t makeNodeID(
    kuzu::common::offset_t offset,
    kuzu::common::table_id_t tableID) {
    kuzu::common::nodeID_t nodeID;
    nodeID.offset = offset;
    nodeID.tableID = tableID;
    return nodeID;
}

} // namespace


TEST(NativeDynamicConnectivityIndexTest, DelegatesToSTree) {
    auto index = createNativeIndex("dc_stree", "stree");

    index->insertEdge(
        makeNodeID(1, TEST_NODE_TABLE_ID),
        makeNodeID(2, TEST_NODE_TABLE_ID));

    index->insertEdge(
        makeNodeID(2, TEST_NODE_TABLE_ID),
        makeNodeID(3, TEST_NODE_TABLE_ID));

    EXPECT_TRUE(index->connected(
        makeNodeID(1, TEST_NODE_TABLE_ID),
        makeNodeID(3, TEST_NODE_TABLE_ID)));

    EXPECT_EQ(index->getMethod(), "stree");
    EXPECT_EQ(index->getSourceRelTableID(), TEST_REL_TABLE_ID);

    index->deleteEdge(
        makeNodeID(2, TEST_NODE_TABLE_ID),
        makeNodeID(3, TEST_NODE_TABLE_ID));

    EXPECT_FALSE(index->connected(
        makeNodeID(1, TEST_NODE_TABLE_ID),
        makeNodeID(3, TEST_NODE_TABLE_ID)));
}

TEST(NativeDynamicConnectivityIndexTest, DelegatesToDTree) {
    auto index = createNativeIndex("dc_dtree", "dtree");

    index->insertEdge(
        makeNodeID(10, TEST_NODE_TABLE_ID),
        makeNodeID(20, TEST_NODE_TABLE_ID));

    index->insertEdge(
        makeNodeID(20, TEST_NODE_TABLE_ID),
        makeNodeID(30, TEST_NODE_TABLE_ID));

    EXPECT_TRUE(index->connected(
        makeNodeID(10, TEST_NODE_TABLE_ID),
        makeNodeID(30, TEST_NODE_TABLE_ID)));

    EXPECT_EQ(index->getMethod(), "dtree");
    EXPECT_EQ(index->getSourceRelTableID(), TEST_REL_TABLE_ID);

    index->deleteEdge(
        makeNodeID(20, TEST_NODE_TABLE_ID),
        makeNodeID(30, TEST_NODE_TABLE_ID));

    EXPECT_FALSE(index->connected(
        makeNodeID(10, TEST_NODE_TABLE_ID),
        makeNodeID(30, TEST_NODE_TABLE_ID)));
}


TEST(NativeDynamicConnectivityIndexTest, RejectsUnexpectedNodeTable) {
    auto index = createNativeIndex("dc_stree", "stree");

    EXPECT_THROW(
        index->insertEdge(
            makeNodeID(1, TEST_NODE_TABLE_ID),
            makeNodeID(2, TEST_NODE_TABLE_ID + 1)),
        kuzu::common::RuntimeException);
}


TEST(NativeDynamicConnectivityIndexTest, CommitRelInsertAndDeleteDelegateToBackend) {
    auto index = createNativeIndex("dc_stree", "stree");

    index->commitRelInsert(1, 2);
    index->commitRelInsert(2, 3);

    EXPECT_TRUE(index->connected(
        makeNodeID(1, TEST_NODE_TABLE_ID),
        makeNodeID(3, TEST_NODE_TABLE_ID)));

    index->commitRelDelete(2, 3);

    EXPECT_FALSE(index->connected(
        makeNodeID(1, TEST_NODE_TABLE_ID),
        makeNodeID(3, TEST_NODE_TABLE_ID)));
}

