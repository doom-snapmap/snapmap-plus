/* Cooked md6mesh dependency reader: envelope walking, refusals and the exact
 * identities a mesh names. Byte layout mirrors the shipped format. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md6_binary.h"

static unsigned char g_file[8192];
static size_t g_used;

static void raw(const void *bytes, size_t length)
{
    assert(g_used + length <= sizeof(g_file));
    memcpy(g_file + g_used, bytes, length); g_used += length;
}
static void magic(void) { raw("\x2b\x02\x4d\x4d", 4); }
static void u32be(unsigned value)
{
    unsigned char bytes[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16),
        (unsigned char)(value >> 8), (unsigned char)value};
    raw(bytes, 4);
}
static void u16be(unsigned value)
{
    unsigned char bytes[2] = {(unsigned char)(value >> 8), (unsigned char)value};
    raw(bytes, 2);
}
static void u8v(unsigned value) { unsigned char byte = (unsigned char)value; raw(&byte, 1); }
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

/* One mesh entry: name, material, then the geometry the reader must skip. */
static void mesh(const char *mesh_name, const char *material, unsigned vertices,
    unsigned triangles, int morph, int wide, unsigned blends)
{
    name(mesh_name); name(material);
    u8v(1); u32be(1); u32be(vertices); u32be(triangles);
    zeros(24); zeros((size_t)vertices * 0x30); zeros((size_t)triangles * 6);
    u32be(0); u32be(0); u32be(0);
    u8v(morph ? 1 : 0);
    if (morph) { u8v(wide ? 1 : 0); zeros((size_t)(wide ? 8 : 4) * vertices); }
    u32be(blends);
    if (blends && !morph) zeros((size_t)blends * vertices * 8);
}

static size_t g_skeleton_at, g_shapes_at;

static void head(const char *skeleton, unsigned shapes, const char *shape)
{
    g_used = 0;
    magic(); zeros(8);
    g_skeleton_at = g_used; name(skeleton);
    zeros(24); u8v(0); name(""); u16be(0); zeros(24);
    g_shapes_at = g_used; u32be(shapes);
    for (unsigned i = 0; i < shapes; i++) name(shape);
    zeros(36);
}

typedef struct seen { char type[32], name[128]; } seen;
static seen g_seen[16];
static size_t g_seen_count;
static int g_refuse_at = -1;

static int visit(void *context, const char *type, const char *value)
{
    assert(context == (void *)0x1234);
    assert(g_seen_count < 16);
    snprintf(g_seen[g_seen_count].type, sizeof(g_seen[0].type), "%s", type);
    snprintf(g_seen[g_seen_count].name, sizeof(g_seen[0].name), "%s", value);
    g_seen_count++;
    return g_refuse_at < 0 || (int)g_seen_count != g_refuse_at + 1;
}

static int read_file(size_t length, char *error, size_t capacity)
{
    g_seen_count = 0; error[0] = 0;
    return sh_md6_mesh_references(g_file, length, visit, (void *)0x1234, error, capacity);
}

static void expect(size_t index, const char *type, const char *value)
{
    assert(index < g_seen_count);
    assert(!strcmp(g_seen[index].type, type) && !strcmp(g_seen[index].name, value));
}

