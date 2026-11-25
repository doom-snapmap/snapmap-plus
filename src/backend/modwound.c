/* modwound.c -- the Graphics SDK's wound sites (modwound.h). Pure text work. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modwound.h"
#include "modlook.h"
#include "sdkparse.h"

typedef struct wctx { sdk_doc *d; char *why; size_t why_len; } wctx;

#define WFAIL(c, line, ...) do { sdk_say((c)->why, (c)->why_len, (c)->d, (line), __VA_ARGS__); return 0; } while (0)

static void put(sdk_buf *b, int ind, const char *fmt, const char *s)
{
    sdk_printf(b, "%.*s", ind, "\t\t\t\t\t\t\t\t\t\t");
    sdk_printf(b, fmt, s);
}

static void put_num(sdk_buf *b, int ind, const char *key, double v)
{
    char n[32];
    sdk_fmt(n, sizeof n, v);
    sdk_printf(b, "%.*s%s = %s;\n", ind, "\t\t\t\t\t\t\t\t\t\t", key, n);
}

/* A name or { name, name, ... } into names (at most 16). */
static int names(wctx *c, const sdk_item *it, char out[16][96], int *n)
{
    size_t q;
    *n = 0;
    if (it->count == 2 && (it->atoms[1].kind == SDK_A_WORD || it->atoms[1].kind == SDK_A_STRING)) {
        sdk_text(&it->atoms[1], out[0], 96);
        *n = 1;
        return 1;
    }
    if (it->count == 2 && it->atoms[1].kind == SDK_A_GROUP && it->atoms[1].group->count >= 1 &&
        it->atoms[1].group->count <= 16) {
        for (q = 0; q < it->atoms[1].group->count; q++) {
            const sdk_item *x = &it->atoms[1].group->items[q];
            if (x->count != 1 || (x->atoms[0].kind != SDK_A_WORD && x->atoms[0].kind != SDK_A_STRING))
                WFAIL(c, it->line, "'%s' takes a name, or up to sixteen in braces.", it->atoms[0].p ? SDK_W(W_PARTS) : "");
            sdk_text(&x->atoms[0], out[q], 96);
        }
        *n = (int)it->atoms[1].group->count;
        return 1;
    }
    {
        char w[64];
        sdk_text(&it->atoms[0], w, sizeof w);
        WFAIL(c, it->line, "'%s' takes a name, or up to sixteen in braces.", w);
    }
}

static void name_list(sdk_buf *b, int ind, const char *key, char v[16][96], int n)
{
    int k;
    sdk_printf(b, "%.*s%s = {\n", ind, "\t\t\t\t\t\t\t\t\t\t", key);
    put_num(b, ind + 1, "num", n);
    for (k = 0; k < n; k++) {
        char item[16];
        snprintf(item, sizeof item, "item[%d]", k);
        sdk_printf(b, "%.*s%s = \"%s\";\n", ind + 1, "\t\t\t\t\t\t\t\t\t\t", item, v[k]);
    }
    sdk_printf(b, "%.*s}\n", ind, "\t\t\t\t\t\t\t\t\t\t");
}

