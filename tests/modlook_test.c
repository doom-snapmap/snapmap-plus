/* modlook_test.c -- the Graphics SDK's look compiler (modlook.c, docs/graphics-sdk.md).
 *
 * Every field and default of both looks, the routes to the game's own programs, the two shared texture places and
 * what overflows them, each mistake's sentence, old-key materials (the four fixtures) against
 * sh_modprog_legacy_material, and determinism. The compiled text is read back the way the engine would: its keys, and
 * the parameter image's 28 vectors decoded from the image the text names.
 *
 *   modlook_test.exe [fixtures=<dir of tests/fixtures/modprog>] */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modlook.h"
#include "modprog.h"
#include "modbundle.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

void backend_log(const char *message) { (void)message; }

/* ------------------------------------------------------------------------------------------ helpers ---- */

typedef struct res { int r; char *out; size_t n; char why[400]; char *notes; } res;

static res compile(const char *name, const char *text)
{
    res x;
    memset(&x, 0, sizeof x);
    x.r = sh_look_compile_notes(name, text, strlen(text), &x.out, &x.n, x.why, sizeof x.why, &x.notes);
    return x;
}
static void release(res *x) { free(x->out); free(x->notes); memset(x, 0, sizeof *x); }

/* The value of the first line `key` (case-insensitive) of the text, as a static string; "" when there is none. */
static const char *value(const char *text, const char *key, int nth)
{
    static char t[600];
    size_t kn = strlen(key);
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n'), *q = p;
        size_t ln = e ? (size_t)(e - p) : strlen(p), n;
        while (ln && (*q == ' ' || *q == '\t')) { q++; ln--; }
        if (ln > kn && !_strnicmp(q, key, kn) && (q[kn] == ' ' || q[kn] == '\t') && nth-- == 0) {
            const char *v = q + kn;
            ln -= kn;
            while (ln && (*v == ' ' || *v == '\t')) { v++; ln--; }
            while (ln && (v[ln - 1] == '\r' || v[ln - 1] == ' ')) ln--;
            n = ln < sizeof t - 1 ? ln : sizeof t - 1;
            memcpy(t, v, n);
            t[n] = 0;
            return t;
        }
        p = e ? e + 1 : NULL;
    }
    t[0] = 0;
    return t;
}
static int count_key(const char *text, const char *key)
{
    int n = 0;
    while (value(text, key, n)[0]) n++;
    return n;
}
/* The value without its quotes. */
static const char *unq(const char *v)
{
    static char t[600];
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { memcpy(t, v + 1, n - 2); t[n - 2] = 0; return t; }
    snprintf(t, sizeof t, "%s", v);
    return t;
}
static int has(const char *s, const char *needle) { return s && strstr(s, needle) != NULL; }

static float half_float(unsigned h)
{
    int e = (int)((h >> 10) & 31), m = (int)(h & 1023);
    float v = e == 0 ? (float)m / 16777216.0f : e == 31 ? 65504.0f : ldexpf(1.0f + (float)m / 1024.0f, e - 15);
    return (h & 0x8000u) ? -v : v;
}

/* The registered image a material names (its path under generated/image/, as .bimage), its bytes, or NULL. */
static unsigned char *image_bytes(const char *name, size_t *n)
{
    char path[300], *dot;
    unsigned char *b = NULL;
    snprintf(path, sizeof path, "generated/image/%s", name);
    dot = strrchr(path, '.');
    if (!dot) return NULL;
    strcpy(dot, ".bimage");
    if (!sh_modprog_owns(path)) return NULL;
    if (sh_modprog_read(path, &b, n) != SH_MB_STOCK_OK) return NULL;
    return b;
}

#define IMG_DATA 62u    /* the .bimage header: 8 big-endian words, the 10-byte format, the mip's 5 words */
static void image_texel(const unsigned char *b, unsigned i, float out[4])
{
    unsigned k;
    for (k = 0; k < 4; k++) {
        const unsigned char *p = b + IMG_DATA + (size_t)i * 8 + k * 2;
        out[k] = half_float((unsigned)p[0] | (unsigned)p[1] << 8);
    }
}

/* The 28 vectors the compiled material's `layerparms` image holds. 1 when it names one. */
static int vectors(const char *text, float v[28][4])
{
    size_t n = 0;
    unsigned char *b = image_bytes(unq(value(text, "layerparms", 0)), &n);
    unsigned k;
    if (!b || n != IMG_DATA + 8u * 4 * 8) { if (b) HeapFree(GetProcessHeap(), 0, b); return 0; }
    for (k = 0; k < 28; k++) image_texel(b, k, v[k]);
    HeapFree(GetProcessHeap(), 0, b);
    return 1;
}

static int near4(const float a[4], float x, float y, float z, float w)
{
    return fabsf(a[0] - x) < 1e-3f && fabsf(a[1] - y) < 1e-3f && fabsf(a[2] - z) < 1e-3f && fabsf(a[3] - w) < 1e-3f;
}
static int vec_is(const char *text, const char *key, float x, float y, float z, float w)
{
    float a[4] = { 0, 0, 0, 0 };
    const char *v = value(text, key, 0), *p = strchr(v, '{');
    int k = 0;
    if (!p) return 0;
    for (p++; k < 4 && *p && *p != '}'; k++) {
        char *e;
        a[k] = strtof(p, &e);
        if (e == p) return 0;
        p = e;
        while (*p == ',' || *p == ' ') p++;
    }
    return k == 4 && near4(a, x, y, z, w);
}

