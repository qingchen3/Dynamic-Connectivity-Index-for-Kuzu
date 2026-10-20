#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>

#include "common/api.h"

// Diagnostic instrumentation only. No versioning/publication semantics change.
namespace kuzu::common::dc_profile {

enum class Phase : uint32_t {
    Classify, NodeLookup, RootWalk, Reroot, Link, Cut, NonTreePath, NteStorage,
    Search, ReplacementInstall, Candidate, Provider, GraphSetup, ScanPrepare, ScanFwd, ScanBwd,
    RelDedup, NeighborDedup, BaseMutation, DeleteStage, LocalWal, IndexDiscover,
    DeleteDispatch, InsertDispatch, RelStorageCommit, IndexCallback, LocalStorageCommit,
    UndoCommit, CommitWal, CatalogPublish, CommitLockWait, ClientCleanup, Checkpoint,
    PrepareSchema, PrepareNodeVectors, PrepareProperties, PreparePredicate,
    PrepareDirection, PrepareStorageState, PrepareBind, ScanInit,
    Count
};
inline constexpr const char* phaseNames[] = {
    "classify", "node_lookup", "root_walk", "reroot", "link", "cut", "non_tree_path", "nte_storage",
    "search", "replacement_install", "candidate", "provider", "graph_setup", "scan_prepare", "scan_fwd", "scan_bwd",
    "rel_dedup", "neighbor_dedup", "base_mutation", "delete_stage", "local_wal", "index_discover",
    "delete_dispatch", "insert_dispatch", "rel_storage_commit", "index_callback", "local_storage_commit",
    "undo_commit", "commit_wal", "catalog_publish", "commit_lock_wait", "client_cleanup", "checkpoint",
    "prepare_schema", "prepare_node_vectors", "prepare_properties", "prepare_predicate",
    "prepare_direction", "prepare_storage_state", "prepare_bind", "scan_init"
};
inline constexpr uint32_t phaseCount = static_cast<uint32_t>(Phase::Count);
static_assert(sizeof(phaseNames) / sizeof(*phaseNames) == phaseCount);
enum class Counter : uint32_t { RootSteps, SearchVertices, Candidates, NeighborIds, Searches, ReplacementFound, Count };
inline constexpr const char* counterNames[] = {"root_steps", "search_vertices", "candidates", "neighbor_ids", "searches", "replacement_found"};
struct Metric { uint64_t ns = 0, selfNs = 0, calls = 0; };
class Scope;
struct Record {
    uint64_t id = 0, parentID = 0, tx = 0, table = 0;
    int64_t u = -1, v = -1;
    const char* event = "-";
    const char* method = "-";
    const char* kind = "-";
    bool hasTx = false, hasTable = false, ok = true;
    uint64_t totalNs = 0, selfNs = 0, rootScopeNs = 0, childOutsideScopeNs = 0, childEventNs = 0;
    Scope* top = nullptr;
    std::array<Metric, phaseCount> phases{};
    std::array<uint64_t, static_cast<uint32_t>(Counter::Count)> counters{};
};
// Defined ONCE in core (transaction.cpp), not separate TLS in the extension DSO.
KUZU_API bool enabled() noexcept;
KUZU_API Record*& current() noexcept;
KUZU_API uint64_t nextID() noexcept;
KUZU_API void finish(const Record& record, bool outermost) noexcept;

using Clock = std::chrono::steady_clock;
inline uint64_t elapsedNs(Clock::time_point start) noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

class Scope {
public:
    explicit Scope(Phase phase) noexcept : record{current()}, phase{phase} {
        if (record) { parent = record->top; record->top = this; start = Clock::now(); }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { stop(); }
    void stop() noexcept {
        if (!record) return;
        const auto ns = elapsedNs(start);
        auto& metric = record->phases[static_cast<uint32_t>(phase)];
        metric.ns += ns;
        metric.selfNs += ns >= childNs ? ns - childNs : 0;
        ++metric.calls;
        record->top = parent;
        if (parent) parent->childNs += ns;
        else record->rootScopeNs += ns;
        record = nullptr;
    }
    void addChild(uint64_t ns) noexcept { childNs += ns; }
private:
    Record* record = nullptr;
    Scope* parent = nullptr;
    Phase phase;
    Clock::time_point start{};
    uint64_t childNs = 0;
};

class Event {
public:
    Event(const char* event, const char* method = "-", int64_t u = -1, int64_t v = -1,
        uint64_t tx = 0, bool hasTx = false, uint64_t table = 0, bool hasTable = false,
        bool selected = true) noexcept : active{selected && enabled()} {
        if (!active) return;
        previous = current();
        record.id = nextID();
        record.event = event; record.method = method; record.u = u; record.v = v;
        record.tx = hasTx ? tx : (previous ? previous->tx : 0);
        record.hasTx = hasTx || (previous && previous->hasTx);
        record.table = hasTable ? table : (previous ? previous->table : 0);
        record.hasTable = hasTable || (previous && previous->hasTable);
        record.parentID = previous ? previous->id : 0;
        exceptionDepth = std::uncaught_exceptions();
        current() = &record;
        start = Clock::now();
    }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    ~Event() noexcept {
        if (!active) return;
        record.totalNs = elapsedNs(start);
        const auto covered = record.rootScopeNs + record.childOutsideScopeNs;
        record.selfNs = record.totalNs >= covered ? record.totalNs - covered : 0;
        record.ok = std::uncaught_exceptions() == exceptionDepth;
        current() = previous;
        if (previous) {
            previous->childEventNs += record.totalNs;
            if (previous->top) previous->top->addChild(record.totalNs);
            else previous->childOutsideScopeNs += record.totalNs;
        }
        finish(record, previous == nullptr);
    }
private:
    bool active = false;
    Record record{};
    Record* previous = nullptr;
    Clock::time_point start{};
    int exceptionDepth = 0;
};
inline void kind(const char* value) noexcept { if (auto* r = current()) r->kind = value; }
inline void count(Counter key, uint64_t n = 1) noexcept {
    if (auto* r = current()) r->counters[static_cast<uint32_t>(key)] += n;
}
} // namespace kuzu::common::dc_profile

