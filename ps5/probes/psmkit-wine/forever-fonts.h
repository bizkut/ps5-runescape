/* App-owned HKLM values for the faces the managed sync puts in
 * C:\windows\Fonts. They are applied to the prefix's system.reg before the
 * title starts Wine, whose in-process wineserver then loads the hive. Only the
 * listed REG_SZ values change; every other key, value and byte is kept. */
#ifndef FOREVER_FONTS_H
#define FOREVER_FONTS_H
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "forever-setup.h"

#define FOREVER_FONT_VALUES 64
#define FOREVER_FONT_REGISTRY_LIMIT (64u << 10)
#define FOREVER_FONT_HIVE_LIMIT (256u << 20)
#define FOREVER_FONT_HIVE_HEADER "WINE REGISTRY Version 2\n"

struct forever_font_value
{
    const char *key, *name, *value;
};

/* Printable ASCII without the characters Wine's hive syntax would escape. */
static inline int forever_font_field(const char *text, int key)
{
    if (!*text) return 0;
    for (; *text; ++text)
        if (*text < 0x20 || *text > 0x7e || *text == '"' || (!key && *text == '\\') ||
            (key && (*text == '[' || *text == ']')))
            return 0;
    return 1;
}

/* Lines "key\tname\tvalue\n"; the key uses single backslashes. */
static inline int forever_font_parse(char *text, struct forever_font_value *values, size_t *count)
{
    *count = 0;
    for (char *line = text, *next; *line; line = next)
    {
        char *end = strchr(line, '\n'), *name, *value;
        if (!end) return -EINVAL;
        next = end + 1;
        *end = 0;
        if (!(name = strchr(line, '\t')) || !(value = strchr(name + 1, '\t'))) return -EINVAL;
        *name++ = 0;
        *value++ = 0;
        if (strchr(value, '\t') || !forever_font_field(line, 1) || !forever_font_field(name, 0) ||
            !forever_font_field(value, 0) || *count == FOREVER_FONT_VALUES)
            return -EINVAL;
        values[(*count)++] = (struct forever_font_value){ line, name, value };
    }
    return *count ? 0 : -EINVAL;
}

/* Wine's saved key line: "[A\\B] stamp", backslashes doubled, case-insensitive. */
static inline int forever_font_key_line(const char *line, const char *end, const char *key)
{
    if (line == end || *line++ != '[') return 0;
    for (; *key; ++key)
    {
        if (*key == '\\')
        {
            if (end - line < 2 || line[0] != '\\' || line[1] != '\\') return 0;
            line += 2;
        }
        else if (line == end || tolower((unsigned char)*line++) != tolower((unsigned char)*key))
            return 0;
    }
    return line < end && *line == ']';
}

static inline const char *forever_font_line_end(const char *line, const char *end)
{
    const char *newline = memchr(line, '\n', (size_t)(end - line));
    return newline ? newline + 1 : end;
}

/* One value's physical lines: hex data continues while a line ends in '\'. */
static inline const char *forever_font_value_end(const char *line, const char *end)
{
    const char *next = forever_font_line_end(line, end);
    while (next < end && next - line >= 2 && next[-1] == '\n' && next[-2] == '\\')
        next = forever_font_line_end(next, end);
    return next;
}

static inline int forever_font_value_line(const char *line, const char *end, const char *name)
{
    size_t length = strlen(name);
    return (size_t)(end - line) > length + 2 && line[0] == '"' &&
           !strncasecmp(line + 1, name, length) && line[length + 1] == '"' && line[length + 2] == '=';
}

struct forever_font_buffer
{
    char *data;
    size_t used, size;
};

static inline int forever_font_append(struct forever_font_buffer *buffer, const char *data, size_t size)
{
    if (size > buffer->size - buffer->used) return -EFBIG;
    memcpy(buffer->data + buffer->used, data, size);
    buffer->used += size;
    return 0;
}

