/* Heap text sized to its content.
 *
 * Names, paths and sentences that carry them have no length of their own
 * (DECISIONS 26): they are formatted into a buffer allocated for exactly what
 * they hold, never into a fixed array that would cut them. Header-only, so
 * every target and test links it without a source line. */
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

/* Copy src whole into out[cap]. Returns 1, or 0 with out untouched when it
 * does not fit: a caller with a fixed buffer refuses rather than cuts. */
static __inline int sh_text_fits(char *out, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (!out || n + 1 > cap) return 0;
    if (n) memcpy(out, src, n);
    out[n] = 0;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_TEXT_H */
