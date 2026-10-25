#ifndef PW_RUNTIME_RELAUNCH_H
#define PW_RUNTIME_RELAUNCH_H
#include "protocol.h"
#include "wine/ps5_game.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
/* Bounded IPC/private-file representation, never platform argv. Values are
 * private; only aggregate sizes/encoding counters may enter diagnostics. */
#define PW_GAME_ARGV_BYTES PW_GAME_WIRE_BYTES
#define PW_GAME_ARGC_MAX 64
#define PW_GAME_PROGRAM 9
#define PW_GAME_ENV_BYTES 16384
struct pw_game_wire { char bytes[PW_GAME_ARGV_BYTES]; const char *argv[PW_GAME_WIRE_ARGV + 1]; unsigned argc; };
struct pw_game_argv_metrics {
    unsigned argc, total_bytes, max_bytes, max_index, empty, invalid_utf8;
};
static inline int pw_game_arg_utf8(const char *argument)
{
    const unsigned char *p = (const unsigned char *)argument;
    while (*p)
    {
        unsigned value, minimum, continuation;
        if (*p < 0x80) { ++p;continue; }
        if (*p >= 0xc2 && *p <= 0xdf) { value = *p & 31;minimum = 0x80;continuation = 1; }
        else if (*p >= 0xe0 && *p <= 0xef) { value = *p & 15;minimum = 0x800;continuation = 2; }
        else if (*p >= 0xf0 && *p <= 0xf4) { value = *p & 7;minimum = 0x10000;continuation = 3; }
        else return 0;
        ++p;
        while (continuation--)
        {
            if ((*p & 0xc0) != 0x80) return 0;
            value = (value << 6) | (*p++ & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}
static inline struct pw_game_argv_metrics pw_game_wire_metrics(const struct pw_game_wire *wire)
{
    struct pw_game_argv_metrics result = {.argc = wire->argc};
    for (unsigned i = 0; i < wire->argc; ++i)
    {
        unsigned bytes = (unsigned)strlen(wire->argv[i]) + 1;
        result.total_bytes += bytes;
        if (bytes > result.max_bytes) { result.max_bytes = bytes;result.max_index = i; }
        result.empty += bytes == 1;
        result.invalid_utf8 += !pw_game_arg_utf8(wire->argv[i]);
    }
    return result;
}
static inline int pw_game_environment_valid(const uint16_t *environment, unsigned bytes)
{
    if (!environment || bytes < 2 || bytes > PW_GAME_ENV_BYTES || bytes % 2) return 0;
    unsigned units = bytes / 2;
    if (units == 1) return !environment[0];
    if (environment[units - 1] || environment[units - 2]) return 0;
    for (unsigned i = 0; i + 2 < units; ++i)
        if (!environment[i] && !environment[i + 1]) return 0;
    return 1;
}
static inline int pw_game_wire_encode(struct pw_game_wire *wire, char *const *program,
                                      const char *windows_cwd, const char *unix_cwd,
                                      const uint16_t *environment, unsigned environment_bytes)
{
    static const char *const header[] = {"game.bin", "--ps5-osrs-v2", "--unix-cwd", NULL,
        "--windows-cwd", NULL, "--environment-utf16", NULL, "--"};
    static const char hex[] = "0123456789abcdef";
    size_t used = 0;
    unsigned count = 0, programs = 0;
    if (!wire || !program || !wine_ps5_game_profile(program[0]) || !windows_cwd || !unix_cwd ||
        !pw_game_environment_valid(environment, environment_bytes)) return EINVAL;
    memset(wire, 0, sizeof(*wire));
#define PW_WIRE_COPY(value) do { const char *v_ = (value); size_t n_ = strlen(v_) + 1; \
    if (count >= PW_GAME_WIRE_ARGV || n_ > sizeof(wire->bytes) - used) return E2BIG; \
    memcpy(wire->bytes + used, v_, n_); wire->argv[count++] = wire->bytes + used; used += n_; } while (0)
    for (unsigned i = 0; i < PW_GAME_PROGRAM; ++i)
    {
        if (i == 7)
        {
            size_t length = (size_t)environment_bytes * 2;
            if (length + 1 > sizeof(wire->bytes) - used) return E2BIG;
            wire->argv[count++] = wire->bytes + used;
            for (unsigned j = 0; j < environment_bytes / 2; ++j)
                for (unsigned k = 0; k < 4; ++k)
                    wire->bytes[used++] = hex[(environment[j] >> ((3 - k) * 4)) & 15];
            wire->bytes[used++] = 0;
        }
        else PW_WIRE_COPY(i == 3 ? unix_cwd : i == 5 ? windows_cwd : header[i]);
    }
    for (; program[programs]; ++programs)
    {
        if (programs == PW_GAME_ARGC_MAX) return E2BIG;
        PW_WIRE_COPY(program[programs]);
    }
#undef PW_WIRE_COPY
    wire->argc = count;
    return 0;
}
static inline int pw_game_wire_decode(int argc, char **argv, const char *prefix,
                                      uint16_t *environment, unsigned *environment_bytes)
{
    char root[4096];
    size_t bytes = 0, length;
    if (!argv || !prefix || !environment || !environment_bytes || argc < PW_GAME_PROGRAM + 1 ||
        argc > PW_GAME_WIRE_ARGV || strcmp(argv[1], "--ps5-osrs-v2") ||
        strcmp(argv[2], "--unix-cwd") || strcmp(argv[4], "--windows-cwd") ||
        strcmp(argv[6], "--environment-utf16") || strcmp(argv[8], "--") ||
        !wine_ps5_game_profile(argv[PW_GAME_PROGRAM])) return -EINVAL;
    for (int i = 0; i < argc; ++i)
        if (!argv[i] || (bytes += strlen(argv[i]) + 1) > PW_GAME_ARGV_BYTES) return -E2BIG;
    /* Wine's initial process-parameter DOS cwd buffer is MAX_PATH WCHARs.
     * This conservative UTF-8 bound rejects rather than truncates it. */
    if (strlen(argv[5]) >= 260) return -E2BIG;
    int n = snprintf(root, sizeof(root), "%s/drive_c/", prefix);
    if (n < 0 || (size_t)n >= sizeof(root)) return -EACCES;
    if (strncmp(argv[3], root, strlen(root)))
    {
        n = snprintf(root, sizeof(root), "%s/dosdevices/c:/", prefix);
        if (n < 0 || (size_t)n >= sizeof(root) || strncmp(argv[3], root, strlen(root))) return -EACCES;
    }
    if (strstr(argv[3], "/../") || strstr(argv[3], "/./") ||
        (argv[5][0] != 'C' && argv[5][0] != 'c') || argv[5][1] != ':' ||
        (argv[5][2] != '\\' && argv[5][2] != '/')) return -EACCES;
    length = strlen(argv[7]);
    if (length < 4 || length % 4 || length / 2 > PW_GAME_ENV_BYTES) return -EINVAL;
    for (size_t i = 0; i < length / 4; ++i)
    {
        unsigned unit = 0;
        for (unsigned j = 0; j < 4; ++j)
        {
            unsigned char c = argv[7][i * 4 + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -EINVAL;
            unit = unit * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
        }
        environment[i] = unit;
    }
    *environment_bytes = length / 2;
    return pw_game_environment_valid(environment, *environment_bytes) ? 0 : -EINVAL;
}
static inline int pw_game_request_pack(struct pw_game_request *request, const struct pw_game_wire *wire)
{
    size_t bytes;
    if (!request || !wire || wire->argc < PW_GAME_PROGRAM + 1 || wire->argc > PW_GAME_WIRE_ARGV) return EINVAL;
    bytes = wire->argv[wire->argc - 1] - wire->bytes + strlen(wire->argv[wire->argc - 1]) + 1;
    if (bytes > sizeof(request->data)) return E2BIG;
    memset(request, 0, sizeof(*request));
    request->argc = wire->argc;
    request->bytes = bytes;
    memcpy(request->data, wire->bytes, bytes);
    for (unsigned i = 0; i < wire->argc; ++i) request->offsets[i] = wire->argv[i] - wire->bytes;
    return 0;
}
static inline int pw_game_request_unpack(const struct pw_game_request *request, char *argv[PW_GAME_WIRE_ARGV + 1])
{
    if (!request || !argv || request->argc < PW_GAME_PROGRAM + 1 || request->argc > PW_GAME_WIRE_ARGV ||
        !request->bytes || request->bytes > sizeof(request->data) || request->offsets[0] ||
        request->data[request->bytes - 1]) return EPROTO;
    for (unsigned i = 0; i < request->argc; ++i)
    {
        uint32_t start = request->offsets[i];
        uint32_t end = i + 1 < request->argc ? request->offsets[i + 1] : request->bytes;
        if (start >= end || end > request->bytes || request->data[end - 1] ||
            memchr(request->data + start, 0, end - start - 1)) return EPROTO;
        argv[i] = (char *)request->data + start;
    }
    argv[request->argc] = NULL;
    return 0;
}
#endif
