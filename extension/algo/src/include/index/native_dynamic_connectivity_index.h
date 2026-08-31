#pragma once

#include "common/dynamic_connectivity_index.h"
#include "storage/table/rel_table_data.h"
#include "storage/index/index.h"
#include "common/types/types.h"

#include <memory>
#include <string>
#include <vector>

namespace kuzu {
namespace algo_extension {

class NativeDynamicConnectivityIndex final : public storage::Index {
public:
    
    NativeDynamicConnectivityIndex(storage::IndexInfo indexInfo,
        std::unique_ptr<storage::IndexStorageInfo> storageInfo,
        common::table_id_t sourceRelTableID, 
        const std::string& method);

    std::unique_ptr<InsertState> initInsertState(main::ClientContext* context,
        storage::visible_func isVisible) override;

    std::unique_ptr<DeleteState> initDeleteState(
        const transaction::Transaction* transaction, storage::MemoryManager* memoryManager,
        storage::visible_func isVisible) override;
    
    void delete_(transaction::Transaction* transaction,
        const common::ValueVector& nodeIDVector, DeleteState& deleteState) override;
    
    void insertEdge(common::nodeID_t src, common::nodeID_t dst);
    void deleteEdge(common::nodeID_t src, common::nodeID_t dst, const DynamicConnectivityIndex::NeighborProvider& getNeighbors);
    bool connected(common::nodeID_t src, common::nodeID_t dst) const;
    
    static constexpr const char* TYPE_NAME = "DYNAMIC_CONNECTIVITY";
    
    common::table_id_t getSourceRelTableID() const {
        return sourceRelTableID;
    }

    bool isBackedByRelTable(common::table_id_t relTableID) const override;
    void commitRelInsert(main::ClientContext* context,
        common::offset_t srcNodeOffset,
        common::offset_t dstNodeOffset, common::internalID_t relID) override;
    void commitRelInsert(common::offset_t srcNodeOffset,
        common::offset_t dstNodeOffset) override;
    void commitRelDelete(main::ClientContext* context,
        common::offset_t srcNodeOffset,
        common::offset_t dstNodeOffset, common::internalID_t relID) override;
    void commitRelDelete(common::offset_t srcNodeOffset,
        common::offset_t dstNodeOffset) override;

    std::string getMethod() const {
        return backend->getName();
    }

private:
    struct IncidentRel {
        common::offset_t nbrOffset;
        common::internalID_t relID;
        common::RelDataDirection direction;
    };

    std::vector<IncidentRel> collectIncidentRels(
        main::ClientContext* context,
        common::offset_t nodeOffset) const;

    static DynamicConnectivityIndex::node_key_t toBackendKey(
        common::offset_t offset);

    DynamicConnectivityIndex::node_key_t toBackendKey(
        common::nodeID_t id) const;
    
        common::nodeID_t makeNodeID(
        common::offset_t offset, common::table_id_t tableID);

private:
    common::table_id_t sourceRelTableID;
    common::table_id_t nodeTableID;
    std::unique_ptr<DynamicConnectivityIndex> backend;
};

} // namespace algo_extension
} // namespace kuzu