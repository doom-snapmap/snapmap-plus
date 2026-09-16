#include "md6_binary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "MM" 0x2b024d4d as it appears on disk, at the head and as the footer. */
static const unsigned char g_md6_mesh_magic[4] = {0x2b, 0x02, 0x4d, 0x4d};
#define MD6_MID_TRAILER 36u
#define MD6_VERTEX      0x30u
#define MD6_NAME_CAP    0x10000u

typedef struct md6_reader {
    const unsigned char *body;
    size_t length, at;
    char *error;
    size_t capacity;
    int failed;
} md6_reader;

static int md6_fail(md6_reader *r, const char *reason)
{
    if (!r->failed && r->error && r->capacity)
        snprintf(r->error, r->capacity, "%s at offset %zu", reason, r->at);
    r->failed = 1; return 0;
}

static const unsigned char *md6_raw(md6_reader *r, size_t count)
{
    const unsigned char *at;
    if (r->failed) return NULL;
    if (count > r->length - r->at) { md6_fail(r, "cooked md6 mesh is truncated"); return NULL; }
    at = r->body + r->at; r->at += count; return at;
}

static unsigned md6_u32be(md6_reader *r)
{
    const unsigned char *at = md6_raw(r, 4);
    if (!at) return 0;
    return ((unsigned)at[0] << 24) | ((unsigned)at[1] << 16) | ((unsigned)at[2] << 8) | at[3];
}

static unsigned md6_u16be(md6_reader *r)
{
    const unsigned char *at = md6_raw(r, 2);
    if (!at) return 0;
    return ((unsigned)at[0] << 8) | at[1];
}

/* idStr: little-endian length, then that many bytes with no terminator. The
 * returned copy is owned by the caller; an empty name yields an empty string. */
static char *md6_name(md6_reader *r)
{
    const unsigned char *at = md6_raw(r, 4);
    unsigned length;
    char *copy;
    if (!at) return NULL;
    length = (unsigned)at[0] | ((unsigned)at[1] << 8) | ((unsigned)at[2] << 16) | ((unsigned)at[3] << 24);
    if (length > MD6_NAME_CAP) { md6_fail(r, "implausible cooked md6 name length"); return NULL; }
    at = md6_raw(r, length);
    if (!at) return NULL;
    copy = malloc((size_t)length + 1);
    if (!copy) { md6_fail(r, "cannot allocate a cooked md6 name"); return NULL; }
    memcpy(copy, at, length); copy[length] = 0;
    /* A name with an embedded NUL is not an identity the engine could resolve. */
    if (strlen(copy) != length) { free(copy); md6_fail(r, "cooked md6 name is not text"); return NULL; }
    return copy;
}

static int md6_emit(md6_reader *r, sh_md6_binary_visitor visitor, void *context,
    const char *type, char *name)
{
    int ok;
    if (!name) return 0;
    ok = !*name || visitor(context, type, name);
    free(name);
    if (!ok) md6_fail(r, "cooked md6 reference was refused");
    return ok;
}

int sh_md6_mesh_references(const unsigned char *body, size_t length,
    sh_md6_binary_visitor visitor, void *context, char *error, size_t capacity)
{
    md6_reader r = {body, length, 0, error, capacity, 0};
    const unsigned char *magic;
    unsigned shapes, meshes, records, i;
    if (error && capacity) error[0] = 0;
    if (!body || !visitor) return md6_fail(&r, "cooked md6 mesh inspection needs a source and a visitor");
    magic = md6_raw(&r, 4);
    if (!magic) return 0;
    if (memcmp(magic, g_md6_mesh_magic, 4)) return md6_fail(&r, "not a cooked md6 mesh");
    if (!md6_raw(&r, 8)) return 0;                                  /* model + skeleton timestamps */
    if (!md6_emit(&r, visitor, context, "skeleton", md6_name(&r))) return 0;
    if (!md6_raw(&r, 24)) return 0;                                 /* bounds */
    if (!md6_raw(&r, 1)) return 0;                                  /* remap for skinning */
    { char *aux = md6_name(&r); if (!aux) return 0; free(aux); }     /* auxiliary name, unused */
    { unsigned remap = md6_u16be(&r); if (r.failed || !md6_raw(&r, remap)) return 0; }
    if (!md6_raw(&r, 24)) return 0;                                 /* second bounds */
    shapes = md6_u32be(&r);
    if (r.failed) return 0;
    if (shapes > 256) return md6_fail(&r, "implausible cooked md6 shape count");
    for (i = 0; i < shapes; i++) { char *name = md6_name(&r); if (!name) return 0; free(name); }
    if (!md6_raw(&r, MD6_MID_TRAILER)) return 0;
    meshes = md6_u32be(&r);
    if (r.failed) return 0;
    for (i = 0; i < meshes; i++) {
        unsigned vertices, triangles, morph;
        { char *name = md6_name(&r); if (!name) return 0; free(name); }
        if (!md6_emit(&r, visitor, context, "material", md6_name(&r))) return 0;
        if (!md6_raw(&r, 1)) return 0;                              /* flag */
        md6_u32be(&r);                                              /* texture coordinate sets */
        vertices = md6_u32be(&r); triangles = md6_u32be(&r);
        if (r.failed) return 0;
        if (vertices > (r.length - r.at) / MD6_VERTEX) return md6_fail(&r, "cooked md6 vertex count exceeds the file");
        if (!md6_raw(&r, 24) || !md6_raw(&r, (size_t)vertices * MD6_VERTEX)) return 0;
        if (triangles > (r.length - r.at) / 6u) return md6_fail(&r, "cooked md6 triangle count exceeds the file");
        if (!md6_raw(&r, (size_t)triangles * 6u)) return 0;
        if (!md6_raw(&r, 12)) return 0;                             /* joint offset, joints, checksum */
        morph = md6_raw(&r, 1) ? r.body[r.at - 1] : 0;
        if (r.failed) return 0;
        if (morph) {
            unsigned wide = md6_raw(&r, 1) ? r.body[r.at - 1] : 0;
            if (r.failed || !md6_raw(&r, (size_t)(wide ? 8u : 4u) * vertices)) return 0;
        }
        { /* Blend shapes follow only when the mesh carries no morph normals. */
            unsigned blends = md6_u32be(&r);
            if (r.failed) return 0;
            if (blends && !morph) {
                if (vertices && blends > (r.length - r.at) / ((size_t)vertices * 8u))
                    return md6_fail(&r, "cooked md6 blend shape count exceeds the file");
                if (!md6_raw(&r, (size_t)blends * vertices * 8u)) return 0;
            }
        }
    }
    records = md6_u32be(&r);
    if (r.failed) return 0;
    for (i = 0; i < records; i++) {
        if (!md6_emit(&r, visitor, context, "material", md6_name(&r))) return 0;
        if (!md6_raw(&r, 12)) return 0;                             /* index, first, last */
    }
    magic = md6_raw(&r, 4);
    if (!magic) return 0;
    if (memcmp(magic, g_md6_mesh_magic, 4)) return md6_fail(&r, "cooked md6 mesh footer is missing");
    if (r.at != r.length) return md6_fail(&r, "cooked md6 mesh has unparsed trailing bytes");
    return 1;
}
