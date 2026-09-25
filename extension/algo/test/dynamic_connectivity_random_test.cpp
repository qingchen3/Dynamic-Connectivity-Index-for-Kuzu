// Randomised differential testing for the four dynamic connectivity backends.
//
// Each trace is replayed against all four backends simultaneously and against an
// independent component oracle. Two things are checked after every operation:
//
//   1. Every backend agrees with the oracle on every vertex pair.
//   2. The backends agree with each other -- a disagreement names which one
//      diverged, which localises a fault to a single implementation.
//
// The oracle labels components once per step in O(n + m) and answers each pair in
// O(1), so all-pairs verification stays affordable.
//
// Graphs are deliberately tiny. Six vertices is enough to produce bridges,
// replacement searches that succeed and replacement searches that fail, while
// keeping a failing trace small enough to step through by hand.
//
// On divergence the trace is truncated at the failing step and then greedily
// minimised -- operations are dropped one at a time for as long as the failure
// survives -- so the report is a minimal reproducer rather than a raw log.
//
// A green run means nothing unless the corpus reached the interesting code path,
// so each shape declares floors on component merges, bridge deletions (where
// replacement search must fail) and non-bridge deletions (where it must
// succeed), and falls over if it stops reaching them.

#include "gtest/gtest.h"

#include "common/dynamic_connectivity_index_factory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace kuzu::algo_extension {
namespace {

using Edge = std::pair<int64_t, int64_t>;

Edge makeEdge(int64_t u, int64_t v) {
    return {std::min(u, v), std::max(u, v)};
}

// Independent model of the graph. Never consults any backend.
class Model {
public:
    explicit Model(int64_t numNodes) : numNodes{numNodes} {}

    bool insert(int64_t u, int64_t v) { return edges.insert(makeEdge(u, v)).second; }
    bool erase(int64_t u, int64_t v) { return edges.erase(makeEdge(u, v)) > 0; }
    bool contains(int64_t u, int64_t v) const { return edges.count(makeEdge(u, v)) > 0; }
    const std::set<Edge>& getEdges() const { return edges; }

    std::vector<int64_t> neighbors(int64_t u) const {
        std::vector<int64_t> out;
        for (auto [a, b] : edges) {
            if (a == u) {
                out.push_back(b);
            } else if (b == u) {
                out.push_back(a);
            }
        }
        return out;
    }

