#pragma once

#include "common/dynamic_connectivity_index.h"
#include "storage/index/index.h"

#include <memory>
#include <string>

namespace kuzu {
namespace algo_extension {

class NativeDynamicConnectivityIndex final : public storage::Index {
public:
    using node_key_t = DynamicConnectivityIndex::node_key_t;

    NativeDynamicConnectivityIndex(storage::IndexInfo indexInfo,
        std::unique_ptr<storage::IndexStorageInfo> storageInfo,
        common::table_id_t sourceRelTableID, const std::string& method);

    std::unique_ptr<InsertState> initInsertState(main::ClientContext* context,
        storage::visible_func isVisible) override;

    std::unique_ptr<DeleteState> initDeleteState(
        const transaction::Transaction* transaction, storage::MemoryManager* memoryManager,
        storage::visible_func isVisible) override;

    void delete_(transaction::Transaction* transaction,
        const common::ValueVector& nodeIDVector, DeleteState& deleteState) override;

    void insertEdge(node_key_t src, node_key_t dst);
    void deleteEdge(node_key_t src, node_key_t dst);
    bool connected(node_key_t src, node_key_t dst) const;
    
    static constexpr const char* TYPE_NAME = "DYNAMIC_CONNECTIVITY";
    
    common::table_id_t getSourceRelTableID() const {
        return sourceRelTableID;
    }

    bool needCommitRelInsert(common::table_id_t relTableID) const override;
    void commitRelInsert(common::offset_t srcNodeOffset,
        common::offset_t dstNodeOffset) override;

    std::string getMethod() const {
        return backend->getName();
    }

private:
    std::unique_ptr<DynamicConnectivityIndex> backend;
    common::table_id_t sourceRelTableID;
};

} // namespace algo_extension
} // namespace kuzu