static inline int forever_font_append_value(struct forever_font_buffer *buffer,
                                            const struct forever_font_value *value)
{
    char line[512];
    int length = snprintf(line, sizeof(line), "\"%s\"=\"%s\"\n", value->name, value->value);
    if (length < 0 || (size_t)length >= sizeof(line)) return -ENAMETOOLONG;
    return forever_font_append(buffer, line, (size_t)length);
}

/* Rewrite one key's values into output; *changed reports any difference. */
static inline int forever_font_key(const char *hive, size_t size, struct forever_font_buffer *output,
                                   const struct forever_font_value *values, size_t count,
                                   const char *key, int *changed)
{
    const char *end = hive + size, *section = NULL, *body = NULL, *close = end, *line;
    unsigned char found[FOREVER_FONT_VALUES] = {0};
    int error;

    output->used = 0;
    for (line = hive; line < end; line = forever_font_line_end(line, end))
    {
        if (!section && forever_font_key_line(line, end, key))
        {
            section = line;
            body = forever_font_line_end(line, end);
        }
        else if (section && line >= body && *line == '[')
        {
            close = line;
            break;
        }
    }
    if (!section)
    {
        char header[1024];
        long long filetime = (long long)time(NULL) * 10000000LL + 116444736000000000LL;
        int length;
        size_t used = 0;
        if ((error = forever_font_append(output, hive, size))) return error;
        if (size && hive[size - 1] != '\n' && (error = forever_font_append(output, "\n", 1))) return error;
        header[used++] = '\n';
        header[used++] = '[';
        for (const char *c = key; *c; ++c)
        {
            if (used + 4 >= sizeof(header)) return -ENAMETOOLONG;
            if (*c == '\\') header[used++] = '\\';
            header[used++] = *c;
        }
        length = snprintf(header + used, sizeof(header) - used, "] %lld\n#time=%llx\n",
                          (long long)time(NULL), (unsigned long long)filetime);
        if (length < 0 || (size_t)length >= sizeof(header) - used) return -ENAMETOOLONG;
        if ((error = forever_font_append(output, header, used + (size_t)length))) return error;
        for (size_t i = 0; i < count; ++i)
            if (!strcmp(values[i].key, key) && (error = forever_font_append_value(output, &values[i])))
                return error;
        *changed = 1;
        return 0;
    }
    /* Keep the section's trailing blank lines after any appended values. */
    const char *tail = close;
    while (tail - body >= 2 && tail[-1] == '\n' && tail[-2] == '\n') --tail;
    if ((error = forever_font_append(output, hive, (size_t)(body - hive)))) return error;
    for (line = body; line < tail;)
    {
        const char *next = forever_font_value_end(line, tail);
        size_t i;
        for (i = 0; i < count; ++i)
            if (!strcmp(values[i].key, key) && !found[i] && forever_font_value_line(line, next, values[i].name))
                break;
        if (i == count)
        {
            if ((error = forever_font_append(output, line, (size_t)(next - line)))) return error;
        }
        else
        {
            char expected[512];
            int length = snprintf(expected, sizeof(expected), "\"%s\"=\"%s\"\n",
                                  values[i].name, values[i].value);
            if (length < 0 || (size_t)length >= sizeof(expected)) return -ENAMETOOLONG;
            found[i] = 1;
            if ((size_t)(next - line) != (size_t)length || memcmp(line, expected, (size_t)length))
                *changed = 1;
            if ((error = forever_font_append(output, expected, (size_t)length))) return error;
        }
        line = next;
    }
    if (tail > body && tail[-1] != '\n' && (error = forever_font_append(output, "\n", 1))) return error;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(values[i].key, key) && !found[i])
        {
            if ((error = forever_font_append_value(output, &values[i]))) return error;
            *changed = 1;
        }
    return forever_font_append(output, tail, (size_t)(end - tail));
}