    // Component label per node; two nodes are connected iff labels match.
    // Isolated nodes each get their own label, matching the backends: a node the
    // forest never saw is connected to nothing but itself.
    std::vector<int64_t> componentLabels() const {
        std::map<int64_t, std::vector<int64_t>> adjacency;
        for (auto [a, b] : edges) {
            adjacency[a].push_back(b);
            adjacency[b].push_back(a);
        }
        std::vector<int64_t> label(static_cast<size_t>(numNodes), -1);
        int64_t next = 0;
        for (int64_t root = 0; root < numNodes; ++root) {
            if (label[static_cast<size_t>(root)] != -1) {
                continue;
            }
            label[static_cast<size_t>(root)] = next;
            std::queue<int64_t> pending;
            pending.push(root);
            while (!pending.empty()) {
                auto x = pending.front();
                pending.pop();
                auto it = adjacency.find(x);
                if (it == adjacency.end()) {
                    continue;
                }
                for (auto y : it->second) {
                    if (label[static_cast<size_t>(y)] == -1) {
                        label[static_cast<size_t>(y)] = next;
                        pending.push(y);
                    }
                }
            }
            ++next;
        }
        return label;
    }

private:
    int64_t numNodes;
    std::set<Edge> edges;
};

struct Op {
    bool isInsert;
    int64_t u;
    int64_t v;
};

std::string describe(const std::vector<Op>& ops) {
    std::ostringstream out;
    for (const auto& op : ops) {
        out << "  " << (op.isInsert ? "INS " : "DEL ") << op.u << " " << op.v << "\n";
    }
    return out.str();
}

const std::vector<std::string>& backendNames() {
    static const std::vector<std::string> names{"dtree", "dtree_csr", "stree", "stree_csr"};
    return names;
}

// Applies one operation to the model and to every supplied backend, keeping the
// two in step. Returns false when the operation was a no-op that the generator
// would also have skipped (re-inserting an edge that is already present), so a
// replay of any subsequence stays self-consistent.
bool applyOp(std::vector<std::unique_ptr<DynamicConnectivityIndex>>& indexes, Model& model,
    const Op& op) {
    if (op.isInsert) {
        if (!model.insert(op.u, op.v)) {
            return false; // parallel edges are out of scope for these backends
        }
        for (auto& index : indexes) {
            index->insertEdge(op.u, op.v);
        }
    } else {
        model.erase(op.u, op.v);
        // The provider reads the model AFTER the deletion, mirroring how the real
        // one reads the CSR once the row is already gone.
        auto provider = [&model](int64_t x) { return model.neighbors(x); };
        for (auto& index : indexes) {
            index->deleteEdge(op.u, op.v, provider);
        }
    }
    return true;
}

struct Divergence {
    bool found = false;
    size_t opIndex = 0; // index into the op list, not the generator's step counter
    int64_t a = 0;
    int64_t b = 0;
    bool got = false;
    bool want = false;
};

// Replays a trace against a single backend and reports the first disagreement
// with the oracle. Used for minimisation and for verifying the minimised trace.
Divergence replayFindDivergence(const std::string& backendName, int64_t numNodes,
    const std::vector<Op>& ops) {
    std::vector<std::unique_ptr<DynamicConnectivityIndex>> indexes;
    indexes.push_back(createDynamicConnectivityIndex(backendName));
    Model model{numNodes};
    for (size_t i = 0; i < ops.size(); ++i) {
        if (!applyOp(indexes, model, ops[i])) {
            continue;
        }
        const auto expected = model.componentLabels();
        for (int64_t a = 0; a < numNodes; ++a) {
            for (int64_t b = 0; b < numNodes; ++b) {
                if (a == b) {
                    continue;
                }
                const bool truth = expected[static_cast<size_t>(a)] ==
                                   expected[static_cast<size_t>(b)];
                const bool got = indexes[0]->connected(a, b);
                if (got != truth) {
                    return Divergence{true, i, a, b, got, truth};
                }
            }
        }
    }
    return Divergence{};
}

bool replayDiverges(const std::string& backendName, int64_t numNodes,
    const std::vector<Op>& ops) {
    return replayFindDivergence(backendName, numNodes, ops).found;
}

// Greedily drops operations for as long as the divergence survives. Quadratic in
// the trace length, but it only runs on failure and the traces are short.
std::vector<Op> shrink(const std::string& backendName, int64_t numNodes, std::vector<Op> ops) {
    if (!replayDiverges(backendName, numNodes, ops)) {
        return ops; // should not happen; leave the trace untouched rather than lie
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < ops.size();) {
            auto candidate = ops;
            candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(i));
            if (replayDiverges(backendName, numNodes, candidate)) {
                ops = std::move(candidate);
                changed = true;
            } else {
                ++i;
            }
        }
    }
    return ops;
}

// How much of the interesting behaviour a corpus actually reached.
struct Coverage {
    uint64_t merges = 0;           // insertion joined two components
    uint64_t bridgeDeletes = 0;    // deletion split a component: replacement search must fail
    uint64_t nonBridgeDeletes = 0; // deletion kept it whole: non-tree edge, or replacement found
    uint64_t noOpDeletes = 0;      // deletion of an absent edge

    void add(const Coverage& other) {
        merges += other.merges;
        bridgeDeletes += other.bridgeDeletes;
        nonBridgeDeletes += other.nonBridgeDeletes;
        noOpDeletes += other.noOpDeletes;
    }
};

