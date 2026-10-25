#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "padmap.h"

#define TOUCH_LEFT (1u << 24)
#define TOUCH_RIGHT (1u << 25)
#define L2_FULL (1u << 26)
#define R2_FULL (1u << 27)
#define ATOM_MOUSE 256u
#define ATOM_WHEEL_UP 512u
#define ATOM_WHEEL_DOWN 513u
#define CONTROL_IME 1
#define CONTROL_MENU 2
#define CONTROL_SHIFT 3
#define IME_BUTTONS (PAD_L3 | PAD_R3)
#define MENU_BUTTONS (PAD_CREATE | PAD_OPTIONS)

const char padmap_defaults[] =
    "# RuneScape PS5 controller mapping. Reloaded on title start.\n"
    "# Native menu: Create+Options or physical keyboard F13; F12 is client input.\n"
    "version=1\n"
    "profile.launcher.stick.right=mouse\n"
    "profile.launcher.touch.pointer=on\n"
    "profile.launcher.button.r1=mouse.left\n"
    "profile.launcher.button.r2.full=mouse.right\n"
    "profile.launcher.button.cross=mouse.left\n"
    "profile.launcher.button.circle=key.escape\n"
    "profile.launcher.button.l3+r3=ime\n"
    "profile.launcher.button.create+options=native.menu\n"
    "profile.osrs.threshold.long_ms=305\n"
    "profile.osrs.threshold.double_ms=250\n"
    "profile.osrs.layer.r2=25%\n"
    "profile.osrs.stick.right=mouse\n"
    "profile.osrs.stick.left=arrows,invert_x\n"
    "profile.osrs.touch.pointer=off\n"
    "profile.osrs.button.r1=mouse.left\n"
    "profile.osrs.button.r2.full=mouse.right\n"
    "profile.osrs.button.r3=key.ctrl+mouse.left\n"
    "profile.osrs.button.l3.tap=key.tab\n"
    "profile.osrs.button.l3.long=key.space\n"
    "profile.osrs.button.l3+r3=ime\n"
    "profile.osrs.button.l1=wheel.up\n"
    "profile.osrs.button.l2=wheel.down\n"
    "profile.osrs.button.cross=key.f4\n"
    "profile.osrs.button.circle=key.f5\n"
    "profile.osrs.button.square=key.f6\n"
    "profile.osrs.button.triangle=key.f7\n"
    "profile.osrs.button.triangle.double=toggle.shift\n"
    "profile.osrs.button.touch.left=key.f2\n"
    "profile.osrs.button.touch.right=key.f3\n"
    "profile.osrs.button.options.tap=key.f1\n"
    "profile.osrs.button.options.long=key.space\n"
    "profile.osrs.button.create.tap=key.f8\n"
    "profile.osrs.button.create.double=key.escape\n"
    "profile.osrs.button.create+options=native.menu\n"
    "profile.osrs.button.dpad_up=key.up\n"
    "profile.osrs.button.dpad_down=key.down\n"
    "profile.osrs.button.dpad_right=key.left\n"
    "profile.osrs.button.dpad_left=key.right\n"
    "profile.osrs.layer.r2.button.dpad_up=key.f9\n"
    "profile.osrs.layer.r2.button.dpad_down=key.f12\n"
    "profile.osrs.layer.r2.button.dpad_right=key.f10\n"
    "profile.osrs.layer.r2.button.dpad_left=key.f11\n";

static int fail(struct padmap_error *error, const char *path, unsigned line, const char *format, ...)
{
    if (error)
    {
        char detail[160];
        va_list args;
        va_start(args, format);
        vsnprintf(detail, sizeof(detail), format, args);
        va_end(args);
        error->line = line;
        snprintf(error->text, sizeof(error->text), "%s:%u: %s", path, line, detail);
    }
    return -1;
}

static char *trim(char *value)
{
    while (*value == ' ' || *value == '\t') value++;
    size_t n = strlen(value);
    while (n && (value[n - 1] == ' ' || value[n - 1] == '\t' || value[n - 1] == '\r')) value[--n] = 0;
    return value;
}

