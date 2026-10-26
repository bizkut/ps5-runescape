#!/usr/bin/env python3
"""Execute production combase create_surrogate_server: no DllSurrogate value, no dllhost."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = spec_from_file_location('extract_functions', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
args.out.mkdir(parents=True, exist_ok=True)
(args.out / 'combase-surrogate.inc').write_text(
    extract.definition(args.wine / 'dlls/combase/rpc.c', 'create_surrogate_server') + '\n')
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

typedef int BOOL; typedef long LONG; typedef unsigned long DWORD; typedef uint32_t HRESULT;
typedef wchar_t WCHAR; typedef const WCHAR *LPCWSTR; typedef unsigned char *LPBYTE; typedef void *HANDLE;
typedef void *HKEY; typedef unsigned long REGSAM; typedef struct { int unused; } GUID;
typedef const GUID *REFCLSID;
typedef struct { DWORD cb; } STARTUPINFOW;
typedef struct { HANDLE hProcess, hThread; } PROCESS_INFORMATION;
#define MAX_PATH 260
#define CHARS_IN_GUID 39
#define TRUE 1
#define FALSE 0
#define S_OK 0
#define ERROR_FILE_NOT_FOUND 2
#define KEY_READ 1
#define KEY_WOW64_32KEY 0x200
#define KEY_WOW64_64KEY 0x100
#define DETACHED_PROCESS 8
#define REGDB_E_CLASSNOTREG ((HRESULT)0x80040154)
#define FAILED(hr) ((int32_t)(hr) < 0)
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HRESULT_FROM_WIN32(e) ((HRESULT)(e))
#define lstrcatW wcscat
#define wcslen wcslen

static LONG value_status;        /* RegQueryValueExW result for DllSurrogate */
static const WCHAR *value_text;  /* its data when it exists */
static unsigned processes, appid_misses;
static GUID guid;

static HRESULT open_key_for_clsid(REFCLSID c, const WCHAR *sub, REGSAM sam, HKEY *key) { (void)c; (void)sub; (void)sam; *key = &guid; return S_OK; }
static HRESULT open_appidkey_from_clsid(REFCLSID c, REGSAM sam, HKEY *key) { (void)c; (void)sam; *key = &guid; return S_OK; }
static LONG RegQueryValueExW(HKEY k, const WCHAR *n, void *r, void *t, LPBYTE data, DWORD *size)
{
    (void)k; (void)r; (void)t; assert(!wcscmp(n, L"DllSurrogate"));
    if (value_status) return value_status;
    wcscpy((WCHAR *)data, value_text); *size = (wcslen(value_text) + 1) * sizeof(WCHAR); return 0;
}
static LONG RegCloseKey(HKEY k) { (void)k; return 0; }
static BOOL IsWow64Process(HANDLE p, BOOL *w) { (void)p; *w = FALSE; return TRUE; }
static HANDLE GetCurrentProcess(void) { return 0; }
static DWORD GetSystemWow64DirectoryW(WCHAR *b, DWORD n) { (void)n; wcscpy(b, L"C:\\windows\\syswow64"); return 1; }
static DWORD GetSystemDirectoryW(WCHAR *b, DWORD n) { (void)n; wcscpy(b, L"C:\\windows\\system32"); return 1; }
static int StringFromGUID2(REFCLSID c, WCHAR *b, int n) { (void)c; (void)n; wcscpy(b, L"{G}"); return 4; }
static BOOL CreateProcessW(const WCHAR *a, WCHAR *cmd, void *pa, void *ta, BOOL inh, DWORD fl, void *env,
                           const WCHAR *dir, STARTUPINFOW *si, PROCESS_INFORMATION *pi)
{ (void)a; (void)cmd; (void)pa; (void)ta; (void)inh; (void)fl; (void)env; (void)dir; (void)si; processes++; pi->hProcess = (HANDLE)1; pi->hThread = 0; return TRUE; }
static BOOL CloseHandle(HANDLE h) { (void)h; return TRUE; }
static void *Wow64DisableWow64FsRedirection(void **c) { *c = 0; return 0; }
static void Wow64RevertWow64FsRedirection(void *c) { (void)c; }
static const WCHAR *debugstr_guid(REFCLSID c) { (void)c; return L""; }
static DWORD GetLastError(void) { return 5; }

#include "combase-surrogate.inc"

int main(void)
{
    HANDLE process = 0;
    /* No DllSurrogate value: Windows starts nothing; neither may we. */
    value_status = ERROR_FILE_NOT_FOUND;
    assert(create_surrogate_server(&guid, &process) == REGDB_E_CLASSNOTREG);
    assert(!processes && !process);
    /* An empty DllSurrogate value means the default dllhost surrogate. */
    value_status = 0; value_text = L"";
    assert(create_surrogate_server(&guid, &process) == S_OK);
    assert(processes == 1 && process);
    /* An explicit surrogate is honoured. */
    processes = 0; process = 0; value_text = L"C:\\x\\surrogate.exe";
    assert(create_surrogate_server(&guid, &process) == S_OK && processes == 1);
    puts("COMBASE_SURROGATE_PASS no_value=refused empty=dllhost explicit=honoured");
    return 0;
}
'''
(args.out / 'test-combase-surrogate.c').write_text(harness.replace('#include <wchar.h>', '#include <wchar.h>\n#include <stdio.h>'))
subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
    '-I' + str(args.out), str(args.out / 'test-combase-surrogate.c'),
    '-o', str(args.out / 'test-combase-surrogate')], check=True)
subprocess.run([str(args.out / 'test-combase-surrogate')], check=True)
