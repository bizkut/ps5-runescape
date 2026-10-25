/* Real main.c adapters, with the hub and native UI/device boundaries stubbed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wine/ps5_desktop.h>
#include "../../title/padmap.h"
#include "../../title/forever-menu.h"
#define PS5_TITLE_EMBEDDED 1
#define KEY_NATIVE_MENU 0x68u
#define KEY_ENTER 0x28u
#define KEY_ESCAPE 0x29u
#define KEY_BACKSPACE 0x2au
#define KEY_RIGHT 0x4fu
#define KEY_LEFT 0x50u
#define KEY_DOWN 0x51u
#define KEY_UP 0x52u
static struct padmap pad_mapping;
static struct forever_menu_state menu;
static unsigned char physical_keys[256], forwarded_keys[256];
static uint32_t physical_mouse, physical_pad, menu_pad, forwarded_mouse;
static int user = 1, input_draining, menu_ime_command, osk_open, osk_calls, menu_calls;
static uint64_t test_now = 1000000;
static uint64_t now_us(void) { return test_now; }
static struct { struct ps5_desktop_key key; struct ps5_desktop_mouse mouse; } sent[64];
static unsigned sent_count;
static int hub_send(const void *message, size_t size)
{
    const struct ps5_desktop_header *header = message;
    assert(sent_count < 64);
    memset(&sent[sent_count], 0, sizeof(sent[sent_count]));
    if (header->type == PS5_DESKTOP_KEY) memcpy(&sent[sent_count].key, message, size);
    else { assert(header->type == PS5_DESKTOP_MOUSE); memcpy(&sent[sent_count].mouse, message, size); }
    sent_count++;
    return 1;
}
static void compositor_cursor_wake(void) {}
static int compositor_menu_line_at(int x, int y) { (void)x; (void)y; return -1; }
static void title_desktop_installer_user_input(void) {}
static int ime_is_open(void) { return osk_open; }
static int ime_start(int id) { assert(id == 1); osk_calls++; osk_open = 1; return 0; }
static void render_menu(void) {}
static void menu_key(uint32_t usage) { (void)usage; }
static void apply_menu_result(struct forever_menu_result result) { (void)result; }
static int physical_input_neutral(void)
{
    if (physical_pad || physical_mouse) return 0;
    for (unsigned i = 0; i < sizeof(physical_keys); i++) if (physical_keys[i]) return 0;
    return 1;
}
static void on_pointer(const struct pointer_event *, void *);
static void open_menu(void)
{
    padmap_reset(&pad_mapping, on_pointer, NULL);
    struct forever_menu_values values = {0};
    forever_menu_open(&menu, &values);
    menu_pad = physical_pad;
    menu_calls++;
}
static void close_menu(void)
{
    padmap_reset(&pad_mapping, on_pointer, NULL);
    forever_menu_close(&menu);
    pointer_cancel();
    input_draining = !physical_input_neutral();
    menu_calls++;
}
#include "maintenance-padmap.inc"

int main(void)
{
    struct padmap_config config;
    struct padmap_error error;
    assert(!padmap_parse(&config, padmap_defaults, strlen(padmap_defaults), "defaults", &error));
    padmap_init(&pad_mapping, &config, PADMAP_LAUNCHER);
    pointer_init(1920, 1080);
    pointer_set_pad_mapping(1);
    forever_menu_init(&menu);
    struct input_event pad = {.kind = INPUT_PAD, .connected = 1, .lx = 128, .ly = 128, .rx = 128, .ry = 128};
    pad.code = PAD_CROSS;
    on_input(&pad, NULL);
    assert(sent_count == 1 && sent[0].mouse.button == PS5_DESKTOP_BUTTON_LEFT && sent[0].mouse.down);
    pad.code = 0;
    on_input(&pad, NULL);
    assert(sent_count == 2 && !sent[1].mouse.down);
    struct input_event key = {.kind = INPUT_KEY, .code = 0x45, .down = 1};
    on_input(&key, NULL);
    assert(sent_count == 3 && sent[2].key.usage == 0x45 && sent[2].key.down && !menu_calls);
    key.down = 0;
    on_input(&key, NULL);
    assert(sent_count == 4 && !sent[3].key.down);
    /* Physical F13 is an out-of-band menu trigger; F12 is never consumed. */
    key.code = KEY_NATIVE_MENU; key.down = 1;
    on_input(&key, NULL);
    assert(forever_menu_is_open(&menu) && menu_calls == 1 && sent_count == 4);
    key.down = 0;
    on_input(&key, NULL);
    pad.code = PAD_CREATE | PAD_OPTIONS;
    on_input(&pad, NULL);
    assert(!forever_menu_is_open(&menu) && menu_calls == 2 && input_draining);
    pad.code = PAD_CROSS;
    on_input(&pad, NULL);
    assert(sent_count == 4); /* Menu close drains held pad controls. */
    pad.code = 0;
    on_input(&pad, NULL);
    on_input(&pad, NULL); /* Shared engine also observes neutral after owner drain. */
    pad.code = PAD_L3 | PAD_R3;
    on_input(&pad, NULL);
    pad.code = PAD_R3;
    on_input(&pad, NULL);
    assert(osk_calls == 1 && osk_open && sent_count == 4);
    pad.code = PAD_CROSS;
    on_input(&pad, NULL);
    assert(sent_count == 4); /* No mapped click while the IME owns the pad. */
    osk_open = 0;
    pad.code = 0;
    on_input(&pad, NULL);
    padmap_init(&pad_mapping, &config, PADMAP_OSRS);
    pad.code = PAD_DOWN; pad.r2 = 64;
    on_input(&pad, NULL);
    assert(sent_count == 5 && sent[4].key.usage == 0x45 && sent[4].key.down);
    assert(menu_calls == 2); /* OSRS layered F12 reaches the actual hub adapter. */
    pad.code = 0; pad.r2 = 0;
    on_input(&pad, NULL);
    assert(sent_count == 6 && !sent[5].key.down);
    puts("maintenance mapper hub/F12/menu/IME/drain adapters: PASS");
    return 0;
}
