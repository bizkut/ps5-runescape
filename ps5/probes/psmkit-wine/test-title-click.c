/* Trusted PE USER32 reference. Two GUI threads; no game, SDK device or credentials. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define TIMEOUT_MS 5000
#define PUMP_BATCH 64
#define COMMAND_CAPTURE (WM_APP + 1)
#define COMMAND_RELEASE (WM_APP + 2)
#define COMMAND_FOCUS (WM_APP + 3)
#define COMMAND_DRAIN (WM_APP + 4)

struct mouse_counts {
    LONG down, up, raw_down, raw_up, moves, order_errors, raw_errors, nc_edges;
    LONG down_key, up_key, x, y;
};
static struct mouse_counts parent_counts, child_counts;
static HWND parent, child;
static HANDLE ready, stop, changed;
static DWORD child_error;
static LONG quit_seen, command_serial, command_complete;
static struct {
    HWND capture, focus, active;
    SHORT key;
    DWORD queue_status;
    BOOL drained;
} command_result;
static int left_injected;

static unsigned count(LONG *value) { return (unsigned)InterlockedCompareExchange(value, 0, 0); }
static void print_counts(const char *phase)
{
    fprintf(stderr, "CLICK_HOST_STATE phase=%s parent=%u/%u child=%u/%u raw=%u/%u "
            "moves=%u/%u order=%u raw_errors=%u nc=%u\n", phase,
            count(&parent_counts.down), count(&parent_counts.up), count(&child_counts.down), count(&child_counts.up),
            count(&child_counts.raw_down), count(&child_counts.raw_up),
            count(&parent_counts.moves), count(&child_counts.moves),
            count(&parent_counts.order_errors) + count(&child_counts.order_errors), count(&child_counts.raw_errors),
            count(&parent_counts.nc_edges) + count(&child_counts.nc_edges));
}
static unsigned pump_pending(void)
{
    MSG message;
    unsigned processed = 0;
    while (processed < PUMP_BATCH && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) InterlockedExchange(&quit_seen, 1);
        else { TranslateMessage(&message);DispatchMessageW(&message); }
        processed++;
    }
    return processed;
}
static BOOL drain_current(void)
{
    DWORD deadline = GetTickCount() + TIMEOUT_MS;
    while (pump_pending() == PUMP_BATCH)
        if ((LONG)(GetTickCount() - deadline) >= 0) return FALSE;
    return TRUE;
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    struct mouse_counts *stats = window == child ? &child_counts : &parent_counts;
    const char *target = window == child ? "child" : "parent";
    if (message >= COMMAND_CAPTURE && message <= COMMAND_DRAIN) {
        BOOL drained = TRUE;
        if (message == COMMAND_CAPTURE) SetCapture(window);
        else if (message == COMMAND_RELEASE) ReleaseCapture();
        else if (message == COMMAND_FOCUS) SetFocus(window);
        else drained = drain_current();
        command_result.capture = GetCapture();command_result.focus = GetFocus();
        command_result.active = GetActiveWindow();command_result.key = GetKeyState(VK_LBUTTON);
        command_result.queue_status = GetQueueStatus(QS_MOUSE | QS_RAWINPUT);
        command_result.drained = drained;
        InterlockedExchange(&command_complete, (LONG)wparam);
        SetEvent(changed);return 0;
    }
    if (message == WM_MOUSEMOVE) {
        InterlockedExchange(&stats->x, (short)LOWORD(lparam));
        InterlockedExchange(&stats->y, (short)HIWORD(lparam));
        if (InterlockedIncrement(&stats->moves) == 1)
            printf("CLICK_HOST_MOVE target=%s x=%d y=%d\n", target, (short)LOWORD(lparam), (short)HIWORD(lparam));
        SetEvent(changed);
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
        SHORT key = GetKeyState(VK_LBUTTON);
        if (message == WM_LBUTTONDOWN) {
            if (count(&stats->down) != count(&stats->up)) InterlockedIncrement(&stats->order_errors);
            InterlockedExchange(&stats->down_key, (unsigned short)key);
            InterlockedIncrement(&stats->down);
        } else {
            if (count(&stats->down) != count(&stats->up) + 1) InterlockedIncrement(&stats->order_errors);
            InterlockedExchange(&stats->up_key, (unsigned short)key);
            InterlockedIncrement(&stats->up);
        }
        printf("CLICK_HOST_WM target=%s msg=%#x x=%d y=%d key=%#x flags=%#lx\n",
               target, message, (short)LOWORD(lparam), (short)HIWORD(lparam), (unsigned short)key, (unsigned long)wparam);
        SetEvent(changed);
    }
    if (message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONUP) {
        InterlockedIncrement(&stats->nc_edges);SetEvent(changed);
    }
    if (message == WM_INPUT) {
        RAWINPUT input;UINT bytes = sizeof(input);
        UINT received = GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER));
        if (received == (UINT)-1 || received < sizeof(RAWINPUTHEADER)) InterlockedIncrement(&stats->raw_errors);
        else if (input.header.dwType == RIM_TYPEMOUSE) {
            USHORT flags = input.data.mouse.usButtonFlags;
            if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) {
                if (count(&stats->raw_down) != count(&stats->raw_up)) InterlockedIncrement(&stats->order_errors);
                InterlockedIncrement(&stats->raw_down);
            }
            if (flags & RI_MOUSE_LEFT_BUTTON_UP) {
                if (count(&stats->raw_down) != count(&stats->raw_up) + 1) InterlockedIncrement(&stats->order_errors);
                InterlockedIncrement(&stats->raw_up);
            }
            if (flags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP))
                printf("CLICK_HOST_RAW target=%s flags=%#x dx=%ld dy=%ld\n",
                       target, flags, input.data.mouse.lLastX, input.data.mouse.lLastY);
        }
        SetEvent(changed);
    }
    /* Foreground WM_INPUT cleanup remains DefWindowProc's responsibility. */
    return DefWindowProcW(window, message, wparam, lparam);
}

