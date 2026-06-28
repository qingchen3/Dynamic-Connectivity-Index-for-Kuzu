#include "function/algo_function.h"

#include "binder/binder.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "main/client_context.h"

using namespace kuzu::common;
using namespace kuzu::function;

namespace kuzu {
namespace algo_extension {

struct DynamicConnectivityQuerySharedState final : TableFuncSharedState {
    std::atomic_bool emitted{false};
};

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput&) {
    return std::make_unique<DynamicConnectivityQuerySharedState>();
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    auto sharedState = input.sharedState->ptrCast<DynamicConnectivityQuerySharedState>();

    bool expected = false;
    if (!sharedState->emitted.compare_exchange_strong(expected, true)) {
        return 0;
    }

    auto& outputVector = output.dataChunk.getValueVectorMutable(0);
    auto pos = output.dataChunk.state->getSelVector()[0];
    outputVector.setValue(pos, true);
    return 1;
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext*,
    const TableFuncBindInput* input) {
    std::vector<std::string> returnColumnNames;
    std::vector<LogicalType> returnTypes;

    returnColumnNames.emplace_back("connected");
    returnTypes.emplace_back(LogicalType::BOOL());

    returnColumnNames =
        TableFunction::extractYieldVariables(returnColumnNames, input->yieldVariables);
    auto columns = input->binder->createVariables(returnColumnNames, returnTypes);

    return std::make_unique<TableFuncBindData>(std::move(columns), 1 /* one row result */);
}

function_set DynamicConnectivityQueryFunction::getFunctionSet() {
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