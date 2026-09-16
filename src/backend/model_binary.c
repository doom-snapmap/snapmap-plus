#include "model_binary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The current revision plus the two the engine calls BMODEL_MAGIC_PREVIOUS.
 * All three share this layout: the three shipped older-revision models parse
 * with the same walk, and the writer emits the file's own magic as its surface
 * terminator, so the value read from the header is the one to match. */
static const unsigned char g_model_magic[3][4] = {
    {0x1b, 0x4c, 0x4d, 0x42}, {0x1a, 0x4c, 0x4d, 0x42}, {0x19, 0x4c, 0x4d, 0x42}
};

/* Returns the count of big-endian fields between a surface name and its material
 * count, which is the one way the previous revisions differ: the current one
 * writes three, the previous two write two. Verified against the only shipped
 * non-empty previous-revision model; the other two carry no surfaces at all.
 * Returns 0 when the word is not a cooked-model magic at all. */
static unsigned mb_surface_fields(const unsigned char *at)
{
    if (!memcmp(at, g_model_magic[0], 4)) return 3;
    for (size_t i = 1; i < sizeof(g_model_magic) / sizeof(g_model_magic[0]); i++)
        if (!memcmp(at, g_model_magic[i], 4)) return 2;
    return 0;
}
#define MB_NAME_CAP      0x10000u
#define MB_SURFACE_CAP   262144u
#define MB_TRAILING_CAP  65536u

typedef struct mb_reader {
    const unsigned char *body;
    size_t length, at, scanned, budget;
    char *error;
    size_t capacity;
    unsigned surface_fields;
    int failed;
} mb_reader;

static int mb_fail(mb_reader *r, const char *reason)
{
    if (!r->failed && r->error && r->capacity)
        snprintf(r->error, r->capacity, "%s at offset %zu", reason, r->at);
    r->failed = 1; return 0;
}

static const unsigned char *mb_raw(mb_reader *r, size_t at, size_t count)
{
    if (count > r->length - at) return NULL;
    return r->body + at;
}

static unsigned mb_be32(mb_reader *r, size_t *at)
{
    const unsigned char *p = mb_raw(r, *at, 4);
    if (!p) { mb_fail(r, "cooked model is truncated"); return 0; }
    *at += 4;
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

/* idStr: little-endian length, then that many bytes with no terminator. */
static const unsigned char *mb_name(mb_reader *r, size_t *at, unsigned *length)
{
    const unsigned char *p = mb_raw(r, *at, 4);
    unsigned n;
    if (!p) return NULL;
    n = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
    if (n > MB_NAME_CAP) return NULL;
    p = mb_raw(r, *at + 4, n);
    if (!p) return NULL;
    *at += 4 + n; *length = n; return p;
}

static int mb_emit(mb_reader *r, sh_model_binary_visitor visitor, void *context,
    const unsigned char *bytes, unsigned length)
{
    char *name;
    int ok;
    if (!length) return 1;                      /* an unnamed surface binds nothing */
    name = malloc((size_t)length + 1);
    if (!name) return mb_fail(r, "cannot allocate a cooked model name");
    memcpy(name, bytes, length); name[length] = 0;
    if (strlen(name) != length) { free(name); return mb_fail(r, "cooked model name is not text"); }
    ok = visitor(context, "material", name);
    free(name);
    return ok ? 1 : mb_fail(r, "cooked model reference was refused");
}

/* One surface header: name, three fields, then its material table. Returns the
 * offset just past the table, or 0 when the header is malformed. */
static size_t mb_surface_header(mb_reader *r, size_t at, sh_model_binary_visitor visitor, void *context)
{
    unsigned materials, length, i;
    size_t fields = (size_t)r->surface_fields * 4u;
    if (!mb_name(r, &at, &length)) return 0;
    if (!mb_raw(r, at, fields)) return 0;
    at += fields;
    materials = mb_be32(r, &at);
    if (r->failed || materials > MB_SURFACE_CAP) { r->failed = 0; return 0; }
    for (i = 0; i < materials; i++) {
        const unsigned char *name = mb_name(r, &at, &length);
        if (!name) return 0;
        if (visitor && !mb_emit(r, visitor, context, name, length)) return 0;
    }
    return at;
}

/* The trailing table is what proves a candidate terminator was the real one. */
static int mb_trailer(mb_reader *r, size_t at)
{
    unsigned trailing, length, i;
    size_t cursor = at;
    trailing = mb_be32(r, &cursor);
    if (r->failed || trailing > MB_TRAILING_CAP) { r->failed = 0; return 0; }
    for (i = 0; i < trailing; i++) {
        if (!mb_name(r, &cursor, &length)) return 0;
        if (!mb_raw(r, cursor, 12)) return 0;
        cursor += 12;
    }
    return cursor == r->length;
}

int sh_model_references(const unsigned char *body, size_t length,
    sh_model_binary_visitor visitor, void *context, char *error, size_t capacity)
{
    mb_reader r = {body, length, 0, 0, 0, error, capacity, 0, 0};
    size_t *start = NULL, *scan = NULL, at = 0;
    unsigned surfaces, i;
    long long depth = 0;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (!body || !visitor) return mb_fail(&r, "cooked model inspection needs a source and a visitor");
    if (!mb_raw(&r, 0, 12)) return mb_fail(&r, "cooked model is truncated");
    r.surface_fields = mb_surface_fields(body);
    if (!r.surface_fields) return mb_fail(&r, "not a cooked model");
    at = 4;
    mb_be32(&r, &at);                           /* stamp */
    surfaces = mb_be32(&r, &at);
    if (r.failed) return 0;
    if (surfaces > MB_SURFACE_CAP) return mb_fail(&r, "implausible cooked model surface count");
    /* Bound the terminator search so a malformed file cannot cost more than a
     * few passes over its own bytes. */
    r.budget = length * 4u + 0x10000u;
    start = calloc((size_t)surfaces + 1, sizeof(*start));
    scan = calloc((size_t)surfaces + 1, sizeof(*scan));
    if (!start || !scan) { free(start); free(scan); return mb_fail(&r, "cooked model walk allocation failed"); }
    start[0] = at;
    while (depth >= 0) {
        size_t body_end, cursor;
        if ((unsigned)depth == surfaces) {
            if (mb_trailer(&r, start[depth])) { ok = 1; break; }
            depth--; continue;
        }
        body_end = mb_surface_header(&r, start[depth], NULL, NULL);
        if (r.failed) break;
        if (!body_end) { depth--; continue; }
        cursor = scan[depth] > body_end ? scan[depth] : body_end;
        while (cursor + 4 <= r.length && memcmp(r.body + cursor, body, 4)) {
            cursor++;
            if (++r.scanned > r.budget) { mb_fail(&r, "cooked model surface walk did not converge"); break; }
        }
        if (r.failed) break;
        if (cursor + 4 > r.length) { depth--; continue; }
        scan[depth] = cursor + 1;
        start[depth + 1] = cursor + 4; scan[depth + 1] = 0;
        depth++;
    }
    /* Report identities only from the accepted walk: a rejected candidate's
     * material table must not become a dependency. */
    if (ok) for (i = 0; i < surfaces && ok; i++)
        ok = mb_surface_header(&r, start[i], visitor, context) != 0;
    free(start); free(scan);
    if (!ok && !r.failed) mb_fail(&r, "cooked model surfaces could not be resolved");
    return ok && !r.failed;
}
