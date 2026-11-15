/* modprog.c -- render programs the product derives from the installed game's own (modprog.h). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/text.h"
#include "modprog.h"
#include "modbundle.h"
#include "modstock.h"

/* ------------------------------------------------------------------------------------------- the family ---- */

static const char *const MP_BASES[] = { "skinvmtr", "skinvmtrwounds" };
#define MP_NBASES (sizeof MP_BASES / sizeof MP_BASES[0])

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
const char *const SH_MODPROG_ML_TEXTURES[] = {
    "layerblendmask", "blendlayer0heightmap",
    "blendlayer1albedomap", "blendlayer1normalmap", "blendlayer1smoothnessmap", "blendlayer1heightmap",
    "blendlayer2albedomap", "blendlayer2normalmap", "blendlayer2smoothnessmap", "blendlayer2heightmap",
    "blendlayer3albedomap", "blendlayer3normalmap", "blendlayer3smoothnessmap", "blendlayer3heightmap",
    "blendlayer4albedomap", "blendlayer4normalmap", "blendlayer4smoothnessmap", "blendlayer4heightmap",
    "multilayerextramap0", "multilayerextramap1",
};
const unsigned SH_MODPROG_ML_TEXTURE_COUNT = sizeof SH_MODPROG_ML_TEXTURES / sizeof SH_MODPROG_ML_TEXTURES[0];
/* The module's samplers, in the textures' order. */
static const char *const MP_SAMPLERS[] = {
    "ml_mask", "ml_height0",
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

/* The render parm decls: the blend's defaults (DOOM Eternal's template/boss_character where it names one). */
typedef struct mp_parm { const char *name, *text; } mp_parm;
static const mp_parm MP_PARM_DECLS[] = {
    { "layerstilingfactor",        "{ Vec { 1.0, 1.0, 1.0, 1.0 } }" },
    { "multiblendlayerscale",      "{ Vec { 1.0, 1.0, 1.0, 1.0 } }" },
    { "multiblendheightmapscale",  "{ Vec { 1.0, 1.0, 1.0, 1.0 } }" },
    { "multiblendsoftness",        "{ Vec { 0.25, 0.25, 0.25, 0.25 } }" },
    { "multiblendalbedoscale",     "{ Vec { 1.0, 1.0, 1.0, 1.0 } }" },
    { "multiblendnormalscale",     "{ Vec { 1.0, 1.0, 1.0, 1.0 } }" },
    { "multiblendspecularscale",   "{ Vec { 1.0, 1.0, 1.0, 1.0 } }" },
    { "multiblendsmoothnessscale", "{ Vec { 1.0, 1.0, 1.0, 1.0 } }" },
    { "multiblendemissivescale",   "{ Vec { 1.0, 1.0, 1.0, 1.0 } }" },
    { "blendheightmapscale",       "{ Vec { 1.0, 0.0, 0.0, 0.0 } }" },
    { "blendlayer0tiling",         "{ Vec { 1.0, 1.0, 0.0, 0.0 } }" },
    { "blendlayer0emissive",       "{ Vec { 1.0, 1.0, 1.0, 0.0 } }" },
    { "blendlayer1tiling",         "{ Vec { 1.0, 1.0, 0.0, 0.0 } }" },
    { "blendlayer2tiling",         "{ Vec { 1.0, 1.0, 0.0, 0.0 } }" },
    { "blendlayer3tiling",         "{ Vec { 1.0, 1.0, 0.0, 0.0 } }" },
    { "blendlayer4tiling",         "{ Vec { 1.0, 1.0, 0.0, 0.0 } }" },
    { "blendlayer1specular",       "{ Vec { 0.0, 0.0, 0.0, 0.0 } }" },
    { "blendlayer2specular",       "{ Vec { 0.0, 0.0, 0.0, 0.0 } }" },
    { "blendlayer3specular",       "{ Vec { 0.0, 0.0, 0.0, 0.0 } }" },
    { "blendlayer4specular",       "{ Vec { 0.0, 0.0, 0.0, 0.0 } }" },
    { "blendlayer1emissive",       "{ Vec { 0.0, 0.0, 0.0, 0.0 } }" },
    { "blendlayer2emissive",       "{ Vec { 0.0, 0.0, 0.0, 0.0 } }" },
    { "blendlayer3emissive",       "{ Vec { 0.0, 0.0, 0.0, 0.0 } }" },
    { "blendlayer4emissive",       "{ Vec { 0.0, 0.0, 0.0, 0.0 } }" },
    /* No ember: an isember material's converter writes all four. */
    { "multilayerember",           "{ Vec { 0.0, 0.0, 0.0, 0.0 } }" },
    { "multilayerembertiling",     "{ Vec { 4.0, 4.0, 0.0, 0.0 } }" },
    { "multilayerembernoise",      "{ Vec { 1.0, 4.0, 0.7, 0.4 } }" },
    { "multilayeremberhot",        "{ Vec { 1.0, 0.07, 0.0, 0.0 } }" },
    /* No mask, no layer: what an image key the material does not set gives. */
    { "layerblendmask",            "{ Tex2D textures/system/constant_color/black }" },
    { "blendlayer0heightmap",      "{ Tex2D textures/system/constant_color/grey_md }" },
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
};
#define MP_NPARM_DECLS (sizeof MP_PARM_DECLS / sizeof MP_PARM_DECLS[0])

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

/* ---------------------------------------------------------------------------------------------- decl ---- */

char *sh_modprog_patch_decl(const char *text, size_t len, size_t *out_len)
{
    static const char key[] = "#template CUSTOM_TEXTURE_INPUTS( inputs ) {";
    const char *nl = mp_find(text, len, "\r\n") ? "\r\n" : "\n";
    const char *k = mp_find(text, len, key), *line, *eol, *p;
    mp_buf b = { 0 };
    size_t nll = strlen(nl);
    if (!k || mp_find(k + 1, len - (size_t)(k + 1 - text), key)) return NULL;
    line = k;
    while (line > text && line[-1] != '\n') line--;
    eol = mp_find(k, len - (size_t)(k - text), nl);
    if (!eol) return NULL;
    mp_put(&b, text, (size_t)(line - text));
    /* the blend's templates, in the base text's line ends */
    for (p = SH_MODPROG_ML_DECL; *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        mp_put(&b, p, n);
        mp_put(&b, nl, nll);
        p += n + (e ? 1 : 0);
    }
    mp_put(&b, line, (size_t)(eol - line));
    mp_put(&b, nl, nll);
    mp_put(&b, line, (size_t)(k - line));
    mp_puts(&b, "\tMULTILAYER_BLEND( inputs );");
    mp_put(&b, eol, len - (size_t)(eol - text));
    return mp_take(&b, out_len);
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
    /* 2. the parameters after the material uniforms */
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
    for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++) mp_printf(&b, "\tvec4 %s;\n", SH_MODPROG_ML_PARMS[k]);
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
                "inputs.emissive, inputs.thickness, inputs.texCoords, freqLow_fragmentUniforms.time.x, vec4[");
    mp_printf(&b, "%u]( ", SH_MODPROG_ML_PARM_COUNT);
    for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++)
        mp_printf(&b, "%sfreqHigh_fragmentUniforms.%s", k ? ", " : "", SH_MODPROG_ML_PARMS[k]);
    mp_puts(&b, " ), vec4[4]( ");
    for (k = 0; k < 4; k++) mp_printf(&b, "%sfreqHigh_fragmentUniforms.%s", k ? ", " : "", MP_EMISSIVE[k]);
    mp_puts(&b, " ) );\n");
    mp_put(&b, a, (size_t)(end - a));
    free(t1);
    return mp_take(&b, out_len);
}

