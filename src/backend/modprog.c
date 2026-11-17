/* modprog.c -- render programs the product derives from the installed game's own (modprog.h). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/text.h"
#include "modprog.h"
#include "sha256.h"
#include "modbundle.h"
#include "modstock.h"

/* ------------------------------------------------------------------------------------------- the family ---- */

/* The programs the product derives: each a variant (a family's prefix + the base's name) of one installed base
 * program; a family says what its variant adds (modprog.h). */
typedef char *(*mp_patch_fn)(const char *text, size_t len, size_t *out_len);
typedef char *(*mp_glsl_fn)(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len);
typedef uint32_t *(*mp_splice_fn)(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                                  size_t *out_words, const char **why);
typedef struct mp_family {
    const char *prefix;            /* the variant's name before the base's */
    const char *what;              /* in the failure sentence: "its <what> of the game's <base> program" */
    const char *const *parms;      /* the material uniforms the fragment stage gains */
    const unsigned *nparms;
    const char *const *textures;   /* the textures it gains, after its own */
    const unsigned *ntextures;
    mp_patch_fn decl;
    mp_glsl_fn glsl;
    mp_splice_fn splice;
    const char *decl_needs;        /* why a decl is refused */
} mp_family;
typedef struct mp_base { const char *name; int family; } mp_base;
static const mp_base MP_BASES[] = { { "skinvmtr", 0 }, { "skinvmtrwounds", 0 }, { "uberparticles", 1 } };
#define MP_NBASES (sizeof MP_BASES / sizeof MP_BASES[0])

/* The layered program's 28 vectors, by the material keys they had when each was a render parm: vector k of the
 * parameter image (layered.rp), the name the serve filter maps to it. NOT render parms: the product declares none
 * of them, the program reads the image (the `layerparms` texture). */