struct TraceSpec {
    const char* name;
    int64_t numNodes;
    int numOps;
    // Probability of attempting an insertion. Low values keep the graph sparse and
    // produce bridges; high values make it dense and produce replacements.
    double insertBias;
    // Floors this shape must reach for its pass to mean anything. Set well under
    // what the generator actually produces, so they fail on a broken generator
    // rather than on RNG drift.
    uint64_t minMerges;
    uint64_t minBridgeDeletes;
    uint64_t minNonBridgeDeletes;
};

// Reports a divergence with a minimised reproducer, and re-verifies that the
// minimised trace really does reproduce it. Replaying a trace is only faithful if
// a subsequence behaves exactly as the generator would have; if that assumption
// ever breaks, the minimal trace is a fiction, so say so loudly rather than hand
// over a reproducer that does not reproduce.
void reportDivergence(const std::string& backendName, int64_t numNodes, uint32_t seed,
    const std::vector<Op>& applied, int64_t a, int64_t b, bool got, bool want) {
    std::ostringstream report;
    report << backendName << " disagrees with the oracle (seed " << seed << ", " << numNodes
           << " nodes): connected(" << a << ", " << b << ") returned " << (got ? "true" : "false")
           << ", expected " << (want ? "true" : "false") << "\n";

    const auto direct = replayFindDivergence(backendName, numNodes, applied);
    if (!direct.found) {
        report << "\nWARNING: replaying the recorded trace against " << backendName
               << " alone does NOT reproduce this. The divergence depends on something "
                  "outside the recorded operations, so minimisation is unsound here.\n"
               << "\nfull recorded trace (" << applied.size() << " operations):\n"
               << describe(applied);
        ADD_FAILURE() << report.str();
        return;
    }

    const auto minimal = shrink(backendName, numNodes, applied);
    const auto check = replayFindDivergence(backendName, numNodes, minimal);
    report << "\nminimal reproducer (" << minimal.size() << " of " << applied.size()
           << " operations):\n"
           << describe(minimal);
    if (check.found) {
        report << "verified: diverges after operation " << check.opIndex << " on connected("
               << check.a << ", " << check.b << ") -- returned " << (check.got ? "true" : "false")
               << ", expected " << (check.want ? "true" : "false") << "\n";
    } else {
        report << "WARNING: the minimised trace does not reproduce on replay; "
                  "trust the full trace below instead.\n"
               << "\nfull recorded trace (" << applied.size() << " operations):\n"
               << describe(applied);
    }
    ADD_FAILURE() << report.str();
}

// Draws the next operation. Deletions mostly target an edge that exists: a
// uniformly random pair is almost always a non-edge at these densities, which
// exercises nothing. `present` reports whether a deletion hit a real edge.
Op generateOp(std::mt19937& rng, const Model& model, const TraceSpec& spec, bool& present) {
    std::uniform_int_distribution<int64_t> pickNode{0, spec.numNodes - 1};
    std::uniform_real_distribution<double> pickAction{0.0, 1.0};
    // Self-loops are excluded throughout: the backends never register a node from
    // a self-loop alone, so connected(u, u) would disagree with the oracle for
    // reasons unrelated to connectivity maintenance.
    auto pickPair = [&]() {
        auto x = pickNode(rng);
        auto y = pickNode(rng);
        if (x == y) {
            y = (y + 1) % spec.numNodes;
        }
        return makeEdge(x, y);
    };

    present = false;
    const bool wantInsert = model.getEdges().empty() || pickAction(rng) < spec.insertBias;
    Op op{wantInsert, 0, 0};
    if (wantInsert) {
        std::tie(op.u, op.v) = pickPair();
    } else if (pickAction(rng) < 0.9) {
        const auto& edges = model.getEdges();
        auto it = edges.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(rng() % edges.size()));
        std::tie(op.u, op.v) = *it;
        present = true;
    } else {
        std::tie(op.u, op.v) = pickPair();
        present = model.contains(op.u, op.v);
    }

    if ((rng() & 1u) != 0u) {
        std::swap(op.u, op.v);
    }

    return op;
}

