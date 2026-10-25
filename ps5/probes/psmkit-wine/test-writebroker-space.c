#define _GNU_SOURCE 1
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

#include "wine/ps5_writebroker.h"
static int client_mutex_lock(pthread_mutex_t *mutex);
#define pthread_mutex_lock(mutex) client_mutex_lock(mutex)
#include "wine/ps5_title_private.h"
#undef pthread_mutex_lock
#include "writebroker-menu.h"

#define WB_CHUNK (1u << 20)
#define OSRS_DATA WB_USER_ROOT "prefix/drive_c/users/steamuser/AppData/Local/Jagex/Old School Runescape/data/main_file_cache.dat2"
#define OSRS_INDEX WB_USER_ROOT "prefix/drive_c/users/steamuser/AppData/Local/Jagex/Old School Runescape/data/main_file_cache.idx0"
static unsigned char token[WB_TOKEN_BYTES];
static unsigned long long served_writes, served_bytes;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
sigset_t ps5_title_block_set;
static char directory[] = "/tmp/writebroker-space-XXXXXX";
static char menu_prefix_root[1024];
static char token_path[1024];
static char writes_path[1024];
static char osrs_data_path[1024], osrs_index_path[1024];
static char repair_data_root[1024];
#ifdef WINE_PS5_RUNESCAPE
static int pool_distinct_files, pool_fds[16];
static char pool_host_paths[16][1024], pool_wire_paths[16][96];
#endif
static int inject_other_device, inject_readdir_error, inject_closedir_error;
static int inject_zero_links;
static unsigned zero_link_before, zero_link_opened;
static int startup_order;
static unsigned int statfs_calls, source_broker_logs, source_fallback_logs, active_broker_logs;
static pthread_t threads[64];
static unsigned int thread_count;
static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t request_cond = PTHREAD_COND_INITIALIZER;
static unsigned int waiting_requests, peak_requests, lock_attempt_mask;
static int hold_requests, track_lock_attempts;
static int failures;
static uint64_t physical_write_bytes, physical_write_calls;
static uint64_t physical_read_bytes, physical_read_calls;
static int fault_write_eintr, fault_write_error, fault_write_zero, fault_read_eintr, fault_read_error;
static int fault_seek_unknown, fault_deny_read;
static int fault_seek_errno = EOPNOTSUPP;
static int fault_pin_dup, fault_zero_inode;
static size_t fault_write_limit, fault_read_limit;
static int fault_write_after = -1;
static pthread_mutex_t extent_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned extent_log_count;
static char extent_log_line[1024];
/* The optional benchmark changes this only with all server threads stopped. */
static int (*serve_write_before)(int, const struct wb_write *, unsigned char *);
static int server_write_dispatch(int, const struct wb_write *, unsigned char *);
static int negative_available;
#ifdef WINE_PS5_RUNESCAPE
static pthread_mutex_t inode_race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t inode_race_cond = PTHREAD_COND_INITIALIZER;
static struct {
    int enabled, a_read, release_a, a_done, b_done, b_same_lock;
    int a_server, b_server;
    pthread_mutex_t *a_lock;
} inode_race;
static _Thread_local int inode_race_role;
static int server_inode_mutex_lock(pthread_mutex_t *);

static struct timespec inode_race_deadline(void)
{
    struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += 10;return deadline;
}
static void inode_race_after_pread(void)
{
    if (!inode_race.enabled || inode_race_role != 1) return;
    assert(!pthread_mutex_lock(&inode_race_mutex));
    if (!inode_race.a_read) {
        struct timespec deadline = inode_race_deadline();
        inode_race.a_read = 1;assert(!pthread_cond_broadcast(&inode_race_cond));
        while (!inode_race.release_a)
            assert(!pthread_cond_timedwait(&inode_race_cond, &inode_race_mutex, &deadline));
    }
    assert(!pthread_mutex_unlock(&inode_race_mutex));
}
static void inode_race_completed(void)
{
    if (!inode_race.enabled || !inode_race_role) return;
    assert(!pthread_mutex_lock(&inode_race_mutex));
    if (inode_race_role == 1) inode_race.a_done = 1;
    else inode_race.b_done = 1;
    assert(!pthread_cond_broadcast(&inode_race_cond));
    assert(!pthread_mutex_unlock(&inode_race_mutex));
}
#endif

static void check(int condition, const char *name)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static void say(const char *format, ...)
{
    if (!strncmp(format, "extent-query ", strlen("extent-query "))) {
        char line[sizeof(extent_log_line)];va_list args;
        va_start(args, format);vsnprintf(line, sizeof(line), format, args);va_end(args);
        assert(!pthread_mutex_lock(&extent_log_mutex));
        extent_log_count++;snprintf(extent_log_line, sizeof(extent_log_line), "%s", line);
        assert(!pthread_mutex_unlock(&extent_log_mutex));
    }
}

/* Deterministic PS5-shaped counters isolate protocol/conversion behavior from
 * unrelated concurrent host-disk allocation during parallel release builds. */
struct test_statfs
{
    uint64_t f_bsize, f_blocks, f_bfree;
    int64_t f_bavail;
};
static const struct test_statfs expected_statfs = {
    .f_bsize = 16384,
    .f_blocks = 1000003,
    .f_bfree = 500009,
    .f_bavail = 400009,
};

static int test_statfs(const char *path, struct test_statfs *info)
{
    __atomic_fetch_add(&statfs_calls, 1, __ATOMIC_SEQ_CST);
    assert(!strcmp(path, WB_USER_ROOT "space"));
    *info = expected_statfs;
    if (__atomic_load_n(&negative_available, __ATOMIC_SEQ_CST)) info->f_bavail = -4;
    return 0;
}

/* Only translate host filesystem access; path validation, framing, writes and
 * replies below are the extracted production broker implementation. */
static const char *server_path(const char *path)
{
    if (!strcmp(path, OSRS_DATA)) return osrs_data_path;
    if (!strcmp(path, OSRS_INDEX)) return osrs_index_path;
#ifdef WINE_PS5_RUNESCAPE
    if (pool_distinct_files)
        for (unsigned i = 0; i < 16; i++)
            if (!strcmp(path, pool_wire_paths[i])) return pool_host_paths[i];
#endif
    return !strcmp(path, WB_USER_ROOT "writes") ? writes_path : path;
}
static int server_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;va_start(args, flags);mode = (mode_t)va_arg(args, int);va_end(args);
    }
    if (fault_deny_read && !strcmp(path, OSRS_DATA) && (flags & O_ACCMODE) != O_WRONLY) {
        errno = EACCES;return -1;
    }
    return open(server_path(path), flags, mode);
}
#ifdef WINE_PS5_RUNESCAPE
static ssize_t server_pread(int fd, void *buffer, size_t length, off_t offset)
{
    __atomic_fetch_add(&physical_read_calls, 1, __ATOMIC_SEQ_CST);
    if (fault_read_eintr) { fault_read_eintr--;errno = EINTR;return -1; }
    if (fault_read_error) { errno = fault_read_error;return -1; }
    if (fault_read_limit && length > fault_read_limit) length = fault_read_limit;
    ssize_t result = pread(fd, buffer, length, offset);
    if (result > 0) __atomic_fetch_add(&physical_read_bytes, result, __ATOMIC_SEQ_CST);
    if (result > 0) inode_race_after_pread();
    return result;
}
static off_t server_lseek(int fd, off_t offset, int whence)
{
    if (fault_seek_unknown && (whence == SEEK_DATA || whence == SEEK_HOLE)) {
        errno = fault_seek_errno;return -1;
    }
    return lseek(fd, offset, whence);
}
static int server_dup(int fd)
{
    if (fault_pin_dup) { errno = EMFILE;return -1; }
    return dup(fd);
}
#endif

static ssize_t server_pwrite(int fd, const void *buffer, size_t length, off_t offset)
{
    assert(!pthread_mutex_lock(&request_lock));
    if (hold_requests)
    {
        waiting_requests++;
        if (waiting_requests > peak_requests) peak_requests = waiting_requests;
        assert(!pthread_cond_broadcast(&request_cond));
        while (hold_requests) assert(!pthread_cond_wait(&request_cond, &request_lock));
        waiting_requests--;
    }
    assert(!pthread_mutex_unlock(&request_lock));
    __atomic_fetch_add(&physical_write_calls, 1, __ATOMIC_SEQ_CST);
    if (fault_write_eintr) { fault_write_eintr--;errno = EINTR;return -1; }
    if (fault_write_after == 0) {
        if (fault_write_zero) return 0;
        errno = fault_write_error ? fault_write_error : ENOSPC;return -1;
    }
    if (fault_write_after > 0) fault_write_after--;
    if (fault_write_limit && length > fault_write_limit) length = fault_write_limit;
    ssize_t result = pwrite(fd, buffer, length, offset);
    if (result > 0) __atomic_fetch_add(&physical_write_bytes, result, __ATOMIC_SEQ_CST);
    return result;
}

static int repair_test_fstatat(int fd, const char *name, struct stat *status, int flags)
{
    int result = fstatat(fd, name, status, flags);
    if (!result && inject_other_device && !strcmp(name, "other-device"))
        status->st_dev++;
    if (!result && inject_zero_links && S_ISREG(status->st_mode) && status->st_nlink == 1) {
        status->st_nlink = 0;zero_link_before++;
    }
    return result;
}

static int repair_test_fstat(int fd, struct stat *status)
{
    int result = fstat(fd, status);
    if (!result && inject_zero_links && S_ISREG(status->st_mode) && status->st_nlink == 1) {
        status->st_nlink = 0;zero_link_opened++;
    }
    if (!result && fault_zero_inode) status->st_ino = 0;
    return result;
}


static struct dirent *repair_test_readdir(DIR *directory_stream)
{
    if (inject_readdir_error)
    {
        inject_readdir_error = 0;
        errno = EIO;
        return NULL;
    }
    return readdir(directory_stream);
}

static int repair_test_closedir(DIR *directory_stream)
{
    int result = closedir(directory_stream);
    if (!result && inject_closedir_error)
    {
        inject_closedir_error = 0;
        errno = EIO;
        return -1;
    }
    return result;
}

static int fake_startup_repair(void)
{
    check(!startup_order, "broker startup repairs before token creation");
    startup_order = 1;
    return 0;
}

static int fake_startup_token(void)
{
    check(startup_order == 1, "broker token is created after startup repair");
    startup_order = 2;
    return 0;
}

#define statfs test_statfs
#define open(...) server_open(__VA_ARGS__)
#define pread(fd, buffer, length, offset) server_pread(fd, buffer, length, offset)
#define lseek(fd, offset, whence) server_lseek(fd, offset, whence)
#define pwrite(fd, buffer, length, offset) server_pwrite(fd, buffer, length, offset)
#ifdef WINE_PS5_RUNESCAPE
#define dup(fd) server_dup(fd)
#endif
#define WB_MENU_PREFIX_ROOT menu_prefix_root
#define WB_MENU_REMOVE_DEPTH 64u
enum { TEST_REPAIR_MAX_ENTRIES = 32, TEST_REPAIR_MAX_DEPTH = 8 };
#undef WB_REPAIR_MAX_ENTRIES
#undef WB_REPAIR_MAX_DEPTH
#define WB_REPAIR_DATA_ROOT repair_data_root
#define WB_REPAIR_MAX_ENTRIES 32u
#define WB_REPAIR_MAX_DEPTH 8u
#define WB_REPAIR_FSTATAT repair_test_fstatat
#define fstat repair_test_fstat
#define WB_REPAIR_READDIR repair_test_readdir
#define WB_REPAIR_CLOSEDIR repair_test_closedir
#define WB_STARTUP_REPAIR fake_startup_repair
#define WB_STARTUP_WRITE_TOKEN fake_startup_token
#include "writebroker-server.inc"
#undef fstat
#undef WB_STARTUP_WRITE_TOKEN
#undef WB_STARTUP_REPAIR
#undef WB_REPAIR_CLOSEDIR
#undef WB_REPAIR_READDIR
#undef WB_REPAIR_FSTATAT
#undef WB_REPAIR_MAX_DEPTH
#undef WB_REPAIR_MAX_ENTRIES
#undef WB_REPAIR_DATA_ROOT
#undef WB_MENU_PREFIX_ROOT
#undef WB_MENU_REMOVE_DEPTH
#undef statfs
#undef pwrite
#undef open
#undef pread
#undef lseek
#undef dup
#ifdef WINE_PS5_RUNESCAPE
static int server_inode_mutex_lock(pthread_mutex_t *mutex)
{
    if (inode_race.enabled && inode_race_role) {
        int stripe = 0;
        for (unsigned i = 0; i < sizeof(wb_inode_locks) / sizeof(wb_inode_locks[0]); i++)
            stripe |= mutex == &wb_inode_locks[i];
        assert(stripe);
        assert(!pthread_mutex_lock(&inode_race_mutex));
        if (inode_race_role == 1) inode_race.a_lock = mutex;
        else if (mutex == inode_race.a_lock) {
            inode_race.b_same_lock = 1;assert(!pthread_cond_broadcast(&inode_race_cond));
        }
        assert(!pthread_mutex_unlock(&inode_race_mutex));
    }
    return pthread_mutex_lock(mutex);
}
#endif

static int server_write_dispatch(int client, const struct wb_write *request, unsigned char *buffer)
{
    int result = serve_write_before ? serve_write_before(client, request, buffer) : serve_write(client, request, buffer);
#ifdef WINE_PS5_RUNESCAPE
    inode_race_completed();
#endif
    return result;
}

static int start_server(void *(*entry)(void *), int fd)
{
    assert(!pthread_mutex_lock(&server_lock));
    assert(thread_count < sizeof(threads) / sizeof(threads[0]));
    assert(!pthread_create(&threads[thread_count++], NULL, entry, (void *)(intptr_t)fd));
    assert(!pthread_mutex_unlock(&server_lock));
    return 0;
}

static int connection(void)
{
    int pair[2];
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_reply reply;

    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    start_server(serve, pair[1]);
    memcpy(hello.token, token, sizeof(token));
    assert(!transfer(pair[0], &hello, sizeof(hello), 1));
    assert(!transfer(pair[0], &reply, sizeof(reply), 0));
    assert(reply.magic == WB_MAGIC && !reply.result);
    return pair[0];
}

static struct wb_statfs_reply query(int fd, const char *path)
{
    struct wb_write request = {.magic = WB_STATFS_MAGIC, .path_length = strlen(path)};
    struct wb_statfs_reply reply;

    assert(!transfer(fd, &request, sizeof(request), 1));
    assert(!transfer(fd, (void *)path, request.path_length, 1));
    assert(!transfer(fd, &reply, sizeof(reply), 0));
    return reply;
}

