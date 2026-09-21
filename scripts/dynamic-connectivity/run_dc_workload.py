#!/usr/bin/env python3
"""Generate and optionally run standard Kuzu .test cases from an update trace.

This is a correctness runner, not a performance benchmark. Requires Python 3.9+.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from collections import defaultdict, deque

METHODS = ("dtree", "dtree_csr", "stree", "stree_csr")


def read_workload(path):
    operations, vertices, live = [], set(), {}
    counts = {"ins": 0, "del": 0, "query_markers": 0}
    for line_no, raw in enumerate(path.read_text().splitlines(), 1):
        fields = raw.split()
        if not fields:
            continue
        kind = fields[0]
        if kind == "query" and len(fields) == 2:
            int(fields[1])  # Validate; retain the original token as a marker label.
            operations.append((line_no, kind, fields[1], None))
            counts["query_markers"] += 1
            continue
        if kind not in ("ins", "del") or len(fields) != 3:
            raise ValueError(f"line {line_no}: expected 'ins u v', 'del u v', or 'query label': {raw!r}")
        u, v = map(int, fields[1:])
        if u < 0 or v < 0 or u == v:
            raise ValueError(f"line {line_no}: use distinct non-negative vertex IDs")
        edge = tuple(sorted((u, v)))
        if kind == "ins":
            if edge in live:
                raise ValueError(f"line {line_no}: duplicate live edge {edge}; parallel edges are not supported")
            live[edge] = line_no
        else:
            if edge not in live:
                raise ValueError(f"line {line_no}: deletion of absent edge {edge}")
            del live[edge]
        vertices.update((u, v))
        operations.append((line_no, kind, u, v))
        counts[kind] += 1
    if not vertices:
        raise ValueError("workload contains no updates")
    return operations, sorted(vertices), dict(counts, final_edges=len(live))


def labels_from_bfs(vertices, adjacency):
    labels = {}
    for root in vertices:
        if root in labels:
            continue
        labels[root] = root
        pending = deque([root])
        while pending:
            for nbr in adjacency[pending.popleft()]:
                if nbr not in labels:
                    labels[nbr] = root
                    pending.append(nbr)
    return labels


def choose_pairs(vertices, labels, rng, sample_count, endpoints=None):
    pairs = set()
    def add(u, v):
        if u != v:
            pairs.add(tuple(sorted((u, v))))
    if endpoints is not None:
        add(*endpoints)
    # Include a connected pair and a disconnected pair whenever they exist.
    groups = defaultdict(list)
    for vertex in vertices:
        groups[labels[vertex]].append(vertex)
    for group in groups.values():
        if len(group) > 1:
            add(group[0], group[-1])
            break
    representatives = [group[0] for group in groups.values()]
    if len(representatives) > 1:
        add(representatives[0], representatives[-1])
    for _ in range(sample_count):
        u, v = rng.sample(vertices, 2)
        add(u, v)
    return sorted(pairs)


def generate(path, method, operations, vertices, seed, samples, marker_samples, checkpoint_every):
    offsets = {vid: offset for offset, vid in enumerate(vertices)}
    adjacency = {vid: set() for vid in vertices}
    live = {}
    rng = random.Random(seed)  # Reset for every backend: identical queries and order.
    updates = query_count = checkpoints = 0
    index_name = "dc_" + method
    with path.open("w") as out:
        def comment(message):
            out.write("\n# " + message + "\n")
        def statement(query, rows=None):
            out.write("-STATEMENT " + query + "\n")
            if rows is None:
                out.write("---- ok\n")
            else:
                out.write(f"---- {len(rows)}\n")
                out.write("".join(str(row) + "\n" for row in rows))
        def verify(sample_count, endpoints=None):
            nonlocal query_count
            labels = labels_from_bfs(vertices, adjacency)
            for u, v in choose_pairs(vertices, labels, rng, sample_count, endpoints):
                expected = labels[u] == labels[v]
                # Check both directions. These queries may reroot DTree backends.
                for a, b in ((u, v), (v, u)):
                    statement(f"CALL DYNAMIC_CONNECTIVITY_QUERY('Vertex', {offsets[a]}, {offsets[b]}, '{index_name}') RETURN *;", [expected])
                    query_count += 1
        def edge_count(eid, expected):
            statement(f"MATCH ()-[r:Edges]->() WHERE r.eid = {eid} RETURN count(r);", [expected])
        def total_count():
            statement("MATCH ()-[r:Edges]->() RETURN count(r);", [len(live)])

        out.write(f"-DATASET CSV EMPTY\n\n--\n\n-CASE Replay_{method}\n-LOAD_DYNAMIC_EXTENSION algo\n")
        comment("Correctness only. One disk-backed database and one index in this case.")
        statement("CREATE NODE TABLE Vertex(vID INT64, PRIMARY KEY(vID));")
        statement("CREATE REL TABLE Edges(FROM Vertex TO Vertex, eid INT64);")
        statement("BEGIN TRANSACTION;")
        for vid in vertices:
            statement(f"CREATE (:Vertex {{vID: {vid}}});")
        statement("COMMIT;")
        # Verify every assumed offset before issuing any offset-based index query.
        statement("MATCH (v:Vertex) RETURN v.vID, OFFSET(ID(v)) ORDER BY v.vID;",
                  [f"{vid}|{offsets[vid]}" for vid in vertices])
        statement(f"CALL CREATE_DYNAMIC_CONNECTIVITY_INDEX('Vertex', 'Edges', '{index_name}', '{method}') RETURN *;",
                  [f"Dynamic connectivity index {index_name} has been created with 0 edges."])
        verify(samples)

        for line_no, kind, first, second in operations:
            if kind == "query":
                comment(f"Source line {line_no}: query {first}. Interpreted as a check marker, not a vertex-pair query.")
                total_count()
                verify(marker_samples)
                continue
            u, v = sorted((first, second))
            comment(f"Source line {line_no}: {kind} {first} {second}; update {updates + 1}.")
            statement("BEGIN TRANSACTION;")
            if kind == "ins":
                eid = line_no
                statement(f"MATCH (a:Vertex {{vID: {u}}}), (b:Vertex {{vID: {v}}}) CREATE (a)-[:Edges {{eid: {eid}}}]->(b);")
                live[(u, v)] = eid
                adjacency[u].add(v)
                adjacency[v].add(u)
            else:
                eid = live.pop((u, v))
                # Delete by the precise live relationship identity, even if input endpoints reverse.
                statement(f"MATCH ()-[r:Edges]->() WHERE r.eid = {eid} DELETE r;")
                adjacency[u].remove(v)
                adjacency[v].remove(u)
            statement("COMMIT;")
            updates += 1
            edge_count(eid, 1 if kind == "ins" else 0)
            verify(samples, (u, v))
            if checkpoint_every and updates % checkpoint_every == 0:
                comment(f"Explicit checkpoint after {updates} committed updates.")
                statement("CHECKPOINT;")
                checkpoints += 1
                total_count()
                verify(samples)

        comment("Final checkpoint and verification; this does not reopen the database.")
        statement("CHECKPOINT;")
        checkpoints += 1
        total_count()
        # Verify the exact final edge set, including endpoint IDs, not just its count.
        statement("MATCH (a:Vertex)-[r:Edges]->(b:Vertex) RETURN a.vID, b.vID, r.eid ORDER BY r.eid;",
                  [f"{u}|{v}|{eid}" for (u, v), eid in sorted(live.items(), key=lambda item: item[1])])
        verify(marker_samples)
    return dict(method=method, updates=updates, index_queries=query_count,
                explicit_checkpoints=checkpoints, final_edges=len(live), test_file=path.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--workload", type=Path, default=Path(__file__).with_name("test1745.workload"))
    parser.add_argument("--build", default="build/relwithdebinfo")
    parser.add_argument("--method", choices=("all",) + METHODS, default="all")
    parser.add_argument("--seed", type=int, default=20260921)
    parser.add_argument("--samples", type=int, default=4, help="random pair draws after each committed update")
    parser.add_argument("--marker-samples", type=int, default=64)
    parser.add_argument("--checkpoint-every", type=int, default=200, help="0 disables periodic checkpoints; final checkpoint remains")
    parser.add_argument("--run", action="store_true", help="execute the generated cases through the existing e2e_test runner")
    args = parser.parse_args()
    if min(args.samples, args.marker_samples, args.checkpoint_every) < 0:
        parser.error("sample counts and checkpoint interval must be non-negative")
    repo, workload = args.repo.resolve(), args.workload.resolve()
    if not repo.is_dir():
        parser.error("--repo must name an existing repository directory")
    runner = repo / args.build / "test/runner/e2e_test"
    if args.run and not runner.is_file():
        parser.error(f"missing runner: {runner}; build e2e_test and kuzu_algo_extension first")
    operations, vertices, counts = read_workload(workload)
    parent = repo / "test-results"
    parent.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="dc_workload_", dir=parent))
    methods = METHODS if args.method == "all" else (args.method,)
    print(f"Input: {counts['ins']} insertions, {counts['del']} deletions, {counts['query_markers']} query markers, {len(vertices)} vertices.", flush=True)
    print("Query markers trigger fixed-seed checks; their labels are not treated as vertex IDs or query counts.", flush=True)
    print(f"Output: {output}", flush=True)
    report = dict(workload=str(workload), sha256=hashlib.sha256(workload.read_bytes()).hexdigest(),
                  vertices=len(vertices), counts=counts, seed=args.seed, samples=args.samples,
                  marker_samples=args.marker_samples, checkpoint_every=args.checkpoint_every,
                  query_marker_policy="named correctness-check points; original query semantics unconfirmed",
                  mode="correctness_only", runs=[])
    report_path = output / "summary.json"
    try:
        git = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo, capture_output=True, text=True)
        report["commit"] = git.stdout.strip() if git.returncode == 0 else None
    except FileNotFoundError:
        report["commit"] = None
    for method in methods:
        path = output / f"dc_workload_{method}.test"
        result = generate(path, method, operations, vertices, args.seed, args.samples,
                          args.marker_samples, args.checkpoint_every)
        report["runs"].append(result)
        result["status"] = "generated_not_executed"
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        print(f"{method}: generated {result['updates']} updates and {result['index_queries']} checked index queries.", flush=True)
        if not args.run:
            continue
        env = os.environ.copy()
        env.update(IN_MEM_MODE="false", E2E_REWRITE_TESTS="OFF",
                   E2E_TEST_FILES_DIRECTORY=str(output.relative_to(repo)))
        for key in tuple(env):
            if key.startswith("GTEST_"):
                del env[key]
        log, xml = output / f"{method}.log", output / f"{method}.xml"
        print(f"{method}: running; detailed output goes to {log}", flush=True)
        with log.open("w") as stream:
            completed = subprocess.run([str(runner), path.name, f"--gtest_output=xml:{xml}"],
                                       cwd=repo, env=env, stdout=stream, stderr=subprocess.STDOUT)
        passed = False
        if xml.exists():
            root = ET.parse(xml).getroot()
            passed = (root.get("tests") == "1"
                      and all(root.get(key, "0") == "0" for key in ("failures", "errors", "disabled", "skipped"))
                      and not root.findall(".//skipped"))
        result.update(status="passed" if completed.returncode == 0 and passed else "failed",
                      exit_code=completed.returncode, log=log.name, xml=xml.name)
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        print(f"{method}: {result['status'].upper()}", flush=True)
        if result["status"] != "passed":
            print(f"Stopped. Share {log} for diagnosis.", file=sys.stderr)
            return 1
    print(f"Summary: {report_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, ET.ParseError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