const char *const SH_MODPROG_ML_PARMS[] = {
    "layerstilingfactor", "multiblendlayerscale", "multiblendheightmapscale", "multiblendsoftness",
    "multiblendalbedoscale", "multiblendnormalscale", "multiblendspecularscale", "multiblendsmoothnessscale",
    "multiblendemissivescale", "blendheightmapscale", "blendlayer0tiling", "blendlayer0emissive",
    "blendlayer1tiling", "blendlayer2tiling", "blendlayer3tiling", "blendlayer4tiling",
    "blendlayer1specular", "blendlayer2specular", "blendlayer3specular", "blendlayer4specular",
    "blendlayer1emissive", "blendlayer2emissive", "blendlayer3emissive", "blendlayer4emissive",
    "multilayerember", "multilayerembertiling", "multilayerembernoise", "multilayeremberhot",
};
const unsigned SH_MODPROG_ML_PARM_COUNT = sizeof SH_MODPROG_ML_PARMS / sizeof SH_MODPROG_ML_PARMS[0];
/* What a vector the material does not set was (the parms' decl defaults), for a material that sets only some. */
const float SH_MODPROG_ML_DEFAULTS[28][4] = {
    { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 0.25f, 0.25f, 0.25f, 0.25f },
    { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 },
    { 1, 1, 1, 1 }, { 1, 0, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 1, 0 },
    { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 },
    { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
    { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
    { 0, 0, 0, 0 }, { 4, 4, 0, 0 }, { 1, 4, 0.7f, 0.4f }, { 1, 0.07f, 0, 0 },
};
#define ML_DEFAULTS SH_MODPROG_ML_DEFAULTS
/* The layered program adds nothing to the fragment stage's material uniforms. */
static const char *const ML_BLOCK_PARMS[] = { NULL };
static const unsigned ML_NBLOCK_PARMS = 0;
const char *const SH_MODPROG_ML_TEXTURES[] = {
    "layerblendmask", "layerparms",
    "blendlayer1albedomap", "blendlayer1normalmap", "blendlayer1smoothnessmap", "blendlayer1heightmap",
    "blendlayer2albedomap", "blendlayer2normalmap", "blendlayer2smoothnessmap", "blendlayer2heightmap",
    "blendlayer3albedomap", "blendlayer3normalmap", "blendlayer3smoothnessmap", "blendlayer3heightmap",
    "blendlayer4albedomap", "blendlayer4normalmap", "blendlayer4smoothnessmap", "blendlayer4heightmap",
    "multilayerextramap0", "multilayerextramap1",
};
const unsigned SH_MODPROG_ML_TEXTURE_COUNT = sizeof SH_MODPROG_ML_TEXTURES / sizeof SH_MODPROG_ML_TEXTURES[0];
/* The module's samplers, in the textures' order. */
static const char *const MP_SAMPLERS[] = {
    "ml_mask", "ml_params",
    "ml_albedo1", "ml_normal1", "ml_smoothness1", "ml_height1", "ml_albedo2", "ml_normal2", "ml_smoothness2",
    "ml_height2", "ml_albedo3", "ml_normal3", "ml_smoothness3", "ml_height3", "ml_albedo4", "ml_normal4",
    "ml_smoothness4", "ml_height4", "ml_extra0", "ml_extra1",
};
#define MP_NSAMPLERS (sizeof MP_SAMPLERS / sizeof MP_SAMPLERS[0])
/* The program's emissive factors ml_blend recomputes the emissive term from, and the lighting inputs it changes
 * (then reads: texCoords). */
static const char *const MP_EMISSIVE[] = { "bloommaskscale", "colorscale", "color", "bloomcolor" };
static const char *const MP_INOUT[] = { "albedo", "specular", "smoothness", "normal", "lightmap", "emissive",
                                         "thickness" };
#define MP_NINOUT 7

/* The particle program (programs/shaders/particles.rp): the parameters appended to the fragment stage's material
 * uniforms (the game's own vector smokebrightness, which no program, fx or engine code names, and two more of the
 * game's own render parms the shading reads), its added texture, the module's samplers (the first three bound to the
 * program's own textures), the stage inputs and the program's own uniforms it reads ('h' the material block, 'l' the
 * view block). The stage's constants live in keys the game's particle program already holds (particleuberparms1..3,
 * emissive) and smokebrightness; no vector render parm is the product's. */
/* The particle program's old keys, in the order of the shading's former parameter array: NOT render parms (the
 * product declares none of them), the names the serve filter maps to the stock keys (particles.rp lists where each
 * went). */
const char *const SH_MODPROG_EP_PARMS[] = {
    "eternalparticle", "eternalparticlefade", "eternalparticlecolor", "eternalparticlecolor2",
};
const unsigned SH_MODPROG_EP_PARM_COUNT = sizeof SH_MODPROG_EP_PARMS / sizeof SH_MODPROG_EP_PARMS[0];
const char *const SH_MODPROG_EP_TEXTURES[] = { "particleemissivemap" };
const unsigned SH_MODPROG_EP_TEXTURE_COUNT = sizeof SH_MODPROG_EP_TEXTURES / sizeof SH_MODPROG_EP_TEXTURES[0];
static const char *const EP_BLOCK_PARMS[] = { "smokebrightness", "projectionmatrixz", "globalvieworigin" };
#define EP_NBLOCK_PARMS (sizeof EP_BLOCK_PARMS / sizeof EP_BLOCK_PARMS[0])
static const char *const EP_SAMPLERS[] = { "ep_trans", "ep_depth", "ep_atlas", "ep_emissive" };
static const char *const EP_SAMPLER_STOCK[] = { "samp_transmap", "samp_viewdepthmap", "samp_particleslightatlasmap", NULL };
#define EP_NSAMPLERS (sizeof EP_SAMPLERS / sizeof EP_SAMPLERS[0])
static const char *const EP_VARYINGS[] = {
    "vofi_Color", "vofi_TexCoord0", "vofi_TexCoord1", "vofi_TexCoord2", "vofi_TexCoord4", "vofi_TexCoord6", "gl_FragCoord",
};
#define EP_NVARYINGS (sizeof EP_VARYINGS / sizeof EP_VARYINGS[0])
static const char *const EP_UNIFORMS[] = {
    "hrenderpositiontoviewtexture", "lparticleatlasparms", "lfoglightinscattercolor", "lfoglightinscatterintensity",
    "lfoglightoppositecolor", "lfogcolor", "ltransparencylightmult",
    "hparticleuberparms1", "hparticleuberparms2", "hparticleuberparms3", "hemissive",
};
#define EP_NUNIFORMS (sizeof EP_UNIFORMS / sizeof EP_UNIFORMS[0])

/* The render parm decls the product derives: textures only. No vector render parm is the product's: the layered
 * program reads its numbers from the `layerparms` image and the particle program from keys of the game's own. A
 * texture a material does not set gives no layer (no mask, a black parameter image), a flat layer image, and no
 * emissive sheet. */
typedef struct mp_parm { const char *name, *text; } mp_parm;
static const mp_parm MP_PARM_DECLS[] = {
    /* No mask, no layer: what an image key the material does not set gives. */
    { "layerblendmask",            "{ Tex2D textures/system/constant_color/black }" },
    { "layerparms",                "{ Tex2D textures/system/constant_color/black }" },
    { "blendlayer1albedomap",      "{ Tex2D textures/system/constant_color/grey_md }" },
    { "blendlayer1normalmap",      "{ Tex2D textures/system/flat_normal_noalpha }" },
    { "blendlayer1smoothnessmap",  "{ Tex2D textures/system/constant_color/white }" },
    { "blendlayer1heightmap",      "{ Tex2D textures/system/constant_color/grey_md }" },
    { "blendlayer2albedomap",      "{ Tex2D textures/system/constant_color/grey_md }" },
    { "blendlayer2normalmap",      "{ Tex2D textures/system/flat_normal_noalpha }" },
    { "blendlayer2smoothnessmap",  "{ Tex2D textures/system/constant_color/white }" },
    { "blendlayer2heightmap",      "{ Tex2D textures/system/constant_color/grey_md }" },
    { "blendlayer3albedomap",      "{ Tex2D textures/system/constant_color/grey_md }" },
    { "blendlayer3normalmap",      "{ Tex2D textures/system/flat_normal_noalpha }" },
    { "blendlayer3smoothnessmap",  "{ Tex2D textures/system/constant_color/white }" },
    { "blendlayer3heightmap",      "{ Tex2D textures/system/constant_color/grey_md }" },
    { "blendlayer4albedomap",      "{ Tex2D textures/system/constant_color/grey_md }" },
    { "blendlayer4normalmap",      "{ Tex2D textures/system/flat_normal_noalpha }" },
    { "blendlayer4smoothnessmap",  "{ Tex2D textures/system/constant_color/white }" },
    { "blendlayer4heightmap",      "{ Tex2D textures/system/constant_color/grey_md }" },
    { "multilayerextramap0",       "{ Tex2D textures/system/constant_color/black }" },
    { "multilayerextramap1",       "{ Tex2D textures/system/constant_color/black }" },
    /* A particle stage with no emissive mask sheet. */
    { "particleemissivemap",       "{ Tex2D textures/system/constant_color/white }" },
};
#define MP_NPARM_DECLS (sizeof MP_PARM_DECLS / sizeof MP_PARM_DECLS[0])

const char *sh_modprog_vector_parm(unsigned i)
{
    (void)i;
    return NULL;   /* the product's programs declare no vector render parm */
}

/* --------------------------------------------------------------------------------------------- helpers ---- */

typedef struct mp_buf { char *p; size_t n, cap; int oom; } mp_buf;

static void mp_put(mp_buf *b, const void *data, size_t n)
{
    if (b->oom || !n) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *g;
        while (cap < b->n + n + 1) cap *= 2;
        if (!(g = (char *)realloc(b->p, cap))) { b->oom = 1; return; }
        b->p = g;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void mp_puts(mp_buf *b, const char *s) { mp_put(b, s, strlen(s)); }
static void mp_printf(mp_buf *b, const char *fmt, ...)
{
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = sh_vtextf(fmt, ap);
    va_end(ap);
    if (!t) { b->oom = 1; return; }
    mp_puts(b, t);
    free(t);
}
static char *mp_take(mp_buf *b, size_t *len)
{
    if (b->oom) { free(b->p); return NULL; }
    if (!b->p) { b->p = (char *)malloc(1); if (!b->p) return NULL; b->p[0] = 0; }
    if (len) *len = b->n;
    return b->p;
}

/* `needle` in [s, s + n), or NULL. */
static const char *mp_find(const char *s, size_t n, const char *needle)
{
    size_t k = strlen(needle), i;
    if (k > n) return NULL;
    for (i = 0; i + k <= n; i++) if (s[i] == needle[0] && !memcmp(s + i, needle, k)) return s + i;
    return NULL;
}

static uint32_t mp_be32(const unsigned char *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t mp_le32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void mp_put_be32(mp_buf *b, uint32_t v)
{ unsigned char x[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
  mp_put(b, x, 4); }
static void mp_put_le32(mp_buf *b, uint32_t v)
{ unsigned char x[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
  mp_put(b, x, 4); }

/* MD5fold of decl text as the engine checksums it: trailing spaces, CR and LF removed. */
static unsigned mp_decl_checksum(const char *t, size_t n)
{
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\r' || t[n - 1] == '\n')) n--;
    return sh_mb_md5fold(t, n);
}

/* `text` (lines ended by '\n') put in `b` one line at a time, each after `prefix` (a blank line stays bare) and
 * followed by `nl`; with `lead` the line end comes before each line instead, for a block that continues the line
 * before it and leaves the last line end to the caller. */
static void mp_put_lines(mp_buf *b, const char *text, const char *prefix, const char *nl, int lead)
{
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (lead) mp_puts(b, nl);
        if (n) { mp_puts(b, prefix); mp_put(b, p, n); }
        if (!lead) mp_puts(b, nl);
        p += n + (e ? 1 : 0);
    }
}

/* The index of the } that closes the { at t[open], comments skipped; len when it is never closed. */
static size_t mp_close_brace(const char *t, size_t len, size_t open)
{
    size_t i;
    int depth = 0;
    for (i = open; i < len; i++) {
        if (t[i] == '/' && i + 1 < len && t[i + 1] == '/') { while (i + 1 < len && t[i + 1] != '\n') i++; }
        else if (t[i] == '/' && i + 1 < len && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i++;
        }
        else if (t[i] == '{') depth++;
        else if (t[i] == '}' && !--depth) return i;
    }
    return len;
}

/* 1 when only blanks precede `at` on its line, with the line's start in *from and the blanks' count in *n. */
static int mp_line_indent(const char *text, const char *at, const char **from, size_t *n)
{
    const char *ls = at;
    while (ls > text && ls[-1] != '\n') ls--;
    for (*from = ls, *n = 0; ls + *n < at; (*n)++) if (ls[*n] != ' ' && ls[*n] != '\t') return 0;
    return 1;
}

/* The decl `text` with the program's functions in its hlsl_fp_header section: a section of their own before its
 * hlsl_fp section, or, when it has one, at the end of that section (which must close on a line of its own). The
 * engine compiles the section before main(), so the functions are real functions. NULL when there is no hlsl_fp
 * section or the base's header cannot be extended. malloc'd. */
static char *mp_add_header(const char *text, size_t len, const char *header, const char *nl, size_t *out_len)
{
    const char *h = mp_find(text, len, "hlsl_fp_header"), *ind;
    size_t in;
    char *prefix;
    mp_buf b = { 0 };
    if (h) {
        const char *open = (const char *)memchr(h, '{', len - (size_t)(h - text));
        size_t close;
        if (!open) return NULL;
        close = mp_close_brace(text, len, (size_t)(open - text));
        if (close >= len || !mp_line_indent(text, text + close, &ind, &in)) return NULL;
        prefix = (char *)malloc(in + 2);
        if (!prefix) return NULL;
        memcpy(prefix, ind, in);
        memcpy(prefix + in, "\t", 2);
        mp_put(&b, text, (size_t)(ind - text));
        mp_put_lines(&b, header, prefix, nl, 0);
        mp_put(&b, ind, len - (size_t)(ind - text));
    } else {
        const char *at = mp_find(text, len, "hlsl_fp {");
        if (!at || !mp_line_indent(text, at, &ind, &in)) return NULL;
        prefix = (char *)malloc(in + 2);
        if (!prefix) return NULL;
        memcpy(prefix, ind, in);
        memcpy(prefix + in, "\t", 2);
        mp_put(&b, text, (size_t)(ind - text));
        mp_put(&b, ind, in);
        mp_puts(&b, "hlsl_fp_header {");
        mp_puts(&b, nl);
        mp_put_lines(&b, header, prefix, nl, 0);
        mp_put(&b, ind, in);
        mp_puts(&b, "}");
        mp_puts(&b, nl);
        mp_put(&b, ind, len - (size_t)(ind - text));
    }
    free(prefix);
    return mp_take(&b, out_len);
}

/* ---------------------------------------------------------------------------------------------- decl ---- */

char *sh_modprog_patch_decl(const char *text, size_t len, size_t *out_len)
{
    static const char key[] = "#template CUSTOM_TEXTURE_INPUTS( inputs ) {";
    const char *nl = mp_find(text, len, "\r\n") ? "\r\n" : "\n";
    const char *k = mp_find(text, len, key), *line, *eol;
    mp_buf b = { 0 };
    char *call, *ind, *t;
    size_t n;
    if (!k || mp_find(k + 1, len - (size_t)(k + 1 - text), key)) return NULL;
    line = k;
    while (line > text && line[-1] != '\n') line--;
    eol = mp_find(k, len - (size_t)(k - text), nl);
    if (!eol) return NULL;
    /* the call, as the template's first statements: one tab in from the template's line */
    if (!(ind = (char *)malloc((size_t)(k - line) + 2))) return NULL;
    memcpy(ind, line, (size_t)(k - line));
    memcpy(ind + (k - line), "\t", 2);
    mp_put(&b, text, (size_t)(eol - text));
    mp_put_lines(&b, SH_MODPROG_ML_CALL, ind, nl, 1);
    mp_put(&b, eol, len - (size_t)(eol - text));
    free(ind);
    if (!(call = mp_take(&b, &n))) return NULL;
    /* the functions, in the header section */
    t = mp_add_header(call, n, SH_MODPROG_ML_HEADER, nl, out_len);
    free(call);
    return t;
}

/* ---------------------------------------------------------------------------------------------- GLSL ---- */

char *sh_modprog_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len)
{
    static const char bind[] = "layout( binding = ";
    static const char ubo[] = "uniform freqHigh_fragmentUniforms_ubo {";
    static const char anchor[] = "inputs.normal = sampleNormal.wyz;\n";
    mp_buf b = { 0 }, s = { 0 };
    const char *p = text, *end = text + len, *u, *close, *first, *mainp, *a, *ls, *ind;
    unsigned k;
    char *t1;
    size_t n1;
    /* 1. bindings at or after tex_end move up */
    while (p < end) {
        const char *q = mp_find(p, (size_t)(end - p), bind);
        if (!q) { mp_put(&s, p, (size_t)(end - p)); break; }
        q += sizeof bind - 1;
        mp_put(&s, p, (size_t)(q - p));
        {
            unsigned v = 0;
            const char *d = q;
            while (d < end && *d >= '0' && *d <= '9') v = v * 10 + (unsigned)(*d++ - '0');
            if (d == q) { p = q; continue; }
            mp_printf(&s, "%u", v >= tex_end ? v + shift : v);
            p = d;
        }
    }
    if (s.oom) { free(s.p); return NULL; }
    t1 = s.p;
    n1 = s.n;
    end = t1 + n1;
    /* 2. the material uniform block (the layered program adds no parameter to it) */
    if (!(u = mp_find(t1, n1, ubo)) || !(close = memchr(u, '}', (size_t)(end - u)))) { free(t1); return NULL; }
    /* 3. the samplers before the program's first struct (or its first input) */
    first = mp_find(t1, n1, "struct clusternumlights_t");
    if (!first) first = mp_find(t1, n1, "layout( location");
    mainp = mp_find(t1, n1, "void main() {");
    a = mp_find(t1, n1, anchor);
    if (!first || !mainp || !a || first < close || mainp < first || a < mainp) { free(t1); return NULL; }
    a += sizeof anchor - 1;
    ls = a - (sizeof anchor - 1);
    ind = ls;
    while (ind > t1 && ind[-1] != '\n') ind--;
    mp_put(&b, t1, (size_t)(close - t1));
    for (k = 0; k < ML_NBLOCK_PARMS; k++) mp_printf(&b, "\tvec4 %s;\n", ML_BLOCK_PARMS[k]);
    mp_put(&b, close, (size_t)(first - close));
    for (k = 0; k < SH_MODPROG_ML_TEXTURE_COUNT; k++)
        mp_printf(&b, "layout( binding = %u ) uniform sampler2D samp_%s;\n", tex_end + k, SH_MODPROG_ML_TEXTURES[k]);
    mp_put(&b, first, (size_t)(mainp - first));
    mp_puts(&b, SH_MODPROG_ML_GLSL);
    mp_put(&b, mainp, (size_t)(a - mainp));
    /* 4. the call: the indent of the anchor's line */
    {
        const char *w = ind;
        while (w < ls && (*w == ' ' || *w == '\t')) w++;
        mp_put(&b, ind, (size_t)(w - ind));
    }
    mp_puts(&b, "ml_blend( inputs.albedo, inputs.specular, inputs.smoothness, inputs.normal, inputs.lightmap, "
                "inputs.emissive, inputs.thickness, inputs.texCoords, freqLow_fragmentUniforms.time.x, vec4[4]( ");
    for (k = 0; k < 4; k++) mp_printf(&b, "%sfreqHigh_fragmentUniforms.%s", k ? ", " : "", MP_EMISSIVE[k]);
    mp_puts(&b, " ) );\n");
    mp_put(&b, a, (size_t)(end - a));
    free(t1);
    return mp_take(&b, out_len);
}

/* --------------------------------------------------------------------------- the particle texts ---- */

char *sh_modprog_ep_patch_decl(const char *text, size_t len, size_t *out_len)
{
    static const char anchor[] = "PREMULTIPLY_ALPHA_BLENDED( result );";
    const char *nl = mp_find(text, len, "\r\n") ? "\r\n" : "\n", *end = text + len;
    const char *fp = mp_find(text, len, "hlsl_fp {"), *call, *eol, *ls;
    mp_buf b = { 0 };
    char *prefix, *t;
    size_t n;
    if (!fp) return NULL;
    call = mp_find(fp, (size_t)(end - fp), anchor);
    if (!call || mp_find(call + 1, (size_t)(end - call - 1), anchor)) return NULL;
    if (!(eol = mp_find(call, (size_t)(end - call), nl))) return NULL;
    for (ls = call; ls > fp && ls[-1] != '\n'; ) ls--;
    eol += strlen(nl);
    /* the call after the premultiply, at its indentation */
    if (!(prefix = (char *)malloc((size_t)(call - ls) + 1))) return NULL;
    memcpy(prefix, ls, (size_t)(call - ls));
    prefix[call - ls] = 0;
    mp_put(&b, text, (size_t)(eol - text));
    mp_put_lines(&b, SH_MODPROG_EP_CALL, prefix, nl, 0);
    mp_put(&b, eol, (size_t)(end - eol));
    free(prefix);
    if (!(t = mp_take(&b, &n))) return NULL;
    /* the functions, in the header section */
    prefix = mp_add_header(t, n, SH_MODPROG_EP_HEADER, nl, out_len);
    free(t);
    return prefix;
}

char *sh_modprog_ep_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len)
{
    static const char bind[] = "layout( binding = ";
    static const char ubo[] = "uniform freqHigh_fragmentUniforms_ubo {";
    mp_buf b = { 0 }, s = { 0 };
    const char *p = text, *end = text + len, *u, *close, *first, *mainp, *last;
    unsigned k;
    char *t1;
    size_t n1;
    while (p < end) {
        const char *q = mp_find(p, (size_t)(end - p), bind);
        if (!q) { mp_put(&s, p, (size_t)(end - p)); break; }
        q += sizeof bind - 1;
        mp_put(&s, p, (size_t)(q - p));
        {
            unsigned v = 0;
            const char *d = q;
            while (d < end && *d >= '0' && *d <= '9') v = v * 10 + (unsigned)(*d++ - '0');
            if (d == q) { p = q; continue; }
            mp_printf(&s, "%u", v >= tex_end ? v + shift : v);
            p = d;
        }
    }
    if (s.oom) { free(s.p); return NULL; }
    t1 = s.p;
    n1 = s.n;
    end = t1 + n1;
    if (!(u = mp_find(t1, n1, ubo)) || !(close = memchr(u, '}', (size_t)(end - u)))) { free(t1); return NULL; }
    first = mp_find(t1, n1, "layout( location");
    mainp = mp_find(t1, n1, "void main() {");
    /* the end of main: its closing brace, the text's last */
    last = end;
    while (last > t1 && (last[-1] == '\n' || last[-1] == '\r' || last[-1] == ' ' || last[-1] == '\t')) last--;
    if (!first || !mainp || last <= mainp || last[-1] != '}' || first < close || mainp < first) { free(t1); return NULL; }
    last--;
    mp_put(&b, t1, (size_t)(close - t1));
    for (k = 0; k < EP_NBLOCK_PARMS; k++) mp_printf(&b, "\tvec4 %s;\n", EP_BLOCK_PARMS[k]);
    mp_put(&b, close, (size_t)(first - close));
    for (k = 0; k < SH_MODPROG_EP_TEXTURE_COUNT; k++)
        mp_printf(&b, "layout( binding = %u ) uniform sampler2D samp_%s;\n", tex_end + k, SH_MODPROG_EP_TEXTURES[k]);
    mp_put(&b, first, (size_t)(mainp - first));
    mp_puts(&b, SH_MODPROG_EP_GLSL);
    mp_put(&b, mainp, (size_t)(last - mainp));
    mp_puts(&b, "\tep_shade( out_FragColor0, vec4[");
    mp_printf(&b, "%u]( ", (unsigned)EP_NVARYINGS);
    for (k = 0; k < EP_NVARYINGS; k++) mp_printf(&b, "%s%s", k ? ", " : "", EP_VARYINGS[k]);
    mp_printf(&b, " ), vec4[%u]( ", (unsigned)EP_NBLOCK_PARMS);
    for (k = 0; k < EP_NBLOCK_PARMS; k++) mp_printf(&b, "%sfreqHigh_fragmentUniforms.%s", k ? ", " : "", EP_BLOCK_PARMS[k]);
    mp_printf(&b, " ), vec4[%u]( ", (unsigned)EP_NUNIFORMS);
    for (k = 0; k < EP_NUNIFORMS; k++)
        mp_printf(&b, "%s%s_fragmentUniforms.%s", k ? ", " : "", EP_UNIFORMS[k][0] == 'h' ? "freqHigh" : "freqLow",
                  EP_UNIFORMS[k] + 1);
    mp_puts(&b, " ) );\n");
    mp_put(&b, last, (size_t)(end - last));
    free(t1);
    return mp_take(&b, out_len);
}

/* --------------------------------------------------------------------------------------------- SPIR-V ---- */
/* A module as a list of instructions; an instruction's words (opcode word first) point into the input or into
 * an owned copy. Only the opcodes glslang emits for programs/shaders/layered.rp need their operand shapes (the
 * multilayer module is renumbered whole); the stock module is only searched and amended. */

enum {
    OpName = 5, OpMemberName = 6, OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28,
    OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33, OpConstantTrue = 41,
    OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44, OpConstantNull = 46, OpFunction = 54,
    OpFunctionParameter = 55, OpFunctionEnd = 56, OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62,
    OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80,
    OpCompositeExtract = 81, OpCompositeInsert = 82, OpLabel = 248, OpLoopMerge = 246, OpSelectionMerge = 247,
    OpBranch = 249, OpBranchConditional = 250, OpReturn = 253, OpReturnValue = 254, OpSource = 3,
    OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88, OpImageFetch = 95, OpImage = 100, OpVectorExtractDynamic = 77, OpUndef = 1,
    OpPhi = 245, OpKill = 252
};

typedef struct mp_ins { uint32_t *w; } mp_ins;     /* w[0]: count << 16 | opcode */
#define MP_OP(i) ((i)->w[0] & 0xffffu)
#define MP_N(i) ((i)->w[0] >> 16)

typedef struct mp_mod {
    mp_ins *ins;
    size_t n, cap;
    uint32_t bound;
    const uint32_t *head;
    uint32_t **owned;
    size_t nowned, cowned;
    int oom;
} mp_mod;

static void mp_mod_free(mp_mod *m)
{
    size_t i;
    for (i = 0; i < m->nowned; i++) free(m->owned[i]);
    free(m->owned);
    free(m->ins);
    memset(m, 0, sizeof *m);
}

static uint32_t *mp_own(mp_mod *m, size_t words)
{
    uint32_t *w;
    if (m->nowned == m->cowned) {
        size_t c = m->cowned ? m->cowned * 2 : 256;
        uint32_t **g = (uint32_t **)realloc(m->owned, c * sizeof *g);
        if (!g) { m->oom = 1; return NULL; }
        m->owned = g;
        m->cowned = c;
    }
    if (!(w = (uint32_t *)calloc(words ? words : 1, sizeof *w))) { m->oom = 1; return NULL; }
    m->owned[m->nowned++] = w;
    return w;
}

static int mp_parse(mp_mod *m, const uint32_t *w, size_t n)
{
    size_t at = 5;
    memset(m, 0, sizeof *m);
    if (n < 5 || w[0] != 0x07230203u) return 0;
    m->head = w;
    m->bound = w[3];
    while (at < n) {
        uint32_t c = w[at] >> 16;
        if (!c || at + c > n) return 0;
        if (m->n == m->cap) {
            size_t cap = m->cap ? m->cap * 2 : 4096;
            mp_ins *g = (mp_ins *)realloc(m->ins, cap * sizeof *g);
            if (!g) return 0;
            m->ins = g;
            m->cap = cap;
        }
        m->ins[m->n++].w = (uint32_t *)(w + at);
        at += c;
    }
    return 1;
}

/* Whether the opcode has a result type and a result id. */
static int mp_has_type(uint32_t op)
{
    switch (op) {
    case OpTypeVoid: case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix:
    case OpTypeImage: case OpTypeSampler: case OpTypeSampledImage: case OpTypeArray: case OpTypeRuntimeArray:
    case OpTypeStruct: case OpTypePointer: case OpTypeFunction: case OpExtInstImport: case OpLabel:
    case OpStore: case OpFunctionEnd: case OpSelectionMerge: case OpLoopMerge: case OpBranch:
    case OpBranchConditional: case OpReturn: case OpReturnValue: case OpName: case OpMemberName: case OpDecorate:
    case OpMemberDecorate: case OpCapability: case OpEntryPoint: case OpExecutionMode: case OpSource: case OpKill:
    case 2: case 4: case 8: case 10: case 14: case 317: case 330:
        return 0;
    default:
        return 1;
    }
}
static int mp_has_result(uint32_t op)
{
    switch (op) {
    case OpStore: case OpFunctionEnd: case OpSelectionMerge: case OpLoopMerge: case OpBranch:
    case OpBranchConditional: case OpReturn: case OpReturnValue: case OpName: case OpMemberName: case OpDecorate:
    case OpMemberDecorate: case OpCapability: case OpEntryPoint: case OpExecutionMode: case OpSource: case OpKill:
    case 2: case 4: case 8: case 10: case 14: case 317: case 330:   /* SourceContinued, SourceExtension, Line,
                                                                     * Extension, MemoryModel, NoLine,
                                                                     * ModuleProcessed */
        return 0;
    default:
        return 1;
    }
}
static uint32_t mp_rid(const mp_ins *i)
{
    uint32_t op = MP_OP(i);
    if (!mp_has_result(op)) return 0;
    return mp_has_type(op) ? i->w[2] : i->w[1];
}
static int mp_is_type(uint32_t op) { return op >= OpTypeVoid && op <= OpTypeFunction && op != 31; }
static int mp_is_const(uint32_t op)
{ return op == OpConstantTrue || op == OpConstantFalse || op == OpConstant || op == OpConstantComposite ||
         op == OpConstantNull; }

/* The id operands of an instruction (word indices into w, result type and id included); -1 for an opcode whose
 * shape is not known. */
static int mp_ids(const mp_ins *in, uint32_t out[], int cap)
{
    uint32_t op = MP_OP(in), n = MP_N(in), k;
    int c = 0, head = (mp_has_type(op) ? 1 : 0) + (mp_has_result(op) ? 1 : 0);
#define ADD(x) do { if (c < cap) out[c++] = (x); } while (0)
    for (k = 1; k <= (uint32_t)head; k++) ADD(k);
    switch (op) {
    case OpTypeVoid: case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeSampler: case OpConstantTrue:
    case OpConstantFalse: case OpConstant: case OpConstantNull: case OpFunctionParameter: case OpLabel:
    case OpReturn: case OpFunctionEnd: case OpExtInstImport: case OpUndef: case OpKill:
        break;
    case OpTypeVector: case OpTypeMatrix: case OpTypeImage: ADD(2); break;
    case OpTypePointer: ADD(3); break;
    case OpTypeSampledImage: case OpTypeArray: case OpTypeRuntimeArray: case OpTypeStruct: case OpTypeFunction:
        for (k = 2; k < n; k++) ADD(k);
        break;
    case OpFunction: ADD(4); break;
    case OpVariable: if (n > 4) ADD(4); break;
    case OpLoad: ADD(3); break;
    case OpStore: ADD(1); ADD(2); break;
    case OpVectorShuffle: ADD(3); ADD(4); break;
    case OpVectorExtractDynamic: ADD(3); ADD(4); break;
    case OpCompositeExtract: ADD(3); break;
    case OpCompositeInsert: ADD(3); ADD(4); break;
    case OpExtInst: ADD(3); for (k = 5; k < n; k++) ADD(k); break;
    case OpImageSampleImplicitLod: case OpImageSampleExplicitLod: case OpImageFetch:
        ADD(3); ADD(4); for (k = 6; k < n; k++) ADD(k);
        break;
    case OpImage: ADD(3); break;
    case OpSelectionMerge: ADD(1); break;
    case OpLoopMerge: ADD(1); ADD(2); break;
    case OpBranch: case OpReturnValue: ADD(1); break;
    case OpBranchConditional: ADD(1); ADD(2); ADD(3); break;
    default:
        /* the value instructions: every operand an id */
        if ((op >= 126 && op <= 157) || (op >= 164 && op <= 191) || (op >= 109 && op <= 112) || op == 124 ||
            op == OpConstantComposite || op == OpFunctionCall || op == OpAccessChain ||
            op == OpCompositeConstruct || op == OpPhi || op == 207 || op == 208) {
            for (k = (uint32_t)head + 1; k < n; k++) ADD(k);
            break;
        }
        return -1;
    }
#undef ADD
    return c;
}

static char *mp_str(const uint32_t *w, size_t words)
{
    size_t n = strnlen((const char *)w, words * 4);
    char *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, w, n);
    s[n] = 0;
    return s;
}

/* OpName lookups: the id named `name` (the only one), or 0. */
static uint32_t mp_named(const mp_mod *m, const char *name, int prefix)
{
    size_t i, k = strlen(name);
    uint32_t hit = 0;
    for (i = 0; i < m->n; i++) {
        const mp_ins *in = &m->ins[i];
        const char *s;
        if (MP_OP(in) != OpName) continue;
        s = (const char *)(in->w + 2);
        if (prefix ? strncmp(s, name, k) == 0 : strncmp(s, name, (MP_N(in) - 2) * 4) == 0 && strlen(s) == k) {
            if (hit && hit != in->w[1]) return 0;
            hit = in->w[1];
        }
    }
    return hit;
}
static int mp_is_named(const mp_mod *m, uint32_t id, const char *name)
{
    size_t i;
    for (i = 0; i < m->n; i++) {
        const mp_ins *in = &m->ins[i];
        if (MP_OP(in) == OpName && in->w[1] == id && !strcmp((const char *)(in->w + 2), name)) return 1;
    }
    return 0;
}
/* The member index OpMemberName gives `name` in struct `type`, or -1. */
static int mp_member(const mp_mod *m, uint32_t type, const char *name)
{
    size_t i;
    for (i = 0; i < m->n; i++) {
        const mp_ins *in = &m->ins[i];
        if (MP_OP(in) == OpMemberName && in->w[1] == type && !strcmp((const char *)(in->w + 3), name))
            return (int)in->w[2];
    }
    return -1;
}
static const mp_ins *mp_def(const mp_mod *m, uint32_t id)
{
    size_t i;
    for (i = 0; i < m->n; i++) if (mp_rid(&m->ins[i]) == id) return &m->ins[i];
    return NULL;
}

/* A growing list of new instructions. */
typedef struct mp_list { mp_ins *v; size_t n, cap; } mp_list;
static uint32_t *mp_emit(mp_mod *m, mp_list *l, uint32_t op, const uint32_t *ops, uint32_t nops)
{
    uint32_t *w = mp_own(m, nops + 1);
    if (!w) return NULL;
    w[0] = (nops + 1) << 16 | op;
    if (nops) memcpy(w + 1, ops, nops * sizeof *ops);
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 256;
        mp_ins *g = (mp_ins *)realloc(l->v, cap * sizeof *g);
        if (!g) { m->oom = 1; return NULL; }
        l->v = g;
        l->cap = cap;
    }
    l->v[l->n++].w = w;
    return w;
}

/* Type and constant identity: the words other than the result id. */
static int mp_same(const uint32_t *a, const uint32_t *b, uint32_t rid_at)
{
    uint32_t n = a[0] >> 16, k;
    if (a[0] != b[0]) return 0;
    for (k = 1; k < n; k++) if (k != rid_at && a[k] != b[k]) return 0;
    return 1;
}

typedef struct mp_ctx {
    mp_mod S, M;
    mp_list globals;          /* types, constants, samplers brought in, then the ones the splice adds */
    mp_list extra;
    mp_list names, decos;
    uint32_t *idmap;          /* M id -> S id (0 unmapped) */
    const char *why;
} mp_ctx;

/* A stock (or already added) type/constant with these words, else add one. `words` has the result id slot at
 * `rid_at` (ignored for the search). */
static uint32_t mp_find_or_add(mp_ctx *c, uint32_t op, uint32_t *ops, uint32_t nops, uint32_t rid_at)
{
    uint32_t probe[16], i;
    size_t k;
    probe[0] = (nops + 1) << 16 | op;
    for (i = 0; i < nops && i < 15; i++) probe[i + 1] = ops[i];
    for (k = 0; k < c->S.n; k++) {
        const mp_ins *in = &c->S.ins[k];
        if (MP_OP(in) == op && mp_same(in->w, probe, rid_at)) return in->w[rid_at];
        if (MP_OP(in) == OpFunction) break;
    }
    for (k = 0; k < c->globals.n; k++)
        if (mp_same(c->globals.v[k].w, probe, rid_at)) return c->globals.v[k].w[rid_at];
    for (k = 0; k < c->extra.n; k++)
        if (mp_same(c->extra.v[k].w, probe, rid_at)) return c->extra.v[k].w[rid_at];
    ops[rid_at - 1] = c->S.bound++;
    if (!mp_emit(&c->S, &c->extra, op, ops, nops)) return 0;
    return ops[rid_at - 1];
}
static uint32_t mp_t_int(mp_ctx *c) { uint32_t o[3] = { 0, 32, 1 }; return mp_find_or_add(c, OpTypeInt, o, 3, 1); }
static uint32_t mp_c_int(mp_ctx *c, uint32_t v)
{ uint32_t o[3] = { mp_t_int(c), 0, v }; return mp_find_or_add(c, OpConstant, o, 3, 2); }
static uint32_t mp_ptr(mp_ctx *c, uint32_t storage, uint32_t t)
{ uint32_t o[3] = { 0, storage, t }; return mp_find_or_add(c, OpTypePointer, o, 3, 1); }

static const mp_ins *mp_def_any(mp_ctx *c, uint32_t id)
{
    size_t k;
    const mp_ins *d = mp_def(&c->S, id);
    if (d) return d;
    for (k = 0; k < c->globals.n; k++) if (mp_rid(&c->globals.v[k]) == id) return &c->globals.v[k];
    for (k = 0; k < c->extra.n; k++) if (mp_rid(&c->extra.v[k]) == id) return &c->extra.v[k];
    return NULL;
}

#define MP_FAIL(c, text) do { (c)->why = (text); goto done; } while (0)

/* A module's functions brought into the stock module: `entry` (an OpName prefix) and what it calls, the globals
 * they need (types and constants shared with the stock module where they are the same), its samplers (module
 * variables named in `samplers`) either bound to the stock sampler `stock[k]` names (no texture added) or added at
 * bindings tex_end, tex_end + 1, ... in the order of those without a stock name. `what` names the module in the
 * sentences. On success *entry_id is the entry's id in the stock module and the functions are in *funcs. */
typedef struct mp_import_spec {
    const uint32_t *words;
    unsigned nwords;
    const char *entry;
    const char *const *samplers;
    const char *const *stock;
    size_t nsamplers;
    const char *what;
} mp_import_spec;

static int mp_import(mp_ctx *c, const mp_import_spec *sp, unsigned tex_end, uint32_t *entry_id, mp_list *funcs)
{
    uint32_t *mark = NULL, *sampler_ids = NULL, entry_fn;
    size_t i, k, nkeep = 0, *keep = NULL, *fstart = NULL, nf = 0, added = 0;
    int ok = 0;
    if (!mp_parse(&c->M, sp->words, sp->nwords)) { c->why = "a module of the product's programs is damaged"; return 0; }
    if (!(c->idmap = (uint32_t *)calloc(c->M.bound, sizeof *c->idmap)) ||
        !(mark = (uint32_t *)calloc(c->M.bound, sizeof *mark)) ||
        !(sampler_ids = (uint32_t *)calloc(sp->nsamplers ? sp->nsamplers : 1, sizeof *sampler_ids)) ||
        !(fstart = (size_t *)calloc(c->M.n + 1, sizeof *fstart)) || !(keep = (size_t *)calloc(c->M.n + 1, sizeof *keep)))
        MP_FAIL(c, "out of memory");

    /* ---- the module's functions: the entry and what it calls (module order) */
    for (i = 0; i < c->M.n; i++) if (MP_OP(&c->M.ins[i]) == OpFunction) fstart[nf++] = i;
    entry_fn = mp_named(&c->M, sp->entry, 1);
    if (!entry_fn) MP_FAIL(c, "a module of the product's programs has no single entry function");
    /* mark bits: 1 kept function, 2 defined inside a kept function, 4 a global a kept function needs */
    mark[entry_fn] |= 1;
    for (;;) {
        int grew = 0;
        for (k = 0; k < nf; k++) {
            uint32_t fid = c->M.ins[fstart[k]].w[2];
            if (!(mark[fid] & 1)) continue;
            for (i = fstart[k]; i < c->M.n && MP_OP(&c->M.ins[i]) != OpFunctionEnd; i++)
                if (MP_OP(&c->M.ins[i]) == OpFunctionCall && !(mark[c->M.ins[i].w[3]] & 1)) {
                    mark[c->M.ins[i].w[3]] |= 1;
                    grew = 1;
                }
        }
        if (!grew) break;
    }
    for (k = 0; k < nf; k++) {
        uint32_t fid = c->M.ins[fstart[k]].w[2];
        if (!(mark[fid] & 1)) continue;
        keep[nkeep++] = fstart[k];
        for (i = fstart[k]; i < c->M.n; i++) {
            uint32_t r = mp_rid(&c->M.ins[i]);
            if (r) mark[r] |= 2;
            if (MP_OP(&c->M.ins[i]) == OpFunctionEnd) break;
        }
    }
    /* the globals they reference, transitively */
    for (;;) {
        int grew = 0;
        for (i = 0; i < c->M.n; i++) {
            const mp_ins *in = &c->M.ins[i];
            uint32_t pos[64], r = mp_rid(in), op = MP_OP(in);
            int np, q, needed = r && (mark[r] & 4);
            int in_fn = 0;
            for (k = 0; k < nkeep; k++) {
                size_t e = keep[k];
                while (e < c->M.n && MP_OP(&c->M.ins[e]) != OpFunctionEnd) e++;
                if (i >= keep[k] && i <= e) { in_fn = 1; break; }
            }
            if (!in_fn && !needed) continue;
            if (op == OpExtInstImport || op == OpName || op == OpMemberName || op == OpDecorate) continue;
            np = mp_ids(in, pos, 64);
            if (np < 0) MP_FAIL(c, "a module of the product's programs uses an instruction the splice does not know");
            for (q = 0; q < np; q++) {
                uint32_t v = in->w[pos[q]];
                if (v < c->M.bound && !(mark[v] & 2) && !(mark[v] & 4) && v != r) {
                    mark[v] |= 4;
                    grew = 1;
                }
            }
        }
        if (!grew) break;
    }

    /* ---- the module's globals into the stock module */
    for (i = 0; i < c->M.n; i++) {
        mp_ins *in = &c->M.ins[i];
        uint32_t r = mp_rid(in), op = MP_OP(in), n = MP_N(in), rid_at, pos[64], *w;
        int np, q;
        if (!r || !(mark[r] & 4) || (mark[r] & 2)) continue;
        if (op == OpExtInstImport) {
            size_t s;
            for (s = 0; s < c->S.n; s++)
                if (MP_OP(&c->S.ins[s]) == OpExtInstImport &&
                    !strcmp((const char *)(c->S.ins[s].w + 2), (const char *)(in->w + 2))) { c->idmap[r] = c->S.ins[s].w[1]; break; }
            if (!c->idmap[r]) MP_FAIL(c, "the program's fragment module does not import the instructions the product's functions use");
            continue;
        }
        if (!mp_is_type(op) && !mp_is_const(op) && op != OpVariable)
            MP_FAIL(c, "the product's functions read a global the splice does not carry");
        rid_at = mp_has_type(op) ? 2 : 1;
        if (!(w = (uint32_t *)malloc(n * sizeof *w))) MP_FAIL(c, "out of memory");
        memcpy(w, in->w, n * sizeof *w);
        np = mp_ids(in, pos, 64);
        for (q = 0; q < np; q++) if (pos[q] != rid_at) {
            uint32_t v = c->idmap[w[pos[q]]];
            if (!v) { free(w); MP_FAIL(c, "a module of the product's programs refers to a global before it declares it"); }
            w[pos[q]] = v;
        }
        if (op == OpVariable) {
            int s, hit = -1;
            for (s = 0; s < (int)sp->nsamplers; s++) if (mp_is_named(&c->M, r, sp->samplers[s])) hit = s;
            if (hit < 0 || w[3] != 0) { free(w); MP_FAIL(c, "the product's functions read a global other than their samplers"); }
            if (sp->stock && sp->stock[hit]) {
                /* a sampler the program already has: no texture is added */
                uint32_t own = mp_named(&c->S, sp->stock[hit], 0);
                const mp_ins *d = own ? mp_def(&c->S, own) : NULL;
                free(w);
                if (!d || MP_OP(d) != OpVariable || d->w[1] != c->idmap[in->w[1]])
                    MP_FAIL(c, "the program does not have a texture the product's functions read");
                c->idmap[r] = own;
                sampler_ids[hit] = own;
                continue;
            }
            w[2] = c->S.bound++;
            c->idmap[r] = w[2];
            sampler_ids[hit] = w[2];
            if (!mp_emit(&c->S, &c->globals, op, w + 1, n - 1)) { free(w); MP_FAIL(c, "out of memory"); }
            free(w);
            continue;
        }
        {
            uint32_t found = 0;
            for (k = 0; k < c->S.n && !found; k++) {
                const mp_ins *s = &c->S.ins[k];
                if (MP_OP(s) == OpFunction) break;
                if ((mp_is_type(MP_OP(s)) || mp_is_const(MP_OP(s))) && mp_same(s->w, w, rid_at)) found = s->w[rid_at];
            }
            for (k = 0; k < c->globals.n && !found; k++)
                if (mp_same(c->globals.v[k].w, w, rid_at)) found = c->globals.v[k].w[rid_at];
            if (found) c->idmap[r] = found;
            else {
                w[rid_at] = c->S.bound++;
                c->idmap[r] = w[rid_at];
                if (!mp_emit(&c->S, &c->globals, op, w + 1, n - 1)) { free(w); MP_FAIL(c, "out of memory"); }
            }
        }
        free(w);
    }
    for (k = 0; k < sp->nsamplers; k++) if (!sampler_ids[k]) MP_FAIL(c, "a module of the product's programs does not read all of its samplers");

    /* ---- the functions, every id renumbered */
    for (k = 0; k < nkeep; k++)
        for (i = keep[k]; i < c->M.n; i++) {
            uint32_t r = mp_rid(&c->M.ins[i]);
            if (r) c->idmap[r] = c->S.bound++;
            if (MP_OP(&c->M.ins[i]) == OpFunctionEnd) break;
        }
    for (k = 0; k < nkeep; k++)
        for (i = keep[k]; i < c->M.n; i++) {
            const mp_ins *in = &c->M.ins[i];
            uint32_t pos[256], *w;
            int np = mp_ids(in, pos, 256), q;
            if (np < 0) MP_FAIL(c, "a module of the product's programs uses an instruction the splice does not know");
            if (!(w = mp_emit(&c->S, funcs, MP_OP(in), in->w + 1, MP_N(in) - 1))) MP_FAIL(c, "out of memory");
            for (q = 0; q < np; q++) {
                uint32_t v = c->idmap[w[pos[q]]];
                if (!v) MP_FAIL(c, "a module of the product's programs refers to an id it does not define");
                w[pos[q]] = v;
            }
            if (MP_OP(in) == OpFunctionEnd) break;
        }

    /* ---- names and decorations of what was brought in (an added sampler's binding after the program's own) */
    for (i = 0; i < c->M.n; i++) {
        const mp_ins *in = &c->M.ins[i];
        uint32_t t = in->w[1];
        int sampler = 0, s;
        if (MP_OP(in) != OpName || t >= c->M.bound || !c->idmap[t]) continue;
        for (s = 0; s < (int)sp->nsamplers; s++)
            if (sampler_ids[s] == c->idmap[t] && !(sp->stock && sp->stock[s])) sampler = 1;
        if (!(mark[t] & 2) && !sampler) continue;
        {
            uint32_t *w = mp_emit(&c->S, &c->names, OpName, in->w + 1, MP_N(in) - 1);
            if (!w) MP_FAIL(c, "out of memory");
            w[1] = c->idmap[t];
        }
    }
    for (k = 0; k < sp->nsamplers; k++) {
        uint32_t d1[3], d2[3];
        if (sp->stock && sp->stock[k]) continue;
        d1[0] = sampler_ids[k]; d1[1] = 34; d1[2] = 0;
        d2[0] = sampler_ids[k]; d2[1] = 33; d2[2] = tex_end + (uint32_t)added++;
        if (!mp_emit(&c->S, &c->decos, OpDecorate, d1, 3) || !mp_emit(&c->S, &c->decos, OpDecorate, d2, 3)) MP_FAIL(c, "out of memory");
    }
    *entry_id = c->idmap[entry_fn];
    ok = 1;
done:
    free(mark); free(sampler_ids); free(fstart); free(keep);
    return ok;
}

/* The stock module's texture bindings at or after tex_end move up by `shift`. */
static int mp_shift_bindings(mp_ctx *c, unsigned tex_end, unsigned shift)
{
    size_t i;
    for (i = 0; i < c->S.n; i++) {
        mp_ins *in = &c->S.ins[i];
        if (MP_OP(in) == OpDecorate && MP_N(in) == 4 && in->w[2] == 33 && in->w[3] >= tex_end) {
            uint32_t *w = mp_own(&c->S, 4);
            if (!w) { c->why = "out of memory"; return 0; }
            memcpy(w, in->w, 4 * sizeof *w);
            w[3] += shift;
            in->w = w;
        }
    }
    return 1;
}

/* The material uniform block (freqHigh_fragmentUniforms) and the view block (freqLow_fragmentUniforms): their
 * variables and struct types; the material block grows by `parms` (16-byte vectors after its own). *n0 is the
 * block's own member count, *v4 its vector type. */
typedef struct mp_blocks { uint32_t fh, fl, fh_struct, fl_struct, v4, n0; } mp_blocks;

static int mp_grow_block_in(mp_ctx *c, const char *high, const char *low, const char *const *parms, unsigned nparms,
                            mp_blocks *bk)
{
    size_t i, k;
    memset(bk, 0, sizeof *bk);
    bk->fh = mp_named(&c->S, high, 0);
    bk->fl = mp_named(&c->S, low, 0);
    if (!bk->fh || !bk->fl) { c->why = "the program's fragment module has no single material uniform block"; return 0; }
    {
        const mp_ins *v = mp_def(&c->S, bk->fh), *p;
        if (!v || MP_OP(v) != OpVariable || !(p = mp_def(&c->S, v->w[1]))) { c->why = "the material uniform block is not a variable"; return 0; }
        bk->fh_struct = p->w[3];
        v = mp_def(&c->S, bk->fl);
        if (!v || MP_OP(v) != OpVariable || !(p = mp_def(&c->S, v->w[1]))) { c->why = "the view uniform block is not a variable"; return 0; }
        bk->fl_struct = p->w[3];
    }
    {
        mp_ins *st = NULL;
        uint32_t *w, offs_ok = 1, cnt = 0;
        for (i = 0; i < c->S.n; i++) if (mp_rid(&c->S.ins[i]) == bk->fh_struct) st = &c->S.ins[i];
        if (!st || MP_OP(st) != OpTypeStruct || MP_N(st) < 3) { c->why = "the material uniform block is not a struct"; return 0; }
        bk->v4 = st->w[2];
        bk->n0 = MP_N(st) - 2;
        if (!(w = mp_own(&c->S, MP_N(st) + nparms))) { c->why = "out of memory"; return 0; }
        memcpy(w, st->w, MP_N(st) * sizeof *w);
        for (k = 0; k < nparms; k++) w[MP_N(st) + k] = bk->v4;
        w[0] = (MP_N(st) + nparms) << 16 | OpTypeStruct;
        st->w = w;
        for (i = 0; i < c->S.n; i++) {
            const mp_ins *in = &c->S.ins[i];
            if (MP_OP(in) == OpMemberDecorate && in->w[1] == bk->fh_struct && in->w[3] == 35) {
                if (in->w[4] != 16 * in->w[2]) offs_ok = 0;
                cnt++;
            }
        }
        if (!offs_ok || cnt != bk->n0) { c->why = "the material uniform block is not 16-byte vectors"; return 0; }
        for (k = 0; k < nparms; k++) {
            uint32_t d[4] = { bk->fh_struct, bk->n0 + (uint32_t)k, 35, 16 * (bk->n0 + (uint32_t)k) };
            size_t len = strlen(parms[k]) / 4 + 1;
            uint32_t nm[2 + 16];
            if (mp_member(&c->S, bk->fh_struct, parms[k]) >= 0) { c->why = "the program already reads a parameter the product adds"; return 0; }
            if (!mp_emit(&c->S, &c->decos, OpMemberDecorate, d, 4)) { c->why = "out of memory"; return 0; }
            memset(nm, 0, sizeof nm);
            nm[0] = bk->fh_struct;
            nm[1] = bk->n0 + (uint32_t)k;
            memcpy(nm + 2, parms[k], strlen(parms[k]));
            if (!mp_emit(&c->S, &c->names, OpMemberName, nm, 2 + (uint32_t)len)) { c->why = "out of memory"; return 0; }
        }
    }
    return 1;
}

static int mp_grow_block(mp_ctx *c, const char *const *parms, unsigned nparms, mp_blocks *bk)
{
    return mp_grow_block_in(c, "freqHigh_fragmentUniforms", "freqLow_fragmentUniforms", parms, nparms, bk);
}

/* The entry's parameters (each one's pointer type) after its OpFunction in `funcs`: `want` of them. */
static int mp_entry_params(const mp_list *funcs, uint32_t entry, uint32_t *params, size_t want)
{
    size_t k, p, np = 0;
    for (k = 0; k < funcs->n; k++) if (MP_OP(&funcs->v[k]) == OpFunction && funcs->v[k].w[2] == entry) break;
    p = k + 1;
    while (p < funcs->n && MP_OP(&funcs->v[p]) == OpFunctionParameter && np < want) params[np++] = funcs->v[p++].w[1];
    return np == want && !(p < funcs->n && MP_OP(&funcs->v[p]) == OpFunctionParameter);
}

/* A vec4 member of a uniform block loaded into `code`: its value id, or 0. */
static uint32_t mp_load_member(mp_ctx *c, mp_list *code, uint32_t var, uint32_t v4, int member)
{
    uint32_t ch, v, o[4];
    if (member < 0) return 0;
    ch = c->S.bound++;
    o[0] = mp_ptr(c, 2, v4); o[1] = ch; o[2] = var; o[3] = mp_c_int(c, (uint32_t)member);
    if (!mp_emit(&c->S, code, OpAccessChain, o, 4)) return 0;
    v = c->S.bound++;
    o[0] = v4; o[1] = v; o[2] = ch;
    if (!mp_emit(&c->S, code, OpLoad, o, 3)) return 0;
    return v;
}

/* Values gathered into one array temp: an OpCompositeConstruct of `n` ids stored to `temp` (pointer to the array
 * type the entry's parameter `param` points to). */
static int mp_store_array(mp_ctx *c, mp_list *code, uint32_t param, uint32_t temp, const uint32_t *vals, unsigned n)
{
    uint32_t o[2 + 64], a;
    const mp_ins *pp = mp_def_any(c, param);
    if (!pp || n > 64) return 0;
    a = c->S.bound++;
    o[0] = pp->w[3]; o[1] = a;
    memcpy(o + 2, vals, n * sizeof *vals);
    if (!mp_emit(&c->S, code, OpCompositeConstruct, o, 2 + n)) return 0;
    o[0] = temp; o[1] = a;
    return mp_emit(&c->S, code, OpStore, o, 2) != NULL;
}

/* One Function variable per parameter, at the head of main's first block (into `heads`); var_end is the index
 * before which they go. */
static int mp_temps(mp_ctx *c, size_t s_main, size_t s_main_end, const uint32_t *params, uint32_t *temps, size_t n,
                    mp_list *heads, size_t *var_end)
{
    size_t i, q;
    for (i = s_main; i < s_main_end && MP_OP(&c->S.ins[i]) != OpLabel; i++) {}
    *var_end = i + 1;
    while (*var_end < s_main_end && MP_OP(&c->S.ins[*var_end]) == OpVariable) (*var_end)++;
    for (q = 0; q < n; q++) {
        uint32_t o[3];
        temps[q] = c->S.bound++;
        o[0] = params[q];
        o[1] = temps[q];
        o[2] = 7;
        if (!mp_emit(&c->S, heads, OpVariable, o, 3)) return 0;
    }
    return 1;
}

/* main's span in the stock module. */
static int mp_main(mp_ctx *c, size_t *s_main, size_t *s_main_end)
{
    size_t i;
    uint32_t mainid = mp_named(&c->S, "main", 0);
    for (i = 0; i < c->S.n; i++) if (MP_OP(&c->S.ins[i]) == OpFunction && c->S.ins[i].w[2] == mainid) break;
    if (!mainid || i == c->S.n) return 0;
    *s_main = i;
    while (i < c->S.n && MP_OP(&c->S.ins[i]) != OpFunctionEnd) i++;
    *s_main_end = i;
    return 1;
}

/* The stock module written out with what the splice added: names after the stock debug section, decorations
 * after the stock annotations, globals before the first function, temps at the head of main, the code after the
 * instruction at `anchor` (or right after the temps when `anchor` is (size_t)-1), the functions last. */
static uint32_t *mp_reassemble(mp_ctx *c, size_t var_end, const mp_list *heads, size_t anchor, const mp_list *code,
                               const mp_list *funcs, size_t *out_words)
{
    size_t first_fn = c->S.n, last_name = 0, last_deco = 0, i;
    mp_buf o = { 0 };
    for (i = 0; i < c->S.n; i++) if (MP_OP(&c->S.ins[i]) == OpFunction) { first_fn = i; break; }
    for (i = 0; i < first_fn; i++) {
        uint32_t op = MP_OP(&c->S.ins[i]);
        if (op == OpName || op == OpMemberName || op == OpSource) last_name = i;
        if (op == OpDecorate || op == OpMemberDecorate) last_deco = i;
    }
    {
        uint32_t h[5] = { c->S.head[0], c->S.head[1], c->S.head[2], c->S.bound, c->S.head[4] };
        mp_put(&o, h, sizeof h);
    }
#define PUT_INS(x) mp_put(&o, (x)->w, MP_N(x) * sizeof(uint32_t))
#define PUT_LIST(l) do { size_t z_; for (z_ = 0; z_ < (l).n; z_++) PUT_INS(&(l).v[z_]); } while (0)
    for (i = 0; i < c->S.n; i++) {
        if (i == first_fn) { PUT_LIST(c->globals); PUT_LIST(c->extra); }
        if (i == var_end) { PUT_LIST(*heads); if (anchor == (size_t)-1) PUT_LIST(*code); }
        PUT_INS(&c->S.ins[i]);
        if (i == last_name) PUT_LIST(c->names);
        if (i == last_deco) PUT_LIST(c->decos);
        if (i == anchor) PUT_LIST(*code);
    }
    if (first_fn == c->S.n) { PUT_LIST(c->globals); PUT_LIST(c->extra); }
    PUT_LIST(*funcs);
#undef PUT_INS
#undef PUT_LIST
    if (o.oom) { free(o.p); c->why = "out of memory"; return NULL; }
    if (out_words) *out_words = o.n / 4;
    return (uint32_t *)o.p;
}

static void mp_ctx_free(mp_ctx *c)
{
    free(c->globals.v); free(c->extra.v); free(c->names.v); free(c->decos.v);
    free(c->idmap);
    mp_mod_free(&c->S);
    mp_mod_free(&c->M);
}

uint32_t *sh_modprog_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                            size_t *out_words, const char **why)
{
    static const char *const stock_samplers[MP_NSAMPLERS + 1] = { NULL };
    static const char *samplers[MP_NSAMPLERS + 1];
    static const char *stock_of[MP_NSAMPLERS + 1];
    mp_ctx c;
    mp_import_spec sp;
    mp_blocks bk;
    uint32_t *out = NULL;
    size_t i, k;
    uint32_t inputs = 0, in_struct = 0, t_float = 0, t_void = 0, ml_entry = 0;
    uint32_t params[MP_NINOUT + 3], temps[MP_NINOUT + 3];
    size_t s_main = 0, s_main_end = 0, anchor = 0, var_end = 0;
    mp_list code = { 0 }, heads = { 0 }, funcs = { 0 };
    (void)stock_samplers;
    memset(&c, 0, sizeof c);
    if (out_words) *out_words = 0;
    /* the blend's samplers, then the ember's noise: the program's own magnificationclouds (no texture added) */
    for (k = 0; k < MP_NSAMPLERS; k++) { samplers[k] = MP_SAMPLERS[k]; stock_of[k] = NULL; }
    samplers[MP_NSAMPLERS] = "ml_noise";
    stock_of[MP_NSAMPLERS] = "samp_magnificationclouds";
    if (!mp_parse(&c.S, stock, stock_words)) MP_FAIL(&c, "the program's fragment module is not SPIR-V");
    sp.words = SH_MODPROG_ML_SPIRV; sp.nwords = SH_MODPROG_ML_SPIRV_WORDS; sp.entry = "ml_blend(";
    sp.samplers = samplers; sp.stock = stock_of; sp.nsamplers = MP_NSAMPLERS + 1; sp.what = "layered";
    if (!mp_import(&c, &sp, tex_end, &ml_entry, &funcs)) goto done;
    if (!mp_shift_bindings(&c, tex_end, shift)) goto done;
    if (!mp_grow_block(&c, ML_BLOCK_PARMS, ML_NBLOCK_PARMS, &bk)) goto done;

    /* ---- helpers' types */
    (void)mp_t_int(&c);
    { uint32_t o[2] = { 0, 32 }; t_float = mp_find_or_add(&c, OpTypeFloat, o, 2, 1); }
    { uint32_t o[1] = { 0 }; t_void = mp_find_or_add(&c, OpTypeVoid, o, 1, 1); }

    /* ---- main: the anchor and the inputs */
    if (!mp_main(&c, &s_main, &s_main_end)) MP_FAIL(&c, "the program's fragment module has no main");
    for (i = s_main; i < s_main_end; i++)
        if (MP_OP(&c.S.ins[i]) == OpVariable && mp_is_named(&c.S, c.S.ins[i].w[2], "inputs")) {
            if (inputs) MP_FAIL(&c, "main has no single inputs");
            inputs = c.S.ins[i].w[2];
        }
    if (!inputs) MP_FAIL(&c, "main has no single inputs");
    {
        const mp_ins *v = mp_def(&c.S, inputs), *p = v ? mp_def(&c.S, v->w[1]) : NULL;
        if (!p) MP_FAIL(&c, "the lighting inputs have no type");
        in_struct = p->w[3];
    }
    {
        static const char *const fields[MP_NINOUT + 1] = { "albedo", "specular", "smoothness", "normal", "lightmap",
                                                           "emissive", "thickness", "texCoords" };
        int members[MP_NINOUT + 1], q;
        uint32_t t_int = mp_t_int(&c), normal_ix, wound;
        const mp_ins *st = mp_def(&c.S, in_struct);
        for (q = 0; q <= MP_NINOUT; q++)
            if ((members[q] = mp_member(&c.S, in_struct, fields[q])) < 0) MP_FAIL(&c, "the lighting inputs lack a field the blend changes");
        normal_ix = (uint32_t)members[3];
        /* the anchor: the first store to inputs.normal in main */
        for (i = s_main; i < s_main_end && !anchor; i++) {
            const mp_ins *in = &c.S.ins[i];
            if (MP_OP(in) == OpStore) {
                const mp_ins *ch = mp_def(&c.S, in->w[1]);
                if (ch && MP_OP(ch) == OpAccessChain && MP_N(ch) == 5 && ch->w[3] == inputs) {
                    const mp_ins *ix = mp_def(&c.S, ch->w[4]);
                    if (ix && MP_OP(ix) == OpConstant && ix->w[1] == t_int && ix->w[3] == normal_ix) anchor = i;
                }
            }
        }
        if (!anchor) MP_FAIL(&c, "main never stores inputs.normal");
        wound = mp_named(&c.S, "samp_woundtexture", 0);
        for (i = anchor + 1; i < s_main_end; i++) {
            const mp_ins *in = &c.S.ins[i];
            uint32_t pos[64];
            int np, hit = 0;
            if (MP_OP(in) == OpLabel) MP_FAIL(&c, "the texture inputs end in another block than the wound block begins");
            if (!wound) break;
            np = mp_ids(in, pos, 64);
            for (q = 0; q < np; q++) if (in->w[pos[q]] == wound) hit = 1;
            if (hit) break;
        }
        if (!mp_entry_params(&funcs, ml_entry, params, MP_NINOUT + 3)) MP_FAIL(&c, "ml_blend does not take the parameters the splice passes");
        if (!mp_temps(&c, s_main, s_main_end, params, temps, MP_NINOUT + 3, &heads, &var_end)) MP_FAIL(&c, "out of memory");
        /* copy in (params[] holds each parameter's pointer type) */
        for (q = 0; q <= MP_NINOUT; q++) {
            uint32_t ft = st->w[2 + members[q]], tptr, ch, v, o[4];
            const mp_ins *pp = mp_def_any(&c, params[q]);
            if (!pp || pp->w[3] != ft) MP_FAIL(&c, "ml_blend takes a lighting input as another type than the program holds it");
            tptr = mp_ptr(&c, 7, ft);
            ch = c.S.bound++;
            o[0] = tptr; o[1] = ch; o[2] = inputs; o[3] = mp_c_int(&c, (uint32_t)members[q]);
            if (!mp_emit(&c.S, &code, OpAccessChain, o, 4)) MP_FAIL(&c, "out of memory");
            v = c.S.bound++;
            o[0] = ft; o[1] = v; o[2] = ch;
            if (!mp_emit(&c.S, &code, OpLoad, o, 3)) MP_FAIL(&c, "out of memory");
            o[0] = temps[q]; o[1] = v;
            if (!mp_emit(&c.S, &code, OpStore, o, 2)) MP_FAIL(&c, "out of memory");
        }
        {
            /* time.x */
            const mp_ins *fls = mp_def(&c.S, bk.fl_struct);
            int tm = mp_member(&c.S, bk.fl_struct, "time");
            uint32_t t, ch, v, x, o[4];
            if (tm < 0 || !fls) MP_FAIL(&c, "the view uniforms have no time");
            t = fls->w[2 + tm];
            ch = c.S.bound++;
            o[0] = mp_ptr(&c, 2, t); o[1] = ch; o[2] = bk.fl; o[3] = mp_c_int(&c, (uint32_t)tm);
            if (!mp_emit(&c.S, &code, OpAccessChain, o, 4)) MP_FAIL(&c, "out of memory");
            v = c.S.bound++;
            o[0] = t; o[1] = v; o[2] = ch;
            if (!mp_emit(&c.S, &code, OpLoad, o, 3)) MP_FAIL(&c, "out of memory");
            x = c.S.bound++;
            o[0] = t_float; o[1] = x; o[2] = v; o[3] = 0;
            if (!mp_emit(&c.S, &code, OpCompositeExtract, o, 4)) MP_FAIL(&c, "out of memory");
            o[0] = temps[MP_NINOUT + 1]; o[1] = x;
            if (!mp_emit(&c.S, &code, OpStore, o, 2)) MP_FAIL(&c, "out of memory");
        }
        {
            /* the program's emissive factors, as one array (the numbers come from the parameter image) */
            uint32_t pv[4];
            for (k = 0; k < 4; k++) {
                int mem = mp_member(&c.S, bk.fh_struct, MP_EMISSIVE[k]);
                if (mem < 0) MP_FAIL(&c, "the program has no emissive factor the blend recomputes from");
                if (!(pv[k] = mp_load_member(&c, &code, bk.fh, bk.v4, mem))) MP_FAIL(&c, "out of memory");
            }
            if (!mp_store_array(&c, &code, params[MP_NINOUT + 2], temps[MP_NINOUT + 2], pv, 4))
                MP_FAIL(&c, "ml_blend's parameter array has no type");
        }
        {
            uint32_t o[3 + MP_NINOUT + 3];
            o[0] = t_void; o[1] = 0; o[2] = ml_entry;
            memcpy(o + 3, temps, sizeof temps);
            o[1] = c.S.bound++;
            if (!mp_emit(&c.S, &code, OpFunctionCall, o, 3 + MP_NINOUT + 3)) MP_FAIL(&c, "out of memory");
        }
        /* copy back */
        for (q = 0; q < MP_NINOUT; q++) {
            uint32_t ft = st->w[2 + members[q]], v, ch, o[4];
            v = c.S.bound++;
            o[0] = ft; o[1] = v; o[2] = temps[q];
            if (!mp_emit(&c.S, &code, OpLoad, o, 3)) MP_FAIL(&c, "out of memory");
            ch = c.S.bound++;
            o[0] = mp_ptr(&c, 7, ft); o[1] = ch; o[2] = inputs; o[3] = mp_c_int(&c, (uint32_t)members[q]);
            if (!mp_emit(&c.S, &code, OpAccessChain, o, 4)) MP_FAIL(&c, "out of memory");
            o[0] = ch; o[1] = v;
            if (!mp_emit(&c.S, &code, OpStore, o, 2)) MP_FAIL(&c, "out of memory");
        }
    }
    if (c.S.oom) MP_FAIL(&c, "out of memory");
    out = mp_reassemble(&c, var_end, &heads, anchor, &code, &funcs, out_words);
done:
    if (why) *why = c.why;
    free(code.v); free(heads.v); free(funcs.v);
    mp_ctx_free(&c);
    if (c.why) { free(out); return NULL; }
    return out;
}

