/* Boundary substitutes only: no live CEF, native firmware, hub socket or server.
 * Exercise the real IME diff, title framing, packet conversion, server keyboard
 * queue construction, keyboard processing and TranslateMessage. This does not
 * reproduce CEF consuming the text. */
#ifndef TEST_TEXT_DIAG
#undef WINEPS5_TEXT_DIAG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define __WINESRC__
#define WINE_UNIX_LIB
#include "windef.h"
#include "winbase.h"
#include "ntuser.h"
#include "wine/ps5_title_input.h"
#undef SEND_HWMSG_INJECTED
#include "wine/server_protocol.h"
#include "kbd.h"

#include "../../title/ime.c"

static uint32_t pending_id;
static unsigned packet_down, packet_up, chars, frames, physical;
static uint16_t received[IME_MAX_TEXT];
static int queue_success = 1;
static unsigned diag_lines;
static char last_event[256], last_edit[160], last_packet[160], last_translate[128];
static int hardware_fail;

int sceKernelLoadStartModule(const char *path, size_t args, const void *argp,
                             uint32_t flags, const void *option, int *result)
{
    (void)path; (void)args; (void)argp; (void)flags; (void)option;
    *result = 0;
    return 1;
}
const char *sceKernelGetFsSandboxRandomWord(void) { return "sandbox"; }
int sceKernelDebugOutText(int channel, const char *text)
{
    assert(!channel && text);
    /* No log may contain the synthetic text or a code-unit dump. */
    assert(!strstr(text, "192") && !strstr(text, "0x39") && !strstr(text, "0039"));
    diag_lines++;
    if (strstr(text, "ime: event ")) snprintf(last_event, sizeof(last_event), "%s", text);
    if (strstr(text, "ime: edit ")) snprintf(last_edit, sizeof(last_edit), "%s", text);
    if (strstr(text, "packets=")) snprintf(last_packet, sizeof(last_packet), "%s", text);
    if (strstr(text, "text translate ")) snprintf(last_translate, sizeof(last_translate), "%s", text);
    return 0;
}
void sceImeParamInit(struct ime_param *param) { memset(param, 0, sizeof(*param)); }
int sceImeOpen(const struct ime_param *param, const void *extended)
{
    assert(param->text == buffer && !extended);
    return 0;
}
int sceImeClose(void) { return 0; }
int sceImeUpdate(void (*handler)(void *, const void *))
{
    /* Event ABI: id at +0, aligned payload at +8; text caret at +16.
     * Event 2 uses a movement direction, not the text union member. */
    struct { uint32_t id, padding; uint64_t payload[8]; } event = {pending_id, 0, {0}};
    if (pending_id == EVENT_TEXT)
        while (buffer[event.payload[1]]) event.payload[1]++;
    else if (pending_id == EVENT_CARET) event.payload[0] = 1;
    if (pending_id != UINT32_MAX) handler(NULL, &event);
    pending_id = UINT32_MAX;
    return 0;
}