static void protocol_tests(void)
{
    struct test_statfs expected = expected_statfs;
    struct wb_statfs_reply reply;
    struct wb_write malformed[] = {
        {.magic = WB_STATFS_MAGIC},
        {.magic = WB_STATFS_MAGIC, .path_length = WB_MAX_PATH + 1},
        {.magic = WB_STATFS_MAGIC, .path_length = 1, .length = 1},
        {.magic = WB_STATFS_MAGIC, .path_length = 1, .offset = 1},
        {.magic = WB_STATFS_MAGIC, .path_length = 1, .reserved = 1},
    };
    static const char *const refused[] = {
        "/tmp/outside", WB_USER_ROOT "../outside", "/data/runescape-other/space", WB_USER_ROOT,
    };
    int fd = connection();
    {
        struct wb_write capability = {.magic = WB_MENU_CAPS_MAGIC};
        struct wb_reply answer;
        assert(!transfer(fd, &capability, sizeof(capability), 1));
        assert(!transfer(fd, &answer, sizeof(answer), 0));
        check(answer.magic == WB_MAGIC && answer.result == WB_MENU_CAPS_REMOVE,
              "menu removal capability advertised");
    }
    {
        static const char outside[] =
            WB_USER_ROOT "prefix/drive_c/Program Files (x86)/World of Warcraft";
        struct wb_write removal = {
            .magic = WB_MENU_REMOVE_MAGIC, .path_length = sizeof(outside) - 1};
        struct wb_reply answer;
        assert(!transfer(fd, &removal, sizeof(removal), 1));
        assert(!transfer(fd, (void *)outside, removal.path_length, 1));
        assert(!transfer(fd, &answer, sizeof(answer), 0));
        check(answer.magic == WB_MAGIC && answer.result == -EACCES,
              "menu broker refuses every non-Battle.net root");
        check(menu_remove_path_allowed(
                  WB_USER_ROOT "prefix/drive_c/Program Files (x86)/Battle.net") &&
              menu_remove_path_allowed(
                  WB_USER_ROOT "prefix/drive_c/ProgramData/Battle.net") &&
              menu_remove_path_allowed(
                  WB_USER_ROOT "prefix/drive_c/users/steamuser/AppData/Local/Battle.net") &&
              menu_remove_path_allowed(WB_USER_ROOT "prefix/drive_c/Battle.net-Setup.exe") &&
              !menu_remove_path_allowed(
                  WB_USER_ROOT "prefix/drive_c/Program Files (x86)/Battle.net/child"),
              "menu broker deletion roots are the exact fixed set");
    }

    reply = query(fd, WB_USER_ROOT "space");
    check(reply.magic == WB_MAGIC && !reply.result, "query reply accepted");
    check(reply.bsize == (uint64_t)expected.f_bsize && reply.blocks == expected.f_blocks,
          "deterministic statfs block size and total");
    check(reply.bfree == expected.f_bfree && reply.bavail == (uint64_t)expected.f_bavail,
          "deterministic statfs free and available");
    __atomic_store_n(&negative_available, 1, __ATOMIC_SEQ_CST);
    reply = query(fd, WB_USER_ROOT "space");
    check(!reply.result && !reply.bavail, "negative statfs availability serialized as zero");

    check(reply.bfree > 0, "negative availability preserves actual free blocks");
    __atomic_store_n(&negative_available, 0, __ATOMIC_SEQ_CST);
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
    {
        unsigned int calls = statfs_calls;
        reply = query(fd, refused[i]);
        check(reply.magic == WB_MAGIC && reply.result == -EACCES, "outside root refused");
        check(statfs_calls == calls, "refused path does not reach statfs");
    }
    reply = query(fd, WB_USER_ROOT "space");
    check(!reply.result, "refused query preserves request stream");
    close(fd);
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++)
    {
        fd = connection();
        assert(!transfer(fd, &malformed[i], sizeof(malformed[i]), 1));
        check(read(fd, &reply, sizeof(reply)) == 0, "malformed query drops connection");
        close(fd);
    }
    fd = connection();
    {
        char path[] = WB_USER_ROOT "sp\0ace";
        struct wb_write request = {.magic = WB_STATFS_MAGIC, .path_length = sizeof(path) - 1};
        assert(!transfer(fd, &request, sizeof(request), 1));
        assert(!transfer(fd, path, request.path_length, 1));
        check(read(fd, &reply, sizeof(reply)) == 0, "embedded NUL query refused");
    }
    close(fd);
}
static void fixture_mkdirs(const char *path)
{
    char copy[1024];
    size_t length = strlen(path);
    assert(length < sizeof(copy));
    memcpy(copy, path, length + 1);
    for (char *p = copy + 1; ; p++)
    {
        if (*p != '/' && *p) continue;
        char saved = *p;
        *p = 0;
        assert(!mkdir(copy, 0777) || errno == EEXIST);
        if (!saved) break;
        *p = saved;
    }
}

static void fixture_put(const char *path, const char *text)
{
    char parent[1024];
    assert(strlen(path) < sizeof(parent));
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    assert(slash);
    *slash = 0;
    fixture_mkdirs(parent);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(!close(fd));
}

static int menu_remove_request(const char *path)
{
    struct wb_write request = {
        .magic = WB_MENU_REMOVE_MAGIC, .path_length = (uint32_t)strlen(path)};
    struct wb_reply reply;
    int fd = connection();
    assert(!transfer(fd, &request, sizeof(request), 1));
    assert(!transfer(fd, (void *)path, request.path_length, 1));
    assert(!transfer(fd, &reply, sizeof(reply), 0));
    close(fd);
    assert(reply.magic == WB_MAGIC);
    return reply.result;
}


static mode_t fixture_mode(const char *path)
{
    struct stat status;
    assert(!lstat(path, &status));
    return status.st_mode & 07777;
}

static void permission_repair_tests(void)
{
    char forever[1024], prefix[1024], home[1024], cache[1024], locked[1024];
    char child[1024], executable[1024], private_dir[1024], made[1024];
    char token_file[1024], outside[1024], link_path[1024], unmanaged[1024];
    char hardlinked[1024], hardlink_outside[1024], other_device[1024];
    char legacy_parent[1024], legacy_root[1024], legacy_file[1024], sibling[1024];
    char store_parent[1024], store_root[1024], store_file[1024];
    char handoff[1024], handoff_secret[1024], handoff_neighbor[1024], nested_handoff[1024];
    struct wb_permission_repair repair = {0};
    struct stat status;
    dev_t device;
    int data_fd, result;

    snprintf(repair_data_root, sizeof(repair_data_root), "%s/repair-data", directory);
    snprintf(forever, sizeof(forever), "%s/runescape", repair_data_root);
    snprintf(prefix, sizeof(prefix), "%s/prefix", forever);
    snprintf(home, sizeof(home), "%s/home", forever);
    snprintf(cache, sizeof(cache), "%s/radv-shader-cache", forever);
    snprintf(locked, sizeof(locked), "%s/locked", prefix);
    snprintf(child, sizeof(child), "%s/child.txt", locked);
    snprintf(executable, sizeof(executable), "%s/run.exe", locked);
    snprintf(private_dir, sizeof(private_dir), "%s/private", prefix);
    snprintf(token_file, sizeof(token_file), "%s/.writebroker-token", forever);
    snprintf(outside, sizeof(outside), "%s/outside.txt", repair_data_root);
    snprintf(unmanaged, sizeof(unmanaged), "%s/unmanaged.txt", forever);
    snprintf(hardlinked, sizeof(hardlinked), "%s/hardlinked", prefix);
    snprintf(hardlink_outside, sizeof(hardlink_outside), "%s/hardlink-outside", repair_data_root);
    snprintf(other_device, sizeof(other_device), "%s/other-device", prefix);
    snprintf(legacy_parent, sizeof(legacy_parent), "%s/homebrew/PPSA99225", repair_data_root);
    snprintf(legacy_root, sizeof(legacy_root), "%s/wine", legacy_parent);
    snprintf(legacy_file, sizeof(legacy_file), "%s/prefix/system.reg", legacy_root);
    snprintf(sibling, sizeof(sibling), "%s/app-file", legacy_parent);
    snprintf(store_parent, sizeof(store_parent), "%s/prosperostore/previous/PPSA99225",
             repair_data_root);
    snprintf(store_root, sizeof(store_root), "%s/wine", store_parent);
    snprintf(store_file, sizeof(store_file), "%s/home/settings", store_root);
    snprintf(handoff, sizeof(handoff), "%s/handoff", forever);
    snprintf(handoff_secret, sizeof(handoff_secret), "%s/request", handoff);
    snprintf(handoff_neighbor, sizeof(handoff_neighbor), "%s/handoff-other", forever);
    snprintf(nested_handoff, sizeof(nested_handoff), "%s/handoff/request", prefix);

    fixture_put(child, "child");
    fixture_put(executable, "exec");
    fixture_mkdirs(private_dir);
    fixture_mkdirs(home);
    fixture_mkdirs(cache);
    fixture_put(token_file, "private token");
    fixture_put(outside, "outside");
    fixture_put(unmanaged, "not an allowlisted subtree");
    fixture_put(hardlinked, "linked");
    assert(!link(hardlinked, hardlink_outside));
    fixture_put(other_device, "mounted");
    fixture_put(legacy_file, "registry");
    fixture_put(sibling, "immutable sibling");
    fixture_put(store_file, "settings");
    fixture_put(handoff_secret, "synthetic private request");
    fixture_put(handoff_neighbor, "ordinary shared file");
    fixture_put(nested_handoff, "ordinary nested file");
    assert(!chmod(handoff, 0700) && !chmod(handoff_secret, 0600));
    assert(!chmod(handoff_neighbor, 0600) && !chmod(nested_handoff, 0600));
    snprintf(link_path, sizeof(link_path), "%s/outside-link", locked);
    assert(!symlink(outside, link_path));

    assert(!chmod(forever, 0555));
    assert(!chmod(locked, 0555));
    assert(!chmod(private_dir, 0700));
    assert(!chmod(child, 0400));
    assert(!chmod(executable, 0500));
    assert(!chmod(token_file, 0600));
    assert(!chmod(outside, 0400));
    assert(!chmod(unmanaged, 0400));
    assert(!chmod(hardlinked, 0400));
    assert(!chmod(other_device, 0400));
    assert(!chmod(legacy_parent, 0555));
    assert(!chmod(legacy_file, 0400));
    assert(!chmod(sibling, 0400));
    assert(!chmod(store_file, 0400));
    snprintf(made, sizeof(made), "%s/new", locked);
    errno = 0;
    check(mkdir(made, 0700) && (errno == EACCES || errno == EPERM),
          "locked directory makes the title-like write fail before startup repair");

    inject_other_device = inject_zero_links = 1;
    result = repair_all(&repair);
    inject_other_device = inject_zero_links = 0;
    check(zero_link_before && zero_link_opened,
          "startup repair accepts PS5 zero-link metadata before and after opening a regular file");
    check(!result && !repair.failed && repair.changed >= 8 && repair.skipped >= 3,
          "production startup repair reports changed, skipped and failed counts");
    check(fixture_mode(forever) == 0777 && fixture_mode(locked) == 0777 &&
          fixture_mode(private_dir) == 0777 && fixture_mode(child) == 0666 &&
          fixture_mode(executable) == 0777,
          "startup repair makes 0555/0700 directories 0777 and preserves executable files");
    check(!mkdir(made, 0700), "startup repair permits the failed title step to retry once");
    check(!lstat(link_path, &status) && S_ISLNK(status.st_mode) &&
          fixture_mode(outside) == 0400,
          "startup repair skips an internal symlink without changing its target");
    check(fixture_mode(hardlinked) == 0400 && fixture_mode(hardlink_outside) == 0400,
          "startup repair skips multiply-linked regular files");
    check(fixture_mode(other_device) == 0400,
          "startup repair skips an entry reported on another device");
    check(fixture_mode(handoff) == 0700 && fixture_mode(handoff_secret) == 0600,
          "startup repair preserves the private top-level handoff directory and its contents");
    check(fixture_mode(nested_handoff) == 0666,
          "nested prefix/handoff is ordinary shared data, not the private top-level exception");
#ifdef WINE_PS5_RUNESCAPE
    check(fixture_mode(token_file) == 0666 && fixture_mode(unmanaged) == 0666,
          "RuneScape shared root repair covers token and arbitrary user-created entries");
    check(fixture_mode(handoff_neighbor) == 0666,
          "handoff-other is ordinary shared data, not a prefix-match exception");
#else
    check(fixture_mode(token_file) == 0600,
          "startup repair never widens the authentication token");
    check(fixture_mode(unmanaged) == 0400,
          "startup repair does not recurse through unknown forever-root children");
#endif
    check(fixture_mode(legacy_parent) == 0777 && fixture_mode(legacy_file) == 0666 &&
          fixture_mode(sibling) == 0400,
          "legacy repair changes only the rename parent and recursive wine tree");
    check(fixture_mode(store_parent) == 0777 && fixture_mode(store_file) == 0666,
          "store legacy root and rename parent are repaired");

    data_fd = repair_open_data_root(&device);
    assert(data_fd >= 0);
#ifdef WINE_PS5_RUNESCAPE
    memset(&repair, 0, sizeof(repair));repair.device = device;
    check(!repair_root(data_fd, "runescape/handoff", 1, &repair) && repair.skipped == 1 &&
          !repair.changed && fixture_mode(handoff) == 0700 && fixture_mode(handoff_secret) == 0600,
          "direct repair-root calls cannot bypass the private subtree exclusion");
#endif
    assert(!rmdir(home));
    assert(!symlink(outside, home));
    memset(&repair, 0, sizeof(repair));
    repair.device = device;
    result = repair_root(data_fd, "runescape/home", 1, &repair);
    check(result < 0 && repair.failed,
          "startup repair refuses a symlinked fixed root");
    assert(!unlink(home));
    fixture_mkdirs(home);

    {
        char budget[1024], entry[1100];
        snprintf(budget, sizeof(budget), "%s/budget", prefix);
        fixture_mkdirs(budget);
        for (unsigned i = 0; i < 40; i++)
        {
            snprintf(entry, sizeof(entry), "%s/link-%02u", budget, i);
            assert(!symlink(outside, entry));
        }
        memset(&repair, 0, sizeof(repair));
        repair.device = device;
        result = repair_root(data_fd, "runescape/prefix/budget", 1, &repair);
        check(result == -E2BIG && repair.scanned == TEST_REPAIR_MAX_ENTRIES &&
              repair.skipped,
              "visited-entry budget is charged for skipped symlinks");
    }

    memset(&repair, 0, sizeof(repair));
    repair.device = device;
    inject_readdir_error = 1;
    result = repair_root(data_fd, "runescape/radv-shader-cache", 1, &repair);
    check(result == -EIO && repair.failed,
          "root readdir error is reported as repair failure");
    memset(&repair, 0, sizeof(repair));
    repair.device = device;
    inject_closedir_error = 1;
    result = repair_root(data_fd, "runescape/radv-shader-cache", 1, &repair);
    check(result == -EIO && repair.failed,
          "root closedir error is reported as repair failure");

    {
        char deep[1024];
        snprintf(deep, sizeof(deep), "%s/depth", home);
        fixture_mkdirs(deep);
        for (unsigned i = 0; i < TEST_REPAIR_MAX_DEPTH + 2; i++)
        {
            size_t used = strlen(deep);
            assert(used + 3 < sizeof(deep));
            memcpy(deep + used, "/d", 3);
            fixture_mkdirs(deep);
        }
        memset(&repair, 0, sizeof(repair));
        repair.device = device;
        result = repair_root(data_fd, "runescape/home", 1, &repair);
        check(result == -E2BIG && repair.failed,
              "startup repair stops at its production depth bound");
    }
    close(data_fd);

    startup_order = 0;
    check(!broker_prepare_startup() && startup_order == 2,
          "production broker startup orders repair before token");
}

