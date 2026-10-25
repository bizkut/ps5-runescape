#!/usr/bin/env python3
"""Exercise actual RuneLite-only evidence helpers, not a JVM or PS5 workload."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
registered_headers = (args.wine / 'include/Makefile.in').read_text().splitlines()
for name in ('wine/ps5_game.h', 'wine/ps5_runelite_diag.h'):
    assert any(line.strip() == name + ' \\' for line in registered_headers), f'Wine makedep must register {name}'
spec = spec_from_file_location('runelite_diag_extraction', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
virtual = args.wine / 'dlls/ntdll/unix/virtual.c'
thread = args.wine / 'dlls/ntdll/unix/thread.c'
window = args.wine / 'dlls/win32u/window.c'
helpers = '\n'.join(extract.definition(path, name) for path, name in (
    (virtual, 'ps5_runelite_log_jvm_map'),
    (thread, 'ps5_runelite_log_compiler_name'),
    (window, 'ps5_runelite_log_awt_window')))
# Callsite contracts keep successful observations on the real loader, thread-name
# and completed-window paths, rather than testing an unconnected formatter.
mapping = extract.definition(virtual, 'virtual_map_image')
assert 'ps5_runelite_log_jvm_map' in mapping
assert mapping.index('ps5_runelite_log_jvm_map') > mapping.index('if (NT_SUCCESS(status))')
thread_info = extract.definition(thread, 'NtSetInformationThread')
name_start = thread_info.index('case ThreadNameInformation:')
name_end = thread_info.index('case ThreadWineNativeThreadName:', name_start)
assert 'ps5_runelite_log_compiler_name' in thread_info[name_start:name_end]
assert 'ps5_runelite_log_compiler_name' not in extract.definition(virtual, 'NtProtectVirtualMemory')
creation = extract.definition(window, 'NtUserCreateWindowEx')
assert creation.index('ps5_runelite_log_awt_window') > creation.index('user_driver->pCreateWindow')
assert creation.index('ps5_runelite_log_awt_window') < creation.index('\nfailed:')

harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TEST_NON_PRODUCT
#undef WINE_PS5_RUNESCAPE
#else
#define WINE_PS5_RUNESCAPE 1
#endif
#define __PROSPERO__ 1
#include "wine/ps5_runelite_diag.h"
typedef int BOOL;
typedef uint16_t WCHAR, USHORT;
typedef uint32_t DWORD, UINT;
typedef int32_t NTSTATUS;
typedef size_t SIZE_T;
typedef struct { WCHAR *Environment;SIZE_T EnvironmentSize; } RTL_USER_PROCESS_PARAMETERS;
static RTL_USER_PROCESS_PARAMETERS fake_params;
static struct { RTL_USER_PROCESS_PARAMETERS *ProcessParameters; } fake_peb;
static struct test_teb { typeof(fake_peb) *Peb; } fake_teb;
static struct test_teb *NtCurrentTeb(void) { return &fake_teb; }
static WCHAR fake_environment[256];
static void pe_environment(const char *bytes, size_t count)
{
    assert(count <= sizeof(fake_environment) / sizeof(fake_environment[0]));
    for (size_t i = 0; i < count; i++) fake_environment[i] = (unsigned char)bytes[i];
    fake_params.Environment = fake_environment;fake_params.EnvironmentSize = count * sizeof(WCHAR);
    fake_peb.ProcessParameters = &fake_params;fake_teb.Peb = &fake_peb;
}
typedef void *HWND;
typedef struct { USHORT Length, MaximumLength;WCHAR *Buffer; } UNICODE_STRING;
typedef struct { int32_t left, top, right, bottom; } RECT;
#define FALSE 0
#define TRUE 1
#define STATUS_SUCCESS 0
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HandleToULong(h) ((uint32_t)(uintptr_t)(h))
static const char *marker;
static DWORD last_error = 0x1234;
static char lines[64][512];
static unsigned line_count, class_calls, rect_calls;
static const char *window_class = "SunAwtFrame";
static BOOL geometry_valid = TRUE;
static char *test_getenv(const char *key)
{ assert(!strcmp(key, "WINEPS5_RUNELITE_DIAG"));return (char *)marker; }
#define getenv test_getenv
static DWORD RtlGetLastWin32Error(void) { return last_error; }
static void RtlSetLastWin32Error(DWORD value) { last_error = value; }
static DWORD GetCurrentThreadId(void) { return 0x24; }
static int sceKernelDebugOutText(int channel, const char *text)
{
    assert(!channel && line_count < ARRAY_SIZE(lines));
    snprintf(lines[line_count++], sizeof(lines[0]), "%s", text);last_error = 0x9876;return 0;
}
static int NtUserGetClassName(HWND hwnd, BOOL real, UNICODE_STRING *name)
{
    assert(hwnd == (HWND)0x100 && !real);class_calls++;last_error = 0x9876;
    unsigned size = strlen(window_class), capacity = name->MaximumLength / sizeof(WCHAR);
    if (size > capacity) size = capacity;
    for (unsigned i = 0; i < size; i++) name->Buffer[i] = (unsigned char)window_class[i];
    return size;
}
static BOOL NtUserGetWindowRect(HWND hwnd, RECT *rect, UINT dpi)
{
    assert(hwnd == (HWND)0x100 && dpi == 144);rect_calls++;last_error = 0x9876;
    if (geometry_valid) *rect = (RECT){10,20,810,620};
    return geometry_valid;
}
static BOOL NtUserGetClientRect(HWND hwnd, RECT *rect, UINT dpi)
{
    assert(hwnd == (HWND)0x100 && dpi == 144);rect_calls++;last_error = 0x9876;
    if (geometry_valid) *rect = (RECT){0,0,784,560};
    return geometry_valid;
}
static UNICODE_STRING unicode(const char *text, WCHAR *buffer, unsigned capacity)
{
    unsigned length = strlen(text);assert(length < capacity);
    for (unsigned i = 0; i <= length; i++) buffer[i] = (unsigned char)text[i];
    return (UNICODE_STRING){length * sizeof(WCHAR), capacity * sizeof(WCHAR), buffer};
}
'''
cases = r'''
static void check_privacy(void)
{
    for (unsigned i = 0; i < line_count; i++) {
        assert(!strstr(lines[i], "SESSION_SENTINEL") && !strstr(lines[i], "RWX"));
        assert(!strstr(lines[i], "initialized=1") && !strstr(lines[i], "compiled_method"));
        for (const unsigned char *p = (const unsigned char *)lines[i]; *p; p++)
            assert((*p >= 0x20 && *p <= 0x7e) || *p == '\n');
    }
}
int main(int argc, char **argv)
{
    WCHAR buffer[256];UNICODE_STRING name;
    unsigned tier = 99, index = 99;
    if (argc == 2) {
        assert(!strcmp(argv[1], "geometry-error"));
        marker = "1";geometry_valid = FALSE;
        ps5_runelite_log_awt_window((HWND)0x100, 144);
#ifndef TEST_NON_PRODUCT
        assert(line_count == 1 && strstr(lines[0], "window_valid=0 client_valid=0") &&
               strstr(lines[0], "window=0x0 client=0x0") && last_error == 0x1234);
#else
        assert(!line_count && !class_calls && !rect_calls);
#endif
        check_privacy();puts("RUNELITE_DIAG_GEOMETRY_ERROR_PASS");return 0;
    }
    assert(argc == 1);
    assert(!wine_ps5_runelite_enabled(NULL) && !wine_ps5_runelite_enabled("0") &&
           !wine_ps5_runelite_enabled("true") && !wine_ps5_runelite_enabled("10"));
#ifndef TEST_NON_PRODUCT
    assert(wine_ps5_runelite_enabled("1"));
#else
    assert(!wine_ps5_runelite_enabled("1"));
#endif
    const char pe_good[] = "SESSION_SENTINEL=private\0WINEPS5_RUNELITE_DIAG=1\0";
    const char pe_duplicate[] = "WINEPS5_RUNELITE_DIAG=1\0WINEPS5_RUNELITE_DIAG=0\0";
    const char *pe_rejected[] = {"XWINEPS5_RUNELITE_DIAG=1\0", "WINEPS5_RUNELITE_DIAG_EXTRA=1\0",
                                 "WINEPS5_RUNELITE_DIAG=0\0", "WINEPS5_RUNELITE_DIAG=10\0"};
    pe_environment(pe_good, sizeof(pe_good));
#ifndef TEST_NON_PRODUCT
    assert(wine_ps5_runelite_pe_marker(fake_environment, fake_params.EnvironmentSize));
    assert(wine_ps5_runelite_process_enabled(NULL, fake_environment, fake_params.EnvironmentSize));
#else
    assert(!wine_ps5_runelite_pe_marker(fake_environment, fake_params.EnvironmentSize));
#endif
    assert(!wine_ps5_runelite_process_enabled("0", fake_environment, fake_params.EnvironmentSize));
    assert(!wine_ps5_runelite_pe_marker(fake_environment, fake_params.EnvironmentSize - sizeof(WCHAR)));
    assert(!wine_ps5_runelite_pe_marker(fake_environment, 1) &&
           !wine_ps5_runelite_pe_marker(NULL, 0) &&
           !wine_ps5_runelite_pe_marker(fake_environment, WINE_PS5_RUNELITE_ENV_MAX_BYTES + 2));
    for (unsigned i = 0; i < ARRAY_SIZE(pe_rejected); i++) {
        pe_environment(pe_rejected[i], strlen(pe_rejected[i]) + 2);
        assert(!wine_ps5_runelite_pe_marker(fake_environment, fake_params.EnvironmentSize));
    }
    const char pe_lower[] = "wineps5_runelite_diag=1\0";
    pe_environment(pe_lower, sizeof(pe_lower));
#ifndef TEST_NON_PRODUCT
    assert(wine_ps5_runelite_pe_marker(fake_environment, fake_params.EnvironmentSize));
#endif
    pe_environment(pe_duplicate, sizeof(pe_duplicate));
    assert(!wine_ps5_runelite_pe_marker(fake_environment, fake_params.EnvironmentSize));
    fake_teb.Peb = NULL;
    name = unicode("C:\\SESSION_SENTINEL\\JvM.DlL", buffer, ARRAY_SIZE(buffer));
    assert(wine_ps5_runelite_jvm_image(name.Buffer, name.Length / sizeof(WCHAR)));
    const char *rejected_images[] = {"jvm.dll.exe", "jvm.dll:stream", "notjvm.dll", "jvm.dll/", "jvm.dl"};
    for (unsigned i = 0; i < ARRAY_SIZE(rejected_images); i++) {
        name = unicode(rejected_images[i], buffer, ARRAY_SIZE(buffer));
        assert(!wine_ps5_runelite_jvm_image(name.Buffer, name.Length / sizeof(WCHAR)));
    }
    name = unicode("C1 CompilerThread0", buffer, ARRAY_SIZE(buffer));
    assert(wine_ps5_runelite_compiler_name(name.Buffer, name.Length / sizeof(WCHAR), &tier, &index));
    assert(tier == 1 && index == 0);
    name = unicode("C2 CompilerThread17", buffer, ARRAY_SIZE(buffer));
    assert(wine_ps5_runelite_compiler_name(name.Buffer, name.Length / sizeof(WCHAR), &tier, &index));
    assert(tier == 2 && index == 17);
    const char *rejected_threads[] = {"C3 CompilerThread0", "C2 CompilerThread", "C2 CompilerThread-1",
        "C1 CompilerThread0 SESSION_SENTINEL", "Application SESSION_SENTINEL", "C1 CompilerThread999999"};
    for (unsigned i = 0; i < ARRAY_SIZE(rejected_threads); i++) {
        name = unicode(rejected_threads[i], buffer, ARRAY_SIZE(buffer));
        assert(!wine_ps5_runelite_compiler_name(name.Buffer, name.Length / sizeof(WCHAR), &tier, &index));
    }
    name = unicode("SunAwtFrame", buffer, ARRAY_SIZE(buffer));
    assert(!strcmp(wine_ps5_runelite_awt_class(name.Buffer, name.Length / sizeof(WCHAR)), "SunAwtFrame"));
    name = unicode("SunAwtFrame SESSION_SENTINEL", buffer, ARRAY_SIZE(buffer));
    assert(!wine_ps5_runelite_awt_class(name.Buffer, name.Length / sizeof(WCHAR)));
    volatile uint32_t budget = 0;
    for (unsigned i = 0; i < 100; i++) assert(wine_ps5_runelite_sample(&budget, 3) == (i < 3 ? i + 1 : 0));
    assert(budget == 3);

    marker = NULL;name = unicode("C:\\SESSION_SENTINEL\\jvm.dll", buffer, ARRAY_SIZE(buffer));
    ps5_runelite_log_jvm_map(&name, 0x8664, 1048576);
    ps5_runelite_log_awt_window((HWND)0x100, 144);
    assert(!line_count && !class_calls && !rect_calls);
    marker = "1";
    name = unicode("C:\\SESSION_SENTINEL\\unrelated.dll", buffer, ARRAY_SIZE(buffer));
    ps5_runelite_log_jvm_map(&name, 0x8664, 1048576);
    name = unicode("C1 CompilerThread0 SESSION_SENTINEL", buffer, ARRAY_SIZE(buffer));
    ps5_runelite_log_compiler_name(&name, 0, 0x40);
    name = unicode("C1 CompilerThread0", buffer, ARRAY_SIZE(buffer));
    ps5_runelite_log_compiler_name(&name, (NTSTATUS)0xc0000001, 0x40);
    ps5_runelite_log_compiler_name(&name, 0, 0);
    assert(!line_count);
    name = unicode("C:\\SESSION_SENTINEL\\jvm.dll", buffer, ARRAY_SIZE(buffer));
    pe_environment(pe_good, sizeof(pe_good));marker = "0";
    ps5_runelite_log_jvm_map(&name, 0x8664, 1048576);
    assert(!line_count);marker = NULL; /* Actual child gate: Windows marker, no Unix marker. */
    for (unsigned i = 0; i < 20; i++) ps5_runelite_log_jvm_map(&name, 0x8664, 1048576);
#ifndef TEST_NON_PRODUCT
    assert(line_count == 1 && strstr(lines[0], "event=jvm-image-mapped") && last_error == 0x1234);
#else
    assert(!line_count);
#endif
    name = unicode("C2 CompilerThread17", buffer, ARRAY_SIZE(buffer));
    marker = "1";ps5_runelite_log_compiler_name(&name, 0, 0x40);marker = NULL;
    for (unsigned i = 0; i < 40; i++) ps5_runelite_log_compiler_name(&name, 0, 0x40);
#ifndef TEST_NON_PRODUCT
    assert(line_count == 17 && strstr(lines[1], "event=compiler-thread-name-set") &&
           strstr(lines[1], "tier=C2 index=17") && last_error == 0x1234);
#endif
    window_class = "SunAwtSecret_SESSION_SENTINEL";
    ps5_runelite_log_awt_window((HWND)0x100, 144);
    assert(last_error == 0x1234);
    window_class = "SunAwtFrame";
    for (unsigned i = 0; i < 40; i++) ps5_runelite_log_awt_window((HWND)0x100, 144);
#ifndef TEST_NON_PRODUCT
    assert(line_count == 33 && strstr(lines[17], "event=awt-window-created class=SunAwtFrame") &&
           strstr(lines[17], "window=800x600 client=784x560") && last_error == 0x1234);
#else
    assert(!line_count && !class_calls && !rect_calls);
#endif
    check_privacy();puts("RUNELITE_DIAG_HOST_PASS scope=EXTRACTED_OBSERVATION_HELPERS_NOT_JAVA_OR_PS5_EXECUTION");
    return 0;
}
'''
args.out.mkdir(parents=True, exist_ok=True)
for control in (False, True):
    generated = args.out / ('test-runelite-diag-control.c' if control else 'test-runelite-diag.c')
    generated.write_text(harness + helpers + cases)
    binary = generated.with_suffix('')
    subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-I' + str(args.wine / 'include')] + (['-DTEST_NON_PRODUCT=1'] if control else []) +
        [str(generated), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    subprocess.run([str(binary), 'geometry-error'], check=True)
