#include "index/native_dynamic_connectivity_index.h"
#include "common/dynamic_connectivity_index_factory.h"
#include "storage/table/rel_table.h"
#include "storage/storage_manager.h"
#include "catalog/catalog.h"
#include "graph/on_disk_graph.h"
#include "common/exception/runtime.h"
#include <limits>
#include <string>
#include <set>
#include <vector>
#include <tuple>

namespace kuzu {
namespace algo_extension {

NativeDynamicConnectivityIndex::NativeDynamicConnectivityIndex(
    storage::IndexInfo indexInfo,
    std::unique_ptr<storage::IndexStorageInfo> storageInfo,
    common::table_id_t sourceRelTableID,
    const std::string& method)
    : storage::Index{std::move(indexInfo), std::move(storageInfo)},
      sourceRelTableID{sourceRelTableID},
      backend{createDynamicConnectivityIndex(method)} {}

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

DynamicConnectivityIndex::node_key_t NativeDynamicConnectivityIndex::toBackendKey(
    common::offset_t offset) {
    using BackendKey = DynamicConnectivityIndex::node_key_t;
    
    if (offset > static_cast<common::offset_t>(
        std::numeric_limits<BackendKey>::max())) {
        throw common::RuntimeException { 
            "Node offset exceeds backend key range."};
    }
    return static_cast<BackendKey>(offset);
}

DynamicConnectivityIndex::node_key_t NativeDynamicConnectivityIndex::toBackendKey(
    common::nodeID_t id) const {
    if (id.tableID != indexInfo.tableID) {
        throw common::RuntimeException {
            "Node belongs to a different node table."};
    }

    return toBackendKey(id.offset);
}

common::nodeID_t NativeDynamicConnectivityIndex::makeNodeID(
    common::offset_t offset,
    common::table_id_t tableID) {
    common::nodeID_t nodeID;
    nodeID.offset = offset;
    nodeID.tableID = tableID;
    return nodeID;
}

void NativeDynamicConnectivityIndex::insertEdge(common::nodeID_t src, common::nodeID_t dst) {
    backend->insertEdge(toBackendKey(src), toBackendKey(dst));
}

void NativeDynamicConnectivityIndex::deleteEdge(
    common::nodeID_t src,
    common::nodeID_t dst,
    const DynamicConnectivityIndex::NeighborProvider& getNeighbors) {

    backend->deleteEdge(
        toBackendKey(src),
        toBackendKey(dst),
        getNeighbors);
}

bool NativeDynamicConnectivityIndex::connected(common::nodeID_t src, common::nodeID_t dst) const {
    return backend->connected(toBackendKey(src), toBackendKey(dst));
}

bool NativeDynamicConnectivityIndex::isBackedByRelTable(
    common::table_id_t relTableID) const {
    return relTableID == sourceRelTableID;
}

std::vector<NativeDynamicConnectivityIndex::IncidentRel>
NativeDynamicConnectivityIndex::collectIncidentRels(
    main::ClientContext* context,
    common::offset_t nodeOffset) const {

    auto transaction = transaction::Transaction::Get(*context);
    auto catalog = catalog::Catalog::Get(*context);
    auto storageManager = storage::StorageManager::Get(*context);

    const auto nodeTableID = indexInfo.tableID;

    auto relTable = storageManager
                        ->getTable(sourceRelTableID)
                        ->ptrCast<storage::RelTable>();

    const auto relGroupID = relTable->getRelGroupID();

    auto nodeEntry =
        catalog->getTableCatalogEntry(transaction, nodeTableID);

    auto relGroupEntry =
        catalog->getTableCatalogEntry(transaction, relGroupID);

    graph::NativeGraphEntry graphEntry{
        std::vector<catalog::TableCatalogEntry*>{nodeEntry},
        std::vector<catalog::TableCatalogEntry*>{relGroupEntry}};

    graph::OnDiskGraph graph{context, std::move(graphEntry)};

    auto scanState = graph.prepareRelScan(
        *relGroupEntry,
        sourceRelTableID,
        nodeTableID,
        {common::InternalKeyword::ID},
        true /* randomLookup */);

    std::vector<NativeDynamicConnectivityIndex::IncidentRel> rawRels;
    const common::nodeID_t nodeID{nodeOffset, nodeTableID};

    auto collect =
        [&](graph::Graph::EdgeIterator iterator,
            common::RelDataDirection direction) {
            for (auto chunk : iterator) {
                chunk.forEach(
                    [&](auto neighbors, auto propertyVectors, auto i) {
                        const auto nbrOffset = neighbors[i].offset;
                        const auto scannedRelID =
                            propertyVectors[0]
                                ->template getValue<common::internalID_t>(i);

                        rawRels.push_back(IncidentRel{
                            nbrOffset,
                            scannedRelID,
                            direction,
                        });
                    });
            }
        };

    collect(
        graph.scanFwd(nodeID, *scanState),
        common::RelDataDirection::FWD);

    collect(
        graph.scanBwd(nodeID, *scanState),
        common::RelDataDirection::BWD);

    using IncidenceKey = std::tuple<
        common::table_id_t,
        common::offset_t,
        common::RelDataDirection>;
    
    std::vector<NativeDynamicConnectivityIndex::IncidentRel> uniqueRels;
    std::set<IncidenceKey> seen;

    for(const auto& edge : rawRels) {
        const IncidenceKey key{
            edge.relID.offset,
            edge.relID.tableID,
            edge.direction};

        if (seen.insert(key).second) {
            uniqueRels.push_back(edge);
        }
    }

    return uniqueRels;
}

void NativeDynamicConnectivityIndex::commitRelInsert(common::offset_t srcNodeOffset,
    common::offset_t dstNodeOffset) {
    backend->insertEdge(
        toBackendKey(srcNodeOffset), 
        toBackendKey(dstNodeOffset));
}

void NativeDynamicConnectivityIndex::commitRelInsert(
    main::ClientContext* context,
    common::offset_t srcNodeOffset, 
    common::offset_t dstNodeOffset, 
    common::internalID_t relID) {
    KU_ASSERT(relID.tableID == sourceRelTableID);
    
    //const auto srcIncidentRels =
    //    collectIncidentRels(context, srcNodeOffset);
    //const auto dstIncidentRels =
    //    collectIncidentRels(context, dstNodeOffset);
    commitRelInsert(srcNodeOffset, dstNodeOffset);
}

void NativeDynamicConnectivityIndex::commitRelDelete(
    main::ClientContext* context,
    common::offset_t srcNodeOffset,
    common::offset_t dstNodeOffset,
    common::internalID_t relID) {

    KU_ASSERT(relID.tableID == sourceRelTableID);

    DynamicConnectivityIndex::NeighborProvider getNeighbors =
        [this, context](
            DynamicConnectivityIndex::node_key_t nodeKey) {

            KU_ASSERT(nodeKey >= 0);

            const auto nodeOffset =
                static_cast<common::offset_t>(nodeKey);

            const auto incidentRels =
                collectIncidentRels(context, nodeOffset);

            std::set<DynamicConnectivityIndex::node_key_t>
                uniqueNeighbors;

            for (const auto& edge : incidentRels) {
                const auto nbrKey =
                    static_cast<
                        DynamicConnectivityIndex::node_key_t>(
                        edge.nbrOffset);

                if (nbrKey != nodeKey) {
                    uniqueNeighbors.insert(nbrKey);
                }
            }

            return std::vector<
                DynamicConnectivityIndex::node_key_t>{
                uniqueNeighbors.begin(),
                uniqueNeighbors.end()};
        };

    backend->deleteEdge(
        toBackendKey(srcNodeOffset),
        toBackendKey(dstNodeOffset),
        getNeighbors);
}

void NativeDynamicConnectivityIndex::commitRelDelete(common::offset_t srcNodeOffset,
    common::offset_t dstNodeOffset) {
    //backend->deleteEdge(
    //    toBackendKey(srcNodeOffset), 
    //    toBackendKey(dstNodeOffset));
    return;
}

} // namespace algo_extension
} // namespace kuzu