// Replays one trace against all four backends. Returns coverage, or fails with a
// minimal reproducer on the first divergence.
Coverage runTrace(const TraceSpec& spec, uint32_t seed) {
    SCOPED_TRACE(std::string(spec.name) + " seed=" + std::to_string(seed));

    std::vector<std::unique_ptr<DynamicConnectivityIndex>> indexes;
    for (const auto& name : backendNames()) {
        indexes.push_back(createDynamicConnectivityIndex(name));
    }

    Model model{spec.numNodes};
    Coverage coverage;
    std::vector<Op> applied;
    std::mt19937 rng{seed};

    for (int step = 0; step < spec.numOps; ++step) {
        const auto before = model.componentLabels();
        bool present = false;
        const Op op = generateOp(rng, model, spec, present);

        if (!applyOp(indexes, model, op)) {
            continue;
        }
        applied.push_back(op);

        const auto expected = model.componentLabels();
        const auto lu = static_cast<size_t>(op.u);
        const auto lv = static_cast<size_t>(op.v);
        if (op.isInsert) {
            if (before[lu] != before[lv]) {
                ++coverage.merges;
            }
        } else if (!present) {
            ++coverage.noOpDeletes;
        } else if (expected[lu] != expected[lv]) {
            ++coverage.bridgeDeletes;
        } else {
            ++coverage.nonBridgeDeletes;
        }

        for (int64_t a = 0; a < spec.numNodes; ++a) {
            for (int64_t b = 0; b < spec.numNodes; ++b) {
                if (a == b) {
                    continue;
                }
                const bool truth = expected[static_cast<size_t>(a)] ==
                                   expected[static_cast<size_t>(b)];
                std::vector<bool> answers;
                answers.reserve(indexes.size());
                for (auto& index : indexes) {
                    answers.push_back(index->connected(a, b));
                }
                for (size_t i = 0; i < indexes.size(); ++i) {
                    if (answers[i] != truth) {
                        reportDivergence(backendNames()[i], spec.numNodes, seed, applied, a, b,
                            answers[i], truth);
                        return coverage;
                    }
                }
                // Redundant while every backend matches the oracle, but it makes a
                // divergence report name the odd one out directly.
                for (size_t i = 1; i < answers.size(); ++i) {
                    if (answers[i] != answers[0]) {
                        ADD_FAILURE()
                            << backendNames()[i] << " disagrees with " << backendNames()[0]
                            << " on connected(" << a << ", " << b << ") at step " << step
                            << " (seed " << seed << ")\ntrace:\n"
                            << describe(applied);
                        return coverage;
                    }
                }
            }
        }
    }
    return coverage;
}

const std::vector<TraceSpec>& traceSpecs() {
    // Six vertices throughout. The shapes differ in trace length and in how
    // strongly they favour insertion, which is what moves the balance between
    // failing and succeeding replacement searches.
    //                 name          n  ops  bias  merges bridge nonBridge
    static const std::vector<TraceSpec> specs{
        {"tiny_sparse", 6, 40, 0.35, 40, 40, 0},
        {"tiny_balanced", 6, 60, 0.50, 60, 50, 3},
        {"tiny_dense", 6, 60, 0.75, 20, 8, 20},
        {"churn_short", 6, 80, 0.45, 80, 70, 3},
        {"churn_long", 6, 120, 0.50, 100, 90, 15},
        {"saturating", 6, 100, 0.80, 20, 5, 40},
        {"thrash", 6, 120, 0.30, 120, 120, 1},
        {"balanced_12",         12,      300,       0.50,      20,      20,           1},
        {"balanced_24",         24,      500,       0.50,      20,      20,           1},
        {"balanced_64",         64,     1000,       0.50,      20,      20,           0},
        {"sparse_64",           64,     1000,       0.30,      20,      20,           0},
        {"dense_24",            24,     1000,       0.80,      20,       1,          20},
        {"dense_64",            64,     2000,       0.85,      20,       1,          20},
    };
    return specs;
}

class RandomTraceTest : public ::testing::TestWithParam<TraceSpec> {};

