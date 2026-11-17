/* modprog_test.c -- the render programs the product derives from the game's own (modprog.c).
 *
 * Without the game, over a stand-in program (tests/fixtures/modprog/lit.frag, compiled into modprog_fixture.c, and a
 * cooked program this test writes around it): the decl, stage-text and SPIR-V transforms, then the whole
 * derivation through a fake reader, every derived file checked as the engine checks it (the cooked program's
 * checksum of its decl, the SPIR-V header's hash of its stage text) and as the splice must leave it (the
 * blend's samplers bound after the program's textures, the later bindings moved, the material uniforms grown,
 * the call right after the store of inputs.normal).
 *
 * With the game installed (doom=<install root>), the same checks on the multilayer variants of skinvmtr and
 * skinvmtrwounds derived from the installed programs through modstock.
 *
 * The particle program the same way: its decl and stage-text transforms and its splice over a stand-in particle
 * stage (tests/fixtures/modprog/particle.frag), and with the game its variant of uberparticles and its permutation.
 *
 *   modprog_test.exe [doom=<install root>] */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "modprog.h"
#include "modbundle.h"
#include "modstock.h"

extern const unsigned MODPROG_FIXTURE_SPIRV_WORDS;
extern const unsigned MODPROG_FIXTURE_SPIRV[];
extern const char MODPROG_FIXTURE_GLSL[];

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

void backend_log(const char *message) { (void)message; }

/* ------------------------------------------------------------------ helpers */

typedef struct buf { unsigned char *p; size_t n, cap; } buf;
static void put(buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->n + n) cap *= 2;
        b->p = (unsigned char *)realloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void be32(buf *b, uint32_t v) { unsigned char x[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v }; put(b, x, 4); }
static void le32(buf *b, uint32_t v) { put(b, &v, 4); }
static void str(buf *b, const char *s) { le32(b, (uint32_t)strlen(s)); put(b, s, strlen(s)); }
static uint32_t rd_be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned decl_sum(const char *t, size_t n)
{
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\r' || t[n - 1] == '\n')) n--;
    return sh_mb_md5fold(t, n);
}

/* ------------------------------------------------------------------ the stand-in install */

static const char DECL[] =
    "{\r\n\tparms {\r\n\t\tstageSort\tsortCoverage\r\n\t}\r\n\thlsl_fp {\r\n\t\t#include \"includes/lighting.inc\"\r\n"
    "\t\t#template CUSTOM_TEXTURE_INPUTS( inputs ) {\r\n\t\t\tWOUND_BLOCK( inputs );\r\n\t\t}\r\n\t\t#endtemplate\r\n"
    "\t\tCOMPUTE_SHADING;\r\n\t}\r\n}";
static const char PERM_DECL[] =
    "{\r\n\tpermutations {\r\n\t\tdefault AMD_VK_EXTENSIONS\r\n\t}\r\n\thlsl_fp {\r\n"
    "\t\t#template CUSTOM_TEXTURE_INPUTS( inputs ) {\r\n\t\t\tWOUND_BLOCK( inputs );\r\n\t\t}\r\n\t\t#endtemplate\r\n"
    "\t\tCOMPUTE_SHADING;\r\n\t}\r\n}";
static const char VERT[] = "#version 450 core\nvoid main() { gl_Position = vec4(0.0); }\n";

/* A cooked program around the stand-in stage text: one parameter (stagesort, a vector parm given by name), the
 * fragment stage's lists (5 material uniforms, 2 view uniforms, 2 textures and their samplers, a storage
 * buffer), binding tables as the engine's cooker writes them (fragment: material 5, view 6, textures 7..8,
 * storage buffer 9), and `perm` permutations of mask 1. */
static void cooked(buf *b, const char *decl, int perm)
{
    static const char *const fh[] = { "usevirtualmapping", "bloommaskscale", "colorscale", "color", "bloomcolor" };
    int s, g, i;
    uint32_t block[48];
    memset(b, 0, sizeof *b);
    be32(b, 0xb68800adu);
    be32(b, decl_sum(decl, strlen(decl)));
    be32(b, 0);
    str(b, "");
    be32(b, 0); be32(b, 0);
    put(b, "", 1);
    be32(b, 1);
    { unsigned char fl[2] = { 1, 15 }; put(b, fl, 2); }
    str(b, "stagesort"); str(b, "sortcoverage"); str(b, "");
    for (s = 0; s < 3; s++)
        for (g = 0; g < 7; g++) {
            if (s == 0 && g == 0) { be32(b, 1); str(b, "mvpmatrixx"); continue; }
            if (s != 1) { be32(b, 0); continue; }
            switch (g) {
            case 0: be32(b, 5); for (i = 0; i < 5; i++) str(b, fh[i]); break;
            case 1: be32(b, 2); str(b, "globalvieworigin"); str(b, "time"); break;
            case 2: be32(b, 3); str(b, "vmtrphysicalpagesmaparray0"); str(b, "woundtexture"); str(b, "magnificationclouds"); break;
            case 3: be32(b, 3); str(b, "defaultsampler"); str(b, "defaultsampler"); str(b, "defaultsampler"); break;
            case 4: be32(b, 1); str(b, "clusternumlights"); break;
            default: be32(b, 0);
            }
        }
    be32(b, 0);              /* includes */
    be32(b, 0);              /* flags */
    be32(b, 0); be32(b, perm ? 0 : 1);
    be32(b, perm ? 0 : 1);
    if (!perm) { be32(b, 0); be32(b, 1); }
    memset(block, 0, sizeof block);
    block[5] = 11;
    for (i = 6; i < 27; i++) block[i] = 0xffffffffu;
    block[6] = 0;                                   /* vertex material uniforms at 0 */
    block[13] = 5; block[14] = 6; block[17] = 7; block[18] = 10;
    block[27] = 1;
    block[34] = 1; block[35] = 1; block[38] = 3; block[39] = 1;
    for (i = 0; i < 48; i++) be32(b, block[i]);
    { unsigned char h[10] = { 0, 0, 0, 0x1f, 0xff, 0xff, 0xff, 0xff, 0, 0x0b }; put(b, h, 10); str(b, VERT); }
    { unsigned char h[10] = { 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0x03 }; put(b, h, 10); str(b, MODPROG_FIXTURE_GLSL); }
    { unsigned char h[10] = { 0 }; put(b, h, 10); str(b, ""); }
    for (i = 0; i < 9; i++) be32(b, i == 1 || i == 2 || i == 3 ? 1 : i == 8 ? 0 : 0xffffffffu);
}

