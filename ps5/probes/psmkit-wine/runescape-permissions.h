/* Before Wine starts, repair data left private by older native reconstruction.
 * The title sandbox has no usable lstat, *at calls, or fdopendir. Follow the existing
 * no-follow open/fstat + opendir traversal convention, without new providers. */
#ifndef RUNESCAPE_PERMISSIONS_H
#define RUNESCAPE_PERMISSIONS_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wine/ps5_shared_mode.h>

struct rs_permission_report {
    unsigned long long scanned, changed, skipped, failed;
    dev_t device;
    int error;
};

static void rs_permission_error(struct rs_permission_report *report, int error)
{
    report->failed++;
    if (!report->error) report->error = -error;
}

static void rs_permission_entry(char *path, size_t length, size_t capacity,
                                unsigned depth, struct rs_permission_report *report)
{
    struct stat opened, named;
    DIR *directory;
    struct dirent *entry;
    int fd;
    mode_t wanted;
    report->scanned++;
    int private_entry = wine_ps5_private_handoff_path(path);
    if (depth > 128) { rs_permission_error(report, ELOOP);return; }
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        int error = errno;
        if (error == ELOOP || (!stat(path, &named) &&
            !S_ISREG(named.st_mode) && !S_ISDIR(named.st_mode))) report->skipped++;
        else rs_permission_error(report, error);
        return;
    }
    if (fstat(fd, &opened)) {
        rs_permission_error(report, errno);close(fd);return;
    }
    if (!depth) report->device = opened.st_dev;
    if (opened.st_dev != report->device ||
        (!S_ISREG(opened.st_mode) && !S_ISDIR(opened.st_mode)) ||
        (S_ISREG(opened.st_mode) && opened.st_nlink > 1)) {
        report->skipped++;close(fd);return;
    }
    /* Only the handoff directory itself is repaired (0777, FTP-deletable); its
     * 0600 session files are never widened and nothing inside is scanned. */
    if (private_entry && !S_ISDIR(opened.st_mode)) { report->skipped++;close(fd);return; }
    wanted = wine_ps5_shared_permissions(opened.st_mode);
    if ((opened.st_mode & 07777) != (wanted & 07777)) {
        if (fchmod(fd, wanted)) rs_permission_error(report, errno);
        else report->changed++;
    }
    if (!S_ISDIR(opened.st_mode) || private_entry) { close(fd);return; }
    directory = opendir(path);
    if (!directory) { rs_permission_error(report, errno);close(fd);return; }
    /* Wine has not started yet. Reject detectable namespace replacement before
     * scanning; every child is separately opened with O_NOFOLLOW as well. */
    if (stat(path, &named) || named.st_dev != opened.st_dev || named.st_ino != opened.st_ino) {
        rs_permission_error(report, ESTALE);closedir(directory);close(fd);return;
    }
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) { if (errno) rs_permission_error(report, errno);break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size_t bytes = strlen(entry->d_name);
        if (bytes + 2 > capacity - length) { rs_permission_error(report, ENAMETOOLONG);continue; }
        path[length] = '/';memcpy(path + length + 1, entry->d_name, bytes + 1);
        rs_permission_entry(path, length + 1 + bytes, capacity, depth + 1, report);
        path[length] = 0;
    }
    if (closedir(directory)) rs_permission_error(report, errno);
    if (close(fd)) rs_permission_error(report, errno);
}

static int rs_repair_permissions(const char *root, struct rs_permission_report *report)
{
    char path[4096];
    size_t length = strlen(root);
    memset(report, 0, sizeof(*report));
    if (!length || length >= sizeof(path)) {
        rs_permission_error(report, ENAMETOOLONG);return report->error;
    }
    memcpy(path, root, length + 1);
    rs_permission_entry(path, length, sizeof(path), 0, report);
    return report->error;
}
#endif