/* --------------------------------------------------------------------------------------------- SPIR-V ---- */
/* A module as a list of instructions; an instruction's words (opcode word first) point into the input or into
 * an owned copy. Only the opcodes glslang emits for shaders/multilayer.frag need their operand shapes (the
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
    OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88, OpUndef = 1, OpPhi = 245
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
    case OpMemberDecorate: case OpCapability: case OpEntryPoint: case OpExecutionMode: case OpSource:
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
    case OpMemberDecorate: case OpCapability: case OpEntryPoint: case OpExecutionMode: case OpSource:
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
    case OpReturn: case OpFunctionEnd: case OpExtInstImport: case OpUndef:
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
    case OpCompositeExtract: ADD(3); break;
    case OpCompositeInsert: ADD(3); ADD(4); break;
    case OpExtInst: ADD(3); for (k = 5; k < n; k++) ADD(k); break;
    case OpImageSampleImplicitLod: case OpImageSampleExplicitLod:
        ADD(3); ADD(4); for (k = 6; k < n; k++) ADD(k);
        break;
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

uint32_t *sh_modprog_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                            size_t *out_words, const char **why)
{
    mp_ctx c;
    uint32_t *out = NULL, *mark = NULL, *ids = NULL;
    size_t i, k, nkeep = 0, *keep = NULL, *fstart = NULL, nf = 0;
    uint32_t entry_fn = 0, inputs = 0, in_struct = 0, fh = 0, fl = 0, fh_struct = 0, fl_struct = 0, v4 = 0;
    uint32_t n0 = 0, wound = 0, t_float = 0, t_void = 0, ml_entry = 0;
    uint32_t params[MP_NINOUT + 4], temps[MP_NINOUT + 4];
    uint32_t sampler_ids[MP_NSAMPLERS];
    size_t s_main = 0, s_main_end = 0, anchor = 0, var_end = 0;
    mp_list code = { 0 }, heads = { 0 }, funcs = { 0 };
    memset(&c, 0, sizeof c);
    memset(sampler_ids, 0, sizeof sampler_ids);
    if (out_words) *out_words = 0;
    if (!mp_parse(&c.S, stock, stock_words)) MP_FAIL(&c, "the program's fragment module is not SPIR-V");
    if (!mp_parse(&c.M, SH_MODPROG_ML_SPIRV, SH_MODPROG_ML_SPIRV_WORDS)) MP_FAIL(&c, "the multilayer module is damaged");
    if (!(c.idmap = (uint32_t *)calloc(c.M.bound, sizeof *c.idmap)) ||
        !(mark = (uint32_t *)calloc(c.M.bound, sizeof *mark)) ||
        !(fstart = (size_t *)calloc(c.M.n + 1, sizeof *fstart)) || !(keep = (size_t *)calloc(c.M.n + 1, sizeof *keep)))
        MP_FAIL(&c, "out of memory");

    /* ---- the multilayer functions: ml_blend and what it calls (module order) */
    for (i = 0; i < c.M.n; i++) if (MP_OP(&c.M.ins[i]) == OpFunction) fstart[nf++] = i;
    entry_fn = mp_named(&c.M, "ml_blend(", 1);
    if (!entry_fn) MP_FAIL(&c, "the multilayer module has no single ml_blend");
    /* mark bits: 1 kept function, 2 defined inside a kept function, 4 a global a kept function needs */
    mark[entry_fn] |= 1;
    for (;;) {
        int grew = 0;
        for (k = 0; k < nf; k++) {
            uint32_t fid = c.M.ins[fstart[k]].w[2];
            if (!(mark[fid] & 1)) continue;
            for (i = fstart[k]; i < c.M.n && MP_OP(&c.M.ins[i]) != OpFunctionEnd; i++)
                if (MP_OP(&c.M.ins[i]) == OpFunctionCall && !(mark[c.M.ins[i].w[3]] & 1)) {
                    mark[c.M.ins[i].w[3]] |= 1;
                    grew = 1;
                }
        }
        if (!grew) break;
    }
    for (k = 0; k < nf; k++) {
        uint32_t fid = c.M.ins[fstart[k]].w[2];
        if (!(mark[fid] & 1)) continue;
        keep[nkeep++] = fstart[k];
        for (i = fstart[k]; i < c.M.n; i++) {
            uint32_t r = mp_rid(&c.M.ins[i]);
            if (r) mark[r] |= 2;
            if (MP_OP(&c.M.ins[i]) == OpFunctionEnd) break;
        }
    }
    /* the globals they reference, transitively */
    for (;;) {
        int grew = 0;
        for (i = 0; i < c.M.n; i++) {
            const mp_ins *in = &c.M.ins[i];
            uint32_t pos[64], r = mp_rid(in), op = MP_OP(in);
            int np, q, inside = r && (mark[r] & 2), needed = r && (mark[r] & 4);
            int in_fn = 0;
            for (k = 0; k < nkeep; k++) {
                size_t e = keep[k];
                while (e < c.M.n && MP_OP(&c.M.ins[e]) != OpFunctionEnd) e++;
                if (i >= keep[k] && i <= e) { in_fn = 1; break; }
            }
            if (!in_fn && !needed) continue;
            if (op == OpExtInstImport || op == OpName || op == OpMemberName || op == OpDecorate) continue;
            np = mp_ids(in, pos, 64);
            if (np < 0) MP_FAIL(&c, "the multilayer module uses an instruction the splice does not know");
            for (q = 0; q < np; q++) {
                uint32_t v = in->w[pos[q]];
                if (v < c.M.bound && !(mark[v] & 2) && !(mark[v] & 4) && v != r) {
                    mark[v] |= 4;
                    grew = 1;
                }
            }
            (void)inside;
        }
        if (!grew) break;
    }

    /* ---- the multilayer module's globals into the stock module */
    for (i = 0; i < c.M.n; i++) {
        mp_ins *in = &c.M.ins[i];
        uint32_t r = mp_rid(in), op = MP_OP(in), n = MP_N(in), rid_at, pos[64], *w;
        int np, q;
        if (!r || !(mark[r] & 4) || (mark[r] & 2)) continue;
        if (op == OpExtInstImport) {
            size_t s;
            for (s = 0; s < c.S.n; s++)
                if (MP_OP(&c.S.ins[s]) == OpExtInstImport &&
                    !strcmp((const char *)(c.S.ins[s].w + 2), (const char *)(in->w + 2))) { c.idmap[r] = c.S.ins[s].w[1]; break; }
            if (!c.idmap[r]) MP_FAIL(&c, "the program's fragment module does not import the blend's instructions");
            continue;
        }
        if (!mp_is_type(op) && !mp_is_const(op) && op != OpVariable)
            MP_FAIL(&c, "the multilayer functions read a global the splice does not carry");
        rid_at = mp_has_type(op) ? 2 : 1;
        if (!(w = (uint32_t *)malloc(n * sizeof *w))) MP_FAIL(&c, "out of memory");
        memcpy(w, in->w, n * sizeof *w);
        np = mp_ids(in, pos, 64);
        for (q = 0; q < np; q++) if (pos[q] != rid_at) {
            uint32_t v = c.idmap[w[pos[q]]];
            if (!v) { free(w); MP_FAIL(&c, "the multilayer module refers to a global before it declares it"); }
            w[pos[q]] = v;
        }
        if (op == OpVariable && mp_is_named(&c.M, r, "ml_noise")) {
            /* the ember's noise is the program's own magnificationclouds: no texture is added */
            uint32_t own = mp_named(&c.S, "samp_magnificationclouds", 0);
            const mp_ins *d = own ? mp_def(&c.S, own) : NULL;
            if (!d || MP_OP(d) != OpVariable || d->w[1] != w[1]) {
                free(w);
                MP_FAIL(&c, "the program does not sample magnificationclouds, which the ember reads");
            }
            c.idmap[r] = own;
            free(w);
            continue;
        }
        if (op == OpVariable) {
            int s, hit = -1;
            for (s = 0; s < (int)MP_NSAMPLERS; s++) if (mp_is_named(&c.M, r, MP_SAMPLERS[s])) hit = s;
            if (hit < 0 || w[3] != 0) { free(w); MP_FAIL(&c, "the multilayer functions read a global other than their samplers"); }
            w[2] = c.S.bound++;
            c.idmap[r] = w[2];
            sampler_ids[hit] = w[2];
            if (!mp_emit(&c.S, &c.globals, op, w + 1, n - 1)) { free(w); MP_FAIL(&c, "out of memory"); }
            free(w);
            continue;
        }
        {
            uint32_t found = 0;
            for (k = 0; k < c.S.n && !found; k++) {
                const mp_ins *s = &c.S.ins[k];
                if (MP_OP(s) == OpFunction) break;
                if ((mp_is_type(MP_OP(s)) || mp_is_const(MP_OP(s))) && mp_same(s->w, w, rid_at)) found = s->w[rid_at];
            }
            for (k = 0; k < c.globals.n && !found; k++)
                if (mp_same(c.globals.v[k].w, w, rid_at)) found = c.globals.v[k].w[rid_at];
            if (found) c.idmap[r] = found;
            else {
                w[rid_at] = c.S.bound++;
                c.idmap[r] = w[rid_at];
                if (!mp_emit(&c.S, &c.globals, op, w + 1, n - 1)) { free(w); MP_FAIL(&c, "out of memory"); }
            }
        }
        free(w);
    }
    for (k = 0; k < MP_NSAMPLERS; k++) if (!sampler_ids[k]) MP_FAIL(&c, "the multilayer module does not read all of its samplers");

    /* ---- the functions, every id renumbered */
    for (k = 0; k < nkeep; k++)
        for (i = keep[k]; i < c.M.n; i++) {
            uint32_t r = mp_rid(&c.M.ins[i]);
            if (r) c.idmap[r] = c.S.bound++;
            if (MP_OP(&c.M.ins[i]) == OpFunctionEnd) break;
        }
    for (k = 0; k < nkeep; k++)
        for (i = keep[k]; i < c.M.n; i++) {
            const mp_ins *in = &c.M.ins[i];
            uint32_t pos[256], *w;
            int np = mp_ids(in, pos, 256), q;
            if (np < 0) MP_FAIL(&c, "the multilayer module uses an instruction the splice does not know");
            if (!(w = mp_emit(&c.S, &funcs, MP_OP(in), in->w + 1, MP_N(in) - 1))) MP_FAIL(&c, "out of memory");
            for (q = 0; q < np; q++) {
                uint32_t v = c.idmap[w[pos[q]]];
                if (!v) MP_FAIL(&c, "the multilayer module refers to an id it does not define");
                w[pos[q]] = v;
            }
            if (MP_OP(in) == OpFunctionEnd) break;
        }

    /* ---- names and decorations of what was brought in */
    for (i = 0; i < c.M.n; i++) {
        const mp_ins *in = &c.M.ins[i];
        uint32_t t = in->w[1];
        int sampler = 0, s;
        if (MP_OP(in) != OpName || t >= c.M.bound || !c.idmap[t]) continue;
        for (s = 0; s < (int)MP_NSAMPLERS; s++) if (sampler_ids[s] == c.idmap[t]) sampler = 1;
        if (!(mark[t] & 2) && !sampler) continue;
        {
            uint32_t *w = mp_emit(&c.S, &c.names, OpName, in->w + 1, MP_N(in) - 1);
            if (!w) MP_FAIL(&c, "out of memory");
            w[1] = c.idmap[t];
        }
    }
    for (k = 0; k < MP_NSAMPLERS; k++) {
        uint32_t d1[3] = { sampler_ids[k], 34, 0 }, d2[3] = { sampler_ids[k], 33, tex_end + (uint32_t)k };
        if (!mp_emit(&c.S, &c.decos, OpDecorate, d1, 3) || !mp_emit(&c.S, &c.decos, OpDecorate, d2, 3)) MP_FAIL(&c, "out of memory");
    }

    /* ---- the stock module: bindings at or after tex_end move up */
    for (i = 0; i < c.S.n; i++) {
        mp_ins *in = &c.S.ins[i];
        if (MP_OP(in) == OpDecorate && MP_N(in) == 4 && in->w[2] == 33 && in->w[3] >= tex_end) {
            uint32_t *w = mp_own(&c.S, 4);
            if (!w) MP_FAIL(&c, "out of memory");
            memcpy(w, in->w, 4 * sizeof *w);
            w[3] += shift;
            in->w = w;
        }
    }

    /* ---- the material uniform block grows by the blend's parameters */
    fh = mp_named(&c.S, "freqHigh_fragmentUniforms", 0);
    fl = mp_named(&c.S, "freqLow_fragmentUniforms", 0);
    if (!fh || !fl) MP_FAIL(&c, "the program's fragment module has no single material uniform block");
    {
        const mp_ins *v = mp_def(&c.S, fh), *p;
        if (!v || MP_OP(v) != OpVariable || !(p = mp_def(&c.S, v->w[1]))) MP_FAIL(&c, "the material uniform block is not a variable");
        fh_struct = p->w[3];
        v = mp_def(&c.S, fl);
        if (!v || MP_OP(v) != OpVariable || !(p = mp_def(&c.S, v->w[1]))) MP_FAIL(&c, "the view uniform block is not a variable");
        fl_struct = p->w[3];
    }
    {
        mp_ins *st = NULL;
        uint32_t *w, offs_ok = 1, cnt = 0;
        for (i = 0; i < c.S.n; i++) if (mp_rid(&c.S.ins[i]) == fh_struct) st = &c.S.ins[i];
        if (!st || MP_OP(st) != OpTypeStruct || MP_N(st) < 3) MP_FAIL(&c, "the material uniform block is not a struct");
        v4 = st->w[2];
        n0 = MP_N(st) - 2;
        if (!(w = mp_own(&c.S, MP_N(st) + SH_MODPROG_ML_PARM_COUNT))) MP_FAIL(&c, "out of memory");
        memcpy(w, st->w, MP_N(st) * sizeof *w);
        for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++) w[MP_N(st) + k] = v4;
        w[0] = (MP_N(st) + SH_MODPROG_ML_PARM_COUNT) << 16 | OpTypeStruct;
        st->w = w;
        for (i = 0; i < c.S.n; i++) {
            const mp_ins *in = &c.S.ins[i];
            if (MP_OP(in) == OpMemberDecorate && in->w[1] == fh_struct && in->w[3] == 35) {
                if (in->w[4] != 16 * in->w[2]) offs_ok = 0;
                cnt++;
            }
        }
        if (!offs_ok || cnt != n0) MP_FAIL(&c, "the material uniform block is not 16-byte vectors");
        for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++) {
            uint32_t d[4] = { fh_struct, n0 + (uint32_t)k, 35, 16 * (n0 + (uint32_t)k) };
            size_t len = strlen(SH_MODPROG_ML_PARMS[k]) / 4 + 1;
            uint32_t nm[2 + 16];
            if (mp_member(&c.S, fh_struct, SH_MODPROG_ML_PARMS[k]) >= 0) MP_FAIL(&c, "the program already reads a multilayer parameter");
            if (!mp_emit(&c.S, &c.decos, OpMemberDecorate, d, 4)) MP_FAIL(&c, "out of memory");
            memset(nm, 0, sizeof nm);
            nm[0] = fh_struct;
            nm[1] = n0 + (uint32_t)k;
            memcpy(nm + 2, SH_MODPROG_ML_PARMS[k], strlen(SH_MODPROG_ML_PARMS[k]));
            if (!mp_emit(&c.S, &c.names, OpMemberName, nm, 2 + (uint32_t)len)) MP_FAIL(&c, "out of memory");
        }
    }

    /* ---- helpers' types */
    (void)mp_t_int(&c);
    { uint32_t o[2] = { 0, 32 }; t_float = mp_find_or_add(&c, OpTypeFloat, o, 2, 1); }
    { uint32_t o[1] = { 0 }; t_void = mp_find_or_add(&c, OpTypeVoid, o, 1, 1); }

    /* ---- main: the anchor and the inputs */
    {
        uint32_t mainid = mp_named(&c.S, "main", 0);
        for (i = 0; i < c.S.n; i++) if (MP_OP(&c.S.ins[i]) == OpFunction && c.S.ins[i].w[2] == mainid) break;
        if (!mainid || i == c.S.n) MP_FAIL(&c, "the program's fragment module has no main");
        s_main = i;
        while (i < c.S.n && MP_OP(&c.S.ins[i]) != OpFunctionEnd) i++;
        s_main_end = i;
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
    }
    {
        static const char *const fields[MP_NINOUT + 1] = { "albedo", "specular", "smoothness", "normal", "lightmap",
                                                           "emissive", "thickness", "texCoords" };
        int members[MP_NINOUT + 1], q;
        uint32_t t_int = mp_t_int(&c), normal_ix;
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
        /* the entry's parameters */
        ml_entry = c.idmap[entry_fn];
        for (k = 0; k < funcs.n; k++) if (MP_OP(&funcs.v[k]) == OpFunction && funcs.v[k].w[2] == ml_entry) break;
        {
            size_t p = k + 1, np = 0;
            while (p < funcs.n && MP_OP(&funcs.v[p]) == OpFunctionParameter && np < MP_NINOUT + 4) params[np++] = funcs.v[p++].w[1];
            if (np != MP_NINOUT + 4) MP_FAIL(&c, "ml_blend does not take the parameters the splice passes");
        }
        /* temps: one Function variable per parameter, at the head of main's first block */
        for (i = s_main; i < s_main_end && MP_OP(&c.S.ins[i]) != OpLabel; i++) {}
        var_end = i + 1;
        while (var_end < s_main_end && MP_OP(&c.S.ins[var_end]) == OpVariable) var_end++;
        for (q = 0; q < MP_NINOUT + 4; q++) {
            uint32_t o[3];
            temps[q] = c.S.bound++;
            o[0] = params[q];
            o[1] = temps[q];
            o[2] = 7;
            if (!mp_emit(&c.S, &heads, OpVariable, o, 3)) MP_FAIL(&c, "out of memory");
        }
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
            const mp_ins *fls = mp_def(&c.S, fl_struct);
            int tm = mp_member(&c.S, fl_struct, "time");
            uint32_t t, ch, v, x, o[4];
            if (tm < 0 || !fls) MP_FAIL(&c, "the view uniforms have no time");
            t = fls->w[2 + tm];
            ch = c.S.bound++;
            o[0] = mp_ptr(&c, 2, t); o[1] = ch; o[2] = fl; o[3] = mp_c_int(&c, (uint32_t)tm);
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
            /* the parameters and the emissive factors, as two arrays */
            uint32_t pv[64], a, o[66], arr;
            int which;
            for (which = 0; which < 2; which++) {
                unsigned cnt = which ? 4u : SH_MODPROG_ML_PARM_COUNT;
                const mp_ins *pp = mp_def_any(&c, params[MP_NINOUT + 2 + which]);
                if (!pp) MP_FAIL(&c, "ml_blend's parameter array has no type");
                arr = pp->w[3];
                for (k = 0; k < cnt; k++) {
                    int mem = which ? mp_member(&c.S, fh_struct, MP_EMISSIVE[k]) : (int)(n0 + k);
                    uint32_t ch, ov[4];
                    if (mem < 0) MP_FAIL(&c, "the program has no emissive factor the blend recomputes from");
                    ch = c.S.bound++;
                    ov[0] = mp_ptr(&c, 2, v4); ov[1] = ch; ov[2] = fh; ov[3] = mp_c_int(&c, (uint32_t)mem);
                    if (!mp_emit(&c.S, &code, OpAccessChain, ov, 4)) MP_FAIL(&c, "out of memory");
                    pv[k] = c.S.bound++;
                    ov[0] = v4; ov[1] = pv[k]; ov[2] = ch;
                    if (!mp_emit(&c.S, &code, OpLoad, ov, 3)) MP_FAIL(&c, "out of memory");
                }
                a = c.S.bound++;
                o[0] = arr; o[1] = a;
                memcpy(o + 2, pv, cnt * sizeof *pv);
                if (!mp_emit(&c.S, &code, OpCompositeConstruct, o, 2 + cnt)) MP_FAIL(&c, "out of memory");
                o[0] = temps[MP_NINOUT + 2 + which]; o[1] = a;
                if (!mp_emit(&c.S, &code, OpStore, o, 2)) MP_FAIL(&c, "out of memory");
            }
        }
        {
            uint32_t o[3 + MP_NINOUT + 4];
            o[0] = t_void; o[1] = 0; o[2] = ml_entry;
            memcpy(o + 3, temps, sizeof temps);
            o[1] = c.S.bound++;
            if (!mp_emit(&c.S, &code, OpFunctionCall, o, 3 + MP_NINOUT + 4)) MP_FAIL(&c, "out of memory");
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

    /* ---- reassemble: names after the stock debug section, decorations after the stock annotations, globals
     * before the first function, temps at the head of main, the call after the anchor, the functions last */
    {
        size_t first_fn = c.S.n, last_name = 0, last_deco = 0, total = 5;
        mp_buf o = { 0 };
        for (i = 0; i < c.S.n; i++) if (MP_OP(&c.S.ins[i]) == OpFunction) { first_fn = i; break; }
        for (i = 0; i < first_fn; i++) {
            uint32_t op = MP_OP(&c.S.ins[i]);
            if (op == OpName || op == OpMemberName || op == OpSource) last_name = i;
            if (op == OpDecorate || op == OpMemberDecorate) last_deco = i;
        }
        {
            uint32_t h[5] = { c.S.head[0], c.S.head[1], c.S.head[2], c.S.bound, c.S.head[4] };
            mp_put(&o, h, sizeof h);
        }
#define PUT_INS(x) mp_put(&o, (x)->w, MP_N(x) * sizeof(uint32_t))
#define PUT_LIST(l) do { size_t z_; for (z_ = 0; z_ < (l).n; z_++) PUT_INS(&(l).v[z_]); } while (0)
        for (i = 0; i < c.S.n; i++) {
            if (i == first_fn) { PUT_LIST(c.globals); PUT_LIST(c.extra); }
            if (i == var_end) PUT_LIST(heads);
            PUT_INS(&c.S.ins[i]);
            if (i == last_name) PUT_LIST(c.names);
            if (i == last_deco) PUT_LIST(c.decos);
            if (i == anchor) PUT_LIST(code);
        }
        if (first_fn == c.S.n) { PUT_LIST(c.globals); PUT_LIST(c.extra); }
        PUT_LIST(funcs);
#undef PUT_INS
#undef PUT_LIST
        (void)total;
        if (o.oom) { free(o.p); MP_FAIL(&c, "out of memory"); }
        out = (uint32_t *)o.p;
        if (out_words) *out_words = o.n / 4;
    }
done:
    if (why) *why = c.why;
    free(code.v); free(heads.v); free(funcs.v);
    free(c.globals.v); free(c.extra.v); free(c.names.v); free(c.decos.v);
    free(c.idmap); free(mark); free(fstart); free(keep); free(ids);
    mp_mod_free(&c.S);
    mp_mod_free(&c.M);
    if (c.why) { free(out); return NULL; }
    return out;
}