static void menu_removal_tests(void)
{
    char app[1024], wow[1024], outside[1024], ancestor[1024], local[1024], link[1024];
    struct stat status;

    snprintf(menu_prefix_root, sizeof(menu_prefix_root), "%s/menu-prefix", directory);
    snprintf(app, sizeof(app), "%s/drive_c/Program Files (x86)/Battle.net", menu_prefix_root);
    snprintf(wow, sizeof(wow), "%s/drive_c/Program Files (x86)/World of Warcraft/keep", menu_prefix_root);
    fixture_put(wow, "keep");
    {
        char nested[1024];
        snprintf(nested, sizeof(nested), "%s/sub/dir/file", app);
        fixture_put(nested, "remove");
    }
    check(!menu_remove_request(WB_USER_ROOT "prefix/drive_c/Program Files (x86)/Battle.net"),
          "production broker recursively deletes an accepted Battle.net root");
    check(lstat(app, &status) && errno == ENOENT, "accepted Battle.net root is gone");
    check(!stat(wow, &status) && S_ISREG(status.st_mode), "accepted deletion keeps WoW");

    /* A file at an allowed root is unlinked without attempting directory traversal. */
    fixture_put(app, "regular root");
    check(!menu_remove_request(WB_USER_ROOT "prefix/drive_c/Program Files (x86)/Battle.net"),
          "production broker removes a non-directory allowed root");
    check(lstat(app, &status) && errno == ENOENT, "non-directory root is gone");

    snprintf(outside, sizeof(outside), "%s/outside/Battle.net/keep", directory);
    fixture_put(outside, "outside");
    snprintf(ancestor, sizeof(ancestor), "%s/drive_c/ProgramData", menu_prefix_root);
    {
        char parent[1024], target[1024];
        snprintf(parent, sizeof(parent), "%s/drive_c", menu_prefix_root);
        fixture_mkdirs(parent);
        snprintf(target, sizeof(target), "%s/outside", directory);
        assert(!symlink(target, ancestor));
    }
    check(menu_remove_request(WB_USER_ROOT "prefix/drive_c/ProgramData/Battle.net") < 0,
          "production broker refuses a symlinked ancestor");
    check(!stat(outside, &status) && S_ISREG(status.st_mode), "ancestor symlink keeps outside tree");
    assert(!unlink(ancestor));

    snprintf(local, sizeof(local), "%s/drive_c/users/steamuser/AppData/Local/Battle.net",
             menu_prefix_root);
    fixture_mkdirs(local);
    snprintf(link, sizeof(link), "%s/wow-link", local);
    {
        char target[1024];
        snprintf(target, sizeof(target), "%s/drive_c/Program Files (x86)/World of Warcraft",
                 menu_prefix_root);
        assert(!symlink(target, link));
    }
    check(!menu_remove_request(
              WB_USER_ROOT "prefix/drive_c/users/steamuser/AppData/Local/Battle.net"),
          "production broker unlinks an internal symlink without descending");
    check(!stat(wow, &status) && S_ISREG(status.st_mode), "internal symlink keeps WoW");
}

typedef uintptr_t HANDLE;
typedef int32_t LONG;
typedef int64_t LONG64;
typedef uint64_t ULONGLONG;
typedef unsigned int NTSTATUS;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct {
    LARGE_INTEGER TotalAllocationUnits, CallerAvailableAllocationUnits, ActualAvailableAllocationUnits;
    uint32_t SectorsPerAllocationUnit, BytesPerSector;
} FILE_FS_FULL_SIZE_INFORMATION;
typedef struct {
    uint64_t ActualTotalAllocationUnits, ActualAvailableAllocationUnits, ActualPoolUnavailableAllocationUnits;
    uint64_t CallerAvailableAllocationUnits, CallerPoolUnavailableAllocationUnits, UsedAllocationUnits;
    uint64_t CallerTotalAllocationUnits, TotalReservedAllocationUnits, VolumeStorageReserveAllocationUnits;
    uint64_t AvailableCommittedAllocationUnits, PoolAvailableAllocationUnits;
    uint32_t SectorsPerAllocationUnit, BytesPerSector;
} FILE_FS_FULL_SIZE_INFORMATION_EX;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_REQUEST 1
#define TRUE 1
#define FALSE 0
#define InterlockedExchange(pointer, value) __atomic_exchange_n(pointer, value, __ATOMIC_SEQ_CST)
#define InterlockedCompareExchange(pointer, value, expected) \
    __sync_val_compare_and_swap(pointer, expected, value)
#define min(a, b) ((a) < (b) ? (a) : (b))

static LONG64 clock_us = 1000000;
static _Thread_local unsigned int current_thread_id;
static _Thread_local int observed_slot = -1;
static int broker_mode, title_enabled = 1, connected, old_queries;
static const char *handle_path = WB_USER_ROOT "space";
static unsigned int errno_to_status(int error) { return (unsigned int)error; }
static LONG64 title_io_now_us(void) { return clock_us; }
static int ps5_direct_enabled(void) { return title_enabled; }
static unsigned int GetCurrentThreadId(void) { return current_thread_id; }
static unsigned int server_get_unix_name(HANDLE handle, char **path)
{
    const char *name = handle == 2 ? WB_USER_ROOT "writes" :
                       handle == 3 ? OSRS_DATA : handle == 4 ? OSRS_INDEX : handle_path;
#ifdef WINE_PS5_RUNESCAPE
    if (handle == 2 && pool_distinct_files) name = pool_wire_paths[(current_thread_id >> 2) % 16];
#endif
    *path = strdup(name);
    assert(*path);
    return 0;
}
static void sceKernelDebugOutText(int channel, const char *line)
{
    (void)channel;
    if (strstr(line, "disk space source=broker"))
        __atomic_fetch_add(&source_broker_logs, 1, __ATOMIC_SEQ_CST);
    if (strstr(line, "disk space source=synthetic"))
        __atomic_fetch_add(&source_fallback_logs, 1, __ATOMIC_SEQ_CST);
    if (strstr(line, "write broker active"))
        __atomic_fetch_add(&active_broker_logs, 1, __ATOMIC_SEQ_CST);
}
static int client_open(const char *path, int flags)
{
    return open(!strcmp(path, WB_TOKEN_FILE) ? token_path : path, flags);
}
static void *old_server(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct wb_hello hello;
    struct wb_reply reply = {.magic = WB_MAGIC, .result = -EPROTONOSUPPORT};
    struct wb_write request;

    assert(!transfer(fd, &hello, sizeof(hello), 0));
    assert(hello.magic == WB_MAGIC && hello.version == WB_VERSION);
    assert(!transfer(fd, &reply, sizeof(reply), 1));
    if (read(fd, &request, sizeof(request)) > 0) old_queries++;
    close(fd);
    return NULL;
}
static int client_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    int pair[2];
    const struct sockaddr_in *endpoint = (const struct sockaddr_in *)address;
    assert(length == sizeof(*endpoint) && endpoint->sin_family == AF_INET);
    assert(ntohs(endpoint->sin_port) == WB_PORT &&
           endpoint->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    __atomic_fetch_add(&connected, 1, __ATOMIC_SEQ_CST);
    if (!broker_mode) { errno = ECONNREFUSED; return -1; }
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(dup2(pair[0], fd) == fd);
    close(pair[0]);
    return start_server(broker_mode == 1 ? serve : old_server, pair[1]);
}
static int synthetic_fstatvfs(int fd, struct statvfs *info)
{
    if (!title_enabled) return fstatvfs(fd, info);
    memset(info, 0, sizeof(*info));
    info->f_bsize = info->f_frsize = 32768;
    info->f_blocks = (UINT64_C(64) << 30) / 32768;
    info->f_bfree = info->f_bavail = (UINT64_C(16) << 30) / 32768;
    return 0;
}

/* Windows virtual-memory checks become ordinary host-memory access. The
 * actual bounce pool and complete client request/write code are extracted. */
static int virtual_check_buffer_for_read(const void *buffer, size_t length)
{
    return buffer || !length;
}

static size_t virtual_uninterrupted_read_memory(const void *source, void *destination, size_t length)
{
    memcpy(destination, source, length);
    return length;
}


/* Exercise the title's statvfs branch, including the existing shim fallback. */
#undef linux
#define __PROSPERO__ 1
#define open(path, flags) client_open(path, flags)
#define connect(fd, address, length) client_connect(fd, address, length)
#define fstatvfs(fd, info) synthetic_fstatvfs(fd, info)
#define pthread_mutex_lock(mutex) client_mutex_lock(mutex)
#include "writebroker-client.inc"
#undef pthread_mutex_lock
#undef fstatvfs
#undef connect
#undef open

static int client_mutex_lock(pthread_mutex_t *mutex)
{
    for (unsigned int i = 0; i < BROKER_CONNECTIONS; i++)
    {
        if (mutex != &broker[i].lock) continue;
        assert(!pthread_mutex_lock(&request_lock));
        if (track_lock_attempts)
        {
            lock_attempt_mask |= 1u << i;
            assert(!pthread_cond_broadcast(&request_cond));
        }
        assert(!pthread_mutex_unlock(&request_lock));
        observed_slot = i;
        break;
    }
    return pthread_mutex_lock(mutex);
}

static void close_connections(void)
{
    pthread_once(&broker_once, broker_init);
    for (unsigned int i = 0; i < BROKER_CONNECTIONS; i++)
        if (broker[i].fd >= 0) { close(broker[i].fd); broker[i].fd = -1; }
    for (unsigned int i = 0; i < thread_count; i++) assert(!pthread_join(threads[i], NULL));
    thread_count = 0;
    broker_retry_at = 0;
}

static void client_tests(void)
{
    FILE_FS_FULL_SIZE_INFORMATION info;
    FILE_FS_FULL_SIZE_INFORMATION_EX extended;
    struct test_statfs expected = expected_statfs;
    int fd = open(directory, O_RDONLY | O_DIRECTORY), attempts;

    assert(fd >= 0);
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS, "absent broker falls back successfully");
    check(info.TotalAllocationUnits.QuadPart == (INT64_C(64) << 30) / 4096 &&
          info.CallerAvailableAllocationUnits.QuadPart == (INT64_C(16) << 30) / 4096,
          "absent broker preserves synthetic size");
    attempts = connected;
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS && connected == attempts,
          "absent broker respects existing retry interval");
    clock_us += 31000000;
    broker_mode = 2;
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS, "old broker falls back successfully");
    check(info.TotalAllocationUnits.QuadPart == (INT64_C(64) << 30) / 4096 &&
          info.ActualAvailableAllocationUnits.QuadPart == (INT64_C(16) << 30) / 4096,
          "old broker preserves synthetic size");
    errno = 0;
    check(broker_connect() == -1 && errno == EPROTONOSUPPORT,
          "rejected broker hello preserves its actual protocol status");
    close_connections();
    check(!old_queries, "old broker receives no statfs request after version rejection");
    clock_us += 31000000;
    broker_mode = 1;
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS, "live broker size query succeeds");
    check(info.TotalAllocationUnits.QuadPart == (int64_t)((uint64_t)expected.f_bsize * expected.f_blocks / 4096),
          "Windows total uses broker result");
    check(info.CallerAvailableAllocationUnits.QuadPart == (int64_t)((uint64_t)expected.f_bsize * expected.f_bavail / 4096),
          "Windows caller availability uses broker result");
    check(info.ActualAvailableAllocationUnits.QuadPart == (int64_t)((uint64_t)expected.f_bsize * expected.f_bfree / 4096),
          "Windows actual availability uses broker result");
    check(info.BytesPerSector == 512 && info.SectorsPerAllocationUnit == 8, "allocation unit unchanged");
    check(get_full_size_info_ex(1, fd, &extended) == STATUS_SUCCESS &&
          extended.ActualTotalAllocationUnits == (uint64_t)info.TotalAllocationUnits.QuadPart &&
          extended.CallerAvailableAllocationUnits == (uint64_t)info.CallerAvailableAllocationUnits.QuadPart,
          "extended size information uses broker result");
    check(source_broker_logs == 1 && source_fallback_logs == 1, "each disk-space source logged once");
    __atomic_store_n(&negative_available, 1, __ATOMIC_SEQ_CST);
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS &&
          !info.CallerAvailableAllocationUnits.QuadPart,
          "negative statfs availability gives Windows zero caller availability");
    check(info.ActualAvailableAllocationUnits.QuadPart > 0,
          "negative availability preserves Windows actual availability");
    check(get_full_size_info_ex(1, fd, &extended) == STATUS_SUCCESS &&
          !extended.CallerAvailableAllocationUnits,
          "negative statfs availability gives extended Windows zero caller availability");
    __atomic_store_n(&negative_available, 0, __ATOMIC_SEQ_CST);
    attempts = connected;
    handle_path = "/app0/outside";
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS && connected == attempts &&
          info.TotalAllocationUnits.QuadPart == (INT64_C(64) << 30) / 4096,
          "outside title root uses existing shim without connecting");
    handle_path = WB_USER_ROOT "space";
    title_enabled = 0;
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS && connected == attempts,
          "payload mode does not use broker");
    close_connections();
    close(fd);
}

