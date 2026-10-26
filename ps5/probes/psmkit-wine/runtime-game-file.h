#ifndef PW_RUNTIME_GAME_FILE_H
#define PW_RUNTIME_GAME_FILE_H
#include "runtime-relaunch.h"
#include <wine/ps5_shared_mode.h>
#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

#define PW_GAME_HANDOFF_DIRECTORY WINE_PS5_PRIVATE_HANDOFF_DIRECTORY
#define PW_GAME_HANDOFF_FLAG "--ps5-osrs-file-v1"
#define PW_GAME_HANDOFF_TOKEN_CHARS 32
/* Same pointer-free header as the validated child request; only used bytes
 * follow it. The reserved private root is excluded from shared-mode policy. */
struct pw_game_handoff_header {
    uint32_t argc, bytes;
    uint32_t offsets[PW_GAME_WIRE_ARGV];
};
_Static_assert(sizeof(struct pw_game_handoff_header) == offsetof(struct pw_game_request, data),
               "private handoff uses the request wire header");

struct pw_game_handoff_diagnostic {
    const char *operation;
    int error, cleanup_error;
    unsigned directory_mode, file_mode;
    uint64_t directory_uid, file_uid, directory_device, file_device, links;
};
static inline void pw_game_handoff_diagnostic_reset(struct pw_game_handoff_diagnostic *diagnostic)
{
    if (diagnostic) { memset(diagnostic, 0, sizeof(*diagnostic));diagnostic->operation = "ready"; }
}
static inline int pw_game_handoff_failure(struct pw_game_handoff_diagnostic *diagnostic,
                                          const char *operation, int error)
{
    if (diagnostic && !diagnostic->error) { diagnostic->operation = operation;diagnostic->error = error; }
    return error;
}

static inline int pw_game_handoff_token_valid(const char *token)
{
    if (!token || strlen(token) != PW_GAME_HANDOFF_TOKEN_CHARS) return 0;
    for (unsigned i = 0; i < PW_GAME_HANDOFF_TOKEN_CHARS; ++i)
        if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) return 0;
    return 1;
}
static inline void pw_game_handoff_token(char token[PW_GAME_HANDOFF_TOKEN_CHARS + 1],
                                         const uint8_t random[16])
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        token[i * 2] = hex[random[i] >> 4];
        token[i * 2 + 1] = hex[random[i] & 15];
    }
    token[PW_GAME_HANDOFF_TOKEN_CHARS] = 0;
}
static inline int pw_game_handoff_path(const char *directory, const char *token, char path[4096])
{
    int length;
    if (!directory || !pw_game_handoff_token_valid(token)) return EINVAL;
    length = snprintf(path, 4096, "%s/%s", directory, token);
    return length < 0 || length >= 4096 ? ENAMETOOLONG : 0;
}
/* The handoff directory is 0777 so FTP and other apps can delete the whole
 * /data/runescape tree. Secrets stay protected by the per-request files: they are
 * created O_EXCL|O_NOFOLLOW with 0600 and must be a single regular file owned by
 * the same uid as the directory, so others can delete but never read or plant one.
 * Creation (startup) chmods an existing directory to 0777 when it can; the startup
 * permission repair (writebroker / rs_repair_permissions) is the backup. Safety does
 * not depend on the directory mode or owner (PS5 uid mapping is unreliable).
 * A nonnegative result is a held directory FD; errors are negative errno. */