static void spirv(buf *b, const char *text, const unsigned *words, unsigned n)
{
    memset(b, 0, sizeof *b);
    be32(b, 4);
    be32(b, sh_mb_md5fold(text, strlen(text)));
    be32(b, 0x12345678u);
    be32(b, n * 4);
    put(b, words, n * 4);
}

static buf g_bin, g_pbin, g_vspv, g_fspv;
static int fake_reader(const char *path, unsigned char **bytes, size_t *len)
{
    const void *d = NULL;
    size_t n = 0;
    static const char vec[] = "{ Vec\t0 }";
    if (strstr(path, "renderparm/stagesort.decl")) { d = vec; n = strlen(vec); }
    else if (!strcmp(path, "generated/decls/renderprog/skinvmtr.decl") ||
             !strcmp(path, "generated/decls/renderprog/skinvmtrwounds.decl")) { d = DECL; n = strlen(DECL); }
    else if (strstr(path, "permutations/skinvmtr") && strstr(path, "0000000000000001.decl")) { d = PERM_DECL; n = strlen(PERM_DECL); }
    else if (strstr(path, "_0000000000000001_pc_vulkan.bin")) { d = g_pbin.p; n = g_pbin.n; }
    else if (strstr(path, "_pc_vulkan.bin")) { d = g_bin.p; n = g_bin.n; }
    else if (strstr(path, ".vspv")) { d = g_vspv.p; n = g_vspv.n; }
    else if (strstr(path, ".fspv")) { d = g_fspv.p; n = g_fspv.n; }
    if (!d) return SH_MB_STOCK_ABSENT;
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    memcpy(*bytes, d, n);
    *len = n;
    return SH_MB_STOCK_OK;
}

/* ------------------------------------------------------------------ SPIR-V reading */

typedef struct mod { const uint32_t *w; size_t n; } mod;
#define FOR_INS(m, at) for (at = 5; at < (m)->n && ((m)->w[at] >> 16); at += (m)->w[at] >> 16)

static uint32_t named(const mod *m, const char *name)
{
    size_t at;
    FOR_INS(m, at) if ((m->w[at] & 0xffff) == 5 && !strcmp((const char *)(m->w + at + 2), name)) return m->w[at + 1];
    return 0;
}
static int binding(const mod *m, uint32_t id)
{
    size_t at;
    FOR_INS(m, at) if ((m->w[at] & 0xffff) == 71 && m->w[at + 1] == id && m->w[at + 2] == 33) return (int)m->w[at + 3];
    return -1;
}
static int struct_members(const mod *m, uint32_t var)
{
    size_t at;
    uint32_t ptr = 0, st = 0;
    FOR_INS(m, at) if ((m->w[at] & 0xffff) == 59 && m->w[at + 2] == var) ptr = m->w[at + 1];
    FOR_INS(m, at) if ((m->w[at] & 0xffff) == 32 && m->w[at + 1] == ptr) st = m->w[at + 3];
    FOR_INS(m, at) if ((m->w[at] & 0xffff) == 30 && m->w[at + 1] == st) return (int)(m->w[at] >> 16) - 2;
    return -1;
}

/* The checks one derived fragment module must pass, against its stock one: the blend's samplers on the bindings
 * after the program's textures (tex_end..), a binding at or after tex_end moved up by the blend's 20, the material
 * uniforms grown by its 24, and a call of ml_blend in main after the store of inputs.normal. */
static void check_module(const uint32_t *w, size_t n, const uint32_t *stock, size_t sn, unsigned tex_end)
{
    mod m = { w, n }, s = { stock, sn };
    size_t at;
    uint32_t blend = 0, k, call_seen = 0;
    CHECK(n > sn && w[0] == 0x07230203u && w[3] > stock[3]);
    CHECK(binding(&m, named(&m, "ml_mask")) == (int)tex_end);
    CHECK(binding(&m, named(&m, "ml_extra1")) == (int)tex_end + 19);
    CHECK(binding(&m, named(&m, "samp_woundtexture")) == binding(&s, named(&s, "samp_woundtexture")));
    FOR_INS(&s, at) {
        if ((s.w[at] & 0xffff) == 71 && s.w[at + 2] == 33 && s.w[at + 3] >= tex_end)
            CHECK(binding(&m, s.w[at + 1]) == (int)s.w[at + 3] + 20);
    }
    CHECK(struct_members(&m, named(&m, "freqHigh_fragmentUniforms")) ==
          struct_members(&s, named(&s, "freqHigh_fragmentUniforms")) + (int)SH_MODPROG_ML_PARM_COUNT);
    FOR_INS(&m, at) if ((m.w[at] & 0xffff) == 5 && !strncmp((const char *)(m.w + at + 2), "ml_blend(", 9)) blend = m.w[at + 1];
    CHECK(blend != 0);
    FOR_INS(&m, at) if ((m.w[at] & 0xffff) == 57 && m.w[at + 3] == blend) call_seen++;
    CHECK(call_seen == 1);
    for (k = 0; k < 4; k++) (void)k;
}

