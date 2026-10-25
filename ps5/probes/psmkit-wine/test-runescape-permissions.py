#!/usr/bin/env python3
"""Compile real native shared I/O/repair and Wine mode/attribute boundaries."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wine', type=Path, default=here.parents[3] / 'ps5-wine')
parser.add_argument('--sdk', type=Path, default=here.parents[4] / 'PS5_PayloadSDK')
parser.add_argument('--cc', default=os.environ.get('HOST_CC', 'cc'))
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('permission_extraction', here / 'build-desktop-surface-test.py')
extraction = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extraction)
definition = extraction.definition
# Match build-path-test.py's Clang recovery-AST isolation. The SDK puts its
# return type on a preceding line, which the generic token extractor omits.
sdk_source = args.sdk / 'platform/src/libc.c'
query = subprocess.run([os.environ.get('AST_CC', 'clang'), '-std=gnu11', '-fsyntax-only',
    '-I' + str(args.sdk / 'platform/include'), '-I' + str(args.sdk / 'platform/src'),
    '-Xclang', '-ast-dump=json', '-Xclang', '-ast-dump-filter=ps5_realpath', str(sdk_source)],
    text=True, capture_output=True)
decoder = json.JSONDecoder()
text, nodes = query.stdout, []
while text.strip():
    text = text.lstrip()
    node, end = decoder.raw_decode(text)
    text = text[end:]
    if node.get('kind') == 'FunctionDecl' and node.get('name') == 'ps5_realpath' and any(
            child.get('kind') == 'CompoundStmt' for child in node.get('inner', [])):
        nodes.append(node)
if len(nodes) != 1:
    raise SystemExit('cannot structurally isolate SDK ps5_realpath: ' + query.stderr)
node = nodes[0]
start = node['range']['begin']['offset']
end = node['range']['end']['offset'] + node['range']['end']['tokLen']
sdk_realpath = sdk_source.read_bytes()[start:end].decode()
functions = '\n\n'.join([
    sdk_realpath,
    definition(args.wine / 'dlls/ntdll/unix/server.c', 'ps5_title_drive'),
    definition(args.wine / 'dlls/ntdll/unix/file.c', 'is_wine_file'),
    definition(args.wine / 'dlls/ntdll/unix/file.c', 'fd_set_file_info'),
    definition(args.wine / 'server/fd.c', 'get_shared_fd_mode'),
    definition(here / '../../writebroker/writebroker.c', 'write_token'),
])
# Preserve the real integration points, including post-create handling of an
# OS umask and subsequent DACL updates. The native wrappers are linked below,
# not mocked, and cover both direct libc calls and imported function pointers.
# open_fd has mutually exclusive preprocessor brace openers, so a token-only
# brace counter cannot isolate it. These are source-wiring assertions, scoped
# to its unique declaration and the next top-level function boundary.
fd_source = (args.wine / 'server/fd.c').read_text()
create = fd_source.split('struct fd *open_fd(', 1)[1].split(
    '/* create an fd for an anonymous file */', 1)[0]
security = definition(args.wine / 'server/file.c', 'set_file_sd')
assert 'wine_ps5_shared_resolve(' in create
assert 'get_shared_fd_mode( fd, st.st_mode, st.st_mode )' in create
assert 'get_shared_fd_mode( fd, st.st_mode, new_mode )' in security
prefix_setup = definition(args.wine / 'dlls/ntdll/unix/server.c', 'setup_title_prefix')
assert 'asprintf( &title_c_drive, "%s/drive_c", config_dir )' in prefix_setup
dos_mapping = (args.wine / 'dlls/ntdll/unix/file.c').read_text()
assert "const char *root = ps5_title_drive( prefix[0] );" in dos_mapping
assert 'strcpy( unix_name, root );' in dos_mapping
native = (here / 'native-entry.c').read_text()
startup = native.split('static int pw_forever_start(void)', 1)[1]
assert startup.index('rs_repair_permissions(FOREVER_ROOT,') < startup.index('pw_forever_sync_managed()')
assert 'permissions scanned=%llu changed=%llu skipped=%llu failed=%llu result=%d' in startup
stage = (here / 'runescape-install.h').read_text()
assert 'mkdir(path,0777)' in stage
assert 'O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0666' in stage
assert 'mkdir(path,0700)' not in stage
title_build = (here / '../../title/build.sh').read_text()
assert 'native-entry.c|native-entry-generated.c|native-entry-parent.c) shared_io=1' in title_build
assert 'flags+=(--wrap=open --wrap=fopen --wrap=mkdir --wrap=chmod)' in title_build
assert 'wine/ps5_shared_mode.h' in (args.wine / 'include/Makefile.in').read_text()
with tempfile.TemporaryDirectory(prefix='.runescape-permissions-', dir=here) as folder:
    work = Path(folder)
    (work / 'runescape-permission-functions.inc').write_text(functions + '\n')
    for control in (False, True):
        fixture = work / ('forever' if control else 'runescape')
        fixture.mkdir()
        root = fixture / 'root'
        output = work / ('test-forever' if control else 'test-runescape')
        command = [*shlex.split(args.cc), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-DWINE_PS5_RUNESCAPE=1', f'-DWINE_PS5_SHARED_ROOT="{root}"',
                   '-I', str(work), '-I', str(args.wine / 'include'),
                   str(here / 'test-runescape-permissions.c'), '-o', str(output),
                   '-Wl,--wrap=open', '-Wl,--wrap=fopen', '-Wl,--wrap=mkdir', '-Wl,--wrap=chmod']
        if control:
            command.append('-DTEST_FOREVER=1')
        subprocess.run(command, check=True)
        subprocess.run([str(output), str(fixture)], check=True)
print('shared permissions production boundaries and non-product isolation PASS')