static BOOL test_post(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    assert(hwnd == ULongToHandle(0x1111) && message == WM_CHAR && lparam == 1);
    if (!queue_success) return FALSE;
    assert(chars < ARRAY_SIZE(received));
    received[chars++] = (uint16_t)wparam;
    return TRUE;
}
static BOOL test_keyboard_state(BYTE *state) { memset(state, 0, 256); return TRUE; }
static HKL test_layout(DWORD thread) { assert(!thread); return 0; }
static INT test_unicode(UINT key, UINT scan, const BYTE *state, WCHAR *text,
                        int size, UINT flags, HKL layout)
{
    (void)key; (void)scan; (void)state; (void)text; (void)size; (void)flags; (void)layout;
    assert(!"Unicode packets must bypass layout conversion");
    return 0;
}
static BOOL test_imm(HWND hwnd, UINT message, WPARAM key, LPARAM data)
{
    (void)hwnd; (void)message; (void)key; (void)data;
    assert(!"Unicode packets must bypass IME translation");
    return FALSE;
}
#define NtUserPostMessage test_post
#define NtUserGetKeyboardState test_keyboard_state
#define NtUserGetKeyboardLayout test_layout
#define NtUserToUnicodeEx test_unicode
#define ImmTranslateMessage test_imm
#define FIXME(...) ((void)0)
#define TRACE_(channel) test_trace
#define test_trace(...) ((void)0)
struct user_thread_info { unsigned int packet_char; };
static struct user_thread_info thread_info;
static unsigned expected_scan;
static struct user_thread_info *get_user_thread_info(void) { return &thread_info; }
static LRESULT call_hooks(int hook, int code, WPARAM key, LPARAM data, UINT size)
{
    (void)code; (void)size;
    if (hook == WH_KEYBOARD && key == VK_PACKET) assert((HIWORD(data) & 0xff) == expected_scan);
    return 0;
}
#define check_hwnd_filter(msg, window) (!(window) || (msg)->hwnd == (window))
#define get_desktop_window() ULongToHandle(0x1000)
#define send_message(...) ((void)0)
#define is_menu_active() FALSE
#define accept_hardware_message(id) ((void)(id))
#define point_phys_to_win_dpi(window, point) (point)
#define ImmProcessKey(...) FALSE
#include "ime-process.inc"
#include "ime-translate.inc"

/* Server boundary fixtures; keyboard message construction below is verbatim. */
struct msg_queue;
struct thread;
struct desktop
{
    desktop_shm_t *shared;
    BYTE keystate[256], alt_pressed;
    struct
    {
        int enable;
        long long delay, period;
        union hw_input input;
        unsigned flags;
        user_handle_t win;
        void *timeout;
    } key_repeat;
};
struct message
{
    user_handle_t win;
    unsigned msg;
    lparam_t wparam, lparam;
    void *data;
};
static struct hardware_msg_data queued_data;
static struct message queued_message;
static unsigned low_scan, low_flags;
static unsigned get_tick_count(void) { return 1; }
static struct message *alloc_hardware_message(lparam_t info, struct hw_msg_source source,
                                               unsigned time, unsigned extra)
{
    (void)time; (void)extra;
    memset(&queued_data, 0, sizeof(queued_data));
    queued_data.info = info;
    queued_data.source = source;
    queued_message = (struct message){.data = &queued_data};
    return &queued_message;
}
static int send_hook_ll_message(struct desktop *desktop, struct message *message, int hook,
                                 lparam_t value, struct msg_queue *sender)
{
    (void)desktop; (void)sender;
    assert(message == &queued_message && hook == WH_KEYBOARD_LL);
    low_scan = HIWORD(value);
    low_flags = queued_data.flags;
    return 0;
}
static void queue_hardware_message(struct desktop *desktop, struct message *message, int always)
{ (void)desktop; assert(message == &queued_message && always); }
#define get_foreground_thread(...) ((struct thread *)NULL)
#define rawkeyboard_init(...) ((void)0)
#define dispatch_rawinput_message(...) ((void)0)
#define release_object(...) ((void)0)
#define stop_key_repeat(...) ((void)0)
#define remove_timeout_user(...) ((void)0)
#define add_timeout_user(timeout, ...) ((void)(timeout), NULL)
#define get_user_full_handle(win) (win)
#include "ime-queue.inc"