/* An old-name path (SH_MODPROG_OLD_PREFIX / SH_MODPROG_EP_OLD_PREFIX) is owned and reads the bytes of the new-name path. */
static void alias_same(const char *head, const char *new_prefix, const char *old_prefix, const char *tail)
{
    char *np = sh_textf("%s%s%s", head, new_prefix, tail), *op = sh_textf("%s%s%s", head, old_prefix, tail);
    unsigned char *nb = NULL, *ob = NULL;
    size_t nn = 0, on = 0;
    int nr = sh_modprog_read(np, &nb, &nn), orr = sh_modprog_read(op, &ob, &on);
    CHECK(sh_modprog_owns(op));
    CHECK(nr == SH_MB_STOCK_OK && orr == SH_MB_STOCK_OK && nn == on && nn > 0 && !memcmp(nb, ob, nn));
    if (nb) HeapFree(GetProcessHeap(), 0, nb);
    if (ob) HeapFree(GetProcessHeap(), 0, ob);
    free(np); free(op);
}

/* dump=<dir>: write each derived decl there (<program>[.perm].decl), to read it with another compiler. */
static const char *g_dump = NULL;
static void dump_decl(const char *program, const char *decl_req, const unsigned char *d, size_t n)
{
    char path[MAX_PATH];
    FILE *f;
    if (!g_dump) return;
    snprintf(path, sizeof path, "%s\\%s%s.decl", g_dump, program, strstr(decl_req, "permutations") ? ".perm" : "");
    if ((f = fopen(path, "wb")) != NULL) { fwrite(d, 1, n, f); fclose(f); }
}

static void check_unit(const char *base, const char *decl_req, const char *bin_req, const char *vreq, const char *freq,
                       const unsigned char *stock_v, size_t stock_vn, const unsigned char *stock_f, size_t stock_fn)
{
    unsigned char *d = NULL, *b = NULL, *v = NULL, *f = NULL;
    size_t dn = 0, bn = 0, vn = 0, fn = 0, off[3], len[3];
    CHECK(sh_modprog_owns(decl_req) && sh_modprog_owns(bin_req) && sh_modprog_owns(vreq) && sh_modprog_owns(freq));
    CHECK(sh_modprog_read(decl_req, &d, &dn) == SH_MB_STOCK_OK);
    CHECK(sh_modprog_read(bin_req, &b, &bn) == SH_MB_STOCK_OK);
    CHECK(sh_modprog_read(vreq, &v, &vn) == SH_MB_STOCK_OK);
    CHECK(sh_modprog_read(freq, &f, &fn) == SH_MB_STOCK_OK);
    if (!d || !b || !v || !f) { fprintf(stderr, "  %s: %s\n", base, sh_modprog_cause()); goto out; }
    dump_decl(base, decl_req, d, dn);
    /* the decl: the blend's functions in an hlsl_fp_header section before hlsl_fp, then the call as the template's
     * first statements */
    {
        const char *hdr = strstr((const char *)d, "hlsl_fp_header {"), *fp = strstr((const char *)d, "hlsl_fp {");
        const char *tmpl = strstr((const char *)d, "#template CUSTOM_TEXTURE_INPUTS( inputs ) {");
        const char *call = tmpl ? strstr(tmpl, "float4 mlP[28] = {") : NULL;
        CHECK(hdr && fp && tmpl && call && hdr < fp && fp < tmpl);
        CHECK(hdr && strstr(hdr, "void ml_blend(") != NULL && strstr(hdr, "void ml_blend(") < fp);
        CHECK(call && strstr(call, "ml_blend( inputs.albedo") != NULL);
        CHECK(!strstr((const char *)d, "#template MULTILAYER_BLEND"));
    }
    /* the engine's checks */
    CHECK(rd_be32(b) == 0xb68800adu && rd_be32(b + 4) == decl_sum((const char *)d, dn));
    CHECK(sh_mb_rp_stages(b, bn, off, len) == 1);
    CHECK(rd_be32(v) == 4 && rd_be32(v + 4) == sh_mb_md5fold(b + off[0], len[0]) && rd_be32(v + 12) == vn - 16);
    CHECK(rd_be32(f) == 4 && rd_be32(f + 4) == sh_mb_md5fold(b + off[1], len[1]) && rd_be32(f + 12) == fn - 16);
    CHECK(vn == stock_vn && !memcmp(v, stock_v, vn));
    CHECK(strstr((const char *)b + off[1], "ml_blend( inputs.albedo") != NULL);
    {
        /* the stock fragment's texture end: its stage text's woundtexture binding + its texture count */
        const char *t = (const char *)b + off[1];
        unsigned tex_end = 0;
        const char *mask = strstr(t, "uniform sampler2D samp_layerblendmask");
        if (mask) {
            const char *eq = mask;
            while (eq > t && strncmp(eq, "binding = ", 10)) eq--;
            tex_end = (unsigned)atoi(eq + 10);
        }
        CHECK(tex_end > 0);
        check_module((const uint32_t *)(f + 16), (fn - 16) / 4, (const uint32_t *)(stock_f + 16), (stock_fn - 16) / 4, tex_end);
    }
out:
    if (d) HeapFree(GetProcessHeap(), 0, d);
    if (b) HeapFree(GetProcessHeap(), 0, b);
    if (v) HeapFree(GetProcessHeap(), 0, v);
    if (f) HeapFree(GetProcessHeap(), 0, f);
}

/* ------------------------------------------------------------------ the cases */