static int number(const char *value, unsigned min, unsigned max, unsigned *out)
{
    unsigned n = 0;
    if (!*value) return 0;
    for (; *value; value++)
    {
        if (*value < '0' || *value > '9' || n > max / 10) return 0;
        n = n * 10 + *value - '0';
        if (n > max) return 0;
    }
    if (n < min) return 0;
    *out = n;
    return 1;
}

static uint32_t button(const char *name)
{
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"create", PAD_CREATE}, {"options", PAD_OPTIONS}, {"l3", PAD_L3}, {"r3", PAD_R3},
        {"l1", PAD_L1}, {"r1", PAD_R1}, {"l2", PAD_L2}, {"r2", PAD_R2},
        {"cross", PAD_CROSS}, {"circle", PAD_CIRCLE}, {"square", PAD_SQUARE}, {"triangle", PAD_TRIANGLE},
        {"dpad_up", PAD_UP}, {"dpad_down", PAD_DOWN}, {"dpad_left", PAD_LEFT}, {"dpad_right", PAD_RIGHT},
        {"touch.left", TOUCH_LEFT}, {"touch.right", TOUCH_RIGHT}, {"l2.full", L2_FULL}, {"r2.full", R2_FULL},
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i].name)) return names[i].bit;
    return 0;
}

static unsigned key(const char *name)
{
    static const struct { const char *name; unsigned usage; } names[] = {
        {"enter", 0x28}, {"escape", 0x29}, {"backspace", 0x2a}, {"tab", 0x2b}, {"space", 0x2c},
        {"right", 0x4f}, {"left", 0x50}, {"down", 0x51}, {"up", 0x52},
        {"ctrl", 0xe0}, {"shift", 0xe1}, {"alt", 0xe2}, {"super", 0xe3},
        {"home", 0x4a}, {"end", 0x4d}, {"delete", 0x4c}, {"pageup", 0x4b}, {"pagedown", 0x4e},
    };
    unsigned n;
    if (name[0] && !name[1])
    {
        if (*name >= 'a' && *name <= 'z') return 4 + *name - 'a';
        if (*name >= '1' && *name <= '9') return 0x1e + *name - '1';
        if (*name == '0') return 0x27;
    }
    if (*name == 'f' && number(name + 1, 1, 12, &n)) return 0x3a + n - 1;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i].name)) return names[i].usage;
    return 0;
}

static int action_parse(struct padmap_action *action, char *value)
{
    if (!strcmp(value, "ime")) { action->control = CONTROL_IME; return 1; }
    if (!strcmp(value, "native.menu")) { action->control = CONTROL_MENU; return 1; }
    if (!strcmp(value, "toggle.shift")) { action->control = CONTROL_SHIFT; return 1; }
    char *save = NULL;
    for (char *part = strtok_r(value, "+", &save); part; part = strtok_r(NULL, "+", &save))
    {
        unsigned atom = 0;
        if (!strncmp(part, "key.", 4)) atom = key(part + 4);
        else if (!strcmp(part, "mouse.left")) atom = ATOM_MOUSE + MOUSE_LEFT;
        else if (!strcmp(part, "mouse.right")) atom = ATOM_MOUSE + MOUSE_RIGHT;
        else if (!strcmp(part, "mouse.middle")) atom = ATOM_MOUSE + MOUSE_MIDDLE;
        else if (!strcmp(part, "wheel.up")) atom = ATOM_WHEEL_UP;
        else if (!strcmp(part, "wheel.down")) atom = ATOM_WHEEL_DOWN;
        if (!atom || action->count == PADMAP_MAX_ACTIONS) return 0;
        for (unsigned i = 0; i < action->count; i++) if (action->atom[i] == atom) return 0;
        action->atom[action->count++] = atom;
    }
    /* Canonical modifier-first ordering, regardless of property spelling. */
    for (unsigned i = 1; i < action->count; i++)
        if (action->atom[i] >= 0xe0 && action->atom[i] <= 0xe7)
        {
            unsigned short atom = action->atom[i];
            memmove(action->atom + 1, action->atom, i * sizeof(atom));
            action->atom[0] = atom;
        }
    return action->count != 0;
}

