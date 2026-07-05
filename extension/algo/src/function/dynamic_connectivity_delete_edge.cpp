#include "function/algo_function.h"

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "common/exception/binder.h"
#include "common/exception/runtime.h"
#include "common/string_format.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "function/table/table_function.h"
#include "index/native_dynamic_connectivity_index.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"

using namespace kuzu::common;
using namespace kuzu::function;

namespace kuzu {
namespace algo_extension {

struct DynamicConnectivityDeleteEdgeBindData final : TableFuncBindData {
    table_id_t nodeTableID;
    std::string indexName;
    int64_t src;
    int64_t dst;

    DynamicConnectivityDeleteEdgeBindData(table_id_t nodeTableID, std::string indexName,
        int64_t src, int64_t dst, binder::expression_vector columns)
        : TableFuncBindData{std::move(columns), 1}, nodeTableID{nodeTableID},
          indexName{std::move(indexName)}, src{src}, dst{dst} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DynamicConnectivityDeleteEdgeBindData>(*this);
    }
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
        throw BinderException(stringFormat("Index {} does not exist.", indexName));
    }
    auto index = indexOpt.value();
    if (index->getIndexInfo().indexType != NativeDynamicConnectivityIndex::TYPE_NAME) {
        throw BinderException(
            stringFormat("Index {} is not a dynamic connectivity index.", indexName));
    }

    std::vector<std::string> names;
    std::vector<LogicalType> types;

    names.emplace_back("result");
    types.emplace_back(LogicalType::STRING());

    names = TableFunction::extractYieldVariables(names, input->yieldVariables);
    auto columns = input->binder->createVariables(names, types);

    return std::make_unique<DynamicConnectivityDeleteEdgeBindData>(nodeTableID,
        std::move(indexName), src, dst, std::move(columns));
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    auto sharedState = input.sharedState->ptrCast<SimpleTableFuncSharedState>();
    auto morsel = sharedState->getMorsel();
    if (!morsel.hasMoreToOutput()) {
        return 0;
    }
    auto bindData = input.bindData->constPtrCast<DynamicConnectivityDeleteEdgeBindData>();
    auto clientContext = input.context->clientContext;
    auto nodeTable = storage::StorageManager::Get(*clientContext)
                         ->getTable(bindData->nodeTableID)
                         ->ptrCast<storage::NodeTable>();
    auto indexOpt = nodeTable->getIndex(bindData->indexName);
    if (!indexOpt.has_value()) {
        throw RuntimeException(stringFormat("Index {} does not exist.", bindData->indexName));
    }
    auto index = indexOpt.value();
    if (index->getIndexInfo().indexType != NativeDynamicConnectivityIndex::TYPE_NAME) {
        throw RuntimeException(
            stringFormat("Index {} is not a dynamic connectivity index.", bindData->indexName));
    }
    auto& dcIndex = index->cast<NativeDynamicConnectivityIndex>();
    dcIndex.deleteEdge(bindData->src, bindData->dst);
    auto& outputVector = output.dataChunk.getValueVectorMutable(0);
    auto pos = output.dataChunk.state->getSelVector()[0];
    outputVector.setValue(pos, stringFormat("Deleted edge ({}, {}) from index {}.", bindData->src,
                                   bindData->dst, bindData->indexName));
    return 1;
}

function_set DynamicConnectivityDeleteEdgeFunction::getFunctionSet() {
    function_set result;

    std::vector inputTypes{
        LogicalTypeID::STRING,
        LogicalTypeID::STRING,
        LogicalTypeID::INT64,
        LogicalTypeID::INT64};

    auto function = std::make_unique<TableFunction>(name, inputTypes);
    function->bindFunc = bindFunc;
    function->tableFunc = tableFunc;
    function->initSharedStateFunc = SimpleTableFunc::initSharedState;
    function->initLocalStateFunc = TableFunction::initEmptyLocalState;
    function->canParallelFunc = [] { return false; };
    function->isReadOnly = false;
    result.push_back(std::move(function));
    return result;
}

} // namespace algo_extension
} // namespace kuzu