static void test_decl(void)
{
    size_t n = 0;
    char *t = sh_modprog_patch_decl(DECL, strlen(DECL), &n);
    char *u;
    printf("-- the decl\n");
    CHECK(t && n == strlen(t));
    if (t) {
        const char *hdr = strstr(t, "\thlsl_fp_header {\r\n"), *fp = strstr(t, "\thlsl_fp {\r\n");
        const char *fn = strstr(t, "\t\tvoid ml_blend( inout float3 albedo");
        const char *custom = strstr(t, "#template CUSTOM_TEXTURE_INPUTS( inputs ) {\r\n");
        const char *call = custom ? strstr(custom, "\t\t\tfloat4 mlP[28] = {\r\n") : NULL;
        CHECK(hdr && fn && fp && custom && call && hdr < fn && fn < fp && fp < custom && custom < call);
        CHECK(hdr && fp && strstr(hdr, "\t}\r\n\thlsl_fp {") != NULL);   /* the section closes before the program's */
        CHECK(strstr(t, "inputs.thickness, inputs.texCoords, $time.x, mlP, mlE );\r\n\t\t\tWOUND_BLOCK( inputs );") != NULL);
        CHECK(strstr(t, "\r\n") && !strstr(t, "\n\n\n") && !strstr(t, "\r\r"));
        CHECK(!strstr(t, "#template MULTILAYER"));
    }
    free(t);
    {
        static const char OWN[] = "{\n\thlsl_fp_header {\n\t\tfloat own( float x ) { return x; }\n\t}\n\thlsl_fp {\n"
            "\t\t#template CUSTOM_TEXTURE_INPUTS( inputs ) {\n\t\t\tWOUND_BLOCK( inputs );\n\t\t}\n\t\t#endtemplate\n\t}\n}";
        t = sh_modprog_patch_decl(OWN, strlen(OWN), &n);
        CHECK(t != NULL);
        if (t) {
            const char *own = strstr(t, "float own( float x )"), *fn = strstr(t, "\t\tvoid ml_blend( inout float3 albedo");
            const char *close = strstr(t, "\n\t}\n\thlsl_fp {");
            CHECK(own && fn && close && own < fn && fn < close);
            CHECK(!strstr(strstr(t, "hlsl_fp_header") + 1, "hlsl_fp_header"));
            CHECK(!strchr(t, '\r'));
        }
        free(t);
    }
    /* no template, or two: refused */
    CHECK(sh_modprog_patch_decl("{ hlsl_fp { COMPUTE_SHADING; } }", 32, &n) == NULL);
    u = (char *)malloc(2 * strlen(DECL) + 1);
    strcpy(u, DECL);
    strcat(u, DECL);
    CHECK(sh_modprog_patch_decl(u, strlen(u), &n) == NULL);
    free(u);
}

static void test_glsl(void)
{
    size_t n = 0;
    char *t = sh_modprog_patch_glsl(MODPROG_FIXTURE_GLSL, strlen(MODPROG_FIXTURE_GLSL), 10, 20, &n);
    printf("-- the stage text\n");
    CHECK(t != NULL);
    if (!t) return;
    CHECK(strstr(t, "layout( binding = 8 ) uniform sampler2D samp_woundtexture;") != NULL);   /* below tex_end */
    CHECK(strstr(t, "layout( binding = 30, std430 ) buffer clusternumlights_b") != NULL);     /* moved up */
    CHECK(strstr(t, "layout( binding = 10 ) uniform sampler2D samp_layerblendmask;") != NULL);
    CHECK(strstr(t, "layout( binding = 29 ) uniform sampler2D samp_multilayerextramap1;") != NULL);
    CHECK(strstr(t, "\tvec4 multilayeremberhot;\n}\nfreqHigh_fragmentUniforms ;") != NULL);
    CHECK(strstr(t, "void ml_blend(") != NULL && strstr(t, "void ml_blend(") < strstr(t, "void main() {"));
    CHECK(strstr(t, "inputs.normal = sampleNormal.wyz;\n\t\tml_blend( inputs.albedo,") != NULL);
    free(t);
    CHECK(sh_modprog_patch_glsl("void main() {}", 14, 10, 20, &n) == NULL);
}

static void test_splice(void)
{
    size_t n = 0;
    const char *why = NULL;
    uint32_t *m = sh_modprog_splice(MODPROG_FIXTURE_SPIRV, MODPROG_FIXTURE_SPIRV_WORDS, 10, 20, &n, &why);
    printf("-- the module\n");
    CHECK(m != NULL && why == NULL);
    if (m) check_module(m, n, MODPROG_FIXTURE_SPIRV, MODPROG_FIXTURE_SPIRV_WORDS, 10);
    free(m);
    /* a module that is not a lit program is refused with a reason */
    {
        static const uint32_t tiny[] = { 0x07230203u, 0x00010000u, 0, 2, 0, 0x00020011u, 1 };
        why = NULL;
        CHECK(sh_modprog_splice(tiny, 7, 9, 20, &n, &why) == NULL && why != NULL);
    }
}

