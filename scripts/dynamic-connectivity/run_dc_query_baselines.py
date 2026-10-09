#!/usr/bin/env python3
"""Database-backed correctness tests for BFS/WCC and the four DC indexes.

Imports workload parsing and the independent Python oracle from the repository's
existing run_dc_workload.py. Generates standard .test cases for e2e_test.
This is NOT a performance benchmark: no timing speedups are reported.
Python 3.9+, standard library only.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


INDEX_METHODS = ("dtree", "dtree_csr", "stree", "stree_csr")
METHODS = INDEX_METHODS + ("bfs", "wcc")


def load_helpers(path):
    if not path.is_file():
        raise ValueError(f"Missing existing workload runner: {path}")
    spec = importlib.util.spec_from_file_location("dc_workload_helpers", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for name in ("read_workload", "labels_from_bfs", "choose_pairs"):
        if not callable(getattr(module, name, None)):
            raise ValueError(f"{path} does not define {name}")
    return module


def trim_operations(operations, max_deletions):
    if not max_deletions:
        return operations
    deletions = 0
    for index, operation in enumerate(operations):
        deletions += operation[1] == "del"
        if deletions == max_deletions:
            return operations[:index + 1]
    raise ValueError(f"Workload contains only {deletions} deletions, fewer than requested")


def make_plan(helpers, operations, vertices, seed, samples, query_every):
    adjacency = {vertex: set() for vertex in vertices}
    live = {}
    checks = []
    rng = random.Random(seed)
    updates = 0

    def check(source_line, reason, endpoints=None):
        labels = helpers.labels_from_bfs(vertices, adjacency)
        pairs = helpers.choose_pairs(vertices, labels, rng, samples, endpoints)
        checks.append({
            "after_updates": updates,
            "source_line": source_line,
            "reason": reason,
            "pairs": [{"u": u, "v": v, "connected": labels[u] == labels[v]}
                      for u, v in pairs],
            # Canonical labels: smallest user vertex ID in each component.
            "partition_rows": [f"{vertex}|{labels[vertex]}" for vertex in vertices],
            "edge_count": len(live),
        })

    for line, kind, first, second in operations:
        if kind == "query":
            check(line, "query_marker")
            continue
        u, v = sorted((first, second))
        if kind == "ins":
            live[(u, v)] = line
            adjacency[u].add(v)
            adjacency[v].add(u)
        else:
            live.pop((u, v))
            adjacency[u].remove(v)
            adjacency[v].remove(u)
        updates += 1
        if query_every and updates % query_every == 0:
            check(line, "periodic", (u, v))
    check(operations[-1][0], "final_before_checkpoint")
    return checks, live


def generate(path, method, operations, vertices, checks, final_edges, threads):
    offsets = {vertex: offset for offset, vertex in enumerate(vertices)}
    hop_bound = max(1, len(vertices) - 1)
    index_name = f"dc_{method}"
    graph_name = "DCQueryGraph"
    by_line = {}
    for check in checks:
        by_line.setdefault(check["source_line"], []).append(check)
    directed_queries = 0
    wcc_calls = 0
    updates = 0
    live = {}

    with path.open("w") as out:
        def statement(query, rows=None):
            out.write(f"-STATEMENT {query}\n")
            if rows is None:
                out.write("---- ok\n")
            else:
                out.write(f"---- {len(rows)}\n")
                out.write("".join(f"{row}\n" for row in rows))

        def verify(check):
            nonlocal directed_queries, wcc_calls
            out.write(f"\n# DC_CHECK after_updates={check['after_updates']} "
                      f"source_line={check['source_line']} reason={check['reason']}\n")
            statement("MATCH ()-[r:Edges]->() RETURN count(r);", [check["edge_count"]])
            if method == "wcc":
                # Raw group_id values are implementation-specific. Canonicalize
                # each component using its minimum vID and check every vertex.
                statement(
                    f"CALL WEAKLY_CONNECTED_COMPONENTS('{graph_name}', "
                    f"maxIterations := {len(vertices) + 1}) "
                    "WITH group_id, collect(node.vID) AS members, "
                    "min(node.vID) AS component "
                    "UNWIND members AS vertex "
                    "RETURN vertex, component ORDER BY vertex;",
                    check["partition_rows"],
                )
                wcc_calls += 1
                return
            for pair in check["pairs"]:
                u, v, expected = pair["u"], pair["v"], pair["connected"]
                for a, b in ((u, v), (v, u)):
                    if method == "bfs":
                        query = (
                            f"MATCH (a:Vertex {{vID: {a}}})"
                            f"-[:Edges* SHORTEST 1..{hop_bound}]-"
                            f"(b:Vertex {{vID: {b}}}) "
                            "RETURN count(*) > 0 AS connected;"
                        )
                    else:
                        query = (
                            f"CALL DYNAMIC_CONNECTIVITY_QUERY('Vertex', "
                            f"{offsets[a]}, {offsets[b]}, '{index_name}') RETURN *;"
                        )
                    statement(query, [expected])
                    directed_queries += 1

        out.write(f"-DATASET CSV EMPTY\n\n--\n\n-CASE Replay_{method}\n")
        out.write("-LOAD_DYNAMIC_EXTENSION algo\n")
        out.write("\n# Correctness test, not a performance benchmark.\n")
        statement(f"CALL THREADS={threads};")
        if method == "bfs":
            statement(f"CALL VAR_LENGTH_EXTEND_MAX_DEPTH={hop_bound};")
        statement("CREATE NODE TABLE Vertex(vID INT64, PRIMARY KEY(vID));")
        # Preserve the original runner's trace identity for precise replay.
        statement("CREATE REL TABLE Edges(FROM Vertex TO Vertex, eid INT64);")
        statement("BEGIN TRANSACTION;")
        for vertex in vertices:
            statement(f"CREATE (:Vertex {{vID: {vertex}}});")
        statement("COMMIT;")
        if method in INDEX_METHODS:
            statement("MATCH (v:Vertex) RETURN v.vID, OFFSET(ID(v)) ORDER BY v.vID;",
                      [f"{vertex}|{offsets[vertex]}" for vertex in vertices])
            statement(
                f"CALL CREATE_DYNAMIC_CONNECTIVITY_INDEX('Vertex', 'Edges', "
                f"'{index_name}', '{method}') RETURN *;",
                [f"Dynamic connectivity index {index_name} has been created with 0 edges."],
            )
        elif method == "wcc":
            statement(f"CALL PROJECT_GRAPH('{graph_name}', ['Vertex'], ['Edges']);")

        for line, kind, first, second in operations:
            if kind != "query":
                u, v = sorted((first, second))
                out.write(f"\n# Source line {line}: {kind} {first} {second}\n")
                statement("BEGIN TRANSACTION;")
                if kind == "ins":
                    live[(u, v)] = line
                    statement(f"MATCH (a:Vertex {{vID: {u}}}), (b:Vertex {{vID: {v}}}) "
                              f"CREATE (a)-[:Edges {{eid: {line}}}]->(b);")
                else:
                    eid = live.pop((u, v))
                    statement(f"MATCH ()-[r:Edges]->() WHERE r.eid = {eid} DELETE r;")
                statement("COMMIT;")
                updates += 1
            for check in by_line.get(line, []):
                verify(check)

        statement("CHECKPOINT;")
        out.write("\n# Verify the exact final relationships after checkpoint.\n")
        statement(
            "MATCH (a:Vertex)-[r:Edges]->(b:Vertex) "
            "RETURN a.vID, b.vID, r.eid ORDER BY r.eid;",
            [f"{u}|{v}|{eid}" for (u, v), eid in
             sorted(final_edges.items(), key=lambda item: item[1])],
        )
        # Check connectivity again to exercise checkpointed storage as well.
        post_checkpoint = dict(checks[-1], reason="final_after_checkpoint")
        verify(post_checkpoint)

    return {
        "method": method, "updates": updates, "check_points": len(checks) + 1,
        "directed_pair_queries": directed_queries, "wcc_calls": wcc_calls,
        "partition_rows_checked": wcc_calls * len(vertices),
        "pair_answers_covered_by_partitions": (
            2 * (sum(len(c["pairs"]) for c in checks) + len(checks[-1]["pairs"]))
            if method == "wcc" else 0
        ),
        "hop_bound": hop_bound if method == "bfs" else None,
        "test_file": path.name,
    }


def run_test(repo, runner, output, method, path):
    env = os.environ.copy()
    env.update(IN_MEM_MODE="false", E2E_REWRITE_TESTS="OFF",
               E2E_TEST_FILES_DIRECTORY=str(output.relative_to(repo)),
               DC_REUSE_SCAN_STATE="1", DC_VERIFY_SCAN_STATE="0")
    for key in tuple(env):
        if key.startswith("GTEST_"):
            del env[key]
    log, xml = output / f"{method}.log", output / f"{method}.xml"
    with log.open("w") as stream:
        completed = subprocess.run(
            [str(runner), path.name, f"--gtest_output=xml:{xml}"],
            cwd=repo, env=env, stdout=stream, stderr=subprocess.STDOUT,
        )
    passed = False
    if xml.is_file():
        root = ET.parse(xml).getroot()
        passed = (root.get("tests") == "1"
                  and all(root.get(k, "0") == "0"
                          for k in ("failures", "errors", "disabled", "skipped"))
                  and not root.findall(".//skipped"))
    return {"status": "passed" if completed.returncode == 0 and passed else "failed",
            "exit_code": completed.returncode, "log": log.name, "xml": xml.name}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--workload", type=Path, required=True)
    parser.add_argument("--base-runner", type=Path)
    parser.add_argument("--build", default="build/relwithdebinfo")
    parser.add_argument("--methods", nargs="+", choices=("all",) + METHODS,
                        default=["bfs", "wcc"])
    parser.add_argument("--seed", type=int, default=20260921)
    parser.add_argument("--marker-samples", type=int, default=8,
                        help="random pair draws per check; also includes representative pairs")
    parser.add_argument("--query-every", type=int, default=0,
                        help="additional check every N updates; 0 means markers and final only")
    parser.add_argument("--max-deletions", type=int, default=0,
                        help="stop input at Nth deletion; 0 uses the whole trace")
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    if min(args.marker_samples, args.query_every, args.max_deletions) < 0 or args.threads < 1:
        parser.error("counts must be non-negative and --threads must be positive")
    if "all" in args.methods and len(args.methods) != 1:
        parser.error("use --methods all by itself")
    repo, workload = args.repo.resolve(), args.workload.resolve()
    helpers_path = (args.base_runner.resolve() if args.base_runner else
                    repo / "scripts/dynamic-connectivity/run_dc_workload.py")
    runner = repo / args.build / "test/runner/e2e_test"
    if args.run and not runner.is_file():
        parser.error(f"Missing {runner}; build e2e_test and kuzu_algo_extension first")
    helpers = load_helpers(helpers_path)
    operations, _, _ = helpers.read_workload(workload)
    operations = trim_operations(operations, args.max_deletions)
    vertices = sorted({v for _, kind, a, b in operations if kind != "query" for v in (a, b)})
    if len(vertices) < 2:
        parser.error("at least two vertices are required")
    counts = {kind: sum(op[1] == kind for op in operations) for kind in ("ins", "del", "query")}
    checks, final_edges = make_plan(helpers, operations, vertices, args.seed,
                                   args.marker_samples, args.query_every)
    parent = repo / "test-results"
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="dc_query_baselines_", dir=parent))
    plan_path = output / "query_pairs.jsonl"
    with plan_path.open("w") as stream:
        for check in checks + [dict(checks[-1], reason="final_after_checkpoint")]:
            stream.write(json.dumps({k: v for k, v in check.items()
                                     if k != "partition_rows"}, sort_keys=True) + "\n")
    methods = METHODS if args.methods == ["all"] else tuple(dict.fromkeys(args.methods))
    report = {
        "mode": "correctness_only", "workload": str(workload),
        "workload_sha256": hashlib.sha256(workload.read_bytes()).hexdigest(),
        "base_runner_sha256": hashlib.sha256(helpers_path.read_bytes()).hexdigest(),
        "query_pairs_sha256": hashlib.sha256(plan_path.read_bytes()).hexdigest(),
        "query_pairs": plan_path.name, "vertices": len(vertices), "counts": counts,
        "max_deletions": args.max_deletions, "query_every": args.query_every,
        "marker_samples": args.marker_samples, "seed": args.seed, "threads": args.threads,
        "checkpoint_policy": "final checkpoint followed by another connectivity check",
        "wcc_policy": "one full partition per check, canonicalized to minimum vID",
        "runs": [],
    }
    git = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo, capture_output=True, text=True)
    report["commit"] = git.stdout.strip() if git.returncode == 0 else None
    summary = output / "summary.json"
    print(f"Input: {counts['ins']} insertions, {counts['del']} deletions, "
          f"{counts['query']} query markers, {len(vertices)} vertices.", flush=True)
    print(f"Output: {output}", flush=True)
    print("Correctness only; checks at markers/final plus any --query-every interval.", flush=True)
    for method in methods:
        path = output / f"dc_queries_{method}.test"
        result = generate(path, method, operations, vertices, checks, final_edges, args.threads)
        result["status"] = "generated_not_executed"
        report["runs"].append(result)
        summary.write_text(json.dumps(report, indent=2) + "\n")
        print(f"{method}: generated {result['updates']} updates, "
              f"{result['directed_pair_queries']} pair queries, "
              f"{result['wcc_calls']} WCC calls.", flush=True)
        if args.run:
            print(f"{method}: running; detailed output: {output / (method + '.log')}", flush=True)
            result.update(run_test(repo, runner, output, method, path))
            summary.write_text(json.dumps(report, indent=2) + "\n")
            print(f"{method}: {result['status'].upper()}", flush=True)
            if result["status"] != "passed":
                print(f"Stopped. Inspect {output / result['log']}", file=sys.stderr)
                return 1
    print(f"Summary: {summary}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, ET.ParseError, ImportError, SyntaxError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
