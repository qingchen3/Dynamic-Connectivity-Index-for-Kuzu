#!/usr/bin/env python3
"""Compare completed worker summaries without mixing measurements from different hosts."""
import argparse
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('summaries', nargs='+', type=Path)
    args = p.parse_args()
    reports = [json.loads(path.read_text()) for path in args.summaries]
    first = reports[0]
    fields = ('workload_sha256', 'query_pairs_sha256', 'script_sha256',
              'baseline_generator_sha256', 'workload_helper_sha256', 'commit')
    for report in reports:
        if report.get('mode') != 'e2e_statement_timing':
            raise ValueError('Not a timing summary')
        for field in fields:
            if report[field] != first[field]:
                raise ValueError(f'Incompatible reports: {field}')
        for field in ('threads', 'seed', 'marker_samples', 'query_every', 'max_deletions'):
            if report['arguments'][field] != first['arguments'][field]:
                raise ValueError(f'Incompatible argument: {field}')
    print('host,rep,method,phase,n,mean_execute_us,median_execute_us,p90_execute_us,total_execute_us')
    seen = set()
    for report in reports:
        host = report['machine']['hostname']
        for run in report['runs']:
            key = (run['repetition'], run['method'])
            if key in seen:
                raise ValueError(f'Duplicate repetition/method: {key}')
            seen.add(key)
            if run['status'] != 'passed_and_timed':
                raise ValueError(f'Incomplete run: {host} {key}')
            for phase in ('ins_transaction', 'del_transaction', 'pair_query', 'wcc_partition_query', 'query_batch'):
                if phase in run['timings']:
                    t = run['timings'][phase]['execute']
                    print(f"{host},{key[0]},{key[1]},{phase},{t['count']},"
                          f"{t['mean_us']:.3f},{t['median_us']:.3f},"
                          f"{t['p90_us']:.3f},{t['total_us']:.3f}")
    expected = {(entry['repetition'], method) for entry in first['schedule'] for method in entry['methods']}
    missing = expected - seen
    if missing:
        raise ValueError(f'Missing scheduled runs: {sorted(missing)}')


if __name__ == '__main__':
    main()
