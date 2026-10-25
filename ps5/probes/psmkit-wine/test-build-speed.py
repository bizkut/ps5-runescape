#!/usr/bin/env python3
"""Small host contracts for configure reuse and byte-identical ZIP assembly."""
import ast
import importlib.util
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import zipfile


PS5 = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def functions(path, names, namespace):
    tree = ast.parse(path.read_text())
    selected = ast.Module(body=[node for node in tree.body
                                if isinstance(node, ast.FunctionDef) and node.name in names],
                          type_ignores=[])
    exec(compile(selected, str(path), 'exec'), namespace)
    return namespace


stamp = load('configure_stamp', PS5 / 'configure-stamp.py')
package = load('release_package', PS5 / 'release/package.py')

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    build = root / 'build'
    script = root / 'configure'
    script.write_text('''#!/bin/sh
set -eu
printf 'configure\\n' >> events
mkdir -p include
touch config.status include/config.h
printf 'CFLAGS = %s\\nCXXFLAGS = -g -O2\\ni386_CFLAGS = %s\\nx86_64_CFLAGS = %s\\nclean:\\n\\t@echo clean >> events\\n\\t@rm -f object.o\\n' "$CFLAGS" "${CROSSCFLAGS:-}" "${CROSSCFLAGS:-}" > Makefile
''')
    script.chmod(0o755)
    script.with_name('configure.ac').write_text('first input\n')
    environment = dict(os.environ, CFLAGS='-g -O2')
    command = [str(script)]
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines() == ['configure']
    (build / 'object.o').touch()
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines() == ['configure']
    assert (build / 'object.o').exists()
    environment['CFLAGS'] = '-g -O2 -DCHANGED'
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines() == ['configure', 'clean', 'configure']
    assert not (build / 'object.o').exists()
    (build / '.configure-stamp.json').unlink()
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines()[-1] == 'configure'
    assert (build / 'events').read_text().splitlines().count('clean') == 1
    script.with_name('configure.ac').write_text('changed input\n')
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines().count('configure') == 4
    assert (build / 'events').read_text().splitlines().count('clean') == 1
    # A legacy build tree without a stamp must not reuse differently flagged objects.
    (build / '.configure-stamp.json').unlink()
    environment['CFLAGS'] = '-g -O2'
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines()[-2:] == ['clean', 'configure']
    cross = stamp.compiler_flags([str(script), 'CROSSCFLAGS=-O3 -DPE'], environment)
    assert cross['i386_CFLAGS'] == cross['x86_64_CFLAGS'] == '-O3 -DPE'
    makefile = build / 'Makefile'
    makefile.write_text(makefile.read_text().replace('CFLAGS = -g -O2\n', 'CFLAGS = -O0\n', 1))
    stamp.configure(build, command, environment)
    assert (build / 'events').read_text().splitlines()[-2:] == ['clean', 'configure']
    cross_build = root / 'cross-build'
    environment['CROSSCFLAGS'] = '-O2'
    stamp.configure(cross_build, command, environment)
    stamp.configure(cross_build, command, environment)
    assert (cross_build / 'events').read_text().splitlines() == ['configure']
    environment['CROSSCFLAGS'] = '-O3'
    stamp.configure(cross_build, command, environment)
    assert (cross_build / 'events').read_text().splitlines() == ['configure', 'clean', 'configure']

    compile_helpers = functions(Path(__file__).with_name('build-runtime.py'), {
        'command_for', 'output_path', 'prepare_compile_entries', 'exact_object_manifest',
    }, {'Path': Path, 'os': os, 'shlex': shlex, 'build': root})
    outputs = ('dlls/one/unix/a.o', 'dlls/one/unix/b.o', 'dlls/one-extra/unix/c.o')
    for name in outputs:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    compiles = [
        {'file': 'a.c', 'directory': str(root),
         'command': "'/tool path/prospero-clang' -c a.c -o dlls/one/unix/a.o"},
        {'file': 'b.c', 'directory': str(root),
         'arguments': ['/tool/prospero-clang++', '-c', 'b.c', '-o', 'dlls/one/unix/b.o']},
        {'file': 'c.c', 'directory': str(root),
         'command': 'prospero-clang -c c.c -o dlls/one-extra/unix/c.o'},
        {'file': 'pe.c', 'directory': str(root),
         'command': 'x86_64-w64-mingw32-clang -c pe.c -o dlls/one/unix/pe.o'},
    ]
    original_commands = [compile_helpers['command_for'](entry) for entry in compiles]
    compile_helpers['prepare_compile_entries'](compiles, root)
    for entry, command in zip(compiles, original_commands):
        assert compile_helpers['command_for'](entry) == command
        changed = compile_helpers['command_for'](entry)
        changed[0] = 'changed'
        assert compile_helpers['command_for'](entry) == command
    assert compile_helpers['exact_object_manifest'](compiles, 'dlls/one') == [
        root / outputs[0], root / outputs[1]]

    entries = []
    for name, data, mode in (
            ('empty', b'', 0o644), ('bin/runner', b'ELF' * 700001, 0o755),
            ('unicode-\u03bb.txt', bytes(range(256)) * 8193, 0o644)):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
        entries.append((name, path))
    names = {name for name, _ in entries}
    serial = root / 'serial.zip'
    with zipfile.ZipFile(serial, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, path in entries:
            info = zipfile.ZipInfo(f'{package.TITLE_ID}/{name}', package.EPOCH)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (0o100755 if path.stat().st_mode & 0o111 else 0o100644) << 16
            with path.open('rb') as source, archive.open(info, 'w') as target:
                while chunk := source.read(1 << 20):
                    target.write(chunk)
    for jobs in (1, 3):
        output = root / f'parallel-{jobs}.zip'
        package.write_archive(output, entries, names, jobs)
        assert output.read_bytes() == serial.read_bytes(), jobs

layout = functions(Path(__file__).with_name('build-runtime.py'),
                   {'module_linker_script', 'harden_module_headers'}, {'struct': struct})
sdk_script = PS5.parents[2] / 'PS5_Vulkan/.deps/native/ps5-payload-sdk/ldscripts/elf_x86_64.x'
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    script = root / 'modules.x'
    script.write_text(layout['module_linker_script'](sdk_script))
    for initialized in (False, True):
        source = root / 'module.c'
        obj = root / 'module.o'
        module = root / 'module.so'
        source.write_text('int zero __attribute__((aligned(64)));\n' + ('int live = 1;\n' if initialized else '') +
                          'int query(void) { return zero; }\n')
        subprocess.run(['clang-19', '-fPIC', '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run(['ld.lld-19', '-shared', '-z', 'max-page-size=0x4000',
                        '--eh-frame-hdr', '-T', str(script), str(obj), '-o', str(module)], check=True)
        data = module.read_bytes()
        shoff = struct.unpack_from('<Q', data, 40)[0]
        shsize, shnum, names_index = struct.unpack_from('<HHH', data, 58)
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize) for i in range(shnum)]
        names = sections[names_index]
        names = data[names[4]:names[4] + names[5]]
        named = {names[s[0]:].split(b'\0', 1)[0].decode(): s for s in sections}
        if initialized:
            data_end = named['.data'][3] + named['.data'][5]
            assert named['.bss'][3] == (data_end + 63) & ~63
        else:
            assert not named['.bss'][3] % 0x4000
        layout['harden_module_headers'](module)
    malformed = root / 'bad.x'
    malformed.write_text('SECTIONS {}')
    try:
        layout['module_linker_script'](malformed)
    except SystemExit:
        pass
    else:
        raise AssertionError('unknown SDK BSS layout accepted')
