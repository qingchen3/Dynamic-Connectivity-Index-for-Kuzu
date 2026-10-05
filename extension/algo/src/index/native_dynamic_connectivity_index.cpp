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
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <stdexcept>


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
    common::offset_t nodeOffset,
    std::unique_ptr<graph::OnDiskGraph>& cachedGraph,
    std::unique_ptr<graph::NbrScanState>& cachedScanState,
    NativeDynamicConnectivityIndex::IncidentScanTiming& timing) const {

    using Clock = std::chrono::steady_clock;
    const auto elapsedNs = [](Clock::time_point start) -> uint64_t {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count();
    };

    // This is needed for every node, including after the first call.
    const auto nodeTableID = indexInfo.tableID;

    // Construct the graph and scan state on the first neighbor request only.
    if (!cachedGraph) {
        const auto setupStart = Clock::now();

        auto transaction = transaction::Transaction::Get(*context);
        auto catalog = catalog::Catalog::Get(*context);
        auto storageManager = storage::StorageManager::Get(*context);

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

        auto newGraph = std::make_unique<graph::OnDiskGraph>(
            context, std::move(graphEntry));

        timing.graphSetupNs += elapsedNs(setupStart);

        const auto prepareStart = Clock::now();
        auto newScanState = newGraph->prepareRelScan(
            *relGroupEntry,
            sourceRelTableID,
            nodeTableID,
            {common::InternalKeyword::ID},
            true /* randomLookup */);

        timing.prepareScanNs += elapsedNs(prepareStart);
        ++timing.prepareCalls;

        cachedGraph = std::move(newGraph);
        cachedScanState = std::move(newScanState);
    }

    std::vector<IncidentRel> rawRels;
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

    // Rebind the prepared state to this node and read forward adjacency.
    auto phaseStart = Clock::now();
    collect(
        cachedGraph->scanFwd(nodeID, *cachedScanState),
        common::RelDataDirection::FWD);
    timing.forwardScanNs += elapsedNs(phaseStart);

    // Rebind the same state and read backward adjacency.
    phaseStart = Clock::now();
    collect(
        cachedGraph->scanBwd(nodeID, *cachedScanState),
        common::RelDataDirection::BWD);
    timing.backwardScanNs += elapsedNs(phaseStart);

    phaseStart = Clock::now();

    using IncidenceKey = std::tuple<
        common::table_id_t,
        common::offset_t,
        common::RelDataDirection>;

    std::vector<IncidentRel> uniqueRels;
    std::set<IncidenceKey> seen;

    for (const auto& edge : rawRels) {
        const IncidenceKey key{
            edge.relID.tableID,
            edge.relID.offset,
            edge.direction};

        if (seen.insert(key).second) {
            uniqueRels.push_back(edge);
        }
    }

    timing.relationshipDedupNs += elapsedNs(phaseStart);
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
    commitRelInsert(srcNodeOffset, dstNodeOffset);
}

