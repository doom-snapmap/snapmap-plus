/* heap_out.h -- the tests' bridge from a heap result to a test's buffer.
 *
 * The product hands every name, path and cause back whole, malloc'd
 * (char **out, or a returned char *). A test that reads the text with strstr
 * or strcmp keeps a buffer of its own: HO(buf, call) runs the call with
 * HO_P(0) (and HO_P(1) for a second result, HO2) as the out parameters and
 * copies each result into its buffer. A result longer than the test's buffer
 * ends the test (reported, then abort), never cut. HOS(buf, call) copies a
 * returned malloc'd string the same way and is 1 when there was one. */
#ifndef SNAPMAP_PLUS_HEAP_OUT_H
#define SNAPMAP_PLUS_HEAP_OUT_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __declspec(thread) char *g_ho[3];   /* per thread: tests call from several */

static intptr_t ho_reset(void)
{
    free(g_ho[0]);
    free(g_ho[1]);
    free(g_ho[2]);
    g_ho[0] = g_ho[1] = g_ho[2] = NULL;
    return 0;
}

static void ho_copy(char *buf, size_t cap, const char *text, const char *where)
{
    size_t n = text ? strlen(text) : 0;
    if (!buf || !cap) return;
    if (n >= cap) {
        fprintf(stderr, "%s: a %u-byte result does not fit the test's %u-byte buffer\n", where, (unsigned)n, (unsigned)cap);
        fflush(stderr);
        abort();   /* a test buffer too small for the whole result is the test's failure */
    }
    if (n) memcpy(buf, text, n);
    buf[n] = 0;
}

static intptr_t ho_take(char *buf, size_t cap, int slot, intptr_t r)
{
    ho_copy(buf, cap, g_ho[slot], "heap_out");
    free(g_ho[slot]);
    g_ho[slot] = NULL;
    return r;
}

static int ho_str(char *buf, size_t cap, char *text)
{
    ho_copy(buf, cap, text, "heap_out");
    if (!text) { if (buf && cap) buf[0] = 0; return 0; }
    free(text);
    return 1;
}

/* A stub's result: *out set to a malloc'd copy (the product frees it). */
static void ho_set(char **out, const char *text)
{
    if (!out) return;
    free(*out);
    *out = text ? _strdup(text) : NULL;
}

static void ho_setf(char **out, const char *fmt, ...)
{
    va_list ap;
    int n;
    char *text;
    if (!out) return;
    va_start(ap, fmt);
    n = _vscprintf(fmt, ap);
    va_end(ap);
    text = n < 0 ? NULL : (char *)malloc((size_t)n + 1);
    if (text) {
        va_start(ap, fmt);
        vsprintf_s(text, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    free(*out);
    *out = text;
}

/* A test's own buffer for a result it reads back. */
#define HO_TEXT 4096

#define HO_P(i) (&g_ho[i])
#define HO(buf, call) ((int)ho_take((buf), sizeof(buf), 0, (ho_reset(), (intptr_t)(call))))
#define HOZ(buf, call) ((size_t)ho_take((buf), sizeof(buf), 0, (ho_reset(), (intptr_t)(call))))
#define HOC(buf, cap, call) ((int)ho_take((buf), (cap), 0, (ho_reset(), (intptr_t)(call))))
#define HO2(b0, b1, call) ((int)ho_take((b0), sizeof(b0), 0, ho_take((b1), sizeof(b1), 1, (ho_reset(), (intptr_t)(call)))))
#define HO3(b0, b1, b2, call) ((int)ho_take((b0), sizeof(b0), 0, ho_take((b1), sizeof(b1), 1, ho_take((b2), sizeof(b2), 2, (ho_reset(), (intptr_t)(call))))))
#define HOT(type, buf, call) ((type)ho_take((buf), sizeof(buf), 0, (ho_reset(), (intptr_t)(call))))
#define HOV2(b0, b1, call) ((void)ho_take((b0), sizeof(b0), 0, ho_take((b1), sizeof(b1), 1, (ho_reset(), (call), (intptr_t)0))))
#define HOS(buf, call) ho_str((buf), sizeof(buf), (call))

#endif /* SNAPMAP_PLUS_HEAP_OUT_H */