int main(void)
{
    char error[256];
    size_t complete;
    /* A complete mesh: the skeleton it loads, one material per mesh and the
     * material record table the engine builds its bindings from. */
    head("md6/boss/boss.md6skl", 1, "openShape");
    u32be(2);
    mesh("body", "art/boss/body", 2, 1, 0, 0, 0);
    mesh("horns", "art/boss/horns", 1, 0, 1, 1, 0);
    u32be(2);
    name("art/boss/body"); u32be(0); u32be(0); u32be(1);
    name("art/boss/horns"); u32be(1); u32be(0); u32be(0);
    magic();
    complete = g_used;
    assert(read_file(complete, error, sizeof(error)));
    assert(g_seen_count == 5);
    expect(0, "skeleton", "md6/boss/boss.md6skl");
    expect(1, "material", "art/boss/body");
    expect(2, "material", "art/boss/horns");
    expect(3, "material", "art/boss/body");
    expect(4, "material", "art/boss/horns");

    /* Every truncation is refused and reported; none is read as a short file. */
    for (size_t at = 1; at < complete; at++) {
        assert(!read_file(at, error, sizeof(error)));
        assert(error[0]);
    }
    /* Trailing bytes are not a valid mesh either: the footer must end the file. */
    assert(g_used + 1 <= sizeof(g_file));
    g_file[g_used] = 0;
    assert(!read_file(complete + 1, error, sizeof(error)) && strstr(error, "trailing"));

    /* A refused visit stops the walk at that reference. */
    g_refuse_at = 0;
    assert(!read_file(complete, error, sizeof(error)) && g_seen_count == 1 && strstr(error, "refused"));
    g_refuse_at = 2;
    assert(!read_file(complete, error, sizeof(error)) && g_seen_count == 3);
    g_refuse_at = -1;

    /* Blend shapes without morph normals carry their own payload; with morph
     * normals present the count is a tail field only. */
    head("rig", 0, "");
    u32be(1); mesh("body", "mat", 2, 1, 0, 0, 3); u32be(0); magic();
    assert(read_file(g_used, error, sizeof(error)) && g_seen_count == 2);
    head("rig", 0, "");
    u32be(1); mesh("body", "mat", 2, 1, 1, 0, 3); u32be(0); magic();
    assert(read_file(g_used, error, sizeof(error)) && g_seen_count == 2);

    /* An unnamed skeleton or material is absence, not an empty identity. */
    head("", 0, "");
    u32be(1); mesh("body", "", 0, 0, 0, 0, 0); u32be(0); magic();
    assert(read_file(g_used, error, sizeof(error)) && !g_seen_count);

    /* Magic is required at both ends. */
    head("rig", 0, ""); u32be(0); u32be(0); magic();
    g_file[1] = 0x03;
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "not a cooked md6 mesh"));
    head("rig", 0, ""); u32be(0); u32be(0); magic();
    g_file[g_used - 1] = 0x4c;
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "footer"));

    /* Implausible counts are refused before any large read is attempted. */
    head("rig", 0, ""); u32be(0); u32be(0); magic();
    g_file[g_shapes_at] = 0x02;   /* big-endian shape count 0x02000000 */
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "shape count"));
    head("rig", 0, "");
    u32be(1); name("body"); name("mat"); u8v(1); u32be(1); u32be(0x40000000u); u32be(0);
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "vertex count"));
    head("rig", 0, "");
    u32be(1); name("body"); name("mat"); u8v(1); u32be(1); u32be(1); u32be(0x40000000u);
    zeros(24); zeros(0x30);
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "triangle count"));
    head("rig", 0, "");
    u32be(1); name("body"); name("mat"); u8v(1); u32be(1); u32be(1); u32be(0);
    zeros(24); zeros(0x30); u32be(0); u32be(0); u32be(0); u8v(0); u32be(0x20000000u);
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "blend shape count"));

    /* A name length beyond the cap is refused rather than allocated. */
    head("rig", 0, "");
    g_file[g_skeleton_at + 2] = 0x02;   /* little-endian length 0x20000 */
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "name length"));
    /* A name is text: an embedded NUL is not an identity. */
    head("rig", 0, ""); g_file[g_skeleton_at + 5] = 0;
    assert(!read_file(g_used, error, sizeof(error)) && strstr(error, "not text"));

    assert(!sh_md6_mesh_references(NULL, 0, visit, NULL, error, sizeof(error)) && error[0]);
    assert(!sh_md6_mesh_references(g_file, g_used, NULL, NULL, error, sizeof(error)));

    /* A cooked animation: the md6 header family, then the skeleton it plays on. */
    g_used = 0; raw("\x26\x02\x41\x4d", 4); zeros(8); name("md6/boss/boss.md6skl"); zeros(64);
    g_seen_count = 0; error[0] = 0;
    assert(sh_md6_anim_references(g_file, g_used, visit, (void *)0x1234, error, sizeof(error)));
    assert(g_seen_count == 1);
    expect(0, "skeleton", "md6/boss/boss.md6skl");
    /* A per-skeleton default clip carries a placeholder, not a path: reading it
     * succeeds and names nothing, so the caller uses the identity instead. */
    g_used = 0; raw("\x26\x02\x41\x4d", 4); zeros(8); name("_defaulted_"); zeros(32);
    g_seen_count = 0;
    assert(sh_md6_anim_references(g_file, g_used, visit, (void *)0x1234, error, sizeof(error)));
    assert(!g_seen_count);
    /* Wrong magic, truncation and a refused visit are all reported. */
    g_used = 0; raw("\x26\x02\x4d\x4d", 4); zeros(8); name("md6/boss/boss.md6skl");
    g_seen_count = 0;
    assert(!sh_md6_anim_references(g_file, g_used, visit, (void *)0x1234, error, sizeof(error)));
    assert(strstr(error, "not a cooked md6 animation"));
    g_used = 0; raw("\x26\x02\x41\x4d", 4); zeros(8); name("md6/boss/boss.md6skl");
    for (size_t at = 1; at < g_used; at++) {
        g_seen_count = 0; error[0] = 0;
        assert(!sh_md6_anim_references(g_file, at, visit, (void *)0x1234, error, sizeof(error)) || !g_seen_count);
    }
    g_used = 0; raw("\x26\x02\x41\x4d", 4); zeros(8); name("md6/boss/boss.md6skl"); zeros(16);
    g_refuse_at = 0; g_seen_count = 0;
    assert(!sh_md6_anim_references(g_file, g_used, visit, (void *)0x1234, error, sizeof(error)));
    assert(g_seen_count == 1 && strstr(error, "refused"));
    g_refuse_at = -1;
    assert(!sh_md6_anim_references(NULL, 0, visit, NULL, error, sizeof(error)) && error[0]);
    assert(!sh_md6_anim_references(g_file, g_used, NULL, NULL, error, sizeof(error)));
    puts("md6 binary reader tests passed"); return 0;
}
