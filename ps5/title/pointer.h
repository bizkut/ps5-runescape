/* USB mouse/keyboard and the legacy pad pointer for native-menu navigation.
 * Product controller mapping lives in padmap.c; mapped gameplay/launcher samples
 * must not also enter this legacy pad path. Physical USB events always pass
 * through. Positions are shared with padmap and programmatic cursor warps. */
#ifndef PS5_TITLE_POINTER_H
#define PS5_TITLE_POINTER_H

#include <stdint.h>
#include "input.h"

enum pointer_kind
{
    POINTER_MOVE,   /* x, y: new position on the display */
    POINTER_BUTTON, /* code: MOUSE_*; down */
    POINTER_WHEEL,  /* dy, dx: in 120ths of a detent, as WHEEL_DELTA */
    POINTER_KEY,    /* code: USB HID usage; down; repeat: an autorepeat */
    POINTER_TEXT,   /* the user asked for the on-screen keyboard */
    POINTER_MENU,   /* out-of-band native menu; never a client key */
};

struct pointer_event
{
    enum pointer_kind kind;
    uint32_t code;
    int down, repeat;
    int x, y, dx, dy;
};

typedef void (*pointer_handler)(const struct pointer_event *event, void *context);

void pointer_init(int width, int height);
/* Legacy/native-menu pad fallback only; mapped Options belongs to padmap.c. */
void pointer_options_escape(int enabled);
/* Invert only USB mouse wheel/tilt; touchpad and stick scroll are unchanged. */
void pointer_set_invert_scroll(int enabled);
/* External padmap owns all pad events; USB input and cursor warps are intact. */
void pointer_set_pad_mapping(int enabled);
/* Native-menu-only pad navigation, never forwarded alongside a mapped sample. */
void pointer_menu_input(const struct input_event *event, pointer_handler handler, void *context);
/* Feeds one input event. */
void pointer_input(const struct input_event *event, pointer_handler handler, void *context);
/* Once a frame, after the frame's input: stick motion, scrolling and key
 * repeat; now_us is a monotonic time. */
void pointer_frame(uint64_t now_us, pointer_handler handler, void *context);
void pointer_position(int *x, int *y);
/* A program's cursor warp, clamped to the display; it does not emit input. */
void pointer_set_position(int x, int y);
/* Forget every held button/key and motion accumulator without emitting an
 * event. The owner uses this while switching input away from Wine. */
void pointer_cancel(void);

#endif
