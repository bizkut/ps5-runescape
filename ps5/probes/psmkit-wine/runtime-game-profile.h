#ifndef PW_RUNTIME_GAME_PROFILE_H
#define PW_RUNTIME_GAME_PROFILE_H
#include "runtime-relaunch.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define PW_RUNELITE_CONFIG_PATH "/data/runescape/runelite.properties"
struct pw_game_parent_options
{
    const struct wine_ps5_game_profile *profile;
    const char *launch_mode, *mode_source;
    const char *hw_accel, *hw_source;
};
struct pw_game_parent_storage
{
    char *argv[PW_GAME_ARGC_MAX + 4];
    char launch_mode[32], hw_accel[32];
};
static inline const char *pw_game_option(const char *text, size_t bytes,
                                        const char *const *values, unsigned count)
{
    while (bytes && isspace((unsigned char)*text)) { ++text;--bytes; }
    while (bytes && isspace((unsigned char)text[bytes - 1])) --bytes;
    for (unsigned i = 0; i < count; ++i)
        if (bytes == strlen(values[i]) && !strncasecmp(text, values[i], bytes)) return values[i];
    return NULL;
}
static inline const char *pw_game_launch_mode(const char *text, size_t bytes)
{
    static const char *const values[] = {"REFLECT", "AUTO", "JVM", "FORK"};
    return pw_game_option(text, bytes, values, sizeof(values) / sizeof(values[0]));
}
static inline const char *pw_game_hw_accel(const char *text, size_t bytes)
{
    static const char *const values[] = {"AUTO", "OFF", "DIRECTDRAW", "OPENGL"};
    return pw_game_option(text, bytes, values, sizeof(values) / sizeof(values[0]));
}
static inline int pw_game_read_config(const char *path, struct pw_game_parent_options *options)
{
    char text[512];
    size_t used = 0, at = 0;
    unsigned seen = 0;
    int fd = open(path, O_RDONLY), error = 0;
    if (fd < 0) return errno == ENOENT ? 0 : -errno;
    while (used < sizeof(text))
    {
        ssize_t got = read(fd, text + used, sizeof(text) - used);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { error = -errno;break; }
        if (!got) break;
        used += (size_t)got;
    }
    close(fd);
    if (error) return error;
    if (used == sizeof(text)) return -E2BIG;
    if (memchr(text, 0, used)) return -EINVAL;
    while (at < used)
    {
        size_t first = at, last, equal, key_end;
        const char *value;
        while (at < used && text[at] != '\n') ++at;
        last = at;
        if (at < used) ++at;
        while (first < last && isspace((unsigned char)text[first])) ++first;
        while (last > first && isspace((unsigned char)text[last - 1])) --last;
        if (first == last || text[first] == '#') continue;
        equal = first;
        while (equal < last && text[equal] != '=') ++equal;
        if (equal == last) return -EINVAL;
        key_end = equal;
        while (key_end > first && isspace((unsigned char)text[key_end - 1])) --key_end;
        if (key_end - first == 11 && !memcmp(text + first, "launch_mode", 11))
        {
            if ((seen & 1) || !(value = pw_game_launch_mode(text + equal + 1, last - equal - 1))) return -EINVAL;
            seen |= 1;options->launch_mode = value;options->mode_source = "file";
        }
        else if (key_end - first == 8 && !memcmp(text + first, "hw_accel", 8))
        {
            if ((seen & 2) || !(value = pw_game_hw_accel(text + equal + 1, last - equal - 1))) return -EINVAL;
            seen |= 2;options->hw_accel = value;options->hw_source = "file";
        }
        else return -EINVAL;
    }
    return 0;
}
static inline int pw_game_original_option(int argc, char **arguments, const char *name, const char **value)
{
    size_t length = strlen(name);
    for (int i = 2; i < argc; ++i)
    {
        if (!strcmp(arguments[i], "--")) break;
        if (!strcmp(arguments[i], name)) { *value = i + 1 < argc ? arguments[i + 1] : "";return 1; }
        if (!strncmp(arguments[i], name, length) && arguments[i][length] == '=')
        { *value = arguments[i] + length + 1;return 1; }
    }
    return 0;
}
static inline int pw_game_parent_arguments(int argc, char ***arguments, const char *config_path,
                                           struct pw_game_parent_storage *storage,
                                           struct pw_game_parent_options *options)
{
    const char *original_mode = NULL, *original_hw = NULL;
    int has_mode, has_hw, error, insertion, added, cursor;
    if (!arguments || !*arguments || argc < 2 || argc > PW_GAME_ARGC_MAX + 1 || !options) return -EINVAL;
    const struct wine_ps5_game_profile *profile = wine_ps5_game_profile((*arguments)[1]);
    if (!profile) return -EINVAL;
    *options = (struct pw_game_parent_options){.profile = profile, .mode_source = "none", .hw_source = "none"};
    if (!profile->launch_mode) return argc; /* OSRS reads no config and changes no argv bytes. */
    options->launch_mode = profile->launch_mode;options->mode_source = "default";
    options->hw_accel = "AUTO";options->hw_source = "default";
    has_mode = pw_game_original_option(argc, *arguments, "--launch-mode", &original_mode);
    has_hw = pw_game_original_option(argc, *arguments, "--hw-accel", &original_hw);
    if (!has_hw) has_hw = pw_game_original_option(argc, *arguments, "--mode", &original_hw);
    if ((!has_mode || !has_hw) && (error = pw_game_read_config(config_path, options))) return error;
    if (has_mode)
    {
        options->launch_mode = pw_game_launch_mode(original_mode, strlen(original_mode));
        options->mode_source = "launcher";
    }
    if (has_hw)
    {
        options->hw_accel = pw_game_hw_accel(original_hw, strlen(original_hw));
        options->hw_source = "launcher";
    }
    if (has_mode && has_hw) return argc; /* Original options remain upstream-owned, even malformed. */
    if (!storage) return -EINVAL;
    insertion = 2;
    while (insertion < argc && strcmp((*arguments)[insertion], "--")) ++insertion;
    added = !has_mode + !has_hw;
    /* Backwards suffix copy also preserves an already-owned argv in place. */
    for (int i = argc; i > insertion; --i) storage->argv[i - 1 + added] = (*arguments)[i - 1];
    for (int i = 0; i < insertion; ++i) storage->argv[i] = (*arguments)[i];
    cursor = insertion;
    if (!has_mode)
    {
        snprintf(storage->launch_mode, sizeof(storage->launch_mode), "--launch-mode=%s", options->launch_mode);
        storage->argv[cursor++] = storage->launch_mode;
    }
    if (!has_hw)
    {
        snprintf(storage->hw_accel, sizeof(storage->hw_accel), "--hw-accel=%s", options->hw_accel);
        storage->argv[cursor++] = storage->hw_accel;
    }
    storage->argv[argc + added] = NULL;
    *arguments = storage->argv;
    return argc + added;
}
/* Native profile values are role policy, not changes to launcher/JX credentials.
 * The shared Windows environment is still restored from the original wire. */