/* --------------------------------------------------------------------------------------- cooked program ---- */
/* package_shader's reader, cut at what the variant changes: a parameter block whose vector values are 16 bytes
 * and every other value a string, the stage parameter lists (3 stages x 7 groups: material uniforms, view
 * uniforms, textures, samplers, storage buffers, uniform-buffer arrays, storage images), includes, flags, the
 * permutation mask and list, 192 bytes of state and binding tables (6 words, then each stage's 7 binding starts
 * and 7 counts by category: material uniforms, view uniforms, uniform-buffer arrays, texel buffers, textures,
 * storage buffers, storage images), the three stage texts and a 36-byte trailer. */

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

static void mp_fail(const char *base, const char *why)
{
    free(g_cause);
    g_cause = sh_textf("Snapmap+ could not build its multilayer version of the game's %s program: %s (check the "
                       "game's files in Steam)", base, why);
}

/* One unit (the program, or one of its permutations) of a base: its four files. Caller holds the lock. */
static int mp_unit(const char *base, const char *mask)
{
    char *sd = NULL, *sb = NULL, *sv = NULL, *sf = NULL, *vd = NULL, *vb = NULL, *vv = NULL, *vf = NULL;
    char *var = sh_textf(SH_MODPROG_PREFIX "%s", base), *decl = NULL, *glsl = NULL, *bin_out = NULL;
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
        unsigned tex_end = a[4] + cn[4], shift = SH_MODPROG_ML_TEXTURE_COUNT, k;
        if (a[4] == 0xffffffffu || x.counts[1][2] != cn[4] || x.counts[1][3] != cn[4]) {
            why = "its fragment textures do not match its binding table";
            goto out;
        }
        for (k = 5; k < 7; k++) if (a[k] != 0xffffffffu) a[k] += shift;
        cn[4] += shift;
        x.block[5] += shift;
        for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++)
            if (!mp_bin_add(&x, 1, 0, SH_MODPROG_ML_PARMS[k], strlen(SH_MODPROG_ML_PARMS[k]))) { why = "out of memory"; goto out; }
        for (k = 0; k < SH_MODPROG_ML_TEXTURE_COUNT; k++)
            if (!mp_bin_add(&x, 1, 2, SH_MODPROG_ML_TEXTURES[k], strlen(SH_MODPROG_ML_TEXTURES[k])) ||
                !mp_bin_add(&x, 1, 3, "defaultsampler", 14)) { why = "out of memory"; goto out; }
        if (!(decl = sh_modprog_patch_decl((const char *)d, dn, &decl_n))) {
            why = "its decl has no single CUSTOM_TEXTURE_INPUTS template";
            goto out;
        }
        if (!(glsl = sh_modprog_patch_glsl(x.text[1], x.text_n[1], tex_end, shift, &glsl_n))) {
            why = "its fragment stage text is not the shape Snapmap+ knows";
            goto out;
        }
        if (!(mod = sh_modprog_splice((const uint32_t *)(f + 16), (fn - 16) / 4, tex_end, shift, &mod_n, &why))) goto out;
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
    if (!ok && why) mp_fail(base, why);
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
    if (g_done[base]) return g_done[base] > 0;
    sb = sh_textf("generated/renderprogs/%s_pc_vulkan.bin", MP_BASES[base]);
    memset(&x, 0, sizeof x);
    ok = sb && mp_read(sb, &b, &bn) == SH_MB_STOCK_OK && mp_bin_parse(&x, b, bn, mp_parm_is_vec);
    free(sb);
    if (!ok) mp_fail(MP_BASES[base], "its cooked program cannot be read");
    ok = ok && mp_unit(MP_BASES[base], NULL);
    for (i = 0; ok && i < x.nperms; i++) {
        char mask[17];
        snprintf(mask, sizeof mask, "%08x%08x", (unsigned)(x.perms[i] >> 32), (unsigned)x.perms[i]);
        ok = mp_unit(MP_BASES[base], mask);
    }
    mp_bin_free(&x);
    mp_heap_free(b);
    g_done[base] = ok ? 1 : -1;
    return ok;
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
static int mp_base_of(const char *path)
{
    static const char *const heads[] = {
        "generated/decls/renderprog/" SH_MODPROG_PREFIX, "generated/renderprogs/" SH_MODPROG_PREFIX,
        "generated/spirv/" SH_MODPROG_PREFIX, "generated/renderprogs/permutations/" SH_MODPROG_PREFIX,
        "generated/spirv/generated/renderprogs/permutations/" SH_MODPROG_PREFIX,
    };
    size_t h, b;
    for (h = 0; h < sizeof heads / sizeof heads[0]; h++) {
        size_t hn = strlen(heads[h]);
        if (_strnicmp(path, heads[h], hn)) continue;
        for (b = 0; b < MP_NBASES; b++) {
            size_t bn = strlen(MP_BASES[b]);
            char c;
            if (_strnicmp(path + hn, MP_BASES[b], bn)) continue;
            c = path[hn + bn];
            if (c == '.' || c == '_' || c == '/') return (int)b;
        }
    }
    return -1;
}

