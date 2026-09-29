#!/usr/bin/env python3
"""Memory footprint of the connectivity backends, test case by test case.

Rewrite of Imp_bench_dyn's evaluate_memory_footprints.py against the Kuzu native
index. The experiment is the same one: for each test case and each backend, load
the whole graph from an edge list with no deletions, then report the footprint
split into vertex space and edge space, in GB. Requires Python 3.9+.

    python3 compare_memory_footprints.py usa youtube stackoverflow
    python3 compare_memory_footprints.py --dataset examples/fb-forum.edges
    python3 compare_memory_footprints.py --dataset wiki.workload --label WI

Positional arguments are test case names, resolved under --datasets (default
<repo>/datasets), exactly as the Python reads datasets/<test_case>. --dataset
takes an explicit path instead.

Either accepts a plain edge list, "102 108" per line, or a recorded trace,
"ins 102 108 1080493920", whose timestamps are ignored and whose del lines are
counted and skipped -- so pointing --dataset at a workload loads every edge it
ever inserts. --workload instead applies those deletions, measuring the graph
that survives them. That is a different quantity, so the mode and the number of
deletions ignored are recorded alongside every figure.

Each run's numbers are checked before they are reported. The benchmark measures
the footprint twice over -- once by walking the index's structures, once by
counting what global operator new handed out and never got back -- and exits
non-zero unless the two agree to the byte. Runs are not retried or averaged:
allocation is deterministic here, so a second run of the same input produces the
same bytes, and a disagreement is a defect in the accounting rather than noise.

Two files come out per test case, both under <repo>/res/memory:

    <testcase>_sum.csv      method,s -- total GB per backend, upserted in place,
                            the layout output_memory_footprint_sum writes
    <testcase>_detail.csv   the split, the counts and the cross-check

The Python original also has two faults worth knowing about if you compare
numbers against it. It reads edge_num at the top of the loop body before
anything assigns it, so it raises NameError on the first iteration. And its
path_graph special case reads `if 'path_graph' and method == 'Dtree'`, where the
non-empty string is always truthy, so for Dtree every test case is replaced by
the synthetic 10M-node path graph and the real measurement is skipped.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

METHODS = ("dtree", "dtree_csr", "stree", "stree_csr")
# Each backend and the CSR variant that drops its nte set.
PAIRS = (("dtree", "dtree_csr"), ("stree", "stree_csr"))
BENCH_RELATIVE = "extension/algo/test/dynamic_connectivity_memory_bench"

RESULT_RE = re.compile(r"^RESULT (.*)$", re.MULTILINE)
INT_FIELDS = ("nodes", "tree_edges", "nte_edges", "space_n", "space_e", "bytes",
              "alloc_bytes", "delta_bytes", "allocations", "ops", "ignored_deletions")
FLOAT_FIELDS = ("gb", "space_n_gb", "space_e_gb", "bytes_per_node")


class RunFailed(Exception):
    """A backend did not produce a usable measurement."""


def sha256_of(path):
    """Digest the input in chunks: a recorded graph can be far larger than RAM."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(bench, method, case, log_path, edges, seed, dedup, verbose):
    """Measure one backend on one test case, writing its full output to log_path."""
    cmd = [str(bench), method, case["source"], f"--testcase={case['name']}"]
    if edges:
        cmd.append(f"--edges={edges}")
    if case["mode"] == "random":
        cmd.append(f"--seed={seed}")
    if dedup and case["mode"] == "dataset":
        cmd.append("--dedup")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    log_path.write_text(proc.stdout + proc.stderr)
    if verbose:
        print(proc.stdout, end="", flush=True)
    if proc.returncode != 0:
        detail = proc.stderr.strip() or "see the log"
        # Exit 1 is the benchmark's own verdict that the two measurements
        # disagreed; anything else is a failure to run at all.
        if proc.returncode == 1:
            detail = "the walk and the allocator disagreed; " + detail
        raise RunFailed(f"{method} on {case['name']}: exit {proc.returncode} ({detail})")
    match = RESULT_RE.search(proc.stdout)
    if match is None:
        raise RunFailed(f"{method} on {case['name']}: no RESULT line")
    fields = dict(token.split("=", 1) for token in match.group(1).split())
    missing = [key for key in INT_FIELDS + FLOAT_FIELDS if key not in fields]
    if missing:
        raise RunFailed(f"{method} on {case['name']}: RESULT is missing {', '.join(missing)}")
    for key in INT_FIELDS:
        fields[key] = int(fields[key])
    for key in FLOAT_FIELDS:
        fields[key] = float(fields[key])
    if fields["delta_bytes"] != 0:
        # Belt and braces: the benchmark already fails on this, so reaching here
        # would mean its exit code and its own report disagree.
        raise RunFailed(f"{method} on {case['name']}: walk is off by "
                        f"{fields['delta_bytes']} bytes")
    return fields


