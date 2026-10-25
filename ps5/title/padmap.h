/* Shared, allocation-free pad mapping. USB devices remain owned by pointer.c. */
#ifndef PS5_TITLE_PADMAP_H
#define PS5_TITLE_PADMAP_H
#include <stddef.h>
#include "pointer.h"

#define PADMAP_PATH "/data/runescape/pad-mapping.properties"
#define PADMAP_MAX_BYTES 16384
#define PADMAP_MAX_LINE 255
#define PADMAP_MAX_BINDINGS 64
#define PADMAP_MAX_ACTIONS 4
#define PADMAP_CHORD_US 60000u

enum padmap_profile_id { PADMAP_LAUNCHER, PADMAP_OSRS, PADMAP_PROFILES };
enum padmap_activator { PADMAP_HOLD, PADMAP_TAP, PADMAP_LONG, PADMAP_DOUBLE };
enum padmap_mode { PADMAP_OFF, PADMAP_MOUSE, PADMAP_ARROWS, PADMAP_WHEEL };
struct padmap_action { unsigned short atom[PADMAP_MAX_ACTIONS]; unsigned char count, control; };
struct padmap_binding {
    uint32_t buttons;
    unsigned char layer, present;
    struct padmap_action action[4];
};
struct padmap_profile {
    unsigned long_ms, double_ms, layer_threshold;
    unsigned char stick[2], invert[2], touch_pointer;
    unsigned count;
    struct padmap_binding binding[PADMAP_MAX_BINDINGS];
};
struct padmap_config { struct padmap_profile profile[PADMAP_PROFILES]; };
struct padmap_error { unsigned line; char text[320]; };
struct padmap_binding_state {
    uint64_t since, deadline, repeat_at;
    unsigned char down, active, pending, second, long_fired, consuming;
};
struct padmap {
    struct padmap_profile profile;
    struct padmap_binding_state state[PADMAP_MAX_BINDINGS];
    struct input_event sample;
    uint64_t last_frame;
    uint32_t buttons, consumed;
    unsigned short owners[288];
    unsigned char arrows[2][4];
    unsigned char layer, touch_half, ime_chord, draining, shift_toggle;
    float wheel_x, wheel_y, mouse_x, mouse_y;
    unsigned generation;
};

extern const char padmap_defaults[];
/* Transactional: a parse failure leaves *out unchanged. Errors contain path:line. */
int padmap_parse(struct padmap_config *out, const char *text, size_t size,
                 const char *path, struct padmap_error *error);
/* Creates missing parents (0777) and exclusively creates a 0666 default only
 * when absent; never modifies existing files, including malformed ones.
 * An interrupted write may leave a partial file, preserved for visible error
 * reporting rather than automatically overwriting externally editable data. */
int padmap_install(const char *path, struct padmap_error *error);
int padmap_load(struct padmap_config *out, const char *path, struct padmap_error *error);
void padmap_init(struct padmap *map, const struct padmap_config *config, enum padmap_profile_id profile);
void padmap_input(struct padmap *map, const struct input_event *event, uint64_t now,
                  pointer_handler handler, void *context);
void padmap_frame(struct padmap *map, uint64_t now, pointer_handler handler, void *context);
/* Releases every owned action and drains until a neutral pad sample. Safe from
 * inside the output callback (menu/IME acquisition). */
void padmap_reset(struct padmap *map, pointer_handler handler, void *context);
#endif