static inline int pw_game_parent_environment(const struct wine_ps5_game_profile *profile)
{
    if (profile)
    {
        if (profile->graphics != WINE_PS5_GAME_WGL_ZINK || profile->pad != WINE_PS5_GAME_PAD_OSRS)
            return ENOTSUP;
        if (setenv("GALLIUM_DRIVER", "zink", 1) || setenv("WINEPS5_GAME_PAD_PROFILE", "osrs", 1)) return errno;
    }
    else if (unsetenv("WINEPS5_GAME_PAD_PROFILE")) return errno;
    if (profile && profile->kind == WINE_PS5_GAME_RUNELITE)
    {
        if (setenv("WINEPS5_RUNELITE_DIAG", "1", 1)) return errno;
    }
    else if (profile ? setenv("WINEPS5_RUNELITE_DIAG", "0", 1) : unsetenv("WINEPS5_RUNELITE_DIAG")) return errno;
    /* win32u's presenter: one direct game swapchain, or the window compositor. */
    if (profile)
    {
        if (profile->present != WINE_PS5_GAME_PRESENT_DIRECT && profile->present != WINE_PS5_GAME_PRESENT_COMPOSITOR)
            return ENOTSUP;
        if (setenv("WINEPS5_TITLE_PRESENT",
                   profile->present == WINE_PS5_GAME_PRESENT_COMPOSITOR ? "compositor" : "direct", 1))
            return errno;
    }
    else if (unsetenv("WINEPS5_TITLE_PRESENT")) return errno;
    return 0;
}
#endif
