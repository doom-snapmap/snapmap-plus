/* modenv.c -- the Graphics SDK's environments (modenv.h). Pure apart from the decls it registers (sdk_register). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modenv.h"
#include "modprog.h"
#include "sdk.h"
#include "../common/text.h"

/* The fields and the env keys they set (the env decl's spelling; the same names, lower case, are the render parms
 * an FX_ENV_CHANGE item names). `bloom` scales the six bloom weights from the game's default env (0.2, 1, 1, 1, 1,
 * 1: env/default). `exposure` is the darkest and the brightest the eye adapts to. */
typedef struct env_field { sdk_word_id word; const char *parm; int dims; } env_field;
static const env_field FIELDS[] = {
    { SDK_E_FOG, "fogColor", 3 },           { SDK_E_FOGDENSITY, "fogDensity", 1 },
    { SDK_E_FOGSTART, "fogStart", 1 },      { SDK_E_FOGEND, "fogEnd", 1 },
    { SDK_E_BLOOM, NULL, 1 },               { SDK_E_EXPOSURE, NULL, 2 },
    { SDK_E_SATURATION, "colorCorrectionMasterSaturation", 1 },
    { SDK_E_TINT, "colorCorrectionMasterColor", 3 },
    { SDK_F_BRIGHTNESS, "colorCorrectionMasterGain", 1 },
    { SDK_E_VIGNETTE, "vignetteScale", 1 }, { SDK_E_VIGNETTECOLOUR, "vignetteColor", 3 },
    { SDK_E_GRAIN, "filmGrain", 1 },        { SDK_E_FRINGE, "chromaticAberrationScale", 1 },
};
#define NFIELDS (sizeof FIELDS / sizeof FIELDS[0])
static const char *const BLOOM[6] = { "bloomWeight0", "bloomWeight1", "bloomWeight2", "bloomWeight3", "bloomWeight4",
                                      "bloomWeight5" };
static const double BLOOM_DEFAULT[6] = { 0.2, 1, 1, 1, 1, 1 };

static void scale_value(sdk_value *v, double k)
{
    int c, i;
    for (c = 0; c < 4; c++) {
        v->c[c] *= k; v->low[c] *= k; v->high[c] *= k; v->from[c] *= k; v->to[c] *= k;
        for (i = 0; i < v->points; i++) v->v[i][c] *= k;
    }
}

static int skipped(const sdk_atom *a, const char *const *skip)
{
    for (; skip && *skip; skip++) if (sdk_is(a, *skip)) return 1;
    return 0;
}

int sdk_env_parms(const sdk_doc *d, const sdk_group *g, const char *what, const char *const *skip, sdk_env_parm *out,
                  char *why, size_t why_len)
{
    const char *names[NFIELDS + 8];
    size_t i, k, nn = 0;
    int n = 0;
    for (k = 0; k < NFIELDS; k++) names[nn++] = sdk_word(FIELDS[k].word);
    for (k = 0; skip && skip[k] && nn < sizeof names / sizeof names[0]; k++) names[nn++] = skip[k];
    for (i = 0; i < g->count; i++) {
        const sdk_item *it = &g->items[i];
        const env_field *f = NULL;
        char w[64];
        if (!it->count) continue;
        if (it->atoms[0].kind != SDK_A_WORD) {
            sdk_say(why, why_len, d, it->line, "each line of %s starts with a field name.", what);
            return -1;
        }
        if (skipped(&it->atoms[0], skip)) continue;
        for (k = 0; k < NFIELDS; k++) if (sdk_is(&it->atoms[0], sdk_word(FIELDS[k].word))) f = &FIELDS[k];
        sdk_text(&it->atoms[0], w, sizeof w);
        if (!f) {
            const char *near = sdk_nearest(it->atoms[0].p, it->atoms[0].n, names, nn);
            if (near) sdk_say(why, why_len, d, it->line, "'%s' is not a field of %s; the nearest is '%s'.", w, what, near);
            else sdk_say(why, why_len, d, it->line, "'%s' is not a field of %s.", w, what);
            return -1;
        }
        {
            sdk_value v;
            if (!sdk_value_read(d, it, 1, f->dims, f->dims, w, &v, why, why_len)) return -1;
            if (f->word == SDK_E_BLOOM) {
                for (k = 0; k < 6; k++) {
                    if (n == SDK_ENV_MAX) break;
                    out[n].parm = BLOOM[k];
                    out[n].v = v;
                    out[n].line = it->line;
                    scale_value(&out[n].v, BLOOM_DEFAULT[k]);
                    n++;
                }
            } else if (f->word == SDK_E_EXPOSURE) {
                if (v.form != SDK_VAL_CONST) {
                    sdk_say(why, why_len, d, it->line, "'%s' takes two numbers, the darkest and the brightest, as in "
                            "'%s { 2, 10 }'.", w, w);
                    return -1;
                }
                for (k = 0; k < 2 && n < SDK_ENV_MAX; k++) {
                    memset(&out[n], 0, sizeof out[n]);
                    out[n].parm = k ? "autoExposureMax" : "autoExposureMin";
                    out[n].v.form = SDK_VAL_CONST;
                    out[n].v.dims = 1;
                    out[n].v.c[0] = v.c[k];
                    out[n].line = it->line;
                    n++;
                }
            } else if (n < SDK_ENV_MAX) {
                out[n].parm = f->parm;
                out[n].v = v;
                out[n].line = it->line;
                n++;
            }
        }
    }
    return n;
}