static void pool_index_tests(void)
{
    static const struct { unsigned int id, slot; } cases[] = {
        {60, 15}, {64, 0}, {68, 1}, {124, 15}, {128, 0}, {132, 1},
        {UINT32_MAX - 3, 15},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        check(broker_windows_index(cases[i].id) == cases[i].slot,
              "aligned Wine thread ID selects expected slot at wrap boundaries");
    check(broker_windows_index(64) == broker_windows_index(128) &&
          broker_windows_index(60) == broker_windows_index(124),
          "Wine thread IDs one pool apart collide deterministically");
}

enum { POOL_WORKERS = 32, POOL_ROUNDS = 4, POOL_STRIDE = 128 };

struct pool_worker
{
    unsigned int number;
    unsigned char bytes[POOL_STRIDE];
    size_t length;
    ssize_t results[POOL_ROUNDS];
    int query_ok, slot;
};

static void *pool_worker(void *arg)
{
    struct pool_worker *worker = arg;
    struct wb_statfs_reply reply;

    current_thread_id = 4 * (16 + worker->number);
    /* Race the once initializer from early space queries and early writes. */
    worker->query_ok = 1;
    if (!(worker->number & 1))
        worker->query_ok = broker_query_fs(1, &reply) && observed_slot == (int)(worker->number % 16);
    for (unsigned int i = 0; i < POOL_ROUNDS; i++)
        worker->results[i] = broker_pwrite(2, worker->bytes, worker->length,
                                           worker->number * POOL_STRIDE, worker->number & 1, NULL);
    worker->slot = observed_slot;
    return NULL;
}

static int wait_for_pool(int contenders)
{
    struct timespec deadline;
    int result = 0;

    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += 10;
    while (contenders ? lock_attempt_mask != 0xffffu : waiting_requests != 16)
    {
        result = pthread_cond_timedwait(&request_cond, &request_lock, &deadline);
        if (result == ETIMEDOUT) return 0;
        assert(!result);
    }
    return 1;
}

#ifdef WINE_PS5_RUNESCAPE
static void pool_prepare_distinct_files(int first_fd, const unsigned char *seed, size_t size)
{
    pthread_mutex_t *locks[16];
    unsigned selected[16] = {UINT32_MAX}, probes = 0;
    struct stat status;
    pool_fds[0] = first_fd;
    snprintf(pool_host_paths[0], sizeof(pool_host_paths[0]), "%s", writes_path);
    assert(!fstat(first_fd, &status));
    locks[0] = wb_lock_inode(status.st_dev, status.st_ino);
    assert(locks[0] && !pthread_mutex_unlock(locks[0]));
    for (unsigned slot = 0; slot < 16; slot++) {
        snprintf(pool_wire_paths[slot], sizeof(pool_wire_paths[slot]), WB_USER_ROOT "pool-%u", slot);
        if (!slot) continue;
        for (;;) {
            unsigned probe = probes++;int distinct = 1;
            assert(probes <= 4096);
            snprintf(pool_host_paths[slot], sizeof(pool_host_paths[slot]), "%s/pool-probe-%u", directory, probe);
            int fd = open(pool_host_paths[slot], O_CREAT | O_EXCL | O_RDWR, 0600);
            assert(fd >= 0 && !fstat(fd, &status));
            pthread_mutex_t *lock = wb_lock_inode(status.st_dev, status.st_ino);
            assert(lock && !pthread_mutex_unlock(lock));
            for (unsigned i = 0; i < slot; i++) if (lock == locks[i]) distinct = 0;
            if (distinct) {
                pool_fds[slot] = fd;locks[slot] = lock;selected[slot] = probe;
                assert(write(fd, seed, size) == (ssize_t)size);break;
            }
            /* Keep the directory entry until selection ends: immediate unlink
             * can recycle the same colliding inode for the next candidate. */
            close(fd);
        }
    }
    for (unsigned probe = 0; probe < probes; probe++) {
        unsigned slot;
        for (slot = 1; slot < 16 && selected[slot] != probe; slot++);
        if (slot == 16) {
            char unused[1100];snprintf(unused, sizeof(unused), "%s/pool-probe-%u", directory, probe);
            assert(!unlink(unused));
        }
    }
    pool_distinct_files = 1;
}
#endif
static ssize_t pool_read_image(int fd, unsigned char *bytes, size_t size)
{
#ifdef WINE_PS5_RUNESCAPE
    (void)fd;
    assert(size == POOL_WORKERS * POOL_STRIDE);
    for (unsigned i = 0; i < POOL_WORKERS; i++)
        if (pread(pool_fds[i % 16], bytes + i * POOL_STRIDE, POOL_STRIDE, i * POOL_STRIDE) != POOL_STRIDE)
            return -1;
    return size;
#else
    return pread(fd, bytes, size, 0);
#endif
}

static void pool_tests(void)
{
    _Static_assert(BROKER_CONNECTIONS == 16, "broker pool must provide sixteen slots");
    struct pool_worker workers[POOL_WORKERS] = {0};
    pthread_t clients[POOL_WORKERS];
    unsigned char expected[POOL_WORKERS * POOL_STRIDE], actual[sizeof(expected)];
    unsigned long long expected_bytes = 0;
    struct stat info;
    int fd = open(writes_path, O_CREAT | O_EXCL | O_RDWR, 0600), all_waiting;

    assert(fd >= 0);
    memset(expected, 0xcc, sizeof(expected));
    assert(write(fd, expected, sizeof(expected)) == sizeof(expected));
#ifdef WINE_PS5_RUNESCAPE
    /* The pool's concurrency contract needs independent inode stripes now;
     * same-inode ordering is exercised separately by the deterministic race. */
    pool_prepare_distinct_files(fd, expected, sizeof(expected));
#endif
    broker_mode = 1;
    hold_requests = 1;
    for (unsigned int i = 0; i < POOL_WORKERS; i++)
    {
        workers[i].number = i;
        workers[i].length = 32 + i;
        for (size_t j = 0; j < workers[i].length; j++) workers[i].bytes[j] = i * 7 + j;
        memcpy(expected + i * POOL_STRIDE, workers[i].bytes, workers[i].length);
        expected_bytes += workers[i].length * POOL_ROUNDS;
    }
    /* Consecutive aligned Wine IDs cover every slot; the second wave wraps
     * to those same slots and must wait for the locked request streams. */
    for (unsigned int i = 0; i < 16; i++)
        assert(!pthread_create(&clients[i], NULL, pool_worker, &workers[i]));
    assert(!pthread_mutex_lock(&request_lock));
    all_waiting = wait_for_pool(0);
    check(all_waiting && peak_requests == 16, "sixteen broker writes concurrently reach real pwrite");
    if (all_waiting)
    {
        for (unsigned int i = 0; i < BROKER_CONNECTIONS; i++)
        {
            int result = pthread_mutex_trylock(&broker[i].lock);
            check(result == EBUSY, "each distinct broker slot stays locked until reply");
            if (!result) assert(!pthread_mutex_unlock(&broker[i].lock));
            check(broker[i].fd >= 0, "each of sixteen slots owns a live connection");
        }
    }
    track_lock_attempts = 1;
    assert(!pthread_mutex_unlock(&request_lock));
    for (unsigned int i = 16; i < POOL_WORKERS; i++)
        assert(!pthread_create(&clients[i], NULL, pool_worker, &workers[i]));
    assert(!pthread_mutex_lock(&request_lock));
    if (all_waiting) check(wait_for_pool(1), "second wave attempts every occupied slot");
    track_lock_attempts = hold_requests = 0;
    assert(!pthread_cond_broadcast(&request_cond));
    assert(!pthread_mutex_unlock(&request_lock));
    for (unsigned int i = 0; i < POOL_WORKERS; i++)
    {
        assert(!pthread_join(clients[i], NULL));
        check(workers[i].query_ok, "concurrent space query completes on shared pool");
        check(workers[i].slot == (int)(i % 16), "Windows thread IDs select all sixteen slots");
        for (unsigned int j = 0; j < POOL_ROUNDS; j++)
            check(workers[i].results[j] == (ssize_t)workers[i].length,
                  "concurrent write receives its exact synchronous reply");
    }
    check(connected == 16, "busy client opens exactly sixteen pooled sockets");
    check(served_writes == POOL_WORKERS * POOL_ROUNDS && served_bytes == expected_bytes,
          "one broker write request per synchronous write, no coalescing");
    check(!fstat(fd, &info) && info.st_size == sizeof(expected) &&
          pool_read_image(fd, actual, sizeof(actual)) == sizeof(actual) &&
          !memcmp(actual, expected, sizeof(actual)),
          "all completed writes are immediately visible with exact size/bytes and untouched gaps");
#ifdef WINE_PS5_RUNESCAPE
    for (unsigned slot = 0; slot < 16; slot++) {
        unsigned char file_expected[sizeof(expected)], file_actual[sizeof(expected)];
        memset(file_expected, 0xcc, sizeof(file_expected));
        for (unsigned i = slot; i < POOL_WORKERS; i += 16)
            memcpy(file_expected + i * POOL_STRIDE, workers[i].bytes, workers[i].length);
        check(!fstat(pool_fds[slot], &info) && info.st_size == sizeof(file_expected) &&
              pread(pool_fds[slot], file_actual, sizeof(file_actual), 0) == sizeof(file_actual) &&
              !memcmp(file_actual, file_expected, sizeof(file_actual)),
              "each independent pool inode retains exact bytes/EOF and untouched regions");
    }
#endif
    {
        unsigned int native_slot = (uintptr_t)pthread_self() % 16;
        current_thread_id = 4 * (16 + (native_slot + 1) % 16);
        check(broker_pwrite(2, workers[0].bytes, workers[0].length, 0, 1,
                            WB_USER_ROOT "writes") == (ssize_t)workers[0].length &&
              observed_slot == (int)native_slot,
              "known-path writes use native thread ID with the sixteen-slot pool");
    }
    close(fd);
    close_connections();
    /* Closing a stream does not reinitialize its mutex or leave fd zero. */
    current_thread_id = 4 * 31;
    check(broker_pwrite(2, workers[31].bytes, workers[31].length, 31 * POOL_STRIDE, 1, NULL) ==
          (ssize_t)workers[31].length && observed_slot == 15 && connected == 17,
          "last slot reconnects safely after connection cleanup");
    close_connections();
    current_thread_id = 4 * 16;
    broker_mode = connected = 0;
#ifdef WINE_PS5_RUNESCAPE
    pool_distinct_files = 0;
    for (unsigned slot = 1; slot < 16; slot++) {
        assert(!close(pool_fds[slot]));assert(!unlink(pool_host_paths[slot]));
    }
#endif
    unlink(writes_path);
}

enum { OSRS_ROUNDS = 32, OSRS_OFFSET = 6302400, OSRS_TAIL = 2600000, OSRS_IDX_BYTES = 20000 };

static uint64_t host_now_ns(void)
{
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int timing_compare(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static void osrs_timing(const char *label, const uint64_t values[OSRS_ROUNDS])
{
    uint64_t sorted[OSRS_ROUNDS], total = 0;
    memcpy(sorted, values, sizeof(sorted));qsort(sorted, OSRS_ROUNDS, sizeof(sorted[0]), timing_compare);
    for (unsigned i = 0; i < OSRS_ROUNDS; i++) total += values[i];
    printf("OSRS_HOST_WRITE_TIME route=%s rounds=%u mean_us=%.1f p95_us=%.1f max_us=%.1f "
           "transport=socketpair cached_no_fsync=1 console_estimate=unavailable\n",
           label, OSRS_ROUNDS, total / (double)OSRS_ROUNDS / 1000.0,
           sorted[OSRS_ROUNDS * 95 / 100] / 1000.0, sorted[OSRS_ROUNDS - 1] / 1000.0);
}

/* Reproduce the observed request shape without silently replacing full-tail
 * writes with EOF growth. Interior mutations prove suffix-only dedup unsafe.
 * These are host timing observations, never console throughput assertions. */
static void osrs_tail_tests(void)
{
    const size_t capacity = OSRS_OFFSET + OSRS_TAIL + OSRS_ROUNDS * 1560;
    unsigned char *expected = malloc(capacity), *actual = malloc(capacity);
    unsigned char index[OSRS_IDX_BYTES], index_read[OSRS_IDX_BYTES];
    char full_path[1024], delta_path[1024], full_idx_path[1024], delta_idx_path[1024];
    int data_fd, index_fd, full_fd, delta_fd, full_idx_fd, delta_idx_fd;
    uint64_t data_ns[OSRS_ROUNDS], index_ns[OSRS_ROUNDS], full_ns[OSRS_ROUNDS], delta_ns[OSRS_ROUNDS];
    unsigned long long before_writes = served_writes, before_bytes = served_bytes, whole_bytes = 0, growth_bytes = 0;
    struct stat status;
    struct wine_ps5_title_write_state state = WINE_PS5_TITLE_WRITE_STATE_INIT;
    struct wine_ps5_title_write_decision decision;
    struct wine_ps5_sticky_writes sticky = {0};
    size_t tail = OSRS_TAIL;

    assert(expected && actual);
    assert(wb_path_root(OSRS_DATA) && wb_path_root(OSRS_INDEX));
    assert(!wine_ps5_title_buffer_path(OSRS_DATA, WB_USER_ROOT "prefix/drive_c", WB_USER_ROOT));
    assert(!wine_ps5_title_buffer_path(OSRS_INDEX, WB_USER_ROOT "prefix/drive_c", WB_USER_ROOT));
    decision = wine_ps5_title_write_route(state, OSRS_TAIL, WINE_PS5_WRITE_LOCAL_NONE, 0, 0, 1, 1, 0);
    check(decision.route == WINE_PS5_WRITE_BROKER, "OSRS full-tail writes prefer broker before throttling");
    decision = wine_ps5_title_write_route(state, OSRS_IDX_BYTES, WINE_PS5_WRITE_LOCAL_NONE, 0,
                                         WINE_PS5_WRITE_LOCAL_LIMIT, 1, 1, 0);
    check(decision.route == WINE_PS5_WRITE_BROKER, "OSRS index writes route through broker after volume takeover");
    assert(wine_ps5_sticky_write_promote(&sticky, 4, OSRS_INDEX) == 1);
    decision = wine_ps5_title_write_route(state, OSRS_IDX_BYTES, WINE_PS5_WRITE_LOCAL_NONE, 0, 0, 1, 1,
                                         wine_ps5_sticky_write_contains(&sticky, 4) ? WINE_PS5_WRITE_STICKY : 0);
    check(decision.route == WINE_PS5_WRITE_BROKER, "OSRS sticky RW index handle is broker eligible");
    assert(wine_ps5_sticky_write_forget(&sticky, 4));

    snprintf(osrs_data_path, sizeof(osrs_data_path), "%s/osrs.dat2", directory);
    snprintf(osrs_index_path, sizeof(osrs_index_path), "%s/osrs.idx0", directory);
    snprintf(full_path, sizeof(full_path), "%s/full-local.dat2", directory);
    snprintf(delta_path, sizeof(delta_path), "%s/delta-local.dat2", directory);
    snprintf(full_idx_path, sizeof(full_idx_path), "%s/full-local.idx0", directory);
    snprintf(delta_idx_path, sizeof(delta_idx_path), "%s/delta-local.idx0", directory);
    data_fd = open(osrs_data_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    full_fd = open(full_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    delta_fd = open(delta_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    index_fd = open(osrs_index_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    full_idx_fd = open(full_idx_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    delta_idx_fd = open(delta_idx_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(data_fd >= 0 && full_fd >= 0 && delta_fd >= 0 && index_fd >= 0 && full_idx_fd >= 0 && delta_idx_fd >= 0);
    memset(expected, 0xa7, capacity);memset(index, 0, sizeof(index));
    assert(pwrite(data_fd, expected, OSRS_OFFSET + tail, 0) == (ssize_t)(OSRS_OFFSET + tail));
    assert(pwrite(full_fd, expected, OSRS_OFFSET + tail, 0) == (ssize_t)(OSRS_OFFSET + tail));
    assert(pwrite(delta_fd, expected, OSRS_OFFSET + tail, 0) == (ssize_t)(OSRS_OFFSET + tail));
    broker_mode = 1;current_thread_id = 64;
    for (unsigned round = 0; round < OSRS_ROUNDS; round++)
    {
        size_t previous = tail, growth = 520 * (1 + round % 3), patch = round * 4096;
        uint64_t started;
        memset(expected + OSRS_OFFSET + tail, (unsigned char)(round + 1), growth);
        if (!(round % 4)) memset(expected + OSRS_OFFSET + patch, (unsigned char)(round + 77), 512);
        index[round] = (unsigned char)(round + 1);tail += growth;
        started = host_now_ns();
        check(broker_pwrite(3, expected + OSRS_OFFSET, tail, OSRS_OFFSET, 0, NULL) == (ssize_t)tail,
              "OSRS game RW dat2 full-tail request receives exact synchronous broker acknowledgement");
        data_ns[round] = host_now_ns() - started;
        check(!fstat(data_fd, &status) && status.st_size == (off_t)(OSRS_OFFSET + tail) &&
              pread(data_fd, actual, growth, OSRS_OFFSET + previous) == (ssize_t)growth &&
              !memcmp(actual, expected + OSRS_OFFSET + previous, growth),
              "OSRS broker EOF grows only requested suffix and reply makes appended bytes immediately visible");
        check(lseek(data_fd, 0, SEEK_CUR) == 0, "broker pwrite does not change the caller fd position");
        started = host_now_ns();
        check(broker_pwrite(4, index, sizeof(index), 0, 0, NULL) == sizeof(index),
              "OSRS idx0 offset-zero rewrite is eligible and acknowledged exactly");
        index_ns[round] = host_now_ns() - started;
        started = host_now_ns();
        assert(pwrite(full_fd, expected + OSRS_OFFSET, tail, OSRS_OFFSET) == (ssize_t)tail);
        assert(pwrite(full_idx_fd, index, sizeof(index), 0) == sizeof(index));
        full_ns[round] = host_now_ns() - started;
        started = host_now_ns();
        if (!(round % 4)) assert(pwrite(delta_fd, expected + OSRS_OFFSET + patch, 512, OSRS_OFFSET + patch) == 512);
        assert(pwrite(delta_fd, expected + OSRS_OFFSET + previous, growth, OSRS_OFFSET + previous) == (ssize_t)growth);
        assert(pwrite(delta_idx_fd, index, sizeof(index), 0) == sizeof(index));
        delta_ns[round] = host_now_ns() - started;
        whole_bytes += tail + sizeof(index);growth_bytes += growth;
    }
    close_connections();
    check(served_writes - before_writes == OSRS_ROUNDS * 2 && served_bytes - before_bytes == whole_bytes,
          "broker transmits the application's whole-tail/index byte count without inflation or lossy deduplication");
    int data_files[] = {data_fd, full_fd, delta_fd}, index_files[] = {index_fd, full_idx_fd, delta_idx_fd};
    for (unsigned i = 0; i < 3; i++)
    {
        check(pread(data_files[i], actual, OSRS_OFFSET + tail, 0) == (ssize_t)(OSRS_OFFSET + tail) &&
              !memcmp(actual, expected, OSRS_OFFSET + tail),
              "OSRS full-tail broker and local controls preserve prefix, overlaps and appended bytes exactly");
        check(pread(index_files[i], index_read, sizeof(index_read), 0) == sizeof(index_read) &&
              !memcmp(index_read, index, sizeof(index)),
              "OSRS whole-index bytes agree across all routes");
        close(data_files[i]);close(index_files[i]);
    }
    printf("OSRS_HOST_PATTERN offset=%u initial_tail=%u rounds=%u growth_bytes=%llu requested_bytes=%llu "
           "internal_overlap_bytes=%u port=%u\n", OSRS_OFFSET, OSRS_TAIL, OSRS_ROUNDS,
           growth_bytes, whole_bytes, (OSRS_ROUNDS / 4) * 512, (unsigned)WB_PORT);
    osrs_timing("broker-dat2-full-tail", data_ns);osrs_timing("broker-idx0-full-file", index_ns);
    osrs_timing("local-full-tail-and-index", full_ns);osrs_timing("local-known-delta-control-and-index", delta_ns);
    check(active_broker_logs == 1, "first fully acknowledged broker write is reported once, not just a connection");
    free(actual);free(expected);broker_mode = connected = 0;
}

/* Actual serve_write/request/ack path: no imitation cache implementation. */
static void osrs_diff_contracts(void)
{
    enum { BASE = 193, INITIAL = WB_CHUNK + 8192, GROWTH = 520 };
    const size_t capacity = BASE + INITIAL + GROWTH;
    unsigned char *expected = malloc(capacity), *actual = malloc(capacity);
    struct stat status;
    uint64_t before, written;
    int fd = open(osrs_data_path, O_TRUNC | O_RDWR);
    assert(fd >= 0 && expected && actual);
    memset(expected, 0xa7, capacity);
    assert(pwrite(fd, expected, BASE + INITIAL, 0) == BASE + INITIAL);
    assert(lseek(fd, 137, SEEK_SET) == 137);
    broker_mode = 1;current_thread_id = 64;
    before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    check(broker_pwrite(3, expected + BASE, INITIAL, BASE, 0, NULL) == INITIAL,
          "identical OSRS write acknowledges full logical length");
    written = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - before;
#ifdef WINE_PS5_RUNESCAPE
    check(written == 1, "identical OSRS request uses exactly one counted native metadata anchor byte");
#else
    check(written == INITIAL, "non-Rune broker preserves ordinary identical full write");
#endif
    check(!fstat(fd, &status) && status.st_size == BASE + INITIAL &&
          pread(fd, actual, BASE + INITIAL, 0) == BASE + INITIAL &&
          !memcmp(actual, expected, BASE + INITIAL), "identical ACK preserves exact data and EOF");
    check(lseek(fd, 0, SEEK_CUR) == 137, "identical broker write preserves caller position");
    expected[BASE + 7] ^= 0xff;
    memset(expected + BASE + 65533, 0x3c, 9);
    memset(expected + BASE + WB_CHUNK - 5, 0x4d, 11);
    memset(expected + BASE + INITIAL, 0, GROWTH);
    before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    check(broker_pwrite(3, expected + BASE, INITIAL + GROWTH, BASE, 0, NULL) == INITIAL + GROWTH,
          "dirty interior and zero EOF extension acknowledge full logical count");
    written = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - before;
#ifdef WINE_PS5_RUNESCAPE
    check(written == 1 + 9 + 11 + GROWTH,
          "OSRS writes exact dirty spans across compare/receive boundaries plus every EOF byte");
#else
    check(written == INITIAL + GROWTH, "non-Rune broker preserves ordinary growing full write");
#endif
    check(!fstat(fd, &status) && status.st_size == (off_t)capacity &&
          pread(fd, actual, capacity, 0) == (ssize_t)capacity && !memcmp(actual, expected, capacity),
          "dirty ACK exposes exact prefix/interior/zero extension and EOF immediately");
    check(lseek(fd, 0, SEEK_CUR) == 137, "dirty broker write preserves caller position");
    check(broker_pwrite(3, expected + BASE, 73, BASE, 0, NULL) == 73 &&
          !fstat(fd, &status) && status.st_size == (off_t)capacity,
          "smaller in-place broker request never truncates an existing larger file");
    close_connections();broker_mode = connected = 0;
    close(fd);free(actual);free(expected);
}

static void reset_write_faults(void)
{
    fault_write_eintr = fault_write_error = fault_write_zero = 0;
    fault_read_eintr = fault_read_error = fault_seek_unknown = fault_deny_read = 0;
    fault_write_limit = fault_read_limit = 0;fault_write_after = -1;
    fault_seek_errno = EOPNOTSUPP;
    fault_pin_dup = fault_zero_inode = 0;
}

static struct wb_reply raw_write_path(int client, const char *path, const void *bytes,
                                     uint32_t length, uint64_t offset)
{
    struct wb_write request = {.magic = WB_MAGIC, .path_length = strlen(path),
                               .length = length, .offset = offset};
    struct wb_reply reply = {0};
    assert(!transfer(client, &request, sizeof(request), 1));
    assert(!transfer(client, (void *)path, request.path_length, 1));
    assert(!transfer(client, (void *)bytes, length, 1));
    assert(!transfer(client, &reply, sizeof(reply), 0));
    return reply;
}
static struct wb_reply raw_write_request(int client, const void *bytes, uint32_t length, uint64_t offset)
{
    struct wb_reply reply = raw_write_path(client, OSRS_DATA, bytes, length, offset);
    check(reply.magic == WB_MAGIC, "write reply preserves protocol magic");
    return reply;
}

#ifdef WINE_PS5_RUNESCAPE
static struct wb_write_metrics write_metrics_snapshot(void)
{
    struct wb_write_metrics snapshot;
    assert(!pthread_mutex_lock(&stats_lock));snapshot = write_metrics_total;
    assert(!pthread_mutex_unlock(&stats_lock));return snapshot;
}
#endif

#ifdef WINE_PS5_RUNESCAPE
static void check_extent_probe_log(uint64_t expected_size, uint64_t expected_at)
{
    char line[sizeof(extent_log_line)];
    unsigned count;
    int stat_result, stat_error, data_whence, data_error, hole_whence, hole_error, hole_initial;
    unsigned long long dev, ino, at;
    long long size, blocks, blksize, data, hole;
    assert(!pthread_mutex_lock(&extent_log_mutex));
    count = extent_log_count;memcpy(line, extent_log_line, sizeof(line));
    assert(!pthread_mutex_unlock(&extent_log_mutex));
    int fields = sscanf(line,
        "extent-query st_result=%d st_errno=%d dev=%llu ino=%llu size=%lld blocks=%lld blksize=%lld "
        "at=%llu data_whence=%d data=%lld data_errno=%d hole_whence=%d hole=%lld hole_errno=%d hole_initial=%d",
        &stat_result, &stat_error, &dev, &ino, &size, &blocks, &blksize, &at,
        &data_whence, &data, &data_error, &hole_whence, &hole, &hole_error, &hole_initial);
    check(count == 1 && fields == 15, "raw extent probe logs complete stat and both seek outcomes exactly once");
    if (fields == 15)
        check(!stat_result && !stat_error && ino && size == (long long)expected_size &&
              blocks >= 0 && blksize > 0 && at == expected_at &&
              data_whence == SEEK_DATA && data == -1 && data_error == EINVAL &&
              hole_whence == SEEK_HOLE && hole == -1 && hole_error == EINVAL && !hole_initial,
              "PS5-shaped raw probe retains independent EINVAL returns and real size/blocks/blksize values");
}

static void osrs_ps5_unknown_extent_rpc_contract(void)
{
    enum { BASE = 193, INITIAL = WB_CHUNK + 8192, GROWTH = 520 };
    const size_t capacity = BASE + INITIAL + GROWTH;
    unsigned char *expected = malloc(capacity), *actual = malloc(capacity);
    char saved_data[1100];
    struct stat status;
    struct wb_write_metrics before, after;
    uint64_t physical_before, physical;
    int fd;
    assert(expected && actual);
    close_connections();reset_write_faults();
    /* A distinct inode prevents another contract's prior RPCs from serving as
     * this request's warmup. Real stat data is retained, never a dense-file
     * inequality or a fabricated allocation certificate. */
    snprintf(saved_data, sizeof(saved_data), "%s.ps5-saved", osrs_data_path);
    assert(!rename(osrs_data_path, saved_data));
    fd = open(osrs_data_path, O_CREAT | O_EXCL | O_RDWR, 0600);assert(fd >= 0);
    memset(expected, 0xa7, capacity);
    assert(pwrite(fd, expected, BASE + INITIAL, 0) == BASE + INITIAL);
    assert(lseek(fd, 137, SEEK_SET) == 137);
    memset(expected + BASE, 0x59, INITIAL);
    fault_seek_unknown = 1;fault_seek_errno = EINVAL;
    broker_mode = 1;current_thread_id = 64;
    physical_before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    check(broker_pwrite(3, expected + BASE, INITIAL, BASE, 0, NULL) == INITIAL,
          "PS5-shaped unknown-seek warmup receives full logical ACK");
    check(__atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - physical_before == INITIAL,
          "first unknown-allocation RPC performs a real full native write before any later optimization");
    check(!fstat(fd, &status) && status.st_size == BASE + INITIAL &&
          pread(fd, actual, BASE + INITIAL, 0) == BASE + INITIAL &&
          !memcmp(actual, expected, BASE + INITIAL) && lseek(fd, 0, SEEK_CUR) == 137,
          "unknown-seek warmup exposes exact file bytes/EOF and leaves caller position untouched");

    before = write_metrics_snapshot();
    physical_before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    check(broker_pwrite(3, expected + BASE, INITIAL, BASE, 0, NULL) == INITIAL,
          "repeated PS5-shaped unknown-seek RPC still acknowledges its entire payload");
    after = write_metrics_snapshot();
    physical = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - physical_before;
    check(physical == 1 && after.pread_bytes - before.pread_bytes >= INITIAL &&
          after.eligible_bytes - before.eligible_bytes >= INITIAL &&
          after.skipped_bytes - before.skipped_bytes >= INITIAL,
          "previous real RPC write permits fresh comparison/skipping despite PS5 SEEK_DATA/HOLE EINVAL");
    check(after.proof_bytes - before.proof_bytes >= INITIAL,
          "unknown-seek optimization explicitly accounts comparison authorized by session allocation proof");
    check(!fstat(fd, &status) && status.st_size == BASE + INITIAL &&
          pread(fd, actual, BASE + INITIAL, 0) == BASE + INITIAL &&
          !memcmp(actual, expected, BASE + INITIAL) && lseek(fd, 0, SEEK_CUR) == 137,
          "optimized identical unknown-seek ACK exposes exact data/EOF/position");

    expected[BASE + 7] ^= 0xff;
    memset(expected + BASE + 65533, 0x3c, 9);
    memset(expected + BASE + WB_CHUNK - 5, 0x4d, 11);
    memset(expected + BASE + INITIAL, 0, GROWTH);
    before = write_metrics_snapshot();
    physical_before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    check(broker_pwrite(3, expected + BASE, INITIAL + GROWTH, BASE, 0, NULL) == INITIAL + GROWTH,
          "PS5-shaped unknown-seek interior edits and zero EOF growth receive full ACK");
    after = write_metrics_snapshot();
    physical = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - physical_before;
    check(physical == 1 + 9 + 11 + GROWTH && after.pread_bytes > before.pread_bytes &&
          after.eligible_bytes > before.eligible_bytes && after.skipped_bytes > before.skipped_bytes,
          "PS5-shaped later RPC writes only exact interior changes and all520zero EOF bytes");
    check(after.proof_bytes > before.proof_bytes, "later unknown-seek delta still freshly compares certified ranges");
    check(!fstat(fd, &status) && status.st_size == (off_t)capacity &&
          pread(fd, actual, capacity, 0) == (ssize_t)capacity && !memcmp(actual, expected, capacity) &&
          lseek(fd, 0, SEEK_CUR) == 137,
          "unknown-seek delta ACK exposes complete prefix/interior/zero tail and exact EOF/position");
    printf("BROKER_PS5_UNKNOWN_RPC physical_delta=%llu eligible_delta=%llu skipped_delta=%llu seek_errno=%d\n",
           (unsigned long long)physical,
           (unsigned long long)(after.eligible_bytes - before.eligible_bytes),
           (unsigned long long)(after.skipped_bytes - before.skipped_bytes), EINVAL);
    check_extent_probe_log(BASE + INITIAL, BASE);
    close_connections();broker_mode = connected = 0;reset_write_faults();
    close(fd);assert(!unlink(osrs_data_path) && !rename(saved_data, osrs_data_path));
    free(actual);free(expected);
}
#endif

#ifdef WINE_PS5_RUNESCAPE
struct proof_fixture {
    int fd, client;
    size_t length;
    unsigned char *bytes, *actual;
    char saved[1100];
};
static void proof_unknown_queries(void)
{
    fault_seek_unknown = 1;fault_seek_errno = EINVAL;
}
static void proof_fixture_begin(struct proof_fixture *fixture, size_t length)
{
    close_connections();reset_write_faults();memset(fixture, 0, sizeof(*fixture));
    fixture->length = length;
    fixture->bytes = malloc(length);fixture->actual = malloc(length);
    assert(fixture->bytes && fixture->actual);
    memset(fixture->bytes, 0x61, length);
    snprintf(fixture->saved, sizeof(fixture->saved), "%s.allocation-saved", osrs_data_path);
    assert(!rename(osrs_data_path, fixture->saved));
    fixture->fd = open(osrs_data_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fixture->fd >= 0 &&
           pwrite(fixture->fd, fixture->bytes, length, 0) == (ssize_t)length &&
           lseek(fixture->fd, 137, SEEK_SET) == 137);
    proof_unknown_queries();fixture->client = connection();
}
static void proof_fixture_end(struct proof_fixture *fixture)
{
    if (fixture->client >= 0) close(fixture->client);
    close_connections();reset_write_faults();
    close(fixture->fd);
    assert(!unlink(osrs_data_path) && !rename(fixture->saved, osrs_data_path));
    free(fixture->actual);free(fixture->bytes);
}
static void proof_rpc(int client, const char *path, const void *bytes, uint32_t length,
                      uint64_t offset, uint64_t physical, const char *label)
{
    uint64_t before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    struct wb_reply reply = raw_write_path(client, path, bytes, length, offset);
    check(reply.magic == WB_MAGIC && reply.result == (int32_t)length, label);
    check(__atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - before == physical, label);
}
static void proof_check_image(struct proof_fixture *fixture, const char *label)
{
    struct stat status;
    check(!fstat(fixture->fd, &status) && status.st_size == (off_t)fixture->length &&
          pread(fixture->fd, fixture->actual, fixture->length, 0) == (ssize_t)fixture->length &&
          !memcmp(fixture->actual, fixture->bytes, fixture->length) &&
          lseek(fixture->fd, 0, SEEK_CUR) == 137, label);
}
static unsigned proof_live_ranges(int fd, int *pin)
{
    struct stat status, pinned;
    assert(!fstat(fd, &status));
    pthread_mutex_t *lock = wb_lock_inode(status.st_dev, status.st_ino);assert(lock);
    struct wb_allocation_proof *proof = &wb_allocation_proofs[lock - wb_inode_locks];
    check(proof->present && proof->snapshot.st_dev == status.st_dev &&
          proof->snapshot.st_ino == status.st_ino && proof->count <= 8,
          "session proof belongs to opened inode and stays within eight ranges");
    check(proof->present && !fstat(proof->fd, &pinned) &&
          pinned.st_dev == status.st_dev && pinned.st_ino == status.st_ino,
          "retained duplicate descriptor really pins the certified inode");
    unsigned count = proof->count;
    if (pin) *pin = proof->fd;
    assert(!pthread_mutex_unlock(lock));return count;
}
static void proof_observable_time(int fd, unsigned serial)
{
    struct timespec changed[2] = {{700000000, 123456789}, {700000000 + serial, 987654321}};
    assert(!futimens(fd, changed));
}
static void osrs_proof_metadata_contracts(void)
{
    struct proof_fixture fixture;
    struct wb_write_metrics before, after;
    char saved_index[1100], replaced[1100];
    proof_fixture_begin(&fixture, 8192);
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "cold unknown-extent request really writes before learning");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, 1,
              "live session reuses allocation proof but freshly compares content");
    check(proof_live_ranges(fixture.fd, NULL) == 1, "successful whole request learns one allocation interval");

    before = write_metrics_snapshot();proof_observable_time(fixture.fd, 1);
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "observable external timestamp mutation discards all old allocation ranges");
    after = write_metrics_snapshot();
    check(after.proof_invalidations > before.proof_invalidations &&
          after.proof_bytes == before.proof_bytes, "timestamp mismatch cannot consume stale proof");

    before = write_metrics_snapshot();assert(!fchmod(fixture.fd, 0640));
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "observable file mode change invalidates the certificate");
    after = write_metrics_snapshot();
    check(after.proof_invalidations > before.proof_invalidations, "mode mismatch is accounted as invalidation");

    snprintf(saved_index, sizeof(saved_index), "%s.proof-saved", osrs_index_path);
    assert(!rename(osrs_index_path, saved_index) && !link(osrs_data_path, osrs_index_path));
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "hard-link count change invalidates before relearning actual writes");
    proof_rpc(fixture.client, OSRS_INDEX, fixture.bytes, fixture.length, 0, 1,
              "different-path alias reuses the same inode's current allocation proof");
    int alias = open(osrs_index_path, O_RDWR);assert(alias >= 0);
    unsigned char byte = 0x42;assert(pwrite(alias, &byte, 1, 123) == 1);
    proof_observable_time(alias, 2);before = write_metrics_snapshot();
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "observable native/FTP-shaped alias overwrite forces an ordinary-write revalidation");
    after = write_metrics_snapshot();
    check(after.proof_invalidations > before.proof_invalidations, "external alias mutation invalidates ranges");
    unsigned char *view = mmap(NULL, fixture.length, PROT_READ | PROT_WRITE, MAP_SHARED, alias, 0);
    assert(view != MAP_FAILED);view[33] = 0x43;proof_observable_time(alias, 3);
    before = write_metrics_snapshot();
    proof_rpc(fixture.client, OSRS_INDEX, fixture.bytes, fixture.length, 0, fixture.length,
              "observable mapped mutation invalidates before writing through the alias");
    after = write_metrics_snapshot();
    check(after.proof_invalidations > before.proof_invalidations && view[33] == fixture.bytes[33],
          "mapped view immediately sees repaired backing data after invalidation");
    assert(!munmap(view, fixture.length));close(alias);
    assert(!unlink(osrs_index_path) && !rename(saved_index, osrs_index_path));
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "removing alias invalidates changed link-count metadata");

    assert(!ftruncate(fixture.fd, 0) && !ftruncate(fixture.fd, fixture.length));
    memset(fixture.bytes, 0, fixture.length);proof_observable_time(fixture.fd, 4);
    before = write_metrics_snapshot();
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "observable truncate/regrow cannot reuse allocation proof over zero-valued holes");
    after = write_metrics_snapshot();
    check(after.proof_invalidations > before.proof_invalidations &&
          after.proof_bytes == before.proof_bytes, "truncate/regrow clears proof rather than skipping matching holes");
    proof_check_image(&fixture, "truncate/regrow recovery preserves exact data/EOF/caller position");

    snprintf(replaced, sizeof(replaced), "%s.proof-replaced", osrs_data_path);
    assert(!rename(osrs_data_path, replaced));
    int replacement = open(osrs_data_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(replacement >= 0 && pwrite(replacement, fixture.bytes, fixture.length, 0) == (ssize_t)fixture.length);
    before = write_metrics_snapshot();
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "same pathname with a different inode cannot inherit old allocation authority");
    after = write_metrics_snapshot();
    check(after.proof_bytes == before.proof_bytes, "replacement inode starts without proof even for equal bytes");
    assert(pread(replacement, fixture.actual, fixture.length, 0) == (ssize_t)fixture.length);
    check(!memcmp(fixture.actual, fixture.bytes, fixture.length), "replacement gets complete real backing data");
    close(replacement);assert(!unlink(replaced));proof_fixture_end(&fixture);
}
static void osrs_proof_eviction_contract(void)
{
    struct proof_fixture fixture;
    struct stat identity, candidate_status;
    struct wb_write_metrics before, after;
    char saved_index[1100], candidate[1100];
    unsigned probe;
    int other = -1;
    proof_fixture_begin(&fixture, 8192);
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length, "eviction target warmup");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, 1, "eviction target initially reusable");
    assert(!fstat(fixture.fd, &identity));
    pthread_mutex_t *target = wb_lock_inode(identity.st_dev, identity.st_ino);
    assert(target && !pthread_mutex_unlock(target));
    for (probe = 0; probe < 4096; probe++) {
        snprintf(candidate, sizeof(candidate), "%s/proof-collision-%u", directory, probe);
        int fd = open(candidate, O_CREAT | O_EXCL | O_RDWR, 0600);assert(fd >= 0);
        assert(!fstat(fd, &candidate_status));
        pthread_mutex_t *lock = wb_lock_inode(candidate_status.st_dev, candidate_status.st_ino);
        assert(lock && !pthread_mutex_unlock(lock));
        if (lock == target) { other = fd;break; }
        close(fd); /* Keep inode allocated until all candidate names are removed. */
    }
    assert(other >= 0);
    snprintf(saved_index, sizeof(saved_index), "%s.eviction-saved", osrs_index_path);
    assert(!rename(osrs_index_path, saved_index) && !rename(candidate, osrs_index_path));
    assert(pwrite(other, fixture.bytes, fixture.length, 0) == (ssize_t)fixture.length);
    before = write_metrics_snapshot();
    proof_rpc(fixture.client, OSRS_INDEX, fixture.bytes, fixture.length, 0, fixture.length,
              "colliding distinct inode learns only after a full ordinary write");
    after = write_metrics_snapshot();
    check(after.proof_evictions > before.proof_evictions, "same-stripe replacement accounts proof eviction");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "evicted inode falls back to ordinary write instead of trusting forgotten proof");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, 1,
              "evicted inode can safely relearn and reuse a subsequent successful request");
    proof_check_image(&fixture, "proof eviction never drops or defers actual data");
    close(other);assert(!unlink(osrs_index_path) && !rename(saved_index, osrs_index_path));
    for (unsigned i = 0; i < probe; i++) {
        snprintf(candidate, sizeof(candidate), "%s/proof-collision-%u", directory, i);assert(!unlink(candidate));
    }
    proof_fixture_end(&fixture);
}
static void osrs_proof_range_bounds(void)
{
    enum { WINDOW = 8192, CAPACITY = 18 * WINDOW };
    struct proof_fixture fixture;
    struct wb_write_metrics before, after;
    _Static_assert(sizeof(wb_allocation_proofs) / sizeof(wb_allocation_proofs[0]) == 64,
                   "allocation proof table has a fixed sixty-four inode budget");
    _Static_assert(sizeof(wb_allocation_proofs[0].ranges) / sizeof(wb_allocation_proofs[0].ranges[0]) == 8,
                   "allocation proof entry has eight merged intervals");
    proof_fixture_begin(&fixture, CAPACITY);
    for (unsigned i = 0; i < 8; i++)
        proof_rpc(fixture.client, OSRS_DATA, fixture.bytes + i * 2 * WINDOW, WINDOW, i * 2 * WINDOW,
                  WINDOW, "disjoint allocation interval requires actual initial write");
    check(proof_live_ranges(fixture.fd, NULL) == 8, "eight disjoint ranges occupy the fixed entry budget");
    before = write_metrics_snapshot();
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes + 16 * WINDOW, WINDOW, 16 * WINDOW,
              WINDOW, "ninth disjoint range still writes data while declining additional proof");
    after = write_metrics_snapshot();
    check(after.proof_full > before.proof_full && proof_live_ranges(fixture.fd, NULL) == 8,
          "ninth range reports full without enlarging or discarding existing ranges");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes + 16 * WINDOW, WINDOW, 16 * WINDOW,
              WINDOW, "declined range cannot be skipped on the next request");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, WINDOW, 0, 1,
              "range saturation preserves older valid proof");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes + WINDOW, WINDOW, WINDOW,
              WINDOW, "writing an unproven adjacent gap merges surrounding intervals");
    check(proof_live_ranges(fixture.fd, NULL) == 7, "adjacent merge releases a range slot");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes + 16 * WINDOW, WINDOW, 16 * WINDOW,
              WINDOW, "released slot learns the formerly declined range after real write");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes + 16 * WINDOW, WINDOW, 16 * WINDOW,
              1, "newly learned range now supports exact fresh-content skipping");
    proof_check_image(&fixture, "range-bound pressure preserves every byte, gap, EOF and caller position");
    proof_fixture_end(&fixture);
}
static void osrs_proof_failed_request_contracts(void)
{
    struct proof_fixture fixture;
    proof_fixture_begin(&fixture, 2 * WB_CHUNK);
    unsigned char *desired = malloc(fixture.length);assert(desired);
    memset(desired, 0x42, fixture.length);
    fault_write_after = 1;fault_write_error = ENOSPC;
    uint64_t physical = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    struct wb_reply reply = raw_write_request(fixture.client, desired, fixture.length, 0);
    check(reply.result == -ENOSPC &&
          __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - physical == WB_CHUNK,
          "failed whole RPC exposes only its actually committed first chunk");
    check(proof_live_ranges(fixture.fd, NULL) == 0, "failed whole request never certifies its successful prefix");
    reset_write_faults();proof_unknown_queries();
    memcpy(fixture.bytes, desired, WB_CHUNK);
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, WB_CHUNK, 0, WB_CHUNK,
              "failed request prefix must be rewritten before it can be certified");
    check(proof_live_ranges(fixture.fd, NULL) == 1, "successful prefix retry learns only its actual interval");
    int interrupted = connection();
    struct wb_write request = {.magic = WB_MAGIC, .path_length = sizeof(OSRS_DATA) - 1,
                               .length = fixture.length};
    assert(!transfer(interrupted, &request, sizeof(request), 1));
    assert(!transfer(interrupted, (void *)OSRS_DATA, request.path_length, 1));
    assert(!transfer(interrupted, fixture.bytes, WB_CHUNK, 1));
    unsigned char partial[37];memset(partial, 0x18, sizeof(partial));
    assert(!transfer(interrupted, partial, sizeof(partial), 1) && !shutdown(interrupted, SHUT_WR));
    check(read(interrupted, &reply, sizeof(reply)) == 0, "partial RPC disconnect never receives successful ACK");
    close(interrupted);
    /* Join only the aborted worker: keep the original connection alive so
     * request invalidation cannot be confused with terminal-session cleanup. */
    assert(!pthread_mutex_lock(&server_lock) && thread_count == 2);
    pthread_t ended = threads[--thread_count];
    assert(!pthread_mutex_unlock(&server_lock) && !pthread_join(ended, NULL));
    check(proof_live_ranges(fixture.fd, NULL) == 0,
          "aborted request forgets all ranges even while another session connection stays alive");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, WB_CHUNK, 0, WB_CHUNK,
              "disconnected request's matching prefix cannot reuse discarded proof");
    proof_check_image(&fixture, "failed/disconnected requests retain exact committed data without partial-buffer corruption");
    free(desired);proof_fixture_end(&fixture);
}
static void osrs_proof_unavailable_contracts(void)
{
    struct proof_fixture fixture;
    struct wb_write_metrics before, after;
    proof_fixture_begin(&fixture, 8192);
    fault_pin_dup = 1;before = write_metrics_snapshot();
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "retained-fd allocation failure never prevents an ordinary write");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "failed descriptor pin cannot publish reusable allocation authority");
    after = write_metrics_snapshot();
    check(after.proof_unavailable >= before.proof_unavailable + 2 && after.proof_bytes == before.proof_bytes,
          "descriptor pin failures are observable and do not authorize skipping");
    fault_pin_dup = 0;fault_zero_inode = 1;before = write_metrics_snapshot();
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "unusable inode identity preserves ordinary writes");
    after = write_metrics_snapshot();
    check(after.proof_unavailable > before.proof_unavailable, "zero inode is an explicit proof-unavailable reason");
    fault_zero_inode = 0;
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "restored pin/identity must perform real warmup before learning");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, 1,
              "available retained descriptor permits reuse after successful warmup");
    int generic = open(writes_path, O_CREAT | O_EXCL | O_RDWR, 0600);assert(generic >= 0);
    assert(pwrite(generic, fixture.bytes, fixture.length, 0) == (ssize_t)fixture.length);
    proof_rpc(fixture.client, WB_USER_ROOT "writes", fixture.bytes, fixture.length, 0, fixture.length,
              "non-cache filename under broker root participates in ordinary warmup");
    proof_rpc(fixture.client, WB_USER_ROOT "writes", fixture.bytes, fixture.length, 0, 1,
              "allocation certificates apply across the approved broker root, not just OSRS cache names");
    close(generic);assert(!unlink(writes_path));
    proof_check_image(&fixture, "proof-unavailable fallback preserves complete data/EOF/position");
    proof_fixture_end(&fixture);
}
static void osrs_proof_terminal_session_contract(void)
{
    struct proof_fixture fixture;
    int pin;
    proof_fixture_begin(&fixture, 8192);
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length, "terminal-session warmup");
    check(proof_live_ranges(fixture.fd, &pin) == 1, "live session retains actual interval and descriptor");
    close(fixture.client);fixture.client = -1;close_connections();
    for (unsigned i = 0; i < 64; i++)
        check(!wb_allocation_proofs[i].present, "last joined worker releases every allocation-proof entry");
    errno = 0;
    check(fcntl(pin, F_GETFD) == -1 && errno == EBADF, "terminal worker cleanup closes retained duplicate fd");
    proof_check_image(&fixture, "terminal proof cleanup never drops already acknowledged backing data");
    fixture.client = connection();
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, fixture.length,
              "new connection cohort starts without previous session allocation authority");
    proof_rpc(fixture.client, OSRS_DATA, fixture.bytes, fixture.length, 0, 1,
              "new cohort can learn a fresh successful write and reuse it");
    proof_fixture_end(&fixture);
}
#endif

