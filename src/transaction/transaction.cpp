#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "common/dc_runtime_profile.h"
namespace dcprof = kuzu::common::dc_profile;

#include "transaction/transaction.h"

#include "common/exception/runtime.h"
#include "main/client_context.h"
#include "main/db_config.h"
#include "storage/local_storage/local_node_table.h"
#include "storage/local_storage/local_storage.h"
#include "storage/storage_manager.h"
#include "storage/undo_buffer.h"
#include "storage/wal/local_wal.h"
#include "transaction/transaction_context.h"

using namespace kuzu::catalog;

namespace kuzu {
namespace transaction {

bool LocalCacheManager::put(std::unique_ptr<LocalCacheObject> object) {
    std::unique_lock lck{mtx};
    const auto key = object->getKey();
    if (cachedObjects.contains(key)) {
        return false;
    }
    cachedObjects[object->getKey()] = std::move(object);
    return true;
}

Transaction::Transaction(main::ClientContext& clientContext, TransactionType transactionType,
    common::transaction_t transactionID, common::transaction_t startTS)
    : type{transactionType}, ID{transactionID}, startTS{startTS},
      commitTS{common::INVALID_TRANSACTION}, forceCheckpoint{false}, hasCatalogChanges{false} {
    this->clientContext = &clientContext;
    localStorage = std::make_unique<storage::LocalStorage>(clientContext);
    undoBuffer = std::make_unique<storage::UndoBuffer>(storage::MemoryManager::Get(clientContext));
    currentTS = common::Timestamp::getCurrentTimestamp().value;
    localWAL = std::make_unique<storage::LocalWAL>(*storage::MemoryManager::Get(clientContext),
        clientContext.getDBConfig()->enableChecksums);
}

Transaction::Transaction(TransactionType transactionType) noexcept
    : type{transactionType}, ID{DUMMY_TRANSACTION_ID}, startTS{DUMMY_START_TIMESTAMP},
      commitTS{common::INVALID_TRANSACTION}, clientContext{nullptr}, undoBuffer{nullptr},
      forceCheckpoint{false}, hasCatalogChanges{false} {
    currentTS = common::Timestamp::getCurrentTimestamp().value;
}

Transaction::Transaction(TransactionType transactionType, common::transaction_t ID,
    common::transaction_t startTS) noexcept
    : type{transactionType}, ID{ID}, startTS{startTS}, commitTS{common::INVALID_TRANSACTION},
      clientContext{nullptr}, undoBuffer{nullptr}, forceCheckpoint{false},
      hasCatalogChanges{false} {
    currentTS = common::Timestamp::getCurrentTimestamp().value;
}

bool Transaction::shouldLogToWAL() const {
    return isWriteTransaction() && !clientContext->isInMemory();
}

bool Transaction::shouldForceCheckpoint() const {
    return !clientContext->isInMemory() && forceCheckpoint;
}

void Transaction::commit(storage::WAL* wal) {
    //localStorage->commit();
    //undoBuffer->commit(commitTS);
    {
        dcprof::Scope dcPhase{dcprof::Phase::LocalStorageCommit};
        localStorage->commit();
    }

    {
        dcprof::Scope dcPhase{dcprof::Phase::UndoCommit};
        undoBuffer->commit(commitTS);
    }

    if (shouldLogToWAL()) {
	dcprof::Scope dcWal{dcprof::Phase::CommitWal};

        KU_ASSERT(localWAL && wal);
        localWAL->logCommit();
        wal->logCommittedWAL(*localWAL, clientContext);
        localWAL->clear();
    }
    if (hasCatalogChanges) {
	dcprof::Scope dcCatalog{dcprof::Phase::CatalogPublish};

        Catalog::Get(*clientContext)->incrementVersion();
        hasCatalogChanges = false;
    }
}

void Transaction::rollback(storage::WAL*) {
    // Rolling back the local storage will free + evict all optimistically-allocated pages
    // Since the undo buffer may do some scanning (e.g. to delete inserted keys from the hash index)
    // this must be rolled back first
    undoBuffer->rollback(clientContext);
    localStorage->rollback();
    hasCatalogChanges = false;
}

bool Transaction::isUnCommitted(common::table_id_t tableID, common::offset_t nodeOffset) const {
    return localStorage && localStorage->getLocalTable(tableID) &&
           nodeOffset >= getMinUncommittedNodeOffset(tableID);
}

void Transaction::pushCreateDropCatalogEntry(CatalogSet& catalogSet, CatalogEntry& catalogEntry,
    bool isInternal, bool skipLoggingToWAL) {
    undoBuffer->createCatalogEntry(catalogSet, catalogEntry);
    hasCatalogChanges = true;
    if (!shouldLogToWAL() || skipLoggingToWAL) {
        return;
    }
    KU_ASSERT(localWAL);
    const auto newCatalogEntry = catalogEntry.getNext();
    switch (newCatalogEntry->getType()) {
    case CatalogEntryType::INDEX_ENTRY:
    case CatalogEntryType::NODE_TABLE_ENTRY:
    case CatalogEntryType::REL_GROUP_ENTRY: {
        if (catalogEntry.getType() == CatalogEntryType::DUMMY_ENTRY) {
            KU_ASSERT(catalogEntry.isDeleted());
            localWAL->logCreateCatalogEntryRecord(newCatalogEntry, isInternal);
        } else {
            throw common::RuntimeException("This shouldn't happen. Alter table is not supported.");
        }
    } break;
    case CatalogEntryType::SEQUENCE_ENTRY: {
        KU_ASSERT(
            catalogEntry.getType() == CatalogEntryType::DUMMY_ENTRY && catalogEntry.isDeleted());
        if (newCatalogEntry->hasParent()) {
            // We don't log SERIAL catalog entry creation as it is implicit
            return;
        }
        localWAL->logCreateCatalogEntryRecord(newCatalogEntry, isInternal);
    } break;
    case CatalogEntryType::SCALAR_MACRO_ENTRY:
    case CatalogEntryType::TYPE_ENTRY: {
        KU_ASSERT(
            catalogEntry.getType() == CatalogEntryType::DUMMY_ENTRY && catalogEntry.isDeleted());
        localWAL->logCreateCatalogEntryRecord(newCatalogEntry, isInternal);
    } break;
    case CatalogEntryType::DUMMY_ENTRY: {
        KU_ASSERT(newCatalogEntry->isDeleted());
        if (catalogEntry.hasParent()) {
            return;
        }
        switch (catalogEntry.getType()) {
        case CatalogEntryType::INDEX_ENTRY:
        case CatalogEntryType::SCALAR_MACRO_ENTRY:
        case CatalogEntryType::NODE_TABLE_ENTRY:
        case CatalogEntryType::REL_GROUP_ENTRY:
        case CatalogEntryType::SEQUENCE_ENTRY: {
            localWAL->logDropCatalogEntryRecord(catalogEntry.getOID(), catalogEntry.getType());
        } break;
        case CatalogEntryType::SCALAR_FUNCTION_ENTRY:
        case CatalogEntryType::TABLE_FUNCTION_ENTRY:
        case CatalogEntryType::STANDALONE_TABLE_FUNCTION_ENTRY: {
            // DO NOTHING. We don't persist function entries.
        } break;
        case CatalogEntryType::TYPE_ENTRY:
        default: {
            throw common::RuntimeException(
                common::stringFormat("Not supported catalog entry type {} yet.",
                    CatalogEntryTypeUtils::toString(catalogEntry.getType())));
        }
        }
    } break;
    case CatalogEntryType::SCALAR_FUNCTION_ENTRY:
    case CatalogEntryType::TABLE_FUNCTION_ENTRY:
    case CatalogEntryType::STANDALONE_TABLE_FUNCTION_ENTRY: {
        // DO NOTHING. We don't persist function entries.
    } break;
    default: {
        throw common::RuntimeException(
            common::stringFormat("Not supported catalog entry type {} yet.",
                CatalogEntryTypeUtils::toString(catalogEntry.getType())));
    }
    }
}

void Transaction::pushAlterCatalogEntry(CatalogSet& catalogSet, CatalogEntry& catalogEntry,
    const binder::BoundAlterInfo& alterInfo) {
    undoBuffer->createCatalogEntry(catalogSet, catalogEntry);
    hasCatalogChanges = true;
    if (shouldLogToWAL()) {
        KU_ASSERT(localWAL);
        localWAL->logAlterCatalogEntryRecord(&alterInfo);
    }
}

void Transaction::pushSequenceChange(SequenceCatalogEntry* sequenceEntry, int64_t kCount,
    const SequenceRollbackData& data) {
    undoBuffer->createSequenceChange(*sequenceEntry, data);
    hasCatalogChanges = true;
    if (shouldLogToWAL()) {
        KU_ASSERT(localWAL);
        localWAL->logUpdateSequenceRecord(sequenceEntry->getOID(), kCount);
    }
}

void Transaction::pushInsertInfo(common::node_group_idx_t nodeGroupIdx, common::row_idx_t startRow,
    common::row_idx_t numRows, const storage::VersionRecordHandler* versionRecordHandler) const {
    undoBuffer->createInsertInfo(nodeGroupIdx, startRow, numRows, versionRecordHandler);
}

void Transaction::pushDeleteInfo(common::node_group_idx_t nodeGroupIdx, common::row_idx_t startRow,
    common::row_idx_t numRows, const storage::VersionRecordHandler* versionRecordHandler) const {
    undoBuffer->createDeleteInfo(nodeGroupIdx, startRow, numRows, versionRecordHandler);
}

void Transaction::pushVectorUpdateInfo(storage::UpdateInfo& updateInfo,
    const common::idx_t vectorIdx, storage::VectorUpdateInfo& vectorUpdateInfo,
    common::transaction_t version) const {
    undoBuffer->createVectorUpdateInfo(&updateInfo, vectorIdx, &vectorUpdateInfo, version);
}

Transaction::~Transaction() = default;

common::offset_t Transaction::getMinUncommittedNodeOffset(common::table_id_t tableID) const {
    if (localStorage && localStorage->getLocalTable(tableID)) {
        return localStorage->getLocalTable(tableID)
            ->cast<storage::LocalNodeTable>()
            .getStartOffset();
    }
    return 0;
}

Transaction* Transaction::Get(const main::ClientContext& context) {
    return TransactionContext::Get(context)->getActiveTransaction();
}

Transaction DUMMY_TRANSACTION = Transaction(TransactionType::DUMMY);
Transaction DUMMY_CHECKPOINT_TRANSACTION = Transaction(TransactionType::CHECKPOINT,
    Transaction::DUMMY_TRANSACTION_ID, Transaction::START_TRANSACTION_ID - 1);

} // namespace transaction
} // namespace kuzu