static BOOL test_hardware(HWND hwnd, UINT flags, const INPUT *input, LPARAM lparam)
{
    assert(!hwnd && !lparam && input->type == INPUT_KEYBOARD);
    if (hardware_fail) return (BOOL)0xc0000001u;
    if (input->ki.dwFlags & KEYEVENTF_UNICODE)
    {
        desktop_shm_t shared = {0};
        struct desktop desktop = {.shared = &shared};
        union hw_input hardware = {.kbd = {.vkey = input->ki.wVk, .scan = input->ki.wScan,
                                          .flags = input->ki.dwFlags}};
        MSG message;
        queue_keyboard_message(&desktop, 0x1111, &hardware, IMO_HARDWARE, NULL, flags, 0);
        message = (MSG){.hwnd = ULongToHandle(0x1111), .message = queued_message.msg,
                        .wParam = queued_message.wparam, .lParam = queued_message.lparam};
        /* A Unicode unit is not a physical scan code. Chromium derives DOM
         * code from these bits, so 0x39 must not masquerade as Space. */
        assert(((message.lParam >> 16) & 0xff) == 0);
        assert(low_scan == input->ki.wScan);
        assert(!!(low_flags & LLKHF_UP) == !!(input->ki.dwFlags & KEYEVENTF_KEYUP));
        assert(process_keyboard_message(&message, 1, &queued_data, 0, 0, ~0u, FALSE));
        assert(process_keyboard_message(&message, 1, &queued_data, 0, 0, ~0u, TRUE));
        assert(flags == SEND_HWMSG_NO_RAW && !input->ki.wVk);
        if (input->ki.dwFlags & KEYEVENTF_KEYUP)
        {
            packet_up++;
            assert(message.message == WM_KEYUP && message.lParam == (LPARAM)0xc0000001u);
        }
        else { packet_down++; assert(message.message == WM_KEYDOWN && message.lParam == 1); }
        assert(NtUserTranslateMessage(&message, 0));
    }
    else
    {
        assert(!flags && input->ki.wVk == '9' && input->ki.wScan == 0x0a);
        physical++;
    }
    return 0; /* NtUserSendHardwareInput returns NTSTATUS, zero is success. */
}
static HWND test_foreground(void) { return 0; }
static HANDLE test_query(HWND hwnd, WINDOWINFOCLASS info) { (void)hwnd; (void)info; return 0; }
static BOOL test_gui(DWORD thread, GUITHREADINFO *info) { (void)thread; (void)info; return FALSE; }
#define NtUserSendHardwareInput test_hardware
#define NtUserGetAsyncKeyboardState test_keyboard_state
#define NtUserGetForegroundWindow test_foreground
#define NtUserQueryWindow test_query
#define NtUserGetGUIThreadInfo test_gui
#include "wine/ps5_input.h"

static int hub_send(const void *message, size_t size)
{
    const struct ps5_desktop_text *text = message;
    assert(size == sizeof(*text) + text->length * sizeof(uint16_t));
    assert(text->header.type == PS5_DESKTOP_TEXT && text->header.size == size);
    frames++;
    send_text(text);
    return 1;
}
#define send_text title_send_text
#include "ime-title-send.inc"
#undef send_text

static void sequence(int caret_only_middle)
{
    static const uint16_t expected[] = {'1', '9', '2'};
    struct ime_edit edit;
    unsigned i;
    packet_down = packet_up = chars = frames = 0;
    assert(!ime_show(77, 1));
    for (i = 0; i < ARRAY_SIZE(expected); i++)
    {
        buffer[i] = expected[i];
        buffer[i + 1] = 0;
        pending_id = caret_only_middle && i == 1 ? 2 : EVENT_TEXT;
        if (ime_poll(&edit))
        {
            assert(!edit.erase && !edit.enter && !edit.cancel);
            title_send_text(edit.text, edit.length);
        }
        /* Repeated/no-change events cannot duplicate previously sent text. */
        pending_id = 2;
        assert(!ime_poll(&edit));
        assert(strstr(last_event, "move_valid=1 move=1"));
    }
    assert(chars == 3 && packet_down == 3 && packet_up == 3);
    assert(!memcmp(received, expected, sizeof(expected)));
    assert(frames == (caret_only_middle ? 2u : 3u));
    assert(strstr(last_edit, caret_only_middle ? "digit=2 letter=0 other=0" : "digit=1 letter=0 other=0"));
#ifdef WINEPS5_TEXT_DIAG
    assert(strstr(last_packet, "letter=0 other=0") && strstr(last_packet, "failed=0"));
    assert(strstr(last_translate, "queued=1 digit=1 letter=0 other=0"));
#endif
    pending_id = EVENT_CLOSE;
    assert(ime_poll(&edit) && edit.enter && !edit.length && !edit.erase);
    assert(!ime_is_open());
}

