#!/usr/bin/env python3
"""Exercise real metadata-only mouse trace helpers; not PS5 WM delivery."""
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
spec = spec_from_file_location('mouse_trace_extraction', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
driver = args.wine / 'dlls/win32u/driver.c'
message = args.wine / 'dlls/win32u/message.c'
queue = args.wine / 'server/queue.c'
private = args.wine / 'dlls/win32u/ntuser_private.h'

def enum_definition(path, name):
    text = path.read_text()
    start = text.index('enum ' + name)
    end = text.index('};', start) + 2
    definition = text[start:end]
    members = re.findall(r'\bPS5_MOUSE_[A-Z_]+\b', definition)
    assert members and len(members) == len(set(members)), name
    return definition, len(members)

message_enum, message_stages = enum_definition(message, 'ps5_mouse_trace_stage')
server_enum, server_stages = enum_definition(queue, 'ps5_mouse_route_stage')
driver_text = driver.read_text()
viewport_state = driver_text[driver_text.index('static pthread_mutex_t ps5_viewport_mutex'):
                             driver_text.index('void ps5_set_present_viewport')]
thread_state = extract.definition(private, 'ps5_mouse_diagnostics', structure=True)
# Keep the actual nested declaration/field name, not a separately maintained
# test version of the per-thread trace counters.
thread_state = 'struct user_thread_info {\n' + thread_state.rstrip(';') + ';\n};\n'
source_message = message.read_text()
activation = extract.definition(message, 'send_hardware_message')
assert 'flags & SEND_HWMSG_PS5_TITLE' in activation and 'input->type == INPUT_MOUSE' in activation
assert 'InterlockedExchange( &ps5_mouse_input_seen, 1 )' in activation
assert 'PS5_MOUSE_RAW' in extract.definition(message, 'process_hardware_message')
assert '"desktop-raw"' in extract.definition(message, 'process_hardware_message')
assert '"dispatch-lparam"' in source_message

harness = r'''
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wine/ps5_desktop.h"
#include "wine/ps5_title_input.h"
#include "wine/ps5_viewport.h"
typedef int BOOL, INT;
typedef int32_t LONG, NTSTATUS;
typedef uint32_t DWORD, UINT, ULONG, user_handle_t;
typedef uint16_t WCHAR, USHORT;
typedef void *HWND;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { USHORT Length, MaximumLength;WCHAR *Buffer; } UNICODE_STRING;
typedef struct { DWORD cbSize, flags;HWND hwndActive, hwndFocus, hwndCapture; } GUITHREADINFO;
typedef struct { DWORD type;struct { LONG dx, dy;DWORD mouseData, dwFlags, time;uintptr_t dwExtraInfo; } mi; } INPUT;
typedef struct { HWND hwnd;UINT message;WPARAM wParam;LPARAM lParam;DWORD time;POINT pt; } MSG;
#define FALSE 0
#define TRUE 1
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define HandleToULong(h) ((ULONG)(uintptr_t)(h))
#define LOWORD(v) ((uint16_t)(uintptr_t)(v))
#define HIWORD(v) ((uint16_t)((uintptr_t)(v)>>16))
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSEWHEEL 0x020a
#define WM_XBUTTONDOWN 0x020b
#define WM_XBUTTONUP 0x020c
#define WM_XBUTTONDBLCLK 0x020d
#define WM_MOUSEHWHEEL 0x020e
#define WM_NCMOUSEFIRST 0x00a0
#define WM_NCMOUSEMOVE 0x00a0
#define WM_NCMOUSELAST 0x00ad
#define WM_KEYDOWN 0x0100
#define WM_CHAR 0x0102
#define WindowThread 1
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GA_ROOT 2
#define MDT_DEFAULT 0
#define MDT_RAW_DPI 2
#define InterlockedCompareExchange(p,v,c) __sync_val_compare_and_swap((p),(c),(v))
#define InterlockedExchange(p,v) __atomic_exchange_n((p),(v),__ATOMIC_SEQ_CST)
#define InterlockedIncrement(p) __atomic_add_fetch((p),1,__ATOMIC_SEQ_CST)
static char lines[1024][1024];
static unsigned line_count, backend_calls, slot, class_mode;
static DWORD tick, last_error = 0x12345678;
static int fitted = 1, cursor_valid = 1;
static struct wine_ps5_viewport_map fake_map;
static unsigned raw_screen_calls, virtual_screen_calls;
static void touch_backend(void) { backend_calls++;last_error = 0xabcdef01; }
static void record_line(const char *text)
{
    assert(line_count < ARRAY_SIZE(lines) && strlen(text) < sizeof(lines[0]));
    strcpy(lines[line_count++], text);
}
static int sceKernelDebugOutText(int channel, const char *text)
{ assert(!channel);record_line(text);touch_backend();return 0; }
static int trace_fprintf(FILE *stream, const char *format, ...)
{
    char line[1024];va_list args;
    assert(stream == stderr);va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);va_end(args);
    assert(length >= 0 && length < (int)sizeof(line));record_line(line);return length;
}
static DWORD RtlGetLastWin32Error(void) { return last_error; }
static void RtlSetLastWin32Error(DWORD error) { last_error = error; }
static DWORD NtGetTickCount(void) { return tick; }
static unsigned get_tick_count(void) { return tick; }
static DWORD GetCurrentThreadId(void) { return slot ? 0x50 : 0x24; }
static UINT get_thread_dpi(void) { return 144; }
static UINT get_dpi_for_window(HWND hwnd) { assert(hwnd);return 192; }
static UINT get_win_monitor_dpi(HWND hwnd, UINT *raw) { assert(hwnd);*raw = 288;return 192; }
static HWND NtUserGetForegroundWindow(void) { touch_backend();return (HWND)0x100; }
static HWND NtUserQueryWindow(HWND hwnd, UINT query)
{ assert(query == WindowThread);touch_backend();return (HWND)(uintptr_t)(hwnd == (HWND)0x100 ? 0x50 : 0x24); }
static ULONG get_window_long(HWND hwnd, INT index)
{ assert(hwnd && (index == GWL_STYLE || index == GWL_EXSTYLE));touch_backend();return index == GWL_STYLE ? 0x15000000 : 0x100; }
static unsigned NtUserGetClassName(HWND hwnd, BOOL real, UNICODE_STRING *name)
{
    static const WCHAR normal[] = {'P','r','o','b','e',10,0x80,0x7f};
    assert(hwnd && !real);touch_backend();
    if (class_mode == 2) return 0;
    if (class_mode == 1) {
        unsigned count = name->MaximumLength / sizeof(WCHAR);
        for (unsigned i = 0; i < count; i++) name->Buffer[i] = 'Q';
        return count;
    }
    assert(name->MaximumLength >= sizeof(normal));memcpy(name->Buffer, normal, sizeof(normal));
    return ARRAY_SIZE(normal);
}
static BOOL NtUserGetCursorPos(POINT *point)
{ touch_backend();if (cursor_valid) *point = (POINT){300,450};return cursor_valid; }
static BOOL NtUserGetWindowRect(HWND hwnd, RECT *rect, UINT dpi)
{
    assert(dpi == 144);touch_backend();
    *rect = hwnd == (HWND)0x100 ? (RECT){90,170,930,870} : (RECT){100,200,900,800};return TRUE;
}
static BOOL NtUserGetClientRect(HWND hwnd, RECT *rect, UINT dpi)
{ assert(hwnd && dpi == 144);touch_backend();*rect = (RECT){0,0,800,600};return TRUE; }
static int NtUserMapWindowPoints(HWND from, HWND to, POINT *points, UINT count, UINT dpi)
{
    assert(from && !to && count == 2 && dpi == 144);touch_backend();
    unsigned char *memory = (void *)points;
    for (unsigned i = 0; i < count; i++) {
        POINT point;memcpy(&point, memory + i * sizeof(point), sizeof(point));
        point.x += 120;point.y += 240;memcpy(memory + i * sizeof(point), &point, sizeof(point));
    }
    return 0;
}
static RECT map_rect_virt_to_raw(RECT rect, UINT dpi)
{
    assert(dpi == 144);touch_backend();
    return (RECT){rect.left*2,rect.top*2,rect.right*2,rect.bottom*2};
}
static RECT get_virtual_screen_rect(UINT dpi, UINT type)
{
    if (type == MDT_RAW_DPI) { assert(!dpi);raw_screen_calls++;return (RECT){0,0,3840,2160}; }
    assert(type == MDT_DEFAULT && dpi == 144);virtual_screen_calls++;return (RECT){0,0,1920,1080};
}
static HWND NtUserGetAncestor(HWND hwnd, UINT kind)
{ assert(hwnd == (HWND)0x200 && kind == GA_ROOT);touch_backend();return (HWND)0x100; }
static BOOL NtUserGetGUIThreadInfo(DWORD thread, GUITHREADINFO *info)
{
    assert(thread == GetCurrentThreadId() && info->cbSize == sizeof(*info));touch_backend();
    info->hwndActive = (HWND)0x100;info->hwndFocus = (HWND)0x200;info->hwndCapture = (HWND)0x300;return TRUE;
}
static BOOL ps5_get_present_viewport(struct wine_ps5_viewport_map *map)
{ if (fitted) *map = fake_map;return fitted; }
struct message { unsigned int unique_id, msg, win;int x,y;unsigned time;uintptr_t hidden_payload; };
struct thread { unsigned int id; };
struct fake_input_shared { unsigned int capture; };
struct thread_input { struct fake_input_shared *shared; };
static struct thread current_thread = {0x50}, target_thread = {0x24};
static struct thread *current = &current_thread;
static struct { int active; } ps5_input_server_diag;
'''
harness += thread_state
harness += r'''
static struct user_thread_info users[2];
static struct user_thread_info *get_user_thread_info(void) { return &users[slot]; }
'''
harness += viewport_state + '\nstatic LONG ps5_mouse_input_seen;\n'
harness += message_enum + '\n' + server_enum + '\n'
harness += f'enum {{ MESSAGE_STAGE_COUNT = {message_stages}, SERVER_STAGE_COUNT = {server_stages} }};\n'
for name in ('ps5_log_mouse_window_geometry', 'ps5_log_mouse_geometry', 'ps5_log_mouse_packet'):
    harness += extract.definition(driver, name) + '\n'
for name in ('ps5_is_mouse_button_message', 'ps5_log_mouse_message', 'ps5_log_mouse_activation', 'ps5_note_message_pump'):
    harness += extract.definition(message, name) + '\n'
harness += '#define fprintf trace_fprintf\n' + extract.definition(queue, 'ps5_log_mouse_route') + '\n#undef fprintf\n'

cases = r'''
static unsigned records(const char *needle)
{
    unsigned result = 0;
    for (unsigned i = 0; i < line_count; i++) if (strstr(lines[i], needle)) result++;
    return result;
}
static const char *record_with(const char *needle)
{
    for (unsigned i = 0; i < line_count; i++) if (strstr(lines[i], needle)) return lines[i];
    assert(!"expected diagnostic row absent");return NULL;
}
static void safe_logs(void)
{
    for (unsigned i = 0; i < line_count; i++) {
        size_t length = strlen(lines[i]);
        assert(!strstr(lines[i], "SESSION_PAYLOAD_SENTINEL") && !strstr(lines[i], "decafbad"));
        for (size_t j = 0; j < length; j++)
            assert((lines[i][j] >= 0x20 && lines[i][j] < 0x7f) || (lines[i][j] == '\n' && j + 1 == length));
    }
}
static MSG mouse_message(UINT kind)
{
    MSG msg = {0};msg.hwnd = (HWND)0x200;msg.message = kind;
    msg.wParam = (WPARAM)(uintptr_t)"SESSION_PAYLOAD_SENTINEL";
    msg.lParam = (LPARAM)((uint32_t)(uint16_t)123 | ((uint32_t)(uint16_t)-45 << 16));
    msg.pt = (POINT){600,900};msg.time = 77;return msg;
}
static void packet(UINT kind)
{
    struct ps5_desktop_mouse mouse = {0}, before_mouse;
    INPUT input = {0}, before_input;
    mouse.kind = kind;mouse.button = PS5_DESKTOP_BUTTON_LEFT;mouse.down = 1;
    mouse.x = 12;mouse.y = 13;
    input.mi.dx = 321;input.mi.dy = 654;input.mi.dwFlags = 2;
    input.mi.dwExtraInfo = (uintptr_t)0xdecafbad;
    memcpy(&before_mouse, &mouse, sizeof(mouse));memcpy(&before_input, &input, sizeof(input));
    last_error = 0x12345678;
    ps5_log_mouse_packet(&mouse, &input, FALSE, 0);
    assert(last_error == 0x12345678);
    assert(!memcmp(&mouse, &before_mouse, sizeof(mouse)) && !memcmp(&input, &before_input, sizeof(input)));
}
static void case_driver(void)
{
    for (unsigned i = 0; i < 4; i++) { tick = 100 + i;packet(PS5_DESKTOP_MOUSE_MOVE); }
    assert(records("title mouse packet") == 4);
    unsigned backend_before = backend_calls;
    tick = 2102;packet(PS5_DESKTOP_MOUSE_MOVE); /* 1999 since fourth sample */
    assert(records("title mouse packet") == 4 && backend_calls == backend_before);
    tick = 2103;packet(PS5_DESKTOP_MOUSE_MOVE);assert(records("title mouse packet") == 5);
    for (unsigned i = 0; i < 65; i++) packet(PS5_DESKTOP_MOUSE_BUTTON);
    assert(records("title mouse packet") == 69 && records("kind=2 ") == 64);
    assert(strstr(record_with("title mouse packet"), "cursor_raw_x=600 cursor_raw_y=900"));
    assert(strstr(record_with("title mouse packet"), "mapped_x=321 mapped_y=654"));
    tick = UINT32_MAX - 1000;packet(PS5_DESKTOP_MOUSE_MOVE);
    unsigned before = records("title mouse packet");
    tick = 998;packet(PS5_DESKTOP_MOUSE_MOVE);assert(records("title mouse packet") == before);
    tick = 999;packet(PS5_DESKTOP_MOUSE_MOVE);assert(records("title mouse packet") == before + 1);
}
static void case_geometry(void)
{
    tick = 100;packet(PS5_DESKTOP_MOUSE_BUTTON);
    assert(raw_screen_calls == 1 && virtual_screen_calls == 1);
    const char *geometry = record_with("title mouse geometry");
    assert(strstr(geometry, "dpi=144 monitor_dpi=192 monitor_raw_dpi=288"));
    assert(strstr(geometry, "desktop_virtual=0,0,1920,1080 desktop_raw=0,0,3840,2160"));
    assert(strstr(geometry, "image=800x600 videoout=3840x2160 fit=480,0,2880,2160 origin=120,240 live_client=800x600"));
    const char *window = record_with("title mouse window role=viewport");
    assert(strstr(window, "class=Probe???") && strstr(window, "caller_dpi=144 window_dpi=192"));
    assert(strstr(window, "win_virtual=100,200,900,800 client_virtual=120,240,920,840"));
    assert(strstr(window, "win_desktop_raw=200,400,1800,1600 client_desktop_raw=240,480,1840,1680"));
    assert(records("title mouse window role=root") == 1);
    tick = 2099;packet(PS5_DESKTOP_MOUSE_BUTTON);assert(records("title mouse geometry") == 1);
    tick = 2100;packet(PS5_DESKTOP_MOUSE_BUTTON);assert(records("title mouse geometry") == 2);
    fitted = 0;tick = 4100;packet(PS5_DESKTOP_MOUSE_BUTTON);
    assert(records("title mouse geometry") == 3 && records("title mouse window") == 4);
    assert(strstr(lines[line_count - 1], "fitted=0") && strstr(lines[line_count - 1], "image=0x0 videoout=0x0"));
}
static void message_stage(enum ps5_mouse_trace_stage stage, UINT id, MSG *msg, const char *space)
{
    MSG before;memcpy(&before, msg, sizeof(before));last_error = 0x12345678;
    ps5_log_mouse_message(stage, "fixed-reason", msg, id, 1, (HWND)0x300, space);
    assert(last_error == 0x12345678 && !memcmp(&before, msg, sizeof(before)));
}
static void all_message_stages(UINT id)
{
    static const enum ps5_mouse_trace_stage stages[] = {PS5_MOUSE_RAW, PS5_MOUSE_HIT, PS5_MOUSE_FILTER,
        PS5_MOUSE_DROP, PS5_MOUSE_RETURN, PS5_MOUSE_PEEK, PS5_MOUSE_DISPATCH};
    _Static_assert(ARRAY_SIZE(stages) == MESSAGE_STAGE_COUNT, "every production trace stage is exercised");
    MSG msg = mouse_message(WM_MOUSEMOVE);
    for (unsigned i = 0; i < ARRAY_SIZE(stages); i++) {
        enum ps5_mouse_trace_stage stage = stages[i];
        UINT identity = stage == PS5_MOUSE_PEEK || stage == PS5_MOUSE_DISPATCH ? 0 : id;
        const char *space = stage == PS5_MOUSE_RAW ? "desktop-raw" :
                            stage == PS5_MOUSE_DISPATCH ? "dispatch-lparam" :
                            stage == PS5_MOUSE_RETURN ? "window-dpi" : "monitor-default";
        message_stage(stage, identity, &msg, space);message_stage(stage, identity, &msg, space);
    }
}
static void case_message(void)
{
    MSG msg = mouse_message(WM_MOUSEMOVE);
    message_stage(PS5_MOUSE_RAW, 1, &msg, "desktop-raw");assert(!line_count);
    InterlockedExchange(&ps5_mouse_input_seen, 1);
    message_stage(PS5_MOUSE_DISPATCH, 0, &msg, "dispatch-lparam");assert(!line_count);
    msg.message = WM_CHAR;message_stage(PS5_MOUSE_HIT, 1, &msg, "monitor-default");assert(!line_count);
    for (unsigned id = 1; id <= 4; id++) { tick = 100 + id;all_message_stages(id); }
    assert(line_count == 4 * MESSAGE_STAGE_COUNT && users[0].ps5_mouse_diag.move_samples == 4);
    tick = 2103;all_message_stages(5);assert(line_count == 4 * MESSAGE_STAGE_COUNT);
    tick = 2104;all_message_stages(5);assert(line_count == 5 * MESSAGE_STAGE_COUNT);
    assert(strstr(record_with("stage=raw "), "ht_valid=0") && strstr(record_with("stage=raw "), "capture_valid=0"));
    assert(strstr(record_with("stage=raw "), "space=desktop-raw"));
    assert(strstr(record_with("stage=dispatch-params "), "ht_valid=0") &&
           strstr(record_with("stage=dispatch-params "), "space=dispatch-lparam"));
    assert(strstr(record_with("stage=hit "), "ht_valid=1") && strstr(record_with("stage=return "), "space=window-dpi"));
    unsigned before = line_count;msg = mouse_message(WM_MOUSEMOVE);msg.lParam++;
    message_stage(PS5_MOUSE_DISPATCH, 0, &msg, "dispatch-lparam");assert(line_count == before);
    slot = 1;all_message_stages(99);
    assert(line_count == before + MESSAGE_STAGE_COUNT && users[1].ps5_mouse_diag.move_samples == 1);
    assert(users[0].ps5_mouse_diag.move_samples == 5);
    slot = 0;msg = mouse_message(WM_LBUTTONDOWN);before = line_count;
    for (unsigned i = 0; i < 193; i++) message_stage(PS5_MOUSE_HIT, 777, &msg, "monitor-default");
    assert(line_count == before + 192 && users[0].ps5_mouse_diag.click_lines == 192);
    slot = 1;message_stage(PS5_MOUSE_HIT, 778, &msg, "monitor-default");assert(users[1].ps5_mouse_diag.click_lines == 1);
    assert(ps5_is_mouse_button_message(WM_LBUTTONDOWN));
    assert(ps5_is_mouse_button_message(WM_XBUTTONDBLCLK));
    assert(ps5_is_mouse_button_message(WM_LBUTTONDOWN - (WM_MOUSEMOVE - WM_NCMOUSEMOVE)));
    assert(!ps5_is_mouse_button_message(WM_MOUSEMOVE) && !ps5_is_mouse_button_message(WM_MOUSEWHEEL));
}
static void note_pump(BOOL get, const MSG *msg)
{
    MSG before;if (msg) memcpy(&before, msg, sizeof(before));last_error = 0x12345678;
    ps5_note_message_pump(get, msg, (HWND)0x400, 0, 0xffff, 1);
    assert(last_error == 0x12345678 && (!msg || !memcmp(&before, msg, sizeof(before))));
}
static void case_pump(void)
{
    MSG msg = mouse_message(WM_LBUTTONUP);
    note_pump(FALSE, NULL);assert(!line_count && !users[0].ps5_mouse_diag.peek_calls);
    ps5_mouse_input_seen = 1;tick = 100;note_pump(FALSE, NULL);
    assert(line_count == 1 && users[0].ps5_mouse_diag.peek_calls == 1);
    tick = 101;note_pump(TRUE, NULL);note_pump(FALSE, &msg);
    msg.message = WM_CHAR;note_pump(TRUE, &msg);
    users[0].ps5_mouse_diag.dispatch_calls = 7;
    assert(line_count == 1 && users[0].ps5_mouse_diag.get_calls == 1 && users[0].ps5_mouse_diag.mouse_returns == 1);
    tick = 2099;note_pump(FALSE, NULL);assert(line_count == 1);
    tick = 2100;note_pump(TRUE, NULL);assert(line_count == 2);
    assert(strstr(lines[1], "peek=2 get=2 mouse_returned=1 dispatch_params=7"));
    assert(strstr(lines[1], "active=100 focus=200 capture=300 filter=400"));
    slot = 1;note_pump(TRUE, NULL);
    assert(line_count == 3 && users[1].ps5_mouse_diag.get_calls == 1 && !users[1].ps5_mouse_diag.peek_calls);
    assert(users[0].ps5_mouse_diag.get_calls == 2);
}
static void case_server(void)
{
    struct message msg = {77,WM_MOUSEMOVE,0x123,600,900,51,0xdecafbad}, before;
    struct fake_input_shared shared = {0x111};struct thread_input input = {&shared};
    memcpy(&before, &msg, sizeof(before));
    ps5_log_mouse_route(PS5_MOUSE_QUEUE, &msg, 0x200, &target_thread, &input, 0);
    assert(!line_count);ps5_input_server_diag.active = 1;
    for (unsigned stage = 0; stage < SERVER_STAGE_COUNT; stage++) {
        for (unsigned i = 0; i < 4; i++) { tick = 100 + i;ps5_log_mouse_route(stage, &msg, 0x200, &target_thread, &input, 0); }
        tick = 104;ps5_log_mouse_route(stage, &msg, 0x200, &target_thread, &input, 0);
    }
    assert(line_count == 4 * SERVER_STAGE_COUNT && !memcmp(&before, &msg, sizeof(before)));
    tick = 2102;
    for (unsigned stage = 0; stage < SERVER_STAGE_COUNT; stage++) ps5_log_mouse_route(stage, &msg, 0x200, &target_thread, &input, 0);
    assert(line_count == 4 * SERVER_STAGE_COUNT);
    tick = 2103;
    for (unsigned stage = 0; stage < SERVER_STAGE_COUNT; stage++) ps5_log_mouse_route(stage, &msg, 0x200, &target_thread, &input, 0);
    assert(line_count == 5 * SERVER_STAGE_COUNT);
    assert(strstr(lines[0], "requested=00000123 target=00000200 target_tid=0024 current_tid=0050 capture=00000111"));
    unsigned start = line_count;msg.msg = WM_LBUTTONDOWN;
    for (unsigned i = 0; i < 129; i++) ps5_log_mouse_route(PS5_MOUSE_QUEUE, &msg, 0, NULL, NULL, 0);
    assert(line_count == start + 128 && strstr(lines[start], "target_tid=0000") && strstr(lines[start], "capture=00000000"));
}
static void case_privacy(void)
{
    ps5_mouse_input_seen = 1;MSG msg = mouse_message(WM_CHAR);
    message_stage(PS5_MOUSE_HIT, 1, &msg, "monitor-default");assert(!line_count);
    msg.message = WM_KEYDOWN;message_stage(PS5_MOUSE_HIT, 1, &msg, "monitor-default");assert(!line_count);
    msg.message = WM_LBUTTONDOWN;message_stage(PS5_MOUSE_HIT, 1, &msg, "monitor-default");
    assert(strstr(lines[0], "class=Probe???"));
    class_mode = 1;message_stage(PS5_MOUSE_RETURN, 2, &msg, "window-dpi");
    const char *name = strstr(lines[1], "class=") + strlen("class=");
    const char *end = strchr(name, ' ');assert(end && end - name == 63);
    for (const char *at = name; at < end; at++) assert(*at == 'Q');
    class_mode = 2;message_stage(PS5_MOUSE_HIT, 3, &msg, "monitor-default");assert(strstr(lines[2], "class=- "));
    cursor_valid = 0;packet(PS5_DESKTOP_MOUSE_BUTTON);
    assert(strstr(record_with("title mouse packet"), "cursor_valid=0 cursor_raw_x=0 cursor_raw_y=0"));
}
static void case_activation(void)
{
    MSG msg = mouse_message(WM_LBUTTONDOWN), before;
    memcpy(&before, &msg, sizeof(before));
    ps5_log_mouse_activation("reply", &msg, (HWND)0x100, (HWND)0x100, 1, FALSE, FALSE, 0);
    assert(!line_count);
    ps5_mouse_input_seen = 1;last_error = 0x12345678;
    ps5_log_mouse_activation("reply", &msg, (HWND)0x100, (HWND)0x100, 1, FALSE, FALSE, 0);
    assert(line_count == 1 && last_error == 0x12345678 && !memcmp(&before, &msg, sizeof(msg)));
    assert(strstr(lines[0], "root=100 active_snapshot=100 foreground=100 ma=1 fg_attempted=0 fg_result=0"));
    ps5_log_mouse_activation("foreground", &msg, NULL, (HWND)0x100, 1, TRUE, FALSE, 0x578);
    assert(line_count == 2 && last_error == 0x12345678 && !memcmp(&before, &msg, sizeof(msg)));
    assert(strstr(lines[1], "root=0 ") && strstr(lines[1], "fg_attempted=1 fg_result=0 fg_last_error=0x578"));
    msg.message = WM_KEYDOWN;
    ps5_log_mouse_activation("reply", &msg, NULL, NULL, 0, FALSE, FALSE, 0);assert(line_count == 2);
    msg.message = WM_LBUTTONDOWN;
    for (unsigned i = 0; i < 65; i++)
        ps5_log_mouse_activation("reply", &msg, (HWND)0x100, (HWND)0x100, 3, FALSE, FALSE, 0);
    assert(line_count == 64 && last_error == 0x12345678);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(wine_ps5_viewport_fit(&fake_map.viewport, 800, 600, 3840, 2160));
    fake_map.origin_x = 120;fake_map.origin_y = 240;fake_map.client_width = 800;fake_map.client_height = 600;
    ps5_present_viewport.hwnd = (HWND)0x200;
    if (!strcmp(argv[1], "driver")) case_driver();
    else if (!strcmp(argv[1], "geometry")) case_geometry();
    else if (!strcmp(argv[1], "message")) case_message();
    else if (!strcmp(argv[1], "pump")) case_pump();
    else if (!strcmp(argv[1], "server")) case_server();
    else if (!strcmp(argv[1], "privacy")) case_privacy();
    else if (!strcmp(argv[1], "activation")) case_activation();
    else abort();
    safe_logs();printf("MOUSE_TRACE_HOST_PASS case=%s scope=EXTRACTED_METADATA_HELPERS_NOT_PS5_DELIVERY\n", argv[1]);
    return 0;
}
'''
args.out.mkdir(parents=True, exist_ok=True)
generated = args.out / 'test-title-mouse-trace.c'
generated.write_text(harness + cases)
binary = generated.with_suffix('')
subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-pthread', '-D__PROSPERO__', '-DWINE_PS5_RUNESCAPE=1', '-I', str(args.wine / 'include'),
    str(generated), '-o', str(binary)], check=True)
# Fresh process per case resets private static rate/cap state without test-only
# production hooks. No real GUI, SDK, game, network or credential state exists.
for case in ('driver', 'geometry', 'message', 'pump', 'server', 'privacy', 'activation'):
    subprocess.run([str(binary), case], check=True)
print('mouse trace contracts: real helper extraction, bounds/privacy/coordinate metadata PASS; PS5 delivery NOT RUN')