/* Durable replacement: a torn system.reg would make the title refuse the prefix. */
static inline int forever_font_publish(const char *prefix, const char *path, const char *data,
                                       size_t size, mode_t mode)
{
    char temporary[1100];
    int fd, error = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.forever-tmp", path) >= (int)sizeof(temporary))
        return -ENAMETOOLONG;
    if (unlink(temporary) && errno != ENOENT) return -errno;
    if ((fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode & 0777)) < 0) return -errno;
    while (size && !error)
    {
        ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) error = written < 0 ? -errno : -EIO;
        else { data += written; size -= (size_t)written; }
    }
    if (!error && fsync(fd)) error = -errno;
    if (close(fd) && !error) error = -errno;
    if (!error && rename(temporary, path)) error = -errno;
    if (error)
    {
        unlink(temporary);
        return error;
    }
    return forever_init_sync_directory(prefix);
}

/* 1 rewritten, 0 already current, -errno failure (the hive is then untouched).
 * A missing, non-regular, symlinked or non-Wine hive is refused. */
static inline int forever_register_fonts(const char *prefix, const char *registration_path)
{
    static char registration[FOREVER_FONT_REGISTRY_LIMIT];
    struct forever_font_value values[FOREVER_FONT_VALUES];
    struct forever_font_buffer output = {0};
    char path[1024], *hive = NULL, *current;
    size_t count, size = 0;
    ssize_t length;
    struct stat status;
    int fd = -1, error = 0, changed = 0;

    if ((length = forever_read_file(registration_path, registration, sizeof(registration))) < 0)
        return (int)length;
    if (memchr(registration, 0, (size_t)length) || (error = forever_font_parse(registration, values, &count)))
        return error ? error : -EINVAL;
    if (snprintf(path, sizeof(path), "%s/system.reg", prefix) >= (int)sizeof(path)) return -ENAMETOOLONG;
    if ((fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)) < 0) return -errno;
    /* PS5 reports st_nlink=0 for ordinary /data files (as in forever-setup.h):
     * only a reported alias (>1) is refused. */
    if (fstat(fd, &status)) error = -errno;
    else if (!S_ISREG(status.st_mode)) error = -EINVAL;
    else if (status.st_nlink > 1) error = -EMLINK;
    else if (status.st_size < (off_t)sizeof(FOREVER_FONT_HIVE_HEADER) - 1 ||
             status.st_size > (off_t)FOREVER_FONT_HIVE_LIMIT) error = -EFBIG;
    else if (!(hive = malloc((size_t)status.st_size + 1))) error = -ENOMEM;
    while (!error && size < (size_t)status.st_size)
    {
        ssize_t count_read = read(fd, hive + size, (size_t)status.st_size - size);
        if (count_read < 0 && errno == EINTR) continue;
        if (count_read <= 0) error = count_read < 0 ? -errno : -EIO;
        else size += (size_t)count_read;
    }
    if (close(fd) && !error) error = -errno;
    if (!error && (memcmp(hive, FOREVER_FONT_HIVE_HEADER, sizeof(FOREVER_FONT_HIVE_HEADER) - 1) ||
                   memchr(hive, 0, size)))
        error = -EINVAL;
    current = hive;
    for (size_t i = 0; !error && i < count; ++i)
    {
        size_t j;
        for (j = 0; j < i && strcmp(values[j].key, values[i].key); ++j) {}
        if (j < i) continue;
        /* Each key's rewritten hive becomes the input for the next key. */
        output.used = 0;
        output.size = size + sizeof(registration) * 2;
        if (!(output.data = malloc(output.size))) { error = -ENOMEM; break; }
        error = forever_font_key(current, size, &output, values, count, values[i].key, &changed);
        free(error ? output.data : current);
        if (!error)
        {
            current = output.data;
            size = output.used;
        }
    }
    if (!error && changed) error = forever_font_publish(prefix, path, current, size, status.st_mode);
    free(current);
    return error ? error : changed;
}

#endif
