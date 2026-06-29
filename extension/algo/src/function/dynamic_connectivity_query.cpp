#include "function/algo_function.h"

#include "binder/binder.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "main/client_context.h"

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
    std::string graphName;
    int64_t src;
    int64_t dst;
    std::string method;

    DynamicConnectivityQueryBindData(std::string graphName, int64_t src, int64_t dst,
        std::string method, binder::expression_vector columns, offset_t numRows)
        : TableFuncBindData{std::move(columns), numRows},
          graphName{std::move(graphName)}, src{src}, dst{dst},
          method{std::move(method)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DynamicConnectivityQueryBindData>(
            graphName, src, dst, method, columns, numRows);
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
    //(void)bindData;

    traceWithThread("graph=" + bindData->graphName +
        ", src=" + std::to_string(bindData->src) +
        ", dst=" + std::to_string(bindData->dst) +
        ", method=" + bindData->method);

    traceWithThread("4c. tableFunc: Won the race, emitting 1 row");

    auto& outputVector = output.dataChunk.getValueVectorMutable(0);
    auto pos = output.dataChunk.state->getSelVector()[0];
    outputVector.setValue(pos, true);
    return 1;
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext*,
    const TableFuncBindInput* input) {
    traceDyn("2. bindFunc called");
    auto graphName = input->getLiteralVal<std::string>(0);
    auto src = input->getLiteralVal<int64_t>(1);
    auto dst = input->getLiteralVal<int64_t>(2);
    auto method = input->getLiteralVal<std::string>(3);

    std::vector<std::string> returnColumnNames;
    std::vector<LogicalType> returnTypes;

    returnColumnNames.emplace_back("connected");
    returnTypes.emplace_back(LogicalType::BOOL());

    returnColumnNames =
        TableFunction::extractYieldVariables(returnColumnNames, input->yieldVariables);
    auto columns = input->binder->createVariables(returnColumnNames, returnTypes);

    //return std::make_unique<TableFuncBindData>(std::move(columns), 1 /* one row result */);
    return std::make_unique<DynamicConnectivityQueryBindData>(
        std::move(graphName), src, dst, std::move(method),
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