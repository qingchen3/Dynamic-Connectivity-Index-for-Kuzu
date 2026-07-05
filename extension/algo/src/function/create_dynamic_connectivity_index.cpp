#include "function/algo_function.h"

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/binder.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "index/native_dynamic_connectivity_index.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"
#include "processor/execution_context.h"

#include <atomic>

using namespace kuzu::common;
using namespace kuzu::function;

namespace kuzu {
namespace algo_extension {

struct CreateDynamicConnectivityIndexBindData final : TableFuncBindData {
    table_id_t nodeTableID;
    table_id_t relGroupID;
    table_id_t relTableID;
    std::string indexName;
    std::string method;

    CreateDynamicConnectivityIndexBindData(table_id_t nodeTableID,
        table_id_t relGroupID, table_id_t relTableID, std::string indexName, 
        std::string method, binder::expression_vector columns)
        : TableFuncBindData{std::move(columns), 1}, nodeTableID{nodeTableID},
          relGroupID{relGroupID}, relTableID{relTableID}, 
          indexName{std::move(indexName)}, method{std::move(method)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<CreateDynamicConnectivityIndexBindData>(
            nodeTableID, relGroupID, relTableID, indexName, method, columns);
    }
};

struct CreateDynamicConnectivityIndexSharedState final
    : TableFuncSharedState {
    std::atomic_bool created{false};
};

static std::unique_ptr<TableFuncBindData> bindFunc(
    main::ClientContext* context, const TableFuncBindInput* input) {
    auto nodeTableName = input->getLiteralVal<std::string>(0);
    auto relTableName = input->getLiteralVal<std::string>(1);
    auto indexName = input->getLiteralVal<std::string>(2);
    auto method = input->getLiteralVal<std::string>(3);

    binder::Binder::validateTableExistence(*context, nodeTableName);
    binder::Binder::validateTableExistence(*context, relTableName);

    auto transaction = transaction::Transaction::Get(*context);
    auto catalog = catalog::Catalog::Get(*context);

    auto nodeEntry =
        catalog->getTableCatalogEntry(transaction, nodeTableName);
    binder::Binder::validateNodeTableType(nodeEntry);

    auto relEntry =
        catalog->getTableCatalogEntry(transaction, relTableName);
    if (relEntry->getType() != catalog::CatalogEntryType::REL_GROUP_ENTRY) {
        throw BinderException{
            relTableName + " is not a relationship table."};
    }

    auto relGroupEntry =
        relEntry->ptrCast<catalog::RelGroupCatalogEntry>();
    if (relGroupEntry->getNumRelTables() != 1) {
        throw BinderException{
            "Dynamic connectivity currently requires one physical relationship table."};
    }

    auto nodeTableID = nodeEntry->getTableID();
    auto relGroupID = relEntry->getTableID();
    auto relTableID = relGroupEntry->getSingleRelEntryInfo().oid;

    auto nodeTable =
        storage::StorageManager::Get(*context)
            ->getTable(nodeTableID)
            ->ptrCast<storage::NodeTable>();
    if (nodeTable->getIndex(indexName).has_value()) {
        throw BinderException{"Index " + indexName + " already exists."};
    }

    std::vector<std::string> names;
    std::vector<LogicalType> types;

    names.emplace_back("result");
    types.emplace_back(LogicalType::STRING());

    names = TableFunction::extractYieldVariables(
        names, input->yieldVariables);
    auto columns = input->binder->createVariables(names, types);

    return std::make_unique<CreateDynamicConnectivityIndexBindData>(
        nodeTableID, relGroupID, relTableID, std::move(indexName), 
        std::move(method), std::move(columns));
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput&) {
    return std::make_unique<
        CreateDynamicConnectivityIndexSharedState>();
}

static offset_t buildIndexFromExistingRels(main::ClientContext* context,
    table_id_t nodeTableID, table_id_t relGroupID, table_id_t relTableID,
    NativeDynamicConnectivityIndex& index) {
    auto transaction = transaction::Transaction::Get(*context);
    auto catalog = catalog::Catalog::Get(*context);

    auto nodeEntry = catalog->getTableCatalogEntry(transaction, nodeTableID);
    auto relEntry = catalog->getTableCatalogEntry(transaction, relGroupID);

    graph::NativeGraphEntry graphEntry{
        std::vector<catalog::TableCatalogEntry*>{nodeEntry},
        std::vector<catalog::TableCatalogEntry*>{relEntry}};
    graph::OnDiskGraph graph{context, std::move(graphEntry)};

    auto maxOffset = graph.getMaxOffset(transaction, nodeTableID);
    auto scanState = graph.prepareRelScan(
        *relEntry, relTableID, nodeTableID, {}, true /* randomLookup */);

    offset_t numInsertedEdges = 0;
    for (offset_t srcOffset = 0; srcOffset < maxOffset; srcOffset++) {
        nodeID_t srcNodeID{srcOffset, nodeTableID};

        for (auto chunk : graph.scanFwd(srcNodeID, *scanState)) {
            chunk.forEach([&](auto neighbors, auto, auto i) {
                auto dstNodeID = neighbors[i];
                index.insertEdge(srcOffset, dstNodeID.offset);
                numInsertedEdges++;
            });
        }
    }
    return numInsertedEdges;
}

static offset_t tableFunc(
    const TableFuncInput& input, TableFuncOutput& output) {
    auto sharedState = input.sharedState->ptrCast<
        CreateDynamicConnectivityIndexSharedState>();

    bool expected = false;
    if (!sharedState->created.compare_exchange_strong(expected, true)) {
        return 0;
    }

    auto bindData = input.bindData->constPtrCast<
        CreateDynamicConnectivityIndexBindData>();

    storage::IndexInfo indexInfo{
        bindData->indexName,
        NativeDynamicConnectivityIndex::TYPE_NAME,
        bindData->nodeTableID,
        std::vector<column_id_t>{},
        std::vector<PhysicalTypeID>{},
        false /* isPrimary */,
        false /* isBuiltin */};

    auto index = std::make_unique<NativeDynamicConnectivityIndex>(
        std::move(indexInfo),
        std::make_unique<storage::IndexStorageInfo>(),
        bindData->relTableID,
        bindData->method);

    auto numBuiltEdges = buildIndexFromExistingRels (
        input.context->clientContext,
        bindData->nodeTableID,
        bindData->relGroupID,
        bindData->relTableID,
        *index);
    
    auto nodeTable =
        storage::StorageManager::Get(*input.context->clientContext)
            ->getTable(bindData->nodeTableID)
            ->ptrCast<storage::NodeTable>();

    nodeTable->addIndex(std::move(index));

    auto& outputVector = output.dataChunk.getValueVectorMutable(0);
    auto pos = output.dataChunk.state->getSelVector()[0];
    outputVector.setValue(
    pos, "Dynamic connectivity index " + bindData->indexName +
             " has been created with " + std::to_string(numBuiltEdges) +
             " edges.");
    return 1;
}

function_set CreateDynamicConnectivityIndexFunction::getFunctionSet() {
    function_set result;
    std::vector inputTypes{
        LogicalTypeID::STRING,
        LogicalTypeID::STRING,
        LogicalTypeID::STRING,
        LogicalTypeID::STRING};

    auto function = std::make_unique<TableFunction>(name, inputTypes);
    function->bindFunc = bindFunc;
    function->initSharedStateFunc = initSharedState;
    function->initLocalStateFunc = TableFunction::initEmptyLocalState;
    function->tableFunc = tableFunc;
    result.push_back(std::move(function));
    return result;
}

} // namespace algo_extension
} // namespace kuzu