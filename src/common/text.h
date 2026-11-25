/* Heap text sized to its content: names and paths are formatted into a buffer allocated for exactly what
 * they hold, never a fixed array that would cut them. Header-only. */
#ifndef SNAPMAP_PLUS_TEXT_H
#define SNAPMAP_PLUS_TEXT_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

static __inline char sh_path_fold(char c)
{
    if (c == '\\') return '/';
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static __inline int sh_path_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && sh_path_fold(*a) == sh_path_fold(*b)) { ++a; ++b; }
    return !*a && !*b;
}

/* Return the path suffix following a case- and slash-insensitive prefix. */
static __inline const char *sh_path_after_prefix(const char *path, const char *prefix)
{
    if (!path) return NULL;
    for (; *prefix; ++path, ++prefix)
        if (sh_path_fold(*path) != sh_path_fold(*prefix)) return NULL;
    return path;
}

static __inline unsigned long long sh_text_hash(const char *s, size_t n)
{
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

typedef struct sh_text_buf { char *p; size_t n, cap; int oom; } sh_text_buf;

static __inline void sh_text_append(sh_text_buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (n >= (size_t)-1 - b->n) { b->oom = 1; return; }
    size_t need = b->n + n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < need && cap <= (size_t)-1 / 2) cap *= 2;
        if (cap < need) cap = need;
        char *p = (char *)realloc(b->p, cap);
        if (!p) { b->oom = 1; return; }
        b->p = p;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static __inline void sh_text_append_str(sh_text_buf *b, const char *s)
{
    sh_text_append(b, s, strlen(s));
}

/* Uppercase, space-separated bytes for a bounded diagnostic. */
static __inline void sh_hex_bytes(const unsigned char *bytes, size_t n, char *out, size_t cap)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t used = 0;
    for (size_t i = 0; i < n && used + 3 < cap; i++) {
        out[used++] = digits[bytes[i] >> 4];
        out[used++] = digits[bytes[i] & 15];
        out[used++] = ' ';
    }
    if (cap) out[used ? used - 1 : 0] = 0;
}

/* The formatted text, malloc'd (free); NULL when memory runs out. */
static __inline char *sh_vtextf(const char *fmt, va_list ap)
{
    va_list copy;
    int n;
    char *out;
    va_copy(copy, ap);
    n = _vscprintf(fmt, copy);
    va_end(copy);
    if (n < 0) return NULL;
    out = (char *)malloc((size_t)n + 1);
    if (!out) return NULL;
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    return out;
}

static __inline char *sh_textf(const char *fmt, ...)
{
    va_list ap;
    char *out;
    va_start(ap, fmt);
    out = sh_vtextf(fmt, ap);
    va_end(ap);
    return out;
}

/* The formatted wide text, malloc'd (free); NULL when memory runs out. */
static __inline wchar_t *sh_wtextf(const wchar_t *fmt, ...)
{
    va_list ap, copy;
    int n;
    wchar_t *out;
    va_start(ap, fmt);
    va_copy(copy, ap);
    n = _vscwprintf(fmt, copy);
    va_end(copy);
    out = n < 0 ? NULL : (wchar_t *)malloc(sizeof(wchar_t) * ((size_t)n + 1));
    if (out) vswprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

/* A copy of the first n bytes, NUL-terminated (free). */
static __inline char *sh_textn(const char *s, size_t n)
{
    if (n == (size_t)-1) return NULL;
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    if (n) memcpy(out, s, n);
    out[n] = 0;
    return out;
}

static __inline char *sh_text(const char *s)
{
    return s ? sh_textn(s, strlen(s)) : NULL;
}

/* Replace *dst with the formatted text (the old text freed). Returns 0 when
 * memory runs out, leaving *dst as it was. */
static __inline int sh_text_set(char **dst, const char *fmt, ...)
{
    va_list ap;
    char *out;
    va_start(ap, fmt);
    out = sh_vtextf(fmt, ap);
    va_end(ap);
    if (!out) return 0;
    free(*dst);
    *dst = out;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_TEXT_H */