/* ------------------------------------------------------------------------------------------- the fx ---- */

static void put_num(sdk_buf *b, const char *ind, const char *key, double v)
{
    char n[32];
    sdk_fmt(n, sizeof n, v);
    sdk_printf(b, "%s%s = %s;\n", ind, key, n);
}

/* The value an fx item writes for a parm: the constant (sRGB-decoded for a colour parm, as the env decl's own parse
 * decodes it), or the middle of a value that changes (an fx value is a constant; noted by the caller). */
static void fx_value(const sdk_env_parm *p, double out[4])
{
    const sdk_value *v = &p->v;
    int c, srgb = sdk_parm_is_srgb(p->parm, strlen(p->parm));
    for (c = 0; c < 4; c++) {
        double x;
        int cc = c < v->dims ? c : (v->dims == 1 ? 0 : -1);
        if (cc < 0) { out[c] = 1; continue; }
        switch (v->form) {
        case SDK_VAL_PULSE: case SDK_VAL_FLICKER: x = (v->low[cc] + v->high[cc]) / 2; break;
        case SDK_VAL_FADE: x = v->to[cc]; break;
        case SDK_VAL_CURVE: x = v->v[0][cc]; break;
        default: x = v->c[cc]; break;
        }
        out[c] = srgb && c < 3 ? sdk_srgb_to_linear(x) : x;
    }
}

