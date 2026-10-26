#define _GNU_SOURCE
#include "runtime-relaunch.h"
#include <unistd.h>
#include <sys/stat.h>
#include <grp.h>
#include <sys/wait.h>
static int test_fstat(int fd, struct stat *status);
static ssize_t test_write(int fd, const void *data, size_t bytes);
#define write test_write
#define fstat test_fstat
#include "runtime-game-file.h"
#undef write
#undef fstat
#include "runtime-game-profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned fail_write_call, write_calls;
static int reported_file_links = -1;
static int test_fstat(int fd, struct stat *status)
{
    int result = fstat(fd, status);
    if (!result && S_ISREG(status->st_mode) && reported_file_links >= 0)
        status->st_nlink = reported_file_links;
    return result;
}
static ssize_t test_write(int fd, const void *data, size_t bytes)
{
    if (++write_calls == fail_write_call) { errno = EACCES;return -1; }
    return write(fd, data, bytes);
}

static void test_private_handoff(const struct pw_game_wire *wire, const uint16_t *environment,
                                  unsigned environment_bytes)
{
    char root[] = "/tmp/runescape-handoff-XXXXXX", directory[4096], path[4096], other[4096];
    uint8_t random[16];
    char token[PW_GAME_HANDOFF_TOKEN_CHARS + 1], *arguments[PW_GAME_WIRE_ARGV + 1];
    struct pw_game_request request;
    struct stat status;
    struct pw_game_handoff_diagnostic diagnostic;
    uint16_t decoded[PW_GAME_ENV_BYTES / 2];
    unsigned removed, bytes;
    mode_t mask = umask(0077);
    assert(mkdtemp(root));
    assert(snprintf(directory, sizeof(directory), "%s/private", root) < (int)sizeof(directory));
    for (unsigned i = 0; i < sizeof(random); ++i) random[i] = i;
    pw_game_handoff_token(token, random);
    assert(pw_game_handoff_token_valid(token));
    assert(!pw_game_handoff_token_valid("../outside"));
    assert(!pw_game_handoff_prepare(directory, &removed, &diagnostic) && !removed);
    assert(!diagnostic.error && !strcmp(diagnostic.operation, "ready"));
    /* An existing 0700 directory (left by 01.000.021) must not fail prepare; it is repaired. */
    assert(!pw_game_handoff_prepare(directory, &removed, &diagnostic) && !removed);
    assert(!diagnostic.error && !stat(directory, &status) && (status.st_mode & 07777) == 0777);
    assert(!chmod(directory, 0700));
    assert(!pw_game_handoff_prepare(directory, &removed, NULL) && !removed);
    assert(!stat(directory, &status) && (status.st_mode & 07777) == 0777);
    assert(!pw_game_handoff_path(directory, token, path));
    assert(!pw_game_handoff_write(directory, token, wire, &diagnostic));
    assert(!diagnostic.error && diagnostic.links == 1 && (diagnostic.file_mode & 07777) == 0600);
    assert(diagnostic.file_uid == diagnostic.directory_uid && diagnostic.file_device == diagnostic.directory_device);
    assert(!stat(path, &status) && (status.st_mode & 07777) == 0600);
    struct pw_game_argv_metrics metrics = pw_game_wire_metrics(wire);
    assert(status.st_size == (off_t)(sizeof(struct pw_game_handoff_header) + metrics.total_bytes));
    assert(!pw_game_handoff_read(directory, token, &request, arguments, NULL));
    assert(access(path, F_OK) == -1 && errno == ENOENT);
    assert(!pw_game_wire_decode(request.argc, arguments, "/prefix", decoded, &bytes));
    assert(bytes == environment_bytes && !memcmp(decoded, environment, bytes));
    for (unsigned i = 0; i < wire->argc; ++i) assert(!strcmp(arguments[i], wire->argv[i]));
    assert(pw_game_handoff_read(directory, token, &request, arguments, NULL) == ENOENT); /* read once */

    /* PS5 /data reports zero links for an exclusively created regular file.
     * Keep all actual I/O, metadata and read-once semantics; change only nlink. */
    reported_file_links = 0;
    assert(!pw_game_handoff_write(directory, token, wire, &diagnostic));
    assert(!diagnostic.error && diagnostic.links == 0 && (diagnostic.file_mode & 07777) == 0600);
    assert(!pw_game_handoff_read(directory, token, &request, arguments, &diagnostic));
    assert(!diagnostic.error && diagnostic.links == 0 && access(path, F_OK) == -1 && errno == ENOENT);
    assert(!pw_game_wire_decode(request.argc, arguments, "/prefix", decoded, &bytes));
    assert(bytes == environment_bytes && !memcmp(decoded, environment, bytes));
    for (unsigned i = 0; i < wire->argc; ++i) assert(!strcmp(arguments[i], wire->argv[i]));
    reported_file_links = 2;
    assert(pw_game_handoff_write(directory, token, wire, &diagnostic) == EACCES);
    assert(!strcmp(diagnostic.operation, "validate-file-links") && diagnostic.links == 2);
    assert(!diagnostic.cleanup_error && access(path, F_OK) == -1 && errno == ENOENT);
    reported_file_links = -1;

    assert(!pw_game_handoff_write(directory, token, wire, NULL));
    assert(pw_game_handoff_write(directory, token, wire, &diagnostic) == EEXIST);
    assert(diagnostic.error == EEXIST && !strcmp(diagnostic.operation, "create-file") && !diagnostic.cleanup_error);
    int fd = open(path, O_WRONLY);assert(fd >= 0 && !ftruncate(fd, 8) && !close(fd));
    assert(pw_game_handoff_read(directory, token, &request, arguments, &diagnostic) == EPROTO);
    assert(!strcmp(diagnostic.operation, "validate-file-size"));
    assert(access(path, F_OK) == -1 && errno == ENOENT); /* malformed is consumed too */
    assert(!pw_game_handoff_write(directory, token, wire, NULL));
    assert(!chmod(path, 0644));
    assert(pw_game_handoff_read(directory, token, &request, arguments, &diagnostic) == EACCES);
    assert(!strcmp(diagnostic.operation, "validate-file-mode") && (diagnostic.file_mode & 07777) == 0644);
    assert(access(path, F_OK) == -1 && errno == ENOENT);
    assert(!pw_game_handoff_write(directory, token, wire, NULL));
    assert(snprintf(other, sizeof(other), "%s/hardlink", root) < (int)sizeof(other));
    assert(!link(path, other));
    assert(pw_game_handoff_read(directory, token, &request, arguments, &diagnostic) == EACCES);
    assert(!strcmp(diagnostic.operation, "validate-file-links") && diagnostic.links == 2);
    assert(access(path, F_OK) == -1 && !stat(other, &status) && status.st_nlink == 1);
    assert(!symlink(other, path));
    assert(pw_game_handoff_read(directory, token, &request, arguments, NULL) != 0);
    assert(access(path, F_OK) == -1 && !stat(other, &status));
    assert(!unlink(other));
    for (unsigned call = 1; call <= 2; ++call) {
        fail_write_call = call;write_calls = 0;
        assert(pw_game_handoff_write(directory, token, wire, &diagnostic) == EACCES);
        assert(!strcmp(diagnostic.operation, call == 1 ? "write-header" : "write-payload"));
        assert(diagnostic.error == EACCES && !diagnostic.cleanup_error);
        assert(access(path, F_OK) == -1 && errno == ENOENT);
    }
    fail_write_call = 0;

    /* The payload, not service argv, may exceed Forever's old 4-KiB envelope. */
    uint16_t large_environment[4096];
    for (unsigned i = 0; i < 4094; ++i) large_environment[i] = 'x';
    large_environment[4094] = large_environment[4095] = 0;
    struct pw_game_wire large;
    char *program[] = {(char *)wire->argv[PW_GAME_PROGRAM], NULL};
    assert(!pw_game_wire_encode(&large, program, wire->argv[5], wire->argv[3],
                               large_environment, sizeof(large_environment)));
    metrics = pw_game_wire_metrics(&large);
    assert(metrics.total_bytes > 4096 && metrics.max_index == 7 && metrics.max_bytes == 16385);
    assert(!pw_game_handoff_write(directory, token, &large, NULL));
    assert(!pw_game_handoff_read(directory, token, &request, arguments, NULL));
    assert(!pw_game_wire_decode(request.argc, arguments, "/prefix", decoded, &bytes));
    assert(bytes == sizeof(large_environment) && !memcmp(decoded, large_environment, bytes));

    assert(!pw_game_handoff_write(directory, token, wire, NULL));
    assert(!pw_game_handoff_prepare(directory, &removed, NULL) && removed == 1);
    assert(access(path, F_OK) == -1 && errno == ENOENT);
    assert(snprintf(other, sizeof(other), "%s/keep", directory) < (int)sizeof(other));
    fd = open(other, O_WRONLY | O_CREAT | O_EXCL, 0600);assert(fd >= 0 && !close(fd));
    assert(!pw_game_handoff_prepare(directory, &removed, NULL) && !removed);
    assert(!stat(other, &status)); /* only own-token names are stale handoffs */
    assert(!unlink(other));
    /* FTP-style deletion: a non-owner can unlink a session file in the 0777 directory
     * but cannot read it (0600). A real foreign uid needs root; otherwise the
     * permission bits that grant those rights are asserted. */
    assert(!pw_game_handoff_write(directory, token, wire, NULL));
    assert(!stat(directory, &status) && (status.st_mode & 0003) == 0003);
    assert(!stat(path, &status) && (status.st_mode & 0077) == 0);
    if (geteuid() == 0) {
        pid_t child = fork();
        int wait_status;
        assert(child >= 0);
        if (!child) {
            if (setgroups(0, NULL) || setgid(65534) || setuid(65534)) _exit(2);
            if (open(path, O_RDONLY) >= 0 || errno != EACCES) _exit(3);
            if (unlink(path)) _exit(4);
            _exit(0);
        }
        assert(waitpid(child, &wait_status, 0) == child && WIFEXITED(wait_status) && !WEXITSTATUS(wait_status));
        assert(access(path, F_OK) == -1 && errno == ENOENT);
    } else {
        assert(!unlink(path));
    }
    assert(!pw_game_handoff_write(directory, token, wire, NULL));
    assert(!pw_game_handoff_discard(directory, token, NULL));
    assert(access(path, F_OK) == -1 && errno == ENOENT);
    assert(!rmdir(directory) && !rmdir(root));
    umask(mask);
}

