#include "index/native_dynamic_connectivity_index.h"

#include "common/dynamic_connectivity_index_factory.h"

namespace kuzu {
namespace algo_extension {

NativeDynamicConnectivityIndex::NativeDynamicConnectivityIndex(
    storage::IndexInfo indexInfo,
    std::unique_ptr<storage::IndexStorageInfo> storageInfo,
    common::table_id_t sourceRelTableID, const std::string& method)
    : storage::Index{std::move(indexInfo), std::move(storageInfo)},
      backend{createDynamicConnectivityIndex(method)},
      sourceRelTableID{sourceRelTableID} {}

std::unique_ptr<storage::Index::InsertState>
NativeDynamicConnectivityIndex::initInsertState(
    main::ClientContext*, storage::visible_func) {
    return std::make_unique<storage::Index::InsertState>();
}

std::unique_ptr<storage::Index::DeleteState>
NativeDynamicConnectivityIndex::initDeleteState(
    const transaction::Transaction*, storage::MemoryManager*,
    storage::visible_func) {
    return std::make_unique<storage::Index::DeleteState>();
}

void NativeDynamicConnectivityIndex::delete_(
    transaction::Transaction*, const common::ValueVector&,
    storage::Index::DeleteState&) {
    // Node-table deletion maintenance is added after relationship maintenance is transactional.
}

void NativeDynamicConnectivityIndex::insertEdge(node_key_t src, node_key_t dst) {
    backend->insertEdge(src, dst);
}

void NativeDynamicConnectivityIndex::deleteEdge(node_key_t src, node_key_t dst) {
    backend->deleteEdge(src, dst);
}

bool NativeDynamicConnectivityIndex::connected(node_key_t src, node_key_t dst) const {
    return backend->connected(src, dst);
}

bool NativeDynamicConnectivityIndex::needCommitRelInsert(
    common::table_id_t relTableID) const {
    return relTableID == sourceRelTableID;
}

void NativeDynamicConnectivityIndex::commitRelInsert(common::offset_t srcNodeOffset,
    common::offset_t dstNodeOffset) {
    backend->insertEdge(static_cast<int64_t>(srcNodeOffset),
        static_cast<int64_t>(dstNodeOffset));
}

} // namespace algo_extension
} // namespace kuzu