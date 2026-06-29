#include "function/algo_function.h"

#include "binder/binder.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "main/client_context.h"

#include "catalog/catalog.h"
#include "common/exception/binder.h"
#include "index/native_dynamic_connectivity_index.h"
#include "processor/execution_context.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"

#include <fstream>


using namespace kuzu::common;
using namespace kuzu::function;

namespace kuzu {
namespace algo_extension {

void traceDyn(const char* msg) {
    std::ofstream out("/tmp/kuzu_dyn_trace.log", std::ios::app);
    out << msg << std::endl;
}

void traceWithThread(const std::string& msg) {
    std::stringstream ss;
    ss << msg << " | Thread ID: " << std::this_thread::get_id();
    traceDyn(ss.str().c_str());
}

struct DynamicConnectivityQueryBindData final : TableFuncBindData {
    table_id_t nodeTableID;
    int64_t src;
    int64_t dst;
    std::string indexName;

    DynamicConnectivityQueryBindData(table_id_t nodeTableID, int64_t src,
        int64_t dst, std::string indexName,
        binder::expression_vector columns, offset_t numRows)
        : TableFuncBindData{std::move(columns), numRows},
          nodeTableID{nodeTableID}, src{src}, dst{dst},
          indexName{std::move(indexName)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DynamicConnectivityQueryBindData>(
            nodeTableID, src, dst, indexName, columns, numRows);
    }
};

struct DynamicConnectivityQuerySharedState final : TableFuncSharedState {
    std::atomic_bool emitted{false};
};

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput&) {
    traceDyn("3. initSharedState called");
    return std::make_unique<DynamicConnectivityQuerySharedState>();
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    traceDyn("4. tableFunc entered");

    auto sharedState = input.sharedState->ptrCast<DynamicConnectivityQuerySharedState>();

    bool expected = false;
    if (!sharedState->emitted.compare_exchange_strong(expected, true)) {
        traceWithThread("4b. tableFunc: Lost the race, returning 0");
        return 0;
    }

    const auto bindData = input.bindData->constPtrCast<DynamicConnectivityQueryBindData>();

    auto nodeTable =
        storage::StorageManager::Get(*input.context->clientContext)
            ->getTable(bindData->nodeTableID)
            ->ptrCast<storage::NodeTable>();

    auto indexOpt = nodeTable->getIndex(bindData->indexName);
    KU_ASSERT(indexOpt.has_value());

    auto& index =
        indexOpt.value()->cast<NativeDynamicConnectivityIndex>();

    std::stringstream ss;
    ss << "index=" << static_cast<const void*>(&index)
    << ", src=" << bindData->src
    << ", dst=" << bindData->dst
    << ", method=" << index.getMethod();
    traceWithThread(ss.str());

    traceWithThread("4c. tableFunc: Won the race, emitting 1 row");

    const auto isConnected =
        index.connected(bindData->src, bindData->dst);

    auto& outputVector = output.dataChunk.getValueVectorMutable(0);
    auto pos = output.dataChunk.state->getSelVector()[0];
    outputVector.setValue(pos, isConnected);
    return 1;
}

static std::unique_ptr<TableFuncBindData> bindFunc(
    main::ClientContext* context, const TableFuncBindInput* input) {
    traceDyn("2. bindFunc called");

    auto tableName = input->getLiteralVal<std::string>(0);
    auto src = input->getLiteralVal<int64_t>(1);
    auto dst = input->getLiteralVal<int64_t>(2);
    auto indexName = input->getLiteralVal<std::string>(3);

    binder::Binder::validateTableExistence(*context, tableName);
    auto transaction = transaction::Transaction::Get(*context);
    auto tableEntry = catalog::Catalog::Get(*context)
                          ->getTableCatalogEntry(transaction, tableName);
    binder::Binder::validateNodeTableType(tableEntry);

    auto nodeTableID = tableEntry->getTableID();
    auto nodeTable = storage::StorageManager::Get(*context)
                         ->getTable(nodeTableID)
                         ->ptrCast<storage::NodeTable>();

    auto indexOpt = nodeTable->getIndex(indexName);
    if (!indexOpt.has_value()) {
        throw BinderException{"Index " + indexName +
                              " does not exist in table " + tableName + "."};
    }

    // Validate that the named index has the expected runtime type.
    (void)indexOpt.value()->cast<NativeDynamicConnectivityIndex>();

    std::vector<std::string> names;
    std::vector<LogicalType> types;
    names.emplace_back("connected");
    types.emplace_back(LogicalType::BOOL());

    names = TableFunction::extractYieldVariables(
        names, input->yieldVariables);
    auto columns = input->binder->createVariables(names, types);

    return std::make_unique<DynamicConnectivityQueryBindData>(
        nodeTableID, src, dst, std::move(indexName),
        std::move(columns), 1);
}

function_set DynamicConnectivityQueryFunction::getFunctionSet() {
    std::ofstream clearFile("/tmp/kuzu_dyn_trace.log", std::ios::trunc);
    traceDyn("1. getFunctionSet called");

    function_set result;
    std::vector inputTypes = {
        LogicalTypeID::STRING,
        LogicalTypeID::INT64,
        LogicalTypeID::INT64,
        LogicalTypeID::STRING};
    auto func = std::make_unique<TableFunction>(name, inputTypes);
    func->bindFunc = bindFunc;
    func->initSharedStateFunc = initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->tableFunc = tableFunc;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace kuzu