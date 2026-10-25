/* Bounded parser for the current x64 production SSN JSON schema. No network,
 * device, or Wine dependency; metadata hashes are consistency, not signatures. */
#ifndef RUNESCAPE_SSN_H
#define RUNESCAPE_SSN_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#define RS_JSON_MAX 262144
#define RS_TOKENS 16384
#define RS_FILES 2048
#define RS_PIECES 1024
#define RS_PIECE_MAX 4194304u
#define RS_TOTAL_MAX (UINT64_C(2) * 1024 * 1024 * 1024)
#define RS_CDN "https://jagex.akamaized.net/direct6/launcher-win-x86_64/"
struct rs_token { int start, end, next, count; char type; };
struct rs_json { const char *text; size_t bytes, at; int used; struct rs_token token[RS_TOKENS]; };
struct rs_file { char name[256]; uint64_t size; };
struct rs_manifest {
    char id[65], version[64];
    unsigned files, pieces;
    uint64_t bytes;
    struct rs_file file[RS_FILES];
    char digest[RS_PIECES][65];
};
static void rs_space(struct rs_json *j)
{ while (j->at < j->bytes && strchr(" \r\n\t", j->text[j->at])) j->at++; }
static int rs_value(struct rs_json *j, unsigned depth)
{
    rs_space(j);
    if (depth > 16 || j->at >= j->bytes || j->used == RS_TOKENS) return -1;
    int n = j->used++;
    struct rs_token *t = &j->token[n];
    *t = (struct rs_token){.start = (int)j->at, .type = j->text[j->at]};
    char c = j->text[j->at++];
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        rs_space(j);
        if (j->at < j->bytes && j->text[j->at] == close) j->at++;
        else for (;;) {
            if (c == '{') {
                int key = rs_value(j, depth + 1);
                if (key < 0 || j->token[key].type != '"') return -1;
                /* Ambiguous duplicate object keys are never accepted. */
                for (int p = n + 1; p < key; p = j->token[j->token[p].next].next)
                    if (j->token[p].end - j->token[p].start == j->token[key].end - j->token[key].start &&
                        !memcmp(j->text + j->token[p].start, j->text + j->token[key].start,
                                (size_t)(j->token[p].end - j->token[p].start))) return -1;
                rs_space(j);
                if (j->at >= j->bytes || j->text[j->at++] != ':') return -1;
            }
            if (rs_value(j, depth + 1) < 0) return -1;
            t->count++;
            rs_space(j);
            if (j->at >= j->bytes) return -1;
            c = j->text[j->at++];
            if (c == close) break;
            if (c != ',') return -1;
            c = t->type;
        }
    } else if (c == '"') {
        while (j->at < j->bytes && j->text[j->at] != '"') {
            /* Current schema is ASCII without escapes. Reject alternative
             * spellings (including escaped path separators and object keys). */
            unsigned char ch = (unsigned char)j->text[j->at++];
            if (ch < 32 || ch > 126 || ch == '\\') return -1;
        }
        if (j->at == j->bytes) return -1;
        j->at++;
    } else {
        while (j->at < j->bytes && !strchr(" \r\n\t,}]", j->text[j->at])) j->at++;
        size_t len = j->at - (size_t)t->start;
        const char *s = j->text + t->start;
        if (!((len == 4 && !memcmp(s, "true", 4)) || (len == 5 && !memcmp(s, "false", 5)) ||
              (len == 4 && !memcmp(s, "null", 4)))) {
            if (!len || (len > 1 && s[0] == '0')) return -1;
            for (size_t k = 0; k < len; k++) if (s[k] < '0' || s[k] > '9') return -1;
        }
    }
    t->end = (int)j->at; t->next = j->used;
    return n;
}
static int rs_parse(struct rs_json *j, const char *text, size_t bytes)
{
    memset(j, 0, sizeof(*j)); j->text = text; j->bytes = bytes;
    if (!bytes || bytes > RS_JSON_MAX || memchr(text, 0, bytes) || rs_value(j, 0) != 0) return -1;
    rs_space(j); return j->at == bytes ? 0 : -1;
}
static int rs_equal(const struct rs_json *j, int n, const char *value)
{
    return n >= 0 && j->token[n].type == '"' &&
        j->token[n].end - j->token[n].start == (int)strlen(value) + 2 &&
        !memcmp(j->text + j->token[n].start + 1, value, strlen(value));
}
static int rs_field(const struct rs_json *j, int obj, const char *key)
{
    if (obj < 0 || j->token[obj].type != '{') return -1;
    for (int n = obj + 1; n < j->token[obj].next;) {
        int v = j->token[n].next;
        if (rs_equal(j, n, key)) return v;
        n = j->token[v].next;
    }
    return -1;
}
static int rs_string(const struct rs_json *j, int n, char *out, size_t cap)
{
    if (n < 0 || j->token[n].type != '"') return -1;
    size_t len = (size_t)(j->token[n].end - j->token[n].start - 2);
    if (len >= cap) return -1;
    memcpy(out, j->text + j->token[n].start + 1, len); out[len] = 0; return 0;
}
static int rs_number(const struct rs_json *j, int n, uint64_t *out)
{
    if (n < 0 || j->token[n].type < '0' || j->token[n].type > '9') return -1;
    uint64_t value = 0;
    for (int i = j->token[n].start; i < j->token[n].end; i++) {
        unsigned d = (unsigned)(j->text[i] - '0');
        if (d > 9 || value > (UINT64_MAX - d) / 10) return -1;
        value = value * 10 + d;
    }
    *out = value; return 0;
}
static int rs_id(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (unsigned i = 0; i < 64; i++) if (!strchr("0123456789abcdef", s[i])) return 0;
    return 1;
}
static int rs_path(const char *s)
{
    if (!*s || *s == '/' || strlen(s) > 240) return 0;
    const char *component = s;
    unsigned components = 0;
    for (const char *p = s;; p++) {
        unsigned char c = (unsigned char)*p;
        if (!c || c == '/') {
            if (++components > 8) return 0;
            size_t n = (size_t)(p - component);
            if (!n || component[n-1] == '.' || component[n-1] == ' ' ||
                (n == 1 && *component == '.') || (n == 2 && !memcmp(component, "..", 2))) return 0;
            char base[256]; size_t b = 0;
            while (b < n && component[b] != '.') { base[b] = (char)toupper((unsigned char)component[b]); b++; }
            base[b] = 0;
            if (!strcmp(base,"CON") || !strcmp(base,"PRN") || !strcmp(base,"AUX") || !strcmp(base,"NUL") ||
                (b == 4 && (!memcmp(base,"COM",3) || !memcmp(base,"LPT",3)) && base[3] >= '1' && base[3] <= '9')) return 0;
            if (!c) break;
            component = p + 1;
        } else if (c < 32 || c > 126 || strchr("\\:<>\"|?*", c)) return 0;
    }
    return 1;
}
static int rs_case_equal(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return !*a && !*b;
}
static int rs_digest(const char *base64, char hex[65])
{
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char digits[] = "0123456789abcdef";
    unsigned accum = 0, bits = 0, bytes = 0;
    if (strlen(base64) != 44 || base64[43] != '=') return -1;
    for (unsigned i = 0; i < 43; i++) {
        const char *at = strchr(chars, base64[i]); if (!at) return -1;
        accum = (accum << 6) | (unsigned)(at - chars); bits += 6;
        if (bits >= 8) { bits -= 8; unsigned v = (accum >> bits) & 255;
            if (bytes >= 32) return -1;
            hex[bytes*2] = digits[v>>4]; hex[bytes*2+1] = digits[v&15]; bytes++; }
    }
    hex[64] = 0; return bytes == 32 && !(accum & 3) ? 0 : -1;
}
static int rs_manifest_parse(struct rs_json *j, struct rs_manifest *m, const char *id)
{
    memset(m, 0, sizeof(*m));
    if (rs_string(j, rs_field(j,0,"id"), m->id, sizeof(m->id)) || !rs_id(m->id) || strcmp(m->id,id) ||
        !rs_equal(j,rs_field(j,0,"algorithm"),"sha256") ||
        rs_string(j,rs_field(j,0,"version"),m->version,sizeof(m->version))) return -1;
    int files = rs_field(j,0,"files"), pieces = rs_field(j,0,"pieces");
    if (files < 0 || j->token[files].type != '[' || j->token[files].count < 1 || j->token[files].count > RS_FILES ||
        pieces < 0 || !rs_equal(j,rs_field(j,pieces,"algorithm"),"sha256")) return -1;
    int padding = rs_field(j,pieces,"hashPadding");
    if (padding < 0 || j->token[padding].end - j->token[padding].start != 5 ||
        memcmp(j->text + j->token[padding].start,"false",5)) return -1;
    for (int n = files + 1; n < j->token[files].next; n = j->token[n].next) {
        struct rs_file *f = &m->file[m->files]; uint64_t attr;
        if (rs_string(j,rs_field(j,n,"name"),f->name,sizeof(f->name)) || !rs_path(f->name) ||
            rs_number(j,rs_field(j,n,"size"),&f->size) ||
            rs_number(j,rs_field(j,n,"attr"),&attr) || attr || f->size > RS_TOTAL_MAX - m->bytes) return -1;
        char first[256]; size_t first_bytes = strcspn(f->name,"/");
        memcpy(first,f->name,first_bytes);first[first_bytes]=0;
        if (rs_case_equal(first,"installer.exe") || rs_case_equal(first,".ssn-metafile.json") ||
            rs_case_equal(first,".ssn-lengths") || rs_case_equal(first,".runescape-installed") ||
            rs_case_equal(first,".runescape-installed.part")) return -1;
        for (unsigned i = 0; i < m->files; i++) {
            const char *a = m->file[i].name, *b = f->name;
            if (rs_case_equal(a,b)) return -1;
            size_t al = strlen(a), bl = strlen(b), min = al < bl ? al : bl;
            char prefix[256]; memcpy(prefix,b,min); prefix[min]=0;
            char other[256]; memcpy(other,a,min); other[min]=0;
            if (rs_case_equal(prefix,other) && ((al < bl && b[al]=='/') || (bl < al && a[bl]=='/'))) return -1;
        }
        m->bytes += f->size; m->files++;
    }
    int digests = rs_field(j,pieces,"digests");
    if (digests < 0 || j->token[digests].type != '[' || j->token[digests].count < 1 ||
        j->token[digests].count > RS_PIECES) return -1;
    for (int n = digests+1; n < j->token[digests].next; n = j->token[n].next) {
        char b64[45];
        if (rs_string(j,n,b64,sizeof(b64)) || rs_digest(b64,m->digest[m->pieces++])) return -1;
    }
    return m->bytes ? 0 : -1;
}
#endif