def human(nbytes):
    value = float(nbytes)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024 or unit == "GB":
            return f"{value:,.1f} {unit}"
        value /= 1024
    return ""


SUM_HEADER = ["method", "s"]  # s: space_n + space_e, in GB
DETAIL_HEADER = ["testcase", "mode", "method", "nodes", "tree_edges", "nte_edges",
                 "space_n", "space_e", "bytes", "space_n_gb", "space_e_gb", "gb",
                 "bytes_per_node", "allocations", "alloc_bytes", "delta_bytes",
                 "ignored_deletions"]


def upsert_rows(path, header, new_rows, key_index):
    """Merge rows into a CSV by key, leaving rows for other keys untouched.

    Follows output_memory_footprint_sum: a result file accumulates one row per
    backend across separate invocations, so re-measuring one backend must not
    drop the others. Staged in a temporary file beside the original and moved
    over it, so an interrupted write cannot truncate results already there.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    incoming = {str(row[key_index]): [str(field) for field in row] for row in new_rows}
    merged, replaced = [], set()
    if path.exists():
        with path.open(newline="") as handle:
            for row in csv.reader(handle):
                key = row[key_index] if len(row) > key_index else None
                if key in incoming:
                    merged.append(incoming[key])
                    replaced.add(key)
                else:
                    merged.append(row)
    else:
        merged.append([str(field) for field in header])
    for key, row in incoming.items():
        if key not in replaced:
            merged.append(row)
    with tempfile.NamedTemporaryFile("w", newline="", dir=path.parent,
                                     delete=False) as handle:
        csv.writer(handle).writerows(merged)
        staged = handle.name
    shutil.move(staged, path)


def detail_row(case, method, r):
    return [case["name"], case["mode"], method, r["nodes"], r["tree_edges"], r["nte_edges"],
            r["space_n"], r["space_e"], r["bytes"], r["space_n_gb"], r["space_e_gb"],
            r["gb"], r["bytes_per_node"], r["allocations"], r["alloc_bytes"],
            r["delta_bytes"], r["ignored_deletions"]]


def report_case(case, results, methods):
    any_result = next(iter(results.values()))
    print(f"\n{case['name']}  ({case['mode']}, {any_result['nodes']:,} vertices, "
          f"{any_result['tree_edges']:,} tree edges, {any_result['ops']:,} updates)")
    header = (f"{'backend':12s} {'space_n GB':>12s} {'space_e GB':>12s} {'total GB':>12s} "
              f"{'total':>12s} {'B/node':>9s} {'nte edges':>11s}")
    print(header)
    print("-" * len(header))
    for method in methods:
        if method not in results:
            continue
        r = results[method]
        print(f"{method:12s} {r['space_n_gb']:>12.6f} {r['space_e_gb']:>12.6f} "
              f"{r['gb']:>12.6f} {human(r['bytes']):>12s} {r['bytes_per_node']:>9.1f} "
              f"{r['nte_edges']:>11,}")
    for base, csr in PAIRS:
        if base in results and csr in results and results[csr]["bytes"]:
            base_bytes, csr_bytes = results[base]["bytes"], results[csr]["bytes"]
            saved = 100.0 * (base_bytes - csr_bytes) / base_bytes
            print(f"  {base} / {csr}: {base_bytes / csr_bytes:.2f}x  "
                  f"({saved:.1f}% less for {csr})")


def collect_cases(args, parser):
    """Resolve every input into {name, mode, source, path}."""
    cases = []
    for name in args.testcases:
        path = (args.datasets / name).resolve()
        if not path.is_file():
            parser.error(f"no dataset for test case {name!r}: {path}")
        cases.append({"name": name, "mode": "dataset", "source": f"--dataset={path}",
                      "path": path})
    for raw in args.dataset:
        path = raw.resolve()
        if not path.is_file():
            parser.error(f"dataset not found: {path}")
        cases.append({"name": path.name, "mode": "dataset", "source": f"--dataset={path}",
                      "path": path})
    for raw in args.workload:
        path = raw.resolve()
        if not path.is_file():
            parser.error(f"workload not found: {path}")
        cases.append({"name": path.name, "mode": "workload", "source": f"--workload={path}",
                      "path": path})
    for spec in args.random:
        cases.append({"name": f"random_{spec.replace(',', '_')}", "mode": "random",
                      "source": f"--random={spec}", "path": None})
    if not cases:
        parser.error("give at least one test case, --dataset, --workload or --random")
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("testcases", nargs="*", metavar="TESTCASE",
        help="test case names, resolved under --datasets")
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--build", default="build/relwithdebinfo")
    parser.add_argument("--bench", type=Path, default=None,
        help=f"benchmark binary; default <repo>/<build>/{BENCH_RELATIVE}")
    parser.add_argument("--datasets", type=Path, default=None,
        help="directory holding the edge lists; default <repo>/datasets")
    parser.add_argument("--dataset", type=Path, action="append", default=[],
        help="an edge list or a trace whose del lines are skipped; repeatable")
    parser.add_argument("--workload", type=Path, action="append", default=[],
        help="a trace, measured after its deletions are applied; repeatable")
    parser.add_argument("--dedup", action="store_true",
        help="filter repeated edges rather than letting the backend refuse them")
    parser.add_argument("--random", action="append", default=[], metavar="NODES,OPS,BIAS",
        help="a synthetic graph; repeatable")
    parser.add_argument("--methods", nargs="+", default=list(METHODS), choices=METHODS)
    parser.add_argument("--edges", type=int, default=0,
        help="stop after N edges, as the Python's edge_num does; 0 means all")
    parser.add_argument("--seed", type=int, default=20260917, help="--random only")
    parser.add_argument("--res", type=Path, default=None,
        help="where the CSVs go; default <repo>/res/memory")
    parser.add_argument("--keep-going", action="store_true",
        help="carry on to the next backend after a failed run")
    parser.add_argument("--verbose", action="store_true",
        help="also print each run's allocation-size breakdown")
    args = parser.parse_args()

    repo = args.repo.resolve()
    if not repo.is_dir():
        parser.error("--repo must name an existing repository directory")
    bench = (args.bench or repo / args.build / BENCH_RELATIVE).resolve()
    if not bench.is_file():
        parser.error(f"missing benchmark: {bench}; build dynamic_connectivity_memory_bench first")
    if args.datasets is None:
        args.datasets = repo / "datasets"
    if args.edges < 0:
        parser.error("--edges must not be negative")
    res = (args.res or repo / "res" / "memory").resolve()
    cases = collect_cases(args, parser)

    parent = repo / "test-results"
    parent.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="dc_memory_", dir=parent))
    print(f"Test cases: {', '.join(case['name'] for case in cases)}", flush=True)
    print(f"Backends:   {', '.join(args.methods)}", flush=True)
    print(f"Output:     {output}", flush=True)

    report = dict(mode="footprint", bench=str(bench), res=str(res), edges=args.edges,
                  methods=list(args.methods), cases=[])
    try:
        git = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo, capture_output=True,
                             text=True)
        report["commit"] = git.stdout.strip() if git.returncode == 0 else None
    except FileNotFoundError:
        report["commit"] = None
    report_path = output / "summary.json"

    def save():
        report_path.write_text(json.dumps(report, indent=2) + "\n")

    failures = []
    for case in cases:
        entry = dict(name=case["name"], mode=case["mode"],
                     source=str(case["path"]) if case["path"] else case["source"],
                     sha256=sha256_of(case["path"]) if case["path"] else None, runs={})
        report["cases"].append(entry)
        save()
        results = {}
        for method in args.methods:
            log = output / f"{case['name']}.{method}.log"
            print(f"  {case['name']} / {method}: measuring -> {log.name}", flush=True)
            try:
                fields = run(bench, method, case, log, args.edges, args.seed,
                             args.dedup, args.verbose)
            except RunFailed as error:
                entry["runs"][method] = {"status": "failed", "log": log.name,
                                         "detail": str(error)}
                save()
                print(f"    FAILED: {error}", file=sys.stderr)
                failures.append(str(error))
                if not args.keep_going:
                    print(f"Stopped. Share {log} for diagnosis.", file=sys.stderr)
                    return 1
                continue
            results[method] = fields
            entry["runs"][method] = dict(fields, status="measured", log=log.name)
            # Written per backend rather than per test case, so an interrupted
            # sweep still leaves every completed measurement on disk.
            upsert_rows(res / f"{case['name']}_sum.csv", SUM_HEADER,
                        [[method, fields["gb"]]], 0)
            upsert_rows(res / f"{case['name']}_detail.csv", DETAIL_HEADER,
                        [detail_row(case, method, fields)], 2)
            save()
            print(f"    {human(fields['bytes'])}  "
                  f"(space_n {human(fields['space_n'])}, space_e {human(fields['space_e'])})",
                  flush=True)
        if not results:
            continue
        report_case(case, results, args.methods)
        entry["detail_csv"] = str(res / f"{case['name']}_detail.csv")
        entry["sum_csv"] = str(res / f"{case['name']}_sum.csv")
        entry["ratios"] = {f"{base}/{csr}": results[base]["bytes"] / results[csr]["bytes"]
                           for base, csr in PAIRS
                           if base in results and csr in results and results[csr]["bytes"]}
        save()

    print(f"\nCSVs:    {res}")
    print(f"Summary: {report_path}")
    if failures:
        print(f"\n{len(failures)} run(s) failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
