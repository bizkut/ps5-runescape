#!/usr/bin/env python3
"""Exercise the production prerequisite gate against real Wine/MSI and redists.

Explicit integration test, not part of the host-only make target. The caller
supplies a disposable prefix and official pinned installers; no MSI state is
mocked or manufactured. No launcher or credentials are involved.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from runtime_profiles import bootstrap_header, profile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--prefix', type=Path, required=True)
parser.add_argument('--redists', type=Path, required=True,
                    help='directory containing pinned VC_redist.x86.exe and VC_redist.x64.exe')
parser.add_argument('--mode', choices=('fresh', 'partial', 'ready', 'job-x86', 'job-x64'), required=True,
                    help='expected initial MSI state, or first-GUI installer-job regression')
parser.add_argument('--wine', default='wine')
args = parser.parse_args()
here = Path(__file__).resolve().parent
prefix = args.prefix.resolve()
if not prefix.is_dir():
    parser.error('initialize the disposable prefix with wineboot first')
stage = prefix / 'drive_c/RuneScape'
stage.mkdir(parents=True, exist_ok=True)
for arch in ('x86', 'x64'):
    name = f'VC_redist.{arch}.exe'
    shutil.copyfile(args.redists / name, stage / name)
compiler = Path(os.environ.get('LLVM_MINGW', '/opt/llvm-mingw')) / 'bin/x86_64-w64-mingw32-clang'
with tempfile.TemporaryDirectory(prefix='.runescape-redist-wine-', dir=here) as folder:
    work = Path(folder)
    (work / 'runescape-product.h').write_text(bootstrap_header(
        profile('b1', json.loads((here / 'b1-product.json').read_text()))))
    # Include the actual gate/launcher/job/logger code, not a copied algorithm.
    (work / 'test.c').write_text('''
#define wmain bootstrap_wmain
#include "runescape-bootstrap.c"
#undef wmain
int wmain(int argc,WCHAR **argv)
{
    if(argc!=2) return 10;
    int x86=rs_redist_installed(&rs_redists[0]);
    int x64=rs_redist_installed(&rs_redists[1]);
    int want_x86=wcscmp(argv[1],L"fresh")!=0;
    int want_x64=!wcscmp(argv[1],L"ready");
    rs_logf("integration initial x86=%d x64=%d expected_x86=%d expected_x64=%d",x86,x64,want_x86,want_x64);
    if(x86!=want_x86 || x64!=want_x64) return 11;
    /* Prove diagnostics do not depend on the PE's stdout handle. */
    SetStdHandle(STD_OUTPUT_HANDLE,INVALID_HANDLE_VALUE);
    if(!rs_prepare_redists()) return 12;
    rs_log("integration prepare=ready");
    if(!rs_prepare_redists()) return 13;
    rs_log("integration retry=ready");
    return 0;
}
''')
    libraries = ['-lwintrust', '-lcrypt32', '-ladvapi32', '-lversion', '-luser32', '-lshell32', '-lmsi']
    if args.mode.startswith('job-'):
        # A kernel32-only parent preserves the no-desktop condition until the
        # production launch boundary. Resolve the real desktop API dynamically
        # so importing user32/COM cannot accidentally pre-create the desktop.
        source = (here / 'runescape-bootstrap.c').read_text()
        logger = 'static void rs_logf(' + source.split('static void rs_logf(', 1)[1].split(
            'static void rs_log(', 1)[0]
        launch = 'static int rs_launch(' + source.split('static int rs_launch(', 1)[1].split(
            '/* Genuine Windows elevation', 1)[0]
        (work / 'test.c').write_text(r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
static HWND real_GetDesktopWindow(void)
{
    HMODULE user=LoadLibraryW(L"user32.dll");
    HWND (WINAPI *desktop)(void)=(void *)GetProcAddress(user,"GetDesktopWindow");
    return desktop ? desktop() : NULL;
}
#define GetDesktopWindow real_GetDesktopWindow
''' + logger + launch + r'''
int wmain(int argc,WCHAR **argv)
{
    if(argc!=2) return 20;
    WCHAR path[128],arguments[256];
    const WCHAR *arch=!wcscmp(argv[1],L"job-x86") ? L"x86" : L"x64";
    if(swprintf(path,128,L"C:\\RuneScape\\VC_redist.%ls.exe",arch)<0 ||
       swprintf(arguments,256,L"/install /quiet /norestart /log C:\\RuneScape\\job-%ls.log",arch)<0) return 21;
    SetStdHandle(STD_OUTPUT_HANDLE,INVALID_HANDLE_VALUE);
    DWORD exit=31;
    int completed=rs_launch(path,arguments,L"C:\\RuneScape\\",600000,&exit);
    rs_logf("integration job completed=%d exit=%lu",completed,exit);
    return completed && (exit==0 || exit==1638 || exit==3010) ? 0 : 22;
}
''')
        libraries = []
    exe = work / 'test.exe'
    subprocess.run([str(compiler), '-std=c11', '-Os', '-static', '-municode',
                    '-Wall', '-Wextra', '-Werror', '-I', str(work), '-I', str(here),
                    str(work / 'test.c'), '-o', str(exe), *libraries], check=True)
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG='-all')
    subprocess.run([args.wine, str(exe), args.mode], env=env, check=True)
print(f'real Wine prerequisite {args.mode} regression PASS')