static void test_derivation(void)
{
    static const char *const bases[] = { "skinvmtr", "skinvmtrwounds" };
    int i, parms = 0;
    unsigned char *p = NULL;
    size_t pn = 0;
    printf("-- the derivation (stand-in install)\n");
    cooked(&g_bin, DECL, 0);
    cooked(&g_pbin, PERM_DECL, 1);
    spirv(&g_vspv, VERT, MODPROG_FIXTURE_SPIRV, MODPROG_FIXTURE_SPIRV_WORDS);
    spirv(&g_fspv, MODPROG_FIXTURE_GLSL, MODPROG_FIXTURE_SPIRV, MODPROG_FIXTURE_SPIRV_WORDS);
    sh_modprog_set_reader(fake_reader);
    for (i = 0; i < 2; i++) {
        char *d = sh_textf("generated/decls/renderprog/" SH_MODPROG_PREFIX "%s.decl", bases[i]);
        char *b = sh_textf("generated/renderprogs/" SH_MODPROG_PREFIX "%s_pc_vulkan.bin", bases[i]);
        char *v = sh_textf("generated/spirv/" SH_MODPROG_PREFIX "%s.vspv", bases[i]);
        char *f = sh_textf("generated/spirv/" SH_MODPROG_PREFIX "%s.fspv", bases[i]);
        char *pd = sh_textf("generated/renderprogs/permutations/" SH_MODPROG_PREFIX "%s_0000000000000001.decl", bases[i]);
        char *pb = sh_textf("generated/renderprogs/permutations/" SH_MODPROG_PREFIX "%s_0000000000000001_pc_vulkan.bin", bases[i]);
        char *pv = sh_mb_spirv_request(pd, ".vspv"), *pf = sh_mb_spirv_request(pd, ".fspv");
        check_unit(bases[i], d, b, v, f, g_vspv.p, g_vspv.n, g_fspv.p, g_fspv.n);
        check_unit(bases[i], pd, pb, pv, pf, g_vspv.p, g_vspv.n, g_fspv.p, g_fspv.n);
        free(d); free(b); free(v); free(f); free(pd); free(pb); free(pv); free(pf);
    }
    /* the names the programs had before the rename are served until 1.0: the same bytes as the new name */
    alias_same("generated/decls/renderprog/", SH_MODPROG_PREFIX, SH_MODPROG_OLD_PREFIX, "skinvmtr.decl");
    alias_same("generated/renderprogs/", SH_MODPROG_PREFIX, SH_MODPROG_OLD_PREFIX, "skinvmtrwounds_pc_vulkan.bin");
    alias_same("generated/spirv/", SH_MODPROG_PREFIX, SH_MODPROG_OLD_PREFIX, "skinvmtr.fspv");
    alias_same("generated/renderprogs/permutations/", SH_MODPROG_PREFIX, SH_MODPROG_OLD_PREFIX, "skinvmtr_0000000000000001.decl");
    /* the render parms the blend reads are product decls; a stock parm is not the product's */
    for (i = 0; i < (int)SH_MODPROG_ML_PARM_COUNT; i++) {
        char *r = sh_textf("generated/decls/renderparm/%s.decl", SH_MODPROG_ML_PARMS[i]);
        if (sh_modprog_read(r, &p, &pn) == SH_MB_STOCK_OK && pn > 6 && !memcmp(p, "{ Vec ", 6)) parms++;
        if (p) HeapFree(GetProcessHeap(), 0, p);
        p = NULL;
        free(r);
    }
    for (i = 0; i < (int)SH_MODPROG_ML_TEXTURE_COUNT; i++) {
        char *r = sh_textf("generated/decls/renderparm/%s.decl", SH_MODPROG_ML_TEXTURES[i]);
        if (sh_modprog_read(r, &p, &pn) == SH_MB_STOCK_OK && pn > 8 && !memcmp(p, "{ Tex2D ", 8)) parms++;
        if (p) HeapFree(GetProcessHeap(), 0, p);
        p = NULL;
        free(r);
    }
    CHECK(parms == (int)(SH_MODPROG_ML_PARM_COUNT + SH_MODPROG_ML_TEXTURE_COUNT));
    CHECK(!sh_modprog_owns("generated/decls/renderparm/bloomcolor.decl"));
    CHECK(!sh_modprog_owns("generated/spirv/skinvmtr.fspv"));
    CHECK(sh_modprog_read("generated/renderprogs/" SH_MODPROG_PREFIX "skinvmtr_pc.bin", &p, &pn) == SH_MB_STOCK_ABSENT);
    /* a damaged install: the derivation stops with a sentence that names the program */
    g_fspv.p[16] ^= 1;
    sh_modprog_reset();
    CHECK(sh_modprog_read("generated/spirv/" SH_MODPROG_PREFIX "skinvmtr.fspv", &p, &pn) == SH_MB_STOCK_UNAVAILABLE);
    CHECK(strstr(sh_modprog_cause(), "skinvmtr program") != NULL);
    g_fspv.p[16] ^= 1;
    sh_modprog_set_reader(NULL);
    free(g_bin.p); free(g_pbin.p); free(g_vspv.p); free(g_fspv.p);
}

/* ------------------------------------------------------------------ the particle program */

extern const unsigned MODPROG_FIXTURE_EP_SPIRV_WORDS;
extern const unsigned MODPROG_FIXTURE_EP_SPIRV[];
extern const char MODPROG_FIXTURE_EP_GLSL[];

static const char EP_DECL[] =
    "{\r\n\thlsl_vp {\r\n\t\t#include \"includes/fog.inc\"\r\n\t\tresult.position = vertex.position;\r\n\t}\r\n"
    "\thlsl_fp {\r\n\t\t#include \"includes/particle.inc\"\r\n\t\t#include \"includes/fog.inc\"\r\n"
    "\t\tresult.color = float4( 1.0 );\r\n\t\tPREMULTIPLY_ALPHA_BLENDED( result );\r\n\t}\r\n}";

/* The checks one derived particle fragment module must pass against its stock one: the emissive sheet's sampler on
 * the binding after the program's textures, the shading's other samplers the program's own (no texture added), a
 * binding at or after tex_end moved up by one, the material uniforms grown by the four parameters and the two
 * render parms the shading reads, and one call of ep_shade, in main, after the stock colour's last store. */