static inline int pw_game_handoff_directory(const char *directory, int create, struct stat *status,
                                             struct pw_game_handoff_diagnostic *diagnostic)
{
    int fd, error;
    if (create && mkdir(directory, 0777) && errno != EEXIST)
        return -pw_game_handoff_failure(diagnostic, "mkdir-directory", errno);
    fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -pw_game_handoff_failure(diagnostic, "open-directory", errno);
    if (fstat(fd, status)) {
        error = pw_game_handoff_failure(diagnostic, "stat-directory", errno);close(fd);return -error;
    }
    if (diagnostic) {
        diagnostic->directory_mode = status->st_mode;
        diagnostic->directory_uid = status->st_uid;diagnostic->directory_device = status->st_dev;
    }
    if (!S_ISDIR(status->st_mode)) {
        close(fd);return -pw_game_handoff_failure(diagnostic, "validate-directory-type", ENOTDIR);
    }
    /* Best effort: make an older 0700 directory 0777 now; the startup permission
     * repair (broker backup) fixes it otherwise. Neither mode nor owner may fail prepare. */
    if (create && (status->st_mode & 07777) != 0777 && !fchmod(fd, 0777)) {
        status->st_mode = (status->st_mode & ~07777) | 0777;
        if (diagnostic) diagnostic->directory_mode = status->st_mode;
    }
    return fd;
}
static inline int pw_game_handoff_prepare(const char *directory, unsigned *removed,
                                           struct pw_game_handoff_diagnostic *diagnostic)
{
    struct stat opened, named;
    struct dirent *entry;
    char path[4096];
    int fd, error = 0;
    DIR *stream;
    pw_game_handoff_diagnostic_reset(diagnostic);
    fd = pw_game_handoff_directory(directory, 1, &opened, diagnostic);
    if (removed) *removed = 0;
    if (fd < 0) return -fd;
    stream = opendir(directory);
    if (!stream) { error = pw_game_handoff_failure(diagnostic, "open-scan", errno);close(fd);return error; }
    if (stat(directory, &named) || named.st_dev != opened.st_dev || named.st_ino != opened.st_ino)
        error = pw_game_handoff_failure(diagnostic, "validate-scan-identity", ESTALE);
    while (!error) {
        errno = 0;entry = readdir(stream);
        if (!entry) { if (errno) error = pw_game_handoff_failure(diagnostic, "read-directory", errno);break; }
        if (!pw_game_handoff_token_valid(entry->d_name)) continue;
        error = pw_game_handoff_path(directory, entry->d_name, path);
        if (error) error = pw_game_handoff_failure(diagnostic, "stale-path", error);
        if (!error && unlink(path)) {
            if (errno != ENOENT) error = pw_game_handoff_failure(diagnostic, "unlink-stale", errno);
        } else if (!error && removed) ++*removed;
    }
    if (closedir(stream) && !error) error = pw_game_handoff_failure(diagnostic, "close-scan", errno);
    if (close(fd) && !error) error = pw_game_handoff_failure(diagnostic, "close-directory", errno);
    return error;
}
static inline int pw_game_handoff_transfer(int fd, void *buffer, size_t bytes, int writing)
{
    size_t used = 0;
    while (used < bytes) {
        ssize_t result = writing ? write(fd, (char *)buffer + used, bytes - used) :
                                   read(fd, (char *)buffer + used, bytes - used);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return result < 0 ? errno : EIO;
        used += (size_t)result;
    }
    return 0;
}
static inline int pw_game_handoff_write(const char *directory, const char *token,
                                         const struct pw_game_wire *wire,
                                         struct pw_game_handoff_diagnostic *diagnostic)
{
    struct pw_game_handoff_header header = {0};
    struct stat parent, file;
    char path[4096];
    size_t bytes;
    int directory_fd, fd, error;
    pw_game_handoff_diagnostic_reset(diagnostic);
    error = pw_game_handoff_path(directory, token, path);
    if (error) return pw_game_handoff_failure(diagnostic, "write-path", error);
    if (!wire || wire->argc < PW_GAME_PROGRAM + 1 || wire->argc > PW_GAME_WIRE_ARGV)
        return pw_game_handoff_failure(diagnostic, "validate-record-count", EINVAL);
    bytes = wire->argv[wire->argc - 1] - wire->bytes + strlen(wire->argv[wire->argc - 1]) + 1;
    if (bytes > sizeof(wire->bytes)) return pw_game_handoff_failure(diagnostic, "validate-record-size", E2BIG);
    header.argc = wire->argc;header.bytes = (uint32_t)bytes;
    for (unsigned i = 0; i < wire->argc; ++i) header.offsets[i] = wire->argv[i] - wire->bytes;
    directory_fd = pw_game_handoff_directory(directory, 1, &parent, diagnostic);
    if (directory_fd < 0) return -directory_fd;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { error = pw_game_handoff_failure(diagnostic, "create-file", errno);close(directory_fd);return error; }
    if (fstat(fd, &file)) error = pw_game_handoff_failure(diagnostic, "stat-file", errno);
    else {
        if (diagnostic) {
            diagnostic->file_mode = file.st_mode;diagnostic->file_uid = file.st_uid;
            diagnostic->file_device = file.st_dev;diagnostic->links = file.st_nlink;
        }
        /* PS5 /data fstat can report zero for this exclusive, nofollow create.
         * Only a reported multiply-linked file is an alias rejection. */
        if (!S_ISREG(file.st_mode)) error = pw_game_handoff_failure(diagnostic, "validate-file-type", EACCES);
        else if (file.st_nlink > 1) error = pw_game_handoff_failure(diagnostic, "validate-file-links", EACCES);
        else if (file.st_uid != parent.st_uid) error = pw_game_handoff_failure(diagnostic, "validate-file-owner", EACCES);
        else if (file.st_dev != parent.st_dev) error = pw_game_handoff_failure(diagnostic, "validate-file-device", EACCES);
        else if ((file.st_mode & 07777) != 0600) {
            if (fchmod(fd, 0600)) error = pw_game_handoff_failure(diagnostic, "chmod-file", errno);
            else if (diagnostic) diagnostic->file_mode = (file.st_mode & ~07777) | 0600;
        }
    }
    if (!error) {
        error = pw_game_handoff_transfer(fd, &header, sizeof(header), 1);
        if (error) error = pw_game_handoff_failure(diagnostic, "write-header", error);
    }
    if (!error) {
        error = pw_game_handoff_transfer(fd, (void *)wire->bytes, bytes, 1);
        if (error) error = pw_game_handoff_failure(diagnostic, "write-payload", error);
    }
    if (!error && fsync(fd)) error = pw_game_handoff_failure(diagnostic, "sync-file", errno);
    if (close(fd) && !error) error = pw_game_handoff_failure(diagnostic, "close-file", errno);
    if (close(directory_fd) && !error) error = pw_game_handoff_failure(diagnostic, "close-directory", errno);
    if (error && unlink(path) && errno != ENOENT && diagnostic) diagnostic->cleanup_error = errno;
    return error;
}
static inline int pw_game_handoff_discard(const char *directory, const char *token,
                                           struct pw_game_handoff_diagnostic *diagnostic)
{
    struct stat parent;
    char path[4096];
    int fd, error;
    pw_game_handoff_diagnostic_reset(diagnostic);
    error = pw_game_handoff_path(directory, token, path);
    if (error) return pw_game_handoff_failure(diagnostic, "discard-path", error);
    fd = pw_game_handoff_directory(directory, 0, &parent, diagnostic);
    if (fd < 0) return -fd;
    if (unlink(path) && errno != ENOENT) error = pw_game_handoff_failure(diagnostic, "unlink-pending", errno);
    if (close(fd) && !error) error = pw_game_handoff_failure(diagnostic, "close-directory", errno);
    return error;
}
static inline int pw_game_handoff_read(const char *directory, const char *token,
                                        struct pw_game_request *request, char **argv,
                                        struct pw_game_handoff_diagnostic *diagnostic)
{
    struct stat parent, file;
    char path[4096];
    int directory_fd, fd, error;
    pw_game_handoff_diagnostic_reset(diagnostic);
    error = pw_game_handoff_path(directory, token, path);
    if (error || !request || !argv) return pw_game_handoff_failure(diagnostic, "read-path", error ? error : EINVAL);
    directory_fd = pw_game_handoff_directory(directory, 0, &parent, diagnostic);
    if (directory_fd < 0) return -directory_fd;
    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) {
        error = pw_game_handoff_failure(diagnostic, "open-file", errno);
        if (error != ENOENT && unlink(path) && diagnostic) diagnostic->cleanup_error = errno;
        close(directory_fd);return error;
    }
    if (fstat(fd, &file)) error = pw_game_handoff_failure(diagnostic, "stat-file", errno);
    else if (diagnostic) {
        diagnostic->file_mode = file.st_mode;diagnostic->file_uid = file.st_uid;
        diagnostic->file_device = file.st_dev;diagnostic->links = file.st_nlink;
    }
    /* Consume before parsing/reading; malformed or interrupted starts cannot replay. */
    if (unlink(path) && !error) error = pw_game_handoff_failure(diagnostic, "unlink-consumed", errno);
    if (!error) {
        if (!S_ISREG(file.st_mode)) error = pw_game_handoff_failure(diagnostic, "validate-file-type", EACCES);
        else if (file.st_nlink > 1) error = pw_game_handoff_failure(diagnostic, "validate-file-links", EACCES);
        else if (file.st_uid != parent.st_uid) error = pw_game_handoff_failure(diagnostic, "validate-file-owner", EACCES);
        else if (file.st_dev != parent.st_dev) error = pw_game_handoff_failure(diagnostic, "validate-file-device", EACCES);
        else if ((file.st_mode & 07777) != 0600) error = pw_game_handoff_failure(diagnostic, "validate-file-mode", EACCES);
    }
    if (!error && (file.st_size < (off_t)sizeof(struct pw_game_handoff_header) ||
                  file.st_size > (off_t)sizeof(*request)))
        error = pw_game_handoff_failure(diagnostic, "validate-file-size", EPROTO);
    if (!error) {
        error = pw_game_handoff_transfer(fd, request, (size_t)file.st_size, 0);
        if (error) error = pw_game_handoff_failure(diagnostic, "read-file", error);
    }
    if (close(fd) && !error) error = pw_game_handoff_failure(diagnostic, "close-file", errno);
    if (close(directory_fd) && !error) error = pw_game_handoff_failure(diagnostic, "close-directory", errno);
    if (!error && request->bytes != (size_t)file.st_size - sizeof(struct pw_game_handoff_header))
        error = pw_game_handoff_failure(diagnostic, "validate-record-size", EPROTO);
    if (!error) {
        error = pw_game_request_unpack(request, argv);
        if (error) error = pw_game_handoff_failure(diagnostic, "unpack-record", error);
    }
    return error;
}
#endif
