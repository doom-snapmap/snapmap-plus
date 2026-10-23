/* Native image exclusion state: what is certified, what keeps its native classification, unknown
 * states, and the native resource-list lock (image_exclusion.h, R-309). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "image_exclusion.h"

static int g_failed;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

/* A resource list: type +0x08, next +0x18, items +0x20, count +0x28. */
typedef struct fake_list { unsigned char bytes[0x40]; void *items[16]; } fake_list;
typedef struct fake_image { unsigned char bytes[0x100]; } fake_image;
static fake_list g_material, g_images;
static void *g_head, *g_live_head;
static int g_lock_object, g_held, g_acquires, g_releases, g_acquire_fails;

/* The list is reachable only while the native lock is held, as a mutation would make it unsafe
 * otherwise: an unlocked read finds no list and so cannot certify anything. */
static void *fake_lock(void) { return &g_lock_object; }
static unsigned fake_acquire(void *lock, char blocking)
{
    CHECK(lock == &g_lock_object); CHECK(blocking == 1);
    if (g_acquire_fails) return 0;
    g_held++; g_acquires++; g_head = g_live_head; return 1;
}
static void fake_release(void *lock)
{ CHECK(lock == &g_lock_object); CHECK(g_held > 0); if (!--g_held) g_head = NULL; g_releases++; }

static void list_init(fake_list *list, const char *type, fake_list *next)
{
    memset(list, 0, sizeof(*list));
    *(const char **)(list->bytes + 0x08) = type;
    *(void **)(list->bytes + 0x18) = next;
    *(void ***)(list->bytes + 0x20) = list->items;
}
static void list_set(fake_list *list, int count) { *(int *)(list->bytes + 0x28) = count; }
/* The renderer whose idImage layout the fixtures are built with and the query reads (R-389). */
static int g_opengl;
static sh_image_exclusion_layout layout_of(int opengl)
{ sh_image_exclusion_layout l = {0}; CHECK(sh_image_exclusion_layout_of(!opengl, &l)); return l; }
enum { PLACEHOLDER, LOADED, DEFAULTED, GENERATED };
/* As idImage leaves an object: the exclusion branch (PLACEHOLDER: fixed option block, constructor
 * sentinel), a binary read (LOADED: header dword at +0x38, file timestamp at +0x3c, the header's
 * options), a failed load (DEFAULTED), or an image generated with explicit options (GENERATED;
 * never through GenericLoad). */
static void image_init(fake_image *image, const char *name, int kind)
{
    memset(image, 0, sizeof(*image));
    *(const char **)(image->bytes + 0x08) = name;
    image->bytes[0x24] = kind == GENERATED ? 0x16 : 0x17;
    image->bytes[0x2c] = kind == DEFAULTED ? 0x14 : 0x10;
    uint32_t low = kind == LOADED ? 0x57871d0au : 0xffffffffu, high = kind == LOADED ? 0u : 0xffffffffu;
    memcpy(image->bytes + 0x38, &low, 4); memcpy(image->bytes + 0x3c, &high, 4);
    *(int *)(image->bytes + 0x58) = kind == LOADED ? 5 : 3;
    *(int *)(image->bytes + 0x60) = kind == LOADED ? 1152 : 8;
    *(int *)(image->bytes + 0x64) = kind == LOADED ? 1152 : 8;
    sh_image_exclusion_layout l = layout_of(g_opengl);
    *(int *)(image->bytes + l.format) = 5;
    *(int *)(image->bytes + l.usage) = kind == LOADED ? 4 : 0;
    *(uint32_t *)(image->bytes + l.bias) = 0x41500000u;
}
static void bind(int main_thread)
{
    sh_image_exclusion_test_native native = {(void *const *)&g_head, fake_lock, fake_acquire, fake_release, main_thread,
        g_opengl};
    sh_image_exclusion_test_bind(&native);
}
/* A bimage header: first dword, magic, type, width, height (big-endian). */
static void header(unsigned char out[24], uint32_t first, uint32_t width, uint32_t height)
{
    memset(out, 0, 24);
    for (int i = 0; i < 4; i++) {
        out[i] = (unsigned char)(first >> (24 - 8 * i));
        out[12 + i] = (unsigned char)(width >> (24 - 8 * i));
        out[16 + i] = (unsigned char)(height >> (24 - 8 * i));
    }
    memcpy(out + 4, "\x07MIB", 4);
}

