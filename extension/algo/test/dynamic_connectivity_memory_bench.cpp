// Measures the memory footprint of the four dynamic connectivity backends.
//
//   dynamic_connectivity_memory_bench <method> --dataset=FILE  [options]
//   dynamic_connectivity_memory_bench <method> --workload=FILE [options]
//   dynamic_connectivity_memory_bench <method> --random=NODES,OPS,BIAS
//
//   <method>      dtree | dtree_csr | stree | stree_csr
//   --dataset     the edges to load, in file order, with nothing ever deleted.
//                 This is the experiment the Python runs. Takes either a plain
//                 edge list, "102 108" per line, or a recorded trace, "ins 102
//                 108 1080493920", whose del lines are counted and skipped and
//                 whose timestamps are ignored.
//   --workload    the same trace with its deletions applied, so the footprint is
//                 of the residual graph rather than of the fully loaded one.
//   --testcase    name for the RESULT line; defaults to the input's file name
//   --edges       stop after N edges have been inserted (0 means all of them)
//   --dedup       filter repeated edges instead of letting the backend refuse
//                 them; costs a set of every edge seen, so it is off by default


#include "common/dynamic_connectivity_index_factory.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace memtrack {

// Every allocation carries a header holding its requested size and the pointer
// malloc actually returned, so the aligned and unaligned paths free identically.
struct Header {
    size_t size;
    void* base;
};
constexpr size_t HEADER = sizeof(Header) < 32 ? 32 : sizeof(Header);

// Sizes above this land in the overflow bucket; node and tree-node allocations
// are far below it.
constexpr size_t MAX_TRACKED_SIZE = 256;

struct Counters {
    size_t liveBytes = 0;
    size_t liveCount = 0;
    size_t totalBytes = 0;
    std::array<size_t, MAX_TRACKED_SIZE + 1> liveBySize{};
    size_t liveLargeBytes = 0;
};

// Plain globals: this benchmark is single threaded by construction, and an
// atomic on every allocation would distort what it is trying to measure.
Counters counters;
bool enabled = false;

void* allocate(size_t bytes, size_t alignment) {
    const size_t slack = alignment > HEADER ? alignment : 0;
    void* base = std::malloc(bytes + HEADER + slack);
    if (base == nullptr) {
        return nullptr;
    }
    auto address = reinterpret_cast<uintptr_t>(base) + HEADER;
    if (alignment > 1) {
        address = (address + alignment - 1) & ~static_cast<uintptr_t>(alignment - 1);
    }
    auto* user = reinterpret_cast<void*>(address);
    auto* header = reinterpret_cast<Header*>(address - sizeof(Header));
    header->size = bytes;
    header->base = base;
    if (enabled) {
        counters.liveBytes += bytes;
        counters.totalBytes += bytes;
        ++counters.liveCount;
        if (bytes <= MAX_TRACKED_SIZE) {
            counters.liveBySize[bytes] += bytes;
        } else {
            counters.liveLargeBytes += bytes;
        }
    }
    return user;
}

void release(void* user) {
    if (user == nullptr) {
        return;
    }
    auto* header = reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(user) - sizeof(Header));
    const size_t bytes = header->size;
    void* base = header->base;
    if (enabled) {
        counters.liveBytes -= bytes;
        --counters.liveCount;
        if (bytes <= MAX_TRACKED_SIZE) {
            counters.liveBySize[bytes] -= bytes;
        } else {
            counters.liveLargeBytes -= bytes;
        }
    }
    std::free(base);
}

// A snapshot of what is live, so two of them can be subtracted.
struct Snapshot {
    size_t bytes = 0;
    size_t count = 0;
    std::array<size_t, MAX_TRACKED_SIZE + 1> bySize{};
    size_t largeBytes = 0;
};

Snapshot snapshot() {
    Snapshot s;
    s.bytes = counters.liveBytes;
    s.count = counters.liveCount;
    s.bySize = counters.liveBySize;
    s.largeBytes = counters.liveLargeBytes;
    return s;
}

} // namespace memtrack

