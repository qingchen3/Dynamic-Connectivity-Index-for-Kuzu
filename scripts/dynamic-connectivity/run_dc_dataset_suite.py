#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile

DATASETS = ('wiki', 'enron', 'star_graph', 'complete_graph', 'power_law',
            'youtube', 'stackoverflow', 'usa')
METHODS = ('dtree', 'dtree_csr', 'stree', 'stree_csr', 'bfs', 'wcc')


def sha256(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def save_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def prepare_trace(source, destination, dedup=False, max_updates=0, max_deletions=0):
    """Stream a trace; SQLite tracks live unique undirected edges on disk.

    Dedup uses SIMPLE GRAPH semantics: duplicate insertion and absent deletion
    are no-ops. Self-loops are connectivity no-ops. Query markers are retained.
    Blank lines replace skipped records to preserve source line numbers/eids.
    A prefix limit is applied to EFFECTIVE updates, without rewriting the
    generator's own --max-deletions policy. No temporary DB is retained.
    """
    counts = dict(insertions=0, deletions=0, markers=0, duplicate_insertions=0,
                  absent_deletions=0, self_loops=0, source_lines=0,
                  truncated=False, simple_graph=dedup)
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, db_name = tempfile.mkstemp(prefix='dc_live_', suffix='.sqlite',
                                   dir=destination.parent)
    os.close(fd)
    connection = sqlite3.connect(db_name)
    partial = destination.with_suffix(destination.suffix + '.tmp')
    try:
        connection.execute('PRAGMA journal_mode=OFF')
        connection.execute('PRAGMA synchronous=OFF')
        connection.execute('PRAGMA cache_size=-32768')
        connection.execute('CREATE TABLE live (u INTEGER, v INTEGER, '
                           'PRIMARY KEY(u,v)) WITHOUT ROWID')
        with source.open() as incoming, partial.open('w') as outgoing:
            for line_no, raw in enumerate(incoming, 1):
                fields = raw.split()
                counts['source_lines'] = line_no
                if not fields:
                    outgoing.write('\n')
                    continue
                kind = fields[0].lower()
                if kind == 'query':
                    if len(fields) != 2:
                        raise ValueError(f'{source}:{line_no}: malformed query marker')
                    int(fields[1])
                    counts['markers'] += 1
                    outgoing.write('query ' + fields[1] + '\n')
                    continue
                if kind not in ('ins', 'del') or len(fields) not in (3, 4):
                    raise ValueError(f'{source}:{line_no}: malformed update')
                u, v = map(int, fields[1:3])
                if min(u, v) < 0 or max(u, v) > 9223372036854775807:
                    raise ValueError(f'{source}:{line_no}: vertex ID outside nonnegative INT64')
                if len(fields) == 4:
                    int(fields[3])
                skip = False
                if u == v:
                    if not dedup:
                        raise ValueError(f'{source}:{line_no}: self-loop; use --dedup')
                    counts['self_loops'] += 1
                    skip = True
                else:
                    a, b = sorted((u, v))
                    if kind == 'ins':
                        changed = connection.execute(
                            'INSERT OR IGNORE INTO live VALUES (?, ?)', (a, b)).rowcount
                        counter = 'duplicate_insertions'
                    else:
                        changed = connection.execute(
                            'DELETE FROM live WHERE u=? AND v=?', (a, b)).rowcount
                        counter = 'absent_deletions'
                    if not changed:
                        if not dedup:
                            raise ValueError(f'{source}:{line_no}: {counter}; use --dedup')
                        counts[counter] += 1
                        skip = True
                if skip:
                    outgoing.write('\n')
                    continue
                counts['insertions' if kind == 'ins' else 'deletions'] += 1
                outgoing.write(' '.join([kind, *fields[1:]]) + '\n')
                updates = counts['insertions'] + counts['deletions']
                if ((max_updates and updates >= max_updates) or
                        (max_deletions and counts['deletions'] >= max_deletions)):
                    # True only if unread source lines remain, even blank ones.
                    counts['truncated'] = bool(incoming.readline())
                    break
        counts['final_unique_edges'] = connection.execute('SELECT count(*) FROM live').fetchone()[0]
        partial.replace(destination)
        return counts
    finally:
        connection.close()
        Path(db_name).unlink(missing_ok=True)
        partial.unlink(missing_ok=True)


def command_for(args, dataset, workload):
    return [sys.executable, str(args.repo / 'scripts/dynamic-connectivity/run_dc_e2e_timing.py'),
            '--repo', str(args.repo), '--workload', str(workload),
            '--methods', *args.methods, '--build', args.build,
            '--workers', str(args.workers), '--worker-id', str(args.worker_id),
            '--repetitions', str(args.repetitions), '--threads', str(args.threads),
            '--marker-samples', str(args.marker_samples), '--query-every', str(args.query_every),
            '--seed', str(args.seed), '--order-seed', str(args.order_seed), '--run']


def run_child(command, log, repo):
    output_folder = None
    environment = os.environ.copy()
    environment.update(DC_REUSE_SCAN_STATE='1', DC_VERIFY_SCAN_STATE='0',
                       DC_E2E_TIMING='1', DC_PRINT_SEARCH_DIAGNOSTICS='0')
    with log.open('w') as saved:
        with subprocess.Popen(command, cwd=repo, env=environment, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
            for line in process.stdout:
                saved.write(line)
                saved.flush()
                print(line, end='', flush=True)
                if line.startswith('Output: '):
                    output_folder = Path(line[len('Output: '):].strip())
            code = process.wait()
    summary = output_folder / 'summary.json' if output_folder else None
    if code or summary is None or not summary.is_file():
        raise ValueError(f'Child runner failed (exit {code}); inspect {log}')
    result = json.loads(summary.read_text())
    if not result.get('runs') or any(r.get('status') != 'passed_and_timed' for r in result['runs']):
        raise ValueError(f'Incomplete timings: {summary}')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path('.'))
    parser.add_argument('--datasets', nargs='+', choices=DATASETS, default=list(DATASETS))
    parser.add_argument('--methods', nargs='+', default=['all'])
    parser.add_argument('--build', default='build/relwithdebinfo')
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--worker-id', type=int, required=True)
    parser.add_argument('--repetitions', type=int, default=4)
    parser.add_argument('--threads', type=int, default=1)
    parser.add_argument('--marker-samples', type=int, default=2)
    parser.add_argument('--query-every', type=int, default=0)
    parser.add_argument('--seed', type=int, default=20260921)
    parser.add_argument('--order-seed', type=int, default=20261009)
    parser.add_argument('--max-updates', type=int, default=0,
                        help='Effective update prefix cap; 0 means no cap')
    parser.add_argument('--max-deletions', type=int, default=0,
                        help='Stop after this many effective deletions; does NOT cap preceding insertions')
    parser.add_argument('--dedup', action='store_true', help='Explicit simple-graph no-op semantics')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--run', action='store_true')
    args = parser.parse_args()
    args.repo = args.repo.resolve()
    args.datasets = list(dict.fromkeys(args.datasets))
    args.methods = list(METHODS) if args.methods == ['all'] else list(dict.fromkeys(args.methods))
    if not args.methods or set(args.methods) - set(METHODS):
        parser.error('choose --methods all or a subset of the six methods')
    if not 1 <= args.worker_id <= args.workers <= args.repetitions:
        parser.error('require 1 <= worker-id <= workers <= repetitions')
    if args.threads < 1 or min(args.max_updates, args.max_deletions, args.query_every, args.marker_samples) < 0:
        parser.error('invalid thread/count argument')
    directory = args.repo / 'scripts/dynamic-connectivity'
    dependencies = [directory / name for name in ('run_dc_e2e_timing.py',
                    'run_dc_query_baselines.py', 'run_dc_workload.py')]
    sources = {name: args.repo / 'test-workloads' / (name + '.workloads') for name in args.datasets}
    missing = [str(p) for p in [*dependencies, *sources.values()] if not p.is_file()]
    if args.run:
        missing += [str(p) for p in (args.repo / args.build / 'test/runner/e2e_test',
                    args.repo / 'extension/algo/build/libalgo.kuzu_extension') if not p.is_file()]
    if missing:
        parser.error('missing required files:\n' + '\n'.join(missing))
    repetitions = list(range(args.worker_id, args.repetitions + 1, args.workers))
    print(f'Host: {socket.gethostname()}; worker {args.worker_id}/{args.workers}; repetitions {repetitions}')
    print('Methods:', ', '.join(args.methods))
    for name, source in sources.items():
        print(f'{name}: {source.stat().st_size / (1024**2):.1f} MiB input; {source}')
    print('Existing e2e generation is memory-resident. Run one dataset/method at a time; '
          'large full traces may exceed available memory. WCC computes the full partition.')
    if not args.run:
        print('Preflight only. Add --run to prepare inputs and execute; no input data were generated.')
        return 0
    output = args.output or Path('test-results') / f'dc_dataset_suite_worker{args.worker_id}'
    output = output.resolve() if output.is_absolute() else (args.repo / output).resolve()
    # The existing child requires output/test paths beneath the repo.
    if not output.is_relative_to(args.repo):
        parser.error('--output must be within --repo')
    output.mkdir(parents=True, exist_ok=True)
    state_path = output / 'suite_summary.json'
    config = {k: v for k, v in vars(args).items() if k not in ('repo', 'output', 'resume', 'run')}
    config['generator_hashes'] = {p.name: sha256(p) for p in [Path(__file__), *dependencies]}
    git = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=args.repo, capture_output=True, text=True)
    config['commit'] = git.stdout.strip() if git.returncode == 0 else None
    config['binary_hashes'] = {p.name: sha256(p) for p in (
        args.repo / args.build / 'test/runner/e2e_test',
        args.repo / 'extension/algo/build/libalgo.kuzu_extension')}
    if state_path.exists():
        if not args.resume:
            parser.error(f'{state_path} exists; use --resume or a new --output')
        state = json.loads(state_path.read_text())
        if state['config'] != config or state['host'] != socket.gethostname():
            parser.error('Resume configuration, source revision, binaries or host changed; use a new --output')
    else:
        state = dict(schema_version=1, mode='dc_dataset_suite', host=socket.gethostname(),
                     config=config, datasets={})
    save_json(state_path, state)
    for name, source in sources.items():
        previous = state['datasets'].get(name)
        original_hash = sha256(source)
        if previous and previous.get('status') == 'passed_and_timed':
            if previous['source_sha256'] != original_hash:
                raise ValueError(f'{name}: source changed; use a new --output')
            summary = args.repo / previous['timing_summary']
            if not summary.is_file() or sha256(summary) != previous['timing_summary_sha256']:
                raise ValueError(f'{name}: saved timing summary missing/changed')
            print(f'{name}: completed; skipping on resume', flush=True)
            continue
        entry = dict(status='preparing', source=str(source.relative_to(args.repo)),
                     source_sha256=original_hash)
        state['datasets'][name] = entry
        save_json(state_path, state)
        try:
            prepared = output / 'inputs' / (name + '.workloads')
            counts = prepare_trace(source, prepared, args.dedup, args.max_updates, args.max_deletions)
            entry.update(preprocessing=counts, prepared_sha256=sha256(prepared))
            if sha256(source) != original_hash:
                raise ValueError(f'{name}: input changed during preparation')
            if counts['insertions'] == 0:
                raise ValueError(f'{name}: no effective insertions')
            if counts['deletions'] == 0:
                print(f'{name}: prefix has NO deletions; it cannot validate deletion/replacement performance.')
            print(f'{name}: effective ins={counts["insertions"]} del={counts["deletions"]} '
                  f'markers={counts["markers"]}; skipped duplicate ins={counts["duplicate_insertions"]} '
                  f'absent del={counts["absent_deletions"]} loops={counts["self_loops"]}; '
                  f'truncated={counts["truncated"]}', flush=True)
            entry['status'] = 'running'
            save_json(state_path, state)
            summary = run_child(command_for(args, name, prepared), output / (name + '.driver.log'), args.repo)
            entry.update(status='passed_and_timed', timing_summary=str(summary.relative_to(args.repo)),
                         timing_summary_sha256=sha256(summary))
            save_json(state_path, state)
        except BaseException as error:
            entry.update(status='failed', error=str(error))
            save_json(state_path, state)
            raise
    print(f'Suite summary: {state_path}', flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, sqlite3.Error) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(2)
