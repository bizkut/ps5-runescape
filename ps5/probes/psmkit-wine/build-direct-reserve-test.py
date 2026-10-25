#!/usr/bin/env python3
"""Host-test the actual native reservation and per-unit cleanup implementation."""
import argparse
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
source = (args.wine / 'dlls/ntdll/unix/ps5_direct.c').read_text()
pieces = []
for begin, end in (
        ('struct ps5_direct_view\n', '\nstruct ps5_direct_pools'),
        ('static BYTE *direct_bitmap(', '\nstatic BOOL direct_metadata_block('),
        ('/* WINE_PS5_CHUNKED_RESERVE_TEST_BEGIN */', '/* WINE_PS5_CHUNKED_RESERVE_TEST_END */'),
        ('BOOL ps5_direct_drop_unit(', '\nBOOL ps5_direct_is_whole_backed('),
        ('int ps5_direct_protect(', '\nstatic int direct_read_pools(')):
    if source.count(begin) != 1 or source.count(end) != 1:
        raise SystemExit('direct reserve production boundaries missing or ambiguous')
    pieces.append(source[source.index(begin):source.index(end)])
virtual_source = (args.wine / 'dlls/ntdll/unix/virtual.c').read_text()
range_body = virtual_source[virtual_source.index('static void *alloc_free_area('):
                            virtual_source.index('\nstatic NTSTATUS map_fixed_area(')]
if range_body.count('limit_low = ps5_direct_reserve_floor( limit_low, size, area.direct );') != 1:
    raise SystemExit('unhinted allocation no longer applies the production large-reserve floor')
for begin, end in (
        ('struct alloc_area\n', '\nstatic void *map_alloc_area('),
        ('/* WINE_PS5_CHUNKED_RANGE_TEST_BEGIN */', '/* WINE_PS5_CHUNKED_RANGE_TEST_END */'),
        ('static void* try_map_free_area(', '\n/***********************************************************************'),
        ('static void *try_map_free_area_range(', '\nstatic void *alloc_free_area_in_range(')):
    if virtual_source.count(begin) != 1 or end not in virtual_source[virtual_source.index(begin):]:
        raise SystemExit('direct reserve range production boundaries missing or ambiguous')
    start = virtual_source.index(begin)
    pieces.append(virtual_source[start:virtual_source.index(end, start)])
args.output.parent.mkdir(parents=True, exist_ok=True)
(args.output.parent / 'wine-direct-reserve.inc').write_text('\n'.join(pieces))
subprocess.run(shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-I' + str(args.output.parent), str(Path(__file__).with_name('test-direct-reserve.c')),
    '-o', str(args.output)], check=True)
print('DIRECT_RESERVE_ISOLATION implementation=PRODUCTION_EXTRACTED')