TEST_P(RandomTraceTest, AllBackendsMatchOracleAcrossSeeds) {
    constexpr uint32_t NUM_SEEDS = 100;
    constexpr uint32_t SEED_BASE = 20260917;
    Coverage total;
    for (uint32_t i = 0; i < NUM_SEEDS; ++i) {
        total.add(runTrace(GetParam(), SEED_BASE + i));
        if (::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure()) {
            return; // the first divergence already printed a minimal reproducer
        }
    }

    // A corpus that never split a component never exercised a failing replacement
    // search, and one that never kept a component whole never exercised a
    // succeeding one. Either way the pass would be vacuous.
    EXPECT_GE(total.bridgeDeletes, GetParam().minBridgeDeletes)
        << "too few deletions split a component: failing replacement searches under-covered";
    EXPECT_GE(total.nonBridgeDeletes, GetParam().minNonBridgeDeletes)
        << "too few deletions were absorbed: succeeding replacement searches under-covered";
    EXPECT_GE(total.merges, GetParam().minMerges) << "too few insertions joined two components";

    RecordProperty("seed_base", std::to_string(SEED_BASE));
    RecordProperty("num_seeds", std::to_string(NUM_SEEDS));
    RecordProperty("merges", std::to_string(total.merges));
    RecordProperty("bridge_deletes", std::to_string(total.bridgeDeletes));
    RecordProperty("non_bridge_deletes", std::to_string(total.nonBridgeDeletes));
    RecordProperty("noop_deletes", std::to_string(total.noOpDeletes));
}

