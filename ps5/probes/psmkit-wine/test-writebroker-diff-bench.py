#!/usr/bin/env python3
"""Build/run the explicitly opt-in complete broker tail trace, before and after."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
parser.add_argument('--baseline-ref', default='6f2dd865797fae1f57cb3b72f34490bc9b9ba0f2')
parser.add_argument('--run-full', action='store_true', help='run both complete 86,539-write arms, not a shortened sample')
parser.add_argument('--arm', choices=('both', 'before', 'after'), default='both',
                    help='run one complete arm without repeating an already completed arm')
args = parser.parse_args()
here = Path(__file__).resolve().parent
product = here.parents[2]
args.out.mkdir(parents=True, exist_ok=True)
# Reuse the actual broker/client extraction and real socket/file fixture, not a
# second implementation of diffing, request splitting or acknowledgement.
subprocess.run([sys.executable, str(here / 'build-writebroker-space-test.py'),
                '--wine', str(args.wine), '--output', str(args.out / 'test-writebroker-space'),
                '--cc', args.cc], check=True)
spec = spec_from_file_location('broker_bench_extraction', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
before = subprocess.check_output(['git', '-C', str(product), 'show',
    args.baseline_ref + ':ps5/writebroker/writebroker.c'], text=True)
before_path = args.out / 'writebroker-before-source.c'
before_path.write_text(before)
function = extract.definition(before_path, 'serve_write')
function, count = re.subn(r'\bserve_write(?=\s*\()', 'serve_write_baseline', function, count=1)
assert count == 1
(args.out / 'writebroker-before.inc').write_text(function + '\n')
binary = args.out / 'test-writebroker-diff-bench'
subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-pthread', '-I' + str(args.wine / 'include'), '-I' + str(here), '-I' + str(args.out),
    str(here / 'test-writebroker-diff-bench.c'), '-o', str(binary)], check=True)
if args.run_full:
    lines = []
    log_path = args.out / ('writebroker-diff-' + args.arm + '.log')
    with log_path.open('w') as log, subprocess.Popen(
            [str(binary), '--full', args.arm], text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT) as process:
        for line in process.stdout:
            print(line, end='', flush=True)
            log.write(line);log.flush()
            lines.append(line)
        result = process.wait()
    if result:
        raise SystemExit(result)
    records = []
    for line in lines:
        if not line.startswith('BROKER_DIFF_FULL '):
            continue
        record = {}
        for item in line.split()[1:]:
            key, value = item.split('=', 1)
            if re.fullmatch(r'[0-9]+', value):
                value = int(value)
            elif re.fullmatch(r'[0-9]+\.[0-9]+', value):
                value = float(value)
            record[key] = value
        records.append(record)
    assert len(records) == (2 if args.arm == 'both' else 1) and all(row['rounds'] == 86539 for row in records)
    report = {'scope': 'HOST_CACHED_BACKING_IO_NOT_PS5_STORAGE', 'baseline_source': args.baseline_ref,
              'full_sequence_each_arm': True, 'arm': args.arm, 'records': records,
              'pwrite_bytes_meaning': 'successful bytes accepted by backing pwrite, not measured device flush traffic',
              'pread_bytes_meaning': 'successful bytes returned by broker pread, cached host filesystem',
              'latency_scope': 'whole client broker_pwrite including 16MiB splits and all synchronous ACKs; verification outside latency',
              'verification': 'exact full tail via preexisting shared mapping + prefix edges + EOF + caller position after every logical ACK'}
    (args.out / ('writebroker-diff-' + args.arm + '.json')).write_text(json.dumps(report, indent=2) + '\n')