/* --------------------------------------------------------------------------- the particle splice ---- */

uint32_t *sh_modprog_ep_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                               size_t *out_words, const char **why)
{
    mp_ctx c;
    mp_import_spec sp;
    mp_blocks bk;
    uint32_t *out = NULL;
    size_t i, k;
    uint32_t entry = 0, frag = 0, v4ptr_out = 0, params[4], temps[4], vals[16];
    size_t s_main = 0, s_main_end = 0, anchor = 0, var_end = 0;
    mp_list code = { 0 }, heads = { 0 }, funcs = { 0 };
    memset(&c, 0, sizeof c);
    if (out_words) *out_words = 0;
    (void)shift;
    if (!mp_parse(&c.S, stock, stock_words)) MP_FAIL(&c, "the program's fragment module is not SPIR-V");
    sp.words = SH_MODPROG_EP_SPIRV; sp.nwords = SH_MODPROG_EP_SPIRV_WORDS; sp.entry = "ep_shade(";
    sp.samplers = EP_SAMPLERS; sp.stock = EP_SAMPLER_STOCK; sp.nsamplers = EP_NSAMPLERS; sp.what = "particle";
    if (!mp_import(&c, &sp, tex_end, &entry, &funcs)) goto done;
    if (!mp_shift_bindings(&c, tex_end, shift)) goto done;
    if (!mp_grow_block(&c, EP_BLOCK_PARMS, EP_NBLOCK_PARMS, &bk)) goto done;
    if (!mp_main(&c, &s_main, &s_main_end)) MP_FAIL(&c, "the program's fragment module has no main");
    /* the anchor: the instruction before main's last return (the stock shading has written its colour) */
    for (i = s_main_end; i > s_main; i--) if (MP_OP(&c.S.ins[i]) == OpReturn) { anchor = i - 1; break; }
    if (!anchor) MP_FAIL(&c, "the program's main does not end in a return");
    frag = mp_named(&c.S, "out_FragColor0", 0);
    if (!frag) MP_FAIL(&c, "the program writes no colour");
    if (!mp_entry_params(&funcs, entry, params, 4)) MP_FAIL(&c, "ep_shade does not take the parameters the splice passes");
    if (!mp_temps(&c, s_main, s_main_end, params, temps, 4, &heads, &var_end)) MP_FAIL(&c, "out of memory");
    {
        /* frag in */
        const mp_ins *pp = mp_def_any(&c, params[0]), *fv = mp_def(&c.S, frag), *fp = fv ? mp_def(&c.S, fv->w[1]) : NULL;
        uint32_t o[3], v;
        if (!pp || !fp || pp->w[3] != fp->w[3]) MP_FAIL(&c, "the program's colour is not a vector");
        v4ptr_out = fp->w[3];
        v = c.S.bound++;
        o[0] = v4ptr_out; o[1] = v; o[2] = frag;
        if (!mp_emit(&c.S, &code, OpLoad, o, 3)) MP_FAIL(&c, "out of memory");
        o[0] = temps[0]; o[1] = v;
        if (!mp_emit(&c.S, &code, OpStore, o, 2)) MP_FAIL(&c, "out of memory");
    }
    {
        /* V: the stage inputs */
        for (k = 0; k < EP_NVARYINGS; k++) {
            uint32_t var = mp_named(&c.S, EP_VARYINGS[k], 0), o[3];
            const mp_ins *d = var ? mp_def(&c.S, var) : NULL, *p = d ? mp_def(&c.S, d->w[1]) : NULL;
            if (!d || MP_OP(d) != OpVariable || d->w[3] != 1 || !p || p->w[3] != v4ptr_out)
                MP_FAIL(&c, "the program's fragment stage lacks an input the particle shading reads");
            vals[k] = c.S.bound++;
            o[0] = v4ptr_out; o[1] = vals[k]; o[2] = var;
            if (!mp_emit(&c.S, &code, OpLoad, o, 3)) MP_FAIL(&c, "out of memory");
        }
        if (!mp_store_array(&c, &code, params[1], temps[1], vals, EP_NVARYINGS)) MP_FAIL(&c, "ep_shade's input array has no type");
    }
    {
        /* P: the parameters the block grew by */
        for (k = 0; k < EP_NBLOCK_PARMS; k++)
            if (!(vals[k] = mp_load_member(&c, &code, bk.fh, bk.v4, (int)(bk.n0 + k)))) MP_FAIL(&c, "out of memory");
        if (!mp_store_array(&c, &code, params[2], temps[2], vals, EP_NBLOCK_PARMS)) MP_FAIL(&c, "ep_shade's parameter array has no type");
    }
    {
        /* U: the program's own uniforms */
        for (k = 0; k < EP_NUNIFORMS; k++) {
            int high = EP_UNIFORMS[k][0] == 'h';
            const char *name = EP_UNIFORMS[k] + 1;
            int mem = mp_member(&c.S, high ? bk.fh_struct : bk.fl_struct, name);
            if (mem < 0) MP_FAIL(&c, "the program lacks a uniform the particle shading reads");
            if (!(vals[k] = mp_load_member(&c, &code, high ? bk.fh : bk.fl, bk.v4, mem))) MP_FAIL(&c, "out of memory");
        }
        if (!mp_store_array(&c, &code, params[3], temps[3], vals, EP_NUNIFORMS)) MP_FAIL(&c, "ep_shade's uniform array has no type");
    }
    {
        uint32_t o[7], t_void, v;
        { uint32_t ov[1] = { 0 }; t_void = mp_find_or_add(&c, OpTypeVoid, ov, 1, 1); }
        o[0] = t_void; o[1] = c.S.bound++; o[2] = entry;
        memcpy(o + 3, temps, sizeof temps);
        if (!mp_emit(&c.S, &code, OpFunctionCall, o, 7)) MP_FAIL(&c, "out of memory");
        v = c.S.bound++;
        o[0] = v4ptr_out; o[1] = v; o[2] = temps[0];
        if (!mp_emit(&c.S, &code, OpLoad, o, 3)) MP_FAIL(&c, "out of memory");
        o[0] = frag; o[1] = v;
        if (!mp_emit(&c.S, &code, OpStore, o, 2)) MP_FAIL(&c, "out of memory");
    }
    if (c.S.oom) MP_FAIL(&c, "out of memory");
    out = mp_reassemble(&c, var_end, &heads, anchor, &code, &funcs, out_words);
done:
    if (why) *why = c.why;
    free(code.v); free(heads.v); free(funcs.v);
    mp_ctx_free(&c);
    if (c.why) { free(out); return NULL; }
    return out;
}