static void check_ep_module(const uint32_t *w, size_t n, const uint32_t *stock, size_t sn, unsigned tex_end)
{
    mod m = { w, n }, s = { stock, sn };
    size_t at, call_at = 0, last_store = 0;
    uint32_t shade = 0, calls = 0, out = named(&s, "out_FragColor0");
    CHECK(n > sn && w[0] == 0x07230203u && w[3] > stock[3]);
    CHECK(binding(&m, named(&m, "ep_emissive")) == (int)tex_end);
    CHECK(named(&m, "ep_trans") == 0 && named(&m, "ep_depth") == 0 && named(&m, "ep_atlas") == 0);
    CHECK(binding(&m, named(&m, "samp_transmap")) == binding(&s, named(&s, "samp_transmap")));
    CHECK(binding(&m, named(&m, "samp_particleslightatlasmap")) == binding(&s, named(&s, "samp_particleslightatlasmap")));
    FOR_INS(&s, at) {
        if ((s.w[at] & 0xffff) == 71 && s.w[at + 2] == 33 && s.w[at + 3] >= tex_end)
            CHECK(binding(&m, s.w[at + 1]) == (int)s.w[at + 3] + 1);
    }
    CHECK(struct_members(&m, named(&m, "freqHigh_fragmentUniforms")) ==
          struct_members(&s, named(&s, "freqHigh_fragmentUniforms")) + (int)SH_MODPROG_EP_PARM_COUNT + 2);
    FOR_INS(&m, at) if ((m.w[at] & 0xffff) == 5 && !strncmp((const char *)(m.w + at + 2), "ep_shade(", 9)) shade = m.w[at + 1];
    CHECK(shade != 0);
    FOR_INS(&m, at) {
        size_t c;
        uint32_t ptr = (m.w[at] & 0xffff) == 62 ? m.w[at + 1] : 0;
        int to_out = ptr && ptr == out;
        if (ptr && !to_out)   /* a store through an access chain into the colour */
            FOR_INS(&m, c) if ((m.w[c] & 0xffff) == 65 && m.w[c + 2] == ptr && m.w[c + 3] == out) to_out = 1;
        if ((m.w[at] & 0xffff) == 57 && m.w[at + 3] == shade) { calls++; call_at = at; }
        if (to_out && !call_at) last_store = at;
    }
    CHECK(calls == 1 && last_store && last_store < call_at);
}

static void test_ep_texts(void)
{
    size_t n = 0;
    char *t = sh_modprog_ep_patch_decl(EP_DECL, strlen(EP_DECL), &n), *u;
    printf("-- the particle program's decl and stage text\n");
    CHECK(t && n == strlen(t));
    if (t) {
        const char *hdr = strstr(t, "\thlsl_fp_header {\r\n"), *fp = strstr(t, "\thlsl_fp {\r\n");
        const char *fn = strstr(t, "\t\tvoid ep_shade( inout float4 frag");
        const char *pm = strstr(t, "PREMULTIPLY_ALPHA_BLENDED( result );\r\n\t\tfloat4 epV[7] = {\r\n");
        const char *call = strstr(t, "\t\tep_shade( result.color, epV, epP, epU );\r\n\t}\r\n}");
        CHECK(hdr && fn && fp && pm && call && hdr < fn && fn < fp && fp < pm && pm < call);
        CHECK(strstr(t, "result.position = vertex.position;") < hdr);   /* the vertex stage untouched */
        CHECK(strstr(t, "\t\tresult.color = float4( 1.0 );\r\n\t\tPREMULTIPLY") != NULL);   /* the base's statements stay */
        CHECK(!strstr(t, "\r\r") && !strstr(t, "#template"));
    }
    free(t);
    /* no premultiply, or two: refused */
    CHECK(sh_modprog_ep_patch_decl("{ hlsl_fp { #include \"includes/fog.inc\"\n } }", 44, &n) == NULL);
    u = (char *)malloc(2 * strlen(EP_DECL) + 1);
    strcpy(u, EP_DECL);
    strcat(u, EP_DECL);
    CHECK(sh_modprog_ep_patch_decl(u, strlen(u), &n) == NULL);
    free(u);
    t = sh_modprog_ep_patch_glsl(MODPROG_FIXTURE_EP_GLSL, strlen(MODPROG_FIXTURE_EP_GLSL), 7, 1, &n);
    CHECK(t != NULL);
    if (t) {
        CHECK(strstr(t, "layout( binding = 6 ) uniform sampler2D samp_particleslightatlasmap;") != NULL);
        CHECK(strstr(t, "layout( binding = 7 ) uniform sampler2D samp_eternalparticleemissivemap;") != NULL);
        CHECK(strstr(t, "\tvec4 globalvieworigin;\n}\nfreqHigh_fragmentUniforms ;") != NULL);
        CHECK(strstr(t, "void ep_shade(") != NULL && strstr(t, "void ep_shade(") < strstr(t, "void main() {"));
        CHECK(strstr(t, "\t}\n\tep_shade( out_FragColor0, vec4[7]( vofi_Color,") != NULL);
    }
    free(t);
    CHECK(sh_modprog_ep_patch_glsl("void main() {}", 14, 7, 1, &n) == NULL);
}

static void test_ep_splice(void)
{
    size_t n = 0;
    const char *why = NULL;
    uint32_t *m = sh_modprog_ep_splice(MODPROG_FIXTURE_EP_SPIRV, MODPROG_FIXTURE_EP_SPIRV_WORDS, 7, 1, &n, &why);
    printf("-- the particle module\n");
    CHECK(m != NULL && why == NULL);
    if (m) check_ep_module(m, n, MODPROG_FIXTURE_EP_SPIRV, MODPROG_FIXTURE_EP_SPIRV_WORDS, 7);
    free(m);
    /* a lit program is not a particle program: refused with a reason */
    why = NULL;
    CHECK(sh_modprog_ep_splice(MODPROG_FIXTURE_SPIRV, MODPROG_FIXTURE_SPIRV_WORDS, 10, 1, &n, &why) == NULL && why != NULL);
}