static void osrs_split_small_tail_contract(void)
{
    enum { BASE = 193, OLD_TAIL = (16u << 20) + 264, NEW_TAIL = (16u << 20) + 784 };
    const size_t capacity = BASE + NEW_TAIL;
    unsigned char *expected = malloc(capacity), *actual = malloc(capacity);
    struct stat status;
    int fd = open(osrs_data_path, O_TRUNC | O_RDWR);
    assert(fd >= 0 && expected && actual);
    memset(expected, 0xa7, capacity);
    memset(expected + BASE + OLD_TAIL, 0, NEW_TAIL - OLD_TAIL);
    assert(pwrite(fd, expected, BASE + OLD_TAIL, 0) == BASE + OLD_TAIL);
    assert(lseek(fd, 137, SEEK_SET) == 137);
    close_connections();reset_write_faults();broker_mode = 1;current_thread_id = 64;
    uint64_t physical_before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
#ifdef WINE_PS5_RUNESCAPE
    struct wb_write_metrics metrics_before = write_metrics_snapshot();
#endif
    check(broker_pwrite(3, expected + BASE, NEW_TAIL, BASE, 0, NULL) == NEW_TAIL,
          "16MiB-plus784 logical write receives complete ACK across protocol split");
    uint64_t physical = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - physical_before;
#ifdef WINE_PS5_RUNESCAPE
    struct wb_write_metrics metrics_after = write_metrics_snapshot();
    check(physical == 785 && metrics_after.pwrite_bytes - metrics_before.pwrite_bytes == 785 &&
          metrics_after.anchor_bytes - metrics_before.anchor_bytes == 1 &&
          metrics_after.small_bytes - metrics_before.small_bytes == 784 &&
          metrics_after.logical_bytes - metrics_before.logical_bytes == NEW_TAIL &&
          metrics_after.requests - metrics_before.requests == 2,
          "split write counts one first-frame anchor plus entire784-byte small frame, not only520-byte growth");
#else
    check(physical == NEW_TAIL, "non-Rune split write retains complete ordinary backing writes");
#endif
    check(!fstat(fd, &status) && status.st_size == (off_t)capacity &&
          pread(fd, actual, capacity, 0) == (ssize_t)capacity && !memcmp(actual, expected, capacity) &&
          lseek(fd, 0, SEEK_CUR) == 137,
          "split small-tail ACK exposes exact prefix/interior/zero EOF growth and preserves caller position");
    close_connections();broker_mode = connected = 0;
    close(fd);free(actual);free(expected);
}

