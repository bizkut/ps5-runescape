#ifdef TEST_FOREVER
#undef WINE_PS5_RUNESCAPE
#endif
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include "runescape-shared-io.h"
static int report_zero_links;
static unsigned zero_link_samples;
static int permission_test_fstat(int fd, struct stat *status)
{
    int result = fstat(fd, status);
    if (!result && report_zero_links && S_ISREG(status->st_mode) && status->st_nlink == 1) {
        status->st_nlink = 0;zero_link_samples++;
    }
    return result;
}
#define fstat permission_test_fstat
#include "runescape-permissions.h"
#undef fstat
#include "runtime-game-file.h"

/* Real attribute implementation, replacing only its server path lookup and
 * DOS-xattr boundary. fstat/fchmod/open/streams below are real host syscalls. */
typedef void *HANDLE;
typedef unsigned int UINT;
typedef unsigned int NTSTATUS;
typedef int BOOL;
#define FALSE 0
#define TRUE 1
#define FILE_ATTRIBUTE_READONLY 1
#define STATUS_ACCESS_DENIED 5
#define STATUS_SUCCESS 0
#define WARN(...) ((void)0)
#define TRACE(...) ((void)0)
static const char *reported_name;
static mode_t start_umask = 0077;
static unsigned int server_get_unix_name(HANDLE handle, char **name)
{ assert(handle);*name = strdup(reported_name);assert(*name);return 0; }
static NTSTATUS errno_to_status(int error) { return (NTSTATUS)error; }
static int fd_set_dos_attrib(int fd, UINT attr, BOOL force)
{ (void)attr;(void)force;assert(fd >= 0);return 0; }
struct fd { const char *unix_name; };
#define WB_TOKEN_FILE WINE_PS5_SHARED_ROOT "/token"
static unsigned char token[16];
static unsigned chmod_calls;
static int deny_chmod;
static const char *title_c_drive = WINE_PS5_SHARED_ROOT "/prefix/drive_c";
static const char *title_w_drive = WINE_PS5_SHARED_ROOT;
static int ps5_direct_enabled(void) { return 1; }
static int observed_fchmod(int fd, mode_t mode)
{
    chmod_calls++;
    if (deny_chmod) { errno = EPERM;return -1; }
    return fchmod(fd, mode);
}
#define fchmod observed_fchmod
#define ps5_getcwd getcwd
#include "runescape-permission-functions.inc"
#undef fchmod
#undef ps5_getcwd

static mode_t mode_of(const char *path)
{ struct stat st;assert(!stat(path, &st));return st.st_mode & 07777; }
static void private_file(const char *path, mode_t mode)
{
    int fd = __real_open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !close(fd));assert(!__real_chmod(path, mode));
}
static void make_path(char out[4096], const char *base, const char *name)
{ assert(snprintf(out, 4096, "%s/%s", base, name) < 4096); }