/* ------------------------------------------------------------------------------------------ layered ---- */

static void test_layered_example(void)
{
    /* the guide's example */
    static const char T[] =
        "{\n\tlook\t\t\tlayered\n\twounds\t\t\tyes\n\t// the base surface\n\tdiffusemap\t\"mods/abcdefgh/body.tga\"\n"
        "\tlayermask\t\tmods/abcdefgh/demon/body_mask.tga\n"
        "\tlayer1\t\t\tmods/abcdefgh/tiles/pores.tga\n\tlayer1normal\t\tmods/abcdefgh/tiles/pores_n.tga\n"
        "\tlayer1height\t\tmods/abcdefgh/tiles/pores_h.tga\n\tlayer1repeat\t\t20\n"
        "\tlayer2\t\t\tmods/abcdefgh/tiles/rock.tga\n\tlayer2normal\t\tmods/abcdefgh/tiles/rock_n.tga\n"
        "\tlayer2repeat\t\t3\n\tlayer2weights\t\t{ 0.2, 0.5, 0, 0, 1 }\n"
        "\tlayer3\t\t\t{ 0.35, 0.05, 0.03 }\n\tburn\t\t\tmods/abcdefgh/tiles/burnt.tga\n\tburnheat\t\t{ 1, 0.07, 0 }\n}\n";
    res x = compile("mods/abcdefgh/body.decl", T);
    float v[28][4];
    CHECK(x.r == SH_LOOK_CHANGED && x.out && x.n == strlen(x.out));
    if (!x.out) return;
    CHECK(!strcmp(unq(value(x.out, "ambientprogram", 0)), "snapmapplus/layered/skinvmtrwounds"));
    CHECK(count_key(x.out, "ambientprogram") == 1 && count_key(x.out, "look") == 0 && count_key(x.out, "wounds") == 0);
    CHECK(!strcmp(value(x.out, "packedspec", 0), "0"));
    CHECK(!strcmp(value(x.out, "diffusemap", 0), "\"mods/abcdefgh/body.tga\""));      /* the base's keys pass through */
    CHECK(has(x.out, "// the base surface"));
    CHECK(!strcmp(unq(value(x.out, "layerblendmask", 0)), "mods/abcdefgh/demon/body_mask.tga"));
    CHECK(!strcmp(unq(value(x.out, "blendlayer1albedomap", 0)), "mods/abcdefgh/tiles/pores.tga"));
    CHECK(!strcmp(unq(value(x.out, "blendlayer1normalmap", 0)), "mods/abcdefgh/tiles/pores_n.tga"));
    CHECK(!strcmp(unq(value(x.out, "blendlayer1heightmap", 0)), "mods/abcdefgh/tiles/pores_h.tga"));
    CHECK(!strcmp(unq(value(x.out, "blendlayer2albedomap", 0)), "mods/abcdefgh/tiles/rock.tga"));
    CHECK(has(unq(value(x.out, "blendlayer3albedomap", 0)), "snapmapplus/look/c"));       /* a colour: a registered image */
    CHECK(has(unq(value(x.out, "blendlayer1smoothnessmap", 0)), "snapmapplus/look/c"));   /* the default 0.5 */
    CHECK(value(x.out, "blendlayer4albedomap", 0)[0] == 0);                                  /* no layer 4 */
    CHECK(!strcmp(unq(value(x.out, "multilayerextramap0", 0)), "mods/abcdefgh/tiles/burnt.tga"));
    CHECK(value(x.out, "multilayerextramap1", 0)[0] == 0);
    CHECK(vectors(x.out, v));
    CHECK(near4(v[0], 20, 3, 1, 1));
    CHECK(near4(v[4], 1, 0.2f, 1, 0) && near4(v[5], 1, 0.5f, 1, 0) && near4(v[6], 1, 0, 1, 0) && near4(v[7], 1, 0, 1, 0) &&
          near4(v[8], 1, 1, 1, 0));
    CHECK(near4(v[24], 1, 0, 1, 0) && near4(v[27], 1, 0.07f, 0, 0));
    CHECK(x.notes == NULL);
    release(&x);
}

static void test_layered_defaults(void)
{
    /* one layer and its mask: every other number is the guide's default */
    static const char T[] = "{\n\tlook layered\n\tlayermask m.tga\n\tlayer1 a.tga\n}\n";
    res x = compile("a.decl", T);
    float v[28][4];
    CHECK(x.r == SH_LOOK_CHANGED);
    if (!x.out) return;
    CHECK(!strcmp(unq(value(x.out, "ambientprogram", 0)), "snapmapplus/layered/skinvmtr"));
    CHECK(vectors(x.out, v));
    CHECK(near4(v[0], 1, 1, 1, 1) && near4(v[1], 1, 1, 1, 1) && near4(v[2], 1, 1, 1, 1) && near4(v[3], 0.25f, 0.25f, 0.25f, 0.25f));
    CHECK(near4(v[4], 1, 0, 0, 0) && near4(v[5], 1, 0, 0, 0) && near4(v[6], 1, 0, 0, 0) && near4(v[7], 1, 0, 0, 0) &&
          near4(v[8], 1, 0, 0, 0));
    CHECK(near4(v[9], 1, 0, 0.5f, 0) && near4(v[10], 1, 1, 0, 0) && near4(v[11], 1, 1, 1, 0));
    CHECK(near4(v[12], 1, 1, 0, 0) && near4(v[15], 1, 1, 0, 0));
    CHECK(near4(v[16], 0, 0, 0, 0) && near4(v[20], 0, 0, 0, 0));
    CHECK(near4(v[24], 0, 0, 0, 0) && near4(v[25], 4, 4, 0, 0) && near4(v[26], 1, 4, 0.7f, 0.4f) && near4(v[27], 1, 0.07f, 0, 0));
    CHECK(value(x.out, "blendlayer1normalmap", 0)[0] == 0 && value(x.out, "blendlayer1heightmap", 0)[0] == 0);
    CHECK(count_key(x.out, "multilayerextramap0") == 0);
    release(&x);
}