print('module-layout: actual SDK script, BSS-only RELRO separation, existing data packing and fail-closed drift PASS')
print('build-speed: configure stamp, compile index and parallel/serial ZIP bytes PASS')

# Exercise the actual SDK extraction function with a real Git worktree. SDK
# compilation is outside this path contract; its installer records the source.
release_script = (PS5 / 'release/build-linux.sh').read_text()
sdk_function = release_script[release_script.index('build_sdk() {'):
                              release_script.index('\nbuild_host_tools()')]
with tempfile.TemporaryDirectory() as directory:
    sandbox = Path(directory)
    root = sandbox / 'sources'
    root.mkdir()
    upstream = sandbox / 'upstream'
    upstream.mkdir()

    def sdk_git(path, *arguments):
        return subprocess.check_output(['git', '-C', str(path),
            '-c', 'user.name=SDK host fixture', '-c', 'user.email=sdk-host@example.invalid',
            *arguments], text=True).strip()

    sdk_git(upstream, 'init', '--quiet')
    installer = upstream / 'platform/tools/setup-sdk.sh'
    installer.parent.mkdir(parents=True)
    installer.write_text('''#!/usr/bin/env bash
set -eu
mkdir -p "$1"
printf '%s\\n' "${BASH_SOURCE[0]}" > "$1/source-script"
printf '%s\\n' "$2" > "$1/revision"
''')
    installer.chmod(0o755)
    obsolete = upstream / 'obsolete-header.h'
    obsolete.write_text('first revision only\n')
    sdk_git(upstream, 'add', '.')
    sdk_git(upstream, 'commit', '--quiet', '-m', 'first SDK fixture')
    first = sdk_git(upstream, 'rev-parse', 'HEAD')
    obsolete.unlink()
    sdk_git(upstream, 'add', '-u')
    sdk_git(upstream, 'commit', '--quiet', '-m', 'remove obsolete SDK input')
    second = sdk_git(upstream, 'rev-parse', 'HEAD')
    worktree = root / 'sdk-runescape'
    sdk_git(upstream, 'worktree', 'add', '--quiet', '--detach', str(worktree), second)
    (root / 'PS5_PayloadSDK').symlink_to(worktree, target_is_directory=True)
    release = root / 'release'
    release.mkdir()
    target = root / 'installed-sdk'
    environment = dict(os.environ, root=str(root), here=str(release),
                       PS5_PAYLOAD_SDK=str(target))
    for revision in (first, second):
        (release / 'deps.json').write_text(json.dumps({'sdk_revision': revision}))
        subprocess.run(['bash', '-c', sdk_function + '\nbuild_sdk'], env=environment, check=True)
        expected = root / '.sdk-source' / revision / 'platform/tools/setup-sdk.sh'
        assert (target / 'source-script').read_text().strip() == str(expected)
        assert (target / 'revision').read_text().strip() == revision
    assert (root / '.sdk-source' / first / 'obsolete-header.h').is_file()
    assert not (root / '.sdk-source' / second / 'obsolete-header.h').exists()
print('sdk-source: isolated worktree archive and revision-specific extraction prevent stale deleted inputs PASS')
