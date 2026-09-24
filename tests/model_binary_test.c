/* Cooked static-model dependency reader: envelope walking, the terminator
 * backtrack, refusals and the materials a model binds. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model_binary.h"
#include "resource_rebind.h"

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
/* Primary material, three fields, the patch table, then an opaque geometry block and the
 * magic that ends the surface. */
static unsigned g_surface_fields = 3;

static void surface(const char *surface_material, const char *const *materials, size_t count,
    const void *geometry, size_t geometry_length)
{
    name(surface_material);
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

static int bind_material(void *context, const char *family, const char *value, const char **out)
{
    static char buffer[256];
    int *mode = context;
    assert(!strcmp(family, "material"));
    if (*mode < 0) return -1;
    if (!*mode) return 0;
    snprintf(buffer, sizeof(buffer), "scope/%s", value); *out = buffer;
    return 1;
}

static void discrete_fixture(const char *prefix, const char *collision)
{
    char buffer[256];
    g_used = 0; be32(0x02024d44); be32(23); be32(0); zeros(24);
    be32(1); raw("mat/opaque", 10); zeros(0x600 - 10);
    be32(2); snprintf(buffer, sizeof(buffer), "%smat/table", prefix); name(buffer); name("");
    be32(1); zeros(0x30);
    be32(1); snprintf(buffer, sizeof(buffer), "%smat/surface", prefix); name(buffer);
    be32(0); be32(1); be32(3); be32(3); zeros(3 * 0x30); zeros(3 * 2);
    be32(1); snprintf(buffer, sizeof(buffer), "%smat/remap", prefix); name(buffer); zeros(16);
    unsigned char present = collision != NULL;
    raw(&present, 1); if (collision) name(collision);
}
typedef struct discrete_binding { char name[256]; size_t count; int mode; } discrete_binding;
static int bind_discrete(void *context, const char *family, const char *value, const char **replacement)
{
    discrete_binding *f = context;
    assert(!strcmp(family, f->count == 3 ? "cm" : "material")); f->count++;
    if (f->mode <= 0) return f->mode;
    snprintf(f->name, sizeof(f->name), "scope/%s", value); *replacement = f->name; return 1;
}
static void discrete_check(void)
{
    unsigned char original[8192], *out;
    char error[256]; size_t length, changed, before;
    discrete_binding f = {0}; f.mode = 1;
    discrete_fixture("", "collision/base");
    before = g_used; memcpy(original, g_file, before);
    out = sh_resource_rebind(SH_REBIND_DISCRETE_MODEL, original, before,
        bind_discrete, &f, &length, &changed, error, sizeof(error));
    assert(out && changed == 4 && f.count == 4);
    discrete_fixture("scope/", "scope/collision/base");
    assert(length == g_used && !memcmp(out, g_file, length)); free(out);
    f = (discrete_binding){0};
    out = sh_resource_rebind(SH_REBIND_DISCRETE_MODEL, original, before,
        bind_discrete, &f, &length, &changed, error, sizeof(error));
    assert(out && !changed && f.count == 4 && length == before && !memcmp(out, original, length)); free(out);
    f.mode = -1; f.count = 0;
    assert(!sh_resource_rebind(SH_REBIND_DISCRETE_MODEL, original, before,
        bind_discrete, &f, &length, &changed, error, sizeof(error)) && !length && !changed && f.count == 1);
    for (size_t i = 0; i < before; i++) {
        f = (discrete_binding){0};
        assert(!sh_resource_rebind(SH_REBIND_DISCRETE_MODEL, original, i,
            bind_discrete, &f, &length, &changed, error, sizeof(error)));
        assert(error[0] && !length && !changed && !f.count);
    }
    original[before] = 0; f = (discrete_binding){0};
    assert(!sh_resource_rebind(SH_REBIND_DISCRETE_MODEL, original, before + 1,
        bind_discrete, &f, &length, &changed, error, sizeof(error)) && !f.count);
    original[36] = 0xff;
    assert(!sh_resource_rebind(SH_REBIND_DISCRETE_MODEL, original, before,
        bind_discrete, &f, &length, &changed, error, sizeof(error)) && !f.count);
    discrete_fixture("", NULL); f = (discrete_binding){0};
    out = sh_resource_rebind(SH_REBIND_DISCRETE_MODEL, g_file, g_used,
        bind_discrete, &f, &length, &changed, error, sizeof(error));
    assert(out && !changed && f.count == 3 && length == g_used); free(out);
    static const char *cases[][2] = {
        {"MAPS/Area.Entities/Chunk.LWO", "scope/maps/area/chunk.bcm"},
        {"maps/area/megabreakable_1.LWO", "scope/maps/area/megabreakable_1.LWO"},
        {"maps/area/MEGABREAKABLE_1.LWO", "scope/maps/area/megabreakable_1.bcm"},
        {"maps/area", "scope/maps/area.bcm"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        discrete_fixture("", cases[i][0]); before = g_used; memcpy(original, g_file, before);
        f = (discrete_binding){0}; f.mode = 1;
        out = sh_resource_rebind(SH_REBIND_DISCRETE_MODEL, original, before,
            bind_discrete, &f, &length, &changed, error, sizeof(error));
        assert(out && changed == 4);
        discrete_fixture("scope/", cases[i][1]);
        assert(length == g_used && !memcmp(out, g_file, length)); free(out);
    }
}

static void binding_check(void)
{
    const char *patch[] = {"mat/patch"}, *trailing[] = {"mat/trailer"};
    const char *bound_patch[] = {"scope/mat/patch"}, *bound_trailing[] = {"scope/mat/trailer"};
    unsigned char original[8192], *out;
    size_t before, length, changed;
    char error[256];
    int mode = 1;
    head(1); surface("mat/primary", patch, 1, "mat/primary", 11); trailer(trailing, 1);
    before = g_used; memcpy(original, g_file, before);
    out = sh_resource_rebind(SH_REBIND_STATIC_MODEL, original, before,
        bind_material, &mode, &length, &changed, error, sizeof(error));
    assert(out && changed == 3 && !memcmp(original, g_file, before));
    head(1); surface("scope/mat/primary", bound_patch, 1, "mat/primary", 11); trailer(bound_trailing, 1);
    assert(length == g_used && !memcmp(out, g_file, length)); free(out);
    mode = 0;
    out = sh_resource_rebind(SH_REBIND_STATIC_MODEL, original, before,
        bind_material, &mode, &length, &changed, error, sizeof(error));
    assert(out && !changed && length == before && !memcmp(out, original, before)); free(out);
    mode = -1;
    assert(!sh_resource_rebind(SH_REBIND_STATIC_MODEL, original, before,
        bind_material, &mode, &length, &changed, error, sizeof(error)) && !length && !changed);
    mode = 1;
    assert(!sh_resource_rebind(SH_REBIND_STATIC_MODEL, original, before - 1,
        bind_material, &mode, &length, &changed, error, sizeof(error)) && !length && !changed);
}

static int particle_unexpected_reference(void *context, const char *family,
    const char *name, const char **replacement)
{
    (void)context; (void)family; (void)name; (void)replacement;
    assert(!"particle vertex bytes must not become resource references"); return -1;
}
static void particle_check(void)
{
    unsigned char body[12 + 48 * 4 + 1] = {1, 'L', 'M', 'P'};
    char error[256]; size_t length, changed, size = sizeof(body) - 1;
    body[4] = 0x12; body[5] = 0x34; body[6] = 0x56; body[7] = 0x78;
    body[11] = 4;
    memcpy(body + 12, "material/not-a-reference", 24);
    assert(sh_particle_model_read(body, size, error, sizeof(error)));
    unsigned char *out = sh_resource_rebind(SH_REBIND_STATIC_PARTICLE, body, size,
        particle_unexpected_reference, NULL, &length, &changed, error, sizeof(error));
    assert(out && length == size && !changed && !memcmp(out, body, size)); free(out);
    for (size_t i = 0; i < size; i++) {
        assert(!sh_resource_rebind(SH_REBIND_STATIC_PARTICLE, body, i,
            particle_unexpected_reference, NULL, &length, &changed, error, sizeof(error)));
        assert(error[0] && !length && !changed);
    }
    assert(!sh_particle_model_read(body, size + 1, error, sizeof(error)) && error[0]);
    body[8] = 0xff;
    assert(!sh_particle_model_read(body, size, error, sizeof(error)) && error[0]);
    body[8] = 0; body[11] = 5;
    assert(!sh_particle_model_read(body, size, error, sizeof(error)) && error[0]);
    body[11] = 0;
    assert(sh_particle_model_read(body, 12, error, sizeof(error)));
    body[0] = 2;
    assert(!sh_particle_model_read(body, 12, error, sizeof(error)) && error[0]);
    assert(!sh_particle_model_read(NULL, 0, error, sizeof(error)) && error[0]);
}

int main(void)
{
    particle_check();
    static const char *body_materials[] = {"art/box/body", "art/box/trim"};
    static const char *horn_materials[] = {"art/box/horn"};
    static const char *trailing[] = {"art/box/decal", ""};
    char error[256];
    size_t complete;

    binding_check();
    discrete_check();

    head(2);
    surface("body", body_materials, 2, "geometry bytes", 14);
    surface("horns", horn_materials, 1, "more geometry", 13);
    trailer(trailing, 2);
    complete = g_used;
    assert(read_file(complete, error, sizeof(error)));
    assert(g_seen_count == 6);
    assert(!strcmp(g_seen[0], "body") && !strcmp(g_seen[1], "art/box/body"));
    assert(!strcmp(g_seen[2], "art/box/trim") && !strcmp(g_seen[3], "horns"));
    assert(!strcmp(g_seen[4], "art/box/horn") && !strcmp(g_seen[5], "art/box/decal"));

    /* No patch table still binds the primary surface material. */
    head(1); surface("body", NULL, 0, "geometry", 8); trailer(NULL, 0);
    assert(read_file(g_used, error, sizeof(error)) && g_seen_count == 1 && !strcmp(g_seen[0], "body"));
    head(1); surface("", NULL, 0, "geometry", 8); trailer(NULL, 0);
    assert(read_file(g_used, error, sizeof(error)) && !g_seen_count);

    /* The geometry is opaque: a copy of the magic inside it must not end the
     * surface, because the remainder would not then parse to the exact end. */
    head(1);
    surface("body", horn_materials, 1, "aa\x1b\x4c\x4d\x42zz", 9);
    trailer(trailing, 1);
    assert(read_file(g_used, error, sizeof(error)));
    assert(g_seen_count == 3 && !strcmp(g_seen[0], "body") && !strcmp(g_seen[1], "art/box/horn") &&
        !strcmp(g_seen[2], "art/box/decal"));

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
    assert(g_seen_count == 3 && !strcmp(g_seen[1], "art/box/horn"));
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