int padmap_parse(struct padmap_config *out, const char *text, size_t size,
                 const char *path, struct padmap_error *error)
{
    struct padmap_config candidate = {0};
    unsigned settings[PADMAP_PROFILES] = {0}, version = 0, line = 1;
    if (size > PADMAP_MAX_BYTES) return fail(error, path, 1, "file exceeds %u bytes", PADMAP_MAX_BYTES);
    for (unsigned i = 0; i < PADMAP_PROFILES; i++)
    {
        candidate.profile[i].long_ms = 305;
        candidate.profile[i].double_ms = 250;
        candidate.profile[i].layer_threshold = 64;
    }
    for (size_t offset = 0; offset < size; line++)
    {
        char buffer[PADMAP_MAX_LINE + 1];
        size_t n = 0;
        while (offset < size && text[offset] != '\n')
        {
            unsigned char c = text[offset++];
            if (n == PADMAP_MAX_LINE) return fail(error, path, line, "line exceeds %u bytes", PADMAP_MAX_LINE);
            if ((!isprint(c) && c != '\t' && c != '\r') || c > 126)
                return fail(error, path, line, "invalid character");
            buffer[n++] = c;
        }
        if (offset < size) offset++;
        buffer[n] = 0;
        char *name = trim(buffer), *value;
        if (!*name || *name == '#' || *name == '!') continue;
        value = strchr(name, '=');
        if (!value) return fail(error, path, line, "expected property=value");
        *value++ = 0;
        name = trim(name); value = trim(value);
        if (!*value || strchr(value, '=') || strstr(value, "++") || *value == '+' || value[strlen(value)-1] == '+')
            return fail(error, path, line, "malformed value");
        if (!strcmp(name, "version"))
        {
            if (version || strcmp(value, "1")) return fail(error, path, line, "duplicate or unsupported version");
            version = 1; continue;
        }
        unsigned id;
        if (!strncmp(name, "profile.launcher.", 17)) { id = PADMAP_LAUNCHER; name += 17; }
        else if (!strncmp(name, "profile.osrs.", 13)) { id = PADMAP_OSRS; name += 13; }
        else return fail(error, path, line, "unknown property or profile");
        struct padmap_profile *p = &candidate.profile[id];
        unsigned setting = 0, v;
        if (!strcmp(name, "threshold.long_ms"))
        {
            setting = 1;
            if (!number(value, 10, 5000, &p->long_ms)) return fail(error, path, line, "long_ms must be 10..5000");
        }
        else if (!strcmp(name, "threshold.double_ms"))
        {
            setting = 2;
            if (!number(value, 10, 2000, &p->double_ms)) return fail(error, path, line, "double_ms must be 10..2000");
        }
        else if (!strcmp(name, "layer.r2"))
        {
            setting = 4;
            size_t len = strlen(value);
            if (value[len - 1] != '%') return fail(error, path, line, "layer requires percent");
            value[len - 1] = 0;
            if (!number(value, 1, 95, &v)) return fail(error, path, line, "layer must be 1..95%%");
            p->layer_threshold = (v * 255 + 99) / 100;
        }
        else if (!strcmp(name, "touch.pointer"))
        {
            setting = 8;
            if (strcmp(value, "on") && strcmp(value, "off")) return fail(error, path, line, "touch.pointer requires on/off");
            p->touch_pointer = !strcmp(value, "on");
        }
        else if (!strcmp(name, "stick.left") || !strcmp(name, "stick.right"))
        {
            unsigned stick = !strcmp(name, "stick.right");
            setting = 16u << stick;
            if (*value == ',' || value[strlen(value) - 1] == ',' || strstr(value, ",,"))
                return fail(error, path, line, "malformed stick mode");
            char *save = NULL, *mode = strtok_r(value, ",", &save);
            if (!strcmp(mode, "off")) p->stick[stick] = PADMAP_OFF;
            else if (!strcmp(mode, "mouse")) p->stick[stick] = PADMAP_MOUSE;
            else if (!strcmp(mode, "arrows")) p->stick[stick] = PADMAP_ARROWS;
            else if (!strcmp(mode, "wheel")) p->stick[stick] = PADMAP_WHEEL;
            else return fail(error, path, line, "unknown stick mode");
            for (char *part = strtok_r(NULL, ",", &save); part; part = strtok_r(NULL, ",", &save))
            {
                unsigned bit = !strcmp(part, "invert_x") ? 1 : !strcmp(part, "invert_y") ? 2 : 0;
                if (!bit || (p->invert[stick] & bit)) return fail(error, path, line, "invalid stick inversion");
                p->invert[stick] |= bit;
            }
        }
        else
        {
            int layer = 0, activator = PADMAP_HOLD;
            if (!strncmp(name, "layer.r2.button.", 16)) { layer = 1; name += 16; }
            else if (!strncmp(name, "button.", 7)) name += 7;
            else return fail(error, path, line, "unknown property");
            char *suffix = strrchr(name, '.');
            if (suffix)
            {
                if (!strcmp(suffix, ".tap")) activator = PADMAP_TAP;
                else if (!strcmp(suffix, ".long")) activator = PADMAP_LONG;
                else if (!strcmp(suffix, ".double")) activator = PADMAP_DOUBLE;
                if (activator != PADMAP_HOLD) *suffix = 0;
            }
            uint32_t mask = 0;
            char *plus = strchr(name, '+');
            if (plus)
            {
                *plus++ = 0;
                uint32_t first = button(name), second = button(plus);
                if (!first || !second || first == second) return fail(error, path, line, "invalid two-button chord");
                mask = first | second;
            }
            else mask = button(name);
            if (!mask) return fail(error, path, line, "unknown button");
            unsigned index;
            for (index = 0; index < p->count; index++)
                if (p->binding[index].buttons == mask && p->binding[index].layer == layer) break;
            if (index == p->count)
            {
                if (p->count == PADMAP_MAX_BINDINGS) return fail(error, path, line, "too many bindings");
                p->count++;
                p->binding[index].buttons = mask;
                p->binding[index].layer = layer;
            }
            struct padmap_binding *b = &p->binding[index];
            if (b->present & (1u << activator)) return fail(error, path, line, "duplicate binding");
            if ((activator == PADMAP_HOLD && (b->present & (1u << PADMAP_TAP))) ||
                (activator == PADMAP_TAP && (b->present & (1u << PADMAP_HOLD))))
                return fail(error, path, line, "hold and tap define the same single action");
            if (!action_parse(&b->action[activator], value)) return fail(error, path, line, "invalid action");
            if ((mask == IME_BUTTONS || mask == MENU_BUTTONS) &&
                (layer || activator != PADMAP_HOLD || b->action[activator].control !=
                    (mask == IME_BUTTONS ? CONTROL_IME : CONTROL_MENU)))
                return fail(error, path, line, "reserved chord");
            b->present |= 1u << activator;
            continue;
        }
        if (settings[id] & setting) return fail(error, path, line, "duplicate property");
        settings[id] |= setting;
    }
    if (!version) return fail(error, path, 1, "missing version=1");
    *out = candidate;
    if (error) memset(error, 0, sizeof(*error));
    return 0;
}

