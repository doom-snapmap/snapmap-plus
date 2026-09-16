/* Cooked static-model dependency reader: envelope walking, the terminator
 * backtrack, refusals and the materials a model binds. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model_binary.h"

static unsigned char g_file[8192];
static size_t g_used;

static void raw(const void *bytes, size_t length)
{
    assert(g_used + length <= sizeof(g_file));
    memcpy(g_file + g_used, bytes, length); g_used += length;
}
/* The writer emits the file's own magic as each surface terminator. */
static unsigned g_revision = 0x1b;
static void magic(void)
{
    unsigned char bytes[4] = {(unsigned char)g_revision, 0x4c, 0x4d, 0x42};
    raw(bytes, 4);
}
static void be32(unsigned value)
{
    unsigned char bytes[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16),
        (unsigned char)(value >> 8), (unsigned char)value};
    raw(bytes, 4);
}
static void name(const char *text)
{
    size_t length = strlen(text);
    unsigned char header[4] = {(unsigned char)length, (unsigned char)(length >> 8),
        (unsigned char)(length >> 16), (unsigned char)(length >> 24)};
    raw(header, 4); raw(text, length);
}
static void zeros(size_t count)
{
    assert(g_used + count <= sizeof(g_file));
    memset(g_file + g_used, 0, count); g_used += count;
}
/* name, three fields, the material table, then an opaque geometry block and the
 * magic that ends the surface. */
static unsigned g_surface_fields = 3;

static void surface(const char *surface_name, const char *const *materials, size_t count,
    const void *geometry, size_t geometry_length)
{
    name(surface_name);
    for (unsigned i = 0; i < g_surface_fields; i++) be32(0);
    be32((unsigned)count);
    for (size_t i = 0; i < count; i++) name(materials[i]);
    if (geometry_length) raw(geometry, geometry_length);
    magic();
}
static void head(unsigned surfaces) { g_used = 0; g_surface_fields = 3; g_revision = 0x1b; magic(); be32(0x1234); be32(surfaces); }
/* The two revisions the engine calls BMODEL_MAGIC_PREVIOUS share this layout
 * with one fewer big-endian field in each surface header. */
static void previous_head(unsigned revision, unsigned surfaces)
{
    g_used = 0; g_surface_fields = 2; g_revision = revision;
    magic(); be32(0x1234); be32(surfaces);
}
static void trailer(const char *const *names, size_t count)
{
    be32((unsigned)count);
    for (size_t i = 0; i < count; i++) { name(names[i]); be32(0); be32(1); be32(2); }
}

static char g_seen[16][128];
static size_t g_seen_count;
static int g_refuse_at = -1;

static int visit(void *context, const char *type, const char *value)
{
    assert(context == (void *)0x99 && !strcmp(type, "material"));
    assert(g_seen_count < 16);
    snprintf(g_seen[g_seen_count], sizeof(g_seen[0]), "%s", value);
    g_seen_count++;
    return g_refuse_at < 0 || (int)g_seen_count != g_refuse_at + 1;
}

static int read_file(size_t length, char *error, size_t capacity)
{
    g_seen_count = 0; error[0] = 0;
    return sh_model_references(g_file, length, visit, (void *)0x99, error, capacity);
}

int main(void)
{
    static const char *body_materials[] = {"art/box/body", "art/box/trim"};
    static const char *horn_materials[] = {"art/box/horn"};
    static const char *trailing[] = {"art/box/decal", ""};
    char error[256];
    size_t complete;

    head(2);
    surface("body", body_materials, 2, "geometry bytes", 14);
    surface("horns", horn_materials, 1, "more geometry", 13);
    trailer(trailing, 2);
    complete = g_used;
    assert(read_file(complete, error, sizeof(error)));
    assert(g_seen_count == 3);
    assert(!strcmp(g_seen[0], "art/box/body") && !strcmp(g_seen[1], "art/box/trim"));
    assert(!strcmp(g_seen[2], "art/box/horn"));

    /* A model with no material table binds nothing and still parses. */
    head(1); surface("body", NULL, 0, "geometry", 8); trailer(NULL, 0);
    assert(read_file(g_used, error, sizeof(error)) && !g_seen_count);

    /* The geometry is opaque: a copy of the magic inside it must not end the
     * surface, because the remainder would not then parse to the exact end. */
    head(1);
    surface("body", horn_materials, 1, "aa\x1b\x4c\x4d\x42zz", 9);
    trailer(trailing, 1);
    assert(read_file(g_used, error, sizeof(error)));
    assert(g_seen_count == 1 && !strcmp(g_seen[0], "art/box/horn"));

    /* Every truncation is refused and reported. */
    head(2);
    surface("body", body_materials, 2, "geometry bytes", 14);
    surface("horns", horn_materials, 1, "more geometry", 13);
    trailer(trailing, 2);
    for (size_t at = 1; at < complete; at++) {
        assert(!read_file(at, error, sizeof(error)));
        assert(error[0]);
    }
    /* Trailing bytes are not a valid model: the trailer must end the file. */
    g_file[g_used] = 0;
    assert(!read_file(complete + 1, error, sizeof(error)) && error[0]);

    /* A refused visit stops the walk. */
    g_refuse_at = 1;
    assert(!read_file(complete, error, sizeof(error)) && g_seen_count == 2 && strstr(error, "refused"));
    g_refuse_at = -1;

    /* Both previous revisions parse, with their own surface header width. */
    previous_head(0x1a, 1); surface("body", horn_materials, 1, "geometry", 8); trailer(trailing, 1);
    assert(read_file(g_used, error, sizeof(error)));
    assert(g_seen_count == 1 && !strcmp(g_seen[0], "art/box/horn"));
    previous_head(0x19, 0); trailer(NULL, 0);
    assert(read_file(g_used, error, sizeof(error)) && !g_seen_count);
    /* A foreign container is declined, not guessed at. */
    head(1); surface("body", horn_materials, 1, "g", 1); trailer(NULL, 0);
    g_file[0] = 0x17;
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "not a cooked model"));

    /* Implausible counts are refused before any allocation or long scan. */
    head(1); surface("body", horn_materials, 1, "g", 1); trailer(NULL, 0);
    g_file[8] = 0x02;   /* big-endian surface count 0x02000000 */
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "surface count"));

    /* A surface whose terminator never appears is refused. */
    head(1);
    name("body"); be32(0); be32(0); be32(0); be32(1); name("art/box/horn");
    zeros(32);
    assert(!read_file(g_used, error, sizeof(error)) && error[0]);

    assert(!sh_model_references(NULL, 0, visit, NULL, error, sizeof(error)) && error[0]);
    assert(!sh_model_references(g_file, g_used, NULL, NULL, error, sizeof(error)));
    puts("cooked model reader tests passed"); return 0;
}