static void check_ep_unit(const char *decl_req, const char *bin_req, const char *vreq, const char *freq,
                          const unsigned char *stock_v, size_t stock_vn, const unsigned char *stock_f, size_t stock_fn)
{
    unsigned char *d = NULL, *b = NULL, *v = NULL, *f = NULL;
    size_t dn = 0, bn = 0, vn = 0, fn = 0, off[3], len[3];
    CHECK(sh_modprog_owns(decl_req) && sh_modprog_owns(bin_req) && sh_modprog_owns(vreq) && sh_modprog_owns(freq));
    CHECK(sh_modprog_read(decl_req, &d, &dn) == SH_MB_STOCK_OK);
    CHECK(sh_modprog_read(bin_req, &b, &bn) == SH_MB_STOCK_OK);
    CHECK(sh_modprog_read(vreq, &v, &vn) == SH_MB_STOCK_OK);
    CHECK(sh_modprog_read(freq, &f, &fn) == SH_MB_STOCK_OK);
    if (!d || !b || !v || !f) { fprintf(stderr, "  uberparticles: %s\n", sh_modprog_cause()); goto out; }
    dump_decl("uberparticles", decl_req, d, dn);
    CHECK(strstr((const char *)d, "hlsl_fp_header {") != NULL && strstr((const char *)d, "void ep_shade(") != NULL);
    CHECK(strstr((const char *)d, "PREMULTIPLY_ALPHA_BLENDED( result );\r\n\t\tfloat4 epV[7] = {") != NULL);
    CHECK(rd_be32(b) == 0xb68800adu && rd_be32(b + 4) == decl_sum((const char *)d, dn));
    CHECK(sh_mb_rp_stages(b, bn, off, len) == 1);
    CHECK(rd_be32(v) == 4 && rd_be32(v + 4) == sh_mb_md5fold(b + off[0], len[0]) && rd_be32(v + 12) == vn - 16);
    CHECK(rd_be32(f) == 4 && rd_be32(f + 4) == sh_mb_md5fold(b + off[1], len[1]) && rd_be32(f + 12) == fn - 16);
    CHECK(vn == stock_vn && !memcmp(v, stock_v, vn));
    CHECK(strstr((const char *)b + off[1], "\tep_shade( out_FragColor0,") != NULL);
    {
        const char *t = (const char *)b + off[1], *sheet = strstr(t, "uniform sampler2D samp_eternalparticleemissivemap");
        unsigned tex_end = 0;
        if (sheet) {
            const char *eq = sheet;
            while (eq > t && strncmp(eq, "binding = ", 10)) eq--;
            tex_end = (unsigned)atoi(eq + 10);
        }
        CHECK(tex_end > 0);
        check_ep_module((const uint32_t *)(f + 16), (fn - 16) / 4, (const uint32_t *)(stock_f + 16), (stock_fn - 16) / 4, tex_end);
    }
out:
    if (d) HeapFree(GetProcessHeap(), 0, d);
    if (b) HeapFree(GetProcessHeap(), 0, b);
    if (v) HeapFree(GetProcessHeap(), 0, v);
    if (f) HeapFree(GetProcessHeap(), 0, f);
}

/* The particle program derived from the installed uberparticles and its permutation. */
static void test_real_ep(void)
{
    static const char *const masks[] = { NULL, "0000000000000001" };
    int i;
    printf("-- the installed game's particle program\n");
    for (i = 0; i < 2; i++) {
        char *sd = masks[i] ? sh_textf("generated/renderprogs/permutations/uberparticles_%s.decl", masks[i]) : NULL;
        char *sv = masks[i] ? sh_mb_spirv_request(sd, ".vspv") : sh_mb_spirv_request("uberparticles", ".vspv");
        char *sf = masks[i] ? sh_mb_spirv_request(sd, ".fspv") : sh_mb_spirv_request("uberparticles", ".fspv");
        char *d = masks[i] ? sh_textf("generated/renderprogs/permutations/" SH_MODPROG_EP_PREFIX "uberparticles_%s.decl", masks[i])
                           : sh_textf("generated/decls/renderprog/" SH_MODPROG_EP_PREFIX "uberparticles.decl");
        char *b = masks[i] ? sh_textf("generated/renderprogs/permutations/" SH_MODPROG_EP_PREFIX "uberparticles_%s_pc_vulkan.bin", masks[i])
                           : sh_textf("generated/renderprogs/" SH_MODPROG_EP_PREFIX "uberparticles_pc_vulkan.bin");
        char *v = masks[i] ? sh_mb_spirv_request(d, ".vspv") : sh_textf("generated/spirv/" SH_MODPROG_EP_PREFIX "uberparticles.vspv");
        char *f = masks[i] ? sh_mb_spirv_request(d, ".fspv") : sh_textf("generated/spirv/" SH_MODPROG_EP_PREFIX "uberparticles.fspv");
        char sha[SH_MB_HASH_HEX + 1];
        unsigned long long sz = 0;
        unsigned char *svb = NULL, *sfb = NULL;
        size_t svn = 0, sfn = 0;
        if (sh_modstock_identify(sv, sha, &sz) == SH_MB_STOCK_OK) sh_modstock_read(sv, sha, &svb, &svn);
        if (sh_modstock_identify(sf, sha, &sz) == SH_MB_STOCK_OK) sh_modstock_read(sf, sha, &sfb, &sfn);
        CHECK(svb && sfb);
        if (svb && sfb) check_ep_unit(d, b, v, f, svb, svn, sfb, sfn);
        if (svb) HeapFree(GetProcessHeap(), 0, svb);
        if (sfb) HeapFree(GetProcessHeap(), 0, sfb);
        free(sd); free(sv); free(sf); free(d); free(b); free(v); free(f);
    }
    alias_same("generated/decls/renderprog/", SH_MODPROG_EP_PREFIX, SH_MODPROG_EP_OLD_PREFIX, "uberparticles.decl");
    alias_same("generated/renderprogs/", SH_MODPROG_EP_PREFIX, SH_MODPROG_EP_OLD_PREFIX, "uberparticles_pc_vulkan.bin");
    alias_same("generated/spirv/", SH_MODPROG_EP_PREFIX, SH_MODPROG_EP_OLD_PREFIX, "uberparticles.fspv");
    /* the parms the shading adds are the product's; the two it reads from the game stay the game's */
    CHECK(sh_modprog_owns("generated/decls/renderparm/eternalparticle.decl"));
    CHECK(sh_modprog_owns("generated/decls/renderparm/eternalparticleemissivemap.decl"));
    CHECK(!sh_modprog_owns("generated/decls/renderparm/projectionmatrixz.decl"));
    CHECK(!sh_modprog_owns("generated/spirv/uberparticles.fspv"));
}