int main(int argc, char **argv)
{
    const char *root = WINE_PS5_SHARED_ROOT;
    char regular[4096], stream[4096], directory[4096], executable[4096], outside[4096];
    char outside_file[4096], escape[4096], escaped[4096], link_file[4096], hard[4096], fifo[4096];
    assert(argc == 2);
    assert(!__real_mkdir(root, 0700));
    assert(snprintf(outside, sizeof(outside), "%s/outside", argv[1]) < (int)sizeof(outside));
    assert(!__real_mkdir(outside, 0700));
    make_path(regular, root, "regular");make_path(stream, root, "stream");
    make_path(directory, root, "private-directory");make_path(executable, root, "executable");
    make_path(outside_file, outside, "keep");make_path(escape, root, "escape");
    make_path(link_file, root, "link-file");make_path(hard, root, "hardlink");make_path(fifo, root, "fifo");
    umask(0077);
    int enabled = wine_ps5_shared_path(root);
#ifdef TEST_FOREVER
    assert(!enabled);
#else
    assert(enabled);
#endif
    assert(!strcmp(ps5_title_drive('c'), WINE_PS5_SHARED_ROOT "/prefix/drive_c"));
    assert(!strstr(ps5_title_drive('c'), "dosdevices") && ps5_title_drive('z') == NULL);
    assert(!wine_ps5_shared_path(NULL));
    char neighbor[4096];assert(snprintf(neighbor, sizeof(neighbor), "%s-neighbor", root) < (int)sizeof(neighbor));
    assert(!wine_ps5_shared_path(neighbor));
    assert(snprintf(neighbor, sizeof(neighbor), "%s/../outside", root) < (int)sizeof(neighbor));
    assert(!wine_ps5_shared_path(neighbor));
    char handoff[4096], handoff_file[4096], alias_root[4096], alias_handoff[4096];
    char handoff_neighbor[4096], nested_handoff[4096];
    make_path(handoff, root, "handoff");make_path(alias_root, root, "root-alias");
    assert(!symlink(root, alias_root));make_path(alias_handoff, alias_root, "handoff");
    char resolved_handoff[4096];
    /* Capable realpath must re-check the missing leaf after resolving its parent. */
    assert(!wine_ps5_shared_resolve(alias_handoff, resolved_handoff, realpath));
    assert(wine_ps5_private_handoff_path(handoff) == enabled);
    assert(snprintf(neighbor, sizeof(neighbor), "%s//./handoff/secret", root) < (int)sizeof(neighbor));
    assert(wine_ps5_private_handoff_path(neighbor) == enabled && !wine_ps5_shared_path(neighbor));
    make_path(handoff_neighbor, root, "handoff-other");
    assert(!wine_ps5_private_handoff_path(handoff_neighbor));
    assert(wine_ps5_shared_path(handoff_neighbor) == enabled);
    assert(!mkdir(handoff_neighbor, 0700));
    struct pw_game_wire handoff_wire;
    char *game_arguments[] = {"C:\\Games\\osclient.exe", "--synthetic", NULL};
    const uint16_t game_environment[] = {'J', 'X', '_', 'S', '=', 't', 0, 0};
    assert(!pw_game_wire_encode(&handoff_wire, game_arguments, "C:\\Games", root,
                               game_environment, sizeof(game_environment)));
    unsigned stale;
    assert(!pw_game_handoff_prepare(handoff, &stale, NULL) && !stale);
    uint8_t entropy[16] = {1};char handoff_token[33];
    pw_game_handoff_token(handoff_token, entropy);
    assert(!pw_game_handoff_write(handoff, handoff_token, &handoff_wire, NULL));
    assert(!pw_game_handoff_path(handoff, handoff_token, handoff_file));
    assert(mode_of(handoff_file) == 0600); /* the directory mode is repaired by startup repair, below */
    /* The shared-mode policy does not widen the session file or the directory. */
    assert(!chmod(handoff, 0700) && !chmod(handoff_file, 0600));
    assert(mode_of(handoff) == 0700 && mode_of(handoff_file) == 0600);
    assert(!wine_ps5_shared_resolve(alias_handoff, resolved_handoff, realpath));

    int fd = open(regular, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !close(fd));assert(mode_of(regular) == (enabled ? 0666 : 0600));
    FILE *file = fopen(stream, "w");assert(file && !fclose(file));
    assert(mode_of(stream) == (enabled ? 0666 : 0600));
    assert(!mkdir(directory, 0700));assert(mode_of(directory) == (enabled ? 0777 : 0700));
    make_path(nested_handoff, directory, "handoff");assert(!mkdir(nested_handoff, 0700));
    assert(!wine_ps5_private_handoff_path(nested_handoff));
    assert(wine_ps5_shared_path(nested_handoff) == enabled);
    fd = open(executable, O_RDWR | O_CREAT | O_EXCL, 0700);
    assert(fd >= 0 && !close(fd));assert(mode_of(executable) == (enabled ? 0766 : 0700));
    assert(!chmod(regular, 0400));assert(mode_of(regular) == (enabled ? 0666 : 0400));
    assert(!chmod(executable, 0400));assert(mode_of(executable) == (enabled ? 0766 : 0400));
    errno = 0;assert(open(regular, O_WRONLY | O_CREAT | O_EXCL, 0600) == -1 && errno == EEXIST);
    assert(!write_token());
    assert(mode_of(WB_TOKEN_FILE) == (enabled ? 0666 : 0644));

    private_file(outside_file, 0600);
    assert(!symlink(outside, escape));assert(!symlink(outside_file, link_file));
    make_path(escaped, escape, "new-private");
    fd = open(escaped, O_WRONLY | O_CREAT | O_EXCL, 0600);assert(fd >= 0 && !close(fd));
    assert(mode_of(escaped) == 0600);
    fd = open(link_file, O_RDWR | O_CREAT, 0600);assert(fd >= 0 && !close(fd));
    assert(mode_of(outside_file) == 0600); /* no world-write through outside symlink */
    make_path(escaped, escape, "new-directory");assert(!mkdir(escaped, 0700));assert(mode_of(escaped) == 0700);
    assert(!link(outside_file, hard));assert(!mkfifo(fifo, 0600));
    /* Special nodes are excluded, not silently frozen at their old mode. */
    assert(!chmod(fifo, 0400));assert(mode_of(fifo) == 0400);
    fd = __real_open(fifo, O_RDONLY | O_NONBLOCK);assert(fd >= 0);reported_name = fifo;
    assert(fd_set_file_info(fd, (HANDLE)1, FILE_ATTRIBUTE_READONLY, FALSE) == STATUS_SUCCESS);
    assert(mode_of(fifo) == 0400);
    assert(fd_set_file_info(fd, (HANDLE)1, 0, FALSE) == STATUS_SUCCESS);
    assert(mode_of(fifo) == 0600);assert(!close(fd));
    struct fd special = {fifo};
    assert(get_shared_fd_mode(&special, S_IFIFO | 0600, 0400) == 0400);
    assert(wine_ps5_shared_mode(fifo, S_IFIFO | 0600, 0400) == 0400);
    /* Actual SDK realpath is lexical: internal POSIX aliases remain an
     * unsupported title neighbor, while capable host realpath can resolve it.
     * The real title C: mapping above uses drive_c directly, without an alias. */
    char internal[4096], via[4096], canonical[4096];
    make_path(internal, root, "internal-link");assert(!symlink(directory, internal));
    make_path(via, internal, "alias-file");
    fd = open(via, O_WRONLY | O_CREAT | O_EXCL, 0600);assert(fd >= 0 && !close(fd));
    assert(mode_of(via) == 0600);
    assert(ps5_realpath(via, canonical) && !strcmp(canonical, via));
    assert(!wine_ps5_shared_resolve(via, canonical, ps5_realpath));
    assert(wine_ps5_shared_resolve(via, canonical, realpath) == enabled);

    /* Actual Wine DOS attributes and ACL projection, not a copied algorithm. */
    assert(!__real_chmod(regular, 0600));
    fd = __real_open(regular, O_RDWR);assert(fd >= 0);reported_name = regular;
    assert(fd_set_file_info(fd, (HANDLE)1, FILE_ATTRIBUTE_READONLY, FALSE) == STATUS_SUCCESS);
    assert(mode_of(regular) == (enabled ? 0666 : 0400));
    if (enabled) {
        unsigned before = chmod_calls;
        deny_chmod = 1; /* another app owns an already-shared inode */
        assert(fd_set_file_info(fd, (HANDLE)1, FILE_ATTRIBUTE_READONLY, FALSE) == STATUS_SUCCESS);
        assert(chmod_calls == before);deny_chmod = 0;
    }
    reported_name = "/app0/lib/wine/builtin.dll";
    assert(fd_set_file_info(fd, (HANDLE)1, 0, FALSE) == STATUS_ACCESS_DENIED);
    assert(!close(fd));
    fd = __real_open(hard, O_RDWR);assert(fd >= 0);reported_name = hard;
    assert(fd_set_file_info(fd, (HANDLE)1, 0, FALSE) == STATUS_SUCCESS);
    assert(mode_of(outside_file) == 0600);assert(!close(fd));
    struct fd object = {regular};
    assert((get_shared_fd_mode(&object, S_IFREG | 0600, 0777) & 07777) == (enabled ? 0666 : 0777));
    assert((get_shared_fd_mode(&object, S_IFREG | 0500, 0700) & 07777) == (enabled ? 0766 : 0700));
    object.unix_name = outside_file;
    assert(get_shared_fd_mode(&object, S_IFREG | 0600, 0400) == 0400);

    /* Older native reconstruction, exact execute-bit retention, no-follow,
     * hardlink/device safety, complete counts, and idempotence. */
    assert(!__real_chmod(regular, 0600));assert(!__real_chmod(stream, 0400));
    assert(!__real_chmod(directory, 0700));assert(!__real_chmod(executable, 0500));
    assert(!__real_chmod(root, 0700));
    struct rs_permission_report report;
    report_zero_links = 1;
    assert(!rs_repair_permissions(root, &report));
    report_zero_links = 0;
    assert(zero_link_samples); /* zero is not misclassified as multiply linked */
    assert(report.scanned >= 9 && report.changed >= 5 && report.skipped >= 4 && !report.failed);
    assert(mode_of(root) == 0777 && mode_of(directory) == 0777);
    assert(mode_of(regular) == 0666 && mode_of(stream) == 0666 && mode_of(executable) == 0766);
    assert(mode_of(outside_file) == 0600 && mode_of(fifo) == 0600);
    assert(mode_of(handoff) == 0777); /* the directory is repaired at startup; session files stay 0600 */
    assert(mode_of(handoff_file) == (enabled ? 0600 : 0666));
    assert(mode_of(handoff_neighbor) == 0777 && mode_of(nested_handoff) == 0777);
    if (enabled) {
        struct pw_game_request request;char *arguments[PW_GAME_WIRE_ARGV + 1];
        assert(!pw_game_handoff_read(handoff, handoff_token, &request, arguments, NULL));
        assert(access(handoff_file, F_OK) == -1 && errno == ENOENT);
        assert(!strcmp(arguments[PW_GAME_PROGRAM + 1], "--synthetic"));
    }
    assert(!rs_repair_permissions(root, &report) && !report.changed && !report.failed);
    assert(rs_repair_permissions("/definitely-missing-runescape-permission-fixture", &report) == -ENOENT);
    assert(report.failed == 1);
    puts(enabled ? "RuneScape shared permissions: native creates/attributes, real Wine mode boundaries, no-follow repair PASS" :
                   "Forever/control paths: original creation and DOS/ACL modes unchanged PASS");
    return 0;
}