int padmap_install(const char *path, struct padmap_error *error)
{
    char directory[512];
    struct stat st;
    /* stat, not lstat: the PS5 title sandbox rejects lstat with EPERM. */
    if (!stat(path, &st)) return 0;
    if (errno != ENOENT) return fail(error, path, 0, "stat failed: %s", strerror(errno));
    if (strlen(path) >= sizeof(directory)) return fail(error, path, 0, "path too long");
    strcpy(directory, path);
    for (char *p = directory + 1; *p; p++) if (*p == '/')
    {
        *p = 0;
        if (!mkdir(directory, 0777))
        {
            if (chmod(directory, 0777)) return fail(error, path, 0, "directory chmod failed: %s", strerror(errno));
        }
        else if (errno != EEXIST) return fail(error, path, 0, "mkdir failed: %s", strerror(errno));
        *p = '/';
    }
    /* Exclusive final-file creation uses only the title's existing open
     * provider. Preserve even an interrupted partial file for visible parser
     * rejection; never truncate or unlink a possibly user-edited file. */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666), result = -1, saved;
    if (fd < 0)
    {
        if (errno == EEXIST) return 0;
        return fail(error, path, 0, "create failed: %s", strerror(errno));
    }
    size_t used = 0, size = sizeof(padmap_defaults) - 1;
    if (fchmod(fd, 0666)) goto done;
    while (used < size)
    {
        ssize_t n = write(fd, padmap_defaults + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += n;
    }
    if (fsync(fd)) goto done;
    result = 0;
 done:
    saved = errno;
    if (close(fd) && !result) { result = -1; saved = errno; }
    if (result) return fail(error, path, 0, "default install failed: %s", strerror(saved));
    return 0;
}

