/* The real parser, installer and engine; no device/Wine dependency or clock. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../title/padmap.h"

static struct padmap map;
static struct padmap_config config;
static struct pointer_event events[2048];
static unsigned count;
static struct input_event pad;
static uint64_t now;
static void record(const struct pointer_event *event, void *context)
{
    (void)context;
    assert(count < sizeof(events) / sizeof(events[0]));
    events[count++] = *event;
}
static void acquire(const struct pointer_event *event, void *context)
{
    record(event, context);
    if (event->kind == POINTER_KEY && event->code == 0xe0 && event->down)
        padmap_reset(&map, record, NULL);
}
static void sample(uint32_t buttons, uint64_t at)
{
    now = at;
    pad.code = buttons;
    padmap_input(&map, &pad, now, record, NULL);
}
static void frame(uint64_t at) { now = at; padmap_frame(&map, now, record, NULL); }
static void init(const char *text, enum padmap_profile_id profile)
{
    struct padmap_error error;
    assert(!padmap_parse(&config, text, strlen(text), "mapping", &error));
    pointer_init(1920, 1080);
    pointer_set_pad_mapping(1);
    padmap_init(&map, &config, profile);
    pad = (struct input_event){.kind = INPUT_PAD, .connected = 1, .lx = 128, .ly = 128, .rx = 128, .ry = 128};
    count = 0;
    now = 1000000;
}
static void expected(unsigned at, enum pointer_kind kind, unsigned code, int down)
{
    assert(at < count);
    assert(events[at].kind == kind && events[at].code == code && events[at].down == down);
}
static void bad(const char *text, unsigned line)
{
    struct padmap_config before;
    struct padmap_error error;
    memset(&before, 0x5a, sizeof(before));
    config = before;
    assert(padmap_parse(&config, text, strlen(text), "/test/pad.properties", &error));
    assert(!memcmp(&before, &config, sizeof(config)));
    assert(error.line == line);
    char prefix[80];
    snprintf(prefix, sizeof(prefix), "/test/pad.properties:%u:", line);
    assert(!strncmp(error.text, prefix, strlen(prefix)));
}
static void parser(void)
{
    init(padmap_defaults, PADMAP_LAUNCHER);
    assert(config.profile[PADMAP_OSRS].count == 24);
    assert(config.profile[PADMAP_OSRS].long_ms == 305);
    assert(config.profile[PADMAP_OSRS].stick[0] == PADMAP_ARROWS);
    assert(!config.profile[PADMAP_OSRS].touch_pointer);
    bad("version=2\n", 1);
    bad("# comment\nversion=1\nprofile.osrs.button.nope=key.f1\n", 3);
    bad("version=1\nprofile.launcher.button.cross=key.nope\n", 2);
    bad("version=1\nprofile.osrs.button.cross=key.f1\nprofile.osrs.button.cross=key.f2\n", 3);
    bad("version=1\nprofile.osrs.button.cross+circle=key.f1\nprofile.osrs.button.circle+cross=key.f2\n", 3);
    bad("version=1\nprofile.osrs.threshold.long_ms=9\n", 2);
    bad("version=1\nprofile.osrs.threshold.double_ms=2001\n", 2);
    bad("version=1\nprofile.osrs.layer.r2=96%\n", 2);
    bad("version=1\nprofile.osrs.layer.r2=25\n", 2);
    bad("version=1\nprofile.osrs.touch.pointer=maybe\n", 2);
    bad("version=1\nprofile.osrs.stick.left=mouse,\n", 2);
    bad("version=1\nprofile.osrs.stick.left=,\n", 2);
    bad("version=1\nprofile.osrs.stick.left=mouse,invert_x,invert_x\n", 2);
    bad("version=1\nprofile.osrs.button.l3+r3=key.f1\n", 2);
    bad("version=1\nprofile.osrs.button.create+options=key.f1\n", 2);
    bad("version=1\nprofile.osrs.button.cross+cross=key.f1\n", 2);
    bad("version=1\nprofile.osrs.button.cross=key.ctrl++mouse.left\n", 2);
    bad("version=1\nprofile.osrs.button.cross=key.ctrl+key.ctrl\n", 2);
    bad("version=1\n[osrs]\n", 2);
    bad("profile.osrs.stick.left=mouse\n", 1);
    char large[PADMAP_MAX_BYTES + 2];
    memset(large, '#', sizeof(large));
    large[sizeof(large)-1] = 0;
    bad(large, 1);
    char line[300] = "version=1\n#";
    memset(line + 11, 'x', sizeof(line) - 12);
    line[sizeof(line)-1] = 0;
    bad(line, 2);
    const char nul[] = "version=1\n\0";
    struct padmap_error error;
    assert(padmap_parse(&config, nul, sizeof(nul)-1, "nul", &error) && error.line == 2);
}
static void launcher(void)
{
    init(padmap_defaults, PADMAP_LAUNCHER);
    sample(PAD_R1 | PAD_CROSS, now);
    assert(count == 1);
    expected(0, POINTER_BUTTON, MOUSE_LEFT, 1);
    sample(PAD_CROSS, now + 1);
    assert(count == 1); /* Two bindings own one left button. */
    sample(0, now + 1);
    assert(count == 2);
    expected(1, POINTER_BUTTON, MOUSE_LEFT, 0);
    pointer_input(&(struct input_event){.kind = INPUT_PAD, .code = PAD_CROSS, .connected = 1}, record, NULL);
    assert(count == 2); /* No legacy Cross click or pad key map. */
    pointer_input(&(struct input_event){.kind = INPUT_KEY, .code = 0x45, .down = 1}, record, NULL);
    expected(2, POINTER_KEY, 0x45, 1); /* F12 is ordinary client input. */
    pointer_input(&(struct input_event){.kind = INPUT_MOUSE_BUTTON, .code = MOUSE_RIGHT, .down = 1}, record, NULL);
    expected(3, POINTER_BUTTON, MOUSE_RIGHT, 1);
    pad.rx = 255;
    sample(0, now + 1);
    frame(now + 1);
    frame(now + 100000);
    assert(events[count-1].kind == POINTER_MOVE && events[count-1].x > 960);
}
static void timed(void)
{
    init(padmap_defaults, PADMAP_OSRS);
    sample(PAD_L3, 1000000);
    frame(999999); /* Frame-start time can precede its newly read pad sample. */
    assert(!count);
    frame(1304999);
    assert(!count);
    sample(0, 1305000); /* Exact long threshold on release. */
    assert(count == 2);
    expected(0, POINTER_KEY, 0x2c, 1);
    expected(1, POINTER_KEY, 0x2c, 0);
    count = 0;
    sample(PAD_L3, 2000000);
    sample(0, 2304999);
    assert(count == 2);
    expected(0, POINTER_KEY, 0x2b, 1);
    expected(1, POINTER_KEY, 0x2b, 0);
    count = 0;
    sample(PAD_TRIANGLE, 3000000);
    sample(0, 3010000);
    frame(3259999);
    assert(!count);
    frame(3260000);
    assert(count == 2);
    expected(0, POINTER_KEY, 0x40, 1);
    expected(1, POINTER_KEY, 0x40, 0);
    count = 0;
    sample(PAD_TRIANGLE, 4000000);
    sample(0, 4010000);
    sample(PAD_TRIANGLE, 4259999);
    sample(0, 4260001);
    assert(count == 1);
    expected(0, POINTER_KEY, 0xe1, 1);
    frame(5000000);
    assert(count == 1); /* No delayed F7 after the double. */
    sample(PAD_TRIANGLE, 6000000);
    sample(0, 6010000);
    sample(PAD_TRIANGLE, 6020000);
    sample(0, 6030000);
    expected(1, POINTER_KEY, 0xe1, 0);
    assert(count == 2);
    count = 0;
    sample(PAD_CREATE, 7000000);
    sample(0, 7010000);
    sample(PAD_CREATE, 7260000); /* Exact deadline: first single already committed. */
    expected(0, POINTER_KEY, 0x41, 1);
    expected(1, POINTER_KEY, 0x41, 0);
    sample(0, 7270000);
    frame(7520000);
    assert(count == 4);
}
static void modifiers(void)
{
    init("version=1\nprofile.osrs.button.r1=mouse.left+key.ctrl\n"
         "profile.osrs.button.cross=key.ctrl+key.a\n", PADMAP_OSRS);
    sample(PAD_R1, now);
    expected(0, POINTER_KEY, 0xe0, 1);
    expected(1, POINTER_BUTTON, MOUSE_LEFT, 1);
    sample(PAD_R1 | PAD_CROSS, now + 1);
    expected(2, POINTER_KEY, 4, 1);
    sample(PAD_CROSS, now + 1);
    expected(3, POINTER_BUTTON, MOUSE_LEFT, 0);
    assert(count == 4); /* Ctrl is still owned by Cross. */
    sample(0, now + 1);
    expected(4, POINTER_KEY, 4, 0);
    expected(5, POINTER_KEY, 0xe0, 0);
    init("version=1\nprofile.osrs.button.r1=key.shift+mouse.left\n"
         "profile.osrs.button.triangle.double=toggle.shift\n", PADMAP_OSRS);
    sample(PAD_TRIANGLE, 1000000); sample(0, 1000001);
    sample(PAD_TRIANGLE, 1000002); sample(0, 1000003);
    expected(0, POINTER_KEY, 0xe1, 1);
    sample(PAD_R1, 1000004);
    assert(map.owners[0xe1] == 2);
    expected(1, POINTER_BUTTON, MOUSE_LEFT, 1);
    sample(0, 1000005);
    expected(2, POINTER_BUTTON, MOUSE_LEFT, 0);
    assert(count == 3 && map.owners[0xe1] == 1);
    padmap_reset(&map, record, NULL);
    expected(3, POINTER_KEY, 0xe1, 0);
}
static void chords(void)
{
    init("version=1\nprofile.osrs.button.cross=key.f1\n"
         "profile.osrs.button.circle=key.f2\n"
         "profile.osrs.button.cross+circle=key.ctrl+mouse.left\n", PADMAP_OSRS);
    sample(PAD_CROSS, 1000000);
    frame(1059999);
    assert(!count);
    sample(PAD_CROSS | PAD_CIRCLE, 1059999);
    expected(0, POINTER_KEY, 0xe0, 1);
    expected(1, POINTER_BUTTON, MOUSE_LEFT, 1);
    frame(1100000);
    assert(count == 2); /* Chord remains held, not cancelled by its own mask. */
    sample(PAD_CIRCLE, 1100001);
    expected(2, POINTER_BUTTON, MOUSE_LEFT, 0);
    expected(3, POINTER_KEY, 0xe0, 0);
    frame(1300000);
    assert(count == 4); /* Remaining constituent is drained. */
    sample(0, 1300001);
    count = 0;
    sample(PAD_CROSS, 1400000);
    frame(1460000);
    expected(0, POINTER_KEY, 0x3a, 1);
    sample(0, 1460001);
    expected(1, POINTER_KEY, 0x3a, 0);
    init(padmap_defaults, PADMAP_OSRS);
    sample(PAD_L3, 1000000);
    sample(PAD_L3 | PAD_R3, 1000001);
    frame(1400000);
    assert(!count);
    sample(PAD_R3, 1400001);
    assert(count == 1 && events[0].kind == POINTER_TEXT);
    sample(PAD_R3, 1500000);
    frame(1600000);
    assert(count == 1);
    sample(0, 1600001); /* Neutral ends drain. */
    sample(PAD_R1, 1600002);
    expected(1, POINTER_BUTTON, MOUSE_LEFT, 1);
    sample(PAD_R1 | PAD_CREATE | PAD_OPTIONS, 1600003);
    expected(2, POINTER_BUTTON, MOUSE_LEFT, 0);
    assert(events[3].kind == POINTER_MENU && count == 4);
    init("version=1\nprofile.osrs.button.cross+circle.tap=key.f1\n"
         "profile.osrs.button.cross+circle.double=key.escape\n", PADMAP_OSRS);
    sample(PAD_CROSS | PAD_CIRCLE, 2000000); sample(0, 2000001);
    assert(!map.consumed); /* Full release must rearm the pending double chord. */
    frame(2100000);
    assert(!count);
    sample(PAD_CROSS | PAD_CIRCLE, 2100001); sample(0, 2100002);
    expected(0, POINTER_KEY, 0x29, 1);
    expected(1, POINTER_KEY, 0x29, 0);
    frame(3000000);
    assert(count == 2);
    init(padmap_defaults, PADMAP_OSRS);
    sample(PAD_L3 | PAD_R3 | PAD_CREATE | PAD_OPTIONS, 4000000);
    assert(!count); /* Reserved IME wins even over the native-menu chord. */
    sample(0, 4000001);
    assert(count == 1 && events[0].kind == POINTER_TEXT);
}
static void layers_touch(void)
{
    init(padmap_defaults, PADMAP_OSRS);
    pad.r2 = 63;
    sample(PAD_DOWN, 1000000);
    expected(0, POINTER_KEY, 0x51, 1);
    pad.r2 = 64;
    sample(PAD_DOWN, 1000001);
    /* Layer down precedes releasing the former unlayered owner. */
    expected(1, POINTER_KEY, 0x45, 1);
    expected(2, POINTER_KEY, 0x51, 0);
    pad.r2 = 56;
    sample(PAD_DOWN, 1000002);
    assert(count == 3 && map.layer);
    pad.r2 = 55;
    sample(PAD_DOWN, 1000003);
    expected(3, POINTER_KEY, 0x45, 0);
    expected(4, POINTER_KEY, 0x51, 1);
    pad.r2 = 249;
    sample(PAD_DOWN, 1000004);
    assert(!map.owners[256 + MOUSE_RIGHT]);
    pad.r2 = 250;
    sample(PAD_DOWN, 1000005);
    assert(map.owners[256 + MOUSE_RIGHT]);
    pad.r2 = 230;
    sample(PAD_DOWN, 1000006);
    assert(map.owners[256 + MOUSE_RIGHT]);
    pad.r2 = 229;
    sample(PAD_DOWN, 1000007);
    assert(!map.owners[256 + MOUSE_RIGHT]);
    pad.r2 = 0;
    sample(0, 1000008);
    count = 0;
    pad.touches = 1; pad.touch_x = 100;
    sample(PAD_TOUCHPAD, 2000000);
    expected(0, POINTER_KEY, 0x3b, 1);
    pad.touch_x = 1800;
    sample(PAD_TOUCHPAD, 2000001);
    assert(count == 1); /* Half is latched at click-down. Touch motion is off. */
    sample(0, 2000002);
    expected(1, POINTER_KEY, 0x3b, 0);
    sample(PAD_TOUCHPAD, 2000003);
    expected(2, POINTER_KEY, 0x3c, 1);
    sample(0, 2000004);
    expected(3, POINTER_KEY, 0x3c, 0);
    init("version=1\nprofile.osrs.layer.r2=1%\n"
         "profile.osrs.layer.r2.button.dpad_up=key.f9\n", PADMAP_OSRS);
    pad.r2 = 3; sample(PAD_UP, 3000000);
    assert(map.layer && count == 1);
    pad.r2 = 0; sample(PAD_UP, 3000001);
    assert(!map.layer && count == 2 && !events[1].down);
}
static void motion_reset(void)
{
    init("version=1\nprofile.osrs.stick.left=arrows,invert_x\n"
         "profile.osrs.stick.right=wheel,invert_y\nprofile.osrs.button.r1=wheel.up\n", PADMAP_OSRS);
    pad.lx = 255; pad.ry = 129;
    sample(0, now);
    frame(now);
    expected(0, POINTER_KEY, 0x50, 1);
    pad.lx = 128; pad.ry = 160; /* fractional wheel movement must accumulate */
    sample(0, now + 1);
    frame(now + 1000);
    assert(count == 2);
    expected(1, POINTER_KEY, 0x50, 0);
    unsigned previous = count;
    for (unsigned i = 0; i < 2000 && count == previous; i++) frame(now + 1000);
    assert(count > previous && events[count-1].kind == POINTER_WHEEL && events[count-1].dy > 0);
    pad.ry = 128;
    sample(PAD_R1, now + 1);
    assert(events[count-1].dy == 120);
    unsigned before = count;
    frame(now + 79999);
    assert(count == before);
    frame(now + 1);
    assert(count == before + 1 && events[count-1].dy == 120);
    init(padmap_defaults, PADMAP_OSRS);
    sample(PAD_R3, 1000000);
    frame(1060000);
    assert(count == 2);
    padmap_reset(&map, record, NULL);
    expected(2, POINTER_BUTTON, MOUSE_LEFT, 0);
    expected(3, POINTER_KEY, 0xe0, 0);
    sample(PAD_R3, 2000000);
    frame(2100000);
    assert(count == 4);
    sample(0, 2100001);
    sample(PAD_R1, 2100002);
    expected(4, POINTER_BUTTON, MOUSE_LEFT, 1);
    pad.code = PAD_R1 | PAD_INTERCEPTED;
    padmap_input(&map, &pad, 2100003, record, NULL);
    expected(5, POINTER_BUTTON, MOUSE_LEFT, 0);
    sample(0, 2100004);
    sample(PAD_R1, 2100005);
    pad.connected = 0;
    padmap_input(&map, &pad, 2100006, record, NULL);
    expected(7, POINTER_BUTTON, MOUSE_LEFT, 0);
    init("version=1\nprofile.osrs.button.r1=key.ctrl+mouse.left\n", PADMAP_OSRS);
    pad.code = PAD_R1;
    padmap_input(&map, &pad, now, acquire, NULL);
    assert(count == 2 && map.draining);
    expected(0, POINTER_KEY, 0xe0, 1);
    expected(1, POINTER_KEY, 0xe0, 0); /* Acquisition aborts the dependent click. */
}
static void file_contract(void)
{
    char root[] = "../../build/psmkit-wine-host/padmap.XXXXXX";
    assert(mkdtemp(root));
    char directory[512], path[512];
    snprintf(directory, sizeof(directory), "%s/shared", root);
    snprintf(path, sizeof(path), "%s/shared/pad-mapping.properties", root);
    mode_t old = umask(0077);
    struct padmap_error error;
    assert(!padmap_load(&config, path, &error));
    umask(old);
    struct stat st;
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0666);
    assert(!stat(directory, &st) && (st.st_mode & 0777) == 0777);
    FILE *file = fopen(path, "w");
    assert(file && fputs("# user\nversion=1\nwrong=keep\n", file) >= 0 && !fclose(file));
    assert(!chmod(path, 0600));
    assert(!padmap_install(path, &error));
    assert(padmap_load(&config, path, &error) && error.line == 3);
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0600); /* Existing modes are preserved. */
    char content[128] = {0};
    file = fopen(path, "r");
    assert(file && fread(content, 1, sizeof(content)-1, file) == strlen("# user\nversion=1\nwrong=keep\n") && !fclose(file));
    assert(!strcmp(content, "# user\nversion=1\nwrong=keep\n"));
    /* Direct exclusive creation can be interrupted. A partial file is an
     * invalid user-owned candidate, not permission to reinstall defaults. */
    file = fopen(path, "w");
    assert(file && fputs("version=1\nprofile.launcher.button.", file) >= 0 && !fclose(file));
    assert(padmap_load(&config, path, &error) && error.line == 2);
    memset(content, 0, sizeof(content));
    file = fopen(path, "r");
    assert(file && fread(content, 1, sizeof(content)-1, file) == strlen("version=1\nprofile.launcher.button.") && !fclose(file));
    assert(!strcmp(content, "version=1\nprofile.launcher.button."));
    assert(!unlink(path) && !rmdir(directory) && !rmdir(root));
}
int main(void)
{
    parser(); launcher(); timed(); modifiers(); chords(); layers_touch(); motion_reset(); file_contract();
    puts("shared pad mapper parser/timing/ownership/layers/reset/file contracts: PASS");
    return 0;
}
