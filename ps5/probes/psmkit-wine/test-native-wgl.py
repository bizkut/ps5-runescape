#!/usr/bin/env python3
"""Execute the production GDI/WGL boundary with native ICD metadata isolated."""
import argparse
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
source = (args.wine / 'dlls/gdi32/opengl.c').read_text()
boundary = source[source.index('static HMODULE opengl32;'):]
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winternl.h"
#define TRACE(...) ((void)0)
static char messages[8192];
void test_message(const char *format, ...) {
    va_list args;va_start(args, format);
    vsnprintf(messages + strlen(messages), sizeof(messages) - strlen(messages), format, args);
    va_end(args);
}
#define MESSAGE(...) test_message(__VA_ARGS__)
static LDR_DATA_TABLE_ENTRY entry;
static unsigned load_calls, choose_calls, describe_calls, set_calls, swap_calls;
static int current_format, lookup_failure, missing_choose;
static HDC expected_dc = (HDC)(UINT_PTR)0x1234;
static const PIXELFORMATDESCRIPTOR *expected_pfd;
HMODULE WINAPI LoadLibraryW(const WCHAR *name) {
    assert(name[0] == 'o');load_calls++;return (HMODULE)(UINT_PTR)0x5678;
}
NTSTATUS WINAPI LdrFindEntryForAddress(const void *address, LDR_DATA_TABLE_ENTRY **result) {
    assert(address == (void *)(UINT_PTR)0x5678);*result = &entry;return lookup_failure;
}
HWND WINAPI NtUserWindowFromDC(HDC dc) { assert(dc == expected_dc);return (HWND)(UINT_PTR)0x9abc; }
DWORD WINAPI GetObjectType(HGDIOBJ object) { assert(object == (HGDIOBJ)expected_dc);return OBJ_DC; }
static INT WINAPI native_choose(HDC dc, const PIXELFORMATDESCRIPTOR *pfd) {
    assert(dc == expected_dc && pfd == expected_pfd);choose_calls++;return 303;
}
static INT WINAPI native_describe(HDC dc, INT format, UINT bytes, PIXELFORMATDESCRIPTOR *pfd) {
    assert(dc == expected_dc);describe_calls++;
    if (!pfd) return 2;
    assert(bytes == sizeof(*pfd));
    if (format < 1 || format > 2) return 0;
    memset(pfd, 0, sizeof(*pfd));pfd->nSize = sizeof(*pfd);pfd->nVersion = 1;
    pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd->cColorBits = format == 1 ? 24 : 32;pfd->cDepthBits = 24;pfd->cStencilBits = 8;
    return 2;
}
static INT WINAPI native_get(HDC dc) { assert(dc == expected_dc);return current_format; }
static BOOL WINAPI native_set(HDC dc, INT format, const PIXELFORMATDESCRIPTOR *pfd) {
    assert(dc == expected_dc && pfd == expected_pfd);set_calls++;
    if (current_format || format < 1 || format > 2) return FALSE;
    current_format = format;return TRUE;
}
static BOOL WINAPI native_swap(HDC dc) { assert(dc == expected_dc);swap_calls++;return TRUE; }
FARPROC WINAPI GetProcAddress(HMODULE module, const char *name) {
    assert(module == (HMODULE)(UINT_PTR)0x5678);
    if (!strcmp(name, "wglChoosePixelFormat")) return missing_choose ? NULL : (FARPROC)(void *)native_choose;
    if (!strcmp(name, "wglDescribePixelFormat")) return (FARPROC)(void *)native_describe;
    if (!strcmp(name, "wglGetPixelFormat")) return (FARPROC)(void *)native_get;
    if (!strcmp(name, "wglSetPixelFormat")) return (FARPROC)(void *)native_set;
    if (!strcmp(name, "wglSwapBuffers")) return (FARPROC)(void *)native_swap;
    assert(0);return NULL;
}
'''
harness += '#include "wine/wgl_pixel_format.h"\n'
cases = r'''
static void reset(ULONG flags) {
    memset(&entry, 0, sizeof(entry));entry.Flags = flags;
    opengl32 = NULL;wglChoosePixelFormat = NULL;wglDescribePixelFormat = NULL;
    wglGetPixelFormat = NULL;wglSetPixelFormat = NULL;wglSwapBuffers = NULL;
    load_calls = choose_calls = describe_calls = set_calls = swap_calls = 0;
    current_format = lookup_failure = missing_choose = 0;messages[0] = 0;
}
int main(void) {
    PIXELFORMATDESCRIPTOR request = {0};
    request.nSize = sizeof(request);request.nVersion = 1;request.dwFlags = 0x225;
    request.cColorBits = 16;request.cDepthBits = 24;request.cStencilBits = 8;expected_pfd = &request;
    reset(LDR_WINE_INTERNAL);
    assert(ChoosePixelFormat(expected_dc, &request) == 303 && choose_calls == 1 && !describe_calls);
    assert(SetPixelFormat(expected_dc, 1, &request));
    assert(!SetPixelFormat(expected_dc, 1, &request) && set_calls == 2); /* builtin unchanged */
    assert(SwapBuffers(expected_dc) && swap_calls == 1 && load_calls == 1);
    reset(0);
#ifdef __WINE_PS5__
    PIXELFORMATDESCRIPTOR actual;
    assert(ChoosePixelFormat(expected_dc, &request) == 1); /* never return pbuffer-only 303 */
    assert(!choose_calls && describe_calls && load_calls == 1);
    assert(DescribePixelFormat(expected_dc, 1, sizeof(actual), &actual) == 2 && actual.cColorBits == 24);
    assert(SetPixelFormat(expected_dc, 1, &request) && current_format == 1 && set_calls == 1);
    assert(SetPixelFormat(expected_dc, 1, &request) && set_calls == 1); /* real existing state, not a second ICD set */
    assert(strstr(messages, "old=1 new=1 count=2 result=1"));
    assert(!SetPixelFormat(expected_dc, 2, &request) && current_format == 1 && set_calls == 1);
    assert(GetPixelFormat(expected_dc) == 1 && SwapBuffers(expected_dc) && swap_calls == 1);
    assert(load_calls == 1);
    reset(0);request.nVersion = 0;
    assert(!ChoosePixelFormat(expected_dc, &request) && !describe_calls);request.nVersion = 1;
    reset(0);request.nSize = 0;
    assert(!ChoosePixelFormat(expected_dc, &request) && !describe_calls);request.nSize = sizeof(request);
    reset(0);request.cColorBits = 32;
    assert(ChoosePixelFormat(expected_dc, &request) == 2);
    reset(0);request.iPixelType = PFD_TYPE_COLORINDEX;
    assert(!ChoosePixelFormat(expected_dc, &request));request.iPixelType = PFD_TYPE_RGBA;
    reset(0);request.dwFlags |= PFD_SUPPORT_GDI;
    assert(!ChoosePixelFormat(expected_dc, &request));request.dwFlags &= ~PFD_SUPPORT_GDI;
    reset(0);assert(!SetPixelFormat(expected_dc, 0, &request) && set_calls == 1 && !current_format);
    reset(0);lookup_failure = -1;
    assert(ChoosePixelFormat(expected_dc, &request) == 303 && choose_calls == 1 && !describe_calls);
    reset(0);missing_choose = 1;
    assert(!ChoosePixelFormat(expected_dc, &request) && !choose_calls && !describe_calls);
#else
    assert(ChoosePixelFormat(expected_dc, &request) == 303 && choose_calls == 1 && !describe_calls);
    assert(SetPixelFormat(expected_dc, 1, &request));
    assert(!SetPixelFormat(expected_dc, 1, &request) && set_calls == 2); /* non-PS5 native unchanged */
#endif
    puts("native WGL: displayable-only selection, same-format idempotence, descriptor/get/swap forwarding, builtin/non-PS5 guards PASS");
    return 0;
}
'''
args.out.mkdir(parents=True, exist_ok=True)
for ps5 in (True, False):
    generated = args.out / ('test-native-wgl' + ('' if ps5 else '-control') + '.c')
    generated.write_text(harness + boundary + cases)
    binary = generated.with_suffix('')
    subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-fshort-wchar', '-O2', '-g',
        '-Wall', '-Wextra', '-Werror',
        *(['-D__WINE_PS5__'] if ps5 else ['-U__WINE_PS5__']),
        '-idirafter', str(args.wine / 'include'), str(generated), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