static void mode_file(const char *path, const char *text, size_t bytes)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    assert(fd >= 0 && write(fd, text, bytes) == (ssize_t)bytes && !close(fd));
}

static void test_game_profiles(void)
{
    char root[] = "/tmp/runescape-profiles-XXXXXX", path[4096], config[96], **arguments;
    struct pw_game_parent_storage storage;
    struct pw_game_parent_options options;
    assert(mkdtemp(root) && snprintf(path, sizeof(path), "%s/config", root) < (int)sizeof(path));
    char *osrs[] = {"wine", "C:\\Game\\osclient.exe", "", "future argument", NULL};
    arguments = osrs;
    assert(pw_game_parent_arguments(4, &arguments, root, NULL, &options) == 4);
    assert(arguments == osrs && !osrs[4] && !strcmp(osrs[2], "") && !strcmp(osrs[3], "future argument"));
    assert(options.profile->kind == WINE_PS5_GAME_OSRS && !options.launch_mode && !strcmp(options.mode_source, "none"));
    struct pw_game_wire wire;
    struct pw_game_request before, after;
    const uint16_t environment[] = {'J','X','_','S','=','x',0,0};
    assert(!pw_game_wire_encode(&wire, osrs + 1, "C:\\Game", "/prefix/drive_c/Game", environment, sizeof(environment)));
    assert(!pw_game_request_pack(&before, &wire));
    assert(pw_game_parent_arguments(4, &arguments, path, NULL, &options) == 4);
    assert(!pw_game_wire_encode(&wire, arguments + 1, "C:\\Game", "/prefix/drive_c/Game", environment, sizeof(environment)));
    assert(!pw_game_request_pack(&after, &wire) && !memcmp(&before, &after, sizeof(before)));

    char *runelite[] = {"wine", "C:\\RuneLite\\RuneLite.exe", "original with spaces", "", NULL};
    arguments = runelite;
    assert(pw_game_parent_arguments(4, &arguments, path, &storage, &options) == 6);
    assert(arguments == storage.argv && !arguments[6] && !strcmp(arguments[4], "--launch-mode=REFLECT"));
    assert(!strcmp(arguments[5], "--hw-accel=AUTO") && !strcmp(options.hw_source, "default"));
    for (unsigned i = 0; i < 4; ++i) assert(arguments[i] == runelite[i]);
    assert(!runelite[4] && options.profile->kind == WINE_PS5_GAME_RUNELITE);
    assert(options.profile->graphics == WINE_PS5_GAME_WGL_ZINK && options.profile->pad == WINE_PS5_GAME_PAD_OSRS);
    assert(!strcmp(options.launch_mode, "REFLECT") && !strcmp(options.mode_source, "default"));
    const char properties[] = "# pipeline, not GPU plugin\r\nlaunch_mode = jVm\r\nhw_accel = opengl\n";
    mode_file(path, properties, sizeof(properties) - 1);arguments = runelite;
    assert(pw_game_parent_arguments(4, &arguments, path, &storage, &options) == 6);
    assert(!strcmp(arguments[4], "--launch-mode=JVM") && !strcmp(options.mode_source, "file"));
    assert(!strcmp(arguments[5], "--hw-accel=OPENGL") && !strcmp(options.hw_source, "file"));
    const char *modes[] = {"REFLECT", "AUTO", "JVM", "FORK"};
    const char *hardware[] = {"AUTO", "OFF", "DIRECTDRAW", "OPENGL"};
    for (unsigned i = 0; i < 4; ++i)
    {
        int bytes = snprintf(config, sizeof(config), "launch_mode=%s\nhw_accel=%s\n", modes[i], hardware[i]);
        assert(bytes > 0 && (size_t)bytes < sizeof(config));mode_file(path, config, (size_t)bytes);
        arguments = runelite;
        assert(pw_game_parent_arguments(4, &arguments, path, &storage, &options) == 6);
        assert(!strcmp(options.launch_mode, modes[i]) && !strcmp(options.hw_accel, hardware[i]));
    }
    const char invalid[] = {'F', 'O', 0, 'K'};
    mode_file(path, invalid, sizeof(invalid));arguments = runelite;
    assert(pw_game_parent_arguments(4, &arguments, path, &storage, &options) == -EINVAL);
    assert(arguments == runelite && !runelite[4]);
    const char *bad[] = {"launch_mode=\n", "hw_accel=METAL\n", "unknown=AUTO\n",
                         "launch_mode=REFLECT\nlaunch_mode=FORK\n", "hw_accel=AUTO\nhw_accel=OPENGL\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        mode_file(path, bad[i], strlen(bad[i]));
        assert(pw_game_parent_arguments(4, &arguments, path, &storage, &options) == -EINVAL);
    }
    mode_file(path, "", 0);
    assert(pw_game_parent_arguments(4, &arguments, path, &storage, &options) == 6);
    assert(!strcmp(options.launch_mode, "REFLECT") && !strcmp(options.hw_accel, "AUTO"));
    char too_long[512];memset(too_long, 'x', sizeof(too_long));mode_file(path, too_long, sizeof(too_long));
    arguments = runelite;
    assert(pw_game_parent_arguments(4, &arguments, path, &storage, &options) == -E2BIG);
    char *explicit_mode[] = {"wine", runelite[1], "--launch-mode=FORK", "", "--hw-accel=OFF", NULL};
    arguments = explicit_mode;
    assert(pw_game_parent_arguments(5, &arguments, root, NULL, &options) == 5);
    assert(arguments == explicit_mode && !strcmp(options.mode_source, "launcher") && !strcmp(options.launch_mode, "FORK"));
    assert(!strcmp(options.hw_source, "launcher") && !strcmp(options.hw_accel, "OFF"));
    char *split_mode[] = {"wine", runelite[1], "--launch-mode", "JVM", "--mode", "OPENGL", NULL};
    arguments = split_mode;
    assert(pw_game_parent_arguments(6, &arguments, root, NULL, &options) == 6 && arguments == split_mode);
    assert(!strcmp(options.launch_mode, "JVM") && !strcmp(options.hw_accel, "OPENGL"));
    assert(!unlink(path));
    char *client_arguments[] = {"wine", runelite[1], "", "--", "--launch-mode=client-value", "client argument", NULL};
    arguments = client_arguments;
    assert(pw_game_parent_arguments(6, &arguments, path, &storage, &options) == 8);
    assert(!strcmp(arguments[3], "--launch-mode=REFLECT") && !strcmp(arguments[4], "--hw-accel=AUTO"));
    for (unsigned i = 0; i < 3; ++i) assert(arguments[i] == client_arguments[i]);
    for (unsigned i = 3; i < 6; ++i) assert(arguments[i + 2] == client_arguments[i]);
    assert(!arguments[8] && !client_arguments[6]);
    char *one_explicit[] = {"wine", runelite[1], "--launch-mode=JVM", NULL};
    arguments = one_explicit;
    assert(pw_game_parent_arguments(3, &arguments, path, &storage, &options) == 4);
    assert(!strcmp(arguments[2], "--launch-mode=JVM") && !strcmp(arguments[3], "--hw-accel=AUTO"));
    char *maximum[PW_GAME_ARGC_MAX + 2] = {"wine", runelite[1]};
    for (unsigned i = 2; i < PW_GAME_ARGC_MAX + 1; ++i) maximum[i] = "unchanged";
    maximum[PW_GAME_ARGC_MAX + 1] = NULL;arguments = maximum;
    assert(pw_game_parent_arguments(65, &arguments, path, &storage, &options) == 67);
    assert(arguments == storage.argv && !arguments[67] && !strcmp(arguments[65], "--launch-mode=REFLECT"));
    assert(!strcmp(arguments[66], "--hw-accel=AUTO"));
    for (unsigned i = 0; i < 65; ++i) assert(arguments[i] == maximum[i]);
    assert(pw_game_parent_arguments(66, &arguments, path, &storage, &options) == -EINVAL);
    assert(!setenv("JX_SESSION_ID", "opaque-parent-test", 1));
    assert(!pw_game_parent_environment(wine_ps5_game_profile(runelite[1])));
    assert(!strcmp(getenv("WINEPS5_RUNELITE_DIAG"), "1") && !strcmp(getenv("GALLIUM_DRIVER"), "zink"));
    assert(!strcmp(getenv("WINEPS5_TITLE_PRESENT"), "compositor"));
    assert(!strcmp(getenv("WINEPS5_GAME_PAD_PROFILE"), "osrs") && !strcmp(getenv("JX_SESSION_ID"), "opaque-parent-test"));
    assert(!pw_game_parent_environment(wine_ps5_game_profile(osrs[1])) && !strcmp(getenv("WINEPS5_RUNELITE_DIAG"), "0"));
    assert(!strcmp(getenv("WINEPS5_TITLE_PRESENT"), "direct"));
    assert(!strcmp(getenv("WINEPS5_GAME_PAD_PROFILE"), "osrs") && !strcmp(getenv("JX_SESSION_ID"), "opaque-parent-test"));
    assert(!pw_game_parent_environment(NULL) && !getenv("WINEPS5_GAME_PAD_PROFILE") && !getenv("WINEPS5_RUNELITE_DIAG"));
    assert(!getenv("WINEPS5_TITLE_PRESENT"));
    assert(!unsetenv("JX_SESSION_ID") && !rmdir(root));
    puts("RuneLite profile: configurable REFLECT/Java2D, exact original arguments and OSRS wire, bounded max argv, role markers PASS");
}