static void osrs_small_atomic_contracts(void)
{
    enum { LENGTH = 8192 };
    const uint32_t sizes[] = {1, 512, 4096};
    unsigned char expected[LENGTH], desired[LENGTH], actual[LENGTH];
    struct stat status;
    int fd = open(osrs_data_path, O_TRUNC | O_RDWR), client;
    assert(fd >= 0);memset(expected, 0x5a, sizeof(expected));
    assert(pwrite(fd, expected, sizeof(expected), 0) == sizeof(expected));
    assert(lseek(fd, 127, SEEK_SET) == 127);
    reset_write_faults();client = connection();
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint32_t size = sizes[i];
        uint64_t bytes_before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
        uint64_t calls_before = __atomic_load_n(&physical_write_calls, __ATOMIC_SEQ_CST);
        uint64_t reads_before = __atomic_load_n(&physical_read_calls, __ATOMIC_SEQ_CST);
#ifdef WINE_PS5_RUNESCAPE
        struct wb_write_metrics metrics_before = write_metrics_snapshot();
#endif
        memcpy(desired, expected, sizeof(desired));
        desired[0] ^= 0xff;desired[size / 2] ^= 0x33;desired[size - 1] ^= 0x7f;
        struct wb_reply changed = raw_write_request(client, desired, size, 0);
        struct wb_reply identical = raw_write_request(client, desired, size, 0);
        memcpy(expected, desired, size);
        check(changed.result == (int32_t)size && identical.result == (int32_t)size,
              "small changed and identical requests acknowledge full logical size");
        check(__atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - bytes_before == 2 * size &&
              __atomic_load_n(&physical_write_calls, __ATOMIC_SEQ_CST) - calls_before == 2 &&
              __atomic_load_n(&physical_read_calls, __ATOMIC_SEQ_CST) == reads_before,
              "1/512/4096-byte requests each retain one whole native pwrite without diff reads/anchors");
#ifdef WINE_PS5_RUNESCAPE
        struct wb_write_metrics metrics_after = write_metrics_snapshot();
        check(metrics_after.small_bytes - metrics_before.small_bytes == 2 * size &&
              metrics_after.pwrite_bytes - metrics_before.pwrite_bytes == 2 * size &&
              metrics_after.pread_bytes == metrics_before.pread_bytes &&
              metrics_after.anchor_bytes == metrics_before.anchor_bytes,
              "production small-atomic metrics distinguish ordinary writes from metadata anchors");
#endif
        check(!fstat(fd, &status) && status.st_size == LENGTH &&
              pread(fd, actual, sizeof(actual), 0) == sizeof(actual) &&
              !memcmp(actual, expected, sizeof(actual)) && lseek(fd, 0, SEEK_CUR) == 127,
              "small native writes preserve complete data/EOF and independent caller position");
    }
    close(client);close_connections();close(fd);
}


