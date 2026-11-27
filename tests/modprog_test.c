/* modprog_test.c -- the render programs the product derives from the game's own (modprog.c).
 *
 * Over a stand-in program (tests/fixtures/modprog/lit.frag and a cooked program written here): the decl, stage-text
 * and SPIR-V transforms and the whole derivation through a fake reader, with every derived file checked as the
 * engine checks it and as the splice must leave it. With doom=<install root> the same checks run on the multilayer
 * and particle variants derived from the installed programs.
 *
 *   modprog_test.exe [doom=<install root>] */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
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
 * fragment stage's lists (5 material uniforms, 2 view uniforms, 2 textures and samplers, a storage buffer), binding
 * tables as the engine's cooker writes them, and `perm` permutations of mask 1. */
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
 * uniforms not grown, and a call of ml_blend in main after the store of inputs.normal. */
static void check_module(const uint32_t *w, size_t n, const uint32_t *stock, size_t sn, unsigned tex_end)
{
    mod m = { w, n }, s = { stock, sn };
    size_t at;
    uint32_t blend = 0, k, call_seen = 0;
    CHECK(n > sn && w[0] == 0x07230203u && w[3] > stock[3]);
    CHECK(binding(&m, named(&m, "ml_mask")) == (int)tex_end);
    CHECK(binding(&m, named(&m, "ml_params")) == (int)tex_end + 1);
    CHECK(binding(&m, named(&m, "ml_extra1")) == (int)tex_end + 19);
    CHECK(binding(&m, named(&m, "samp_woundtexture")) == binding(&s, named(&s, "samp_woundtexture")));
    FOR_INS(&s, at) {
        if ((s.w[at] & 0xffff) == 71 && s.w[at + 2] == 33 && s.w[at + 3] >= tex_end)
            CHECK(binding(&m, s.w[at + 1]) == (int)s.w[at + 3] + 20);
    }
    CHECK(struct_members(&m, named(&m, "freqHigh_fragmentUniforms")) ==
          struct_members(&s, named(&s, "freqHigh_fragmentUniforms")));   /* the layered program adds no parameter */
    FOR_INS(&m, at) if ((m.w[at] & 0xffff) == 5 && !strncmp((const char *)(m.w + at + 2), "ml_blend(", 9)) blend = m.w[at + 1];
    CHECK(blend != 0);
    FOR_INS(&m, at) if ((m.w[at] & 0xffff) == 57 && m.w[at + 3] == blend) call_seen++;
    CHECK(call_seen == 1);
    for (k = 0; k < 4; k++) (void)k;
}

/* The original layered path resolves to the same bytes. */
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

