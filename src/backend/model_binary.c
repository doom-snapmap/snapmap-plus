#include "model_binary.h"
#include <stdint.h>
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
    sh_model_binary_site_visitor site;
} mb_reader;

static int mb_fail(mb_reader *r, const char *reason)
{
    if (!r->failed && r->error && r->capacity)
        snprintf(r->error, r->capacity, "%s at offset %zu", reason, r->at);
    r->failed = 1; return 0;
}

static const unsigned char *mb_raw(mb_reader *r, size_t at, size_t count)
{
    if (at > r->length || count > r->length - at) return NULL;
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
    ok = (!visitor || visitor(context, "material", name)) &&
        (!r->site || r->site(context, "material", name, (size_t)(bytes - r->body) - 4, (size_t)length + 4));
    free(name);
    return ok ? 1 : mb_fail(r, "cooked model reference was refused");
}

/* One surface header: name, three fields, then its material table. Returns the
 * offset just past the table, or 0 when the header is malformed. */
static size_t mb_surface_header(mb_reader *r, size_t at, sh_model_binary_visitor visitor, void *context)
{
    unsigned materials, length, i;
    size_t fields = (size_t)r->surface_fields * 4u;
    const unsigned char *primary = mb_name(r, &at, &length);
    if (!primary || ((visitor || r->site) && !mb_emit(r, visitor, context, primary, length))) return 0;
    if (!mb_raw(r, at, fields)) return 0;
    at += fields;
    materials = mb_be32(r, &at);
    if (r->failed || materials > MB_SURFACE_CAP) { r->failed = 0; return 0; }
    for (i = 0; i < materials; i++) {
        const unsigned char *name = mb_name(r, &at, &length);
        if (!name) return 0;
        if ((visitor || r->site) && !mb_emit(r, visitor, context, name, length)) return 0;
    }
    return at;
}

/* The trailing table is what proves a candidate terminator was the real one. */
static int mb_trailer(mb_reader *r, size_t at, sh_model_binary_visitor visitor, void *context)
{
    unsigned trailing, length, i;
    size_t cursor = at;
    trailing = mb_be32(r, &cursor);
    if (r->failed || trailing > MB_TRAILING_CAP) { r->failed = 0; return 0; }
    for (i = 0; i < trailing; i++) {
        const unsigned char *name = mb_name(r, &cursor, &length);
        if (!name || ((visitor || r->site) && !mb_emit(r, visitor, context, name, length))) return 0;
        if (!mb_raw(r, cursor, 12)) return 0;
        cursor += 12;
    }
    return cursor == r->length;
}