// DC_RUNTIME_PROFILE_IMPLEMENTATION:
// One shared core state for the core and extension.
namespace kuzu::common::dc_profile {
namespace {

thread_local Record* activeRecord = nullptr;
thread_local std::vector<Record> completedRecords;

std::atomic<uint64_t> profileSequence{0};
std::mutex profileOutputMutex;

void printRecord(const Record& r) noexcept {
    char line[16384];
    size_t used = 0;
    bool valid = true;

    const auto append = [&](const char* format, auto... values) {
        if (!valid) {
            return;
        }

        const auto n =
            std::snprintf(line + used, sizeof(line) - used, format, values...);

        if (n < 0 || static_cast<size_t>(n) >= sizeof(line) - used) {
            valid = false;
            return;
        }

        used += static_cast<size_t>(n);
    };

    append(
        "DC_UPDATE_PROFILE event_id=%llu parent_id=%llu "
        "tx=%llu has_tx=%d table=%llu has_table=%d "
        "event=%s method=%s u=%lld v=%lld kind=%s ok=%d "
        "total_ns=%llu self_ns=%llu child_event_ns=%llu",
        static_cast<unsigned long long>(r.id),
        static_cast<unsigned long long>(r.parentID),
        static_cast<unsigned long long>(r.tx),
        r.hasTx ? 1 : 0,
        static_cast<unsigned long long>(r.table),
        r.hasTable ? 1 : 0,
        r.event,
        r.method,
        static_cast<long long>(r.u),
        static_cast<long long>(r.v),
        r.kind,
        r.ok ? 1 : 0,
        static_cast<unsigned long long>(r.totalNs),
        static_cast<unsigned long long>(r.selfNs),
        static_cast<unsigned long long>(r.childEventNs));

    for (uint32_t i = 0; i < phaseCount; ++i) {
        append(
            " %s_ns=%llu %s_self_ns=%llu %s_calls=%llu",
            phaseNames[i],
            static_cast<unsigned long long>(r.phases[i].ns),
            phaseNames[i],
            static_cast<unsigned long long>(r.phases[i].selfNs),
            phaseNames[i],
            static_cast<unsigned long long>(r.phases[i].calls));
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(Counter::Count); ++i) {
        append(
            " %s=%llu",
            counterNames[i],
            static_cast<unsigned long long>(r.counters[i]));
    }

    if (valid) {
        std::fprintf(stderr, "%s\n", line);
    } else {
        std::fputs("DC_UPDATE_PROFILE_ERROR line_overflow\n", stderr);
    }
}

} // namespace

bool enabled() noexcept {
    static const bool value = [] {
        const auto* setting = std::getenv("DC_PROFILE_UPDATES");
        return setting && setting[0] == '1' && setting[1] == '\0';
    }();

    return value;
}

Record*& current() noexcept {
    return activeRecord;
}

uint64_t nextID() noexcept {
    return profileSequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

void finish(const Record& record, bool outermost) noexcept {
    try {
        completedRecords.push_back(record);

        if (outermost) {
            std::lock_guard<std::mutex> lock{profileOutputMutex};

            for (const auto& item : completedRecords) {
                printRecord(item);
            }

            completedRecords.clear();
        }
    } catch (...) {
        completedRecords.clear();
        std::fputs(
            "DC_UPDATE_PROFILE_ERROR telemetry_allocation_failed\n",
            stderr);
    }
}

} // namespace kuzu::common::dc_profile