int sh_modprog_owns(const char *path)
{
    return path && (mp_parm_of(path) >= 0 || mp_base_of(path) >= 0);
}

static unsigned char *mp_heap_copy(const void *p, size_t n)
{
    unsigned char *h = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (h) memcpy(h, p, n);
    return h;
}

int sh_modprog_read(const char *path, unsigned char **bytes, size_t *len)
{
    int base, parm, r = SH_MB_STOCK_ABSENT;
    size_t i;
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (!path || !bytes || !len) return SH_MB_STOCK_ABSENT;
    if ((parm = mp_parm_of(path)) >= 0) {
        size_t t = strlen(MP_PARM_DECLS[parm].text);
        if (!(*bytes = mp_heap_copy(MP_PARM_DECLS[parm].text, t))) return SH_MB_STOCK_UNAVAILABLE;
        *len = t;
        return SH_MB_STOCK_OK;
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
    static const char *const heads[] = {
        "generated/decls/renderprog/" SH_MODPROG_PREFIX, "generated/renderprogs/" SH_MODPROG_PREFIX,
        "generated/spirv/" SH_MODPROG_PREFIX, "generated/renderprogs/permutations/" SH_MODPROG_PREFIX,
        "generated/spirv/generated/renderprogs/permutations/" SH_MODPROG_PREFIX,
    };
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
    /* Only a prefix inside (or above) a program namespace asks for the derivation. */
    for (i = 0; i < sizeof heads / sizeof heads[0]; i++) {
        size_t hn = strlen(heads[i]);
        if (!_strnicmp(heads[i], pre, n < hn ? n : hn)) overlaps = 1;
    }
    if (!overlaps) return;
    AcquireSRWLockExclusive(&g_lock);
    for (b = 0; b < MP_NBASES; b++) (void)mp_derive(b);
    if (g_nfiles && (paths = (char **)calloc(g_nfiles, sizeof *paths)) != NULL)
        for (i = 0; i < g_nfiles; i++)
            if (!_strnicmp(g_files[i].path, pre, n)) paths[np++] = mp_dupn(g_files[i].path, strlen(g_files[i].path));
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
    free(g_cause);
    g_cause = NULL;
    ReleaseSRWLockExclusive(&g_lock);
}