static void test_layered_fields(void)
{
    /* every numeric field at a value of its own */
    static const char T[] =
        "{\n\tlook layered\n\tlayermask m.tga\n"
        "\tlayer1 a.tga\n\tlayer1repeat 5\n\tlayer1tiling { 2, 3 }\n\tlayer1scroll { 0.5, 0.25 }\n\tlayer1coverage 0.8\n"
        "\tlayer1heightblend 0.6\n\tlayer1softness 0.1\n\tlayer1weights { 0.9, 0.8, 0.7, 0.6, 0.5 }\n"
        "\tlayer1specular { 0.1, 0.2, 0.3 }\n\tlayer1glow { 1, 0.5, 0.25 }\n"
        "\tlayer2 b.tga\n\tlayer2repeat 7\n\tlayer2specular s.tga\n\tlayer2glow g.tga\n"
        "\tlayer4normal n.tga\n\tlayer4 { 1, 0, 0 }\n"
        "\tbaseheight 0.7\n\tbaseheightblend 0.4\n\tbasetiling { 3, 4 }\n\tbasescroll { 0.1, 0.2 }\n\tbaseglow { 0.5, 0.6, 0.7 }\n"
        "\tburn t.tga\n\tburntiling { 6, 7 }\n\tburnnoise { 2, 3, 0.5, 0.2 }\n\tburnheat { 0.9, 0.1, 0.05 }\n}\n";
    res x = compile("a.decl", T);
    float v[28][4];
    CHECK(x.r == SH_LOOK_CHANGED);
    if (!x.out) return;
    CHECK(vectors(x.out, v));
    CHECK(near4(v[0], 5, 7, 1, 1) && near4(v[1], 0.8f, 1, 1, 1) && near4(v[2], 0.6f, 1, 1, 1) && near4(v[3], 0.1f, 0.25f, 0.25f, 0.25f));
    CHECK(near4(v[4], 0.9f, 1, 0, 1) && near4(v[5], 0.8f, 1, 0, 1) && near4(v[6], 0.7f, 1, 0, 1) && near4(v[7], 0.6f, 1, 0, 1) &&
          near4(v[8], 0.5f, 1, 0, 1));
    CHECK(near4(v[9], 1, 0, 0.7f, 0) == 0 && near4(v[9], 0.4f, 0, 0.7f, 0));
    CHECK(near4(v[10], 3, 4, 0.1f, 0.2f) && near4(v[11], 0.5f, 0.6f, 0.7f, 0));
    CHECK(near4(v[12], 2, 3, 0.5f, 0.25f) && near4(v[13], 1, 1, 0, 0));
    /* layer 1's colours; layer 2's specular and glow images in the places the burn's tile left (1: burn; 2: specular) */
    CHECK(near4(v[16], 0.1f, 0.2f, 0.3f, 0) && near4(v[20], 1, 0.5f, 0.25f, 0));
    CHECK(near4(v[17], 1, 1, 1, 2) && near4(v[21], 0, 0, 0, 0));     /* the glow image does not fit */
    CHECK(near4(v[24], 1, 0, 1, 0) && near4(v[25], 6, 7, 0, 0) && near4(v[26], 2, 3, 0.5f, 0.2f) && near4(v[27], 0.9f, 0.1f, 0.05f, 0));
    CHECK(!strcmp(unq(value(x.out, "multilayerextramap0", 0)), "t.tga") && !strcmp(unq(value(x.out, "multilayerextramap1", 0)), "s.tga"));
    CHECK(has(x.notes, "layer2glow") && has(x.notes, "a.decl"));
    /* a layer with no colour: the base colour stays; its other images are its own */
    CHECK(near4(v[4], 0.9f, 1, 0, 1));
    CHECK(!strcmp(unq(value(x.out, "blendlayer4normalmap", 0)), "n.tga") && has(unq(value(x.out, "blendlayer4albedomap", 0)), "snapmapplus/look/c"));
    CHECK(value(x.out, "blendlayer3albedomap", 0)[0] == 0 && value(x.out, "blendlayer3smoothnessmap", 0)[0] == 0);
    release(&x);
}

