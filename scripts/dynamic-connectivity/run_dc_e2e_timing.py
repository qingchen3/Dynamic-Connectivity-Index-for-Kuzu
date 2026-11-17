#!/usr/bin/env python3
"""Generate and time database-backed .test replay for four indexes, BFS and WCC."""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(query):
    return hashlib.sha256(query.encode()).hexdigest()


def manifest(test_path, destination):
    """One record per explicit -STATEMENT; extension loading is runner-managed."""
    update_kind = None
    check_id = 0
    count = 0
    with test_path.open() as source, destination.open('w') as out:
        for line in source:
            if line.startswith('# DC_CHECK '):
                check_id += 1
            match = re.match(r'# Source line (\d+): (ins|del) ', line)
            if match:
                update_kind = match[2]
            if not line.startswith('-STATEMENT '):
                continue
            query = line[len('-STATEMENT '):].strip()
            if query == 'CHECKPOINT;':
                phase = 'checkpoint'
                update_kind = None
            elif query == 'BEGIN TRANSACTION;' and update_kind:
                phase = update_kind + '_begin'
            elif query == 'COMMIT;' and update_kind:
                phase = update_kind + '_commit'
            elif ' CREATE (a)-[:Edges ' in query:
                phase = 'ins_dml'
            elif query.endswith(' DELETE r;'):
                phase = 'del_dml'
            elif query.startswith('CALL DYNAMIC_CONNECTIVITY_QUERY') or '* SHORTEST ' in query:
                phase = 'pair_query'
            elif query.startswith('CALL WEAKLY_CONNECTED_COMPONENTS'):
                phase = 'wcc_partition_query'
            elif query.startswith('MATCH '):
                phase = 'validation'
            else:
                phase = 'setup'
            count += 1
            out.write(json.dumps({'statement': count, 'query_sha256': digest(query),
                                  'phase': phase, 'check_id': check_id}) + '\n')
    return count


def stats(values):
    if not values:
        return None
    ordered = sorted(values)
    return {'count': len(values), 'total_us': sum(values),
            'mean_us': statistics.mean(values), 'median_us': statistics.median(values),
            'p90_us': ordered[math.ceil(.90 * len(ordered)) - 1],
            'p99_us': ordered[math.ceil(.99 * len(ordered)) - 1], 'max_us': ordered[-1]}


