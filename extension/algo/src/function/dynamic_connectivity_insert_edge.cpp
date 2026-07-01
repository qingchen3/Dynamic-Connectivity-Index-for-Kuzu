#include "function/algo_function.h"

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "common/exception/binder.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "index/native_dynamic_connectivity_index.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"

#include <atomic>

using namespace kuzu::common;
using namespace kuzu::function;

namespace kuzu {
namespace algo_extension {

struct DynamicConnectivityInsertEdgeBindData final : TableFuncBindData {
    table_id_t nodeTableID;
    std::string indexName;
    int64_t src;
    int64_t dst;

    DynamicConnectivityInsertEdgeBindData(table_id_t nodeTableID, std::string indexName,
        int64_t src, int64_t dst, binder::expression_vector columns)
        : TableFuncBindData{std::move(columns), 1}, nodeTableID{nodeTableID},
          indexName{std::move(indexName)}, src{src}, dst{dst} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DynamicConnectivityInsertEdgeBindData>(
            nodeTableID, indexName, src, dst, columns);
    }
};

struct DynamicConnectivityInsertEdgeSharedState final : TableFuncSharedState {
    std::atomic_bool inserted{false};
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto tableName = input->getLiteralVal<std::string>(0);
    auto indexName = input->getLiteralVal<std::string>(1);
    auto src = input->getLiteralVal<int64_t>(2);
    auto dst = input->getLiteralVal<int64_t>(3);

    binder::Binder::validateTableExistence(*context, tableName);

    auto transaction = transaction::Transaction::Get(*context);
    auto catalog = catalog::Catalog::Get(*context);
    auto tableEntry = catalog->getTableCatalogEntry(transaction, tableName);
    binder::Binder::validateNodeTableType(tableEntry);

    auto nodeTableID = tableEntry->getTableID();
    auto nodeTable = storage::StorageManager::Get(*context)
                         ->getTable(nodeTableID)
                         ->ptrCast<storage::NodeTable>();

    auto indexOpt = nodeTable->getIndex(indexName);
    if (!indexOpt.has_value()) {
        throw BinderException{"Index " + indexName + " does not exist."};
    }

    (void)indexOpt.value()->cast<NativeDynamicConnectivityIndex>();

    std::vector<std::string> names;
    std::vector<LogicalType> types;

    names.emplace_back("result");
    types.emplace_back(LogicalType::STRING());

    names = TableFunction::extractYieldVariables(names, input->yieldVariables);
    auto columns = input->binder->createVariables(names, types);

    return std::make_unique<DynamicConnectivityInsertEdgeBindData>(
        nodeTableID, std::move(indexName), src, dst, std::move(columns));
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput&) {
    return std::make_unique<DynamicConnectivityInsertEdgeSharedState>();
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    auto sharedState =
        input.sharedState->ptrCast<DynamicConnectivityInsertEdgeSharedState>();

    bool expected = false;
    if (!sharedState->inserted.compare_exchange_strong(expected, true)) {
        return 0;
    }

    auto bindData =
        input.bindData->constPtrCast<DynamicConnectivityInsertEdgeBindData>();

    auto nodeTable = storage::StorageManager::Get(*input.context->clientContext)
                         ->getTable(bindData->nodeTableID)
                         ->ptrCast<storage::NodeTable>();

    auto indexOpt = nodeTable->getIndex(bindData->indexName);
    if (!indexOpt.has_value()) {
        throw RuntimeException{"Index " + bindData->indexName + " does not exist."};
    }

    auto& index = indexOpt.value()->cast<NativeDynamicConnectivityIndex>();
    index.insertEdge(bindData->src, bindData->dst);

    auto& outputVector = output.dataChunk.getValueVectorMutable(0);
    auto pos = output.dataChunk.state->getSelVector()[0];
    outputVector.setValue(pos,
        "Inserted edge (" + std::to_string(bindData->src) + ", " +
            std::to_string(bindData->dst) + ") into dynamic connectivity index " +
            bindData->indexName + ".");
    return 1;
}

function_set DynamicConnectivityInsertEdgeFunction::getFunctionSet() {
    function_set result;
    std::vector inputTypes{
        LogicalTypeID::STRING,
        LogicalTypeID::STRING,
        LogicalTypeID::INT64,
        LogicalTypeID::INT64};

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