void* operator new(size_t n) {
    void* p = memtrack::allocate(n, 0);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new[](size_t n) {
    return ::operator new(n);
}
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    return memtrack::allocate(n, 0);
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
    return memtrack::allocate(n, 0);
}
void* operator new(size_t n, std::align_val_t a) {
    void* p = memtrack::allocate(n, static_cast<size_t>(a));
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new[](size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}
void operator delete(void* p) noexcept {
    memtrack::release(p);
}
void operator delete[](void* p) noexcept {
    memtrack::release(p);
}
void operator delete(void* p, size_t) noexcept {
    memtrack::release(p);
}
void operator delete[](void* p, size_t) noexcept {
    memtrack::release(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    memtrack::release(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    memtrack::release(p);
}
void operator delete(void* p, size_t, std::align_val_t) noexcept {
    memtrack::release(p);
}
void operator delete[](void* p, size_t, std::align_val_t) noexcept {
    memtrack::release(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    memtrack::release(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    memtrack::release(p);
}

// ---------------------------------------------------------------------------
// Benchmark
// ---------------------------------------------------------------------------
namespace {

using kuzu::algo_extension::DynamicConnectivityIndex;
using kuzu::algo_extension::IndexMemoryFootprint;
using Edge = std::pair<int64_t, int64_t>;

Edge makeEdge(int64_t u, int64_t v) {
    return {std::min(u, v), std::max(u, v)};
}

// What the reader had to leave out, so a run can say so rather than quietly
// measuring a different graph from the one in the file.
struct InputStats {
    uint64_t lines = 0;
    uint64_t malformed = 0;
    uint64_t selfLoops = 0;
    uint64_t duplicates = 0;
    uint64_t absent = 0;
    uint64_t queryMarkers = 0;
    // Deletions a dataset run passed over, since it loads a graph rather than
    // replaying a trace. Reported rather than left silent: it is the difference
    // between this run and a --workload run on the same file.
    uint64_t ignoredDeletions = 0;
    bool truncated = false;
};

[[noreturn]] void usage(const char* argv0, const std::string& why) {
    std::cerr << "error: " << why << "\n\n"
              << "usage: " << argv0 << " <dtree|dtree_csr|stree|stree_csr>"
              << " (--dataset=FILE | --workload=FILE | --random=NODES,OPS,BIAS)\n"
              << "       [--testcase=NAME] [--edges=N] [--seed=N] [--dedup]\n";
    std::exit(2);
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::ifstream openOrDie(const std::string& path) {
    std::ifstream in{path};
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(2);
    }
    return in;
}

enum class LineKind { Edge, Deletion, Marker, Blank, Malformed };

// One line of a graph to load. Accepts the plain edge list
//
//     102 108
//
// and the recorded trace form, with or without its timestamp
//
//     ins 102 108 1080493920
//
// so the same file can be loaded here or replayed with its deletions by
// --workload. A del line is not an edge to load, so it is reported and skipped;
// the timestamp is ignored either way, since the order to replay in is the file's.
LineKind parseEdgeLine(const std::string& line, int64_t& u, int64_t& v) {
    std::istringstream fields{line};
    std::string first;
    if (!(fields >> first)) {
        return LineKind::Blank;
    }
    if (first == "del") {
        return LineKind::Deletion;
    }
    if (first == "query") {
        return LineKind::Marker;
    }
    if (first == "ins") {
        return (fields >> u >> v) ? LineKind::Edge : LineKind::Malformed;
    }
    // No keyword, so the first token is already an endpoint. Parsed strictly:
    // stoll would otherwise accept "12abc" as 12 and read a neighbouring column
    // as a vertex.
    try {
        size_t consumed = 0;
        u = std::stoll(first, &consumed);
        if (consumed != first.size()) {
            return LineKind::Malformed;
        }
    } catch (const std::exception&) {
        return LineKind::Malformed;
    }
    return (fields >> v) ? LineKind::Edge : LineKind::Malformed;
}

// Every input is applied to the index as it is read, never collected first.
// Reading a 10M-edge dataset into a vector of operations would cost a few
// hundred MB, and tracking the edges seen so far a few hundred more, all of it
// scratch memory competing with the index for RAM on exactly the inputs where
// memory is the constraint. Streaming is also what the Python does.
struct Replay {
    DynamicConnectivityIndex& index;
    InputStats stats;
    uint64_t applied = 0;

    // Only maintained where a deletion can happen, since it exists solely to
    // answer the CSR backends' neighbour provider, which only deletion calls.
    // An insert-only dataset leaves it empty, which on a large graph saves more
    // than the index itself occupies.
    bool trackAdjacency = false;
    std::unordered_map<int64_t, std::set<int64_t>> adjacency;

    std::vector<int64_t> neighbors(int64_t x) const {
        std::vector<int64_t> out;
        auto it = adjacency.find(x);
        if (it != adjacency.end()) {
            out.assign(it->second.begin(), it->second.end());
        }
        return out;
    }

    void insert(int64_t u, int64_t v) {
        if (trackAdjacency) {
            adjacency[u].insert(v);
            adjacency[v].insert(u);
        }
        index.insertEdge(u, v);
        ++applied;
    }

    void remove(int64_t u, int64_t v) {
        adjacency[u].erase(v);
        adjacency[v].erase(u);
        index.deleteEdge(u, v, [this](int64_t x) { return neighbors(x); });
        ++applied;
    }
};

// An edge list, as evaluate_memory_footprints.py reads datasets/: every line an
// insertion, in file order, with nothing ever deleted. Splits on whitespace
// rather than on a per-dataset delimiter, which covers the space-separated and
// the tab-separated files without having to know which is which.
//
// Repeated edges are passed to the backend rather than filtered, as the Python
// passes them: every backend already refuses an edge it holds, DTree in
// insert_edge and insert_nte, STree in insertEdge. Filtering them here would
// mean keeping every edge seen so far in a set, which is the single largest
// avoidable allocation in this program. --dedup asks for that set anyway, for
// inputs small enough not to care and where knowing the duplicate count matters.
void streamDataset(const std::string& path, uint64_t maxEdges, bool dedup, Replay& replay) {
    auto in = openOrDie(path);
    std::set<Edge> seen;
    std::string line;
    while (std::getline(in, line)) {
        ++replay.stats.lines;
        int64_t u = 0;
        int64_t v = 0;
        const auto kind = parseEdgeLine(line, u, v);
        if (kind == LineKind::Blank) {
            continue;
        }
        if (kind == LineKind::Deletion) {
            ++replay.stats.ignoredDeletions;
            continue;
        }
        if (kind == LineKind::Marker) {
            ++replay.stats.queryMarkers;
            continue;
        }
        if (kind == LineKind::Malformed) {
            ++replay.stats.malformed;
            continue;
        }
        if (u == v) {
            ++replay.stats.selfLoops; // as the Python: `if items[0] == items[1]: continue`
            continue;
        }
        if (dedup && !seen.insert(makeEdge(u, v)).second) {
            ++replay.stats.duplicates;
            continue;
        }
        replay.insert(u, v);
        if (maxEdges != 0 && replay.applied >= maxEdges) {
            replay.stats.truncated = true;
            break;
        }
    }
}

// An ins/del trace. The footprint is then of whatever survives the deletions,
// which is a different quantity from the dataset mode's fully loaded graph. The
// live set is needed here whatever the input size, since a deletion of an edge
// the index does not hold is not a thing the backends can be asked to do; it is
// bounded by the edges alive at once rather than by the length of the trace.
void streamWorkload(const std::string& path, uint64_t maxEdges, Replay& replay) {
    auto in = openOrDie(path);
    std::set<Edge> live;
    uint64_t inserted = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++replay.stats.lines;
        std::istringstream fields{line};
        std::string kind;
        int64_t u = 0;
        int64_t v = 0;
        if (!(fields >> kind >> u >> v) || (kind != "ins" && kind != "del")) {
            if (kind == "query") {
                ++replay.stats.queryMarkers;
                continue;
            }
            ++replay.stats.malformed;
            continue;
        }
        if (u == v) {
            ++replay.stats.selfLoops;
            continue;
        }
        const auto edge = makeEdge(u, v);
        if (kind == "ins") {
            if (!live.insert(edge).second) {
                ++replay.stats.duplicates;
                continue;
            }
            replay.insert(u, v);
            if (maxEdges != 0 && ++inserted >= maxEdges) {
                replay.stats.truncated = true;
                break;
            }
        } else {
            if (live.erase(edge) == 0) {
                ++replay.stats.absent;
                continue;
            }
            replay.remove(u, v);
        }
    }
}

void streamRandom(int64_t numNodes, int numOps, double bias, uint32_t seed, Replay& replay) {
    std::set<Edge> edges;
    std::mt19937 rng{seed};
    std::uniform_int_distribution<int64_t> pickNode{0, numNodes - 1};
    std::uniform_real_distribution<double> pickAction{0.0, 1.0};
    for (int i = 0; i < numOps; ++i) {
        ++replay.stats.lines;
        if (edges.empty() || pickAction(rng) < bias) {
            auto u = pickNode(rng);
            auto v = pickNode(rng);
            if (u == v) {
                v = (v + 1) % numNodes;
            }
            if (!edges.insert(makeEdge(u, v)).second) {
                ++replay.stats.duplicates;
                continue;
            }
            replay.insert(u, v);
        } else {
            auto it = edges.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(rng() % edges.size()));
            const auto [u, v] = *it;
            edges.erase(it);
            replay.remove(u, v);
        }
    }
}

double toGB(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0], "need a method and an input");
    }
    const std::string method = argv[1];
    std::string dataset;
    std::string workload;
    std::string testcase;
    int64_t randomNodes = 0;
    int randomOps = 0;
    double randomBias = 0.5;
    uint64_t maxEdges = 0;
    uint32_t seed = 20260917;
    bool dedup = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dedup") {
            dedup = true;
        } else if (startsWith(arg, "--dataset=")) {
            dataset = arg.substr(10);
        } else if (startsWith(arg, "--workload=")) {
            workload = arg.substr(11);
        } else if (startsWith(arg, "--testcase=")) {
            testcase = arg.substr(11);
        } else if (startsWith(arg, "--edges=")) {
            maxEdges = std::strtoull(arg.c_str() + 8, nullptr, 10);
        } else if (startsWith(arg, "--random=")) {
            long long nodes = 0;
            if (std::sscanf(arg.c_str() + 9, "%lld,%d,%lf", &nodes, &randomOps, &randomBias) != 3 ||
                nodes < 2 || randomOps < 1) {
                usage(argv[0], "--random wants NODES,OPS,BIAS with NODES >= 2");
            }
            randomNodes = static_cast<int64_t>(nodes);
        } else if (startsWith(arg, "--seed=")) {
            seed = static_cast<uint32_t>(std::strtoul(arg.c_str() + 7, nullptr, 10));
        } else {
            usage(argv[0], "unknown option " + arg);
        }
    }

    const int sources = (dataset.empty() ? 0 : 1) + (workload.empty() ? 0 : 1) +
                        (randomNodes == 0 ? 0 : 1);
    if (sources != 1) {
        usage(argv[0], "give exactly one of --dataset, --workload, --random");
    }

    const std::string mode = !dataset.empty()  ? "dataset" :
                             !workload.empty() ? "workload" :
                                                 "random";
    if (testcase.empty()) {
        testcase = !dataset.empty()  ? baseName(dataset) :
                   !workload.empty() ? baseName(workload) :
                                       "random";
    }

    memtrack::enabled = true;

    std::unique_ptr<DynamicConnectivityIndex> index;
    try {
        index = kuzu::algo_extension::createDynamicConnectivityIndex(method);
    } catch (const std::exception& e) {
        memtrack::enabled = false;
        usage(argv[0], e.what());
    }

    // Warm the measurement containers on the empty index. They allocate and free
    // a small container, so doing it before the snapshot keeps that traffic out
    // of the measured difference entirely.
    (void)index->memoryFootprint();

    // Read each edge and insert it as it is read; measure once the input is
    // exhausted. The adjacency the CSR backends' provider reads is needed only
    // where something gets deleted, so a dataset run does not build it.
    Replay replay{*index};
    replay.trackAdjacency = mode != "dataset";
    if (mode == "dataset") {
        streamDataset(dataset, maxEdges, dedup, replay);
    } else if (mode == "workload") {
        streamWorkload(workload, maxEdges, replay);
    } else {
        streamRandom(randomNodes, randomOps, randomBias, seed, replay);
    }
    const auto stats = replay.stats;
    const uint64_t applied = replay.applied;
    if (applied == 0) {
        memtrack::enabled = false;
        std::cerr << "error: no operations to replay\n";
        return 2;
    }

    // Both measurements, taken over the same instant.
    const auto withIndex = memtrack::snapshot();
    const auto walk = index->memoryFootprint();
    index.reset();
    const auto withoutIndex = memtrack::snapshot();
    memtrack::enabled = false;

    const uint64_t allocBytes = withIndex.bytes - withoutIndex.bytes;
    const uint64_t allocations = withIndex.count - withoutIndex.count;
    const int64_t delta = static_cast<int64_t>(walk.total()) - static_cast<int64_t>(allocBytes);

    std::printf("method=%s testcase=%s mode=%s\n", method.c_str(), testcase.c_str(),
        mode.c_str());
    std::printf("input:     %llu lines, %llu %s%s\n",
        static_cast<unsigned long long>(stats.lines),
        static_cast<unsigned long long>(applied),
        // Only a dataset run is purely insertions; the others delete too.
        mode == "dataset" ? "edges inserted as read" : "updates applied as read",
        mode == "dataset" ? (dedup ? ", repeats filtered" :
                                     ", repeats passed to the backend") :
                            "");
    if (stats.malformed || stats.selfLoops || stats.duplicates || stats.absent) {
        std::printf("skipped:   %llu malformed, %llu self-loops, %llu duplicate edges, "
                    "%llu absent deletions\n",
            static_cast<unsigned long long>(stats.malformed),
            static_cast<unsigned long long>(stats.selfLoops),
            static_cast<unsigned long long>(stats.duplicates),
            static_cast<unsigned long long>(stats.absent));
    }
    if (stats.ignoredDeletions > 0) {
        // Not a defect: it is what loading a graph from a trace means. Said out
        // loud because the same file under --workload measures a smaller graph.
        std::printf("ignored:   %llu del lines -- this is the loaded graph, not the "
                    "residual one\n",
            static_cast<unsigned long long>(stats.ignoredDeletions));
    }
    if (stats.queryMarkers > 0) {
        std::printf("ignored:   %llu query markers (no graph mutation)\n",
            static_cast<unsigned long long>(stats.queryMarkers));
    }
    if (stats.truncated) {
        std::printf("           stopped early at the --edges limit\n");
    }
    std::printf("graph:     %llu vertices, %llu tree edges, %llu non-tree edges\n",
        static_cast<unsigned long long>(walk.numNodes),
        static_cast<unsigned long long>(walk.numTreeEdges),
        static_cast<unsigned long long>(walk.numNonTreeEdges));
    std::printf("space_n:   %llu bytes (%.6f GB)\n",
        static_cast<unsigned long long>(walk.bytesNodes), toGB(walk.bytesNodes));
    std::printf("space_e:   %llu bytes (%.6f GB)\n",
        static_cast<unsigned long long>(walk.bytesEdges), toGB(walk.bytesEdges));
    std::printf("total:     %llu bytes (%.6f GB)\n",
        static_cast<unsigned long long>(walk.total()), toGB(walk.total()));
    if (walk.numNodes > 0) {
        std::printf("per node:  %.2f bytes\n",
            static_cast<double>(walk.total()) / static_cast<double>(walk.numNodes));
    }

    // The independent figure, and the gap between the two.
    std::printf("\nallocator: %llu bytes in %llu live allocations\n",
        static_cast<unsigned long long>(allocBytes),
        static_cast<unsigned long long>(allocations));
    if (delta == 0) {
        std::printf("           walk agrees to the byte\n");
    } else {
        std::printf("           WALK DISAGREES by %+lld bytes (%.4f%%): the walk is missing an\n"
                    "           allocation, so space_n + space_e understates the footprint\n",
            static_cast<long long>(delta),
            100.0 * static_cast<double>(delta) / static_cast<double>(allocBytes));
    }

    // Allocation sizes, for reading off which structure the bytes went into:
    // node objects and red-black tree nodes land in distinct size classes.
    std::printf("\nby allocation size (bytes held, count):\n");
    uint64_t accounted = 0;
    for (size_t size = 1; size <= memtrack::MAX_TRACKED_SIZE; ++size) {
        const size_t held = withIndex.bySize[size] - withoutIndex.bySize[size];
        if (held == 0) {
            continue;
        }
        accounted += held;
        std::printf("  %3zu B x %-10zu = %10zu B  (%5.1f%%)\n", size, held / size, held,
            100.0 * static_cast<double>(held) / static_cast<double>(allocBytes));
    }
    const size_t large = withIndex.largeBytes - withoutIndex.largeBytes;
    if (large > 0) {
        std::printf("  >%zu B%*s = %10zu B  (%5.1f%%)\n", memtrack::MAX_TRACKED_SIZE, 16, "",
            large, 100.0 * static_cast<double>(large) / static_cast<double>(allocBytes));
        accounted += large;
    }
    if (accounted != allocBytes) {
        std::printf("  (unattributed: %llu B)\n",
            static_cast<unsigned long long>(allocBytes - accounted));
    }

    std::printf("\nRESULT method=%s testcase=%s mode=%s dedup=%d nodes=%llu tree_edges=%llu "
                "nte_edges=%llu space_n=%llu space_e=%llu bytes=%llu gb=%.9f "
                "space_n_gb=%.9f space_e_gb=%.9f bytes_per_node=%.2f alloc_bytes=%llu "
                "delta_bytes=%lld allocations=%llu ops=%llu ignored_deletions=%llu\n",
        method.c_str(), testcase.c_str(), mode.c_str(), dedup ? 1 : 0,
        static_cast<unsigned long long>(walk.numNodes),
        static_cast<unsigned long long>(walk.numTreeEdges),
        static_cast<unsigned long long>(walk.numNonTreeEdges),
        static_cast<unsigned long long>(walk.bytesNodes),
        static_cast<unsigned long long>(walk.bytesEdges),
        static_cast<unsigned long long>(walk.total()), toGB(walk.total()),
        toGB(walk.bytesNodes), toGB(walk.bytesEdges),
        walk.numNodes == 0 ? 0.0 :
                             static_cast<double>(walk.total()) /
                                 static_cast<double>(walk.numNodes),
        static_cast<unsigned long long>(allocBytes), static_cast<long long>(delta),
        static_cast<unsigned long long>(allocations),
        static_cast<unsigned long long>(applied),
        static_cast<unsigned long long>(stats.ignoredDeletions));

    // A walk that has missed an allocation is a defect in the accounting, not a
    // property of the graph, so it fails the run rather than being reported.
    return delta == 0 ? 0 : 1;
}