typedef BOOL (*predicate)(void *);
static BOOL wait_pumping(HANDLE handle, predicate satisfied, void *context, const char *phase)
{
    DWORD deadline = GetTickCount() + TIMEOUT_MS;
    HANDLE handles[2];unsigned number = 0;
    if (handle) handles[number++] = handle;
    handles[number++] = changed;
    for (;;) {
        DWORD state, remaining;
        pump_pending();
        if (handle) {
            state = WaitForSingleObject(handle, 0);
            if (state == WAIT_OBJECT_0) return TRUE;
            if (state != WAIT_TIMEOUT) break;
        }
        if (satisfied && satisfied(context)) return TRUE;
        if (count(&quit_seen) || (LONG)(GetTickCount() - deadline) >= 0) break;
        remaining = deadline - GetTickCount();
        if (remaining > TIMEOUT_MS) remaining = 0;
        state = MsgWaitForMultipleObjectsEx(number, handles, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (handle && state == WAIT_OBJECT_0) return TRUE;
        if (state == WAIT_FAILED) break;
    }
    fprintf(stderr, "CLICK_HOST_WAIT_FAILED phase=%s error=%lu\n", phase, GetLastError());
    print_counts(phase);return FALSE;
}
static DWORD WINAPI render_thread(void *unused)
{
    (void)unused;
    child = CreateWindowW(L"TitleClickHostChild", L"", WS_CHILD | WS_VISIBLE,
                          20, 30, 600, 400, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!child) child_error = GetLastError();
    SetEvent(ready);
    if (!child) return 2;
    for (;;) {
        DWORD state;
        pump_pending();
        state = MsgWaitForMultipleObjectsEx(1, &stop, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (state == WAIT_OBJECT_0 || state == WAIT_FAILED || count(&quit_seen)) break;
    }
    DestroyWindow(child);return 0;
}
static BOOL command_finished(void *context)
{ return count(&command_complete) == *(unsigned *)context; }
static BOOL owner_command(UINT message, const char *phase)
{
    unsigned serial = (unsigned)InterlockedIncrement(&command_serial);
    if (!PostMessageW(child, message, serial, 0)) return FALSE;
    if (!wait_pumping(NULL, command_finished, &serial, phase)) return FALSE;
    printf("CLICK_HOST_OWNER phase=%s capture=%p focus=%p active=%p key=%#x queue=%#lx drained=%u\n",
           phase, (void *)command_result.capture, (void *)command_result.focus, (void *)command_result.active,
           (unsigned)(USHORT)command_result.key, command_result.queue_status, (unsigned)command_result.drained);
    return command_result.drained;
}
static BOOL drain_both(void)
{
    /* SendInput has returned and this probe is the only producer. Explicitly
     * drain both owners instead of sleeping before a NOLEGACY absence oracle. */
    return owner_command(COMMAND_DRAIN, "drain-child") && drain_current();
}
static void reset_counts(void)
{
    struct mouse_counts *all[] = {&parent_counts, &child_counts};
    for (unsigned i = 0; i < 2; i++) {
#define CLEAR(field) InterlockedExchange(&all[i]->field, 0)
        CLEAR(down);CLEAR(up);CLEAR(raw_down);CLEAR(raw_up);CLEAR(moves);CLEAR(order_errors);
        CLEAR(raw_errors);CLEAR(nc_edges);CLEAR(down_key);CLEAR(up_key);CLEAR(x);CLEAR(y);
#undef CLEAR
    }
}
struct edge_wait { unsigned legacy, up; };
static BOOL edges_ready(void *context)
{
    const struct edge_wait *edge = context;
    return count(&child_counts.raw_down) >= 1 && count(&child_counts.raw_up) >= edge->up &&
           (!edge->legacy || (count(&child_counts.down) >= 1 && count(&child_counts.up) >= edge->up));
}
static BOOL wait_edges(unsigned legacy, unsigned up, const char *phase)
{
    struct edge_wait edge = {legacy, up};
    return wait_pumping(NULL, edges_ready, &edge, phase);
}
struct move_wait { POINT point;unsigned after; };
static BOOL move_ready(void *context)
{
    const struct move_wait *move = context;
    return count(&child_counts.moves) > move->after &&
           (LONG)count(&child_counts.x) == move->point.x && (LONG)count(&child_counts.y) == move->point.y;
}
static BOOL move_once(POINT requested, POINT *actual, const char *phase)
{
    struct move_wait move = {.after = count(&child_counts.moves)};
    if (!SetCursorPos(requested.x, requested.y) || !GetCursorPos(actual)) return FALSE;
    move.point = *actual;
    if (!ScreenToClient(child, &move.point)) return FALSE;
    return wait_pumping(NULL, move_ready, &move, phase);
}
static BOOL move_cursor(POINT *screen, const char *phase)
{
    POINT current, parked;
    if (!GetCursorPos(&current)) return FALSE;
    if (current.x == screen->x && current.y == screen->y) {
        /* A repeated probe can inherit this exact cursor position. Require a
         * real, acknowledged MOVE instead of waiting for a nonexistent warp. */
        if (!move_once((POINT){screen->x + 16, screen->y}, &parked, phase)) return FALSE;
    }
    return move_once(*screen, screen, phase);
}
static BOOL foreground_ready(void *unused)
{
    GUITHREADINFO info = {.cbSize = sizeof(info)};
    (void)unused;
    return GetForegroundWindow() == parent && GetGUIThreadInfo(GetCurrentThreadId(), &info) &&
           info.hwndActive == parent && info.hwndFocus == child;
}
static BOOL inject(DWORD flags)
{
    INPUT input = {.type = INPUT_MOUSE};input.mi.dwFlags = flags;
    if (SendInput(1, &input, sizeof(input)) != 1) return FALSE;
    if (flags & MOUSEEVENTF_LEFTDOWN) left_injected = 1;
    if (flags & MOUSEEVENTF_LEFTUP) left_injected = 0;
    return TRUE;
}
static BOOL inject_tap(void)
{
    INPUT input[2] = {{.type = INPUT_MOUSE}, {.type = INPUT_MOUSE}};
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    left_injected = 1; /* Cleanup must release a partial two-input insertion. */
    if (SendInput(2, input, sizeof(input[0])) != 2) return FALSE;
    left_injected = 0;return TRUE;
}
static BOOL check_edges(unsigned legacy, const char *phase)
{
    BOOL valid = count(&child_counts.raw_down) == 1 && count(&child_counts.raw_up) == 1 &&
                 count(&child_counts.down) == legacy && count(&child_counts.up) == legacy &&
                 !count(&parent_counts.down) && !count(&parent_counts.up) &&
                 !count(&parent_counts.raw_down) && !count(&parent_counts.raw_up) &&
                 !count(&parent_counts.order_errors) && !count(&child_counts.order_errors) &&
                 !count(&parent_counts.raw_errors) && !count(&child_counts.raw_errors) &&
                 !count(&parent_counts.nc_edges) && !count(&child_counts.nc_edges);
    if (legacy) valid = valid && (count(&child_counts.down_key) & 0x8000) && !(count(&child_counts.up_key) & 0x8000);
    if (!valid) print_counts(phase);
    return valid;
}

int main(void)
{
    WNDCLASSW cls = {.lpfnWndProc = window_proc, .hInstance = GetModuleHandleW(NULL),
                    .lpszClassName = L"TitleClickHostParent"};
    HANDLE thread = NULL;
    DWORD main_id = GetCurrentThreadId(), child_id = 0;
    BOOL attached = FALSE;
    POINT point = {123, 145};
    RECT parent_rect, child_rect, client_rect;
    RAWINPUTDEVICE raw = {.usUsagePage = 1, .usUsage = 2};
    int result = 2;
#define REQUIRE(test, code, phase) do { if (!(test)) { \
    fprintf(stderr, "CLICK_HOST_FAIL phase=%s error=%lu\n", phase, GetLastError()); \
    print_counts(phase);result = code;goto cleanup; } } while (0)
    ready = CreateEventW(NULL, TRUE, FALSE, NULL);stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    REQUIRE(ready && stop && changed, 2, "events");
    REQUIRE(RegisterClassW(&cls), 2, "register-parent");
    cls.lpszClassName = L"TitleClickHostChild";REQUIRE(RegisterClassW(&cls), 2, "register-child");
    parent = CreateWindowW(L"TitleClickHostParent", L"Input API probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          100, 100, 800, 600, NULL, NULL, cls.hInstance, NULL);
    REQUIRE(parent, 2, "parent-window");
    thread = CreateThread(NULL, 0, render_thread, NULL, 0, &child_id);REQUIRE(thread, 2, "render-thread");
    REQUIRE(wait_pumping(ready, NULL, NULL, "child-ready") && child && !child_error, 2, "child-window");
    REQUIRE(GetWindowThreadProcessId(parent, NULL) == main_id &&
            GetWindowThreadProcessId(child, NULL) == child_id && main_id != child_id, 2, "two-gui-owners");
    REQUIRE(AttachThreadInput(main_id, child_id, TRUE), 2, "attach-input");attached = TRUE;
    REQUIRE(SetForegroundWindow(parent), 2, "foreground");SetActiveWindow(parent);
    REQUIRE(GetAncestor(child, GA_PARENT) == parent && GetAncestor(child, GA_ROOT) == parent &&
            GetAncestor(parent, GA_ROOT) == parent, 1, "child-top-level-ancestor");
    REQUIRE(owner_command(COMMAND_FOCUS, "focus-child") && command_result.focus == child,
            2, "owner-focus");
    REQUIRE(wait_pumping(NULL, foreground_ready, NULL, "foreground-focus"), 2, "foreground-focus");
    REQUIRE(ClientToScreen(child, &point) && move_cursor(&point, "child-move"), 1, "child-move");
    REQUIRE(WindowFromPoint(point) == child, 1, "child-hit-test");
    REQUIRE(GetWindowRect(parent, &parent_rect) && GetWindowRect(child, &child_rect) &&
            GetClientRect(child, &client_rect), 2, "rectangles");
    printf("CLICK_HOST_FOCUS foreground=%p focus=%p hit=%p child=%p point=%ld,%ld owner_threads=%lu/%lu\n",
           (void *)GetForegroundWindow(), (void *)command_result.focus, (void *)WindowFromPoint(point),
           (void *)child, point.x, point.y, main_id, child_id);
    printf("CLICK_HOST_RECTS parent=%ld,%ld,%ld,%ld child=%ld,%ld,%ld,%ld client=%ld,%ld,%ld,%ld\n",
           parent_rect.left, parent_rect.top, parent_rect.right, parent_rect.bottom,
           child_rect.left, child_rect.top, child_rect.right, child_rect.bottom,
           client_rect.left, client_rect.top, client_rect.right, client_rect.bottom);
    for (unsigned mode = 0; mode < 2; mode++) {
        unsigned legacy = !mode;
        raw.dwFlags = mode ? RIDEV_NOLEGACY : 0;raw.hwndTarget = child;
        REQUIRE(RegisterRawInputDevices(&raw, 1, sizeof(raw)) && drain_both(), 2, "raw-mode");
        reset_counts();REQUIRE(!(GetAsyncKeyState(VK_LBUTTON) & 0x8000), 1, "initial-released");
        REQUIRE(inject(MOUSEEVENTF_LEFTDOWN) && wait_edges(legacy, 0, "held-down"), 1, "held-down");
        SHORT held = GetAsyncKeyState(VK_LBUTTON);
        REQUIRE((held & 0x8000) && !count(&child_counts.raw_up) && !count(&child_counts.up), 1, "held-state");
        REQUIRE(inject(MOUSEEVENTF_LEFTUP) && wait_edges(legacy, 1, "held-up") && drain_both(), 1, "held-up");
        SHORT released = GetAsyncKeyState(VK_LBUTTON);
        printf("CLICK_HOST_RESULT mode=%s parent=%u child=%u raw=%u held_async=%#x held_key=%#x released_async=%#x moves=%u\n",
               mode ? "raw-no-legacy" : "legacy-and-raw", count(&parent_counts.down) + count(&parent_counts.up),
               count(&child_counts.down) + count(&child_counts.up), count(&child_counts.raw_down) + count(&child_counts.raw_up),
               (unsigned short)held, count(&child_counts.down_key), (unsigned short)released,
               count(&parent_counts.moves) + count(&child_counts.moves));
        REQUIRE(!(released & 0x8000) && check_edges(legacy, "held-oracle"), 1, "held-oracle");
        reset_counts();GetAsyncKeyState(VK_LBUTTON);
        REQUIRE(inject_tap() && wait_edges(legacy, 1, "tap-edges") && drain_both(), 1, "tap-edges");
        SHORT tap = GetAsyncKeyState(VK_LBUTTON), again = GetAsyncKeyState(VK_LBUTTON);
        printf("CLICK_HOST_TAP mode=%s first=%#x second=%#x lowbit=diagnostic_global_consumable\n",
               mode ? "raw-no-legacy" : "legacy-and-raw", (unsigned short)tap, (unsigned short)again);
        REQUIRE(!((tap | again) & 0x8000) && check_edges(legacy, "tap-oracle"), 1, "tap-oracle");
    }
    raw.dwFlags = 0;raw.hwndTarget = child;
    REQUIRE(RegisterRawInputDevices(&raw, 1, sizeof(raw)) && drain_both(), 2, "capture-mode");
    REQUIRE(owner_command(COMMAND_CAPTURE, "capture-owner") && command_result.capture == child, 1, "capture-owner");
    REQUIRE(GetClientRect(parent, &client_rect) && client_rect.right > 660 && client_rect.bottom > 470,
            2, "outside-parent-client");
    POINT outside = {client_rect.right - 20, client_rect.bottom - 20};
    REQUIRE(ClientToScreen(parent, &outside), 2, "outside-screen");
    reset_counts();REQUIRE(move_cursor(&outside, "capture-move"), 1, "capture-move");
    REQUIRE(WindowFromPoint(outside) == parent, 1, "outside-child-hit");
    REQUIRE(inject(MOUSEEVENTF_LEFTDOWN) && wait_edges(1, 0, "capture-down"), 1, "capture-down");
    REQUIRE(GetAsyncKeyState(VK_LBUTTON) & 0x8000, 1, "capture-held");
    REQUIRE(inject(MOUSEEVENTF_LEFTUP) && wait_edges(1, 1, "capture-up") && drain_both(), 1, "capture-up");
    REQUIRE(owner_command(COMMAND_DRAIN, "capture-query") && command_result.capture == child &&
            check_edges(1, "capture-oracle"), 1, "capture-oracle");
    printf("CLICK_HOST_CAPTURE owner_capture=%p child=%p delivered=%u\n",
           (void *)command_result.capture, (void *)child, count(&child_counts.down) + count(&child_counts.up));
    REQUIRE(owner_command(COMMAND_RELEASE, "release-owner") && !command_result.capture, 1, "release-owner");
    result = 0;
cleanup:
    if (left_injected) { INPUT up = {.type = INPUT_MOUSE};up.mi.dwFlags = MOUSEEVENTF_LEFTUP;SendInput(1, &up, sizeof(up)); }
    if (child && IsWindow(child)) owner_command(COMMAND_RELEASE, "cleanup-release");
    raw.dwFlags = RIDEV_REMOVE;raw.hwndTarget = NULL;RegisterRawInputDevices(&raw, 1, sizeof(raw));
    if (attached) { SetFocus(parent);AttachThreadInput(main_id, child_id, FALSE); }
    if (stop) SetEvent(stop);
    if (thread) { if (!wait_pumping(thread, NULL, NULL, "thread-stop")) result = 2;CloseHandle(thread); }
    if (parent && IsWindow(parent)) DestroyWindow(parent);
    if (ready) CloseHandle(ready);if (stop) CloseHandle(stop);if (changed) CloseHandle(changed);
    if (!result) puts("CLICK_HOST_API_PASS scope=USER32_REFERENCE_NOT_PS5 gui_threads=2 button=left");
    return result;
#undef REQUIRE
}