typedef struct seen { int files; } seen;
static void count(void *c, const char *path) { (void)path; ((seen *)c)->files++; }

static void test_real(const char *doom)
{
    static const char *const bases[] = { "skinvmtr", "skinvmtrwounds" };
    int i;
    seen s = { 0 };
    printf("-- the installed game's programs\n");
    sh_modstock_set_install(doom);
    sh_modprog_reset();
    for (i = 0; i < 2; i++) {
        char *sd = sh_textf("generated/renderprogs/%s_pc_vulkan.bin", bases[i]);
        char *spd = sh_textf("generated/renderprogs/permutations/%s_0000000000000001.decl", bases[i]);
        char *sv = sh_mb_spirv_request(bases[i], ".vspv"), *sf = sh_mb_spirv_request(bases[i], ".fspv");
        char *spv = sh_mb_spirv_request(spd, ".vspv"), *spf = sh_mb_spirv_request(spd, ".fspv");
        char *d = sh_textf("generated/decls/renderprog/" SH_MODPROG_PREFIX "%s.decl", bases[i]);
        char *b = sh_textf("generated/renderprogs/" SH_MODPROG_PREFIX "%s_pc_vulkan.bin", bases[i]);
        char *v = sh_textf("generated/spirv/" SH_MODPROG_PREFIX "%s.vspv", bases[i]);
        char *f = sh_textf("generated/spirv/" SH_MODPROG_PREFIX "%s.fspv", bases[i]);
        char *pd = sh_textf("generated/renderprogs/permutations/" SH_MODPROG_PREFIX "%s_0000000000000001.decl", bases[i]);
        char *pb = sh_textf("generated/renderprogs/permutations/" SH_MODPROG_PREFIX "%s_0000000000000001_pc_vulkan.bin", bases[i]);
        char *pv = sh_mb_spirv_request(pd, ".vspv"), *pf = sh_mb_spirv_request(pd, ".fspv");
        char sha[SH_MB_HASH_HEX + 1], where[32];
        unsigned long long sz = 0;
        unsigned char *svb = NULL, *sfb = NULL, *spvb = NULL, *spfb = NULL;
        size_t svn = 0, sfn = 0, spvn = 0, spfn = 0;
        if (sh_modstock_identify(sv, sha, &sz) == SH_MB_STOCK_OK) sh_modstock_read(sv, sha, &svb, &svn);
        if (sh_modstock_identify(sf, sha, &sz) == SH_MB_STOCK_OK) sh_modstock_read(sf, sha, &sfb, &sfn);
        if (sh_modstock_identify(spv, sha, &sz) == SH_MB_STOCK_OK) sh_modstock_read(spv, sha, &spvb, &spvn);
        if (sh_modstock_identify(spf, sha, &sz) == SH_MB_STOCK_OK) sh_modstock_read(spf, sha, &spfb, &spfn);
        CHECK(svb && sfb && spvb && spfb);
        if (svb && sfb && spvb && spfb) {
            check_unit(bases[i], d, b, v, f, svb, svn, sfb, sfn);
            check_unit(bases[i], pd, pb, pv, pf, spvb, spvn, spfb, spfn);
        }
        /* modstock answers for them as installed content of the product's container */
        CHECK(sh_modstock_identify(f, sha, &sz) == SH_MB_STOCK_OK && sz > 0);
        CHECK(sh_modstock_container(f, sha, where, sizeof where) == SH_MB_STOCK_OK && !strcmp(where, SH_MODPROG_CONTAINER));
        CHECK(sh_modstock_exists(d) == 1);
        HeapFree(GetProcessHeap(), 0, svb); HeapFree(GetProcessHeap(), 0, sfb);
        HeapFree(GetProcessHeap(), 0, spvb); HeapFree(GetProcessHeap(), 0, spfb);
        free(sd); free(spd); free(sv); free(sf); free(spv); free(spf);
        free(d); free(b); free(v); free(f); free(pd); free(pb); free(pv); free(pf);
    }
    sh_modstock_list("generated/renderprogs/permutations/" SH_MODPROG_PREFIX, count, &s);
    CHECK(s.files == 4);      /* each base's permutation decl and cooked program */
    test_real_ep();
    s.files = 0;
    sh_modstock_list("generated/renderprogs/permutations/" SH_MODPROG_EP_PREFIX, count, &s);
    CHECK(s.files == 2);
    if (sh_modprog_cause()[0]) fprintf(stderr, "  %s\n", sh_modprog_cause());
    sh_modstock_reset();
}

int main(int argc, char **argv)
{
    const char *doom = NULL;
    int i;
    for (i = 1; i < argc; ++i) if (!strncmp(argv[i], "doom=", 5) && argv[i][5]) doom = argv[i] + 5;
    for (i = 1; i < argc; ++i) if (!strncmp(argv[i], "dump=", 5) && argv[i][5]) g_dump = argv[i] + 5;
    printf("modprog_test\n");
    test_decl();
    test_glsl();
    test_splice();
    test_derivation();
    test_ep_texts();
    test_ep_splice();
    if (doom) test_real(doom);
    else printf("-- the installed game's programs: no install given (doom=<root>); skipped\n");
    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