/* ---------------------------------------------------------------------------------------------- families ---- */

static const unsigned EP_BIN_PARM_COUNT = sizeof EP_BLOCK_PARMS / sizeof EP_BLOCK_PARMS[0];
static const mp_family MP_FAMILIES[] = {
    { SH_MODPROG_PREFIX, "layered version", ML_BLOCK_PARMS, &ML_NBLOCK_PARMS, SH_MODPROG_ML_TEXTURES,
      &SH_MODPROG_ML_TEXTURE_COUNT, sh_modprog_patch_decl, sh_modprog_patch_glsl, sh_modprog_splice,
      "its decl has no single CUSTOM_TEXTURE_INPUTS template, or no hlsl_fp section for the functions" },
    { SH_MODPROG_EP_PREFIX, "particle version", EP_BLOCK_PARMS, &EP_BIN_PARM_COUNT, SH_MODPROG_EP_TEXTURES,
      &SH_MODPROG_EP_TEXTURE_COUNT, sh_modprog_ep_patch_decl, sh_modprog_ep_patch_glsl, sh_modprog_ep_splice,
      "its decl's fragment stage has no single PREMULTIPLY_ALPHA_BLENDED( result ) statement, or no hlsl_fp section for the functions" },
};
#define MP_NFAMILIES (sizeof MP_FAMILIES / sizeof MP_FAMILIES[0])

