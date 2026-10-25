#!/usr/bin/env python3
"""Exercise production ancestor lookup and the post-hit mouse activation tail.

Only shared-object/GUI/server boundaries are modeled. This is not PS5 delivery
qualification, and does not substitute a stock Wine GUI for the detached desktop.
"""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = spec_from_file_location('mouse_activation_extraction', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
window = args.wine / 'dlls/win32u/window.c'
message = args.wine / 'dlls/win32u/message.c'
ntuser = args.wine / 'include/ntuser.h'
protocol = args.wine / 'include/wine/server_protocol.h'


def macro(path, name):
    matches = re.findall(r'^#define ' + re.escape(name) + r'(?:\([^\n]*?\))?[^\n]*$',
                         path.read_text(), re.MULTILINE)
    if len(matches) != 1:
        raise SystemExit(f'expected one {name} macro in {path}, got {len(matches)}')
    return matches[0] + '\n'


# Keep the real union/generation layout and index macros: stale/truncated handle
# cases must go through the same validity predicates as the production lookup.
layout = extract.definition(ntuser, 'user_entry', structure=True)
constants = ''.join(macro(path, name) for path, name in (
    (protocol, 'FIRST_USER_HANDLE'), (protocol, 'LAST_USER_HANDLE'),
    (ntuser, 'MAX_USER_HANDLES'), (ntuser, 'NTUSER_OBJ_WINDOW'), (ntuser, 'NTUSER_OBJ_ICON'),
    (window, 'USER_HANDLE_TO_INDEX'), (window, 'USER_HANDLE_FROM_INDEX')))
helpers = '\n\n'.join(extract.definition(window, name) for name in (
    'is_valid_entry_uniq', 'is_valid_entry', 'read_acquire_user_entry', 'get_user_entry',
    'get_user_handle_ptr', 'get_full_window_handle', 'is_desktop_window', 'get_win_ptr',
    'get_parent', 'list_window_parents', 'NtUserGetAncestor'))
mouse = extract.definition(message, 'process_mouse_message')
start = mouse.index('    eat_msg = FALSE;')
end = mouse.index('    return !eat_msg;', start) + len('    return !eat_msg;')
activation = mouse[start:end]
assert activation.count('NtUserGetAncestor') == 1
assert 'WM_MOUSEACTIVATE' in activation and 'set_foreground_window' in activation

preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL;
typedef uint16_t WORD, USHORT;
typedef uint32_t UINT, DWORD, ULONG, user_handle_t;
typedef uint64_t UINT64, ULONG64;
typedef int64_t LONG64;
typedef uintptr_t ULONG_PTR, WPARAM;
typedef intptr_t LONG_PTR, LPARAM;
typedef int16_t INT16;
typedef void *HANDLE, *HWND;
typedef struct { int32_t x, y; } POINT;
typedef struct { HWND hwnd;UINT message;WPARAM wParam;LPARAM lParam;DWORD time;POINT pt; } MSG;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define LOWORD(v) ((WORD)(uintptr_t)(v))
#define HIWORD(v) ((WORD)((uintptr_t)(v) >> 16))
#define MAKELONG(lo,hi) ((uint32_t)(WORD)(lo) | ((uint32_t)(WORD)(hi) << 16))
#define UlongToHandle(v) ((HANDLE)(uintptr_t)(uint32_t)(v))
#define GA_PARENT 1
#define GA_ROOT 2
#define GA_ROOTOWNER 3
#define GWL_STYLE (-16)
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_MBUTTONDOWN 0x0207
#define WM_XBUTTONDOWN 0x020b
#define WM_MOUSEACTIVATE 0x0021
#define WM_SETCURSOR 0x0020
#define MA_ACTIVATE 1
#define MA_ACTIVATEANDEAT 2
#define MA_NOACTIVATE 3
#define MA_NOACTIVATEANDEAT 4
#define HTCLIENT 1
#define ERROR_INVALID_WINDOW_HANDLE 1400
#define OBJ_OTHER_PROCESS ((void *)1)
#define WND_OTHER_PROCESS ((WND *)1)
#define WND_DESKTOP ((WND *)2)
#define ReadAcquire64(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ReadNoFence64(p) (*(p))
#define __SHARED_READ_FENCE __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define WARN(...) ((void)0)
#define min(a,b) ((a)<(b)?(a):(b))
'''
backend = r'''
struct ntuser_thread_info { ULONG top_window, msg_window; } thread_info;
static struct { struct user_entry user_entries[MAX_USER_HANDLES]; } session;
static const typeof(session) *shared_session = &session;
static void *client_objects[MAX_USER_HANDLES];
typedef struct { HWND parent, owner;DWORD dwStyle; } WND;
static WND windows[8];
static HWND server_parent[8];
static unsigned lock_depth, errors, server_calls;
static DWORD last_error;
static unsigned foreground_calls, activate_calls, notify_calls, cursor_calls;
static HWND foreground_target, activate_root;
static UINT ma_result;
static BOOL foreground_failure, server_failure;
static int trace_stage;
#define DESKTOP USER_HANDLE_FROM_INDEX(0,1)
#define ROOT USER_HANDLE_FROM_INDEX(1,1)
#define CHILD USER_HANDLE_FROM_INDEX(2,1)
#define MESSAGE_ROOT USER_HANDLE_FROM_INDEX(3,1)
#define MESSAGE_CHILD USER_HANDLE_FROM_INDEX(4,1)
#define FOREIGN USER_HANDLE_FROM_INDEX(5,1)
#define INACTIVE USER_HANDLE_FROM_INDEX(6,1)
static DWORD GetCurrentProcessId(void) { return 0x20; }
static struct ntuser_thread_info *NtUserGetThreadInfo(void) { return &thread_info; }
static void user_lock(void) { assert(!lock_depth);lock_depth = 1; }
static void user_unlock(void) { assert(lock_depth == 1);lock_depth = 0; }
static void release_win_ptr(WND *win)
{ assert(win && win != WND_DESKTOP && win != WND_OTHER_PROCESS);user_unlock(); }
static void RtlSetLastWin32Error(DWORD error) { last_error = error; }
static DWORD get_window_long(HWND hwnd, int index)
{
    unsigned slot = USER_HANDLE_TO_INDEX(hwnd);
    assert(index == GWL_STYLE);
    if (!hwnd || slot >= 8 || session.user_entries[slot].type != NTUSER_OBJ_WINDOW) {
        last_error = ERROR_INVALID_WINDOW_HANDLE;return 0;
    }
    return windows[slot].dwStyle;
}
/* The server boundary returns the actual fixture tree including the desktop.
 * It does not route ancestor lookup around a NULL local desktop lookup. */
enum request_kind { REQUEST_get_window_parents, REQUEST_get_window_tree };
struct fake_reply { int count;user_handle_t parent, owner; };
struct fake_request {
    enum request_kind kind;user_handle_t handle;void *buffer;size_t bytes;struct fake_reply *reply;
};
#define SERVER_START_REQ(name) do { struct fake_reply reply_storage = {0}; \
    struct fake_reply *reply = &reply_storage; \
    struct fake_request request_storage = {.kind = REQUEST_##name, .reply = reply}; \
    struct fake_request *req = &request_storage;
#define SERVER_END_REQ } while (0)
static user_handle_t wine_server_user_handle(HWND hwnd) { return (uintptr_t)hwnd; }
static HWND wine_server_ptr_handle(user_handle_t hwnd) { return UlongToHandle(hwnd); }
static void wine_server_set_reply(struct fake_request *req, void *buffer, size_t bytes)
{ req->buffer = buffer;req->bytes = bytes; }
static int wine_server_call(struct fake_request *req)
{
    unsigned slot = USER_HANDLE_TO_INDEX(req->handle);
    assert(!lock_depth);server_calls++;
    if (server_failure || slot >= 8) return 1;
    if (req->kind == REQUEST_get_window_tree) {
        req->reply->parent = wine_server_user_handle(server_parent[slot]);return 0;
    }
    HWND next = server_parent[slot];
    unsigned count = 0;
    while (next) {
        assert(count < 8);
        if ((count + 1) * sizeof(user_handle_t) <= req->bytes)
            ((user_handle_t *)req->buffer)[count] = wine_server_user_handle(next);
        count++;next = server_parent[USER_HANDLE_TO_INDEX(next)];
    }
    req->reply->count = count;return 0;
}
static int wine_server_call_err(struct fake_request *req)
{
    int status = wine_server_call(req);
    if (status) last_error = ERROR_INVALID_WINDOW_HANDLE;
    return status;
}
static void send_parent_notify(HWND hwnd, UINT message, unsigned id, POINT point)
{
    assert(hwnd == CHILD && message != WM_MOUSEMOVE && message != WM_LBUTTONUP && !id);
    assert(point.x == 123 && point.y == 145);notify_calls++;
}
static UINT send_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    assert(hwnd == CHILD);
    if (message == WM_MOUSEACTIVATE) {
        activate_calls++;activate_root = (HWND)wparam;
        assert(LOWORD(lparam) == HTCLIENT);return ma_result;
    }
    assert(message == WM_SETCURSOR && (HWND)wparam == CHILD && LOWORD(lparam) == HTCLIENT);
    cursor_calls++;return 0;
}
static BOOL set_foreground_window(HWND hwnd, BOOL mouse, BOOL internal)
{
    assert(mouse && !internal);foreground_calls++;foreground_target = hwnd;
    if (hwnd != ROOT || foreground_failure) {
        last_error = hwnd ? 5 : ERROR_INVALID_WINDOW_HANDLE;return FALSE;
    }
    return TRUE;
}
/* Metadata logging is not an activation policy boundary. */
#define PS5_MOUSE_ACTIVATION_LOG(...) ((void)0)
#define PS5_MOUSE_DROP 1
#define PS5_MOUSE_RETURN 2
#define PS5_CLICK_LOG(stage,reason,space) do { \
    trace_stage = (stage);(void)(reason);(void)(space); \
} while (0)
'''
# The production GA_ROOTOWNER branch calls get_parent(), so extract that helper
# too rather than replacing a core ancestor dependency with a favorable stub.
wrapper = r'''
static BOOL activation_tail(MSG *msg, HWND active)
{
    struct { HWND hwndActive; } info = {active};
    BOOL eat_msg;
    UINT message = msg->message;
    int hittest = HTCLIENT;
'''

cases = r'''
static void check(BOOL condition, const char *what)
{
    if (!condition) { fprintf(stderr, "MOUSE_ACTIVATION_HOST_FAIL %s\n", what);errors++; }
}
static void reset_activation(void)
{
    foreground_calls = activate_calls = notify_calls = cursor_calls = 0;
    foreground_target = activate_root = NULL;trace_stage = 0;last_error = 0;
    ma_result = MA_ACTIVATE;foreground_failure = FALSE;
}
static void fixture(void)
{
    assert(!lock_depth);
    memset(&session, 0, sizeof(session));memset(client_objects, 0, sizeof(client_objects));
    memset(windows, 0, sizeof(windows));memset(server_parent, 0, sizeof(server_parent));
    for (unsigned i = 0; i < 7; i++) {
        session.user_entries[i].uniq = MAKELONG(NTUSER_OBJ_WINDOW, 1);
        session.user_entries[i].pid = GetCurrentProcessId();session.user_entries[i].tid = 0x24;
    }
    thread_info.top_window = (uintptr_t)DESKTOP;thread_info.msg_window = (uintptr_t)MESSAGE_ROOT;
    windows[0].dwStyle = WS_POPUP;
    windows[1] = (WND){.parent = DESKTOP, .dwStyle = WS_POPUP};
    windows[2] = (WND){.parent = ROOT, .dwStyle = WS_CHILD};
    windows[3].dwStyle = WS_POPUP;
    windows[4] = (WND){.parent = MESSAGE_ROOT, .dwStyle = WS_POPUP};
    windows[5] = (WND){.parent = DESKTOP, .dwStyle = WS_POPUP};
    windows[6] = (WND){.parent = DESKTOP, .dwStyle = WS_POPUP};
    for (unsigned i = 1; i < 7; i++) if (i != 3) {
        client_objects[i] = &windows[i];server_parent[i] = windows[i].parent;
    }
    server_failure = FALSE;server_calls = 0;reset_activation();
}
static void expect_ptr(HWND hwnd, WND *expected, const char *label)
{
    unsigned before = lock_depth;
    WND *actual = get_win_ptr(hwnd);
    check(actual == expected, label);
    if (actual && actual != WND_OTHER_PROCESS && actual != WND_DESKTOP) {
        check(lock_depth == before + 1, "real WND retains user lock");release_win_ptr(actual);
    }
    check(lock_depth == before, "sentinel/invalid lookup does not leak user lock");
}
static BOOL button(UINT message, HWND active)
{
    MSG msg = {.hwnd = CHILD, .message = message, .wParam = 0x12345678,
               .lParam = 0x11223344, .time = 73, .pt = {123,145}};
    MSG before;memcpy(&before, &msg, sizeof(msg));
    BOOL result = activation_tail(&msg, active);
    check(!memcmp(&msg, &before, sizeof(msg)), "activation tail preserves normal MSG");
    check(!lock_depth, "activation returns with user lock released");
    check(cursor_calls == 1, "uncaptured activation tail still sends WM_SETCURSOR");
    check(trace_stage == (result ? PS5_MOUSE_RETURN : PS5_MOUSE_DROP), "trace agrees with retained/eaten result");
    return result;
}
static void case_ready(void)
{
    fixture();
    check(is_valid_entry(DESKTOP, NTUSER_OBJ_WINDOW), "detached desktop entry is valid");
    check(!client_objects[0], "detached desktop has no local WND");
    check(!get_user_handle_ptr(DESKTOP, NTUSER_OBJ_WINDOW), "generic same-PID lookup remains NULL");
    check(!lock_depth, "generic NULL lookup unlocks");
    HWND ancestor = NtUserGetAncestor(CHILD, GA_ROOT);
    BOOL kept = button(WM_LBUTTONDOWN, ROOT);
    printf("MOUSE_ACTIVATION_OBSERVED root=%lx expected_root=%lx retained=%u ma=%u foreground=%lx calls=%u error=%u\n",
           (unsigned long)(uintptr_t)ancestor, (unsigned long)(uintptr_t)ROOT, kept, ma_result,
           (unsigned long)(uintptr_t)foreground_target, foreground_calls, last_error);
    check(ancestor == ROOT, "same-PID detached desktop yields top-level GA_ROOT");
    check(kept, "MA_ACTIVATE DOWN is retained with already-active root");
    check(activate_root == ROOT && foreground_target == ROOT, "activation uses actual top-level root");
    check(!server_calls, "local detached-desktop chain needs no server fallback");
    expect_ptr(DESKTOP, WND_DESKTOP, "same-PID detached desktop sentinel");
    reset_activation();ma_result = 0;
    check(button(WM_LBUTTONDOWN, ROOT), "default MA return DOWN retained");
    for (unsigned i = 0; i < 3; i++) {
        const UINT downs[] = {WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN};
        reset_activation();check(button(downs[i], ROOT), "other normal button DOWN retained");
    }
}
static void case_guards(void)
{
    fixture();
    expect_ptr(ROOT, &windows[1], "local real WND pointer preserved");
    client_objects[0] = &windows[0];
    expect_ptr(DESKTOP, &windows[0], "materialized local desktop WND preserved, not converted");
    check(NtUserGetAncestor(CHILD, GA_ROOT) == ROOT, "materialized desktop ancestor unchanged");
    client_objects[0] = NULL;
    session.user_entries[5].pid = 0x44;
    expect_ptr(FOREIGN, WND_OTHER_PROCESS, "foreign normal WND remains foreign");
    session.user_entries[0].pid = 0x44;
    expect_ptr(DESKTOP, WND_DESKTOP, "foreign desktop sentinel unchanged");
    check(NtUserGetAncestor(CHILD, GA_ROOT) == ROOT, "foreign desktop ancestor unchanged");
    session.user_entries[1].pid = 0x44;
    check(NtUserGetAncestor(CHILD, GA_ROOT) == ROOT, "foreign-parent server list includes desktop");
    check(server_calls == 1, "foreign parent exercises server fallback");
    check(NtUserGetAncestor(ROOT, GA_PARENT) == DESKTOP, "foreign GA_PARENT server result");
    fixture();
    expect_ptr(UlongToHandle(LOWORD(DESKTOP)), WND_DESKTOP, "truncated detached desktop handle");
    expect_ptr(UlongToHandle(0xffff0000u | LOWORD(DESKTOP)), WND_DESKTOP, "FFFF-generation desktop handle");
    check(NtUserGetAncestor(UlongToHandle(LOWORD(CHILD)), GA_ROOT) == ROOT,
          "truncated child handle resolves canonical root");
    expect_ptr(MESSAGE_ROOT, WND_DESKTOP, "same-PID detached message-parent sentinel");
    check(!NtUserGetAncestor(MESSAGE_ROOT, GA_PARENT), "message-parent sentinel has no parent");
    check(NtUserGetAncestor(MESSAGE_CHILD, GA_ROOT) == MESSAGE_CHILD, "message child root excludes message parent");
}
static void case_invalid(void)
{
    fixture();
    expect_ptr(NULL, NULL, "NULL handle rejected");
    expect_ptr(UlongToHandle(FIRST_USER_HANDLE - 2), NULL, "out-of-range handle rejected");
    expect_ptr(USER_HANDLE_FROM_INDEX(7,1), NULL, "unused valid-range slot rejected");
    HWND stale = USER_HANDLE_FROM_INDEX(0,2);
    thread_info.top_window = (uintptr_t)stale;
    expect_ptr(stale, NULL, "stale cached desktop generation rejected");
    check(!is_valid_entry(stale, NTUSER_OBJ_WINDOW), "stale entry predicate rejects");
    windows[1].parent = stale;
    check(!NtUserGetAncestor(CHILD, GA_ROOT), "stale desktop terminates invalid chain");
    fixture();session.user_entries[0].uniq = MAKELONG(NTUSER_OBJ_ICON, 1);
    expect_ptr(DESKTOP, NULL, "cached desktop with wrong object type rejected");
    expect_ptr(UlongToHandle(LOWORD(DESKTOP)), NULL, "truncated wrong-type desktop rejected");
    check(!is_valid_entry(DESKTOP, NTUSER_OBJ_WINDOW), "wrong-type entry predicate rejects");
    check(!NtUserGetAncestor(CHILD, GA_ROOT), "wrong-type desktop does not rescue chain");
    fixture();session.user_entries[0].uniq = 0;
    expect_ptr(DESKTOP, NULL, "freed cached desktop rejected");
    fixture();client_objects[1] = NULL;
    expect_ptr(ROOT, NULL, "same-PID non-desktop missing WND stays invalid");
    check(!NtUserGetAncestor(CHILD, GA_ROOT), "missing non-desktop parent fails traversal");
    fixture();session.user_entries[1].pid = 0x44;server_failure = TRUE;
    check(!NtUserGetAncestor(CHILD, GA_ROOT), "foreign invalid server chain fails traversal");
    check(!lock_depth, "invalid chains release all user locks");
}
static void case_eat(void)
{
    fixture();
    const UINT results[] = {MA_ACTIVATEANDEAT, MA_NOACTIVATEANDEAT};
    for (unsigned i = 0; i < 2; i++) {
        reset_activation();ma_result = results[i];
        check(!button(WM_LBUTTONDOWN, ROOT), "explicit MA_*ANDEAT still drops DOWN");
        check(activate_calls == 1 && notify_calls == 1, "explicit eat still notifies/activates");
        check(foreground_calls == (results[i] == MA_ACTIVATEANDEAT), "NOACTIVATEANDEAT does not set foreground");
    }
    reset_activation();ma_result = MA_NOACTIVATE;
    check(button(WM_LBUTTONDOWN, ROOT), "MA_NOACTIVATE retains DOWN");
    check(!foreground_calls, "MA_NOACTIVATE does not set foreground");
}
static void case_foreground_failure(void)
{
    fixture();foreground_failure = TRUE;
    check(!button(WM_LBUTTONDOWN, INACTIVE), "real inactive-root foreground failure still drops DOWN");
    check(foreground_calls == 1 && foreground_target == ROOT, "failure uses real root, not fallback pass");
    check(last_error == 5, "foreground failure status remains visible");
}
static void case_non_down(void)
{
    fixture();
    const UINT messages[] = {WM_LBUTTONUP, WM_MOUSEMOVE};
    /* Also test a broken chain: fixing lookup must not gate UP/MOVE on it. */
    for (unsigned broken = 0; broken < 2; broken++) {
        if (broken) client_objects[1] = NULL;
        for (unsigned i = 0; i < 2; i++) {
            reset_activation();foreground_failure = TRUE;
            check(button(messages[i], INACTIVE), "UP/MOVE retained independent of activation");
            check(!foreground_calls && !activate_calls && !notify_calls, "UP/MOVE does not enter DOWN activation");
        }
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "ready")) case_ready();
    else if (!strcmp(argv[1], "guards")) case_guards();
    else if (!strcmp(argv[1], "invalid")) case_invalid();
    else if (!strcmp(argv[1], "explicit-eat")) case_eat();
    else if (!strcmp(argv[1], "foreground-failure")) case_foreground_failure();
    else if (!strcmp(argv[1], "non-down")) case_non_down();
    else abort();
    check(!lock_depth, "final user lock balanced");
    if (errors) return 1;
    printf("MOUSE_ACTIVATION_HOST_PASS case=%s scope=EXTRACTED_ANCESTOR_AND_ACTIVATION_NOT_PS5_DELIVERY\n", argv[1]);
    return 0;
}
'''
args.out.mkdir(parents=True, exist_ok=True)
generated = args.out / 'test-title-mouse-activation.c'
generated.write_text(preamble + constants + layout + '\n' + backend + '\n' + helpers + '\n' +
                     wrapper + activation + '\n}\n' + cases)
binary = generated.with_suffix('')
# Preserve the real signed user_entry.uniq / unsigned snapshot comparison.
# Its existing Wine warning stays visible rather than rewriting the extraction.
subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-Wno-error=sign-compare', str(generated), '-o', str(binary)], check=True)
failed = []
for case in ('ready', 'guards', 'invalid', 'explicit-eat', 'foreground-failure', 'non-down'):
    if subprocess.run([str(binary), case], check=False).returncode:
        failed.append(case)
if failed:
    raise SystemExit('mouse activation contracts FAILED: ' + ', '.join(failed))
print('mouse activation contracts: production lookup/ancestor/activation-tail extraction PASS; PS5 delivery NOT RUN')