static void run(int opengl)
{
    g_opengl = opengl; g_held = g_acquires = g_releases = g_acquire_fails = 0;
    sh_image_exclusion_test_bind(NULL);
    static fake_image korataki, korataki_again, korataki_loaded, square, small, small_ones, blocked_loaded,
        blocked_default, generated, engine, probe;
    unsigned char normal[24], probe_header[24], ones_8x8[24], plain_8x8[24];
    header(normal, 0x56c75717u, 1152, 1152);
    header(probe_header, 0xffffffffu, 128, 128);    /* the installed environment probes' shape */
    header(ones_8x8, 0xffffffffu, 8, 8);           /* a valid image a binary read leaves ambiguous */
    header(plain_8x8, 0x12345678u, 8, 8);
    list_init(&g_images, "image", NULL);
    list_init(&g_material, "material", &g_images);
    g_live_head = &g_material;
    const char *kname = "fonts/korataki_rg/64_df.tga$borderclamp$alpha";
    image_init(&korataki, kname, PLACEHOLDER);
    image_init(&square, "fonts/square721_ex_tl/64_df.tga$borderclamp$alpha", LOADED);
    /* A legitimate small image read from its binary: real 8 x 8, state 3, header timestamp. */
    image_init(&small, "textures/small_8x8", LOADED);
    *(int *)(small.bytes + 0x58) = 3; *(int *)(small.bytes + 0x60) = 8; *(int *)(small.bytes + 0x64) = 8;
    /* Two histories under one installed 8 x 8 header whose first dword is all ones, with the
     * placeholder's option words. small_ones_read was read from a source with an ordinary
     * Timestamp(), which the binary path wrote at +0x3c; small_ones has the exclusion branch's exact
     * state, which a read from a source whose Timestamp() is all ones would also leave. */
    static fake_image small_ones_read;
    image_init(&small_ones_read, "textures/ones_8x8_read", PLACEHOLDER);
    *(uint32_t *)(small_ones_read.bytes + 0x3c) = 0;
    image_init(&small_ones, "textures/ones_8x8", PLACEHOLDER);
    image_init(&blocked_loaded, "fonts/korataki_rg/48.tga", LOADED);
    image_init(&blocked_default, "fonts/korataki_rg/12.tga", DEFAULTED);
    image_init(&generated, "generated/swf/watermark.bimage", GENERATED);
    image_init(&engine, "_black", PLACEHOLDER);
    image_init(&probe, "env/default_hdr_prefiltered.bimage", PLACEHOLDER);
    void *items[] = {&korataki, &square, &small, &small_ones, &blocked_loaded, &blocked_default, &generated,
        &engine, &probe, &small_ones_read};
    for (int i = 0; i < 10; i++) g_images.items[i] = items[i];
    list_set(&g_images, 10);

    /* Unknown is never usable: unbound, another thread, a refused lock, an implausible registry. */
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == -1);
    bind(0);
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == -1 && g_acquires == 0);
    bind(1);
    g_acquire_fails = 1;
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == -1);
    g_acquire_fails = 0;
    list_set(&g_images, -1);
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == -1);
    list_set(&g_images, 10);
    CHECK(g_held == 0 && g_acquires == g_releases);

    /* The exclusion branch's object is certified; the name is case-insensitive, as the native list. */
    CHECK(sh_image_exclusion_query("FONTS/KORATAKI_RG/64_df.tga$borderclamp$alpha", normal, sizeof(normal)) == 1);
    CHECK(sh_image_exclusion_query(kname, NULL, 0) == 1);           /* no installed binary to read */
    /* Everything else keeps its native classification. */
    CHECK(sh_image_exclusion_query("fonts/square721_ex_tl/64_df.tga$borderclamp$alpha", normal, sizeof(normal)) == 0);
    CHECK(sh_image_exclusion_query("textures/small_8x8", plain_8x8, sizeof(plain_8x8)) == 0);
    /* Under the ambiguous header: the read history is proven by its timestamp and stays usable; the
     * exclusion-state history is not certified either way. */
    CHECK(sh_image_exclusion_query("textures/ones_8x8_read", ones_8x8, sizeof(ones_8x8)) == 0);
    CHECK(sh_image_exclusion_query("textures/ones_8x8", ones_8x8, sizeof(ones_8x8)) == -1);
    /* With a header that cannot produce the state, the same object is the exclusion branch's. */
    CHECK(sh_image_exclusion_query("textures/ones_8x8", plain_8x8, sizeof(plain_8x8)) == 1);
    CHECK(sh_image_exclusion_query("fonts/korataki_rg/48.tga", normal, sizeof(normal)) == 0);
    CHECK(sh_image_exclusion_query("fonts/korataki_rg/12.tga", normal, sizeof(normal)) == 0);
    CHECK(sh_image_exclusion_query("generated/swf/watermark.bimage", normal, sizeof(normal)) == 0);
    CHECK(sh_image_exclusion_query("_black", normal, sizeof(normal)) == 0);
    CHECK(sh_image_exclusion_query("not/registered", normal, sizeof(normal)) == 0);
    /* The same placeholder state with an installed header that could produce it is unknown; an
     * all-ones header of another size cannot produce 8 x 8, so the probe is certified. */
    CHECK(sh_image_exclusion_query(kname, ones_8x8, sizeof(ones_8x8)) == -1);
    CHECK(sh_image_exclusion_query("env/default_hdr_prefiltered.bimage", probe_header, sizeof(probe_header)) == 1);
    CHECK(sh_image_exclusion_header_ambiguous(ones_8x8, sizeof(ones_8x8)));
    CHECK(!sh_image_exclusion_header_ambiguous(ones_8x8, 19));
    CHECK(!sh_image_exclusion_header_ambiguous(probe_header, sizeof(probe_header)));
    CHECK(!sh_image_exclusion_header_ambiguous(plain_8x8, sizeof(plain_8x8)));
    /* An option word away from the exclusion branch's fixed block is not its object. */
    sh_image_exclusion_layout l = layout_of(opengl);
    *(int *)(korataki.bytes + l.usage) = 4;
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == 0);
    *(int *)(korataki.bytes + l.usage) = 0;
    /* The other renderer's layout of the same placeholder is not this renderer's exclusion object: e72a8ea's
     * OpenGL query read Vulkan's words (+0x90, +0xb0) where OpenGL's block holds 0 and answered 0 (R-389). */
    {
        static fake_image other;
        sh_image_exclusion_layout x = layout_of(!opengl);
        memcpy(&other, &korataki, sizeof(other));
        *(int *)(other.bytes + l.format) = 0; *(uint32_t *)(other.bytes + l.bias) = 0;
        *(int *)(other.bytes + x.format) = 5; *(int *)(other.bytes + x.usage) = 0;
        *(uint32_t *)(other.bytes + x.bias) = 0x41500000u;
        g_images.items[0] = &other;
        CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == 0);
        g_images.items[0] = &korataki;
        CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == 1);
    }

    /* Repeated unregister / reload: the answer follows the current registered object only. */
    g_images.items[0] = &small_ones_read; list_set(&g_images, 9);             /* unregistered */
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == 0);
    image_init(&korataki_loaded, kname, LOADED);                               /* reloaded, gate open */
    g_images.items[9] = &korataki_loaded; list_set(&g_images, 10);
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == 0);
    image_init(&korataki_again, kname, PLACEHOLDER);                           /* reloaded, gate closed */
    g_images.items[9] = &korataki_again;
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == 1);
    /* The same storage under another name certifies only by that name's own state and header. */
    *(const char **)(korataki_again.bytes + 8) = "textures/reused";
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == 0);
    CHECK(sh_image_exclusion_query("textures/reused", ones_8x8, sizeof(ones_8x8)) == -1);
    CHECK(sh_image_exclusion_query("textures/reused", normal, sizeof(normal)) == 1);

    /* Every registry read happened under the native lock, balanced. */
    CHECK(g_held == 0 && g_acquires == g_releases && g_acquires > 0);
    /* The lock is taken only from a verified purge driver: other bytes do not decode. */
    {
        static unsigned char junk[0x70];
        sh_image_exclusion_native decoded;
        CHECK(!sh_image_exclusion_decode(NULL, &decoded));
        CHECK(!sh_image_exclusion_decode(junk, &decoded) && !decoded.lock && !decoded.head);
    }
    sh_image_exclusion_test_bind(NULL);
    CHECK(sh_image_exclusion_query(kname, normal, sizeof(normal)) == -1);
}

int main(void)
{
    /* The two layouts as read from the executables (R-389), and no layout for an unknown renderer. */
    sh_image_exclusion_layout l;
    CHECK(sh_image_exclusion_layout_of(1, &l) && l.format == 0x90 && l.usage == 0x94 && l.bias == 0xb0);
    CHECK(sh_image_exclusion_layout_of(0, &l) && l.format == 0x8c && l.usage == 0x90 && l.bias == 0xac);
    CHECK(!sh_image_exclusion_layout_of(-1, &l) && !sh_image_exclusion_layout_of(2, &l));
    run(0);
    run(1);
    if (g_failed) { printf("image_exclusion_test: %d failure(s)\n", g_failed); return 1; }
    printf("image_exclusion_test: all checks passed\n");
    return 0;
}