/* --------------------------------------------------------------------------------------- cooked program ---- */
/* package_shader's reader, cut at what the variant changes: a parameter block whose vector values are 16 bytes
 * and every other value a string, the stage parameter lists (3 stages x 7 groups: material uniforms, view
 * uniforms, textures, samplers, storage buffers, uniform-buffer arrays, storage images), includes, flags, the
 * permutation mask and list, 192 bytes of state and binding tables (6 words, then each stage's 7 binding starts
 * and 7 counts by category: material uniforms, view uniforms, uniform-buffer arrays, texel buffers, textures,
 * storage buffers, storage images), the three stage texts and a 36-byte trailer. */

#define MP_MAX_BOUND_TEXTURES 32u   /* ID_VULKAN_MAX_BOUND_TEXTURES (VK 0x141a7bbb0) */

typedef int (*mp_vec_fn)(const char *parm);    /* 1 when the render parm is a vector (16-byte value) */

typedef struct mp_bin {
    const unsigned char *b;
    size_t len, lists_at, mid_at, block_at;
    char **names[3][7];
    size_t counts[3][7], caps[3][7];
    uint32_t block[48];
    const unsigned char *hdr[3];
    char *text[3];
    size_t text_n[3];
    const unsigned char *trailer;
    uint64_t perms[16];
    size_t nperms;
} mp_bin;