#ifdef WINE_PS5_RUNESCAPE
struct inode_race_client {
    int fd;
    const char *path;
    const unsigned char *bytes;
    struct wb_reply reply;
};
static void *inode_race_server(void *arg)
{
    int fd = (int)(intptr_t)arg;
    if (fd == inode_race.a_server) inode_race_role = 1;
    else { assert(fd == inode_race.b_server);inode_race_role = 2; }
    return serve(arg);
}
static int inode_race_connection(int role)
{
    int pair[2];
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_reply reply;
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    if (role == 1) inode_race.a_server = pair[1];
    else inode_race.b_server = pair[1];
    start_server(inode_race_server, pair[1]);
    memcpy(hello.token, token, sizeof(token));
    assert(!transfer(pair[0], &hello, sizeof(hello), 1));
    assert(!transfer(pair[0], &reply, sizeof(reply), 0));
    assert(reply.magic == WB_MAGIC && !reply.result);
    return pair[0];
}
static void *inode_race_write(void *arg)
{
    struct inode_race_client *client = arg;
    client->reply = raw_write_path(client->fd, client->path, client->bytes, 8192, 0);
    return NULL;
}
static void osrs_same_inode_writer_race(void)
{
    enum { LENGTH = 8192 };
    unsigned char a[LENGTH], b[LENGTH], actual[LENGTH];
    char saved_index[1100];
    struct stat identity;
    pthread_t a_thread, b_thread;
    struct inode_race_client a_client = {.path = OSRS_DATA, .bytes = a};
    struct inode_race_client b_client = {.path = OSRS_INDEX, .bytes = b};
    int b_completed_before_release;
    close_connections();reset_write_faults();
    int fd = open(osrs_data_path, O_TRUNC | O_RDWR);assert(fd >= 0);
    memset(a, 0x41, sizeof(a));memset(b, 0x42, sizeof(b));
    assert(pwrite(fd, a, sizeof(a), 0) == sizeof(a) && !fstat(fd, &identity));
    snprintf(saved_index, sizeof(saved_index), "%s.race-saved", osrs_index_path);
    assert(!rename(osrs_index_path, saved_index));
    assert(!link(osrs_data_path, osrs_index_path));

    /* Exercise the real helper even in RED, where serve_write is not yet
     * wired to it. This also initializes its bounded stripes before threads. */
    pthread_mutex_t *lock = wb_lock_inode(identity.st_dev, identity.st_ino);
    assert(lock && !pthread_mutex_unlock(lock));
    memset(&inode_race, 0, sizeof(inode_race));inode_race.enabled = 1;
    a_client.fd = inode_race_connection(1);
    assert(!pthread_create(&a_thread, NULL, inode_race_write, &a_client));
    assert(!pthread_mutex_lock(&inode_race_mutex));
    struct timespec deadline = inode_race_deadline();
    while (!inode_race.a_read && !inode_race.a_done)
        assert(!pthread_cond_timedwait(&inode_race_cond, &inode_race_mutex, &deadline));
    check(inode_race.a_read, "race pauses A after actual fresh backing pread");
    assert(!pthread_mutex_unlock(&inode_race_mutex));

    b_client.fd = inode_race_connection(2);
    assert(!pthread_create(&b_thread, NULL, inode_race_write, &b_client));
    assert(!pthread_mutex_lock(&inode_race_mutex));
    deadline = inode_race_deadline();
    /* Unwired/incorrect locking must let B finish before releasing A's
     * snapshot. Correct same-inode locking instead blocks B at its real lock
     * attempt, so release A without a timing-based blocked-thread oracle. */
    while (!inode_race.b_same_lock && !inode_race.b_done)
        assert(!pthread_cond_timedwait(&inode_race_cond, &inode_race_mutex, &deadline));
    b_completed_before_release = inode_race.b_done;
    inode_race.release_a = 1;assert(!pthread_cond_broadcast(&inode_race_cond));
    assert(!pthread_mutex_unlock(&inode_race_mutex));
    assert(!pthread_join(a_thread, NULL) && !pthread_join(b_thread, NULL));
    close(a_client.fd);close(b_client.fd);close_connections();
    check(a_client.reply.magic == WB_MAGIC && a_client.reply.result == LENGTH &&
          b_client.reply.magic == WB_MAGIC && b_client.reply.result == LENGTH,
          "both concurrent same-inode requests retain complete logical ACKs");
    assert(pread(fd, actual, sizeof(actual), 0) == sizeof(actual));
    check(!memcmp(actual, a, sizeof(actual)) || !memcmp(actual, b, sizeof(actual)),
          "same-inode writers cannot leave B image with stale A anchor or other mixed content");
    check(inode_race.a_lock && inode_race.b_same_lock && !b_completed_before_release,
          "different-path aliases acquire the same real inode stripe before comparison");
    printf("BROKER_INODE_RACE first=%u last=%u a_lock=%u b_same_lock=%u b_completed_before_release=%u\n",
           (unsigned)actual[0], (unsigned)actual[LENGTH - 1], !!inode_race.a_lock,
           inode_race.b_same_lock, b_completed_before_release);
    inode_race.enabled = 0;
    close(fd);assert(!unlink(osrs_index_path) && !rename(saved_index, osrs_index_path));
}
#endif

