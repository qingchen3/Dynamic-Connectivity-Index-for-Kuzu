// Replays an Imp_bench_dyn workload against the Kuzu connectivity backends.
//
// Bridges qingchen3/Imp_bench_dyn's evaluate_update_performance.py 
// https://github.com/qingchen3/Imp_bench_dyn/blob/main/evaluate_update_performance.py 
// to the C++ backends: same workload files, same metric (cumulative time / operation
// count), so the numbers line up with the Python results per graph and ratio.
//
//   dynamic_connectivity_workload_bench <method> <workload> [options]
//
//   <method>    dtree | dtree_csr | stree | stree_csr
//   <workload>  a file of "ins a b" / "del a b" lines, as produced for
//               workloads/<graph>_ratio_<r>; lines with a == b are skipped,
//               matching the Python driver
//
//   --check=none            time only (default; use this for reported numbers)
//   --check=sampled:K       after each update verify K random pairs
//   --check=full            after each update verify every pair (small graphs)
//   --seed=N                seed for sampling (default 20260917)
//   --limit=N               stop after N operations
//   --label=SG --ratio=1000 echoed into the RESULT line for the CSV shim
//
// SCOPE. This measures the data structures standalone, exactly as the Python
// benchmark does -- neighbours come from an in-memory adjacency map, not from
// Kuzu's CSR. It is the apples-to-apples comparison against the published
// numbers, not a measurement of the integrated index. Driving the real storage
// costs a graph and scan-state construction per provider call, which this does
// not model; the provider counters below are what that work would replace.
//
// CORRECTNESS. The Python driver checks every method against Dtree's
// query_simple, so Dtree itself is never checked against anything independent.
// Here the oracle is a union-find rebuilt from the edge set, which shares no
// code with any backend. Checking is excluded from the timings.

#include "common/dynamic_connectivity_index_factory.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <exception>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using kuzu::algo_extension::DynamicConnectivityIndex;
using Clock = std::chrono::steady_clock;
using Edge = std::pair<int64_t, int64_t>;

Edge makeEdge(int64_t u, int64_t v) {
    return {std::min(u, v), std::max(u, v)};
}

// Independent oracle. Shares no code with any backend: union by size with path
// compression, rebuilt from the live edge set whenever an edge is removed.
class UnionFind {
public:
    void reset(size_t n) {
        parent.resize(n);
        size.assign(n, 1);
        std::iota(parent.begin(), parent.end(), size_t{0});
    }
    size_t find(size_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(size_t a, size_t b) {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        if (size[a] < size[b]) {
            std::swap(a, b);
        }
        parent[b] = a;
        size[a] += size[b];
    }

private:
    std::vector<size_t> parent;
    std::vector<size_t> size;
};

enum class CheckMode { None, Sampled, Full };

struct Options {
    std::string method;
    std::string workload;
    CheckMode check = CheckMode::None;
    int samplesPerStep = 0;
    uint32_t seed = 20260917;
    uint64_t limit = 0; // 0 = no limit
    std::string label = "NA";
    std::string ratio = "NA";
};

[[noreturn]] void usage(const char* argv0, const std::string& why) {
    std::cerr << "error: " << why << "\n\n"
              << "usage: " << argv0 << " <dtree|dtree_csr|stree|stree_csr> <workload> [options]\n"
              << "  --check=none|sampled:K|full   default none\n"
              << "  --seed=N --limit=N --label=SG --ratio=1000\n";
    std::exit(2);
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

Options parseArgs(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0], "need a method and a workload file");
    }
    Options opt;
    opt.method = argv[1];
    opt.workload = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check=none") {
            opt.check = CheckMode::None;
        } else if (arg == "--check=full") {
            opt.check = CheckMode::Full;
        } else if (startsWith(arg, "--check=sampled:")) {
            opt.check = CheckMode::Sampled;
            opt.samplesPerStep = std::atoi(arg.c_str() + 16);
            if (opt.samplesPerStep <= 0) {
                usage(argv[0], "sampled check needs a positive count");
            }
        } else if (startsWith(arg, "--seed=")) {
            opt.seed = static_cast<uint32_t>(std::strtoul(arg.c_str() + 7, nullptr, 10));
        } else if (startsWith(arg, "--limit=")) {
            opt.limit = std::strtoull(arg.c_str() + 8, nullptr, 10);
        } else if (startsWith(arg, "--label=")) {
            opt.label = arg.substr(8);
        } else if (startsWith(arg, "--ratio=")) {
            opt.ratio = arg.substr(8);
        } else {
            usage(argv[0], "unknown option " + arg);
        }
    }
    return opt;
}