int padmap_load(struct padmap_config *out, const char *path, struct padmap_error *error)
{
    char text[PADMAP_MAX_BYTES + 1];
    size_t used = 0;
    if (padmap_install(path, error)) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail(error, path, 0, "open failed: %s", strerror(errno));
    while (used < sizeof(text))
    {
        ssize_t n = read(fd, text + used, sizeof(text) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { int saved = errno; close(fd); return fail(error, path, 0, "read failed: %s", strerror(saved)); }
        if (!n) break;
        used += n;
    }
    close(fd);
    return padmap_parse(out, text, used, path, error);
}

static void owned(struct padmap *m, unsigned atom, int down, pointer_handler handler, void *context)
{
    if (atom >= ATOM_WHEEL_UP)
    {
        if (down) handler(&(struct pointer_event){.kind = POINTER_WHEEL,
            .dy = atom == ATOM_WHEEL_UP ? 120 : -120}, context);
        return;
    }
    unsigned short *count = &m->owners[atom];
    if (down ? (*count)++ != 0 : (!*count || --*count != 0)) return;
    handler(&(struct pointer_event){.kind = atom >= ATOM_MOUSE ? POINTER_BUTTON : POINTER_KEY,
        .code = atom >= ATOM_MOUSE ? atom - ATOM_MOUSE : atom, .down = down}, context);
}

static void action(struct padmap *m, const struct padmap_action *a, int down,
                   pointer_handler handler, void *context)
{
    if (a->control)
    {
        if (!down) return;
        if (a->control == CONTROL_SHIFT)
        {
            m->shift_toggle = !m->shift_toggle;
            owned(m, 0xe1, m->shift_toggle, handler, context);
        }
        else handler(&(struct pointer_event){.kind = a->control == CONTROL_IME ? POINTER_TEXT : POINTER_MENU,
                                               .down = 1}, context);
        return;
    }
    unsigned generation = m->generation;
    for (unsigned i = 0; i < a->count; i++)
    {
        owned(m, a->atom[down ? i : a->count - i - 1], down, handler, context);
        if (generation != m->generation) return;
    }
}

static void pulse(struct padmap *m, const struct padmap_action *a, pointer_handler handler, void *context)
{
    unsigned generation = m->generation;
    action(m, a, 1, handler, context);
    if (generation == m->generation) action(m, a, 0, handler, context);
}

void padmap_init(struct padmap *m, const struct padmap_config *config, enum padmap_profile_id profile)
{
    memset(m, 0, sizeof(*m));
    m->profile = config->profile[profile];
    m->sample.lx = m->sample.ly = m->sample.rx = m->sample.ry = 128;
}

void padmap_reset(struct padmap *m, pointer_handler handler, void *context)
{
    /* Release dependent keys/buttons before modifiers. */
    for (unsigned i = 288; i-- > 0;)
        if (m->owners[i] && !(i >= 0xe0 && i <= 0xe7))
        { m->owners[i] = 1; owned(m, i, 0, handler, context); }
    for (unsigned i = 0xe8; i-- > 0xe0;)
        if (m->owners[i]) { m->owners[i] = 1; owned(m, i, 0, handler, context); }
    memset(m->state, 0, sizeof(m->state));
    memset(m->arrows, 0, sizeof(m->arrows));
    m->buttons = m->consumed = 0;
    m->layer = m->touch_half = m->ime_chord = m->shift_toggle = 0;
    m->wheel_x = m->wheel_y = m->mouse_x = m->mouse_y = 0;
    m->last_frame = 0;
    m->sample = (struct input_event){.lx = 128, .ly = 128, .rx = 128, .ry = 128};
    m->draining = 1;
    m->generation++;
}

static int multiple(uint32_t mask) { return !!(mask & (mask - 1)); }

static void cancel_binding(struct padmap *m, unsigned i, pointer_handler handler, void *context)
{
    struct padmap_binding_state *s = &m->state[i];
    unsigned active = s->active;
    memset(s, 0, sizeof(*s));
    if (active) action(m, &m->profile.binding[i].action[active - 1], 0, handler, context);
}

static int chord_possible(const struct padmap *m, uint32_t mask)
{
    if (mask & (IME_BUTTONS | MENU_BUTTONS)) return 1;
    for (unsigned i = 0; i < m->profile.count; i++)
        if (multiple(m->profile.binding[i].buttons) && (m->profile.binding[i].buttons & mask)) return 1;
    return 0;
}
static void update(struct padmap *m, uint64_t now, pointer_handler handler, void *context)
{
    unsigned generation = m->generation;
    uint32_t used = m->consumed;
    /* Reserved chords always win, independent of the editable bindings. */
    if ((m->buttons & IME_BUTTONS) == IME_BUTTONS && !m->ime_chord)
    {
        for (unsigned i = 0; i < m->profile.count; i++)
            if (m->profile.binding[i].buttons & IME_BUTTONS) cancel_binding(m, i, handler, context);
        m->consumed |= IME_BUTTONS;
        m->ime_chord = 1;
    }
    if (m->ime_chord && (m->buttons & IME_BUTTONS) != IME_BUTTONS)
    {
        padmap_reset(m, handler, context);
        handler(&(struct pointer_event){.kind = POINTER_TEXT, .down = 1}, context);
        return;
    }
    if (!m->ime_chord && (m->buttons & MENU_BUTTONS) == MENU_BUTTONS)
    {
        padmap_reset(m, handler, context);
        handler(&(struct pointer_event){.kind = POINTER_MENU, .down = 1}, context);
        return;
    }
    used |= m->consumed;
    /* Two-button chords, layer bindings, then unlayered single bindings. */
    for (unsigned pass = 0; pass < 4; pass++)
    for (unsigned i = 0; i < m->profile.count; i++)
    {
        const struct padmap_binding *b = &m->profile.binding[i];
        struct padmap_binding_state *s = &m->state[i];
        int chord = multiple(b->buttons);
        unsigned rank = chord ? (b->layer ? 0 : 1) : (b->layer ? 2 : 3);
        if (pass != rank) continue;
        int down = (m->buttons & b->buttons) == b->buttons;
        if (chord && !s->down && (m->consumed & b->buttons)) down = 0;
        int allowed = (!(used & b->buttons) || (chord && s->consuming)) && (!b->layer || m->layer);
        if (b->buttons == IME_BUTTONS || b->buttons == MENU_BUTTONS) continue;
        if (!allowed)
        {
            cancel_binding(m, i, handler, context);
            continue;
        }
        if (down && chord)
        {
            used |= b->buttons;
            s->consuming = 1;
            m->consumed |= b->buttons;
            for (unsigned j = 0; j < m->profile.count; j++)
                if (j != i && (m->profile.binding[j].buttons & b->buttons)) cancel_binding(m, j, handler, context);
        }
        else if (down && b->layer) used |= b->buttons;
        if (s->pending && now >= s->deadline)
        {
            s->pending = 0;
            pulse(m, &b->action[(b->present & (1u << PADMAP_TAP)) ? PADMAP_TAP : PADMAP_HOLD], handler, context);
            if (m->generation != generation) return;
        }
        if (down && !s->down)
        {
            s->down = 1;
            s->since = now;
            s->second = s->pending;
            s->pending = 0;
            s->long_fired = 0;
        }
        if (down)
        {
            /* A maintenance frame timestamp may predate the sample read in
             * that same frame; never underflow the hold duration. */
            uint64_t elapsed = now >= s->since ? now - s->since : 0;
            if ((b->present & (1u << PADMAP_LONG)) && !s->long_fired && elapsed >= m->profile.long_ms * 1000ull)
            {
                if (s->active) action(m, &b->action[s->active - 1], 0, handler, context);
                s->active = PADMAP_LONG + 1;
                s->long_fired = 1;
                s->second = 0;
                s->repeat_at = now + 80000;
                action(m, &b->action[PADMAP_LONG], 1, handler, context);
                if (m->generation != generation) return;
            }
            else if (!s->active && !s->long_fired &&
                     !(b->present & ((1u << PADMAP_LONG) | (1u << PADMAP_DOUBLE) | (1u << PADMAP_TAP))) &&
                     (!chord_possible(m, b->buttons) || chord || elapsed >= PADMAP_CHORD_US))
            {
                s->active = PADMAP_HOLD + 1;
                s->repeat_at = now + 80000;
                action(m, &b->action[PADMAP_HOLD], 1, handler, context);
                if (m->generation != generation) return;
            }
            if (s->active && now >= s->repeat_at)
            {
                const struct padmap_action *a = &b->action[s->active - 1];
                for (unsigned j = 0; j < a->count; j++)
                    if (a->atom[j] >= ATOM_WHEEL_UP) owned(m, a->atom[j], 1, handler, context);
                s->repeat_at = now + 80000;
            }
        }
        else if (s->down)
        {
            s->down = 0;
            if (s->active)
            {
                unsigned active = s->active;
                s->active = 0;
                action(m, &b->action[active - 1], 0, handler, context);
            }
            else if (!s->long_fired)
            {
                if (s->second)
                {
                    s->second = 0;
                    pulse(m, &b->action[PADMAP_DOUBLE], handler, context);
                }
                else if (b->present & (1u << PADMAP_DOUBLE))
                {
                    s->pending = 1;
                    s->deadline = now + m->profile.double_ms * 1000ull;
                }
                else pulse(m, &b->action[(b->present & (1u << PADMAP_TAP)) ? PADMAP_TAP : PADMAP_HOLD], handler, context);
            }
            s->long_fired = 0;
        }
        if (m->generation != generation) return;
    }
}

void padmap_input(struct padmap *m, const struct input_event *e, uint64_t now,
                  pointer_handler handler, void *context)
{
    if (e->kind != INPUT_PAD) return;
    if (!e->connected || (e->code & PAD_INTERCEPTED)) { padmap_reset(m, handler, context); return; }
    if (m->draining)
    {
        if (!e->code && e->l2 < 16 && e->r2 < 16 && abs((int)e->lx - 128) < 24 &&
            abs((int)e->ly - 128) < 24 && abs((int)e->rx - 128) < 24 && abs((int)e->ry - 128) < 24)
            m->draining = 0;
        return;
    }
    /* Advance the previously held state before applying this transition: a
     * release exactly at long_ms is a long press, never a late tap. */
    unsigned generation = m->generation;
    update(m, now, handler, context);
    if (generation != m->generation) return;
    if ((e->code & PAD_TOUCHPAD) && !(m->sample.code & PAD_TOUCHPAD))
        m->touch_half = e->touch_x < 960 ? 1 : 2;
    if (!(e->code & PAD_TOUCHPAD)) m->touch_half = 0;
    uint32_t buttons = e->code & ~(PAD_TOUCHPAD | PAD_L2 | PAD_R2);
    if ((e->code & PAD_L2) || e->l2 >= 32) buttons |= PAD_L2;
    if ((e->code & PAD_R2) || e->r2 >= 32) buttons |= PAD_R2;
    if (e->l2 >= ((m->buttons & L2_FULL) ? 230 : 250)) buttons |= L2_FULL;
    if (e->r2 >= ((m->buttons & R2_FULL) ? 230 : 250)) buttons |= R2_FULL;
    if (m->touch_half) buttons |= m->touch_half == 1 ? TOUCH_LEFT : TOUCH_RIGHT;
    unsigned release_threshold = m->profile.layer_threshold > 8 ? m->profile.layer_threshold - 8 : 1;
    m->layer = e->r2 >= (m->layer ? release_threshold : m->profile.layer_threshold);
    /* A consumed chord drains both constituents, even when released separately. */
    if (!(buttons & m->consumed)) m->consumed = 0;
    m->buttons = buttons;
    if (m->profile.touch_pointer && e->touches && m->sample.touches && e->touch_id == m->sample.touch_id)
    {
        int x, y;
        pointer_position(&x, &y);
        pointer_set_position(x + (int)e->touch_x - m->sample.touch_x, y + (int)e->touch_y - m->sample.touch_y);
        pointer_position(&x, &y);
        handler(&(struct pointer_event){.kind = POINTER_MOVE, .x = x, .y = y}, context);
    }
    m->sample = *e;
    update(m, now, handler, context);
}

static float axis(unsigned value)
{
    int v = (int)value - 128;
    if (abs(v) < 24) return 0;
    float f = (abs(v) - 24) / 104.0f;
    if (f > 1) f = 1;
    return v < 0 ? -f * f : f * f;
}

void padmap_frame(struct padmap *m, uint64_t now, pointer_handler handler, void *context)
{
    if (m->draining || !m->sample.connected) return;
    unsigned generation = m->generation;
    update(m, now, handler, context);
    if (m->generation != generation) return;
    float seconds = m->last_frame && now >= m->last_frame ? (now - m->last_frame) / 1000000.0f : 0;
    if (seconds > 0.1f) seconds = 0.1f;
    m->last_frame = now;
    for (unsigned stick = 0; stick < 2; stick++)
    {
        float x = axis(stick ? m->sample.rx : m->sample.lx), y = axis(stick ? m->sample.ry : m->sample.ly);
        if (m->profile.invert[stick] & 1) x = -x;
        if (m->profile.invert[stick] & 2) y = -y;
        switch (m->profile.stick[stick])
        {
        case PADMAP_MOUSE:
            m->mouse_x += x * 1400 * seconds;
            m->mouse_y += y * 1400 * seconds;
            break;
        case PADMAP_WHEEL:
            m->wheel_x += x * 960 * seconds;
            m->wheel_y -= y * 960 * seconds;
            break;
        case PADMAP_ARROWS:
        {
            static const unsigned keys[] = {0x50, 0x4f, 0x52, 0x51};
            int down[] = {x < 0, x > 0, y < 0, y > 0};
            for (unsigned i = 0; i < 4; i++) if (down[i] != m->arrows[stick][i])
            {
                m->arrows[stick][i] = down[i];
                owned(m, keys[i], down[i], handler, context);
            }
            break;
        }
        default: break;
        }
    }
    int dx = (int)m->mouse_x, dy = (int)m->mouse_y;
    if (dx || dy)
    {
        int x, y;
        pointer_position(&x, &y);
        pointer_set_position(x + dx, y + dy);
        pointer_position(&x, &y);
        handler(&(struct pointer_event){.kind = POINTER_MOVE, .x = x, .y = y}, context);
        m->mouse_x -= dx; m->mouse_y -= dy;
    }
    dx = (int)m->wheel_x; dy = (int)m->wheel_y;
    if (dx || dy)
    {
        handler(&(struct pointer_event){.kind = POINTER_WHEEL, .dx = dx, .dy = dy}, context);
        m->wheel_x -= dx; m->wheel_y -= dy;
    }
}