int main(void)
{
    test_game_profiles();
    struct pw_game_wire wire;
    struct pw_game_request request;
    char *argv[PW_GAME_WIRE_ARGV + 1];
    uint16_t original[1024], decoded[PW_GAME_ENV_BYTES / 2];
    const char entries[] = "JX_SESSION_ID=session-private\0JX_CHARACTER_ID=character-private\0"
        "JX_DISPLAY_NAME=Display Name\0PATH=C:\\windows;C:\\Game\0= C:=drive-state\0EMPTY=\0";
    unsigned bytes = sizeof(entries) * 2, decoded_bytes;
    for (unsigned i = 0; i < sizeof(entries); ++i) original[i] = (unsigned char)entries[i];
    /* Exact UTF-16 transport, including non-ASCII and surrogate pairs. */
    original[14] = 0x03a9; original[15] = 0xd83d; original[16] = 0xde00;
    char *program[] = {"C:\\Program Files\\Jagex Launcher\\Games\\Old School RuneScape\\Client\\osclient.exe",
        "", "argument with spaces", "quote\"backslash\\", "--", "--future=unchanged", NULL};
    const char *cwd = "C:\\Program Files\\Jagex Launcher\\Games\\Old School RuneScape\\Client\\";
    const char *unix_cwd = "/prefix/drive_c/Program Files/Jagex Launcher/Games/Old School RuneScape/Client/";
    assert(!pw_game_wire_encode(&wire, program, cwd, unix_cwd, original, bytes));
    assert(wire.argc == PW_GAME_PROGRAM + 6);
    assert(!pw_game_request_pack(&request, &wire));
    assert(!pw_game_request_unpack(&request, argv));
    assert(!pw_game_wire_decode(request.argc, argv, "/prefix", decoded, &decoded_bytes));
    assert(decoded_bytes == bytes && !memcmp(original, decoded, bytes));
    assert(!strcmp(argv[3], unix_cwd) && !strcmp(argv[5], cwd));
    for (unsigned i = 0; program[i]; ++i) assert(!strcmp(argv[PW_GAME_PROGRAM + i], program[i]));
    struct pw_game_argv_metrics metrics = pw_game_wire_metrics(&wire);
    unsigned total = 0;
    for (unsigned i = 0; i < wire.argc; ++i) total += strlen(wire.argv[i]) + 1;
    assert(metrics.argc == wire.argc && metrics.total_bytes == total);
    assert(metrics.max_index == 7 && metrics.max_bytes == bytes * 2 + 1);
    assert(metrics.empty == 1 && !metrics.invalid_utf8);
    assert(pw_game_arg_utf8("\xf0\x9f\x98\x80"));
    assert(!pw_game_arg_utf8("\xc0\x80") && !pw_game_arg_utf8("\xed\xa0\x80"));
    assert(!pw_game_arg_utf8("\xf4\x90\x80\x80") && !pw_game_arg_utf8("\xe2\x82"));
    test_private_handoff(&wire, original, bytes);
    /* Request offsets must describe exact strings, not hidden trailing bytes. */
    struct pw_game_request bad = request;
    bad.offsets[0] = 1; assert(pw_game_request_unpack(&bad, argv) == EPROTO);
    bad = request; bad.offsets[1]--; assert(pw_game_request_unpack(&bad, argv) == EPROTO);
    bad = request; bad.bytes--; assert(pw_game_request_unpack(&bad, argv) == EPROTO);
    assert(!pw_game_request_unpack(&request, argv));
    argv[7][0] = 'z'; assert(pw_game_wire_decode(request.argc, argv, "/prefix", decoded, &decoded_bytes) == -EINVAL);
    assert(!pw_game_request_pack(&request, &wire));
    assert(!pw_game_request_unpack(&request, argv));
    argv[3] = "/outside/prefix"; assert(pw_game_wire_decode(request.argc, argv, "/prefix", decoded, &decoded_bytes) == -EACCES);
    argv[3] = "/prefix/drive_c/../outside/"; assert(pw_game_wire_decode(request.argc, argv, "/prefix", decoded, &decoded_bytes) == -EACCES);
    argv[3] = (char *)unix_cwd;
    argv[PW_GAME_PROGRAM] = "C:\\Game\\Wow.exe";
    assert(pw_game_wire_decode(request.argc, argv, "/prefix", decoded, &decoded_bytes) == -EINVAL);
    assert(wine_ps5_game_profile("c:/Game/OSCLIENT.EXE"));
    assert(!wine_ps5_game_profile("osclient.exe") && !wine_ps5_game_profile("Z:\\Game\\osclient.exe"));
    assert(!wine_ps5_game_profile("C:\\Game\\osclient.exe.dll"));
    assert(wine_ps5_game_profile("C:\\RuneLite\\RuneLite.exe"));
    assert(wine_ps5_game_profile("c:/RuneLite/RUNELITE.EXE"));
    assert(!wine_ps5_game_profile("RuneLite.exe") && !wine_ps5_game_profile("Z:\\RuneLite\\RuneLite.exe"));
    assert(!wine_ps5_game_profile("C:\\RuneLite\\RuneLite.exe.dll") && !wine_ps5_game_profile("C:\\Game\\HDOS.exe"));
    assert(pw_game_wire_encode(&wire, program, cwd, unix_cwd, original, bytes - 2) == EINVAL);
    assert(pw_game_wire_encode(&wire, program, cwd, unix_cwd, original, bytes - 1) == EINVAL);
    assert(pw_game_wire_encode(&wire, program, cwd, unix_cwd, NULL, 0) == EINVAL);
    char *large = malloc(PW_GAME_ARGV_BYTES + 1);
    assert(large); memset(large, 'x', PW_GAME_ARGV_BYTES); large[PW_GAME_ARGV_BYTES] = 0;
    char *oversized[] = {program[0], large, NULL};
    assert(pw_game_wire_encode(&wire, oversized, cwd, unix_cwd, original, bytes) == E2BIG);
    free(large);
    char *many[PW_GAME_ARGC_MAX + 2];
    many[0] = program[0];
    for (unsigned i = 1; i <= PW_GAME_ARGC_MAX; ++i) many[i] = "";
    many[PW_GAME_ARGC_MAX + 1] = NULL;
    assert(pw_game_wire_encode(&wire, many, cwd, unix_cwd, original, bytes) == E2BIG);
    many[PW_GAME_ARGC_MAX] = NULL;
    assert(!pw_game_wire_encode(&wire, many, cwd, unix_cwd, original, bytes));
    uint16_t empty[] = {0, 0};
    assert(!pw_game_wire_encode(&wire, program, cwd, unix_cwd, empty, sizeof(empty)));
    assert(!pw_game_wire_decode(wire.argc, (char **)wire.argv, "/prefix", decoded, &decoded_bytes));
    assert(decoded_bytes == sizeof(empty) && !memcmp(empty, decoded, sizeof(empty)));
    puts("OSRS handoff: exact argv/cwd/UTF-16 environment, safe metrics, private0600 read-once/stale/failure cleanup PASS");
    return 0;
}