static void packet_edges(void)
{
    static const uint16_t units[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                                      'A', 'z', 0x00e9, 0xd83d, 0xde00, 0xffff, 0};
    union ps5_title_input wire;
    struct ps5_desktop_text *text = (void *)&wire;
    desktop_shm_t shared = {0};
    struct desktop desktop = {.shared = &shared};
    union hw_input input = {.kbd = {.vkey = VK_PACKET, .scan = 0x3c0}};
    MSG message;
    *text = (struct ps5_desktop_text){.length = ARRAY_SIZE(units)};
    memcpy(text->text, units, sizeof(units));
    chars = packet_down = packet_up = 0;
    send_text(text);
    assert(chars == ARRAY_SIZE(units) && packet_down == chars && packet_up == chars);
    assert(!memcmp(received, units, sizeof(units)));
    /* Explicit VK_PACKET without KEYEVENTF_UNICODE is still a raw key, not
     * the previous packet's character; its low scan byte remains visible. */
    queue_keyboard_message(&desktop, 0x1111, &input, IMO_HARDWARE, NULL, SEND_HWMSG_NO_RAW, 0);
    message = (MSG){.hwnd = ULongToHandle(0x1111), .message = queued_message.msg,
                    .wParam = queued_message.wparam, .lParam = queued_message.lparam};
    assert(HIWORD(message.lParam) == 0xc0 && !queued_data.unicode_char);
    expected_scan = 0xc0;
    assert(process_keyboard_message(&message, 1, &queued_data, 0, 0, ~0u, TRUE));
    expected_scan = 0;
    assert(NtUserTranslateMessage(&message, 0) && chars == ARRAY_SIZE(units));
    input.kbd.flags = KEYEVENTF_UNICODE;
    queue_keyboard_message(&desktop, 0x1111, &input, IMO_HARDWARE, NULL, SEND_HWMSG_NO_RAW, 0);
    message.lParam = queued_message.lparam;
    assert(!HIWORD(message.lParam) && low_scan == 0xc0);
    assert(process_keyboard_message(&message, 1, &queued_data, 0, 0, ~0u, TRUE));
    assert(NtUserTranslateMessage(&message, 0));
    assert(chars == ARRAY_SIZE(units) + 1 && received[chars - 1] == 0xc0);
    _Static_assert(sizeof(struct hardware_msg_data) == 48, "hardware metadata wire size unchanged");
    _Static_assert(offsetof(struct hardware_msg_data, unicode_char) == 12, "reuse protocol padding");
}

int main(void)
{
    struct ps5_desktop_key nine = {{PS5_DESKTOP_KEY, sizeof(nine)}, 0x26, 1, 0};
    sequence(0);
    /* Ignoring event 2 can delay a buffer change, but the next text update
     * must catch up including the middle 9; it cannot emit only 12. */
    sequence(1);
    packet_edges();
    send_key(&nine);
    nine.down = 0;
    send_key(&nine);
    assert(physical == 2 && diag_lines);
#ifdef WINEPS5_TEXT_DIAG
    {
        /* Counts distinguish queue acceptance from consumption. */
        union ps5_title_input message;
        struct ps5_desktop_text *text = (void *)&message;
        *text = (struct ps5_desktop_text){.length = 3};
        text->text[0] = 'Q'; text->text[1] = '?'; text->text[2] = '9';
        queue_success = 0;
        send_text(text);
        assert(strstr(last_translate, "queued=0 digit=1 letter=0 other=0"));
        assert(strstr(last_packet, "digit=1 letter=1 other=1 packets=6 failed=0"));
        hardware_fail = 1;
        send_text(text);
        assert(strstr(last_packet, "packets=6 failed=6"));
    }
#endif
    puts("title IME 1-9-2 -> maintenance frames -> Unicode packets -> WM_CHAR: PASS");
    return 0;
}