static void test_values_as_images(void)
{
    /* a value or a colour where an image is expected is a registered constant image of that colour */
    static const char T[] =
        "{\n\tlook layered\n\tlayermask m.tga\n\tlayer1 { 0.25, 0.5, 1 }\n\tlayer1smoothness 0.8\n\tlayer1height 0.3\n}\n";
    res x = compile("a.decl", T);
    size_t n = 0;
    unsigned char *b;
    float t[4];
    CHECK(x.r == SH_LOOK_CHANGED);
    if (!x.out) return;
    b = image_bytes(unq(value(x.out, "blendlayer1albedomap", 0)), &n);
    CHECK(b && n == IMG_DATA + 16u * 8);
    if (b) {
        image_texel(b, 0, t);
        CHECK(near4(t, 0.25f, 0.5f, 1, 1));
        image_texel(b, 15, t);
        CHECK(near4(t, 0.25f, 0.5f, 1, 1));
        HeapFree(GetProcessHeap(), 0, b);
    }
    b = image_bytes(unq(value(x.out, "blendlayer1smoothnessmap", 0)), &n);
    CHECK(b != NULL);
    if (b) { image_texel(b, 3, t); CHECK(near4(t, 0.8f, 0.8f, 0.8f, 1)); HeapFree(GetProcessHeap(), 0, b); }
    b = image_bytes(unq(value(x.out, "blendlayer1heightmap", 0)), &n);
    CHECK(b != NULL);
    if (b) { image_texel(b, 7, t); CHECK(near4(t, 0.3f, 0.3f, 0.3f, 1)); HeapFree(GetProcessHeap(), 0, b); }
    {
        const char *p = sh_modprog_register_const_image((const float[4]){ 0.25f, 0.5f, 1, 1 });
        CHECK(p && !strncmp(p, "generated/image/snapmapplus/look/c", 34) && strlen(p) == strlen("generated/image/snapmapplus/look/c") + 16 + 7);
        CHECK(p && has(x.out, p + strlen("generated/image/")) == 0);   /* the material names it as .tga, not .bimage */
        CHECK(p && sh_modprog_owns(p));
    }
    release(&x);
}

static void test_base_height(void)
{
    /* a base height image takes a shared place; a number is the constant */
    static const char T[] = "{\n\tlook layered\n\tlayermask m.tga\n\tlayer1 a.tga\n\tbaseheight h.tga\n}\n";
    res x = compile("a.decl", T);
    float v[28][4];
    CHECK(x.r == SH_LOOK_CHANGED && x.out);
    if (!x.out) return;
    CHECK(vectors(x.out, v) && near4(v[9], 1, 1, 0, 0));
    CHECK(!strcmp(unq(value(x.out, "multilayerextramap0", 0)), "h.tga"));
    release(&x);
}

static void test_pool(void)
{
    float v[28][4];
    res x;
    /* the order: burn, burnglow, burnblood, base height, then layers' specular and glow images */
    static const char A[] =
        "{\n\tlook layered\n\tlayermask m.tga\n\tlayer1 a.tga\n\tburn t.tga\n\tburnglow bg.tga\n\tburnblood bb.tga\n\tbaseheight h.tga\n"
        "\tlayer1specular s.tga\n}\n";
    x = compile("body.decl", A);
    CHECK(x.r == SH_LOOK_CHANGED && x.out);
    if (!x.out) return;
    CHECK(vectors(x.out, v));
    CHECK(near4(v[24], 1, 2, 1, 0));                           /* the blood does not fit: none */
    CHECK(near4(v[9], 1, 0, 0.5f, 0));                         /* the base height: flat */
    CHECK(near4(v[16], 0, 0, 0, 0));                           /* the specular: black */
    CHECK(near4(v[11], 1, 1, 1, 1));
    CHECK(!strcmp(unq(value(x.out, "multilayerextramap0", 0)), "t.tga") && !strcmp(unq(value(x.out, "multilayerextramap1", 0)), "bg.tga"));
    CHECK(x.notes && has(x.notes, "burnblood") && has(x.notes, "baseheight") && has(x.notes, "layer1specular") &&
          has(x.notes, "body.decl"));
    {
        int lines = 0;
        const char *p;
        for (p = x.notes; *p; p++) if (*p == '\n') lines++;
        CHECK(lines == 3);
    }
    release(&x);
    /* without a burn the pool begins at the base height, then layer 1's specular, glow ... in layer order */
    {
        static const char B[] =
            "{\n\tlook layered\n\tlayermask m.tga\n\tlayer1 a.tga\n\tlayer1glow g1.tga\n\tlayer1specular s1.tga\n\tlayer2 b.tga\n"
            "\tlayer2specular s2.tga\n\tbaseheight h.tga\n}\n";
        x = compile("body.decl", B);
        CHECK(x.r == SH_LOOK_CHANGED && x.out && vectors(x.out, v));
        if (x.out) {
            /* base height 1, layer 1's specular 2 (before its glow), the glow and layer 2's specular overflow */
            CHECK(near4(v[9], 1, 1, 0, 0) && near4(v[16], 1, 1, 1, 2) && near4(v[20], 0, 0, 0, 0) && near4(v[17], 0, 0, 0, 0));
            CHECK(has(x.notes, "layer1glow") && has(x.notes, "layer2specular"));
        }
        release(&x);
    }
}

