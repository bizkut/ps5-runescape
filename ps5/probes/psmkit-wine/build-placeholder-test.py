#!/usr/bin/env python3
"""Compile actual native placeholder reservation, replacement and split/merge code."""
import argparse
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
virtual = (args.wine / 'dlls/ntdll/unix/virtual.c').read_text()
direct = (args.wine / 'dlls/ntdll/unix/ps5_direct.c').read_text()
pieces = []
for source, begin, end in (
        (direct, '/* WINE_PS5_PLACEHOLDER_TEST_BEGIN */', '/* WINE_PS5_PLACEHOLDER_TEST_END */'),
        (virtual, 'static NTSTATUS remove_pages_from_view(', '\n\n/***********************************************************************'),
        (virtual, 'static NTSTATUS free_pages_preserve_placeholder(', '\n\n/***********************************************************************'),
        (virtual, 'static NTSTATUS coalesce_placeholders(', '\n\n/***********************************************************************')):
    assert source.count(begin) == 1
    start = source.index(begin)
    pieces.append(source[start:source.index(end, start)])
start = virtual.index('    if (alloc_type & MEM_REPLACE_PLACEHOLDER)\n')
end = virtual.index('\n    if (limit_high &&', start)
pieces.append('static NTSTATUS replace_placeholder(struct file_view **view_ret, void *base, size_t size, unsigned int vprot)\n{\n'
              '    unsigned int alloc_type = MEM_REPLACE_PLACEHOLDER;\n'
              '    size_t host_size = size; int unix_prot = get_unix_prot(vprot);\n'
              + virtual[start:end] + '\n    return STATUS_INVALID_PARAMETER;\n}\n')
# Guard the actual allocation marker and both placement paths, not a test-only policy.
assert 'if ((type & MEM_RESERVE_PLACEHOLDER) && ps5_direct_reserve_placeholders())' in virtual
assert 'area.placeholder = !!(vprot & VPROT_PS5_PLACEHOLDER);' in virtual
assert 'ret = area->placeholder ? ps5_direct_placeholder_reserve( base, area->size, FALSE ) :' in virtual
assert 'if (area.placeholder && size >= (size_t)0x20000000)' in virtual
assert 'host_size > (size_t)((char *)end - (char *)start)' in virtual
start = virtual.index('        if (start >= end || host_size >')
end = virtual.index('\n#endif', start)
pieces.append('static NTSTATUS check_span(void *start, void *end, size_t host_size)\n{\n'
              + virtual[start:end] + '\n    return STATUS_SUCCESS;\n}\n')
start = virtual.index('    if (area.placeholder && size >=')
end = virtual.index('\n#endif', start)
pieces.append('static char *placeholder_floor(char *limit_low, size_t size, int placeholder)\n{\n'
              '    struct { int placeholder; } area = {placeholder};\n'
              + virtual[start:end] + '\n    return limit_low;\n}\n')
entry = Path(__file__).with_name('native-entry.c').read_text()
startup = entry[entry.index('    /* Profile values overwrite stale LoadExec state'):]
assert startup.index('setenv("WINEPS5_CHUNKED_RESERVE", PW_RUNTIME_CHUNKED_RESERVE, 1)') < startup.index('#ifdef PW_PARENT_ROLE')
build = Path(__file__).with_name('build-runtime.py').read_text()
assert "profile_defines + '#define PW_P1C_FAULT_INJECTION 1" in build
args.output.parent.mkdir(parents=True, exist_ok=True)
(args.output.parent / 'wine-placeholder.inc').write_text('\n'.join(pieces))
subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-I' + str(args.output.parent), str(Path(__file__).with_name('test-placeholder.c')),
    '-o', str(args.output)], check=True)
print('PLACEHOLDER_ISOLATION implementation=PRODUCTION_EXTRACTED child_profile_gate=1')