struct Op {
    bool isInsert;
    int64_t u;
    int64_t v;
};

// Reads the whole workload up front so file I/O never lands inside a timed
// region.
std::vector<Op> readWorkload(const std::string& path, uint64_t limit) {
    std::ifstream in{path};
    if (!in) {
        std::cerr << "error: cannot open workload " << path << "\n";
        std::exit(2);
    }
    std::vector<Op> ops;
    std::string line;
    uint64_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::istringstream fields{line};
        std::string kind;
        int64_t a = 0;
        int64_t b = 0;
        if (!(fields >> kind >> a >> b)) {
            continue; // blank or malformed; the Python driver is equally lenient
        }
        if (a == b) {
            continue; // matches evaluate_update_performance.py
        }
        if (kind == "ins") {
            ops.push_back({true, a, b});
        } else if (kind == "del") {
            ops.push_back({false, a, b});
        } else {
            std::cerr << "warning: ignoring line " << lineNo << ": " << line << "\n";
        }
        if (limit != 0 && ops.size() >= limit) {
            break;
        }
    }
    return ops;
}

struct Stats {
    uint64_t count = 0;
    double seconds = 0.0;
    double average() const { return count == 0 ? 0.0 : seconds / static_cast<double>(count); }
};

} // namespace

int main(int argc, char** argv) {
    const Options opt = parseArgs(argc, argv);

    std::unique_ptr<DynamicConnectivityIndex> index;
    try {
        index = kuzu::algo_extension::createDynamicConnectivityIndex(opt.method);
    } catch (const std::exception& e) {
        usage(argv[0], e.what());
    }

    const auto ops = readWorkload(opt.workload, opt.limit);
    if (ops.empty()) {
        std::cerr << "error: workload contained no usable operations\n";
        return 2;
    }

    // Dense indices for the oracle; the backends keep the original ids.
    std::unordered_map<int64_t, size_t> denseId;
    std::vector<int64_t> originalId;
    auto dense = [&](int64_t key) {
        auto [it, inserted] = denseId.try_emplace(key, originalId.size());
        if (inserted) {
            originalId.push_back(key);
        }
        return it->second;
    };
    for (const auto& op : ops) {
        dense(op.u);
        dense(op.v);
    }

    // Live edge set, plus the adjacency the CSR backends read through.
    std::set<Edge> edges;
    std::unordered_map<int64_t, std::unordered_set<int64_t>> adjacency;

    uint64_t providerCalls = 0;
    uint64_t providerNeighbors = 0;
    uint64_t providerMaxDegree = 0;
    auto provider = [&](int64_t x) {
        ++providerCalls;
        std::vector<int64_t> out;
        auto it = adjacency.find(x);
        if (it != adjacency.end()) {
            out.assign(it->second.begin(), it->second.end());
        }
        providerNeighbors += out.size();
        providerMaxDegree = std::max<uint64_t>(providerMaxDegree, out.size());
        return out;
    };

    UnionFind oracle;
    bool oracleStale = true;
    auto refreshOracle = [&]() {
        if (!oracleStale) {
            return;
        }
        oracle.reset(originalId.size());
        for (auto [a, b] : edges) {
            oracle.unite(denseId.at(a), denseId.at(b));
        }
        oracleStale = false;
    };

    Stats inserts;
    Stats deletes;
    uint64_t skipped = 0;
    uint64_t mismatches = 0;
    std::mt19937 rng{opt.seed};

    for (size_t i = 0; i < ops.size(); ++i) {
        const auto& op = ops[i];

        if (op.isInsert) {
            if (!edges.insert(makeEdge(op.u, op.v)).second) {
                ++skipped; // parallel edges are out of scope for these backends
                continue;
            }
            adjacency[op.u].insert(op.v);
            adjacency[op.v].insert(op.u);

            const auto start = Clock::now();
            index->insertEdge(op.u, op.v);
            inserts.seconds += std::chrono::duration<double>(Clock::now() - start).count();
            ++inserts.count;

            if (!oracleStale) {
                oracle.unite(denseId.at(op.u), denseId.at(op.v)); // insertion is incremental
            }
        } else {
            if (edges.erase(makeEdge(op.u, op.v)) == 0) {
                ++skipped;
                continue;
            }
            adjacency[op.u].erase(op.v);
            adjacency[op.v].erase(op.u);

            const auto start = Clock::now();
            index->deleteEdge(op.u, op.v, provider);
            deletes.seconds += std::chrono::duration<double>(Clock::now() - start).count();
            ++deletes.count;

            oracleStale = true; // union-find cannot un-merge; rebuild on demand
        }

        if (opt.check == CheckMode::None) {
            continue;
        }
        refreshOracle(); // deliberately after the timed region
        const auto n = originalId.size();
        auto verify = [&](size_t a, size_t b) {
            const bool truth = oracle.find(a) == oracle.find(b);
            if (index->connected(originalId[a], originalId[b]) != truth) {
                if (mismatches < 10) {
                    std::cerr << "MISMATCH at op " << i << " (" << (op.isInsert ? "ins " : "del ")
                              << op.u << " " << op.v << "): connected(" << originalId[a] << ", "
                              << originalId[b] << ") disagrees with the oracle, expected "
                              << (truth ? "true" : "false") << "\n";
                }
                ++mismatches;
            }
        };
        if (opt.check == CheckMode::Full) {
            for (size_t a = 0; a < n; ++a) {
                for (size_t b = a + 1; b < n; ++b) {
                    verify(a, b);
                }
            }
        } else {
            std::uniform_int_distribution<size_t> pick{0, n - 1};
            for (int k = 0; k < opt.samplesPerStep; ++k) {
                const auto a = pick(rng);
                const auto b = pick(rng);
                if (a != b) {
                    verify(a, b);
                }
            }
        }
    }

    std::printf("method=%s workload=%s nodes=%zu\n", opt.method.c_str(), opt.workload.c_str(),
        originalId.size());
    std::printf("insertions: count=%llu total_s=%.6f avg_s=%.9f\n",
        static_cast<unsigned long long>(inserts.count), inserts.seconds, inserts.average());
    std::printf("deletions:  count=%llu total_s=%.6f avg_s=%.9f\n",
        static_cast<unsigned long long>(deletes.count), deletes.seconds, deletes.average());
    std::printf("skipped=%llu (duplicate inserts and absent deletes)\n",
        static_cast<unsigned long long>(skipped));
    if (providerCalls > 0) {
        std::printf("provider: calls=%llu neighbors=%llu avg_per_call=%.2f max_degree=%llu\n",
            static_cast<unsigned long long>(providerCalls),
            static_cast<unsigned long long>(providerNeighbors),
            static_cast<double>(providerNeighbors) / static_cast<double>(providerCalls),
            static_cast<unsigned long long>(providerMaxDegree));
        std::printf("provider: calls_per_deletion=%.2f\n",
            deletes.count == 0 ?
                0.0 :
                static_cast<double>(providerCalls) / static_cast<double>(deletes.count));
    }
    if (opt.check != CheckMode::None) {
        std::printf("check: mismatches=%llu\n", static_cast<unsigned long long>(mismatches));
    }

    // One parseable line for the CSV shim.
    std::printf("RESULT method=%s graph=%s ratio=%s insertions_avg_s=%.9f deletions_avg_s=%.9f "
                "insertions=%llu deletions=%llu mismatches=%llu\n",
        opt.method.c_str(), opt.label.c_str(), opt.ratio.c_str(), inserts.average(),
        deletes.average(), static_cast<unsigned long long>(inserts.count),
        static_cast<unsigned long long>(deletes.count),
        static_cast<unsigned long long>(mismatches));

    return mismatches == 0 ? 0 : 1;
}
