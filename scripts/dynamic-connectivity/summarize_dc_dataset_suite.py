#!/usr/bin/env python3
"""Validate all workers, then write per-host and per-dataset average timing CSVs."""
import argparse
import csv
import json
from pathlib import Path
import statistics

PHASES = ('ins_transaction', 'del_transaction', 'pair_query',
          'wcc_partition_query', 'query_batch')


def collect(repo, suite_paths):
    suites = [json.loads(path.read_text()) for path in suite_paths]
    if not suites or any(s.get('mode') != 'dc_dataset_suite' for s in suites):
        raise ValueError('Expected suite_summary.json files')
    first = suites[0]
    compare = ('datasets', 'methods', 'workers', 'repetitions', 'threads', 'marker_samples',
               'query_every', 'seed', 'order_seed', 'max_updates', 'max_deletions',
               'dedup', 'generator_hashes', 'commit')
    workers = set()
    for suite in suites:
        for key in compare:
            if suite['config'][key] != first['config'][key]:
                raise ValueError(f'Incompatible suite configuration: {key}')
        worker = suite['config']['worker_id']
        if worker in workers:
            raise ValueError(f'Duplicate worker: {worker}')
        workers.add(worker)
    if workers != set(range(1, first['config']['workers'] + 1)):
        raise ValueError('Missing workers; supply one suite summary per worker')
    rows = []
    for dataset in first['config']['datasets']:
        reference = None
        reference_entry = None
        seen = set()
        for suite in suites:
            entry = suite['datasets'].get(dataset, {})
            if entry.get('status') != 'passed_and_timed':
                raise ValueError(f'Incomplete dataset {dataset} on {suite["host"]}')
            if reference_entry:
                for key in ('source_sha256', 'prepared_sha256', 'preprocessing'):
                    if entry[key] != reference_entry[key]:
                        raise ValueError(f'{dataset}: preprocessing/source differs: {key}')
            reference_entry = entry
            summary = repo / entry['timing_summary']
            import hashlib
            if hashlib.sha256(summary.read_bytes()).hexdigest() != entry['timing_summary_sha256']:
                raise ValueError(f'Missing/changed timing summary: {summary}')
            report = json.loads(summary.read_text())
            if report.get('mode') != 'e2e_statement_timing':
                raise ValueError(f'Wrong timing mode: {summary}')
            if report['machine']['hostname'] != suite['host']:
                raise ValueError('Recorded host differs between suite and timing summary')
            if report['worker_id'] != suite['config']['worker_id']:
                raise ValueError('Worker differs between suite and timing summary')
            if report['workload_sha256'] != entry['prepared_sha256']:
                raise ValueError('Child did not time the prepared input')
            for key in ('threads', 'marker_samples', 'query_every', 'seed', 'order_seed',
                        'workers', 'repetitions'):
                if report.get('arguments', {}).get(key) != suite['config'][key]:
                    raise ValueError(f'{dataset}: child argument differs: {key}')
            if reference:
                for key in ('workload_sha256', 'query_pairs_sha256', 'script_sha256',
                            'baseline_generator_sha256', 'workload_helper_sha256', 'commit', 'schedule'):
                    if report[key] != reference[key]:
                        raise ValueError(f'{dataset}: incompatible timing metadata: {key}')
            reference = report
            for run in report['runs']:
                key = (run['repetition'], run['method'])
                if key in seen:
                    raise ValueError(f'{dataset}: duplicate run {key}')
                if run.get('status') != 'passed_and_timed':
                    raise ValueError(f'{dataset}: incomplete run {key}')
                seen.add(key)
                for phase in PHASES:
                    if phase not in run['timings']:
                        continue
                    value = run['timings'][phase]['execute']
                    rows.append(dict(dataset=dataset, host=suite['host'], rep=key[0],
                                     method=key[1], phase=phase, n=value['count'],
                                     mean_execute_us=value['mean_us'],
                                     median_execute_us=value['median_us'],
                                     p90_execute_us=value['p90_us'], total_execute_us=value['total_us']))
        expected = {(rep, method) for rep in range(1, first['config']['repetitions'] + 1)
                    for method in first['config']['methods']}
        scheduled = {(s['repetition'], method) for s in reference['schedule'] for method in s['methods']}
        if scheduled != expected:
            raise ValueError(f'{dataset}: child schedule differs from requested suite')
        if seen != expected:
            raise ValueError(f'{dataset}: missing/unexpected repetitions/methods: {seen ^ expected}')
    averages = []
    groups = {}
    for row in rows:
        groups.setdefault((row['dataset'], row['method'], row['phase']), []).append(row)
    for (dataset, method, phase), values in sorted(groups.items()):
        if len(values) != first['config']['repetitions']:
            raise ValueError(f'{dataset}/{method}/{phase}: phase missing from a repetition')
        if len({v['n'] for v in values}) != 1:
            raise ValueError(f'{dataset}/{method}/{phase}: operation counts differ')
        means = [v['mean_execute_us'] for v in values]
        averages.append(dict(dataset=dataset, method=method, phase=phase,
                             repetitions=len(values), n_per_run=values[0]['n'],
                             mean_execute_us=statistics.mean(means),
                             host_mean_min_us=min(means), host_mean_max_us=max(means),
                             host_mean_sd_us=statistics.stdev(means) if len(means) > 1 else 0))
    return rows, averages


def write_csv(path, rows):
    if not rows:
        raise ValueError('No timing rows')
    with path.open('w', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path('.'))
    parser.add_argument('--suites', type=Path, nargs='+', required=True)
    parser.add_argument('--output', type=Path, default=Path('res/timing/all_datasets'))
    args = parser.parse_args()
    repo = args.repo.resolve()
    suites = [p if p.is_absolute() else repo / p for p in args.suites]
    rows, averages = collect(repo, suites)  # Validate everything before writing.
    output = args.output if args.output.is_absolute() else repo / args.output
    output.mkdir(parents=True, exist_ok=True)
    write_csv(output / 'per_host.csv', rows)
    write_csv(output / 'averages.csv', averages)
    for dataset in sorted({r['dataset'] for r in averages}):
        write_csv(output / (dataset + '_average.csv'), [r for r in averages if r['dataset'] == dataset])
    print(f'Validated {len(suites)} workers. Results: {output}')
    print('Averages are arithmetic means of per-run means. Host/repetition variability is combined.')
    print('pair_query is pair reachability; wcc_partition_query is a full partition. '
          'query_batch compares checkpoint work; it does not make these operations identical.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        parser_message = f'error: {error}'
        import sys
        print(parser_message, file=sys.stderr)
        sys.exit(2)