static void osrs_write_fault_contracts(void)
{
    unsigned char original[4096], desired[4096], actual[4096];
    uint64_t before;
    struct wb_reply reply;
    int fd = open(osrs_data_path, O_TRUNC | O_RDWR), client;
    assert(fd >= 0);memset(original, 0x61, sizeof(original));memset(desired, 0x62, sizeof(desired));
    assert(pwrite(fd, original, sizeof(original), 0) == sizeof(original));
    reset_write_faults();client = connection();
    fault_write_eintr = 1;fault_write_limit = 7;
    reply = raw_write_request(client, desired, sizeof(desired), 0);
    check(reply.result == sizeof(desired) && !fault_write_eintr,
          "broker retries EINTR and successful short backing writes to full logical ACK");
    check(pread(fd, actual, sizeof(actual), 0) == sizeof(actual) && !memcmp(actual, desired, sizeof(actual)),
          "short-write retries preserve every input byte");
    reset_write_faults();
    assert(pwrite(fd, original, sizeof(original), 0) == sizeof(original));
    fault_write_limit = 3;fault_write_after = 1;fault_write_error = ENOSPC;
    before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
#ifdef WINE_PS5_RUNESCAPE
    struct wb_write_metrics metrics_before = write_metrics_snapshot();
#endif
    reply = raw_write_request(client, desired, sizeof(desired), 0);
    check(reply.result == -ENOSPC, "partially committed request keeps existing negative ENOSPC reply");
    check(__atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - before == 3,
          "physical accounting includes only successful prefix on failed request, with no anchor");
#ifdef WINE_PS5_RUNESCAPE
    struct wb_write_metrics metrics_after = write_metrics_snapshot();
    check(metrics_after.pwrite_bytes - metrics_before.pwrite_bytes == 3 &&
          metrics_after.requests - metrics_before.requests == 1,
          "production metrics include committed bytes and request count on negative ACK");
#endif
    assert(pread(fd, actual, sizeof(actual), 0) == sizeof(actual));
    check(!memcmp(actual, desired, 3) && !memcmp(actual + 3, original + 3, sizeof(actual) - 3),
          "failed backing write preserves exact committed prefix and untouched suffix");
    reset_write_faults();
    assert(pwrite(fd, original, sizeof(original), 0) == sizeof(original));
    fault_write_after = 0;fault_write_zero = 1;
    before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    reply = raw_write_request(client, desired, sizeof(desired), 0);
    check(reply.result == -EIO && __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) == before,
          "zero backing write becomes negative EIO with no fabricated bytes/anchor");
    reset_write_faults();
    reply = raw_write_request(client, desired, sizeof(desired), 0);
    check(reply.result == sizeof(desired), "negative write replies leave stream correctly framed for retry");
    before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    reply = raw_write_request(client, desired, 0, 0);
    check(!reply.result && __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) == before,
          "zero-length request performs no backing write or metadata anchor");
    fault_write_after = 0;fault_write_error = ENOSPC;
    reply = raw_write_request(client, desired, sizeof(desired), 0);
    check(reply.result == -ENOSPC && __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) == before,
          "identical small native write failure remains a negative write reply");
    reset_write_faults();
    close(client);close_connections();

    assert(pwrite(fd, original, sizeof(original), 0) == sizeof(original));
    fault_deny_read = 1;client = connection();
    before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    reply = raw_write_request(client, desired, sizeof(desired), 0);
    check(reply.result == sizeof(desired) &&
          __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - before == sizeof(desired),
          "read-denied open falls back to permitted ordinary write without new read permission requirement");
    close(client);close_connections();reset_write_faults();
    close(fd);
}

#ifdef WINE_PS5_RUNESCAPE
static void osrs_diff_helper_contracts(void)
{
    enum { LENGTH = 3 * 65536 + 19 };
    unsigned char *desired = malloc(LENGTH), *actual = malloc(LENGTH), *compare = malloc(WB_COMPARE_CHUNK);
    struct wb_write_metrics metrics;
    struct stat before_status, after_status;
    int fd = open(osrs_data_path, O_TRUNC | O_RDWR);
    assert(fd >= 0 && desired && actual && compare);
    memset(desired, 0x67, LENGTH);
    assert(pwrite(fd, desired, LENGTH, 0) == LENGTH);
    assert(lseek(fd, 291, SEEK_SET) == 291);
    int alias = open(osrs_data_path, O_RDWR);assert(alias >= 0);
    assert(lseek(alias, 127, SEEK_SET) == 127);
    memset(&metrics, 0, sizeof(metrics));reset_write_faults();
    check(!wb_write_diff(fd, desired, LENGTH, 0, LENGTH, 1, compare, &metrics, NULL),
          "actual diff helper accepts unchanged allocated data extents");
    check(metrics.pread_bytes == LENGTH && metrics.skipped_bytes == LENGTH &&
          !metrics.pwrite_bytes && !metrics.anchor_bytes,
          "helper compares current bytes and leaves request-level anchor to serve_write");
    check(lseek(alias, 0, SEEK_CUR) == 127, "helper extent queries leave independent caller position untouched");

    pid_t child = fork();assert(child >= 0);
    if (!child) {
        unsigned char changed = 0x31;
        _exit(pwrite(alias, &changed, 1, 123) == 1 ? 0 : 1);
    }
    int child_status;assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status) && !WEXITSTATUS(child_status));
    unsigned char *view = mmap(NULL, LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED, alias, 0);
    assert(view != MAP_FAILED);view[65535] = 0x32;
    memset(&metrics, 0, sizeof(metrics));
    check(!wb_write_diff(fd, desired, LENGTH, 0, LENGTH, 1, compare, &metrics, NULL) &&
          metrics.pwrite_bytes == 2, "fresh comparison sees external-process alias and live mmap mutations");
    check(view[123] == 0x67 && view[65535] == 0x67,
          "real backing writes are immediately visible through pre-existing shared mapping");
    assert(!munmap(view, LENGTH));close(alias);

    for (unsigned mode = 0; mode < 3; mode++) {
        reset_write_faults();memset(&metrics, 0, sizeof(metrics));
        if (mode == 0) fault_read_eintr = 1;
        else if (mode == 1) fault_read_limit = 97;
        else fault_read_error = EIO;
        check(!wb_write_diff(fd, desired, LENGTH, 0, LENGTH, 1, compare, &metrics, NULL),
              "comparison EINTR/short/read-error never breaks a valid ordinary write");
        check(pread(fd, actual, LENGTH, 0) == LENGTH && !memcmp(actual, desired, LENGTH),
              "comparison faults preserve exact final bytes");
        if (mode == 2)
            check(metrics.pwrite_bytes == LENGTH && metrics.read_error_bytes == LENGTH,
                  "comparison error falls back to full write with explicit reason accounting");
    }
    reset_write_faults();fault_seek_unknown = 1;memset(&metrics, 0, sizeof(metrics));
    check(!wb_write_diff(fd, desired, LENGTH, 0, LENGTH, 1, compare, &metrics, NULL) &&
          metrics.pwrite_bytes == LENGTH && metrics.unknown_bytes == LENGTH && !metrics.pread_bytes,
          "unsupported extent query is ordinary-write fallback, never permission to skip");
    reset_write_faults();memset(&metrics, 0, sizeof(metrics));
    check(!wb_write_diff(fd, desired, LENGTH, 0, LENGTH, 0, compare, &metrics, NULL) &&
          metrics.pwrite_bytes == LENGTH && metrics.write_only_bytes == LENGTH && !metrics.pread_bytes,
          "write-only helper does no reads and writes the full input");

    assert(!ftruncate(fd, 0) && !ftruncate(fd, LENGTH));
    assert(!fstat(fd, &before_status));memset(desired, 0, LENGTH);memset(&metrics, 0, sizeof(metrics));
    check(!wb_write_diff(fd, desired, LENGTH, 0, LENGTH, 1, compare, &metrics, NULL) &&
          metrics.pwrite_bytes == LENGTH && metrics.hole_bytes + metrics.unknown_bytes == LENGTH,
          "equal zero-filled holes are physically written, not skipped as cached zeros");
    check(!fstat(fd, &after_status) && after_status.st_size == LENGTH &&
          after_status.st_blocks >= before_status.st_blocks,
          "zero-hole fallback preserves EOF and performs native allocation");
    assert(!ftruncate(fd, 0) && !ftruncate(fd, LENGTH));
    memset(desired, 0, LENGTH);memset(desired, 0x67, 65536);
    memset(desired + 2 * 65536, 0x67, LENGTH - 2 * 65536);
    assert(pwrite(fd, desired, 65536, 0) == 65536);
    assert(pwrite(fd, desired + 2 * 65536, LENGTH - 2 * 65536, 2 * 65536) == LENGTH - 2 * 65536);
    memset(&metrics, 0, sizeof(metrics));
    check(!wb_write_diff(fd, desired, LENGTH, 0, LENGTH, 1, compare, &metrics, NULL) &&
          metrics.pwrite_bytes == 65536 && metrics.hole_bytes == 65536 &&
          metrics.skipped_bytes == LENGTH - 65536,
          "mixed data/hole/data request compares reported data and physically fills the zero hole");
    check(pread(fd, actual, LENGTH, 0) == LENGTH && !memcmp(actual, desired, LENGTH),
          "mixed extent traversal preserves all bytes across extent boundaries");
    reset_write_faults();close(fd);free(compare);free(actual);free(desired);
}
#endif

static void osrs_identical_metadata_contract(void)
{
    enum { LENGTH = 8192 };
    unsigned char bytes[LENGTH];
    struct stat before, after;
    struct timespec old[2] = {{1000000000, 123456789}, {1000000000, 987654321}};
    int fd = open(osrs_data_path, O_TRUNC | O_RDWR), client;
    assert(fd >= 0);memset(bytes, 0x59, sizeof(bytes));
    assert(pwrite(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    assert(!futimens(fd, old) && !fstat(fd, &before));
#ifdef __linux__
    int events = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    assert(events >= 0 && inotify_add_watch(events, osrs_data_path, IN_MODIFY) >= 0);
#endif
    uint64_t physical = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    client = connection();
    struct wb_reply reply = raw_write_request(client, bytes, sizeof(bytes), 0);
    check(reply.result == sizeof(bytes), "identical metadata write still acknowledges full payload");
    check(!fstat(fd, &after) && after.st_mtim.tv_sec > before.st_mtim.tv_sec &&
          (after.st_ctim.tv_sec > before.st_ctim.tv_sec ||
           (after.st_ctim.tv_sec == before.st_ctim.tv_sec && after.st_ctim.tv_nsec >= before.st_ctim.tv_nsec)),
          "identical native anchor updates mtime and does not restore/clobber ctime");
#ifdef WINE_PS5_RUNESCAPE
    check(__atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - physical == 1,
          "identical metadata request performs exactly one counted backing byte");
#else
    check(__atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - physical == sizeof(bytes),
          "non-Rune identical metadata request retains full native write");
#endif
#ifdef __linux__
    unsigned char event_bytes[4096];
    ssize_t length = read(events, event_bytes, sizeof(event_bytes));
    int modified = 0;
    for (ssize_t at = 0; length > 0 && at + (ssize_t)sizeof(struct inotify_event) <= length; ) {
        struct inotify_event event;memcpy(&event, event_bytes + at, sizeof(event));
        modified |= !!(event.mask & IN_MODIFY);at += sizeof(event) + event.len;
    }
    check(modified, "identical anchor emits native WRITE notification, not an attributes-only timestamp change");
    close(events);
#endif
    uint64_t failed_before = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    fault_write_after = 0;fault_write_error = ENOSPC;
    reply = raw_write_request(client, bytes, sizeof(bytes), 0);
    check(reply.result == -ENOSPC &&
          __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) == failed_before,
          "failed >4096 identical anchor keeps negative ACK and records no successful backing bytes");
    reset_write_faults();
    close(client);close_connections();close(fd);
}

static void osrs_disconnect_contracts(void)
{
    enum { LENGTH = 2 * WB_CHUNK };
    unsigned char *bytes = malloc(LENGTH), *actual = malloc(LENGTH);
    int fd = open(osrs_data_path, O_TRUNC | O_RDWR);
    assert(fd >= 0 && bytes && actual);memset(bytes, 0x73, LENGTH);
    assert(pwrite(fd, bytes, LENGTH, 0) == LENGTH);
    for (unsigned completed = 0; completed < 2; completed++) {
        int client = connection();
        struct wb_write request = {.magic = WB_MAGIC, .path_length = sizeof(OSRS_DATA) - 1,
                                   .length = LENGTH};
        uint64_t physical = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
#ifdef WINE_PS5_RUNESCAPE
        struct wb_write_metrics metrics_before = write_metrics_snapshot();
#endif
        assert(!transfer(client, &request, sizeof(request), 1));
        assert(!transfer(client, (void *)OSRS_DATA, request.path_length, 1));
        if (completed) assert(!transfer(client, bytes, WB_CHUNK, 1));
        /* Distinct incomplete next-chunk bytes must never become an anchor. */
        unsigned char partial[37];memset(partial, 0x18, sizeof(partial));
        assert(!transfer(client, partial, sizeof(partial), 1));
        assert(!shutdown(client, SHUT_WR));
        struct wb_reply reply;
        check(read(client, &reply, sizeof(reply)) == 0, "incomplete payload closes connection without ACK");
        close(client);close_connections();
        uint64_t written = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST) - physical;
#ifdef WINE_PS5_RUNESCAPE
        check(written == completed, "disconnect anchors exactly one completed identical prefix, never partial input");
        struct wb_write_metrics metrics_after = write_metrics_snapshot();
        check(metrics_after.requests - metrics_before.requests == 1 &&
              metrics_after.anchor_bytes - metrics_before.anchor_bytes == completed &&
              metrics_after.pwrite_bytes - metrics_before.pwrite_bytes == completed,
              "production metrics retain completed-prefix anchor and failed request on disconnect");
#else
        check(written == (uint64_t)completed * WB_CHUNK, "non-Rune disconnect preserves original completed chunks");
#endif
        check(pread(fd, actual, LENGTH, 0) == LENGTH && !memcmp(actual, bytes, LENGTH),
              "disconnect never writes partial network buffer or corrupts saved prefix anchor");
    }
    close(fd);free(actual);free(bytes);
}

int main(void)
{
    int fd;
#ifdef WINE_PS5_RUNESCAPE
    assert(WB_VERSION == 8);
#else
    assert(WB_VERSION == 6);
    check(!physical_read_bytes && !physical_read_calls, "non-Rune control starts without comparison reads");
#endif
    wine_ps5_title_critical_signals(&ps5_title_block_set);
    assert(mkdtemp(directory));
    snprintf(token_path, sizeof(token_path), "%s/token", directory);
    snprintf(writes_path, sizeof(writes_path), "%s/writes", directory);
    fd = open(token_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, token, sizeof(token)) == sizeof(token));
    close(fd);
    pool_index_tests();
    pool_tests();
    osrs_tail_tests();
    osrs_diff_contracts();
#ifdef WINE_PS5_RUNESCAPE
    osrs_same_inode_writer_race();
#endif
    osrs_write_fault_contracts();
    osrs_small_atomic_contracts();
    osrs_split_small_tail_contract();
#ifdef WINE_PS5_RUNESCAPE
    osrs_ps5_unknown_extent_rpc_contract();
    osrs_diff_helper_contracts();
    osrs_proof_metadata_contracts();
    osrs_proof_eviction_contract();
    osrs_proof_range_bounds();
    osrs_proof_failed_request_contracts();
    osrs_proof_unavailable_contracts();
    osrs_proof_terminal_session_contract();
#endif
    osrs_identical_metadata_contract();
    osrs_disconnect_contracts();
    protocol_tests();
    menu_removal_tests();
    permission_repair_tests();
    close_connections();
    client_tests();
    for (unsigned int i = 0; i < BOUNCE_SLOTS; i++)
        if (bounce_buffer[i]) assert(!munmap(bounce_buffer[i], BOUNCE_SIZE));
    {
        char cleanup[1100];
        snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", directory);
        assert(!system(cleanup));
    }
#ifdef WINE_PS5_RUNESCAPE
    check(extent_log_count == 1, "raw allocation/seek diagnostic remains once-only across sessions and faults");
#endif
    if (failures) return 1;
    puts("write broker: concurrent writes/statfs, no-follow removal, device-bounded startup permission repair PASS");
    return 0;
}
