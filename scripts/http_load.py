#!/usr/bin/env python3
"""Fixed-arrival local HTTP load sample. Counts rejects and incomplete streams explicitly."""
import argparse
import hashlib
from datetime import datetime, timezone
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import math
import platform
from pathlib import Path
import resource
import select
import subprocess
import tempfile
import time


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)] if ordered else None


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', required=True)
    p.add_argument('--model')
    p.add_argument('--requests', type=int, default=24)
    p.add_argument('--interval-ms', type=float, default=5)
    p.add_argument('--tokens', type=int, default=64)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if not 1 <= args.requests <= 256 or args.interval_ms < 0 or not 1 <= args.tokens <= 1024:
        p.error('invalid workload bounds')
    command = [str(Path(args.binary).resolve()), '--port', '0', '--max-inflight', '8']
    if args.model:
        command += ['--model', str(Path(args.model).resolve())]
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors, text=True)
        try:
            if not select.select([process.stdout], [], [], 60)[0]:
                raise RuntimeError('server startup deadline')
            ready = json.loads(process.stdout.readline())
            port = ready['port']
            start = time.monotonic() + .1

            def request(index):
                planned = start + index * args.interval_ms / 1000
                time.sleep(max(0, planned - time.monotonic()))
                began = time.monotonic()
                row = {'index': index, 'launch_lag_ms': (began - planned) * 1000,
                       'status': None, 'outcome': 'transport_error', 'ttft_ms': None, 'tokens': 0}
                c = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
                try:
                    c.request('POST', '/generate', 'Briefly explain a database index.',
                              {'X-Max-Tokens': str(args.tokens)})
                    response = c.getresponse()
                    row['status'] = response.status
                    if response.status != 200:
                        response.read()
                        row['outcome'] = 'http_rejected'
                    else:
                        row['outcome'] = 'incomplete_stream'
                        for line in response:
                            if not line.startswith(b'data: '):
                                continue
                            event = json.loads(line[6:])
                            if event['type'] == 'token':
                                row['tokens'] += 1
                                if row['ttft_ms'] is None:
                                    row['ttft_ms'] = (time.monotonic() - planned) * 1000
                            if event['type'] in {'completed', 'cancelled', 'rejected', 'failed'}:
                                row['outcome'] = event['type']
                except (OSError, http.client.HTTPException, ValueError):
                    row['outcome'] = 'transport_error'
                finally:
                    c.close()
                row['duration_ms'] = (time.monotonic() - planned) * 1000
                return row

            # One worker per scheduled arrival avoids a hidden client-side queue.
            with ThreadPoolExecutor(args.requests) as pool:
                rows = list(pool.map(request, range(args.requests)))
            c = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
            c.request('GET', '/metrics')
            metrics = json.loads(c.getresponse().read())
            c.close()
        finally:
            process.terminate()
            try:
                process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
        if process.returncode:
            errors.seek(0)
            raise RuntimeError(errors.read().decode()[-4000:])
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    completed = [r for r in rows if r['outcome'] == 'completed']
    first_tokens = [r['ttft_ms'] for r in completed if r['ttft_ms'] is not None]
    deadlines = {str(d): sum(r['outcome'] == 'completed' and r['ttft_ms'] is not None
                             and r['ttft_ms'] <= d for r in rows) / len(rows) for d in (100, 500, 1000)}
    report = {'schema_version': 1, 'recorded_at': datetime.now(timezone.utc).isoformat(),
              'binary_sha256': hashlib.sha256(Path(args.binary).read_bytes()).hexdigest(),
              'model_sha256': hashlib.sha256(Path(args.model).read_bytes()).hexdigest() if args.model else None, 'measurement': ready, 'platform': platform.platform(),
              'workload': {'requests': args.requests, 'interval_ms': args.interval_ms,
                           'max_output_tokens': args.tokens, 'inflight_limit': 8},
              'summary': {'completed': len(completed), 'total': len(rows),
                          'ttft_completed_p50_ms': percentile(first_tokens, .5),
                          'ttft_completed_p95_ms': percentile(first_tokens, .95),
                          'completion_p95_ms': percentile([r['duration_ms'] for r in completed], .95),
                          'completed_with_ttft_within_ms_fraction_of_all': deadlines,
                          'server_cpu_seconds_including_startup': after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
                          'server_max_rss_bytes': after.ru_maxrss * (1 if platform.system() == 'Darwin' else 1024)},
              'server_metrics': metrics, 'requests': rows,
              'limitations': 'Single local run; CPU includes model startup; not a policy comparison or production capacity claim.'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['summary'], indent=2))


if __name__ == '__main__':
    main()
