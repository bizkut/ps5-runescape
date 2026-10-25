#!/usr/bin/env python3
"""Build only the pinned Mesa PE64 WGL/Zink frontend; never a Vulkan loader/ICD."""
import json
import os
from pathlib import Path
import subprocess

here = Path(__file__).resolve().parent
source_root = here.parents[2]
mesa = Path(os.environ.get('PS5_MESA_FORK', source_root / 'PS5_Mesa')).resolve()
mingw = Path(os.environ.get('LLVM_MINGW', Path.home() / 'llvm-mingw')).resolve()
build = here / 'build/zink-64'
build.mkdir(parents=True, exist_ok=True)
manifest = json.loads((here / 'release/deps.json').read_text())
pin = next(source for source in manifest['sources'] if source['name'] == 'PS5_Mesa')
revision = subprocess.check_output(['git', '-C', str(mesa), 'rev-parse', 'HEAD'], text=True).strip()
if revision != pin['commit']:
    raise SystemExit('WGL/Zink Mesa source does not match the release pin')
cross = build / 'cross.ini'
settings = '[binaries]\n' + ''.join(
    f"{name} = '{mingw / 'bin' / executable}'\n" for name, executable in (
        ('c', 'x86_64-w64-mingw32-clang'), ('cpp', 'x86_64-w64-mingw32-clang++'),
        ('ar', 'llvm-ar'), ('strip', 'llvm-strip'), ('windres', 'x86_64-w64-mingw32-windres')))
settings += """pkg-config = 'false'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
"""
if not cross.is_file() or cross.read_text() != settings:
    cross.write_text(settings)
command = ['meson', 'setup', *(['--reconfigure'] if (build / 'build.ninja').exists() else []),
           str(build), str(mesa), '--cross-file', str(cross), '--wrap-mode=nofallback',
           '--buildtype=release', '--default-library=static', '-Dplatforms=windows',
           '-Dgallium-drivers=zink', '-Dvulkan-drivers=', '-Dopengl=true',
           '-Degl=disabled', '-Dgbm=disabled', '-Dglx=disabled', '-Dgles1=disabled',
           '-Dgles2=disabled', '-Dllvm=disabled', '-Dzlib=disabled', '-Dzstd=disabled',
           '-Dshader-cache=disabled', '-Dxmlconfig=disabled', '-Dexpat=disabled',
           '-Dlibunwind=disabled', '-Dvalgrind=disabled', '-Dvideo-codecs=',
           '-Dbuild-tests=false', '-Dtools=', '-Dgallium-va=disabled', '-Dgallium-rusticl=false']
command += ['-Dc_link_args=-Wl,--no-insert-timestamp', '-Dcpp_link_args=-Wl,--no-insert-timestamp']
subprocess.run(command, check=True)
subprocess.run(['ninja', '-C', str(build), '-j', os.environ.get('JOBS', '8'),
                'src/gallium/targets/libgl-gdi/opengl32.dll',
                'src/gallium/targets/wgl/libgallium_wgl.dll'], check=True)
