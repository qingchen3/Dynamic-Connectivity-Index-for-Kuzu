#!/usr/bin/env python3
"""Compare per-neighbor scan preparation with per-deletion scan reuse in Kuzu.

Requires the temporary DC_REUSE_SCAN_STATE switch in commitRelDelete(), and
the DC_STORAGE_SEARCH / DC_SCAN_PHASE diagnostic lines. The workload runs
against the database-backed index through the existing e2e runner.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path


METHODS = ("dtree_csr", "stree_csr")
LINE = re.compile(r"^DC_(STORAGE_SEARCH|SCAN_PHASE) method=(\S+) (.*)$")
FIELD = re.compile(r"(\w+)=(\d+)")


def statement(query: str, result: str = "ok") -> list[str]:
    return [f"-STATEMENT {query}", f"---- {result}"]


def make_fixture(gadgets: int) -> str:
    """Create disjoint eight-node paths, then cut their middle edges."""
    n = gadgets * 8
    out = [
        "-DATASET CSV EMPTY",
        "",
        "--",
        "",
        "-CASE DCScanStateReuse",
        "-LOAD_DYNAMIC_EXTENSION algo",
        *statement("CREATE NODE TABLE Person(id INT64 PRIMARY KEY);"),
        *statement("CREATE REL TABLE Knows(FROM Person TO Person, eid INT64);"),
        *statement(f"UNWIND range(0, {n - 1}) AS i CREATE (:Person {{id: i}});"),
        *statement("MATCH (p:Person) RETURN count(p);", f"1\n{n}"),
    ]

    edges = []
    for i in range(gadgets):
        a = 8 * i
        edges.extend((a + j, a + j + 1) for j in range(7))

    for eid, (u, v) in enumerate(edges):
        out.extend(statement(
            f"MATCH (a:Person {{id: {u}}}), (b:Person {{id: {v}}}) "
            f"CREATE (a)-[:Knows {{eid: {eid}}}]->(b);"
        ))

    out.extend(statement("MATCH ()-[r:Knows]->() RETURN count(r);", f"1\n{len(edges)}"))
    for method in METHODS:
        out.extend(statement(
            f"CALL CREATE_DYNAMIC_CONNECTIVITY_INDEX('Person', 'Knows', "
            f"'dc_{method}', '{method}') RETURN *;",
            f"1\nDynamic connectivity index dc_{method} has been created "
            f"with {len(edges)} edges.",
        ))

    for i in range(gadgets):
        eid = 7 * i + 3
        out.extend(statement(f"MATCH ()-[r:Knows]->() WHERE r.eid = {eid} DELETE r;"))

    out.extend(statement("MATCH ()-[r:Knows]->() RETURN count(r);", f"1\n{len(edges) - gadgets}"))
    for method in METHODS:
        out.extend(statement(
            f"CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 3, 4, 'dc_{method}') RETURN *;",
            "1\nFalse",
        ))
    return "\n".join(out) + "\n"


def parse_log(path: Path) -> dict[str, list[dict[str, int]]]:
    by_kind = defaultdict(lambda: defaultdict(list))
    content = path.read_text(errors="replace")
    if "[  PASSED  ] 1 test." not in content and "[  PASSED  ] 1 tests." not in content:
        raise RuntimeError(f"e2e test did not report one passing test: {path}")
    for line in content.splitlines():
        match = LINE.match(line)
        if match is None:
            continue
        kind, method, rest = match.groups()
        if method in METHODS:
            by_kind[method][kind].append({key: int(value) for key, value in FIELD.findall(rest)})

    result = {}
    for method in METHODS:
        searches = by_kind[method]["STORAGE_SEARCH"]
        phases = by_kind[method]["SCAN_PHASE"]
        if not searches or len(searches) != len(phases):
            raise RuntimeError(f"missing or unpaired diagnostic lines for {method}: {path}")
        result[method] = [dict(search=search, phase=phase)
                          for search, phase in zip(searches, phases)]
    return result


def signature(events: list[dict]) -> list[tuple[int, ...]]:
    keys = ("found", "calls", "returned", "candidates")
    return [tuple(event["search"][key] for key in keys) for event in events]


def validate(baseline: dict, reuse: dict, gadgets: int) -> None:
    for method in METHODS:
        before, after = baseline[method], reuse[method]
        if signature(before) != signature(after):
            raise RuntimeError(f"different replacement-search trace for {method}; do not compare timings")
        if len(before) != gadgets:
            raise RuntimeError(f"too few tree-edge searches for {method}: {len(before)}")
        if not any(e["search"]["calls"] > 1 for e in before):
            raise RuntimeError(f"workload did not test multi-neighbor reuse for {method}")
        for mode, events in (("baseline", before), ("reuse", after)):
            for event in events:
                calls = event["search"]["calls"]
                expected = calls if mode == "baseline" else int(calls > 0)
                if event["phase"]["prepare_calls"] != expected:
                    raise RuntimeError(
                        f"{mode} {method}: expected {expected} preparations for "
                        f"{calls} neighbor calls, got {event['phase']['prepare_calls']}"
                    )


def totals(events: list[dict]) -> dict[str, int]:
    return {
        "searches": len(events),
        "found": sum(e["search"]["found"] for e in events),
        "neighbor_calls": sum(e["search"]["calls"] for e in events),
        "search_ns": sum(e["search"]["search_ns"] for e in events),
        "get_neighbors_ns": sum(e["search"]["get_neighbors_ns"] for e in events),
        "prepare_calls": sum(e["phase"]["prepare_calls"] for e in events),
        "prepare_ns": sum(e["phase"]["prepare_ns"] for e in events),
        "forward_ns": sum(e["phase"]["forward_ns"] for e in events),
        "backward_ns": sum(e["phase"]["backward_ns"] for e in events),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--runner", type=Path, default=Path("build/relwithdebinfo/test/runner/e2e_test"))
    parser.add_argument("--gadgets", type=int, default=128)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if args.gadgets < 2 or args.repetitions < 1:
        parser.error("need at least two gadgets and one repetition")

    repo = args.repo.resolve()
    runner = (repo / args.runner).resolve()
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=True)
    else:
        results = repo / "test-results"
        results.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="dc_scan_reuse_", dir=results))
    fixture = output / "dc_scan_reuse.test"
    fixture.write_text(make_fixture(args.gadgets))
    print(f"Fixture: {fixture} ({args.gadgets} paths, {args.gadgets} middle-edge deletions)")
    if args.generate_only:
        return 0
    if not runner.is_file():
        parser.error(f"e2e runner not found: {runner}")

    parsed = {}
    for repeat in range(args.repetitions):
        # Alternate order to reduce systematic warm-cache and thermal effects.
        modes = ("baseline", "reuse") if repeat % 2 == 0 else ("reuse", "baseline")
        for mode in modes:
            log = output / f"{repeat + 1:02d}.{mode}.log"
            env = os.environ.copy()
            env.update({
                "IN_MEM_MODE": "false",
                "E2E_REWRITE_TESTS": "OFF",
                "E2E_TEST_FILES_DIRECTORY": str(output),
                "DC_REUSE_SCAN_STATE": "0" if mode == "baseline" else "1",
            })
            print(f"Run {repeat + 1}/{args.repetitions} {mode}: {log}", flush=True)
            with log.open("w") as stream:
                process = subprocess.run(
                    [str(runner), fixture.name], cwd=repo, env=env,
                    stdout=stream, stderr=subprocess.STDOUT,
                    check=False,
                )
            if process.returncode != 0:
                raise RuntimeError(f"e2e runner failed ({process.returncode}); inspect {log}")
            parsed[repeat, mode] = parse_log(log)
        validate(parsed[repeat, "baseline"], parsed[repeat, "reuse"], args.gadgets)

    for method in METHODS:
        reference = signature(parsed[0, "baseline"][method])
        for repeat in range(1, args.repetitions):
            if signature(parsed[repeat, "baseline"][method]) != reference:
                raise RuntimeError(
                    f"search trace changed across repetitions for {method}; "
                    "timings are not comparable"
                )

    summary = {"gadgets": args.gadgets, "repetitions": args.repetitions, "methods": {}}
    for method in METHODS:
        summary["methods"][method] = {}
        per_mode = {}
        for mode in ("baseline", "reuse"):
            runs = [totals(parsed[i, mode][method]) for i in range(args.repetitions)]
            summary["methods"][method][mode] = runs
            per_mode[mode] = statistics.median(
                run["search_ns"] / run["searches"] / 1000 for run in runs
            )
            print(f"{method:10s} {mode:8s}: "
                  f"median search {per_mode[mode]:.2f} us, "
                  f"searches/run {runs[0]['searches']}, "
                  f"found/run {runs[0]['found']}, "
                  f"preparations/run {runs[0]['prepare_calls']}, "
                  f"neighbor calls/run {runs[0]['neighbor_calls']}")
        ratio = per_mode["baseline"] / per_mode["reuse"] if per_mode["reuse"] else float("nan")
        print(f"{method:10s} search ratio baseline/reuse: {ratio:.2f}x")

    summary_path = output / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    print(f"Summary: {summary_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