static void pinned_particle(const char *head, const char *tail)
{
    char *current = sh_textf("%s" SH_MODPROG_EP_PREFIX "%s", head, tail);
    char *old = sh_textf("%ssnapmapplus/eternal/%s", head, tail);
    char sha[65];
    unsigned long long size = 0;
    size_t n = 0;
    unsigned char *bytes = NULL;
    CHECK(!sh_modprog_owns(old) && sh_modprog_read(old, &bytes, &n) == SH_MB_STOCK_ABSENT);
    CHECK(sh_modstock_identify(current, sha, &size) == SH_MB_STOCK_OK && size);
    CHECK(sh_modstock_verify(old, sha, &size) == SH_MB_STOCK_OK);
    CHECK(sh_modstock_read(old, sha, &bytes, &n) == SH_MB_STOCK_OK && bytes && n == size);
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    sha[0] = sha[0] == '0' ? '1' : '0';
    CHECK(sh_modstock_verify(old, sha, &size) == SH_MB_STOCK_MISMATCH);
    free(current);
    free(old);
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

/* The textures a derived program binds over its stages: the sampler declarations of its three stage texts. */
static int g_stage_textures;
static int count_textures(const unsigned char *b, size_t bn)
{
    size_t off[3], len[3];
    int st, total = 0;
    if (sh_mb_rp_stages(b, bn, off, len) != 1) return -1;
    for (st = 0; st < 3; st++) {
        const char *t = (const char *)b + off[st], *e = t + len[st], *at = t;
        while ((at = strstr(at, "uniform sampler")) != NULL && at < e) { total++; at += 15; }
    }
    return total;
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
    g_stage_textures = count_textures(b, bn);
    /* the decl: the blend's functions in an hlsl_fp_header section before hlsl_fp, then the call as the template's
     * first statements */
    {
        const char *hdr = strstr((const char *)d, "hlsl_fp_header {"), *fp = strstr((const char *)d, "hlsl_fp {");
        const char *tmpl = strstr((const char *)d, "#template CUSTOM_TEXTURE_INPUTS( inputs ) {");
        const char *call = tmpl ? strstr(tmpl, "float4 mlE[4] = {") : NULL;
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
        const char *call = custom ? strstr(custom, "\t\t\tfloat4 mlE[4] = { $bloomMaskScale") : NULL;
        CHECK(hdr && fn && fp && custom && call && hdr < fn && fn < fp && fp < custom && custom < call);
        CHECK(hdr && fp && strstr(hdr, "\t}\r\n\thlsl_fp {") != NULL);   /* the section closes before the program's */
        CHECK(strstr(t, "inputs.thickness, inputs.texCoords, $time.x, mlE );\r\n\t\t\tWOUND_BLOCK( inputs );") != NULL);
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
    CHECK(strstr(t, "layout( binding = 11 ) uniform sampler2D samp_layerparms;") != NULL);
    CHECK(strstr(t, "layout( binding = 29 ) uniform sampler2D samp_multilayerextramap1;") != NULL);
    CHECK(strstr(t, "samp_blendlayer0heightmap") == NULL);
    CHECK(strstr(t, "multilayerember") == NULL && strstr(t, "layerstilingfactor") == NULL);   /* no vector parm of its own */
    CHECK(strstr(t, "vec4 bloomcolor;\n}\nfreqHigh_fragmentUniforms ;") != NULL);            /* the block is the program's */
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
    /* the render parms the program reads that are textures are product decls; no vector render parm is the product's */
    for (i = 0; i < (int)SH_MODPROG_ML_PARM_COUNT; i++) {
        char *r = sh_textf("generated/decls/renderparm/%s.decl", SH_MODPROG_ML_PARMS[i]);
        CHECK(!sh_modprog_owns(r));
        CHECK(sh_modprog_read(r, &p, &pn) == SH_MB_STOCK_ABSENT);
        free(r);
    }
    CHECK(sh_modprog_vector_parm(0) == NULL && sh_modprog_vector_parm(27) == NULL && sh_modprog_vector_parm(999) == NULL);
    CHECK(SH_MODPROG_ML_PARM_COUNT == 28 && SH_MODPROG_ML_TEXTURE_COUNT == 20);
    for (i = 0; i < (int)SH_MODPROG_ML_TEXTURE_COUNT; i++) {
        char *r = sh_textf("generated/decls/renderparm/%s.decl", SH_MODPROG_ML_TEXTURES[i]);
        if (sh_modprog_read(r, &p, &pn) == SH_MB_STOCK_OK && pn > 8 && !memcmp(p, "{ Tex2D ", 8)) parms++;
        if (p) HeapFree(GetProcessHeap(), 0, p);
        p = NULL;
        free(r);
    }
    CHECK(parms == (int)SH_MODPROG_ML_TEXTURE_COUNT);
    CHECK(sh_modprog_owns("generated/decls/renderparm/layerparms.decl"));
    CHECK(!sh_modprog_owns("generated/decls/renderparm/blendlayer0heightmap.decl"));
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
 * the binding after the program's textures, the shading's other samplers unchanged, bindings at or after tex_end
 * moved up by one, the material uniforms grown by the three parameters the shading reads, and one call of ep_shade
 * in main after the stock colour's last store. */
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
          struct_members(&s, named(&s, "freqHigh_fragmentUniforms")) + 3);   /* smokebrightness, projectionmatrixz, globalvieworigin */
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
        CHECK(strstr(t, "layout( binding = 7 ) uniform sampler2D samp_particleemissivemap;") != NULL);
        CHECK(strstr(t, "\tvec4 smokebrightness;\n\tvec4 projectionmatrixz;\n\tvec4 globalvieworigin;\n}\nfreqHigh_fragmentUniforms ;") != NULL);
        CHECK(strstr(t, "eternalparticle") == NULL);
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

/* ------------------------------------------------------------------ the hair programs' death fade */

extern const unsigned MODPROG_FIXTURE_HR_SPIRV_WORDS;
extern const unsigned MODPROG_FIXTURE_HR_SPIRV[];
extern const char MODPROG_FIXTURE_HR_GLSL[];

/* hairprez's shape: a vertex stage, a fragment stage whose last line is its alpha test's clip and a commented block. */
static const char HR_DECL[] =
    "{\r\n\tstate {\r\n\t\tdepthfunc GL_LEQUAL\r\n\t}\r\n\thlsl_vp {\r\n\t\tresult.position = vertex.position;\r\n\t}\r\n"
    "\thlsl_fp {\r\n\t\tfloat alpha = tex2DDeGamma( $spareDiffuseMap, fragment.texcoord0.xy ).w;\r\n"
    "\t\tclip( alpha - $hairParms.w );\r\n\t\tresult.color = _float4( 0.0 );\r\n//\t\tif ( x ) { y }\r\n\t}\r\n}";

/* The checks one derived hair fragment module must pass against its stock one: the noise's sampler on the binding
 * after the program's textures, a binding at or after tex_end moved up by one, the material uniforms grown by
 * deathfadeopacity, and one call of hr_fade, in main, after every other statement (just before its return). */
static void check_hr_module(const uint32_t *w, size_t n, const uint32_t *stock, size_t sn, unsigned tex_end)
{
    mod m = { w, n }, s = { stock, sn };
    size_t at, call_at = 0, ret_at = 0, kills = 0;
    uint32_t fade = 0, calls = 0, mainid = named(&m, "main"), in_main = 0;
    CHECK(n > sn && w[0] == 0x07230203u && w[3] > stock[3]);
    CHECK(binding(&m, named(&m, "hr_noise")) == (int)tex_end);
    CHECK(binding(&m, named(&m, "samp_sparediffusemap")) == binding(&s, named(&s, "samp_sparediffusemap")));
    FOR_INS(&s, at) {
        if ((s.w[at] & 0xffff) == 71 && s.w[at + 2] == 33 && s.w[at + 3] >= tex_end)
            CHECK(binding(&m, s.w[at + 1]) == (int)s.w[at + 3] + 1);
    }
    CHECK(struct_members(&m, named(&m, "freqHigh_fragmentUniforms")) ==
          struct_members(&s, named(&s, "freqHigh_fragmentUniforms")) + 1);   /* deathfadeopacity */
    FOR_INS(&m, at) if ((m.w[at] & 0xffff) == 5 && !strncmp((const char *)(m.w + at + 2), "hr_fade(", 8)) fade = m.w[at + 1];
    CHECK(fade != 0);
    FOR_INS(&m, at) {
        uint32_t op = m.w[at] & 0xffff;
        if (op == 54) in_main = m.w[at + 2] == mainid;
        if (op == 57 && m.w[at + 3] == fade) { calls++; call_at = at; }
        if (in_main && op == 253) ret_at = at;
        if (op == 252) kills++;
    }
    CHECK(calls == 1 && call_at && ret_at > call_at);
    CHECK(kills >= 1);   /* the fade's discard */
}

static void test_hr_texts(void)
{
    size_t n = 0;
    char *t = sh_modprog_hr_patch_decl(HR_DECL, strlen(HR_DECL), &n), *u;
    printf("-- the hair programs' decl and stage text\n");
    CHECK(t && n == strlen(t));
    if (t) {
        const char *hdr = strstr(t, "\thlsl_fp_header {\r\n"), *fp = strstr(t, "\thlsl_fp {\r\n");
        const char *fn = strstr(t, "\t\tvoid hr_fade( float4 V[1], float4 P[1] )");
        const char *clipp = strstr(t, "\t\tclip( alpha - $hairParms.w );");
        const char *call = strstr(t, "\t\tfloat4 hrV[1] = { fragment.texcoord0 };\r\n\t\tfloat4 hrP[1] = { $deathfadeopacity };\r\n"
                                     "\t\thr_fade( hrV, hrP );\r\n\t}\r\n}");
        CHECK(hdr && fn && fp && clipp && call && hdr < fn && fn < fp && fp < clipp && clipp < call);
        CHECK(strstr(t, "result.position = vertex.position;") < hdr);   /* the vertex stage untouched */
        CHECK(strstr(t, "//\t\tif ( x ) { y }\r\n\t\tfloat4 hrV[1]") != NULL);   /* a commented brace is not the close */
        CHECK(!strstr(t, "\r\r"));
    }
    free(t);
    /* no hlsl_fp section, or two: refused */
    CHECK(sh_modprog_hr_patch_decl("{ hlsl_vp { x; } }", 18, &n) == NULL);
    u = (char *)malloc(2 * strlen(HR_DECL) + 1);
    strcpy(u, HR_DECL);
    strcat(u, HR_DECL);
    CHECK(sh_modprog_hr_patch_decl(u, strlen(u), &n) == NULL);
    free(u);
    t = sh_modprog_hr_patch_glsl(MODPROG_FIXTURE_HR_GLSL, strlen(MODPROG_FIXTURE_HR_GLSL), 5, 1, &n);
    CHECK(t != NULL);
    if (t) {
        CHECK(strstr(t, "layout( binding = 4 ) uniform sampler2D samp_sparediffusemap;") != NULL);
        CHECK(strstr(t, "layout( binding = 5 ) uniform sampler2D samp_magnificationclouds;") != NULL);
        CHECK(strstr(t, "\tvec4 hairparms;\n\tvec4 deathfadeopacity;\n}\nfreqHigh_fragmentUniforms ;") != NULL);
        CHECK(strstr(t, "void hr_fade(") != NULL && strstr(t, "void hr_fade(") < strstr(t, "void main() {"));
        CHECK(strstr(t, "\t}\n\thr_fade( vec4[1]( vofi_TexCoord0 ), vec4[1]( freqHigh_fragmentUniforms.deathfadeopacity ) );\n}") != NULL);
    }
    free(t);
    CHECK(sh_modprog_hr_patch_glsl("void main() {}", 14, 5, 1, &n) == NULL);
    {
        /* a program that already samples the noise: refused (its texture would be bound twice) */
        static const char noisy[] = "layout( binding = 3 ) uniform freqHigh_fragmentUniforms_ubo {\n\tvec4 a;\n}\nf ;\n"
                                    "layout( binding = 4 ) uniform sampler2D samp_magnificationclouds;\n"
                                    "layout( location = 2 ) in vec4 vofi_TexCoord0;\nvoid main() {\n}\n";
        CHECK(sh_modprog_hr_patch_glsl(noisy, strlen(noisy), 5, 1, &n) == NULL);
    }
}

static void test_hr_splice(void)
{
    size_t n = 0;
    const char *why = NULL;
    uint32_t *m = sh_modprog_hr_splice(MODPROG_FIXTURE_HR_SPIRV, MODPROG_FIXTURE_HR_SPIRV_WORDS, 5, 1, &n, &why);
    printf("-- the hair module\n");
    CHECK(m != NULL && why == NULL);
    if (m) check_hr_module(m, n, MODPROG_FIXTURE_HR_SPIRV, MODPROG_FIXTURE_HR_SPIRV_WORDS, 5);
    free(m);
    /* a module that is not SPIR-V is refused with a reason */
    why = NULL;
    CHECK(sh_modprog_hr_splice(MODPROG_FIXTURE_HR_SPIRV, 3, 5, 1, &n, &why) == NULL && why != NULL);
}

static void check_hr_unit(const char *base, const char *decl_req, const char *bin_req, const char *vreq, const char *freq,
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
    CHECK(strstr((const char *)d, "hlsl_fp_header {") != NULL && strstr((const char *)d, "void hr_fade(") != NULL);
    CHECK(strstr((const char *)d, "hr_fade( hrV, hrP );") != NULL);
    CHECK(rd_be32(b) == 0xb68800adu && rd_be32(b + 4) == decl_sum((const char *)d, dn));
    CHECK(sh_mb_rp_stages(b, bn, off, len) == 1);
    CHECK(rd_be32(v) == 4 && rd_be32(v + 4) == sh_mb_md5fold(b + off[0], len[0]) && rd_be32(v + 12) == vn - 16);
    CHECK(rd_be32(f) == 4 && rd_be32(f + 4) == sh_mb_md5fold(b + off[1], len[1]) && rd_be32(f + 12) == fn - 16);
    CHECK(vn == stock_vn && !memcmp(v, stock_v, vn));
    CHECK(strstr((const char *)b + off[1], "\thr_fade( vec4[1]( vofi_TexCoord0 ),") != NULL);
    g_stage_textures = count_textures(b, bn);
    printf("   %s: %d textures over its stages\n", base, g_stage_textures);
    CHECK(g_stage_textures > 0 && g_stage_textures <= 32);
    {
        const char *t = (const char *)b + off[1], *noise = strstr(t, "uniform sampler2D samp_magnificationclouds");
        unsigned tex_end = 0;
        if (noise) {
            const char *eq = noise;
            while (eq > t && strncmp(eq, "binding = ", 10)) eq--;
            tex_end = (unsigned)atoi(eq + 10);
        }
        CHECK(tex_end > 0);
        check_hr_module((const uint32_t *)(f + 16), (fn - 16) / 4, (const uint32_t *)(stock_f + 16), (stock_fn - 16) / 4, tex_end);
    }
out:
    if (d) HeapFree(GetProcessHeap(), 0, d);
    if (b) HeapFree(GetProcessHeap(), 0, b);
    if (v) HeapFree(GetProcessHeap(), 0, v);
    if (f) HeapFree(GetProcessHeap(), 0, f);
}

/* The hair programs derived from the installed hairprez, hairshadow and hairblending and their permutations. */
static void test_real_hr(void)
{
    static const char *const bases[] = { "hairprez", "hairshadow", "hairblending" };
    static const char *const masks[] = { NULL, "0000000000000001" };
    int i, j;
    printf("-- the installed game's hair programs\n");
    for (j = 0; j < 3; j++)
        for (i = 0; i < 2; i++) {
            char *sd = masks[i] ? sh_textf("generated/renderprogs/permutations/%s_%s.decl", bases[j], masks[i]) : NULL;
            char *sv = masks[i] ? sh_mb_spirv_request(sd, ".vspv") : sh_mb_spirv_request(bases[j], ".vspv");
            char *sf = masks[i] ? sh_mb_spirv_request(sd, ".fspv") : sh_mb_spirv_request(bases[j], ".fspv");
            char *d = masks[i] ? sh_textf("generated/renderprogs/permutations/" SH_MODPROG_HR_PREFIX "%s_%s.decl", bases[j], masks[i])
                               : sh_textf("generated/decls/renderprog/" SH_MODPROG_HR_PREFIX "%s.decl", bases[j]);
            char *b = masks[i] ? sh_textf("generated/renderprogs/permutations/" SH_MODPROG_HR_PREFIX "%s_%s_pc_vulkan.bin", bases[j], masks[i])
                               : sh_textf("generated/renderprogs/" SH_MODPROG_HR_PREFIX "%s_pc_vulkan.bin", bases[j]);
            char *v = masks[i] ? sh_mb_spirv_request(d, ".vspv") : sh_textf("generated/spirv/" SH_MODPROG_HR_PREFIX "%s.vspv", bases[j]);
            char *f = masks[i] ? sh_mb_spirv_request(d, ".fspv") : sh_textf("generated/spirv/" SH_MODPROG_HR_PREFIX "%s.fspv", bases[j]);
            char sha[SH_MB_HASH_HEX + 1], where[32];
            unsigned long long sz = 0;
            unsigned char *svb = NULL, *sfb = NULL;
            size_t svn = 0, sfn = 0;
            if (sh_modstock_identify(sv, sha, &sz) == SH_MB_STOCK_OK) sh_modstock_read(sv, sha, &svb, &svn);
            if (sh_modstock_identify(sf, sha, &sz) == SH_MB_STOCK_OK) sh_modstock_read(sf, sha, &sfb, &sfn);
            CHECK(svb && sfb);
            if (svb && sfb) check_hr_unit(bases[j], d, b, v, f, svb, svn, sfb, sfn);
            CHECK(sh_modstock_identify(f, sha, &sz) == SH_MB_STOCK_OK && sz > 0);
            CHECK(sh_modstock_container(f, sha, where, sizeof where) == SH_MB_STOCK_OK && !strcmp(where, SH_MODPROG_CONTAINER));
            HeapFree(GetProcessHeap(), 0, svb); HeapFree(GetProcessHeap(), 0, sfb);
            free(sd); free(sv); free(sf); free(d); free(b); free(v); free(f);
        }
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
    g_stage_textures = count_textures(b, bn);
    printf("   uberparticles: %d textures over its stages\n", g_stage_textures);
    CHECK(g_stage_textures > 0 && g_stage_textures <= 32);
    {
        const char *t = (const char *)b + off[1], *sheet = strstr(t, "uniform sampler2D samp_particleemissivemap");
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

static void test_real_recolourable(void)
{
    const char *base = "skinvmtr_custom";
    const char *masks[] = { NULL, "0000000000000001" };
    for (unsigned i = 0; i < 2; i++) {
        const char *prefix = "snapmapplus/recolourable/";
        char *decl = i ? sh_textf("generated/renderprogs/permutations/%s%s_%s.decl", prefix, base, masks[i])
                       : sh_textf("generated/decls/renderprog/%s%s.decl", prefix, base);
        char *bin = i ? sh_textf("generated/renderprogs/permutations/%s%s_%s_pc_vulkan.bin", prefix, base, masks[i])
                      : sh_textf("generated/renderprogs/%s%s_pc_vulkan.bin", prefix, base);
        char *frag = i ? sh_mb_spirv_request(decl, ".fspv") : sh_textf("generated/spirv/%s%s.fspv", prefix, base);
        unsigned char *d = NULL, *b = NULL, *f = NULL;
        size_t dn = 0, bn = 0, fn = 0, off[3], len[3];
        CHECK(sh_modprog_read(decl, &d, &dn) == SH_MB_STOCK_OK && d);
        CHECK(sh_modprog_read(bin, &b, &bn) == SH_MB_STOCK_OK && b);
        CHECK(sh_modprog_read(frag, &f, &fn) == SH_MB_STOCK_OK && f);
        if (d && b && f && fn > 16 && sh_mb_rp_stages(b, bn, off, len) == 1) {
            CHECK(strstr((char *)d, "tex2D( $recolourMask, fragment.texcoord0.xy )") != NULL);
            CHECK(strstr((char *)b + off[1], "texture( samp_recolourmask, vofi_TexCoord0.xy )") != NULL);
            CHECK(rd_be32(f + 4) == sh_mb_md5fold(b + off[1], len[1]));
            if (g_dump) {
                char path[MAX_PATH];
                snprintf(path, sizeof path, "%s/recolourable-%u.spv", g_dump, i);
                FILE *file = fopen(path, "wb");
                if (file) { fwrite(f + 16, 1, fn - 16, file); fclose(file); }
            }
        }
        if (d) HeapFree(GetProcessHeap(), 0, d);
        if (b) HeapFree(GetProcessHeap(), 0, b);
        if (f) HeapFree(GetProcessHeap(), 0, f);
        free(decl); free(bin); free(frag);
    }
}

static void test_real_ribbon(void)
{
    const char *paths[] = {
        "generated/decls/renderprog/snapmapplus/ribbons/ribbonblend.decl",
        "generated/renderprogs/snapmapplus/ribbons/ribbonblend_pc_vulkan.bin",
        "generated/spirv/snapmapplus/ribbons/ribbonblend.vspv",
        "generated/spirv/snapmapplus/ribbons/ribbonblend.fspv",
    };
    unsigned char *files[4] = { 0 }, *stock = NULL;
    size_t sizes[4] = { 0 }, bytes = 0;
    char sha[65];
    unsigned long long size = 0;
    for (unsigned i = 0; i < 4; i++) {
        CHECK(sh_modprog_owns(paths[i]));
        CHECK(sh_modprog_read(paths[i], &files[i], &sizes[i]) == SH_MB_STOCK_OK && files[i]);
    }
    if (files[0] && files[1] && files[2] && files[3]) {
        size_t off[3], len[3];
        CHECK(strstr((const char *)files[0], "vertex.tangent.x, vertex.color.a") != NULL);
        if (sh_mb_rp_stages(files[1], sizes[1], off, len) == 1) {
            CHECK(strstr((const char *)files[1] + off[0], "in_Tangent.x, in_Color.a") != NULL);
            CHECK(rd_be32(files[2] + 4) == sh_mb_md5fold(files[1] + off[0], len[0]));
        } else CHECK(0);
        CHECK(sh_modstock_identify("generated/spirv/ribbonblend.fspv", sha, &size) == SH_MB_STOCK_OK);
        CHECK(sh_modstock_read("generated/spirv/ribbonblend.fspv", sha, &stock, &bytes) == SH_MB_STOCK_OK);
        CHECK(stock && bytes == sizes[3] && !memcmp(stock, files[3], bytes));
        if (g_dump) {
            char path[MAX_PATH];
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/ribbon-age.spv", g_dump);
            FILE *file = fopen(path, "wb");
            if (file) { fwrite(files[2] + 16, 1, sizes[2] - 16, file); fclose(file); }
        }
    }
    for (unsigned i = 0; i < 4; i++) if (files[i]) HeapFree(GetProcessHeap(), 0, files[i]);
    if (stock) HeapFree(GetProcessHeap(), 0, stock);
}

/* The particle program derived from the installed uberparticles and its permutation. */
static void test_real_ep(const char *prefix)
{
    static const char *const masks[] = { NULL, "0000000000000001" };
    int i;
    printf("-- the installed game's particle program\n");
    for (i = 0; i < 2; i++) {
        char *sd = masks[i] ? sh_textf("generated/renderprogs/permutations/uberparticles_%s.decl", masks[i]) : NULL;
        char *sv = masks[i] ? sh_mb_spirv_request(sd, ".vspv") : sh_mb_spirv_request("uberparticles", ".vspv");
        char *sf = masks[i] ? sh_mb_spirv_request(sd, ".fspv") : sh_mb_spirv_request("uberparticles", ".fspv");
        char *d = masks[i] ? sh_textf("generated/renderprogs/permutations/%suberparticles_%s.decl", prefix, masks[i])
                           : sh_textf("generated/decls/renderprog/%suberparticles.decl", prefix);
        char *b = masks[i] ? sh_textf("generated/renderprogs/permutations/%suberparticles_%s_pc_vulkan.bin", prefix, masks[i])
                           : sh_textf("generated/renderprogs/%suberparticles_pc_vulkan.bin", prefix);
        char *v = masks[i] ? sh_mb_spirv_request(d, ".vspv") : sh_textf("generated/spirv/%suberparticles.vspv", prefix);
        char *f = masks[i] ? sh_mb_spirv_request(d, ".fspv") : sh_textf("generated/spirv/%suberparticles.fspv", prefix);
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
    pinned_particle("generated/decls/renderprog/", "uberparticles.decl");
    pinned_particle("generated/renderprogs/", "uberparticles_pc_vulkan.bin");
    pinned_particle("generated/spirv/", "uberparticles.fspv");
    /* the sheet is the product's texture parm; every vector the shading reads is the game's */
    CHECK(sh_modprog_owns("generated/decls/renderparm/particleemissivemap.decl"));
    CHECK(!sh_modprog_owns("generated/decls/renderparm/eternalparticle.decl"));
    CHECK(!sh_modprog_owns("generated/decls/renderparm/smokebrightness.decl"));
    CHECK(!sh_modprog_owns("generated/decls/renderparm/particleuberparms1.decl"));
    CHECK(!sh_modprog_owns("generated/decls/renderparm/emissive.decl"));
    CHECK(!sh_modprog_owns("generated/decls/renderparm/projectionmatrixz.decl"));
    CHECK(!sh_modprog_owns("generated/spirv/uberparticles.fspv"));
}

typedef struct seen { int files; } seen;
static void count(void *c, const char *path) { (void)path; ((seen *)c)->files++; }

/* ------------------------------------------------------------------ the parameter image */

/* const_37f8_37f8_37f8_3c00.bimage as the research repository's writer wrote it (4 x 4 RGBA16F, every texel
 * 0x37f8 0x37f8 0x37f8 0x3c00). */
static const unsigned char CONST_4X4[190] = {
    0x00, 0x00, 0x00, 0x00, 0x07, 0x4d, 0x49, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x80, 0xf8, 0x37,
    0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37, 0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37,
    0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37, 0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37,
    0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37, 0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37,
    0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37, 0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37,
    0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37, 0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37,
    0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37, 0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37,
    0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37, 0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37,
    0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c, 0xf8, 0x37, 0xf8, 0x37, 0xf8, 0x37, 0x00, 0x3c,
};
#define IMG_DATA 62      /* the first texel: a 42-byte header and one 20-byte mip record */

static unsigned rd_le16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

/* A half float's value (written here, apart from the product's). */
static float half_value(unsigned h)
{
    int e = (int)(h >> 10) & 31, m = (int)(h & 1023);
    float v = e == 0 ? (float)ldexp((double)m, -24) : (float)ldexp(1.0 + m / 1024.0, e - 15);
    return (h & 0x8000u) ? -v : v;
}

static void test_image(void)
{
    /* each pair: a float and the half the nearest-even rule gives (ties to the even mantissa) */
    static const struct { float in; unsigned half; } H[] = {
        { 0.0f, 0x0000 }, { 1.0f, 0x3c00 }, { -2.0f, 0xc000 }, { 0.5f, 0x3800 }, { 0.1f, 0x2e66 },
        { 3.14159274f, 0x4248 }, { 65504.0f, 0x7bff }, { 65519.0f, 0x7bff }, { 65520.0f, 0x7c00 },
        { 1.0009765625f, 0x3c01 },        /* 1 + 2^-10: exact */
        { 1.00048828125f, 0x3c00 },       /* 1 + 2^-11: a tie, to the even 0x3c00 */
        { 1.00146484375f, 0x3c02 },       /* 1 + 3 x 2^-11: a tie, to the even 0x3c02 */
        { 5.9604645e-8f, 0x0001 },        /* 2^-24, the smallest subnormal */
        { 2.9802322e-8f, 0x0000 },        /* 2^-25: a tie, to the even 0 */
        { 8.9406967e-8f, 0x0002 },        /* 1.5 x 2^-24: a tie, to the even 2 */
        { 6.1035156e-5f, 0x0400 },        /* 2^-14, the smallest normal */
    };
    float tex[4][4], vec[28][4];
    unsigned char *b = NULL;
    size_t n = 0;
    unsigned k;
    int i;
    printf("-- the parameter image\n");
    /* a 4 x 4 image of the research repository's constant is that file, byte for byte */
    {
        float c[16][4];
        for (i = 0; i < 16; i++) { c[i][0] = c[i][1] = c[i][2] = 0.498046875f; c[i][3] = 1.0f; }
        CHECK(sh_modprog_bimage_rgba16f(4, 4, c, &b, &n) == 1 && n == sizeof CONST_4X4 && !memcmp(b, CONST_4X4, n));
        free(b);
        b = NULL;
    }
    /* the rounding rule */
    for (i = 0; i < (int)(sizeof H / sizeof H[0]); i += 4) {
        for (k = 0; k < 4; k++) tex[k][0] = tex[k][1] = tex[k][2] = tex[k][3] = 0.0f;
        for (k = 0; k < 4 && (unsigned)i + k < sizeof H / sizeof H[0]; k++) tex[k][0] = H[i + k].in;
        CHECK(sh_modprog_bimage_rgba16f(4, 1, tex, &b, &n) == 1 && n == IMG_DATA + 32);
        for (k = 0; b && k < 4 && (unsigned)i + k < sizeof H / sizeof H[0]; k++) {
            if (rd_le16(b + IMG_DATA + k * 8) != H[i + k].half)
                fprintf(stderr, "half of %.9g: %04x, want %04x\n", H[i + k].in, rd_le16(b + IMG_DATA + k * 8), H[i + k].half);
            CHECK(rd_le16(b + IMG_DATA + k * 8) == H[i + k].half);
        }
        free(b);
        b = NULL;
    }
    /* the parameter image: 8 x 4, vector k at texel ( k % 8, k / 8 ) */
    for (k = 0; k < 28; k++) { vec[k][0] = (float)k; vec[k][1] = (float)k + 0.5f; vec[k][2] = -(float)k; vec[k][3] = 0.25f; }
    CHECK(sh_modprog_param_image(vec, 28, &b, &n) == 1 && n == IMG_DATA + 256);
    if (b) {
        CHECK(rd_be32(b + 4) == 0x074d4942u && rd_be32(b + 12) == 8 && rd_be32(b + 16) == 4 && rd_be32(b + 24) == 1);
        CHECK(b[0x20] == 2 && b[0x24] == 5 && rd_be32(b + 0x2a) == 0 && rd_be32(b + 0x32) == 8 && rd_be32(b + 0x36) == 4 &&
              rd_be32(b + 0x3a) == 256);
        for (k = 0; k < 32; k++) {
            unsigned x = k % 8, y = k / 8, c;
            for (c = 0; c < 4; c++) {
                float want = k < 28 ? vec[k][c] : 0.0f;
                CHECK(half_value(rd_le16(b + IMG_DATA + (y * 8 + x) * 8 + c * 2)) == want);
            }
        }
    }
    free(b);
    b = NULL;
    CHECK(sh_modprog_param_image(vec, 33, &b, &n) == 0 && b == NULL);
    CHECK(sh_modprog_bimage_rgba16f(0, 4, tex, &b, &n) == 0);
}

/* ------------------------------------------------------------------ the registered images */

static void test_registry(void)
{
    float vec[28][4];
    unsigned k;
    char *p1, *p2, hex[SH_MB_HASH_HEX + 1], sha[SH_MB_HASH_HEX + 1], where[32];
    const char *r1, *r2, *r3;
    unsigned char *b = NULL, *img = NULL;
    size_t n = 0, in = 0;
    unsigned long long sz = 0;
    seen s = { 0 };
    static const char head[] = "generated/image/snapmapplus/look/";
    printf("-- the registered parameter images\n");
    sh_modprog_reset();
    for (k = 0; k < 28; k++) { vec[k][0] = (float)k; vec[k][1] = 1.0f; vec[k][2] = 0.5f; vec[k][3] = 0.0f; }
    CHECK(sh_modprog_param_image(vec, 28, &img, &in) == 1);
    sh_mb_sha256_hex(img, in, hex);
    r1 = sh_modprog_register_param_image(vec, 28);
    CHECK(r1 != NULL);
    if (!r1) { free(img); return; }
    p1 = sh_textf("%s", r1);
    /* the hash of the image's bytes, 16 hex digits, lower case */
    CHECK(strlen(p1) == strlen(head) + 16 + 7 && !strncmp(p1, head, strlen(head)) &&
          !strncmp(p1 + strlen(head), hex, 16) && !strcmp(p1 + strlen(head) + 16, ".bimage"));
    /* the same vectors, the same path; another vector, another */
    r2 = sh_modprog_register_param_image(vec, 28);
    CHECK(r2 && !strcmp(r2, p1));
    vec[3][1] = 2.0f;
    r3 = sh_modprog_register_param_image(vec, 28);
    CHECK(r3 && strcmp(r3, p1) != 0);
    vec[3][1] = 1.0f;
    /* served: owned, read back as the image's bytes, in the product's container */
    CHECK(sh_modprog_owns(p1));
    CHECK(sh_modprog_read(p1, &b, &n) == SH_MB_STOCK_OK && n == in && b && !memcmp(b, img, n));
    if (b) HeapFree(GetProcessHeap(), 0, b);
    b = NULL;
    p2 = sh_textf("%s%s", head, "0000000000000000.bimage");
    CHECK(!sh_modprog_owns(p2) && sh_modprog_read(p2, &b, &n) == SH_MB_STOCK_ABSENT);
    free(p2);
    CHECK(sh_modstock_identify(p1, sha, &sz) == SH_MB_STOCK_OK && sz == in && !strcmp(sha, hex));
    CHECK(sh_modstock_container(p1, sha, where, sizeof where) == SH_MB_STOCK_OK && !strcmp(where, SH_MODPROG_CONTAINER));
    sh_modprog_list(head, count, &s);
    CHECK(s.files == 2);
    sh_modprog_reset();
    CHECK(!sh_modprog_owns(p1));
    free(p1);
    free(img);
}

/* ---- old material keys ---- */

static char *g_fixtures;
static char *read_fixture(const char *name, size_t *n)
{
    char path[MAX_PATH];
    FILE *f;
    char *t;
    long sz;
    snprintf(path, sizeof path, "%s\\%s", g_fixtures ? g_fixtures : ".", name);
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

/* The value of a key line of a material (a pointer to it), or NULL; `nth` counts the lines of the key. */
static const char *mat_value(const char *text, const char *key, int nth)
{
    size_t k = strlen(key);
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n'), *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (!strncmp(q, key, k) && (q[k] == ' ' || q[k] == '\t')) {
            if (!nth--) { q += k; while (*q == ' ' || *q == '\t') q++; return q; }
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}
static int mat_count(const char *text, const char *key)
{
    int n = 0;
    while (mat_value(text, key, n)) n++;
    return n;
}
/* A value as up to four floats, `{ a, b, c, d }` or a scalar; the count read. */
static int mat_floats(const char *v, float out[4])
{
    int k = 0;
    char *e;
    if (!v) return 0;
    if (*v == '{') v++;
    while (k < 4) {
        while (*v == ' ' || *v == ',') v++;
        out[k] = strtof(v, &e);
        if (e == v) break;
        v = e;
        k++;
    }
    return k;
}
/* The text of a key's value up to its line's end (no terminator), in a static buffer. */
static const char *mat_line(const char *text, const char *key)
{
    static char t[512];
    const char *v = mat_value(text, key, 0);
    size_t n = 0;
    if (!v) return "";
    while (v[n] && v[n] != '\r' && v[n] != '\n' && n < sizeof t - 1) n++;
    memcpy(t, v, n);
    t[n] = 0;
    return t;
}

/* A particle material's rewrite: each stock key holds exactly the floats the mapping says, bit for bit. */
static void check_particle(const char *label, const char *text, size_t len, const float want[5][4], int sheet)
{
    static const char *const keys[5] = { "particleuberparms1", "particleuberparms2", "particleuberparms3", "emissive",
                                         "smokebrightness" };
    static const char *const old[] = { "eternalparticle", "eternalparticlefade", "eternalparticlecolor",
                                       "eternalparticlecolor2", "eternalparticleemissivemap" };
    char *out = NULL;
    size_t on = 0;
    int k, c, r = sh_modprog_legacy_material(text, len, &out, &on);
    CHECK(r == 1 && out && on == strlen(out));
    if (!out) return;
    for (k = 0; k < 5; k++) {
        float got[4];
        int cnt = mat_floats(mat_value(out, keys[k], 0), got);
        if (cnt != 4 || mat_count(out, keys[k]) != 1) { CHECK(cnt == 4 && mat_count(out, keys[k]) == 1); continue; }
        for (c = 0; c < 4; c++) {
            if (memcmp(&got[c], &want[k][c], 4)) fprintf(stderr, "%s: %s[%d] = %.9g, want %.9g\n", label, keys[k], c, got[c], want[k][c]);
            CHECK(!memcmp(&got[c], &want[k][c], 4));
        }
    }
    for (k = 0; k < 5; k++) CHECK(mat_count(out, old[k]) == 0);
    CHECK(sheet ? mat_count(out, "particleemissivemap") == 1 : mat_count(out, "particleemissivemap") == 0);
    /* nothing else changed: the stage's other keys stay and a second pass finds nothing to do */
    CHECK(!strcmp(mat_line(out, "transmap"), mat_line(text, "transmap")) &&
          !strcmp(mat_line(out, "texanimdims"), mat_line(text, "texanimdims")) &&
          !strcmp(mat_line(out, "virtualtexturefeedbackfloat"), mat_line(text, "virtualtexturefeedbackfloat")));
    if (sheet) CHECK(!strcmp(mat_line(out, "particleemissivemap"), mat_line(text, "eternalparticleemissivemap")));
    {
        char *again = NULL;
        size_t an = 0;
        CHECK(sh_modprog_legacy_material(out, on, &again, &an) == 0 && again == NULL);
    }
    free(out);
}

static void test_legacy_particle(void)
{
    size_t n = 0;
    char *t;
    printf("-- old particle keys\n");
    /* an explosion stage with an emissive sheet (the tyrant's exp_m_02): mode 4 + 16, near fade 52.49 to 26.25, soft
     * distance 21, gradients (1, 0.0999, 0) and (1, 0.1119, 0.0222), emissive multiplier 10, mask multiplier 1 */
    t = read_fixture("legacy_particle_explosion.decl", &n);
    if (t) {
        static const float want[5][4] = {
            { 0.0f, 0.0f, 20.0f, 0.0f },
            { 1.0f, 0.099899f, 0.0f, 52.493438f },
            { 1.0f, 0.111932f, 0.022174f, 10.0f },
            { 1.0f, 26.246719f, 20.997376f, 1.0f },
            { 1.0f, 0.0f, 0.0f, 0.0f },
        };
        check_particle("explosion", t, n, want, 1);
        free(t);
    }
    /* an unlit stage with no sheet (fir_l): mode 0, emissive multiplier 1 */
    t = read_fixture("legacy_particle_unlit.decl", &n);
    if (t) {
        static const float want[5][4] = {
            { 0.0f, 0.0f, 0.0f, 0.0f },
            { 1.0f, 1.0f, 1.0f, 5.249344f },
            { 0.0f, 0.0f, 0.0f, 1.0f },
            { 1.0f, 0.524934f, 5.249344f, 1.0f },
            { 0.0f, 0.0f, 0.0f, 0.0f },
        };
        check_particle("unlit", t, n, want, 0);
        free(t);
    }
    /* a lit stage that names only some old keys and has none of the stock ones: defaults fill the rest, the new lines
     * stand in the old keys' place, in the material's own line ends */
    {
        static const char LIT[] = "{\r\n\tstageprogram\t\"snapmapplus/eternal/uberparticles\"\r\n"
            "\teternalparticle\t{ 1, 2.5, 3, 0.75 }\r\n\teternalparticlefade\t{ 5, 6, 7, 1 }\r\n\ttransmap\tx.tga\r\n}\r\n";
        static const float want[5][4] = {
            { 0.0f, 0.0f, 17.0f, 0.75f },
            { 1.0f, 1.0f, 1.0f, 5.0f },
            { 1.0f, 1.0f, 1.0f, 2.5f },
            { 1.0f, 6.0f, 7.0f, 1.0f },
            { 3.0f, 0.0f, 0.0f, 0.0f },
        };
        char *out = NULL;
        size_t on = 0, i;
        check_particle("lit", LIT, strlen(LIT), want, 0);
        CHECK(sh_modprog_legacy_material(LIT, strlen(LIT), &out, &on) == 1 && out);
        if (out) {
            const char *a = strstr(out, "particleuberparms1"), *tm = strstr(out, "transmap");
            int bare = 0;
            for (i = 0; i < on; i++) if (out[i] == '\n' && (i == 0 || out[i - 1] != '\r')) bare = 1;
            CHECK(a && tm && a < tm && !bare);
            free(out);
        }
    }
    /* a material with no old key is left alone */
    {
        char *out = (char *)1;
        size_t on = 5;
        CHECK(sh_modprog_legacy_material("{\n\tdiffusemap\tx.tga\n}\n", 22, &out, &on) == 0 && out == NULL && on == 0);
    }
}

/* The 28 vectors a layered material gives, from its text (every key set), in the table's order. */
static int read_vectors(const char *text, float v[28][4])
{
    unsigned k;
    for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++)
        if (mat_floats(mat_value(text, SH_MODPROG_ML_PARMS[k], 0), v[k]) < 1) { fprintf(stderr, "no %s\n", SH_MODPROG_ML_PARMS[k]); return 0; }
    return 1;
}

/* A layered material's rewrite: `layerparms` names a registered image, vector k of the image (within half
 * precision) is the vector the material gave, and the extra map the height took (if one) holds its image. Prints
 * the largest relative error of each vector. */
static void check_layered(const char *label, const char *text, size_t len, float src, float cst, const char *extra_key,
                          const char *extra_value)
{
    char *out = NULL, *path = NULL;
    size_t on = 0, in = 0;
    float want[28][4], worst[28], overall = 0.0f;
    unsigned k, c;
    const char *lp;
    unsigned char *img = NULL;
    int r = sh_modprog_legacy_material(text, len, &out, &on);
    CHECK(r == 1 && out);
    if (!out) return;
    CHECK(read_vectors(text, want));
    for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++) CHECK(mat_count(out, SH_MODPROG_ML_PARMS[k]) == 0);
    CHECK(mat_count(out, "blendlayer0heightmap") == 0 && mat_count(out, "layerparms") == 1);
    lp = mat_line(out, "layerparms");
    /* its name: snapmapplus/look/<16 hex>.tga, the path of the registered image */
    CHECK(strlen(lp) == 17 + 16 + 4 && !strncmp(lp, "snapmapplus/look/", 17) && !strcmp(lp + 33, ".tga"));
    path = sh_textf("generated/image/%.*s.bimage", 33, lp);
    CHECK(sh_modprog_owns(path) && sh_modprog_read(path, &img, &in) == SH_MB_STOCK_OK && in == IMG_DATA + 256);
    want[9][1] = src;
    want[9][2] = cst;
    want[9][3] = 0.0f;
    if (img) {
        for (k = 0; k < 28; k++) {
            worst[k] = 0.0f;
            for (c = 0; c < 4; c++) {
                float got = half_value(rd_le16(img + IMG_DATA + k * 8 + c * 2)), w = want[k][c], rel;
                if (w == 0.0f) { CHECK(got == 0.0f); continue; }
                rel = (float)fabs((double)(got - w) / (double)w);
                if (rel > worst[k]) worst[k] = rel;
                if (rel > 4.89e-4f) fprintf(stderr, "%s: vector %u[%u] = %.9g in the image, %.9g given\n", label, k, c, got, w);
                CHECK(rel <= 4.89e-4f);
            }
            if (worst[k] > overall) overall = worst[k];
        }
        for (k = 28; k < 32; k++) for (c = 0; c < 4; c++) CHECK(rd_le16(img + IMG_DATA + k * 8 + c * 2) == 0);
        printf("   %s: largest relative error per vector (0..27):", label);
        for (k = 0; k < 28; k++) printf(" %.2e", worst[k]);
        printf("\n   %s: largest overall %.3e (half precision: 4.88e-04)\n", label, overall);
        HeapFree(GetProcessHeap(), 0, img);
    }
    /* where the new line stands, and what stays */
    {
        const char *a = strstr(out, "layerparms"), *w = strstr(out, "usewoundclip"), *m = strstr(out, "layerblendmask");
        CHECK(a && w && m && w < a && a < m);
        CHECK(!strcmp(mat_line(out, "layerblendmask"), mat_line(text, "layerblendmask")));
        CHECK(!strcmp(mat_line(out, "blendlayer1albedomap"), mat_line(text, "blendlayer1albedomap")));
        CHECK(!strcmp(mat_line(out, "diffusemap"), mat_line(text, "diffusemap")));
        CHECK(!strcmp(mat_line(out, "ambientprogram"), mat_line(text, "ambientprogram")));
        CHECK(!strcmp(mat_line(out, "landpagefile"), mat_line(text, "landpagefile")));
    }
    /* the pool */
    CHECK(mat_count(out, "multilayerextramap0") == 1 && mat_count(out, "multilayerextramap1") == 1);
    if (extra_key) {
        CHECK(!strcmp(mat_line(out, extra_key), extra_value));
        CHECK(!strcmp(mat_line(out, !strcmp(extra_key, "multilayerextramap0") ? "multilayerextramap1" : "multilayerextramap0"),
                      mat_line(text, !strcmp(extra_key, "multilayerextramap0") ? "multilayerextramap1" : "multilayerextramap0")));
    } else {
        CHECK(!strcmp(mat_line(out, "multilayerextramap0"), mat_line(text, "multilayerextramap0")));
        CHECK(!strcmp(mat_line(out, "multilayerextramap1"), mat_line(text, "multilayerextramap1")));
    }
    /* a second pass has nothing left to rewrite; the same material rewrites to the same text */
    {
        char *again = NULL, *same = NULL;
        size_t an = 0, sn = 0;
        CHECK(sh_modprog_legacy_material(out, on, &again, &an) == 0 && again == NULL);
        CHECK(sh_modprog_legacy_material(text, len, &same, &sn) == 1 && same && sn == on && !memcmp(same, out, on));
        free(same);
    }
    free(path);
    free(out);
}

/* A copy of `text` with the line of `key` replaced by `line` (no terminator), or with `line` put before the closing brace. */
static char *with_line(const char *text, const char *key, const char *line)
{
    const char *v = key ? mat_value(text, key, 0) : NULL, *ls, *le;
    size_t head, tail_at;
    char *o;
    if (v) {
        ls = v;
        while (ls > text && ls[-1] != '\n') ls--;
        le = strchr(v, '\n');
        head = (size_t)(ls - text);
        tail_at = le ? (size_t)(le - text) : strlen(text);
    } else {
        le = strrchr(text, '}');
        head = (size_t)(le - text);
        tail_at = head;
    }
    o = (char *)malloc(strlen(text) + strlen(line) + 8);
    memcpy(o, text, head);
    strcpy(o + head, line);
    strcat(o, v ? "" : "\n");
    strcat(o, text + tail_at);
    return o;
}

static void test_legacy_layered(void)
{
    size_t n = 0;
    char *head, *wounds, *t;
    printf("-- old layered keys\n");
    sh_modprog_reset();
    head = read_fixture("legacy_head.decl", &n);
    if (head) {
        /* the tyrant's head: a constant height image (0x37f8 = 0.498046875), both extra maps unused */
        check_layered("tyrant head", head, n, 0.0f, 0.498046875f, NULL, NULL);
        /* a real height image takes a free extra map, the lowest one */
        t = with_line(head, "blendlayer0heightmap", "blendlayer0heightmap\t\"mods/x/ml/base_h.tga\"");
        check_layered("height in extra map 0", t, strlen(t), 1.0f, 0.0f, "multilayerextramap0", "mods/x/ml/base_h.tga");
        /* a layer's specular image holds map 0: the height takes map 1 */
        {
            char *u = with_line(t, "blendlayer2specular", "blendlayer2specular\t{ 0, 0, 0, 1 }");
            check_layered("height in extra map 1", u, strlen(u), 2.0f, 0.0f, "multilayerextramap1", "mods/x/ml/base_h.tga");
            free(u);
        }
        /* both maps held (a burn's tile and cap blood): no room, the neutral height as the constant */
        {
            char *u = with_line(t, "multilayerember", "multilayerember\t{ 1, 0, 1, 2 }");
            check_layered("both extra maps held", u, strlen(u), 0.0f, 0.498046875f, NULL, NULL);
            free(u);
        }
        /* a material that names no height image: the neutral constant */
        {
            char *u = with_line(head, "blendlayer0heightmap", "// no base height");
            check_layered("no height image", u, strlen(u), 0.0f, 0.498046875f, NULL, NULL);
            free(u);
        }
        free(t);
        free(head);
    }
    /* the tyrant's wounds: the specular images of layers 1 to 3 hold the other defaults; its constants as the head's */
    wounds = read_fixture("legacy_wounds.decl", &n);
    if (wounds) {
        check_layered("tyrant wounds", wounds, n, 0.0f, 0.498046875f, NULL, NULL);
        free(wounds);
    }
    sh_modprog_reset();
}

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
            printf("   %s: %d textures over its stages (the engine's 32)\n", bases[i], g_stage_textures);
            CHECK(g_stage_textures > 0 && g_stage_textures <= 32 && (i == 0 || g_stage_textures == 32));
            check_unit(bases[i], pd, pb, pv, pf, spvb, spvn, spfb, spfn);
            printf("   %s permutation: %d textures\n", bases[i], g_stage_textures);
            CHECK(g_stage_textures > 0 && g_stage_textures <= 32 && (i == 0 || g_stage_textures == 32));
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
    test_real_ribbon();
    test_real_recolourable();
    test_real_ep(SH_MODPROG_EP_PREFIX);
    test_real_ep(SH_MODPROG_EP_EXT_PREFIX);
    test_real_ep(SH_MODPROG_EP_EROSION_PREFIX);
    s.files = 0;
    sh_modstock_list("generated/renderprogs/permutations/" SH_MODPROG_EP_PREFIX, count, &s);
    CHECK(s.files == 2);
    test_real_hr();
    s.files = 0;
    sh_modstock_list("generated/renderprogs/permutations/" SH_MODPROG_HR_PREFIX, count, &s);
    CHECK(s.files == 6);      /* each hair base's permutation decl and cooked program */
    if (sh_modprog_cause()[0]) fprintf(stderr, "  %s\n", sh_modprog_cause());
    sh_modstock_reset();
}

int main(int argc, char **argv)
{
    const char *doom = NULL;
    int i;
    for (i = 1; i < argc; ++i) if (!strncmp(argv[i], "doom=", 5) && argv[i][5]) doom = argv[i] + 5;
    for (i = 1; i < argc; ++i) if (!strncmp(argv[i], "dump=", 5) && argv[i][5]) g_dump = argv[i] + 5;
    for (i = 1; i < argc; ++i) if (!strncmp(argv[i], "fixtures=", 9) && argv[i][9]) g_fixtures = argv[i] + 9;
    printf("modprog_test\n");
    test_decl();
    test_glsl();
    test_splice();
    test_derivation();
    test_ep_texts();
    test_ep_splice();
    test_hr_texts();
    test_hr_splice();
    test_image();
    test_registry();
    if (g_fixtures) { test_legacy_particle(); test_legacy_layered(); }
    else printf("-- old material keys: no fixtures given (fixtures=<dir>); skipped\n");
    if (doom) test_real(doom);
    else printf("-- the installed game's programs: no install given (doom=<root>); skipped\n");
    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