char *sdk_env_fx_text(const sdk_env_parm *p, int n, double blend)
{
    sdk_buf b = { 0 };
    int items = (n + 15) / 16, i, k;
    sdk_printf(&b, "{\n\tedit = {\n\t\teditEvents = {\n\t\t\tnum = %d;\n", items);
    for (i = 0; i < items; i++) {
        int first = i * 16, count = n - first < 16 ? n - first : 16;
        sdk_printf(&b, "\t\t\titem[%d] = {\n", i);
        sdk_printf(&b, "\t\t\t\tname = \"environment%d\";\n\t\t\t\ttype = \"FX_ENV_CHANGE\";\n\t\t\t\tlooping = true;\n", i);
        put_num(&b, "\t\t\t\t", "fadeInTime", blend);
        put_num(&b, "\t\t\t\t", "fadeOutTime", blend);
        sdk_printf(&b, "\t\t\t\tstartCondition = {\n\t\t\t\t\tnum = 1;\n\t\t\t\t\titem[0] = \"FX_NONE\";\n\t\t\t\t}\n");
        sdk_printf(&b, "\t\t\t\tstopCondition = {\n\t\t\t\t\tnum = 1;\n\t\t\t\t\titem[0] = \"FX_NONE\";\n\t\t\t\t}\n");
        sdk_printf(&b, "\t\t\t\tenvParms = {\n\t\t\t\t\tenvRenderParm = {\n\t\t\t\t\t\tnum = %d;\n", count);
        for (k = 0; k < count; k++) {
            double v[4];
            char lower[64];
            size_t j;
            for (j = 0; p[first + k].parm[j] && j < sizeof lower - 1; j++) lower[j] = (char)tolower((unsigned char)p[first + k].parm[j]);
            lower[j] = 0;
            fx_value(&p[first + k], v);
            sdk_printf(&b, "\t\t\t\t\t\titem[%d] = {\n\t\t\t\t\t\t\tparm = \"%s\";\n\t\t\t\t\t\t\tvalue = {\n", k, lower);
            put_num(&b, "\t\t\t\t\t\t\t\t", "x", v[0]);
            put_num(&b, "\t\t\t\t\t\t\t\t", "y", v[1]);
            put_num(&b, "\t\t\t\t\t\t\t\t", "z", v[2]);
            put_num(&b, "\t\t\t\t\t\t\t\t", "w", v[3]);
            sdk_printf(&b, "\t\t\t\t\t\t\t}\n\t\t\t\t\t\t}\n");
        }
        sdk_printf(&b, "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n");
    }
    sdk_printf(&b, "\t\t}\n\t}\n}\n");
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

/* --------------------------------------------------------------------------------------- the decl ---- */

/* 1 when the doc's first item is `type environment`. */
static int is_environment(const sdk_doc *d)
{
    const sdk_item *it = d->root->count ? &d->root->items[0] : NULL;
    return it && it->count == 2 && sdk_is(&it->atoms[0], SDK_W(TYPE)) && sdk_is(&it->atoms[1], SDK_W(KIND_ENVIRONMENT));
}

static int starts_with_type(const char *text, size_t len)
{
    char kind[32];
    return sdk_first_type(text, len, kind, sizeof kind) && !strcmp(kind, SDK_W(KIND_ENVIRONMENT));
}

static int compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                   size_t why_len, char **needs, char **moment, char **notes)
{
    sdk_doc d;
    sdk_env_parm p[SDK_ENV_MAX];
    const char *skip[3];
    sdk_buf b = { 0 };
    double blend = 1.2;
    int n, r, i, temp = 0, animated = 0;
    const sdk_item *bl;
    *out = NULL;
    if (out_len) *out_len = 0;
    if (!starts_with_type(text, len)) return SH_LOOK_SAME;
    r = sdk_parse(path, text, len, &d, why, why_len);
    if (r < 0) return SH_LOOK_OOM;
    if (r == 0) return SH_LOOK_MISTAKE;
    if (!is_environment(&d)) { sdk_doc_free(&d); return SH_LOOK_SAME; }
    skip[0] = SDK_W(TYPE);
    skip[1] = SDK_W(E_BLEND);
    skip[2] = NULL;
    if ((bl = sdk_find(d.root, SDK_W(E_BLEND))) != NULL) {
        double x[1];
        if (bl->count != 2 || sdk_numbers(&bl->atoms[1], x, 1) != 1 || x[0] < 0) {
            sdk_say(why, why_len, &d, bl->line, "'%s' takes the seconds to blend in and out, as in '%s 1.5'.",
                    SDK_W(E_BLEND), SDK_W(E_BLEND));
            sdk_doc_free(&d);
            return SH_LOOK_MISTAKE;
        }
        blend = x[0];
    }
    n = sdk_env_parms(&d, d.root, "an environment", skip, p, why, why_len);
    if (n < 0) { sdk_doc_free(&d); return SH_LOOK_MISTAKE; }
    sdk_printf(&b, "{\n\tinherit {\n\t\tdefault\n\t}\n\trenderParms {\n");
    {
        char t[32];
        sdk_fmt(t, sizeof t, blend);
        sdk_printf(&b, "\t\tenvBlendTime\t%s\n", t);
    }
    for (i = 0; i < n; i++) {
        if (p[i].v.form != SDK_VAL_CONST) animated = 1;
        r = sdk_value_material(&d, p[i].line, p[i].parm, &p[i].v, &temp, &b, "\n", why, why_len);
        if (r < 0) { sdk_doc_free(&d); free(b.p); return SH_LOOK_OOM; }
        if (!r) { sdk_doc_free(&d); free(b.p); return SH_LOOK_MISTAKE; }
    }
    sdk_printf(&b, "\t}\n}\n");
    if (b.oom) { sdk_doc_free(&d); free(b.p); return SH_LOOK_OOM; }
    /* the fx set_environment plays, and its emitter */
    {
        char *fx = sdk_env_fx_text(p, n, blend);
        const char *name = fx ? sdk_register("fx", fx) : NULL;
        free(fx);
        if (!name) { sdk_doc_free(&d); free(b.p); return SH_LOOK_OOM; }
        if (moment) *moment = _strdup(name);
        if (needs) {
            char *em = sh_modprog_effect_emitter(name);
            *needs = em ? sh_textf("fx %s\nentityDef %s\n", name, em) : NULL;
            free(em);
        }
    }
    if (animated && notes)
        *notes = sh_textf("%s: set_environment holds each value that changes over time at its middle (an area of the map draws it changing)\n", d.file);
    sdk_doc_free(&d);
    *out = b.p;
    if (out_len) *out_len = b.n;
    return SH_LOOK_CHANGED;
}

int sh_env_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                   size_t why_len, char **needs, char **notes)
{
    if (needs) *needs = NULL;
    if (notes) *notes = NULL;
    return compile(path, text, len, out, out_len, why, why_len, needs, NULL, notes);
}

char *sh_env_moment_fx(const char *path, const char *text, size_t len)
{
    char *out = NULL, *moment = NULL, why[300];
    int r = compile(path, text, len, &out, NULL, why, sizeof why, NULL, &moment, NULL);
    free(out);
    if (r != SH_LOOK_CHANGED) { free(moment); return NULL; }
    return moment;
}
