/* Linked with the existing title --wrap convention, so direct native calls
 * and ELF import-table aliases use the same shared-data creation boundary.
 * No process umask, new syscall, or FD/path tracking table is required. */
#ifndef RUNESCAPE_SHARED_IO_H
#define RUNESCAPE_SHARED_IO_H
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wine/ps5_shared_mode.h>

char *ps5_realpath(const char *, char *);
int __real_open(const char *, int, ...);
FILE *__real_fopen(const char *, const char *);
int __real_mkdir(const char *, mode_t);
int __real_chmod(const char *, mode_t);


static int rs_shared_created_fd(int fd, mode_t execute)
{
    struct stat status;
    mode_t wanted;
    if (fstat(fd, &status)) return -1;
    if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)) return 0;
    /* A hardlink may have another name outside this data root. */
    if (S_ISREG(status.st_mode) && status.st_nlink > 1) return 0;
    wanted = wine_ps5_shared_permissions(status.st_mode | (execute & 0111));
    return (wanted & 07777) == (status.st_mode & 07777) ? 0 : fchmod(fd, wanted);
}

int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    int fd, selected;
    char resolved[4096];
    if (flags & O_CREAT) {
        va_list args;va_start(args, flags);mode = (mode_t)va_arg(args, int);va_end(args);
    }
    selected = (flags & O_CREAT) && wine_ps5_shared_resolve(path, resolved, ps5_realpath);
    if (selected) {
        mode = (mode & 0111) | 0666;
        fd = __real_open(resolved, flags, mode);
    } else fd = __real_open(path, flags, mode);
    if (fd >= 0 && selected && rs_shared_created_fd(fd, mode)) {
        int error = errno;close(fd);errno = error;return -1;
    }
    return fd;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    char resolved[4096];
    int writing = mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+') != NULL;
    int selected = writing && wine_ps5_shared_resolve(path, resolved, ps5_realpath);
    FILE *file = __real_fopen(selected ? resolved : path, mode);
    if (file && selected && rs_shared_created_fd(fileno(file), 0)) {
        int error = errno;fclose(file);errno = error;return NULL;
    }
    return file;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    char resolved[4096];
    int selected = wine_ps5_shared_resolve(path, resolved, ps5_realpath);
    int result = __real_mkdir(selected ? resolved : path, selected ? 0777 : mode);
    if (!result && selected) {
        int fd = __real_open(resolved, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0) return -1;
        result = rs_shared_created_fd(fd, 0);
        int error = errno;close(fd);errno = error;
    }
    return result;
}

int __wrap_chmod(const char *path, mode_t mode)
{
    char resolved[4096];
    struct stat status;
    if (!wine_ps5_shared_resolve(path, resolved, ps5_realpath)) return __real_chmod(path, mode);
    if (stat(resolved, &status)) return -1;
    if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)) return __real_chmod(path, mode);
    if (S_ISREG(status.st_mode) && status.st_nlink > 1) return __real_chmod(path, mode);
    mode = wine_ps5_shared_permissions(status.st_mode | (mode & 0111));
    if ((mode & 07777) == (status.st_mode & 07777)) return 0;
    return __real_chmod(resolved, mode);
}
#endif