static int mb_references(const unsigned char *body, size_t length,
    sh_model_binary_visitor visitor, sh_model_binary_site_visitor site, void *context,
    char *error, size_t capacity)
{
    mb_reader r = {body, length, 0, 0, 0, error, capacity, 0, 0};
    size_t *start = NULL, *scan = NULL, at = 0;
    unsigned surfaces, i;
    long long depth = 0;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (!body || (!visitor && !site)) return mb_fail(&r, "cooked model inspection needs a source and a visitor");
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
    r.budget = length > (SIZE_MAX - 0x10000u) / 4u ? SIZE_MAX : length * 4u + 0x10000u;
    start = calloc((size_t)surfaces + 1, sizeof(*start));
    scan = calloc((size_t)surfaces + 1, sizeof(*scan));
    if (!start || !scan) { free(start); free(scan); return mb_fail(&r, "cooked model walk allocation failed"); }
    start[0] = at;
    while (depth >= 0) {
        size_t body_end, cursor;
        if ((unsigned)depth == surfaces) {
            if (mb_trailer(&r, start[depth], NULL, NULL)) { ok = 1; break; }
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
    r.site = site;
    if (ok) for (i = 0; i < surfaces && ok; i++)
        ok = mb_surface_header(&r, start[i], visitor, context) != 0;
    if (ok) ok = mb_trailer(&r, start[surfaces], visitor, context);
    free(start); free(scan);
    if (!ok && !r.failed) mb_fail(&r, "cooked model surfaces could not be resolved");
    return ok && !r.failed;
}

int sh_model_references(const unsigned char *body, size_t length,
    sh_model_binary_visitor visitor, void *context, char *error, size_t capacity)
{ return mb_references(body, length, visitor, NULL, context, error, capacity); }

int sh_model_reference_sites(const unsigned char *body, size_t length,
    sh_model_binary_site_visitor visitor, void *context, char *error, size_t capacity)
{ return mb_references(body, length, NULL, visitor, context, error, capacity); }

/* Discrete models have an explicit counted envelope. Bound each count by its
 * remaining physical bytes before iterating; there is no package-size quota. */
static int db_skip(mb_reader *r, size_t count, size_t stride)
{
    if (r->failed || r->at > r->length || (stride && count > (r->length - r->at) / stride))
        return mb_fail(r, "discrete model is truncated");
    r->at += count * stride; return 1;
}
static unsigned db_count(mb_reader *r, size_t minimum)
{
    unsigned count = mb_be32(r, &r->at);
    if (r->failed || count > INT32_MAX || count > (r->length - r->at) / minimum) {
        mb_fail(r, "invalid discrete model count"); return 0;
    }
    return count;
}
static int db_reference(mb_reader *r, const char *family, sh_model_binary_visitor visitor, void *context)
{
    size_t start = r->at;
    unsigned length = 0;
    const unsigned char *bytes = mb_name(r, &r->at, &length);
    if (!bytes) return mb_fail(r, "invalid discrete model reference");
    if (!length) return 1;
    if (memchr(bytes, 0, length)) return mb_fail(r, "discrete model name contains a null byte");
    if (!visitor && !r->site) return 1;
    char *name = malloc((size_t)length + 5);
    if (!name) return mb_fail(r, "cannot allocate discrete model reference");
    memcpy(name, bytes, length); name[length] = 0;
    /* The base-collision loader changes ordinary maps/... requests to .bcm,
     * lowercases them, then removes .entities fragments. Its megabreakable
     * exception is case-sensitive; requests outside maps/ stay as authored. */
    if (!strcmp(family, "cm") && !_strnicmp(name, "maps/", 5) && !strstr(name, "/megabreakable")) {
        char *end = name + strlen(name);
        while (end > name && end[-1] != '/' && end[-1] != '\\') {
            if (*--end == '.') { *end = 0; break; }
        }
        memcpy(name + strlen(name), ".bcm", 5);
        for (char *p = name; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c < 0xe0)) *p = (char)(c + 32);
        }
        char *fragment;
        while ((fragment = strstr(name, ".entities")) != NULL)
            memmove(fragment, fragment + 9, strlen(fragment + 9) + 1);
    }
    int ok = (!visitor || visitor(context, family, name)) &&
        (!r->site || r->site(context, family, name, start, r->at - start));
    free(name);
    return ok ? 1 : mb_fail(r, "discrete model reference was refused");
}
static int db_walk(mb_reader *r, sh_model_binary_visitor visitor, void *context)
{
    static const unsigned char magic[4] = {2, 2, 'M', 'D'};
    if (!mb_raw(r, 0, 4) || memcmp(r->body, magic, 4)) return mb_fail(r, "not a discrete model");
    if (!db_skip(r, 1, 36)) return 0; /* magic, stamp, flags and bounds */
    unsigned count = db_count(r, 0x600);
    if (!db_skip(r, count, 0x600)) return 0;
    count = db_count(r, 4);
    for (unsigned i = 0; i < count; i++) if (!db_reference(r, "material", visitor, context)) return 0;
    count = db_count(r, 0x30);
    if (!db_skip(r, count, 0x30)) return 0;
    count = db_count(r, 20);
    for (unsigned i = 0; i < count; i++) {
        if (!db_reference(r, "material", visitor, context) || !db_skip(r, 2, 4)) return 0;
        unsigned vertices = db_count(r, 0x30), indices = db_count(r, 2);
        if (!db_skip(r, vertices, 0x30) || !db_skip(r, indices, 2)) return 0;
    }
    count = db_count(r, 20);
    for (unsigned i = 0; i < count; i++)
        if (!db_reference(r, "material", visitor, context) || !db_skip(r, 4, 4)) return 0;
    if (!db_skip(r, 1, 1)) return 0;
    if (r->body[r->at - 1] && !db_reference(r, "cm", visitor, context)) return 0;
    if (r->at != r->length) return mb_fail(r, "discrete model has trailing bytes");
    return !r->failed;
}
static int db_references(const unsigned char *body, size_t length,
    sh_model_binary_visitor visitor, sh_model_binary_site_visitor site, void *context,
    char *error, size_t capacity)
{
    mb_reader r = {body, length, 0, 0, 0, error, capacity, 0, 0};
    if (error && capacity) *error = 0;
    if (!body || (!visitor && !site)) return mb_fail(&r, "discrete model needs source bytes and a visitor");
    /* As with static models, a malformed tail must not publish partial edges. */
    if (!db_walk(&r, NULL, NULL)) return 0;
    r.at = 0; r.site = site;
    return db_walk(&r, visitor, context);
}
int sh_discrete_model_references(const unsigned char *body, size_t length,
    sh_model_binary_visitor visitor, void *context, char *error, size_t capacity)
{ return db_references(body, length, visitor, NULL, context, error, capacity); }
int sh_discrete_model_reference_sites(const unsigned char *body, size_t length,
    sh_model_binary_site_visitor visitor, void *context, char *error, size_t capacity)
{ return db_references(body, length, NULL, visitor, context, error, capacity); }

int sh_particle_model_read(const unsigned char *body, size_t length, char *error, size_t capacity)
{
    mb_reader r = {body, length, 0, 0, 0, error, capacity, 0, 0};
    static const unsigned char magic[] = {1, 'L', 'M', 'P'};
    if (error && capacity) *error = 0;
    if (!body || length < 12) return mb_fail(&r, "static particle model is truncated");
    if (memcmp(body, magic, sizeof(magic))) return mb_fail(&r, "not a static particle model");
    r.at = 8;
    unsigned count = mb_be32(&r, &r.at);
    if (count > INT32_MAX || count > (length - r.at) / 48)
        return mb_fail(&r, "invalid static particle vertex count");
    r.at += (size_t)count * 48;
    if (r.at != length) return mb_fail(&r, "static particle model has trailing bytes");
    return 1;
}