def parse_timings(log, manifest_path, csv_path):
    import csv
    phases = {}
    batches = {}
    expected = iter(json.loads(line) for line in manifest_path.open())
    next_expected = next(expected, None)
    current_query = None
    measured = 0
    with csv_path.open('w') as out:
        writer = csv.writer(out)
        writer.writerow(['statement', 'phase', 'check_id', 'compile_us', 'execute_us', 'wall_us'])
        for line in log.open():
            if 'QUERY: ' in line:
                current_query = line.split('QUERY: ', 1)[1].strip()
            if not line.startswith('DC_STATEMENT_TIMING '):
                continue
            fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
            if fields.get('success') != '1':
                raise ValueError(f'Failed statement in {log}: {current_query}')
            # LOAD EXTENSION is inserted by the runner rather than the fixture.
            if current_query and current_query.startswith('LOAD EXTENSION '):
                continue
            if next_expected is None or digest(current_query or '') != next_expected['query_sha256']:
                raise ValueError(f'Timing/fixture alignment failed in {log}: {current_query}')
            values = [float(fields[key]) * 1000 for key in ('compile_ms', 'execute_ms', 'wall_ms')]
            if not all(math.isfinite(x) and x >= 0 for x in values):
                raise ValueError('Invalid timing value')
            phase = next_expected['phase']
            bucket = phases.setdefault(phase, {key: [] for key in ('compile', 'execute', 'wall')})
            for key, value in zip(bucket, values):
                bucket[key].append(value)
            if phase in ('pair_query', 'wcc_partition_query'):
                batch = batches.setdefault(next_expected['check_id'], [0., 0., 0.])
                for index, value in enumerate(values):
                    batch[index] += value
            writer.writerow([next_expected['statement'], phase, next_expected['check_id'], *values])
            measured += 1
            next_expected = next(expected, None)
    if next_expected is not None or measured == 0:
        raise ValueError(f'Missing timing records in {log}; install timers and rebuild e2e_test')
    report = {phase: {key: stats(values) for key, values in bucket.items()}
              for phase, bucket in phases.items()}
    if batches:
        report['query_batch'] = {
            metric: stats([batch[index] for batch in batches.values()])
            for index, metric in enumerate(('compile', 'execute', 'wall'))}
    # Paired BEGIN+DML+COMMIT, retaining transaction boundaries in the raw CSV.
    for kind in ('ins', 'del'):
        keys = [kind + suffix for suffix in ('_begin', '_dml', '_commit')]
        if all(key in phases for key in keys):
            lengths = {len(phases[key]['execute']) for key in keys}
            if len(lengths) != 1:
                raise ValueError(f'Incomplete {kind} transactions')
            report[kind + '_transaction'] = {
                metric: stats([sum(xs) for xs in zip(*(phases[key][metric] for key in keys))])
                for metric in ('compile', 'execute', 'wall')}
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, default=Path('.'))
    p.add_argument('--workload', type=Path, required=True)
    p.add_argument('--methods', nargs='+', default=['all'])
    p.add_argument('--build', default='build/relwithdebinfo')
    p.add_argument('--workers', type=int, default=1)
    p.add_argument('--worker-id', type=int, default=1)
    p.add_argument('--repetitions', type=int, default=1)
    p.add_argument('--threads', type=int, default=1)
    p.add_argument('--marker-samples', type=int, default=2)
    p.add_argument('--query-every', type=int, default=0)
    p.add_argument('--max-deletions', type=int, default=0)
    p.add_argument('--seed', type=int, default=20260921)
    p.add_argument('--order-seed', type=int, default=20261009)
    p.add_argument('--run', action='store_true')
    args = p.parse_args()
    repo = args.repo.resolve()
    directory = repo / 'scripts/dynamic-connectivity'
    baseline_path = directory / 'run_dc_query_baselines.py'
    base = load(baseline_path, 'dc_baselines')
    helpers_path = directory / 'run_dc_workload.py'
    helpers = base.load_helpers(helpers_path)
    methods = base.METHODS if args.methods == ['all'] else tuple(dict.fromkeys(args.methods))
    if not methods or any(m not in base.METHODS for m in methods):
        p.error('choose all or dtree dtree_csr stree stree_csr bfs wcc')
    if args.workers < 1 or not 1 <= args.worker_id <= args.workers or args.repetitions < args.workers:
        p.error('require 1 <= worker-id <= workers <= repetitions')
    if args.threads < 1 or min(args.marker_samples, args.query_every, args.max_deletions) < 0:
        p.error('invalid thread/count argument')
    runner = repo / args.build / 'test/runner/e2e_test'
    extension = repo / 'extension/algo/build/libalgo.kuzu_extension'
    if args.run and (not runner.is_file() or not extension.is_file()):
        p.error('build e2e_test and kuzu_algo_extension first')
    workload = args.workload.resolve()
    operations, _, _ = helpers.read_workload(workload)
    operations = base.trim_operations(operations, args.max_deletions)
    vertices = sorted({v for _, kind, a, b in operations if kind != 'query' for v in (a, b)})
    if len(vertices) < 2:
        p.error('at least two vertices required')
    checks, final_edges = base.make_plan(helpers, operations, vertices, args.seed,
                                        args.marker_samples, args.query_every)
    parent = repo / 'test-results'
    parent.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix=f'dc_timing_worker{args.worker_id}_', dir=parent))
    schedule = base.make_schedule(methods, args.repetitions, args.workers, args.order_seed)
    plan = output / 'query_pairs.jsonl'
    with plan.open('w') as out:
        for check in checks + [dict(checks[-1], reason='final_after_checkpoint')]:
            out.write(json.dumps({k: v for k, v in check.items() if k != 'partition_rows'}, sort_keys=True) + '\n')
    git = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=repo, capture_output=True, text=True)
    report = {'schema_version': 1, 'mode': 'e2e_statement_timing',
              'machine': base.machine_metadata(), 'worker_id': args.worker_id,
              'workers': args.workers, 'schedule': schedule, 'arguments': vars(args).copy(),
              'commit': git.stdout.strip() if git.returncode == 0 else None,
              'workload_sha256': base.file_sha256(workload),
              'query_pairs_sha256': base.file_sha256(plan),
              'script_sha256': base.file_sha256(Path(__file__)),
              'baseline_generator_sha256': base.file_sha256(baseline_path),
              'workload_helper_sha256': base.file_sha256(helpers_path),
              'binaries': {name: base.file_sha256(path) if path.is_file() else None
                           for name, path in [('e2e_test', runner), ('algo_extension', extension)]},
              'scan_state_reuse': True, 'scan_verification': False,
              'wcc_policy': 'full WCC plus canonicalization and ORDER BY; no cached pair lookup',
              'units': 'microseconds', 'runs': []}
    report['arguments'] = {k: str(v) if isinstance(v, Path) else v for k, v in report['arguments'].items()}
    summary = output / 'summary.json'
    def save():
        summary.write_text(json.dumps(report, indent=2) + '\n')
    save()
    print(f'Output: {output}', flush=True)
    for entry in schedule:
        if entry['worker_id'] != args.worker_id:
            continue
        folder = output / f"rep_{entry['repetition']:03d}"
        folder.mkdir()
        for method in entry['methods']:
            test = folder / f'dc_timing_{method}.test'
            result = base.generate(test, method, operations, vertices, checks, final_edges, args.threads)
            # Keep generated statements/expected answers unchanged; only change the annotation.
            text = test.read_text().replace('# Correctness test, not a performance benchmark.',
                                             '# Opt-in statement timing; expected results checked outside engine timers.')
            test.write_text(text)
            metadata = folder / f'{method}.statements.jsonl'
            result.update(repetition=entry['repetition'], status='generated_not_executed',
                          statement_count=manifest(test, metadata),
                          fixture_sha256=base.file_sha256(test),
                          test_file=str(test.relative_to(output)))
            report['runs'].append(result)
            save()
            print(f"rep={entry['repetition']} method={method} statements={result['statement_count']}", flush=True)
            if args.run:
                os.environ.update(DC_E2E_TIMING='1', DC_PRINT_SEARCH_DIAGNOSTICS='0')
                outcome = base.run_test(repo, runner, folder, method, test)
                result.update(outcome)
                result['log'] = str((folder / outcome['log']).relative_to(output))
                result['xml'] = str((folder / outcome['xml']).relative_to(output))
                save()
                if outcome['status'] != 'passed':
                    raise ValueError(f"Test failed; inspect {output / result['log']}")
                csv_path = folder / f'{method}.timings.csv'
                result['timings'] = parse_timings(folder / outcome['log'], metadata, csv_path)
                result['timings_csv'] = str(csv_path.relative_to(output))
                result['status'] = 'passed_and_timed'
                save()
                for phase in ('ins_transaction', 'del_transaction', 'pair_query', 'wcc_partition_query', 'query_batch'):
                    if phase in result['timings']:
                        timing = result['timings'][phase]['execute']
                        print(f"  {phase}: n={timing['count']} mean_us={timing['mean_us']:.3f} median_us={timing['median_us']:.3f}", flush=True)
    print(f'Summary: {summary}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, ImportError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(2)