static void mp_bin_free(mp_bin *x)
{
    int s, g;
    size_t i;
    for (s = 0; s < 3; s++) {
        for (g = 0; g < 7; g++) {
            for (i = 0; i < x->counts[s][g]; i++) free(x->names[s][g][i]);
            free(x->names[s][g]);
        }
        free(x->text[s]);
    }
    memset(x, 0, sizeof *x);
}

static int mp_bin_string(const mp_bin *x, size_t *at, const unsigned char **s, size_t *n)
{
    uint32_t k;
    if (*at + 4 > x->len) return 0;
    k = mp_le32(x->b + *at);
    if (k > x->len - *at - 4) return 0;
    if (s) *s = x->b + *at + 4;
    if (n) *n = k;
    *at += 4 + k;
    return 1;
}

static int mp_bin_add(mp_bin *x, int s, int g, const char *name, size_t n)
{
    char *t;
    if (x->counts[s][g] == x->caps[s][g]) {
        size_t cap = x->caps[s][g] ? x->caps[s][g] * 2 : 16;
        char **gr = (char **)realloc(x->names[s][g], cap * sizeof *gr);
        if (!gr) return 0;
        x->names[s][g] = gr;
        x->caps[s][g] = cap;
    }
    if (!(t = (char *)malloc(n + 1))) return 0;
    memcpy(t, name, n);
    t[n] = 0;
    x->names[s][g][x->counts[s][g]++] = t;
    return 1;
}

static char *mp_dupn(const void *p, size_t n)
{
    char *t = (char *)malloc(n + 1);
    if (!t) return NULL;
    memcpy(t, p, n);
    t[n] = 0;
    return t;
}

static int mp_bin_parse(mp_bin *x, const unsigned char *b, size_t len, mp_vec_fn is_vec)
{
    size_t at = 12, i;
    uint32_t count;
    int s, g;
    memset(x, 0, sizeof *x);
    x->b = b;
    x->len = len;
    if (len < 64 || mp_be32(b) != 0xb68800adu) return 0;
    if (!mp_bin_string(x, &at, NULL, NULL) || at + 9 > len) return 0;
    at += 9;
    if (at + 4 > len) return 0;
    count = mp_be32(b + at);
    at += 4;
    for (i = 0; i < count; i++) {
        const unsigned char *nm;
        size_t nn;
        unsigned op;
        char *name;
        int vec;
        if (at + 2 > len) return 0;
        op = b[at];
        at += 2;
        if (!mp_bin_string(x, &at, &nm, &nn) || !(name = mp_dupn(nm, nn))) return 0;
        vec = is_vec(name);
        free(name);
        if (vec < 0) return 0;
        if (!(op & 0x1d)) {
            if (vec) at += 16;
            else if (!mp_bin_string(x, &at, NULL, NULL)) return 0;
        } else {
            if (!mp_bin_string(x, &at, NULL, NULL)) return 0;
            if (op == 4) at += 16;
            if (!mp_bin_string(x, &at, NULL, NULL)) return 0;
        }
        if (at > len) return 0;
    }
    x->lists_at = at;
    for (s = 0; s < 3; s++)
        for (g = 0; g < 7; g++) {
            if (at + 4 > len) return 0;
            count = mp_be32(b + at);
            at += 4;
            for (i = 0; i < count; i++) {
                const unsigned char *nm;
                size_t nn;
                if (!mp_bin_string(x, &at, &nm, &nn) || !mp_bin_add(x, s, g, (const char *)nm, nn)) return 0;
            }
        }
    x->mid_at = at;
    if (at + 4 > len) return 0;
    count = mp_be32(b + at);
    at += 4;
    for (i = 0; i < count; i++) { if (!mp_bin_string(x, &at, NULL, NULL)) return 0; at += 4; }
    if (at + 4 > len) return 0;
    count = mp_be32(b + at);
    at += 4;
    for (i = 0; i < count; i++) if (!mp_bin_string(x, &at, NULL, NULL)) return 0;
    at += 8;
    if (at + 4 > len) return 0;
    count = mp_be32(b + at);
    at += 4;
    if (count > 16 || at + 8 * (size_t)count > len) return 0;
    for (i = 0; i < count; i++)
        x->perms[x->nperms++] = (uint64_t)mp_be32(b + at + 8 * i) << 32 | mp_be32(b + at + 8 * i + 4);
    at += 8 * (size_t)count;
    x->block_at = at;
    if (at + 192 > len) return 0;
    for (i = 0; i < 48; i++) x->block[i] = mp_be32(b + at + 4 * i);
    at += 192;
    for (s = 0; s < 3; s++) {
        const unsigned char *t;
        size_t n;
        if (at + 10 > len) return 0;
        x->hdr[s] = b + at;
        at += 10;
        if (!mp_bin_string(x, &at, &t, &n) || !(x->text[s] = mp_dupn(t, n))) return 0;
        x->text_n[s] = n;
    }
    if (at + 36 != len) return 0;
    x->trailer = b + at;
    return 1;
}

static char *mp_bin_encode(const mp_bin *x, unsigned checksum, size_t *out_len)
{
    mp_buf o = { 0 };
    int s, g, i;
    size_t k;
    mp_put(&o, x->b, 4);
    mp_put_be32(&o, checksum);
    mp_put(&o, x->b + 8, x->lists_at - 8);
    for (s = 0; s < 3; s++)
        for (g = 0; g < 7; g++) {
            mp_put_be32(&o, (uint32_t)x->counts[s][g]);
            for (k = 0; k < x->counts[s][g]; k++) {
                size_t n = strlen(x->names[s][g][k]);
                mp_put_le32(&o, (uint32_t)n);
                mp_put(&o, x->names[s][g][k], n);
            }
        }
    mp_put(&o, x->b + x->mid_at, x->block_at - x->mid_at);
    for (i = 0; i < 48; i++) mp_put_be32(&o, x->block[i]);
    for (s = 0; s < 3; s++) {
        mp_put(&o, x->hdr[s], 10);
        mp_put_le32(&o, (uint32_t)x->text_n[s]);
        mp_put(&o, x->text[s], x->text_n[s]);
    }
    mp_put(&o, x->trailer, 36);
    return mp_take(&o, out_len);
}

/* ------------------------------------------------------------------------------------------- derivation ---- */

typedef struct mp_file { char *path; unsigned char *bytes; size_t len; } mp_file;

static SRWLOCK g_lock = SRWLOCK_INIT;
static mp_file *g_files;
static size_t g_nfiles, g_cfiles;
static int g_done[MP_NBASES];        /* 1 derived, -1 failed */
static char *g_cause;
static sh_modprog_reader g_reader;

static int mp_default_read(const char *path, unsigned char **bytes, size_t *len)
{
    char sha[SH_MB_HASH_HEX + 1];
    unsigned long long size = 0;
    int r = sh_modstock_identify(path, sha, &size);
    if (r != SH_MB_STOCK_OK) return r;
    return sh_modstock_read(path, sha, bytes, len);
}

static int mp_read(const char *path, unsigned char **bytes, size_t *len)
{
    sh_modprog_reader r = g_reader ? g_reader : mp_default_read;
    return r(path, bytes, len);
}