void NativeDynamicConnectivityIndex::commitRelDelete(
    main::ClientContext* context,
    common::offset_t srcNodeOffset,
    common::offset_t dstNodeOffset,
    common::internalID_t relID) {
    KU_ASSERT(relID.tableID == sourceRelTableID);
    
    uint64_t collectIncidentNs = 0;
    uint64_t uniqueNeighborsNs = 0;
    IncidentScanTiming scanTiming{};

    std::unique_ptr<graph::OnDiskGraph> cachedGraph;
    std::unique_ptr<graph::NbrScanState> cachedScanState;
    const char* setting = std::getenv("DC_REUSE_SCAN_STATE");
    const bool reuseScanState =
        setting == nullptr || !(setting[0] == '0' && setting[1] == '\0');

    const char* verifySetting = std::getenv("DC_VERIFY_SCAN_STATE");
    const bool verifyScanState =
        verifySetting != nullptr && verifySetting[0] == '1';

    DynamicConnectivityIndex::NeighborProvider getNeighbors =
        [this, context, &collectIncidentNs, &uniqueNeighborsNs, 
            &scanTiming, &cachedGraph, &cachedScanState, 
            reuseScanState, verifyScanState](
            DynamicConnectivityIndex::node_key_t nodeKey) {

            KU_ASSERT(nodeKey >= 0);

            const auto nodeOffset =
                static_cast<common::offset_t>(nodeKey);
            
            std::unique_ptr<graph::OnDiskGraph> perCallGraph;
            std::unique_ptr<graph::NbrScanState> perCallScanState;

            auto& graphForCall =
                reuseScanState ? cachedGraph : perCallGraph;
            auto& scanStateForCall =
                reuseScanState ? cachedScanState : perCallScanState;

            const auto scanStart = std::chrono::steady_clock::now();
            const auto incidentRels = collectIncidentRels(
                context, nodeOffset, graphForCall, scanStateForCall, scanTiming);
            collectIncidentNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - scanStart).count();

            const auto dedupStart = std::chrono::steady_clock::now();
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

            //return std::vector<DynamicConnectivityIndex::node_key_t>{
            //    uniqueNeighbors.begin(),
            //    uniqueNeighbors.end()};
            std::vector<DynamicConnectivityIndex::node_key_t> result{
                uniqueNeighbors.begin(), uniqueNeighbors.end()};
            uniqueNeighborsNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - dedupStart).count();
            if (reuseScanState && verifyScanState) {
                std::unique_ptr<graph::OnDiskGraph> freshGraph;
                std::unique_ptr<graph::NbrScanState> freshScanState;
                IncidentScanTiming verificationTiming{};

                const auto freshRels = collectIncidentRels(
                    context, nodeOffset, freshGraph, freshScanState,
                    verificationTiming);

                std::set<DynamicConnectivityIndex::node_key_t> freshNeighbors;
                for (const auto& edge : freshRels) {
                    const auto neighbor =
                        static_cast<DynamicConnectivityIndex::node_key_t>(
                            edge.nbrOffset);
                    if (neighbor != nodeKey) {
                        freshNeighbors.insert(neighbor);
                    }
                }

                const std::vector<DynamicConnectivityIndex::node_key_t> freshResult{
                    freshNeighbors.begin(), freshNeighbors.end()};

                if (result != freshResult) {
                    std::fprintf(stderr,
                        "DC_SCAN_MISMATCH node=%lld cached=%zu fresh=%zu\n",
                        static_cast<long long>(nodeKey),
                        result.size(), freshResult.size());
                    throw std::runtime_error("Cached and fresh neighbor scans differ");
                }
            }
            return result;
        };

    backend->deleteEdge(
        toBackendKey(srcNodeOffset),
        toBackendKey(dstNodeOffset),
        getNeighbors);

    const auto diag = backend->lastDeleteDiagnostics();
    if (diag.replacementSearchTriggered) {
        std::fprintf(stderr,
            "DC_STORAGE_SEARCH method=%s found=%d search_ns=%llu "
            "get_neighbors_ns=%llu calls=%llu returned=%llu candidates=%llu\n",
            backend->getName().c_str(),
            diag.replacementFound ? 1 : 0,
            static_cast<unsigned long long>(diag.replacementSearchElapsedNs),
            static_cast<unsigned long long>(diag.getNeighborsElapsedNs),
            static_cast<unsigned long long>(diag.getNeighborsCallCount),
            static_cast<unsigned long long>(diag.getNeighborsReturnedIdCount),
            static_cast<unsigned long long>(diag.replacementCandidatesScanned));
        std::fprintf(stderr,
            "DC_PROVIDER_PHASE method=%s collect_ns=%llu unique_ns=%llu\n",
            backend->getName().c_str(),
            static_cast<unsigned long long>(collectIncidentNs),
            static_cast<unsigned long long>(uniqueNeighborsNs));
        std::fprintf(stderr,
            "DC_SCAN_PHASE method=%s setup_ns=%llu prepare_ns=%llu "
            "forward_ns=%llu backward_ns=%llu rel_dedup_ns=%llu prepare_calls=%llu\n",
            backend->getName().c_str(),
            static_cast<unsigned long long>(scanTiming.graphSetupNs),
            static_cast<unsigned long long>(scanTiming.prepareScanNs),
            static_cast<unsigned long long>(scanTiming.forwardScanNs),
            static_cast<unsigned long long>(scanTiming.backwardScanNs),
            static_cast<unsigned long long>(scanTiming.relationshipDedupNs),
            static_cast<unsigned long long>(scanTiming.prepareCalls));
    }
}

void NativeDynamicConnectivityIndex::commitRelDelete(common::offset_t srcNodeOffset,
    common::offset_t dstNodeOffset) {
    return;
}

} // namespace algo_extension
} // namespace kuzu