static void test_stock_routes(void)
{
    res x;
    /* nothing a layer can show on: the game's own skinned program, the base keys only */
    x = compile("a.decl", "{\n\tlook layered\n\tdiffusemap \"d.tga\"\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && !strcmp(value(x.out, "ambientprogram", 0), "skinvmtr") && count_key(x.out, "layerparms") == 0 &&
          !strcmp(value(x.out, "diffusemap", 0), "\"d.tga\"") && count_key(x.out, "look") == 0);
    release(&x);
    x = compile("a.decl", "{\n\tlook layered\n\twounds yes\n\tlayer1repeat 3\n\tlayer1 a.tga\n}\n");     /* no mask: no layer shows */
    CHECK(x.r == SH_LOOK_CHANGED && x.out && !strcmp(value(x.out, "ambientprogram", 0), "skinvmtrwounds") && count_key(x.out, "layerparms") == 0 &&
          count_key(x.out, "blendlayer1albedomap") == 0 && count_key(x.out, "wounds") == 0 && count_key(x.out, "layer1") == 0);
    release(&x);
    x = compile("a.decl", "{\n\tlook layered\n\tlayermask m.tga\n\tlayer1repeat 3\n}\n");            /* a mask, no layer */
    CHECK(x.r == SH_LOOK_CHANGED && x.out && !strcmp(value(x.out, "ambientprogram", 0), "skinvmtr") && count_key(x.out, "layerblendmask") == 0);
    release(&x);
    /* a burn needs the platform program on its own */
    x = compile("a.decl", "{\n\tlook layered\n\tburn t.tga\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && has(value(x.out, "ambientprogram", 0), "snapmapplus/layered/skinvmtr") && count_key(x.out, "layerparms") == 1);
    release(&x);
    /* particles: the game's own shading for lit and unlit */
    x = compile("a.decl", "{\n\tlook particle\n\ttransmap \"t.tga\"\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && !strcmp(value(x.out, "stageprogram", 0), "transsortblend") && count_key(x.out, "particleuberparms1") == 0 &&
          !strcmp(value(x.out, "transmap", 0), "\"t.tga\""));
    release(&x);
    x = compile("a.decl", "{\n\tlook particle\n\tlighting lit\n\tglow 4\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && !strcmp(value(x.out, "stageprogram", 0), "particleshighq") && !strcmp(value(x.out, "emissive", 0), "4"));
    release(&x);
    /* a lit or explosion particle carries the light atlas program; an unlit one (as the game's unlit stock) does not */
    x = compile("a.decl", "{\n\tlook particle\n\tlighting lit\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && !strcmp(value(x.out, "particlelightatlasprogram", 0), "particlelightatlas") && count_key(x.out, "particlelightatlasprogram") == 1);
    release(&x);
    x = compile("a.decl", "{\n\tlook particle\n\tlighting lit\n\tlitscale 2\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && count_key(x.out, "particlelightatlasprogram") == 1);
    release(&x);
    x = compile("a.decl", "{\n\tlook particle\n\tlighting explosion\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && count_key(x.out, "particlelightatlasprogram") == 1);
    release(&x);
    x = compile("a.decl", "{\n\tlook particle\n\tlighting lit\n\tparticlelightatlasprogram \"mine\"\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && count_key(x.out, "particlelightatlasprogram") == 1 && !strcmp(value(x.out, "particlelightatlasprogram", 0), "\"mine\""));
    release(&x);
    x = compile("a.decl", "{\n\tlook particle\n\tnearfade { 40, 8 }\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && count_key(x.out, "particlelightatlasprogram") == 0);
    release(&x);
    /* each of these needs the platform program */
    {
        static const char *const need[] = { "glowmask m.tga", "gradient { 1, 0, 0, 0, 1, 0 }", "nearfade { 40, 8 }", "softdistance 5",
                                            "depthoffset 2", "lighting explosion" };
        size_t i;
        for (i = 0; i < sizeof need / sizeof need[0]; i++) {
            char t[200];
            snprintf(t, sizeof t, "{\n\tlook particle\n\t%s\n}\n", need[i]);
            x = compile("a.decl", t);
            CHECK(x.r == SH_LOOK_CHANGED && x.out && !strcmp(unq(value(x.out, "stageprogram", 0)), "snapmapplus/particles/uberparticles") &&
                  count_key(x.out, "particleuberparms1") == 1);
            release(&x);
        }
    }
}

/* ------------------------------------------------------------------------------------------ particle ---- */

static void test_particle_fields(void)
{
    static const char T[] =
        "{\n\tlook\t\t\tparticle\n\ttransmap\t\tmods/abcdefgh/fx/embers.tga\n\tlighting\t\tlit\n\tglow\t\t\t4\n"
        "\tglowmask\t\tmods/abcdefgh/fx/embers_mask.tga\n\tglowmaskcolour\t{ 1, 0.4, 0.1 }\n\tglowmaskstrength\t2\n"
        "\tnearfade\t\t{ 40, 8 }\n\tsoftdistance\t5\n\tdepthoffset\t1.5\n\tlitscale\t0.5\n\temissive\t3\n}\n";
    res x = compile("embers.decl", T);
    CHECK(x.r == SH_LOOK_CHANGED && x.out);
    if (!x.out) return;
    CHECK(!strcmp(unq(value(x.out, "stageprogram", 0)), "snapmapplus/particles/uberparticles"));
    /* particleuberparms1: z the mode (+ 16 with a glow mask), w the depth offset; 2: the mask colour, near fade begin;
     * 3: glow; emissive: the material's own x, near fade end, soft distance, lit scale; smokebrightness: the strength */
    CHECK(vec_is(x.out, "particleuberparms1", 0, 0, 17, 1.5f));
    CHECK(vec_is(x.out, "particleuberparms2", 1, 0.4f, 0.1f, 40));
    CHECK(vec_is(x.out, "particleuberparms3", 0, 0, 0, 4));
    CHECK(vec_is(x.out, "emissive", 3, 8, 5, 0.5f) && count_key(x.out, "emissive") == 1);
    CHECK(vec_is(x.out, "smokebrightness", 2, 0, 0, 0));
    CHECK(!strcmp(unq(value(x.out, "particleemissivemap", 0)), "mods/abcdefgh/fx/embers_mask.tga"));
    CHECK(!strcmp(unq(value(x.out, "transmap", 0)), "mods/abcdefgh/fx/embers.tga") || !strcmp(value(x.out, "transmap", 0), "mods/abcdefgh/fx/embers.tga"));
    CHECK(count_key(x.out, "look") == 0 && count_key(x.out, "glow") == 0 && count_key(x.out, "lighting") == 0);
    release(&x);
    /* the explosion: the gradient's colours, mode 4; unlit and defaults */
    x = compile("e.decl", "{\n\tlook particle\n\tlighting explosion\n\tgradient { 1, 0.1, 0, 0.5, 0.2, 0.1 }\n\tglow 10\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out);
    if (x.out) {
        CHECK(vec_is(x.out, "particleuberparms1", 0, 0, 4, 0) && vec_is(x.out, "particleuberparms2", 1, 0.1f, 0, 0) &&
              vec_is(x.out, "particleuberparms3", 0.5f, 0.2f, 0.1f, 10) && vec_is(x.out, "emissive", 1, 0, 0, 1) &&
              vec_is(x.out, "smokebrightness", 0, 0, 0, 0));
        CHECK(count_key(x.out, "particleemissivemap") == 0);
    }
    release(&x);
    x = compile("u.decl", "{\n\tlook particle\n\tsoftdistance 3\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out);
    if (x.out) {
        CHECK(vec_is(x.out, "particleuberparms1", 0, 0, 0, 0) && vec_is(x.out, "particleuberparms2", 1, 1, 1, 0) &&
              vec_is(x.out, "particleuberparms3", 0, 0, 0, 1) && vec_is(x.out, "emissive", 1, 0, 3, 1));
    }
    release(&x);
    /* a lit stage that scales the light or glows on its own needs the platform program too */
    x = compile("l.decl", "{\n\tlook particle\n\tlighting lit\n\tlitscale 2\n}\n");
    CHECK(x.r == SH_LOOK_CHANGED && x.out && count_key(x.out, "particleuberparms1") == 1);
    release(&x);
}

/* ------------------------------------------------------------------------------------------ mistakes ---- */

static void mistake(const char *text, const char *want)
{
    res x = compile("body.decl", text);
    CHECK(x.r == SH_LOOK_MISTAKE && x.out == NULL);
    if (strcmp(x.why, want)) fprintf(stderr, "  got:  %s\n  want: %s\n", x.why, want);
    CHECK(!strcmp(x.why, want));
    CHECK(!has(x.why, "limit") && !has(x.why, "too many") && !has(x.why, "budget") && !has(x.why, "slot") && !has(x.why, "program"));
    release(&x);
}

static void test_mistakes(void)
{
    /* the guide's three */
    mistake("{\n\tlook layered\n\twounds yes\n\n\tlayermask m.tga\n\tlayer1 a.tga\n\tlayer1repeat 2\n\tlayer5 b.tga\n}\n",
            "body.decl line 8: 'layer5' is not a field of a layered look; a surface has layers 1 to 4.");
    mistake("{\n\tlook layered\n\tlayer2repeat { 3, 4 }\n}\n", "body.decl line 3: 'layer2repeat' needs one number, as in 'layer2repeat 3'.");
    {
        res x = compile("mods/x/embers.decl", "{\n\tlook particle\n\ttransmap t.tga\n\tlighting glowing\n}\n");
        CHECK(x.r == SH_LOOK_MISTAKE && !strcmp(x.why, "embers.decl line 4: 'lighting' is 'glowing'; write unlit, lit or explosion."));
        release(&x);
    }
    mistake("{\n\tlook layered\n\tlayer0 a.tga\n}\n", "body.decl line 3: 'layer0' is not a field of a layered look; a surface has layers 1 to 4.");
    mistake("{\n\tlook layered\n\tlayer12normal a.tga\n}\n", "body.decl line 3: 'layer12normal' is not a field of a layered look; a surface has layers 1 to 4.");
    mistake("{\n\tlook layered\n\tlayer1nromal a.tga\n}\n", "body.decl line 3: 'layer1nromal' is not a field of a layered look; the nearest is 'layer1normal'.");
    mistake("{\n\tlook layered\n\tlayermsk a.tga\n}\n", "body.decl line 3: 'layermsk' is not a field of a layered look; the nearest is 'layermask'.");
    mistake("{\n\tlook layered\n\tburnheet { 1, 0, 0 }\n}\n", "body.decl line 3: 'burnheet' is not a field of a layered look; the nearest is 'burnheat'.");
    mistake("{\n\tlook layered\n\twounds maybe\n}\n", "body.decl line 3: 'wounds' is 'maybe'; write yes or no.");
    mistake("{\n\tlook layered\n\tlayer1tiling 3\n}\n", "body.decl line 3: 'layer1tiling' needs two numbers in braces, as in 'layer1tiling { 2, 2 }'.");
    mistake("{\n\tlook layered\n\tlayer1weights { 1, 1 }\n}\n", "body.decl line 3: 'layer1weights' needs five numbers in braces, as in 'layer1weights { 1, 1, 1, 1, 1 }'.");
    mistake("{\n\tlook layered\n\tlayermask 3\n}\n", "body.decl line 3: 'layermask' needs an image path, as in 'layermask mods/abcdefgh/demon/body_mask.tga'.");
    mistake("{\n\tlook layered\n\tlayer1 { 1, 2 }\n}\n", "body.decl line 3: 'layer1' needs an image path or three numbers in braces, as in 'layer1 { 0.35, 0.05, 0.03 }'.");
    mistake("{\n\tlook layered\n\tlayer1height { 1 }\n}\n", "body.decl line 3: 'layer1height' needs an image path or one number, as in 'layer1height 0.5'.");
    mistake("{\n\tlook layered\n\tlayer1repeat\n}\n", "body.decl line 3: 'layer1repeat' needs one number, as in 'layer1repeat 3'.");
    mistake("{\n\tlook layered\n\tburnnoise { 1, 2, 3 }\n}\n", "body.decl line 3: 'burnnoise' needs four numbers in braces, as in 'burnnoise { 1, 4, 0.7, 0.4 }'.");
    mistake("{\n\tlook layered\n\tlighting lit\n}\n", "body.decl line 3: 'lighting' is not a field of a layered look; it belongs to a particle look.");
    mistake("{\n\tlook particle\n\tlayer1 a.tga\n}\n", "body.decl line 3: 'layer1' is not a field of a particle look; it belongs to a layered look.");
    mistake("{\n\tlook particle\n\tlighting\tlit\n\tnearfade { 40 }\n}\n", "body.decl line 4: 'nearfade' needs two numbers in braces, as in 'nearfade { 40, 8 }'.");
    mistake("{\n\tlook particle\n\tglow { 1 }\n}\n", "body.decl line 3: 'glow' needs one number, as in 'glow 4'.");
    mistake("{\n\tlook particle\n\tglowmask 5\n}\n", "body.decl line 3: 'glowmask' needs an image path, as in 'glowmask mods/abcdefgh/fx/embers_mask.tga'.");
    mistake("{\n\tlook particle\n\tgradient { 1, 2, 3 }\n}\n", "body.decl line 3: 'gradient' needs six numbers in braces, as in 'gradient { 1, 0.5, 0, 1, 0.1, 0 }'.");
    mistake("{\n\tlook particle\n\tsoftdistanse 5\n}\n", "body.decl line 3: 'softdistanse' is not a field of a particle look; the nearest is 'softdistance'.");
    mistake("{\n\tlook layered\n\tlayer2rpeat 2\n}\n", "body.decl line 3: 'layer2rpeat' is not a field of a layered look; the nearest is 'layer2repeat'.");
    mistake("{\n\tlook layered\n\tburntilng { 1, 1 }\n}\n", "body.decl line 3: 'burntilng' is not a field of a layered look; the nearest is 'burntiling'.");
    mistake("{\n\tlook particle\n\tnearfaed { 40, 8 }\n}\n", "body.decl line 3: 'nearfaed' is not a field of a particle look; the nearest is 'nearfade'.");
    /* a game material key is not a misspelt field, whatever it resembles */
    {
        static const char *const stock[] = { "woundbloodblend t.tga", "woundtexture t.tga", "bloomcolor { 1, 1, 1, 1 }", "hdrbloomcolorfilter 1",
                                             "addbrightness 1", "depthoffield 1", "lightingmodel 2", "glowstrength 1", "wound3posa { 1, 2, 3, 4 }" };
        size_t i;
        for (i = 0; i < sizeof stock / sizeof stock[0]; i++) {
            char t[200];
            res x, y;
            snprintf(t, sizeof t, "{\n\tlook layered\n\tlayer1 a.tga\n\t%s\n}\n", stock[i]);
            x = compile("a.decl", t);
            CHECK(x.r == SH_LOOK_CHANGED && x.out && has(x.out, stock[i]));
            release(&x);
            snprintf(t, sizeof t, "{\n\tlook particle\n\ttransmap t.tga\n\t%s\n}\n", stock[i]);
            y = compile("a.decl", t);
            CHECK(y.r == SH_LOOK_CHANGED && y.out && has(y.out, stock[i]));
            release(&y);
        }
    }
    mistake("{\n\tdiffusemap d.tga\n\tlook layered\n}\n", "body.decl line 3: 'look' must be the first key of the material; move it to the top.");
    mistake("{\n\tlook glossy\n}\n", "body.decl line 2: 'look' is 'glossy'; write layered or particle.");
    /* the first mistake is the one reported, by its line; a comment or a quoted value does not hide a field */
    mistake("{\n\tlook layered\n\t// layer5 is not here\n\tlayer1repeat 2 // fine\n\tlayer9 a.tga\n\tlayer1scroll 2\n}\n",
            "body.decl line 5: 'layer9' is not a field of a layered look; a surface has layers 1 to 4.");
    /* CRLF text counts the same lines */
    mistake("{\r\n\tlook layered\r\n\tlayer5 a.tga\r\n}\r\n", "body.decl line 3: 'layer5' is not a field of a layered look; a surface has layers 1 to 4.");
}

static void test_not_looks(void)
{
    res x;
    /* a material that is not a look, with no old keys: served as it is; the game's own keys are never mistakes */
    x = compile("a.decl", "{\n\tdiffusemap d.tga\n\tlayerstilingfactor_x 1\n\tambientprogram skinvmtr\n\tglow 3\n\tlayer1 x\n}\n");
    CHECK(x.r == SH_LOOK_SAME && x.out == NULL);
    release(&x);
    x = compile("a.decl", "");
    CHECK(x.r == SH_LOOK_SAME);
    release(&x);
    /* look keys of another decl type are not touched; an unbraced look word later in a value is not a key */
    x = compile("a.decl", "{\n\tdiffusemap \"look layered\"\n}\n");
    CHECK(x.r == SH_LOOK_SAME);
    release(&x);
}

/* ------------------------------------------------------------------------------------------ old keys ---- */

static char *g_fixtures;
static char *read_fixture(const char *name, size_t *n)
{
    char path[MAX_PATH];
    FILE *f;
    char *t;
    long sz;
    snprintf(path, sizeof path, "%s\\%s", g_fixtures, name);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no fixture %s\n", path); g_failed++; return NULL; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    t = (char *)malloc((size_t)sz + 1);
    *n = fread(t, 1, (size_t)sz, f);
    t[*n] = 0;
    fclose(f);
    return t;
}

/* `text` with the old program names renamed. */
static char *renamed(const char *text)
{
    static const char *const old[2] = { "snapmapplus/multilayer/", "snapmapplus/eternal/" };
    static const char *const now[2] = { "snapmapplus/layered/", "snapmapplus/particles/" };
    char *o = (char *)malloc(strlen(text) * 2 + 16);
    size_t i = 0, n = 0;
    while (text[i]) {
        int k, did = 0;
        for (k = 0; k < 2 && !did; k++)
            if (!_strnicmp(text + i, old[k], strlen(old[k]))) { n += (size_t)sprintf(o + n, "%s", now[k]); i += strlen(old[k]); did = 1; }
        if (!did) o[n++] = text[i++];
    }
    o[n] = 0;
    return o;
}

static void test_legacy(void)
{
    static const char *const names[4] = { "legacy_head.decl", "legacy_wounds.decl", "legacy_particle_unlit.decl", "legacy_particle_explosion.decl" };
    int i;
    for (i = 0; i < 4; i++) {
        size_t n = 0, ln = 0;
        char *t = read_fixture(names[i], &n), *legacy = NULL, *want;
        res x;
        if (!t) continue;
        x = compile(names[i], t);
        /* the old keys are read as the look they say and compiled as a look: no old key is left, and the stored vectors the
         * old rewrite would have written are written by the look branch */
        CHECK(sh_modprog_legacy_material(t, n, &legacy, &ln) == 1 && legacy);
        want = renamed(legacy);
        CHECK(x.r == SH_LOOK_CHANGED && x.out && (has(x.out, "layerparms") || has(x.out, "particleuberparms3")));
        CHECK(x.out && !has(x.out, "eternalparticle") && !has(x.out, "multiblendalbedoscale") &&
              !has(x.out, "layerstilingfactor") && !has(x.out, "blendlayer0heightmap"));
        CHECK(x.out && !has(x.out, "snapmapplus/multilayer/") && !has(x.out, "snapmapplus/eternal/"));
        if (x.out) {
            res again = compile(names[i], x.out);
            CHECK(again.r == SH_LOOK_SAME);                      /* nothing left to move */
            release(&again);
        }
        free(want);
        free(legacy);
        release(&x);
        free(t);
    }
    /* only an old program name: renamed, nothing else touched */
    {
        res x = compile("a.decl", "{\n\tambientprogram \"snapmapplus/multilayer/skinvmtr\"\n\tdiffusemap d.tga\n}\n");
        CHECK(x.r == SH_LOOK_CHANGED && x.out && !strcmp(x.out, "{\n\tambientprogram \"snapmapplus/layered/skinvmtr\"\n\tdiffusemap d.tga\n}\n"));
        release(&x);
    }
}

/* ---------------------------------------------------------------------------------------- determinism ---- */

static void test_determinism(void)
{
    static const char T[] =
        "{\n\tlook layered\n\twounds yes\n\tlayermask m.tga\n\tlayer1 { 0.1, 0.2, 0.3 }\n\tlayer2 b.tga\n\tburn t.tga\n\tlayer2glow g.tga\n}\n";
    res a = compile("a.decl", T), b;
    char *first = NULL;
    CHECK(a.r == SH_LOOK_CHANGED && a.out);
    if (!a.out) return;
    first = _strdup(a.out);
    b = compile("elsewhere\\a.decl", T);
    CHECK(b.r == SH_LOOK_CHANGED && b.out && !strcmp(first, b.out));
    release(&b);
    sh_modprog_reset();                                  /* the registry is forgotten: the same text, the same paths */
    b = compile("a.decl", T);
    CHECK(b.r == SH_LOOK_CHANGED && b.out && !strcmp(first, b.out));
    release(&b);
    /* a CRLF text compiles to CRLF lines with the same content */
    {
        char *crlf = (char *)malloc(strlen(T) * 2 + 1);
        size_t i, n = 0;
        for (i = 0; T[i]; i++) { if (T[i] == '\n') crlf[n++] = '\r'; crlf[n++] = T[i]; }
        crlf[n] = 0;
        b = compile("a.decl", crlf);
        CHECK(b.r == SH_LOOK_CHANGED && b.out && has(b.out, "\r\n") && !strcmp(value(b.out, "layerparms", 0), value(first, "layerparms", 0)));
        release(&b);
        free(crlf);
    }
    free(first);
    release(&a);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; ++i) if (!strncmp(argv[i], "fixtures=", 9) && argv[i][9]) g_fixtures = argv[i] + 9;
    printf("modlook_test\n");
    test_layered_example();
    test_layered_defaults();
    test_layered_fields();
    test_values_as_images();
    test_base_height();
    test_pool();
    test_stock_routes();
    test_particle_fields();
    test_mistakes();
    test_not_looks();
    if (g_fixtures) test_legacy();
    else printf("-- old material keys: no fixtures given (fixtures=<dir>); skipped\n");
    test_determinism();
    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