static void mp_heap_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* A render parm's value shape in a cooked program's parameter block: 1 a vector, 0 a name, -1 unreadable. */
static int mp_parm_is_vec(const char *parm)
{
    unsigned char *t = NULL;
    size_t n = 0, i = 0;
    char *path = sh_textf("generated/decls/renderparm/%s.decl", parm);
    int r;
    if (!path) return -1;
    r = mp_read(path, &t, &n);
    free(path);
    if (r != SH_MB_STOCK_OK) return -1;
    while (i < n && (t[i] == '{' || t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
    r = i + 3 <= n && (t[i] | 32) == 'v' && (t[i + 1] | 32) == 'e' && (t[i + 2] | 32) == 'c' &&
        (i + 3 == n || !((t[i + 3] | 32) >= 'a' && (t[i + 3] | 32) <= 'z'));
    mp_heap_free(t);
    return r;
}

static int mp_keep(const char *path, char *bytes, size_t len)
{
    char *p;
    if (!bytes) return 0;
    if (g_nfiles == g_cfiles) {
        size_t cap = g_cfiles ? g_cfiles * 2 : 32;
        mp_file *g = (mp_file *)realloc(g_files, cap * sizeof *g);
        if (!g) { free(bytes); return 0; }
        g_files = g;
        g_cfiles = cap;
    }
    if (!(p = mp_dupn(path, strlen(path)))) { free(bytes); return 0; }
    g_files[g_nfiles].path = p;
    g_files[g_nfiles].bytes = (unsigned char *)bytes;
    g_files[g_nfiles].len = len;
    g_nfiles++;
    return 1;
}

static void mp_fail(const mp_family *f, const char *base, const char *why)
{
    free(g_cause);
    g_cause = sh_textf("Snapmap+ could not build its %s of the game's %s program: %s (check the "
                       "game's files in Steam)", f->what, base, why);
}

/* One unit (the program, or one of its permutations) of a base: its four files. Caller holds the lock. */
static int mp_unit(const mp_family *fam, const char *base, const char *mask)
{
    char *sd = NULL, *sb = NULL, *sv = NULL, *sf = NULL, *vd = NULL, *vb = NULL, *vv = NULL, *vf = NULL;
    char *var = sh_textf("%s%s", fam->prefix, base), *decl = NULL, *glsl = NULL, *bin_out = NULL;
    unsigned char *d = NULL, *b = NULL, *v = NULL, *f = NULL;
    size_t dn = 0, bn = 0, vn = 0, fn = 0, decl_n = 0, glsl_n = 0, bin_n = 0, mod_n = 0;
    uint32_t *mod = NULL;
    mp_bin x;
    int ok = 0;
    const char *why = NULL;
    memset(&x, 0, sizeof x);
    if (!var) return 0;
    if (mask) {
        sd = sh_textf("generated/renderprogs/permutations/%s_%s.decl", base, mask);
        sb = sh_textf("generated/renderprogs/permutations/%s_%s_pc_vulkan.bin", base, mask);
        vd = sh_textf("generated/renderprogs/permutations/%s_%s.decl", var, mask);
        vb = sh_textf("generated/renderprogs/permutations/%s_%s_pc_vulkan.bin", var, mask);
        if (sd && vd) {
            sv = sh_mb_spirv_request(sd, ".vspv"); sf = sh_mb_spirv_request(sd, ".fspv");
            vv = sh_mb_spirv_request(vd, ".vspv"); vf = sh_mb_spirv_request(vd, ".fspv");
        }
    } else {
        sd = sh_textf("generated/decls/renderprog/%s.decl", base);
        sb = sh_textf("generated/renderprogs/%s_pc_vulkan.bin", base);
        vd = sh_textf("generated/decls/renderprog/%s.decl", var);
        vb = sh_textf("generated/renderprogs/%s_pc_vulkan.bin", var);
        sv = sh_mb_spirv_request(base, ".vspv"); sf = sh_mb_spirv_request(base, ".fspv");
        vv = sh_mb_spirv_request(var, ".vspv"); vf = sh_mb_spirv_request(var, ".fspv");
    }
    if (!sd || !sb || !sv || !sf || !vd || !vb || !vv || !vf) { why = "out of memory"; goto out; }
    if (mp_read(sd, &d, &dn) != SH_MB_STOCK_OK || mp_read(sb, &b, &bn) != SH_MB_STOCK_OK ||
        mp_read(sv, &v, &vn) != SH_MB_STOCK_OK || mp_read(sf, &f, &fn) != SH_MB_STOCK_OK) {
        why = "its installed files cannot be read";
        goto out;
    }
    if (!mp_bin_parse(&x, b, bn, mp_parm_is_vec)) { why = "its cooked program is not the shape Snapmap+ knows"; goto out; }
    if (mp_decl_checksum((const char *)d, dn) != mp_be32(b + 4)) { why = "its cooked program does not match its decl"; goto out; }
    if (vn < 16 || fn < 16 || sh_mb_md5fold(x.text[0], x.text_n[0]) != mp_be32(v + 4) ||
        sh_mb_md5fold(x.text[1], x.text_n[1]) != mp_be32(f + 4)) {
        why = "its SPIR-V does not match its cooked program";
        goto out;
    }
    {
        /* the fragment stage's categories: binding starts at block[13..19], counts at block[34..40] */
        uint32_t *a = x.block + 6 + 7, *cn = x.block + 27 + 7;
        unsigned tex_end = a[4] + cn[4], shift = *fam->ntextures, k;
        if (a[4] == 0xffffffffu || x.counts[1][2] != cn[4] || x.counts[1][3] != cn[4]) {
            why = "its fragment textures do not match its binding table";
            goto out;
        }
        for (k = 5; k < 7; k++) if (a[k] != 0xffffffffu) a[k] += shift;
        cn[4] += shift;
        x.block[5] += shift;
        for (k = 0; k < *fam->nparms; k++)
            if (!mp_bin_add(&x, 1, 0, fam->parms[k], strlen(fam->parms[k]))) { why = "out of memory"; goto out; }
        for (k = 0; k < *fam->ntextures; k++)
            if (!mp_bin_add(&x, 1, 2, fam->textures[k], strlen(fam->textures[k])) ||
                !mp_bin_add(&x, 1, 3, "defaultsampler", 14)) { why = "out of memory"; goto out; }
        {
            /* a program's textures over all its stages are bounded by the engine (ID_VULKAN_MAX_BOUND_TEXTURES) */
            size_t bound = 0;
            int st;
            for (st = 0; st < 3; st++) bound += x.counts[st][2];
            if (bound > MP_MAX_BOUND_TEXTURES) { why = "its textures would exceed the engine's 32 per program"; goto out; }
        }
        if (!(decl = fam->decl((const char *)d, dn, &decl_n))) {
            why = fam->decl_needs;
            goto out;
        }
        if (!(glsl = fam->glsl(x.text[1], x.text_n[1], tex_end, shift, &glsl_n))) {
            why = "its fragment stage text is not the shape Snapmap+ knows";
            goto out;
        }
        if (!(mod = fam->splice((const uint32_t *)(f + 16), (fn - 16) / 4, tex_end, shift, &mod_n, &why))) goto out;
    }
    free(x.text[1]);
    x.text[1] = glsl;
    x.text_n[1] = glsl_n;
    glsl = NULL;
    if (!(bin_out = mp_bin_encode(&x, mp_decl_checksum(decl, decl_n), &bin_n))) { why = "out of memory"; goto out; }
    {
        mp_buf o = { 0 };
        char *fo, *vo = (char *)malloc(vn ? vn : 1);
        size_t fo_n = 0;
        mp_put_be32(&o, 4);
        mp_put_be32(&o, sh_mb_md5fold(x.text[1], x.text_n[1]));
        mp_put(&o, f + 8, 4);
        mp_put_be32(&o, (uint32_t)(mod_n * 4));
        mp_put(&o, mod, mod_n * 4);
        fo = mp_take(&o, &fo_n);
        if (!fo || !vo) { free(fo); free(vo); why = "out of memory"; goto out; }
        memcpy(vo, v, vn);
        ok = mp_keep(vd, decl, decl_n);
        decl = NULL;
        ok = mp_keep(vb, bin_out, bin_n) && ok;
        bin_out = NULL;
        ok = mp_keep(vv, vo, vn) && ok;
        ok = mp_keep(vf, fo, fo_n) && ok;
        if (!ok) why = "out of memory";
    }
out:
    if (!ok && why) mp_fail(fam, base, why);
    mp_bin_free(&x);
    free(sd); free(sb); free(sv); free(sf); free(vd); free(vb); free(vv); free(vf); free(var);
    free(decl); free(glsl); free(bin_out); free(mod);
    mp_heap_free(d); mp_heap_free(b); mp_heap_free(v); mp_heap_free(f);
    return ok;
}

/* Every unit of a base: the program and each permutation its cooked program lists. Caller holds the lock. */
static int mp_derive(size_t base)
{
    unsigned char *b = NULL;
    size_t bn = 0, i;
    char *sb;
    mp_bin x;
    int ok;
    const mp_family *fam = &MP_FAMILIES[MP_BASES[base].family];
    const char *name = MP_BASES[base].name;
    if (g_done[base]) return g_done[base] > 0;
    sb = sh_textf("generated/renderprogs/%s_pc_vulkan.bin", name);
    memset(&x, 0, sizeof x);
    ok = sb && mp_read(sb, &b, &bn) == SH_MB_STOCK_OK && mp_bin_parse(&x, b, bn, mp_parm_is_vec);
    free(sb);
    if (!ok) mp_fail(fam, name, "its cooked program cannot be read");
    ok = ok && mp_unit(fam, name, NULL);
    for (i = 0; ok && i < x.nperms; i++) {
        char mask[17];
        snprintf(mask, sizeof mask, "%08x%08x", (unsigned)(x.perms[i] >> 32), (unsigned)x.perms[i]);
        ok = mp_unit(fam, name, mask);
    }
    mp_bin_free(&x);
    mp_heap_free(b);
    g_done[base] = ok ? 1 : -1;
    return ok;
}

/* ------------------------------------------------------------------------------- the parameter image ---- */
/* The layered program reads its numbers from an 8 x 4 RGBA16F image (layered.rp): a DOOM 2016 .bimage (the layout
 * the research repository's writer, doomforge/doom2016/image/bimage.py, gives: a 42-byte header, then one record per
 * mip), served from a registry of images the product derives, at a path that is the hash of its bytes. */

#define MP_PARAM_W 8u
#define MP_PARAM_H 4u
#define MP_IMAGE_HEAD "generated/image/snapmapplus/look/"
#define MP_BIMAGE_MAGIC 0x074d4942u
#define MP_FMT_RGBA16F 2u

/* A float as the nearest half (ties to even); beyond the half range, infinity. */
static uint16_t mp_half(float f)
{
    uint32_t x, sign, e, m, half, rem;
    int ex;
    memcpy(&x, &f, 4);
    sign = (x >> 16) & 0x8000u;
    e = (x >> 23) & 0xffu;
    m = x & 0x7fffffu;
    if (e == 0xffu) return (uint16_t)(sign | 0x7c00u | (m ? 0x200u : 0u));
    ex = (int)e - 127 + 15;
    if (ex >= 31) return (uint16_t)(sign | 0x7c00u);
    if (ex <= 0) {
        unsigned shift;
        if (ex < -10) return (uint16_t)sign;
        m |= 0x800000u;
        shift = (unsigned)(14 - ex);
        half = m >> shift;
        rem = m & ((1u << shift) - 1u);
        if (rem > (1u << (shift - 1)) || (rem == (1u << (shift - 1)) && (half & 1u))) half++;
        return (uint16_t)(sign | half);
    }
    half = ((uint32_t)ex << 10) | (m >> 13);
    rem = m & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) half++;   /* a carry runs into the exponent: right */
    return (uint16_t)(sign | half);
}

static float mp_half_float(uint16_t h)
{
    int e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = (float)m * (1.0f / 16777216.0f);
    else if (e == 31) { uint32_t x = 0x7f800000u | ((uint32_t)m << 13); memcpy(&v, &x, 4); }
    else {
        uint32_t x = (uint32_t)(e - 15 + 127) << 23 | (uint32_t)m << 13;
        memcpy(&v, &x, 4);
    }
    return (h & 0x8000u) ? -v : v;
}

int sh_modprog_bimage_rgba16f(unsigned width, unsigned height, const float texel[][4], unsigned char **bytes,
                              size_t *len)
{
    static const unsigned char fmt[10] = { MP_FMT_RGBA16F, 0, 0, 0, 5, 0, 0, 0, 0, 0 };   /* format, filter 5, repeat 0 */
    mp_buf b = { 0 };
    size_t px, i, k;
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (!bytes || !len || !texel || !width || !height || width > 4096u || height > 4096u) return 0;
    px = (size_t)width * height;
    mp_put_be32(&b, 0);                 /* source file time */
    mp_put_be32(&b, MP_BIMAGE_MAGIC);
    mp_put_be32(&b, 0);                 /* texture type */
    mp_put_be32(&b, width);
    mp_put_be32(&b, height);
    mp_put_be32(&b, 0);
    mp_put_be32(&b, 1);                 /* one mip */
    mp_put_be32(&b, 0);
    mp_put(&b, fmt, sizeof fmt);
    mp_put_be32(&b, 0);                 /* the mip: level, destination z, size, data size */
    mp_put_be32(&b, 0);
    mp_put_be32(&b, width);
    mp_put_be32(&b, height);
    mp_put_be32(&b, (uint32_t)(px * 8));
    for (i = 0; i < px; i++)
        for (k = 0; k < 4; k++) {
            uint16_t h = mp_half(texel[i][k]);
            unsigned char x[2] = { (unsigned char)h, (unsigned char)(h >> 8) };
            mp_put(&b, x, 2);
        }
    *bytes = (unsigned char *)mp_take(&b, len);
    return *bytes != NULL;
}

int sh_modprog_param_image(const float vec[][4], unsigned count, unsigned char **bytes, size_t *len)
{
    float texel[MP_PARAM_W * MP_PARAM_H][4];
    unsigned i;
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (!vec || count > MP_PARAM_W * MP_PARAM_H) return 0;
    memset(texel, 0, sizeof texel);
    for (i = 0; i < count; i++) memcpy(texel[i], vec[i], sizeof texel[i]);
    return sh_modprog_bimage_rgba16f(MP_PARAM_W, MP_PARAM_H, texel, bytes, len);
}

/* The registered images (under g_lock): path and bytes. */
typedef struct mp_img { char *path; unsigned char *bytes; size_t len; } mp_img;
static mp_img *g_imgs;
static size_t g_nimgs, g_cimgs;

/* 1 when `path` has the shape of a registered image's (the hash is 16 hex digits; a constant image's has a 'c' before). */
static int mp_image_shape(const char *path)
{
    size_t hn = strlen(MP_IMAGE_HEAD), i;
    if (_strnicmp(path, MP_IMAGE_HEAD, hn)) return 0;
    if (path[hn] == 'c' || path[hn] == 'C') hn++;
    for (i = 0; i < 16; i++) {
        char c = path[hn + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return !_stricmp(path + hn + 16, ".bimage");
}

static long mp_image_find(const char *path)
{
    size_t i;
    for (i = 0; i < g_nimgs; i++) if (!_stricmp(g_imgs[i].path, path)) return (long)i;
    return -1;
}

/* Registers `b` (malloc'd; taken, or freed on failure) under MP_IMAGE_HEAD, `tag`, the first 16 hex digits of its
 * SHA-256 and .bimage; the path, or NULL. */
static const char *mp_register(const char *tag, unsigned char *b, size_t n)
{
    sh_sha256 h;
    char hex[17], *path;
    const char *r = NULL;
    long at;
    sh_sha256_init(&h);
    sh_sha256_update(&h, b, n);
    sh_sha256_hex(&h, hex, 16);
    path = sh_textf(MP_IMAGE_HEAD "%s%s.bimage", tag, hex);
    if (!path) { free(b); return NULL; }
    AcquireSRWLockExclusive(&g_lock);
    if ((at = mp_image_find(path)) >= 0) {
        r = g_imgs[at].path;
    } else {
        if (g_nimgs == g_cimgs) {
            size_t cap = g_cimgs ? g_cimgs * 2 : 16;
            mp_img *g = (mp_img *)realloc(g_imgs, cap * sizeof *g);
            if (g) { g_imgs = g; g_cimgs = cap; }
        }
        if (g_nimgs < g_cimgs) {
            g_imgs[g_nimgs].path = path;
            g_imgs[g_nimgs].bytes = b;
            g_imgs[g_nimgs].len = n;
            r = g_imgs[g_nimgs++].path;
            path = NULL;
            b = NULL;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    free(path);
    free(b);
    return r;
}

const char *sh_modprog_register_param_image(const float vec[][4], unsigned count)
{
    unsigned char *b = NULL;
    size_t n = 0;
    if (!sh_modprog_param_image(vec, count, &b, &n)) return NULL;
    return mp_register("", b, n);
}

const char *sh_modprog_register_const_image(const float rgba[4])
{
    float texel[16][4];
    unsigned char *b = NULL;
    size_t n = 0, i;
    if (!rgba) return NULL;
    for (i = 0; i < 16; i++) memcpy(texel[i], rgba, sizeof texel[i]);
    if (!sh_modprog_bimage_rgba16f(4, 4, texel, &b, &n)) return NULL;
    return mp_register("c", b, n);
}

/* The parm decl a path names (index), or -1. */
static int mp_parm_of(const char *path)
{
    size_t b;
    if (_strnicmp(path, "generated/decls/renderparm/", 27)) return -1;
    for (b = 0; b < MP_NPARM_DECLS; b++) {
        size_t n = strlen(MP_PARM_DECLS[b].name);
        if (!_strnicmp(path + 27, MP_PARM_DECLS[b].name, n) && !_stricmp(path + 27 + n, ".decl")) return (int)b;
    }
    return -1;
}

/* The base a product program path belongs to, or -1. */
static const char *const MP_HEADS[] = {
    "generated/decls/renderprog/", "generated/renderprogs/", "generated/spirv/", "generated/renderprogs/permutations/",
    "generated/spirv/generated/renderprogs/permutations/",
};
#define MP_NHEADS (sizeof MP_HEADS / sizeof MP_HEADS[0])
static int mp_base_of(const char *path)
{
    size_t h, b;
    for (h = 0; h < MP_NHEADS; h++) {
        size_t hn = strlen(MP_HEADS[h]);
        if (_strnicmp(path, MP_HEADS[h], hn)) continue;
        for (b = 0; b < MP_NBASES; b++) {
            const char *pre = MP_FAMILIES[MP_BASES[b].family].prefix;
            size_t pn = strlen(pre), bn = strlen(MP_BASES[b].name);
            char c;
            if (_strnicmp(path + hn, pre, pn) || _strnicmp(path + hn + pn, MP_BASES[b].name, bn)) continue;
            c = path[hn + pn + bn];
            if (c == '.' || c == '_' || c == '/') return (int)b;
        }
    }
    return -1;
}

/* The old program names (SH_MODPROG_OLD_PREFIX, SH_MODPROG_EP_OLD_PREFIX), served until 1.0 for mods emitted before
 * the rename: a path under one of them, after any of the heads, is rewritten to the same path under the new name
 * (a malloc'd copy), so the old name answers with the new name's bytes. NULL when the path names neither. */
static char *mp_canonical(const char *path)
{
    static const char *const OLD[2] = { SH_MODPROG_OLD_PREFIX, SH_MODPROG_EP_OLD_PREFIX };
    static const char *const NEW[2] = { SH_MODPROG_PREFIX, SH_MODPROG_EP_PREFIX };
    size_t h, k;
    for (h = 0; h < MP_NHEADS; h++) {
        size_t hn = strlen(MP_HEADS[h]);
        if (_strnicmp(path, MP_HEADS[h], hn)) continue;
        for (k = 0; k < 2; k++) {
            size_t on = strlen(OLD[k]);
            if (_strnicmp(path + hn, OLD[k], on)) continue;
            return sh_textf("%.*s%s%s", (int)hn, path, NEW[k], path + hn + on);
        }
    }
    return NULL;
}

int sh_modprog_owns(const char *path)
{
    char *c;
    int r;
    if (!path) return 0;
    if (mp_parm_of(path) >= 0) return 1;
    if (mp_image_shape(path)) {
        AcquireSRWLockExclusive(&g_lock);
        r = mp_image_find(path) >= 0;
        ReleaseSRWLockExclusive(&g_lock);
        return r;
    }
    c = mp_canonical(path);
    r = mp_base_of(c ? c : path) >= 0;
    free(c);
    return r;
}

static unsigned char *mp_heap_copy(const void *p, size_t n)
{
    unsigned char *h = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (h) memcpy(h, p, n);
    return h;
}

static int mp_read_new(const char *path, unsigned char **bytes, size_t *len);

int sh_modprog_read(const char *path, unsigned char **bytes, size_t *len)
{
    char *c;
    int r;
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (!path || !bytes || !len) return SH_MB_STOCK_ABSENT;
    c = mp_canonical(path);
    r = mp_read_new(c ? c : path, bytes, len);
    free(c);
    return r;
}

static int mp_read_new(const char *path, unsigned char **bytes, size_t *len)
{
    int base, parm, r = SH_MB_STOCK_ABSENT;
    size_t i;
    if ((parm = mp_parm_of(path)) >= 0) {
        size_t t = strlen(MP_PARM_DECLS[parm].text);
        if (!(*bytes = mp_heap_copy(MP_PARM_DECLS[parm].text, t))) return SH_MB_STOCK_UNAVAILABLE;
        *len = t;
        return SH_MB_STOCK_OK;
    }
    if (mp_image_shape(path)) {
        long at;
        AcquireSRWLockExclusive(&g_lock);
        if ((at = mp_image_find(path)) >= 0) {
            r = (*bytes = mp_heap_copy(g_imgs[at].bytes, g_imgs[at].len)) != NULL ? SH_MB_STOCK_OK : SH_MB_STOCK_UNAVAILABLE;
            if (*bytes) *len = g_imgs[at].len;
        }
        ReleaseSRWLockExclusive(&g_lock);
        return r;
    }
    if ((base = mp_base_of(path)) < 0) return SH_MB_STOCK_ABSENT;
    AcquireSRWLockExclusive(&g_lock);
    if (!mp_derive((size_t)base)) r = SH_MB_STOCK_UNAVAILABLE;
    else
        for (i = 0; i < g_nfiles; i++)
            if (!_stricmp(g_files[i].path, path)) {
                r = (*bytes = mp_heap_copy(g_files[i].bytes, g_files[i].len)) != NULL ? SH_MB_STOCK_OK : SH_MB_STOCK_UNAVAILABLE;
                if (*bytes) *len = g_files[i].len;
                break;
            }
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

void sh_modprog_list(const char *prefix, void (*fn)(void *context, const char *path), void *context)
{
    const char *pre = prefix ? prefix : "";
    size_t i, n = strlen(pre), b, np = 0;
    char **paths = NULL;
    int overlaps = 0;
    if (!fn) return;
    for (i = 0; i < MP_NPARM_DECLS; i++) {
        char *p = sh_textf("generated/decls/renderparm/%s.decl", MP_PARM_DECLS[i].name);
        if (p && !_strnicmp(p, pre, n)) fn(context, p);
        free(p);
    }
    /* the registered parameter images */
    {
        char **img = NULL;
        size_t ni = 0;
        AcquireSRWLockExclusive(&g_lock);
        if (g_nimgs && (img = (char **)calloc(g_nimgs, sizeof *img)) != NULL)
            for (i = 0; i < g_nimgs; i++)
                if (!_strnicmp(g_imgs[i].path, pre, n)) img[ni++] = mp_dupn(g_imgs[i].path, strlen(g_imgs[i].path));
        ReleaseSRWLockExclusive(&g_lock);
        for (i = 0; i < ni; i++) { if (img[i]) fn(context, img[i]); free(img[i]); }
        free(img);
    }
    /* Only a prefix inside (or above) a program namespace asks for the derivation. */
    for (i = 0; i < MP_NHEADS; i++) {
        size_t f;
        for (f = 0; f < MP_NFAMILIES; f++) {
            char *head = sh_textf("%s%s", MP_HEADS[i], MP_FAMILIES[f].prefix);
            size_t hn = head ? strlen(head) : 0;
            if (head && !_strnicmp(head, pre, n < hn ? n : hn)) overlaps = 1;
            free(head);
        }
    }
    if (!overlaps) return;
    AcquireSRWLockExclusive(&g_lock);
    for (b = 0; b < MP_NBASES; b++) (void)mp_derive(b);
    if (g_nfiles && (paths = (char **)calloc(g_nfiles, sizeof *paths)) != NULL)
        for (i = 0; i < g_nfiles; i++)
            if (!_strnicmp(g_files[i].path, pre, n))
                paths[np++] = mp_dupn(g_files[i].path, strlen(g_files[i].path));
    ReleaseSRWLockExclusive(&g_lock);
    for (i = 0; i < np; i++) { if (paths[i]) fn(context, paths[i]); free(paths[i]); }
    free(paths);
}

const char *sh_modprog_cause(void) { return g_cause ? g_cause : ""; }

void sh_modprog_set_reader(sh_modprog_reader reader)
{
    AcquireSRWLockExclusive(&g_lock);
    g_reader = reader;
    ReleaseSRWLockExclusive(&g_lock);
    sh_modprog_reset();
}

void sh_modprog_reset(void)
{
    size_t i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_nfiles; i++) { free(g_files[i].path); free(g_files[i].bytes); }
    free(g_files);
    g_files = NULL;
    g_nfiles = g_cfiles = 0;
    memset(g_done, 0, sizeof g_done);
    for (i = 0; i < g_nimgs; i++) { free(g_imgs[i].path); free(g_imgs[i].bytes); }
    free(g_imgs);
    g_imgs = NULL;
    g_nimgs = g_cimgs = 0;
    free(g_cause);
    g_cause = NULL;
    ReleaseSRWLockExclusive(&g_lock);
}

/* ------------------------------------------------------------------------------- old material keys ---- */
/* A material emitted before the numbers moved names the old keys (the 28 layered vectors and blendlayer0heightmap;
 * eternalparticle, eternalparticlefade, eternalparticlecolor, eternalparticlecolor2 and eternalparticleemissivemap).
 * sh_modprog_legacy_material rewrites them into the new storage so that the material draws the same. A material is a
 * braced list of lines, `key<blanks>value`. */

typedef struct mp_ln {
    const char *a;          /* the line, without its terminator */
    size_t an;
    const char *eol;        /* its terminator ("" for a last line without one) */
    size_t en;
    size_t ks, kn;          /* the key's offset and length in the line (kn 0: not a key line: a brace, a comment, blank) */
    size_t v0, vn;          /* the value's offset and length in the line */
    int drop;
    char *text;             /* the replacement line, without its terminator (malloc'd), or NULL */
} mp_ln;

typedef struct mp_new { size_t before; char *text; } mp_new;   /* a new line, put before line `before` */
typedef struct mp_mat {
    mp_ln *l;
    size_t n;
    size_t closing;         /* the line of the closing brace: where a line is appended */
    mp_new q[16];
    size_t nq;
    const char *eol;
} mp_mat;

static int mp_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static void mp_mat_free(mp_mat *m)
{
    size_t i;
    for (i = 0; i < m->n; i++) free(m->l[i].text);
    for (i = 0; i < m->nq; i++) free(m->q[i].text);
    free(m->l);
    memset(m, 0, sizeof *m);
}

/* The text split into lines. 0 on no memory. */
static int mp_split(mp_mat *m, const char *text, size_t len)
{
    size_t count = 1, i, at = 0;
    memset(m, 0, sizeof *m);
    m->eol = (len && memchr(text, '\r', len)) ? "\r\n" : "\n";
    for (i = 0; i < len; i++) if (text[i] == '\n') count++;
    if (!(m->l = (mp_ln *)calloc(count, sizeof *m->l))) return 0;
    for (;;) {
        mp_ln *x = &m->l[m->n++];
        size_t e = at, k = 0;
        while (e < len && text[e] != '\n') e++;
        x->a = text + at;
        x->an = e - at;
        x->eol = text + e;
        x->en = e < len ? 1 : 0;
        if (x->an && x->a[x->an - 1] == '\r') { x->an--; x->eol = text + e - 1; x->en = e < len ? 2 : 1; }
        while (k < x->an && mp_blank(x->a[k])) k++;
        if (k < x->an && x->a[k] != '{' && x->a[k] != '}' && x->a[k] != '/') {
            x->ks = k;
            while (k < x->an && !mp_blank(x->a[k])) k++;
            x->kn = k - x->ks;
            while (k < x->an && mp_blank(x->a[k])) k++;
            x->v0 = k;
            x->vn = x->an - k;
            while (x->vn && mp_blank(x->a[x->v0 + x->vn - 1])) x->vn--;
        } else if (k < x->an && x->a[k] == '}') {
            m->closing = m->n - 1;
        }
        if (e >= len) break;
        at = e + 1;
    }
    return 1;
}

static int mp_is_key(const mp_ln *x, const char *key)
{
    size_t k = strlen(key);
    return x->kn == k && !_strnicmp(x->a + x->ks, key, k);
}

/* The last line of the key (case-insensitive), or -1. */
static long mp_key_at(const mp_mat *m, const char *key)
{
    size_t i;
    long hit = -1;
    for (i = 0; i < m->n; i++) if (mp_is_key(&m->l[i], key)) hit = (long)i;
    return hit;
}

/* A vector value: `{ a, b, c, d }` (missing components 0) or a scalar (all four components). 1 on success. */
static int mp_parse_vec(const char *v, size_t n, float out[4])
{
    char t[256], *p, *e;
    int k = 0;
    if (!n || n >= sizeof t) return 0;
    memcpy(t, v, n);
    t[n] = 0;
    p = t;
    while (mp_blank(*p)) p++;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (*p == '{') {
        p++;
        while (k < 4) {
            while (mp_blank(*p) || *p == ',') p++;
            if (!*p || *p == '}') break;
            out[k] = strtof(p, &e);
            if (e == p) return 0;
            k++;
            p = e;
        }
        return k > 0;
    }
    out[0] = strtof(p, &e);
    if (e == p) return 0;
    out[1] = out[2] = out[3] = out[0];
    return 1;
}

/* The shortest text that reads back as the same float. */
static void mp_fmt_float(char *out, size_t cap, float v)
{
    int p;
    for (p = 1; p <= 9; p++) {
        snprintf(out, cap, "%.*g", p, (double)v);
        if (strtof(out, NULL) == v) return;
    }
}

/* A key's value as a vector (`def` when the material does not set it or sets it unreadably). */
static void mp_get_vec(const mp_mat *m, const char *key, const float def[4], float out[4])
{
    long at = mp_key_at(m, key);
    if (at < 0 || !mp_parse_vec(m->l[at].a + m->l[at].v0, m->l[at].vn, out)) memcpy(out, def, 4 * sizeof(float));
}

/* Every line of a key dropped. */
static void mp_drop_key(mp_mat *m, const char *key)
{
    size_t i;
    for (i = 0; i < m->n; i++) if (mp_is_key(&m->l[i], key)) m->l[i].drop = 1;
}

/* A key set to `value` (`vn` characters): in place where the material has it (its other lines of the key dropped), else
 * as a new line before line `before`. 0 on no memory. */
static int mp_set_text(mp_mat *m, size_t before, const char *key, const char *value, size_t vn)
{
    long at = mp_key_at(m, key);
    char *t = sh_textf("%s\t%.*s", key, (int)vn, value);
    if (!t) return 0;
    if (at < 0) {
        char *u = sh_textf("%s%s", t, m->eol);
        free(t);
        if (!u || m->nq == sizeof m->q / sizeof m->q[0]) { free(u); return 0; }
        m->q[m->nq].before = before;
        m->q[m->nq++].text = u;
        return 1;
    }
    mp_drop_key(m, key);
    m->l[at].drop = 0;
    free(m->l[at].text);
    m->l[at].text = t;
    return 1;
}

static int mp_set_vec(mp_mat *m, size_t before, const char *key, const float v[4])
{
    char c[4][40], t[200];
    int k;
    for (k = 0; k < 4; k++) mp_fmt_float(c[k], sizeof c[k], v[k]);
    snprintf(t, sizeof t, "{ %s, %s, %s, %s }", c[0], c[1], c[2], c[3]);
    return mp_set_text(m, before, key, t, strlen(t));
}

/* The earliest of two lines (-1 for none). */
static long mp_first(long a, long b) { return a < 0 ? b : (b < 0 || a < b) ? a : b; }

/* eternalparticle (mode, emissive multiplier, mask multiplier, depth offset scale), eternalparticlefade (near fade
 * begin and end, soft distance, 1 with an emissive sheet), eternalparticlecolor (mask colour or gradient 2),
 * eternalparticlecolor2 (gradient 3; w the lit scale) and eternalparticleemissivemap become the stock keys
 * particles.rp reads (particleuberparms1..3, emissive, smokebrightness) and particleemissivemap. 1 rewritten, 0 no
 * old key, -1 no memory. */
static int mp_legacy_particle(mp_mat *m)
{
    static const char *const old[] = { "eternalparticle", "eternalparticlefade", "eternalparticlecolor",
                                       "eternalparticlecolor2" };
    static const float z4[4] = { 0, 0, 0, 0 }, o4[4] = { 1, 1, 1, 1 };
    float ep[4], fade[4], col[4], col2[4], u1[4], u2[4], u3[4], em[4], sb[4];
    long first = -1, sheet = mp_key_at(m, "eternalparticleemissivemap");
    size_t k;
    for (k = 0; k < 4; k++) first = mp_first(first, mp_key_at(m, old[k]));
    first = mp_first(first, sheet);
    if (first < 0) return 0;
    mp_get_vec(m, "eternalparticle", z4, ep);
    mp_get_vec(m, "eternalparticlefade", z4, fade);
    mp_get_vec(m, "eternalparticlecolor", o4, col);
    mp_get_vec(m, "eternalparticlecolor2", o4, col2);
    /* particleuberparms1: x and y 0; z the mode, + 16 with the emissive sheet; w the depth offset scale */
    u1[0] = 0.0f; u1[1] = 0.0f; u1[2] = ep[0] + (fade[3] != 0.0f ? 16.0f : 0.0f); u1[3] = ep[3];
    u2[0] = col[0]; u2[1] = col[1]; u2[2] = col[2]; u2[3] = fade[0];
    u3[0] = col2[0]; u3[1] = col2[1]; u3[2] = col2[2]; u3[3] = ep[1];
    /* emissive: x the material's own (a scalar or a vector), y the near fade's end, z the soft distance, w the lit scale */
    mp_get_vec(m, "emissive", o4, em);
    em[1] = fade[1]; em[2] = fade[2]; em[3] = col2[3];
    mp_get_vec(m, "smokebrightness", z4, sb);
    sb[0] = ep[2];
    for (k = 0; k < 4; k++) mp_drop_key(m, old[k]);
    if (sheet >= 0) {
        mp_ln *x = &m->l[sheet];
        char *t = sh_textf("particleemissivemap\t%.*s", (int)x->vn, x->a + x->v0);
        if (!t) return -1;
        free(x->text);
        x->text = t;
        mp_drop_key(m, "eternalparticleemissivemap");
        x->drop = 0;
    }
    return mp_set_vec(m, (size_t)first, "particleuberparms1", u1) && mp_set_vec(m, (size_t)first, "particleuberparms2", u2) &&
           mp_set_vec(m, (size_t)first, "particleuberparms3", u3) && mp_set_vec(m, (size_t)first, "emissive", em) &&
           mp_set_vec(m, (size_t)first, "smokebrightness", sb) ? 1 : -1;
}

/* 1 when the image name is a constant colour image (const_<h>_<h>_<h>_<h>, four half floats, as the converters name
 * them: the last component of the path), with its first channel in *value. */
static int mp_const_image(const char *v, size_t n, float *value)
{
    size_t i = n, k, d;
    unsigned h = 0;
    const char *s;
    while (i && v[i - 1] != '/') i--;
    s = v + i;
    n -= i;
    if (n < 6 + 19 || _strnicmp(s, "const_", 6)) return 0;
    s += 6;
    for (k = 0; k < 4; k++) {
        for (d = 0; d < 4; d++) {
            char c = (char)(s[k * 5 + d] | 32);
            unsigned x;
            if (c >= '0' && c <= '9') x = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') x = (unsigned)(c - 'a' + 10);
            else return 0;
            if (k == 0) h = h << 4 | x;
        }
        if (k < 3 && s[k * 5 + 4] != '_') return 0;
    }
    *value = mp_half_float((uint16_t)h);
    return 1;
}

/* The base height's neutral value: half of the 8-bit range, as the converters write a flat height. */
#define MP_FLAT_HEIGHT 0.498046875f

/* The 1 or 2 an extra-map reference holds (the extra map 0 or 1 it names), else 0. */
static int mp_extra_use(float v) { return v == 1.0f ? 1 : v == 2.0f ? 2 : 0; }

/* The 28 vectors and blendlayer0heightmap become the `layerparms` image the vectors are written into, vector 9
 * carrying where layer 0's height comes from. 1 rewritten, 0 no old key, -1 no memory. */
static int mp_legacy_layered(mp_mat *m)
{
    float v[28][4];
    long first = mp_key_at(m, "blendlayer0heightmap"), hm = first;
    unsigned k;
    int used[3] = { 0, 0, 0 };
    const char *path;
    char name[96];
    float y = 0.0f, z = MP_FLAT_HEIGHT, c;
    size_t pn;
    for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++) {
        mp_get_vec(m, SH_MODPROG_ML_PARMS[k], ML_DEFAULTS[k], v[k]);
        first = mp_first(first, mp_key_at(m, SH_MODPROG_ML_PARMS[k]));
    }
    if (first < 0) return 0;
    /* the extra maps in use: a layer's specular or glow image (w of 16..23), layer 0's glow image, the burn's tile and
     * the wound cap's blood (y, z, w of 24) */
    for (k = 16; k < 24; k++) used[mp_extra_use(v[k][3])] = 1;
    for (k = 1; k < 4; k++) used[mp_extra_use(v[24][k])] = 1;
    if (hm >= 0) {
        const char *val = m->l[hm].a + m->l[hm].v0;
        size_t vn = m->l[hm].vn;
        if (vn >= 2 && val[0] == '"' && val[vn - 1] == '"') { val++; vn -= 2; }
        if (mp_const_image(val, vn, &c)) {
            z = c;                                   /* a constant image: its value */
        } else if (!used[1] || !used[2]) {
            char key[24];                            /* a free extra map takes the image */
            int slot = used[1] ? 1 : 0;
            snprintf(key, sizeof key, "multilayerextramap%d", slot);
            if (!mp_set_text(m, m->closing, key, val, vn)) return -1;
            y = (float)(slot + 1);
            z = 0.0f;
        }                                            /* else its neutral value: its own mean is not known here */
    }
    v[9][1] = y;
    v[9][2] = z;
    v[9][3] = 0.0f;
    if (!(path = sh_modprog_register_param_image((const float (*)[4])v, SH_MODPROG_ML_PARM_COUNT))) return -1;
    /* the material names the image by its name: the path without generated/image/ and its extension, as .tga */
    pn = strlen(path) - strlen("generated/image/") - strlen(".bimage");
    snprintf(name, sizeof name, "%.*s.tga", (int)pn, path + strlen("generated/image/"));
    for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++) mp_drop_key(m, SH_MODPROG_ML_PARMS[k]);
    mp_drop_key(m, "blendlayer0heightmap");
    return mp_set_text(m, (size_t)first, "layerparms", name, strlen(name)) ? 1 : -1;
}

int sh_modprog_legacy_material(const char *text, size_t len, char **out, size_t *out_len)
{
    mp_mat m;
    mp_buf b = { 0 };
    size_t i, j;
    int p, ml;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!text || !out) return -1;
    if (!mp_split(&m, text, len)) return -1;
    p = mp_legacy_particle(&m);
    ml = p < 0 ? -1 : mp_legacy_layered(&m);
    if (p < 0 || ml < 0) { mp_mat_free(&m); return -1; }
    if (!p && !ml) { mp_mat_free(&m); return 0; }
    for (i = 0; i <= m.n; i++) {
        for (j = 0; j < m.nq; j++) if (m.q[j].before == i) mp_puts(&b, m.q[j].text);
        if (i == m.n || m.l[i].drop) continue;
        if (m.l[i].text) mp_puts(&b, m.l[i].text);
        else mp_put(&b, m.l[i].a, m.l[i].an);
        mp_put(&b, m.l[i].eol, m.l[i].en);
    }
    mp_mat_free(&m);
    *out = mp_take(&b, out_len);
    return *out ? 1 : -1;
}