int sh_wound_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                     size_t why_len)
{
    static const sdk_word_id FIELDS[] = { SDK_W_KIND, SDK_W_PARTS, SDK_W_OPENS, SDK_W_INONEHIT, SDK_W_SHOWS, SDK_W_CUTS,
                                          SDK_W_BLOOD, SDK_W_THROWS, SDK_F_AT, SDK_W_WHILEALIVE, SDK_W_RAGDOLL };
    sdk_doc d;
    wctx c;
    sdk_buf b = { 0 };
    char kind[32], parts[16][96], kits[16][96], at[96] = "", throws[160] = "";
    int nparts = 0, nkits = 0, alive = 0, ragdoll = 0, blood = 1, r;   /* blood: 0 none, 1 spray, 2 pool, 3 both */
    double opens = -1, onehit = -1, cuts[16][4];
    int ncuts = 0;
    const char *woundtype = NULL;
    size_t i;
    *out = NULL;
    if (out_len) *out_len = 0;
    if (why && why_len) why[0] = 0;
    if (!sdk_first_type(text, len, kind, sizeof kind) || strcmp(kind, SDK_W(KIND_WOUND))) return SH_LOOK_SAME;
    r = sdk_parse(path, text, len, &d, why, why_len);
    if (r < 0) return SH_LOOK_OOM;
    if (r == 0) return SH_LOOK_MISTAKE;
    memset(&c, 0, sizeof c);
    c.d = &d;
    c.why = why;
    c.why_len = why_len;
    for (i = 1; i < d.root->count; i++) {
        const sdk_item *it = &d.root->items[i];
        char w[64];
        size_t k;
        int known = 0;
        double x[4];
        if (!it->count) continue;
        sdk_text(&it->atoms[0], w, sizeof w);
        for (k = 0; k < sizeof FIELDS / sizeof FIELDS[0]; k++) if (sdk_is(&it->atoms[0], sdk_word(FIELDS[k]))) known = 1;
        if (!known) {
            const char *cand[16], *near;
            for (k = 0; k < sizeof FIELDS / sizeof FIELDS[0]; k++) cand[k] = sdk_word(FIELDS[k]);
            near = it->atoms[0].kind == SDK_A_WORD ? sdk_nearest(it->atoms[0].p, it->atoms[0].n, cand, k) : NULL;
            if (near) sdk_say(why, why_len, &d, it->line, "'%s' is not a field of a wound; the nearest is '%s'.", w, near);
            else sdk_say(why, why_len, &d, it->line, "'%s' is not a field of a wound.", w);
            goto mistake;
        }
        if (sdk_is(&it->atoms[0], SDK_W(W_KIND))) {
            if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_SMALL))) woundtype = "GORETYPE_SMALL_WOUND";
            else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_BIG))) woundtype = "GORETYPE_BIG_WOUND";
            else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_HEADSHOT))) woundtype = "GORETYPE_HEAD_SHOT";
            else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_LIMB))) woundtype = "GORETYPE_LIMBLOSS";
            else { sdk_say(why, why_len, &d, it->line, "'%s' is %s, %s, %s or %s.", w, SDK_W(V_SMALL), SDK_W(V_BIG), SDK_W(V_HEADSHOT), SDK_W(V_LIMB)); goto mistake; }
        } else if (sdk_is(&it->atoms[0], SDK_W(W_PARTS))) {
            if (!names(&c, it, parts, &nparts)) goto mistake;
        } else if (sdk_is(&it->atoms[0], SDK_W(W_SHOWS))) {
            if (!names(&c, it, kits, &nkits)) goto mistake;
        } else if (sdk_is(&it->atoms[0], SDK_W(W_OPENS)) || sdk_is(&it->atoms[0], SDK_W(W_INONEHIT))) {
            if (it->count != 2 || it->atoms[1].kind != SDK_A_NUMBER || it->atoms[1].num < 0) {
                sdk_say(why, why_len, &d, it->line, "'%s' needs the damage as one number, as in '%s 200'.", w, w);
                goto mistake;
            }
            if (sdk_is(&it->atoms[0], SDK_W(W_OPENS))) opens = it->atoms[1].num; else onehit = it->atoms[1].num;
        } else if (sdk_is(&it->atoms[0], SDK_W(W_CUTS))) {
            size_t q;
            if (it->count != 2 || it->atoms[1].kind != SDK_A_GROUP || !it->atoms[1].group->count || it->atoms[1].group->count > 16) {
                sdk_say(why, why_len, &d, it->line, "'%s' takes spheres in braces, each { x, y, z, radius }.", w);
                goto mistake;
            }
            for (q = 0; q < it->atoms[1].group->count; q++) {
                const sdk_item *s = &it->atoms[1].group->items[q];
                if (s->count != 1 || sdk_numbers(&s->atoms[0], x, 4) != 4) {
                    sdk_say(why, why_len, &d, it->line, "'%s' takes spheres in braces, each { x, y, z, radius }.", w);
                    goto mistake;
                }
                memcpy(cuts[ncuts++], x, sizeof x);
            }
        } else if (sdk_is(&it->atoms[0], SDK_W(W_BLOOD))) {
            if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_SPRAY))) blood = 1;
            else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_POOL))) blood = 2;
            else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_BOTH))) blood = 3;
            else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_NONE))) blood = 0;
            else { sdk_say(why, why_len, &d, it->line, "'%s' is %s, %s, %s or %s.", w, SDK_W(V_SPRAY), SDK_W(V_POOL), SDK_W(V_BOTH), SDK_W(V_NONE)); goto mistake; }
        } else if (sdk_is(&it->atoms[0], SDK_W(W_THROWS)) || sdk_is(&it->atoms[0], SDK_W(F_AT))) {
            if (it->count != 2 || (it->atoms[1].kind != SDK_A_WORD && it->atoms[1].kind != SDK_A_STRING)) {
                sdk_say(why, why_len, &d, it->line, "'%s' takes one name.", w);
                goto mistake;
            }
            if (sdk_is(&it->atoms[0], SDK_W(F_AT))) sdk_text(&it->atoms[1], at, sizeof at);
            else sdk_text(&it->atoms[1], throws, sizeof throws);
        } else if (sdk_is(&it->atoms[0], SDK_W(W_WHILEALIVE)) || sdk_is(&it->atoms[0], SDK_W(W_RAGDOLL))) {
            int v;
            if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_YES))) v = 1;
            else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_NO))) v = 0;
            else { sdk_say(why, why_len, &d, it->line, "'%s' is %s or %s.", w, SDK_W(V_YES), SDK_W(V_NO)); goto mistake; }
            if (sdk_is(&it->atoms[0], SDK_W(W_WHILEALIVE))) alive = v; else ragdoll = v;
        }
    }
    if (!nparts) { sdk_say(why, why_len, &d, d.root->line, "a wound needs the '%s' that open it, as in '%s chest'.", SDK_W(W_PARTS), SDK_W(W_PARTS)); goto mistake; }
    if (blood && !at[0]) { sdk_say(why, why_len, &d, d.root->line, "a wound that bleeds needs the joint its blood comes from, as in '%s gorefx_chest' (or 'blood none').", SDK_W(F_AT)); goto mistake; }
    /* 2016's own default wound underneath (wounds/default: its gibs and thresholds) */
    sdk_printf(&b, "{\n\tinherit = \"wounds/default\";\n\tedit = {\n");
    if (woundtype) put(&b, 2, "woundType = \"%s\";\n", woundtype);
    if (alive) put(&b, 2, "woundFlags = \"%s\";\n", "WOUND_FLAGS_ALLOW_WHEN_ALIVE");
    if (ragdoll) put(&b, 2, "instantRagdoll = %s;\n", "true");
    sdk_printf(&b, "\t\tvisuals = {\n");
    if (nkits) name_list(&b, 3, "meshKits", kits, nkits);
    if (ncuts) {
        int k;
        sdk_printf(&b, "\t\t\tclippingSpheres = {\n");
        put_num(&b, 4, "num", ncuts);
        for (k = 0; k < ncuts; k++) {
            sdk_printf(&b, "\t\t\t\titem[%d] = {\n", k);
            put_num(&b, 5, "x", cuts[k][0]);
            put_num(&b, 5, "y", cuts[k][1]);
            put_num(&b, 5, "z", cuts[k][2]);
            put_num(&b, 5, "w", cuts[k][3]);
            sdk_printf(&b, "\t\t\t\t}\n");
        }
        sdk_printf(&b, "\t\t\t}\n");
    }
    if (blood & 1) {
        /* 2016's spray: its most used stock values (the most used gorewounds values) */
        sdk_printf(&b, "\t\t\tgoreSpray = {\n");
        put(&b, 4, "sprayFX = \"%s\";\n", "FX_GORE_SPRAY_1");
        put(&b, 4, "tagJoint = \"%s\";\n", at);
        put_num(&b, 4, "coneAngle", 35);
        put_num(&b, 4, "dist", 100);
        put_num(&b, 4, "secondsBetweenSprays", 0.5);
        put_num(&b, 4, "timeVarianceInSeconds", 0.2);
        put_num(&b, 4, "sizeVariance", 1);
        sdk_printf(&b, "\t\t\t\tdecalParms = {\n\t\t\t\t\tnum = 1;\n\t\t\t\t\titem[0] = {\n");
        put(&b, 6, "decalMat = \"%s\";\n", "textures/decals/blood_splatter/blood_splatter_small_01");
        put_num(&b, 6, "fadeInEndTime", 500);
        put(&b, 6, "isPersistent = %s;\n", "false");
        sdk_printf(&b, "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n");
    }
    if (blood & 2) {
        sdk_printf(&b, "\t\t\tbloodPool = {\n");
        put(&b, 4, "tagJoint = \"%s\";\n", at);
        put_num(&b, 4, "sizeVariance", 0.2);
        put_num(&b, 4, "dist", 200);
        sdk_printf(&b, "\t\t\t\tdecalParms = {\n\t\t\t\t\tnum = 1;\n\t\t\t\t\titem[0] = {\n");
        put(&b, 6, "decalMat = \"%s\";\n", "textures/decals/blood_splatter/blood_pool_01");
        sdk_printf(&b, "\t\t\t\t\t\tsize = {\n\t\t\t\t\t\t\tx = 120;\n\t\t\t\t\t\t\ty = 120;\n\t\t\t\t\t\t}\n");
        put_num(&b, 6, "lifetime", 3000);
        put_num(&b, 6, "fadetime", 2000);
        put_num(&b, 6, "fadeInEndTime", 2000);
        put(&b, 6, "isPersistent = %s;\n", "true");
        sdk_printf(&b, "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n");
    }
    sdk_printf(&b, "\t\t}\n");
    if (throws[0]) {
        sdk_printf(&b, "\t\tspawning = {\n\t\t\tspawnBreakables = {\n\t\t\t\tnum = 1;\n\t\t\t\titem[0] = {\n");
        if (at[0]) put(&b, 5, "fxTagJoint = \"%s\";\n", at);
        put(&b, 5, "entityDef = \"%s\";\n", throws);
        sdk_printf(&b, "\t\t\t\t}\n\t\t\t}\n\t\t}\n");
    }
    sdk_printf(&b, "\t\tdamageRequirements = {\n");
    name_list(&b, 3, "damageGroups", parts, nparts);
    if (opens >= 0) put_num(&b, 3, "totalDamageRequiredToGore", opens);
    if (onehit >= 0) put_num(&b, 3, "frameDamageRequiredToGore", onehit);
    sdk_printf(&b, "\t\t}\n\t}\n}\n");
    sdk_doc_free(&d);
    if (b.oom) { free(b.p); return SH_LOOK_OOM; }
    *out = b.p;
    if (out_len) *out_len = b.n;
    return SH_LOOK_CHANGED;
mistake:
    sdk_doc_free(&d);
    free(b.p);
    return SH_LOOK_MISTAKE;
}