// connected() is not a pure read in the DTree backends: it reroots to rebalance.
// So its own side effects must not be visible in its answers. Three properties
// follow, and the stale-root fault this suite first caught violated all three:
//
//   repetition   asking the same pair twice must give the same answer
//   order        the answer must not depend on which pairs were asked before it
//   stability    a storm of unrelated queries must not change any answer
//
// Checking against the oracle alone does not cover these: a backend could be
// right on a first pass in one fixed order and wrong on a second.
TEST_P(RandomTraceTest, QueryingDoesNotDisturbItsOwnAnswers) {
    const auto& spec = GetParam();
    constexpr uint32_t NUM_SEEDS = 30;
    constexpr int QUERY_STORM = 50;

    std::vector<Edge> pairs;
    for (int64_t a = 0; a < spec.numNodes; ++a) {
        for (int64_t b = 0; b < spec.numNodes; ++b) {
            if (a != b) {
                pairs.emplace_back(a, b);
            }
        }
    }

    for (uint32_t s = 0; s < NUM_SEEDS; ++s) {
        const auto seed = 20260917u + s;
        SCOPED_TRACE(std::string(spec.name) + " seed=" + std::to_string(seed));

        std::vector<std::unique_ptr<DynamicConnectivityIndex>> indexes;
        for (const auto& name : backendNames()) {
            indexes.push_back(createDynamicConnectivityIndex(name));
        }
        Model model{spec.numNodes};
        std::mt19937 rng{seed};
        std::uniform_int_distribution<int64_t> pickNode{0, spec.numNodes - 1};

        for (int step = 0; step < spec.numOps; ++step) {
            bool present = false;
            const Op op = generateOp(rng, model, spec, present);
            if (!applyOp(indexes, model, op)) {
                continue;
            }
            const auto expected = model.componentLabels();

            for (size_t i = 0; i < indexes.size(); ++i) {
                SCOPED_TRACE(backendNames()[i] + " step=" + std::to_string(step));

                std::shuffle(pairs.begin(), pairs.end(), rng);
                std::map<Edge, bool> firstPass;
                for (auto [a, b] : pairs) {
                    const bool got = indexes[i]->connected(a, b);
                    ASSERT_EQ(got, expected[static_cast<size_t>(a)] ==
                                       expected[static_cast<size_t>(b)])
                        << "first pass, pair (" << a << ", " << b << ")";
                    firstPass[{a, b}] = got;
                }

                // Churn the structure with queries that are not being checked.
                for (int k = 0; k < QUERY_STORM; ++k) {
                    auto a = pickNode(rng);
                    auto b = pickNode(rng);
                    if (a != b) {
                        const bool truth =
                        expected[static_cast<size_t>(a)] ==
                        expected[static_cast<size_t>(b)];

                        ASSERT_EQ(indexes[i]->connected(a, b), truth)
                            << "query storm, query=" << k
                            << ", pair=(" << a << "," << b << ")";
                    }
                }

                // Same pairs, different order, after the storm.
                std::shuffle(pairs.begin(), pairs.end(), rng);
                for (auto [a, b] : pairs) {
                    const bool got = indexes[i]->connected(a, b);
                    ASSERT_EQ(got, firstPass.at({a, b}))
                        << "answer changed for (" << a << ", " << b
                        << ") after re-querying in a different order";
                    ASSERT_EQ(got, expected[static_cast<size_t>(a)] ==
                                       expected[static_cast<size_t>(b)])
                        << "second pass, pair (" << a << ", " << b << ")";
                }

                // A node is connected to itself, and asking must stay harmless.
                for (int64_t a = 0; a < spec.numNodes; ++a) {
                    if (indexes[i]->containsNode(a)) {
                        ASSERT_TRUE(indexes[i]->connected(a, a)) << "self pair " << a;
                    }
                }
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Shapes, RandomTraceTest, ::testing::ValuesIn(traceSpecs()),
    [](const ::testing::TestParamInfo<TraceSpec>& info) { return info.param.name; });

// Deleting every edge must leave every pair disconnected, whatever order the
// deletions happen in. This is the case where replacement search runs most often
// and finally has nothing left to find.
TEST(RandomTraceTest, DrainingAllEdgesDisconnectsEverything) {
    constexpr int64_t NUM_NODES = 6;
    for (uint32_t seed = 0; seed < 12; ++seed) {
        SCOPED_TRACE("drain seed=" + std::to_string(seed));
        std::vector<std::unique_ptr<DynamicConnectivityIndex>> indexes;
        for (const auto& name : backendNames()) {
            indexes.push_back(createDynamicConnectivityIndex(name));
        }
        Model model{NUM_NODES};
        std::mt19937 rng{20260917u + seed};
        std::uniform_int_distribution<int64_t> pickNode{0, NUM_NODES - 1};

        for (int i = 0; i < 20; ++i) {
            auto u = pickNode(rng);
            auto v = pickNode(rng);
            if (u == v) {
                v = (v + 1) % NUM_NODES;
            }
            applyOp(indexes, model, Op{true, u, v});
        }

        std::vector<Edge> remaining{model.getEdges().begin(), model.getEdges().end()};
        std::shuffle(remaining.begin(), remaining.end(), rng);
        for (auto [a, b] : remaining) {
            if ((rng() & 1u) != 0u) {
                std::swap(a, b);
            }

            applyOp(indexes, model, Op{false, a, b});
            const auto expected = model.componentLabels();

            for (size_t i = 0; i < indexes.size(); ++i) {
                SCOPED_TRACE(
                    backendNames()[i] +
                    " after deleting (" + std::to_string(a) +
                    "," + std::to_string(b) + ")");

                for (int64_t u = 0; u < NUM_NODES; ++u) {
                    for (int64_t v = 0; v < NUM_NODES; ++v) {
                        if (u == v) {
                            continue;
                        }

                        const bool truth =
                            expected[static_cast<size_t>(u)] ==
                            expected[static_cast<size_t>(v)];

                        ASSERT_EQ(indexes[i]->connected(u, v), truth)
                            << "pair=(" << u << "," << v << ")";
                    }
                }
            }
        }

        ASSERT_TRUE(model.getEdges().empty());
        for (size_t i = 0; i < indexes.size(); ++i) {
            SCOPED_TRACE(backendNames()[i]);
            for (int64_t a = 0; a < NUM_NODES; ++a) {
                for (int64_t b = 0; b < NUM_NODES; ++b) {
                    if (a != b) {
                        ASSERT_FALSE(indexes[i]->connected(a, b)) << a << "," << b;
                    }
                }
            }
        }
    }
}

} // namespace
} // namespace kuzu::algo_extension
