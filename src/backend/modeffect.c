/* modeffect.c -- the Graphics SDK's effects and particles (modeffect.h). */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modeffect.h"
#include "modenv.h"
#include "modlook.h"
#include "modprog.h"
#include "sdk.h"
#include "sdkparse.h"
#include "sdkvalues.h"
#include "../common/text.h"

typedef struct ectx {
    sdk_doc *d;
    char *why;
    size_t why_len;
    sdk_buf notes;
    int failed;   /* a sentence is in why */
    int oom;
} ectx;

static void note(ectx *c, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    sdk_printf(&c->notes, "%s: %s\n", c->d->file, line);
}

#define FAIL(c, line, ...) do { sdk_say((c)->why, (c)->why_len, (c)->d, (line), __VA_ARGS__); (c)->failed = 1; return 0; } while (0)

static const sdk_item *fld(const sdk_group *g, sdk_word_id w) { return sdk_find(g, sdk_word(w)); }

/* Every line of `g` names a field of `ok`; else one sentence with the nearest. */
static int check_fields(ectx *c, const sdk_group *g, const char *what, const sdk_word_id *ok, int nok)
{
    const char *names[64];
    size_t i;
    int k;
    for (k = 0; k < nok && k < 64; k++) names[k] = sdk_word(ok[k]);
    for (i = 0; i < g->count; i++) {
        const sdk_item *it = &g->items[i];
        char w[64];
        int found = 0;
        if (!it->count) continue;
        if (it->atoms[0].kind != SDK_A_WORD) FAIL(c, it->line, "each line of %s starts with a field name.", what);
        for (k = 0; k < nok; k++) if (sdk_is(&it->atoms[0], names[k])) found = 1;
        if (found) continue;

        sdk_text(&it->atoms[0], w, sizeof w);
        {
            const char *near = sdk_nearest(it->atoms[0].p, it->atoms[0].n, names, (size_t)nok);
            if (near) FAIL(c, it->line, "'%s' is not a field of %s; the nearest is '%s'.", w, what, near);
            FAIL(c, it->line, "'%s' is not a field of %s.", w, what);
        }
    }
    return 1;
}

static int get_num(ectx *c, const sdk_group *g, sdk_word_id w, double *out, int *have)
{
    const sdk_item *it = fld(g, w);
    double x[1];
    *have = 0;
    if (!it) return 1;
    if (it->count != 2 || sdk_numbers(&it->atoms[1], x, 1) != 1 || it->atoms[1].kind != SDK_A_NUMBER)
        FAIL(c, it->line, "'%s' needs one number, as in '%s 2'.", sdk_word(w), sdk_word(w));
    *out = x[0];
    *have = 1;
    return 1;
}

/* A number, or { low, high }. */
static int get_range(ectx *c, const sdk_group *g, sdk_word_id w, double *lo, double *hi, int *have)
{
    const sdk_item *it = fld(g, w);
    double x[2];
    int n;
    *have = 0;
    if (!it) return 1;
    n = it->count == 2 ? sdk_numbers(&it->atoms[1], x, 2) : 0;
    if (n == 1) { *lo = *hi = x[0]; *have = 1; return 1; }
    if (n == 2) { *lo = x[0] < x[1] ? x[0] : x[1]; *hi = x[0] < x[1] ? x[1] : x[0]; *have = 1; return 1; }
    FAIL(c, it->line, "'%s' needs a number or two (the least and the most), as in '%s { 1, 2 }'.", sdk_word(w), sdk_word(w));
}

/* Exactly n numbers (one is spread to all when `spread`). */
static int get_vec(ectx *c, const sdk_group *g, sdk_word_id w, int n, int spread, double *out, int *have)
{
    const sdk_item *it = fld(g, w);
    double x[4];
    int k, m;
    *have = 0;
    if (!it) return 1;
    m = it->count == 2 ? sdk_numbers(&it->atoms[1], x, 4) : 0;
    if (m == n || (spread && m == 1)) {
        for (k = 0; k < n; k++) out[k] = m == 1 ? x[0] : x[k];
        *have = 1;
        return 1;
    }
    if (n == 3) FAIL(c, it->line, "'%s' needs three numbers, as in '%s { 1, 0.5, 0.2 }'.", sdk_word(w), sdk_word(w));
    if (n == 2) FAIL(c, it->line, "'%s' needs two numbers, as in '%s { 1, 2 }'.", sdk_word(w), sdk_word(w));
    FAIL(c, it->line, "'%s' needs %d numbers.", sdk_word(w), n);
}

/* yes or no. */
static int get_flag(ectx *c, const sdk_group *g, sdk_word_id w, int *out, int *have)
{
    const sdk_item *it = fld(g, w);
    *have = 0;
    if (!it) return 1;
    if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_YES))) { *out = 1; *have = 1; return 1; }
    if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_NO))) { *out = 0; *have = 1; return 1; }
    FAIL(c, it->line, "'%s' is %s or %s.", sdk_word(w), SDK_W(V_YES), SDK_W(V_NO));
}

/* A name: a word or a quoted name (the mod's own, as written) or native("...") (DOOM's). */
static int get_name(ectx *c, const sdk_group *g, sdk_word_id w, char *out, size_t cap, int *have)
{
    const sdk_item *it = fld(g, w);
    *have = 0;
    if (!it) return 1;
    if (it->count != 2 || (it->atoms[1].kind != SDK_A_WORD && it->atoms[1].kind != SDK_A_STRING))
        FAIL(c, it->line, "'%s' needs one name: the mod's, beginning with its folder, or the game's, as in '%s %s:name'.",
             sdk_word(w), sdk_word(w), SDK_W(C_GAME));
    sdk_text(&it->atoms[1], out, cap);
    *have = 1;
    return 1;
}

/* An item's colour: { r, g, b } or { r, g, b, opacity } (opacity 1 when left out). */
static int get_colour(ectx *c, const sdk_group *g, struct ev *e);

static int get_value(ectx *c, const sdk_group *g, sdk_word_id w, int dmin, int dmax, sdk_value *v, int *have)
{
    const sdk_item *it = fld(g, w);
    *have = 0;
    if (!it) return 1;
    if (!sdk_value_read(c->d, it, 1, dmin, dmax, sdk_word(w), v, c->why, c->why_len)) { c->failed = 1; return 0; }
    *have = 1;
    return 1;
}

static void put_num(sdk_buf *b, int ind, const char *key, double v)
{
    char n[32];
    sdk_fmt(n, sizeof n, v);
    sdk_printf(b, "%.*s%s = %s;\n", ind, "\t\t\t\t\t\t\t\t\t\t\t\t", key, n);
}
static void put_str(sdk_buf *b, int ind, const char *key, const char *s)
{
    sdk_printf(b, "%.*s%s = \"%s\";\n", ind, "\t\t\t\t\t\t\t\t\t\t\t\t", key, s);
}
static void put_word(sdk_buf *b, int ind, const char *key, const char *s)
{
    sdk_printf(b, "%.*s%s = %s;\n", ind, "\t\t\t\t\t\t\t\t\t\t\t\t", key, s);
}
static void put_open(sdk_buf *b, int ind, const char *key) { sdk_printf(b, "%.*s%s = {\n", ind, "\t\t\t\t\t\t\t\t\t\t\t\t", key); }
static void put_close(sdk_buf *b, int ind) { sdk_printf(b, "%.*s}\n", ind, "\t\t\t\t\t\t\t\t\t\t\t\t"); }
static void put_vec(sdk_buf *b, int ind, const char *key, const char *names, const double *v, int n)
{
    int k;
    char nm[2] = { 0, 0 };
    put_open(b, ind, key);
    for (k = 0; k < n; k++) { nm[0] = names[k]; put_num(b, ind + 1, nm, v[k]); }
    put_close(b, ind);
}
static void put_range(sdk_buf *b, int ind, const char *key, double lo, double hi, const char *calc)
{
    put_open(b, ind, key);
    put_num(b, ind + 1, "val0", lo);
    put_num(b, ind + 1, "val1", hi);
    if (calc) put_str(b, ind + 1, "calcType", calc);
    put_close(b, ind);
}

/* One fx event: the common fields; `parms` is the item type's own block (written at indent 4). */
typedef struct ev {
    const char *type;
    char name[40];
    double dur, d0, d1, fin, fout, dist, fsd, size, col[4], off[3], turn[3];
    int has_dur, has_delay, loop, has_fin, has_fout, has_dist, has_fsd, has_size, has_col, has_off, has_turn;
    int follows, restarts, ntags, nwhen, nuntil, everytag;
    const char *rotation;
    char tag[8][96], when[8][96], until[8][96];
    sdk_buf parms;
} ev;

static int get_colour(ectx *c, const sdk_group *g, ev *e)
{
    const sdk_item *it = fld(g, SDK_F_COLOUR);
    double x[4];
    int n, k;
    if (!it) return 1;
    n = it->count == 2 ? sdk_numbers(&it->atoms[1], x, 4) : 0;
    if (n != 3 && n != 4)
        FAIL(c, it->line, "'%s' needs three numbers, as in '%s { 1, 0.5, 0.2 }', or four with the opacity last.",
             SDK_W(F_COLOUR), SDK_W(F_COLOUR));
    e->has_col = 1;
    for (k = 0; k < 3; k++) e->col[k] = x[k];
    e->col[3] = n == 4 ? x[3] : 1;
    return 1;
}

static void write_ev(sdk_buf *b, int index, const ev *e)
{
    sdk_printf(b, "\t\t\titem[%d] = {\n", index);
    put_str(b, 4, "name", e->name);
    put_str(b, 4, "type", e->type);
    if (e->has_dur) put_num(b, 4, "duration", e->dur);
    if (e->has_delay) { double d[2] = { e->d0, e->d1 }; put_vec(b, 4, "delay", "xy", d, 2); }
    if (e->restarts) put_word(b, 4, "restart", "true");
    if (e->loop) put_word(b, 4, "looping", "true");
    if (e->has_fin) put_num(b, 4, "fadeInTime", e->fin);
    if (e->has_fout) put_num(b, 4, "fadeOutTime", e->fout);
    if (e->has_fsd) put_num(b, 4, "fadeStartDistance", e->fsd);
    if (e->has_dist) put_num(b, 4, "distance", e->dist);
    if (e->has_size) put_num(b, 4, "size", e->size);
    if (e->has_col) put_vec(b, 4, "color", "xyzw", e->col, 4);
    {
        int k, w;
        for (w = 0; w < 2; w++) {
            int n = w ? e->nuntil : e->nwhen;
            put_open(b, 4, w ? "stopCondition" : "startCondition");
            put_num(b, 5, "num", n ? n : 1);
            if (!n) put_str(b, 5, "item[0]", "FX_NONE");
            for (k = 0; k < n; k++) {
                char item[16];
                snprintf(item, sizeof item, "item[%d]", k);
                put_str(b, 5, item, w ? e->until[k] : e->when[k]);
            }
            put_close(b, 4);
        }
    }
    if (e->everytag) put_str(b, 4, "multiTagUseType", e->everytag > 0 ? "FX_MULTI_TAG_USE_ALL" : "FX_MULTI_TAG_USE_EXPLICIT");
    if (e->follows) put_str(b, 4, "originType", "FX_ORG_TRACK_POS");
    if (e->has_off) put_vec(b, 4, "offset", "xyz", e->off, 3);
    if (e->rotation) put_str(b, 4, "rotationType", e->rotation);
    if (e->ntags) {
        int k;
        put_open(b, 4, "tagNames");
        put_num(b, 5, "num", e->ntags);
        for (k = 0; k < e->ntags; k++) {
            char item[16];
            snprintf(item, sizeof item, "item[%d]", k);
            put_str(b, 5, item, e->tag[k]);
        }
        put_close(b, 4);
    }
    if (e->has_turn) {
        put_open(b, 4, "rotOffsetAngles");
        put_num(b, 5, "pitch", e->turn[0]);
        put_num(b, 5, "yaw", e->turn[1]);
        put_num(b, 5, "roll", e->turn[2]);
        put_close(b, 4);
    }
    if (e->parms.p) sh_text_append(b, e->parms.p, e->parms.n);
    sdk_printf(b, "\t\t\t}\n");
}

static const sdk_word_id COMMON[] = { SDK_F_STARTS, SDK_F_LASTS, SDK_F_FADEIN, SDK_F_FADEOUT, SDK_F_REPEATS,
                                      SDK_F_OFFSET, SDK_F_AT, SDK_F_WHEN, SDK_F_TURN, SDK_F_FOLLOWS, SDK_F_FACING,
                                      SDK_F_RESTARTS, SDK_F_REACH, SDK_F_FADESFROM, SDK_F_UNTIL, SDK_F_EVERYTAG };
#define NCOMMON (int)(sizeof COMMON / sizeof COMMON[0])

/* A condition name or up to eight in braces, written as the game spells them (FX_ added when left out, upper case). */
static int conditions(ectx *c, const sdk_group *g, sdk_word_id w, char out[8][96], int *n)
{
    const sdk_item *it = fld(g, w);
    size_t q;
    *n = 0;
    if (!it) return 1;
    if (it->count == 2 && (it->atoms[1].kind == SDK_A_WORD || it->atoms[1].kind == SDK_A_STRING)) {
        char x[80];
        sdk_text(&it->atoms[1], x, sizeof x);
        snprintf(out[0], 96, "%s%s", _strnicmp(x, "FX_", 3) ? "FX_" : "", x);
        *n = 1;
    } else if (it->count == 2 && it->atoms[1].kind == SDK_A_GROUP && it->atoms[1].group->count >= 1 &&
               it->atoms[1].group->count <= 8) {
        for (q = 0; q < it->atoms[1].group->count; q++) {
            const sdk_item *x = &it->atoms[1].group->items[q];
            char y[80];
            if (x->count != 1 || (x->atoms[0].kind != SDK_A_WORD && x->atoms[0].kind != SDK_A_STRING))
                FAIL(c, it->line, "'%s' takes a condition name, or up to eight in braces.", sdk_word(w));
            sdk_text(&x->atoms[0], y, sizeof y);
            snprintf(out[q], 96, "%s%s", _strnicmp(y, "FX_", 3) ? "FX_" : "", y);
        }
        *n = (int)it->atoms[1].group->count;
    } else FAIL(c, it->line, "'%s' takes a condition name, or up to eight in braces.", sdk_word(w));
    for (q = 0; q < (size_t)*n; q++) {
        char *s;
        for (s = out[q]; *s; s++) *s = (char)toupper((unsigned char)*s);
    }
    return 1;
}

/* The common fields into e; `lasts_default` < 0: no duration unless written. `reach` and `fadesfrom` are the distance
 * a screen item reaches and where it starts to weaken, on any item but a light (whose reach is its radius). */
static int common(ectx *c, const sdk_group *g, ev *e, double lasts_default)
{
    if (strcmp(e->type ? e->type : "", "FX_LIGHT")) {
        if (!get_num(c, g, SDK_F_REACH, &e->dist, &e->has_dist)) return 0;
        if (!get_num(c, g, SDK_F_FADESFROM, &e->fsd, &e->has_fsd)) return 0;
    }
    int have, flag = 0;
    double lo, hi;
    {
        /* the delay: one number, or the two the game draws between, in the order written */
        const sdk_item *s = fld(g, SDK_F_STARTS);
        double x[2];
        int n = s && s->count == 2 ? sdk_numbers(&s->atoms[1], x, 2) : 0;
        (void)lo; (void)hi;
        if (s && n == 1) { e->has_delay = 1; e->d0 = e->d1 = x[0]; }
        else if (s && n == 2) { e->has_delay = 1; e->d0 = x[0]; e->d1 = x[1]; }
        else if (s) FAIL(c, s->line, "'%s' needs a number or two (the least and the most), as in '%s { 0.1, 0.3 }'.",
                         SDK_W(F_STARTS), SDK_W(F_STARTS));
    }
    if (!get_num(c, g, SDK_F_LASTS, &e->dur, &e->has_dur)) return 0;
    if (!e->has_dur && lasts_default >= 0) { e->dur = lasts_default; e->has_dur = 1; }
    if (!get_num(c, g, SDK_F_FADEIN, &e->fin, &e->has_fin)) return 0;
    if (!get_num(c, g, SDK_F_FADEOUT, &e->fout, &e->has_fout)) return 0;
    if (!get_flag(c, g, SDK_F_REPEATS, &flag, &have)) return 0;
    e->loop = have && flag;
    if (!get_vec(c, g, SDK_F_OFFSET, 3, 0, e->off, &e->has_off)) return 0;
    if (!get_vec(c, g, SDK_F_TURN, 3, 0, e->turn, &e->has_turn)) return 0;
    if (!get_flag(c, g, SDK_F_FOLLOWS, &e->follows, &have)) return 0;
    if (!get_flag(c, g, SDK_F_RESTARTS, &e->restarts, &have)) return 0;
    {
        const sdk_item *f = fld(g, SDK_F_FACING);
        if (f) {
            if (f->count == 2 && sdk_is(&f->atoms[1], SDK_W(F_FOLLOWS))) e->rotation = "FX_ROT_TRACK_AXIS";
            else if (f->count == 2 && sdk_is(&f->atoms[1], SDK_W(V_FIXED))) e->rotation = "FX_ROT_EXPLICIT_ANGLES";
            else if (f->count == 2 && sdk_is(&f->atoms[1], SDK_W(V_PARENT))) e->rotation = "FX_ROT_TRACK_AXIS_PARENT";
            else FAIL(c, f->line, "'%s' is %s, %s or %s.", SDK_W(F_FACING), SDK_W(F_FOLLOWS), SDK_W(V_FIXED), SDK_W(V_PARENT));
        }
    }
    {
        /* one tag, or a group of tags */
        const sdk_item *at = fld(g, SDK_F_AT);
        if (at) {
            size_t q;
            if (at->count == 2 && (at->atoms[1].kind == SDK_A_WORD || at->atoms[1].kind == SDK_A_STRING)) {
                sdk_text(&at->atoms[1], e->tag[0], sizeof e->tag[0]);
                e->ntags = 1;
            } else if (at->count == 2 && at->atoms[1].kind == SDK_A_GROUP && at->atoms[1].group->count &&
                       at->atoms[1].group->count <= 8) {
                for (q = 0; q < at->atoms[1].group->count; q++) {
                    const sdk_item *x = &at->atoms[1].group->items[q];
                    if (x->count != 1 || (x->atoms[0].kind != SDK_A_WORD && x->atoms[0].kind != SDK_A_STRING))
                        FAIL(c, at->line, "'%s' takes a tag name, or up to eight in braces.", SDK_W(F_AT));
                    sdk_text(&x->atoms[0], e->tag[q], sizeof e->tag[q]);
                }
                e->ntags = (int)at->atoms[1].group->count;
            } else FAIL(c, at->line, "'%s' takes a tag name, or up to eight in braces.", SDK_W(F_AT));
        }
    }
    if (!conditions(c, g, SDK_F_WHEN, e->when, &e->nwhen)) return 0;
    if (!conditions(c, g, SDK_F_UNTIL, e->until, &e->nuntil)) return 0;
    {
        int every;
        if (!get_flag(c, g, SDK_F_EVERYTAG, &every, &have)) return 0;
        if (have) e->everytag = every ? 1 : -1;
    }
    return 1;
}

/* Merges the item's own fields after the common ones into one accepted list. */
static int accepted(sdk_word_id *out, const sdk_word_id *own, int nown)
{
    int n = 0, k;
    for (k = 0; k < NCOMMON; k++) out[n++] = COMMON[k];
    for (k = 0; k < nown; k++) out[n++] = own[k];
    return n;
}

static int fields(ectx *c, const sdk_group *g, const char *what, const sdk_word_id *own, int nown)
{
    sdk_word_id all[64];
    int n = accepted(all, own, nown);
    return check_fields(c, g, what, all, n);
}

static int item_light(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_COLOUR, SDK_F_BRIGHTNESS, SDK_F_REACH, SDK_F_FLICKER, SDK_F_SHAPE,
                                       SDK_F_SHINE, SDK_F_SHADOWS, SDK_F_SEENFROM };
    double reach[3] = { 300, 300, 300 }, shine = 1, colour[3] = { 1, 1, 1 };
    sdk_value bright;
    char shape[160] = "", table[96] = "";
    int have, flag, hb;
    const sdk_item *fl;
    if (!fields(c, g, "a light", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    e->type = "FX_LIGHT";
    if (!common(c, g, e, -1)) return 0;
    if (!get_colour(c, g, e)) return 0;
    if (!get_value(c, g, SDK_F_BRIGHTNESS, 1, 1, &bright, &hb)) return 0;
    if (!hb) { memset(&bright, 0, sizeof bright); bright.form = SDK_VAL_CONST; bright.dims = 1; bright.c[0] = 5; }
    if (!get_vec(c, g, SDK_F_REACH, 3, 1, reach, &have)) return 0;
    if (!get_num(c, g, SDK_F_SHINE, &shine, &have)) return 0;
    if ((fl = fld(g, SDK_F_FLICKER)) != NULL) {
        if (fl->count == 2 && sdk_is(&fl->atoms[1], SDK_W(V_NONE))) snprintf(shape, sizeof shape, "lights/gaus");
        else if (fl->count == 2 && sdk_is(&fl->atoms[1], SDK_W(V_FLICKER))) snprintf(shape, sizeof shape, "lights/gaus_flicker_01");
        else if (fl->count == 2 && sdk_is(&fl->atoms[1], SDK_W(V_FIRE))) snprintf(shape, sizeof shape, "lights/fire_light_01");
        else FAIL(c, fl->line, "'%s' is %s, %s or %s; to change the brightness yourself, write '%s %s { ... }'.",
                  SDK_W(F_FLICKER), SDK_W(V_NONE), SDK_W(V_FLICKER), SDK_W(V_FIRE), SDK_W(F_BRIGHTNESS), SDK_W(V_PULSE));
    }
    {
        char s[160];
        if (!get_name(c, g, SDK_F_SHAPE, s, sizeof s, &have)) return 0;
        if (have) snprintf(shape, sizeof shape, "%s", s);
    }
    if (!get_flag(c, g, SDK_F_SHADOWS, &flag, &have)) return 0;
    if (have) put_word(&e->parms, 4, "noshadows", flag ? "false" : "true");
    /* a brightness that changes: the item's intensity table, its peak as the intensity */
    {
        double peak = bright.c[0];
        if (bright.form != SDK_VAL_CONST) {
            sdk_value cv = bright;
            int i, n = 33;
            char *text;
            double lo, hi;
            if (bright.form == SDK_VAL_PULSE || bright.form == SDK_VAL_FLICKER || bright.form == SDK_VAL_FADE) {
                /* sampled into a curve over one period (a fade over the item's life) */
                memset(&cv, 0, sizeof cv);
                cv.form = SDK_VAL_CURVE;
                cv.dims = 1;
                cv.points = n;
                for (i = 0; i < n; i++) {
                    double u = (double)i / (n - 1), x;
                    if (bright.form == SDK_VAL_FADE) x = bright.from[0] + (bright.to[0] - bright.from[0]) * u;
                    else if (bright.form == SDK_VAL_PULSE) x = (bright.low[0] + bright.high[0]) / 2 + (bright.high[0] - bright.low[0]) / 2 * sin(u * 6.283185307179586);
                    else x = bright.low[0] + (bright.high[0] - bright.low[0]) * (0.5 + 0.5 * sin(u * 37.0) * cos(u * 13.0));
                    cv.t[i] = u;
                    cv.v[i][0] = x;
                }
            }
            text = sdk_value_table_text(&cv, 0, bright.form == SDK_VAL_FADE || (cv.form == SDK_VAL_CURVE && !cv.loop && bright.form == SDK_VAL_CURVE), &lo, &hi);
            if (!text) { c->oom = 1; return 0; }
            {
                /* the table holds value / peak: the light's intensity is the peak times the table (inferred) */
                sdk_value nv = cv;
                for (i = 0; i < nv.points; i++) nv.v[i][0] = hi > 0 ? cv.v[i][0] / hi : 0;
                free(text);
                text = sdk_value_table_text(&nv, 0, 0, &lo, &lo);
            }
            {
                const char *name = text ? sdk_register("table", text) : NULL;
                free(text);
                if (!name) { c->oom = 1; return 0; }
                snprintf(table, sizeof table, "%s", name);
            }
            peak = hi;
        }
        put_open(&e->parms, 4, "lightParms");
        put_vec(&e->parms, 5, "radius", "xyz", reach, 3);
        if (hb) put_num(&e->parms, 5, "intensity", peak);
        if (shine != 1) put_num(&e->parms, 5, "specularScale", shine);
        {
            double seen;
            int hseen;
            if (!get_num(c, g, SDK_F_SEENFROM, &seen, &hseen)) return 0;
            if (hseen) put_num(&e->parms, 5, "maxVisibleRange", seen);
        }
        if (table[0]) put_str(&e->parms, 5, "intensityTable", table);
        if (shape[0]) put_str(&e->parms, 5, "lightMtr", shape);
        put_close(&e->parms, 4);
    }
    return 1;
}

static int item_decal(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_MATERIAL, SDK_F_SIZE, SDK_F_DEPTH, SDK_F_ANGLE, SDK_F_GLOW,
                                       SDK_F_GLOWLASTS, SDK_F_COLOUR, SDK_F_STAYS };
    char mtr[200];
    double depth = 16, angle = 0, glow[4] = { 0, 0, 0, 0 }, glowlasts = 0;
    int have, hm, hdepth, stays = 0;
    if (!fields(c, g, "a decal", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, 10)) return 0;
    e->type = "FX_DECAL";
    if (!get_name(c, g, SDK_F_MATERIAL, mtr, sizeof mtr, &hm)) return 0;
    if (!hm) {
        const sdk_item *any = g->count ? &g->items[0] : NULL;
        FAIL(c, any ? any->line : c->d->root->line, "a decal needs its '%s', as in '%s %s(\"textures/decals/blood_splatter/blood_splatter_01\")'.",
             SDK_W(F_MATERIAL), SDK_W(F_MATERIAL), SDK_W(C_GAME));
    }
    /* the engine projects a decal only with a duration above 0 (VK FUN_1416f0240 case 2) */
    if (!(e->dur > 0)) { note(c, "a decal's 'lasts' must be above 0 to show; drawn for 10 seconds"); e->dur = 10; }
    e->has_size = 1;
    e->size = 64;
    if (!get_num(c, g, SDK_F_SIZE, &e->size, &have)) return 0;
    if (!get_num(c, g, SDK_F_DEPTH, &depth, &hdepth)) return 0;
    if (!get_num(c, g, SDK_F_ANGLE, &angle, &have)) return 0;
    if (!get_vec(c, g, SDK_F_GLOW, 4, 0, glow, &have)) return 0;
    if (!get_num(c, g, SDK_F_GLOWLASTS, &glowlasts, &have)) return 0;
    if (!get_colour(c, g, e)) return 0;
    if (!get_flag(c, g, SDK_F_STAYS, &stays, &have)) return 0;
    put_open(&e->parms, 4, "decalParms");
    if (angle) put_num(&e->parms, 5, "angle", angle);
    if (hdepth) put_num(&e->parms, 5, "depth", depth);
    if (glowlasts > 0) put_num(&e->parms, 5, "emissiveLifetime", glowlasts * 1000);
    if (glow[3] > 0) put_vec(&e->parms, 5, "emissiveTint", "xyzw", glow, 4);
    put_str(&e->parms, 5, "decalMtr", mtr);
    if (stays) put_word(&e->parms, 5, "isPersistent", "true");
    put_close(&e->parms, 4);
    return 1;
}

static int item_shake(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_STRENGTH, SDK_F_REACH, SDK_F_FADESFROM, SDK_F_USE, SDK_F_ANGLES, SDK_F_MOVE };
    double s = 0.05;
    char use[200];
    int hu, hs;
    if (!fields(c, g, "a shake", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    e->type = "FX_SCREEN_SHAKE";
    if (!get_num(c, g, SDK_F_STRENGTH, &s, &hs)) return 0;
    if (!get_name(c, g, SDK_F_USE, use, sizeof use, &hu)) return 0;
    {
        double ang[3], mv[3];
        int ha, hm;
        if (!get_vec(c, g, SDK_F_ANGLES, 3, 0, ang, &ha)) return 0;
        if (!get_vec(c, g, SDK_F_MOVE, 3, 0, mv, &hm)) return 0;
        if (hu || hs || ha || hm) {
            put_open(&e->parms, 4, "screenShakeParms");
            if (hs) put_num(&e->parms, 5, "magnitude", s);
            if (ha) {
                put_open(&e->parms, 5, "maxAngles");
                put_num(&e->parms, 6, "pitch", ang[0]);
                put_num(&e->parms, 6, "yaw", ang[1]);
                put_num(&e->parms, 6, "roll", ang[2]);
                put_close(&e->parms, 5);
            }
            if (hm) put_vec(&e->parms, 5, "maxOffset", "xyz", mv, 3);
            if (hu) put_str(&e->parms, 5, "advancedShakeDecl", use);
            put_close(&e->parms, 4);
        }
    }
    return 1;
}

static int item_flash(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_BRIGHTNESS, SDK_F_REACH, SDK_F_FADESFROM };
    double x = 0.6;
    int have;
    if (!fields(c, g, "a flash", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    e->type = "FX_SCREEN_FLASH";
    if (!get_num(c, g, SDK_F_BRIGHTNESS, &x, &have)) return 0;
    if (have) {
        put_open(&e->parms, 4, "screenFlashParms");
        put_num(&e->parms, 5, "exposure", x);
        put_close(&e->parms, 4);
    }
    return 1;
}

static int item_screen(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_MATERIAL, SDK_F_COLOUR, SDK_F_TILES, SDK_F_MIRROR, SDK_F_BOTTOM };
    char mtr[200];
    double tint[4] = { 1, 1, 1, 1 }, tiles[2];
    int have, hm, ht, hc, mirror = 0, bottom = 0;
    if (!fields(c, g, "a screen image", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    e->type = "FX_SCREEN_OVERLAY";
    if (!get_name(c, g, SDK_F_MATERIAL, mtr, sizeof mtr, &hm)) return 0;
    if (!hm) FAIL(c, g->line, "a screen image needs its '%s', as in '%s %s(\"textures/effects/misc/screen_player_damage_02\")'.",
                  SDK_W(F_MATERIAL), SDK_W(F_MATERIAL), SDK_W(C_GAME));
    if (!get_vec(c, g, SDK_F_COLOUR, 3, 0, tint, &hc)) return 0;
    if (!get_vec(c, g, SDK_F_TILES, 2, 0, tiles, &ht)) return 0;
    if (!get_flag(c, g, SDK_F_MIRROR, &mirror, &have)) return 0;
    if (!get_flag(c, g, SDK_F_BOTTOM, &bottom, &have)) return 0;
    put_open(&e->parms, 4, "screenOverlayParms");
    put_str(&e->parms, 5, "overlay", mtr);
    if (hc) put_vec(&e->parms, 5, "tint", "rgba", tint, 4);
    if (ht) put_vec(&e->parms, 5, "tileDimensions", "xy", tiles, 2);
    if (mirror) put_word(&e->parms, 5, "mirrorTiles", "true");
    if (bottom) put_word(&e->parms, 5, "bottomOnly", "true");
    put_close(&e->parms, 4);
    return 1;
}

static int item_blur(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_STRENGTH, SDK_F_STYLE };
    double s = 100;
    int have, soft = 0;
    const sdk_item *st;
    if (!fields(c, g, "a blur", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    if ((st = fld(g, SDK_F_STYLE)) != NULL) {
        if (st->count == 2 && sdk_is(&st->atoms[1], SDK_W(V_SOFT))) soft = 1;
        else if (!(st->count == 2 && sdk_is(&st->atoms[1], SDK_W(V_RADIAL))))
            FAIL(c, st->line, "'%s' is %s or %s.", SDK_W(F_STYLE), SDK_W(V_RADIAL), SDK_W(V_SOFT));
    }
    if (!get_num(c, g, SDK_F_STRENGTH, &s, &have)) return 0;
    e->type = soft ? "FX_BLUR_GAUSSIAN" : "FX_BLUR_RADIAL";
    if (have) {
        put_open(&e->parms, 4, soft ? "blurGaussianParms" : "blurRadial2Parms");
        put_num(&e->parms, 5, "radius", s);
        put_close(&e->parms, 4);
    }
    return 1;
}

static int item_focus(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_NEAR, SDK_F_FOCUS, SDK_F_FAR };
    double n = 0, f = 0, far = 0;
    int have;
    if (!fields(c, g, "a focus", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    e->type = "FX_DEPTH_OF_FIELD";
    int hn, hf, hr;
    if (!get_num(c, g, SDK_F_NEAR, &n, &hn)) return 0;
    if (!get_num(c, g, SDK_F_FOCUS, &f, &hf)) return 0;
    if (!get_num(c, g, SDK_F_FAR, &far, &hr)) return 0;
    (void)have;
    if (hn || hf || hr) {
        put_open(&e->parms, 4, "depthOfFieldParms");
        if (hn) put_num(&e->parms, 5, "nearDepth", n);
        if (hr) put_num(&e->parms, 5, "farDepth", far);
        if (hf) put_num(&e->parms, 5, "focusDepth", f);
        put_close(&e->parms, 4);
    }
    return 1;
}

static int item_named(ectx *c, const sdk_group *g, ev *e, const char *what, const char *type, const char *block,
                      const char *key, double lasts)
{
    static const sdk_word_id OWN[] = { SDK_F_USE, SDK_F_COLOUR, SDK_F_SIZE };
    char use[200];
    int hu;
    if (!fields(c, g, what, OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, lasts)) return 0;
    e->type = type;
    if (!get_name(c, g, SDK_F_USE, use, sizeof use, &hu)) return 0;
    if (!hu) FAIL(c, g->line, "%s needs '%s' and the name of the one to play.", what, SDK_W(F_USE));
    if (!get_colour(c, g, e)) return 0;
    if (!get_num(c, g, SDK_F_SIZE, &e->size, &e->has_size)) return 0;
    put_open(&e->parms, 4, block);
    put_str(&e->parms, 5, key, use);
    if (!strcmp(type, "FX_FLARE")) put_word(&e->parms, 5, "isAutosprited", "true");
    put_close(&e->parms, 4);
    return 1;
}

static int item_model(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_MODEL, SDK_F_MATERIAL, SDK_F_COLOUR, SDK_F_SIZE };
    char model[200], mtr[200];
    int hm, hmt;
    if (!fields(c, g, "a model", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    e->type = "FX_MODEL";
    if (!get_name(c, g, SDK_F_MODEL, model, sizeof model, &hm)) return 0;
    if (!hm) FAIL(c, g->line, "a model needs its '%s'.", SDK_W(F_MODEL));
    if (!get_name(c, g, SDK_F_MATERIAL, mtr, sizeof mtr, &hmt)) return 0;
    if (!get_colour(c, g, e)) return 0;
    if (!get_num(c, g, SDK_F_SIZE, &e->size, &e->has_size)) return 0;
    put_open(&e->parms, 4, "modelParms");
    put_str(&e->parms, 5, "modelName", model);
    if (hmt) put_str(&e->parms, 5, "customMaterial", mtr);
    put_close(&e->parms, 4);
    return 1;
}

static int item_sound(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_SOUND, SDK_F_STOPS };
    char snd[200];
    int hs, stops = 0, have;
    if (!fields(c, g, "a sound", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    e->type = "FX_SOUND";
    if (!get_name(c, g, SDK_F_SOUND, snd, sizeof snd, &hs)) return 0;
    if (!hs) FAIL(c, g->line, "a sound needs its '%s'.", SDK_W(F_SOUND));
    if (!get_flag(c, g, SDK_F_STOPS, &stops, &have)) return 0;
    put_open(&e->parms, 4, "soundParms");
    put_str(&e->parms, 5, "sound", snd);
    if (stops) put_word(&e->parms, 5, "stopSound", "true");
    put_close(&e->parms, 4);
    return 1;
}

static int item_glow(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_COLOUR, SDK_F_STRENGTH, SDK_F_VALUE, SDK_F_STAYS };
    char parm[96] = "bloomcolor";
    double colour[4] = { 1, 1, 1, 1 }, s = 1;
    int have, stays = 0, k;
    const sdk_item *ci;
    if (!fields(c, g, "a glow", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    e->type = "FX_RENDERPARM";
    if (!get_name(c, g, SDK_F_VALUE, parm, sizeof parm, &have)) return 0;
    if ((ci = fld(g, SDK_F_COLOUR)) != NULL) {
        double x[4];
        int n = ci->count == 2 ? sdk_numbers(&ci->atoms[1], x, 4) : 0;
        if (n != 3 && n != 4) FAIL(c, ci->line, "'%s' needs three or four numbers, as in '%s { 1, 0.5, 0.2 }'.",
                                   SDK_W(F_COLOUR), SDK_W(F_COLOUR));
        for (k = 0; k < n; k++) colour[k] = x[k];
    }
    if (!get_num(c, g, SDK_F_STRENGTH, &s, &have)) return 0;
    if (ci || have) {
        e->has_col = 1;
        for (k = 0; k < 3; k++) e->col[k] = colour[k] * s;
        e->col[3] = colour[3];
    }
    if (!get_flag(c, g, SDK_F_STAYS, &stays, &have)) return 0;
    put_open(&e->parms, 4, "renderParmParms");
    put_str(&e->parms, 5, "declRenderParm", parm);
    if (stays) put_word(&e->parms, 5, "forceValueWhenStopped", "true");
    put_close(&e->parms, 4);
    return 1;
}

static int item_wind(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_STRENGTH, SDK_F_ANGLE };
    double s = 1, a = 0, st[2];
    int have;
    if (!fields(c, g, "a wind", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    e->type = "FX_WIND";
    int hs, ha;
    if (!get_num(c, g, SDK_F_STRENGTH, &s, &hs)) return 0;
    if (!get_num(c, g, SDK_F_ANGLE, &a, &ha)) return 0;
    (void)have;
    st[0] = st[1] = s;
    if (hs || ha) {
        put_open(&e->parms, 4, "windParms");
        if (ha) put_num(&e->parms, 5, "angle", a);
        if (hs) put_vec(&e->parms, 5, "strength", "xy", st, 2);
        put_close(&e->parms, 4);
    }
    return 1;
}

/* An environment item: `use` a whole env decl (FX_ENV_OVERRIDE), or values (FX_ENV_CHANGE; more than 16 are split
 * over items by the caller: this writes the first 16, *rest the count left). */
static int item_environment(ectx *c, const sdk_group *g, ev *e, sdk_env_parm *p, int *np)
{
    static const char *skipw[24];
    char use[200];
    int hu, k = 0, n;
    skipw[k++] = SDK_W(F_STARTS); skipw[k++] = SDK_W(F_LASTS); skipw[k++] = SDK_W(F_FADEIN);
    skipw[k++] = SDK_W(F_FADEOUT); skipw[k++] = SDK_W(F_REPEATS); skipw[k++] = SDK_W(F_OFFSET);
    skipw[k++] = SDK_W(F_AT); skipw[k++] = SDK_W(F_WHEN); skipw[k++] = SDK_W(F_TURN); skipw[k++] = SDK_W(F_USE);
    skipw[k++] = SDK_W(F_REACH); skipw[k++] = SDK_W(F_FADESFROM); skipw[k++] = SDK_W(F_UNTIL); skipw[k++] = SDK_W(F_EVERYTAG);
    skipw[k++] = SDK_W(F_FOLLOWS); skipw[k++] = SDK_W(F_FACING); skipw[k++] = SDK_W(F_RESTARTS);
    skipw[k] = NULL;
    if (!common(c, g, e, -1)) return 0;
    if (!get_name(c, g, SDK_F_USE, use, sizeof use, &hu)) return 0;
    n = sdk_env_parms(c->d, g, "an environment item", skipw, p, c->why, c->why_len);
    if (n < 0) { c->failed = 1; return 0; }
    if (hu && n) FAIL(c, g->line, "an environment item uses one environment or names its values, not both.");
    if (hu) {
        e->type = "FX_ENV_OVERRIDE";
        put_open(&e->parms, 4, "envParms");
        put_str(&e->parms, 5, "declEnv", use);
        put_close(&e->parms, 4);
        *np = 0;
        return 1;
    }
    if (!n) FAIL(c, g->line, "an environment item needs '%s' or at least one of its values (fog, bloom, ...).", SDK_W(F_USE));
    e->type = "FX_ENV_CHANGE";
    *np = n;
    return 1;
}

static void env_parms_block(sdk_buf *b, const sdk_env_parm *p, int n)
{
    int k;
    put_open(b, 4, "envParms");
    put_open(b, 5, "envRenderParm");
    put_num(b, 6, "num", n);
    for (k = 0; k < n; k++) {
        char item[16], lower[64];
        double v[4];
        size_t j;
        int cc, srgb = sdk_parm_is_srgb(p[k].parm, strlen(p[k].parm));
        for (j = 0; p[k].parm[j] && j < sizeof lower - 1; j++) lower[j] = (char)tolower((unsigned char)p[k].parm[j]);
        lower[j] = 0;
        for (cc = 0; cc < 4; cc++) {
            const sdk_value *x = &p[k].v;
            int s = cc < x->dims ? cc : (x->dims == 1 ? 0 : -1);
            double y = 1;
            if (s >= 0) {
                if (x->form == SDK_VAL_PULSE || x->form == SDK_VAL_FLICKER) y = (x->low[s] + x->high[s]) / 2;
                else if (x->form == SDK_VAL_FADE) y = x->to[s];
                else if (x->form == SDK_VAL_CURVE) y = x->v[0][s];
                else y = x->c[s];
                if (srgb && cc < 3) y = sdk_srgb_to_linear(y);
            }
            v[cc] = y;
        }
        snprintf(item, sizeof item, "item[%d]", k);
        put_open(b, 6, item);
        put_str(b, 7, "parm", lower);
        put_vec(b, 7, "value", "xyzw", v, 4);
        put_close(b, 6);
    }
    put_close(b, 5);
    put_close(b, 4);
}

/* A value over the particle's life as a stage parm: a constant (val0 = val1), a range per particle (GENERIC), a fade
 * (PARAMETRIC_EVAL: val0 at birth to val1 at death), or a curve, pulse or flicker (CURVE_GENERIC: val0 x table(t) +
 * val1, the table the product derives). `comp` picks the value's component. */
static int life_parm(ectx *c, sdk_buf *b, int ind, const char *key, const sdk_value *v, int comp, double k,
                     double rlo, double rhi, int ranged, char tables[][96], int *ntables)
{
    if (ranged) { put_range(b, ind, key, rlo * k, rhi * k, "PARTICLE_CALC_GENERIC"); return 1; }
    switch (v->form) {
    case SDK_VAL_CONST: put_range(b, ind, key, v->c[comp] * k, v->c[comp] * k, "PARTICLE_CALC_GENERIC"); return 1;
    case SDK_VAL_FADE: put_range(b, ind, key, v->from[comp] * k, v->to[comp] * k, "PARTICLE_CALC_PARAMETRIC_EVAL"); return 1;
    default: {
        sdk_value cv = *v;
        double lo, hi;
        char *text;
        const char *name;
        int i, n = 33, t;
        if (v->form != SDK_VAL_CURVE) {
            memset(&cv, 0, sizeof cv);
            cv.form = SDK_VAL_CURVE;
            cv.dims = v->dims;
            cv.points = n;
            for (i = 0; i < n; i++) {
                double u = (double)i / (n - 1);
                int q;
                cv.t[i] = u;
                for (q = 0; q < v->dims; q++) {
                    double s = v->form == SDK_VAL_PULSE ? sin(u * 6.283185307179586 * v->rate) : 0.5 + 0.5 * sin(u * 37.0 * v->rate) * cos(u * 13.0);
                    cv.v[i][q] = v->form == SDK_VAL_PULSE ? (v->low[q] + v->high[q]) / 2 + (v->high[q] - v->low[q]) / 2 * s
                                                          : v->low[q] + (v->high[q] - v->low[q]) * s;
                }
            }
        }
        /* a particle's curve runs over its life: times 0..1 */
        {
            double last = cv.t[cv.points - 1] > 0 ? cv.t[cv.points - 1] : 1;
            for (i = 0; i < cv.points; i++) cv.t[i] /= last;
        }
        text = sdk_value_table_text(&cv, comp, 1, &lo, &hi);
        if (!text) { c->oom = 1; return 0; }
        name = sdk_register("table", text);
        free(text);
        if (!name) { c->oom = 1; return 0; }
        for (t = 0; t < *ntables; t++) if (!strcmp(tables[t], name)) break;
        if (t == *ntables) {
            if (*ntables == 16) { note(c, "a stage holds at most 16 curves; '%s' drawn at its first value", key);
                                  put_range(b, ind, key, cv.v[0][comp] * k, cv.v[0][comp] * k, "PARTICLE_CALC_GENERIC");
                                  return 1; }
            snprintf(tables[t], 96, "%s", name);
            (*ntables)++;
        }
        put_open(b, ind, key);
        put_num(b, ind + 1, "val0", (hi - lo) * k);
        put_num(b, ind + 1, "val1", lo * k);
        put_num(b, ind + 1, "tableIdx", t);
        put_str(b, ind + 1, "calcType", "PARTICLE_CALC_CURVE_GENERIC");
        put_close(b, ind);
        return 1;
    }
    }
}

static double value_peak(const sdk_value *v)
{
    double m = 0;
    int q, i;
    for (q = 0; q < (v->dims ? v->dims : 1); q++) {
        double a = fabs(v->c[q]);
        if (v->form == SDK_VAL_PULSE || v->form == SDK_VAL_FLICKER) a = fabs(v->high[q]) > fabs(v->low[q]) ? fabs(v->high[q]) : fabs(v->low[q]);
        if (v->form == SDK_VAL_FADE) a = fabs(v->from[q]) > fabs(v->to[q]) ? fabs(v->from[q]) : fabs(v->to[q]);
        if (v->form == SDK_VAL_CURVE) for (i = 0; i < v->points; i++) if (fabs(v->v[i][q]) > a) a = fabs(v->v[i][q]);
        if (a > m) m = a;
    }
    return m;
}

/* The stage's material: the author's, or a `type particle` material made from a bare image, compiled by modlook.c
 * and registered. */
static int particle_material(ectx *c, const sdk_group *g, char *out, size_t cap)
{
    char img[200], mtr[200];
    int hi, hm, have, lit = 0;
    double glow = 1;
    const sdk_item *li;
    if (!get_name(c, g, SDK_F_MATERIAL, mtr, sizeof mtr, &hm)) return 0;
    if (!get_name(c, g, SDK_F_IMAGE, img, sizeof img, &hi)) return 0;
    if (hm && hi) FAIL(c, g->line, "particles take an '%s' or a '%s', not both.", SDK_W(F_IMAGE), SDK_W(F_MATERIAL));
    if (hm) {
        if (fld(g, SDK_F_LIGHTING) || fld(g, SDK_F_GLOW))
            FAIL(c, (fld(g, SDK_F_LIGHTING) ? fld(g, SDK_F_LIGHTING) : fld(g, SDK_F_GLOW))->line,
                 "'%s' and '%s' go with an '%s'; a '%s' has its own.", SDK_W(F_LIGHTING), SDK_W(F_GLOW), SDK_W(F_IMAGE),
                 SDK_W(F_MATERIAL));
        snprintf(out, cap, "%s", mtr);
        return 1;
    }
    if (!hi) FAIL(c, g->line, "particles need an '%s' (or a '%s'), as in '%s images/fx/ember.tga'.", SDK_W(F_IMAGE),
                  SDK_W(F_MATERIAL), SDK_W(F_IMAGE));
    if ((li = fld(g, SDK_F_LIGHTING)) != NULL) {
        if (li->count == 2 && sdk_is(&li->atoms[1], SDK_W(V_LIT))) lit = 1;
        else if (!(li->count == 2 && sdk_is(&li->atoms[1], SDK_W(V_UNLIT))))
            FAIL(c, li->line, "'%s' is %s or %s.", SDK_W(F_LIGHTING), SDK_W(V_UNLIT), SDK_W(V_LIT));
    }
    if (!get_num(c, g, SDK_F_GLOW, &glow, &have)) return 0;
    {
        char text[600], gl[32], why[400], *compiled = NULL;
        size_t cn = 0;
        int r;
        const char *name;
        sdk_fmt(gl, sizeof gl, glow);
        snprintf(text, sizeof text, "{\ntype\tparticle\ntransmap\t\"%s\"\nlighting\t%s\nglow\t%s\n}\n", img,
                 lit ? "lit" : "unlit", gl);
        r = sh_look_compile(c->d->file, text, strlen(text), &compiled, &cn, why, sizeof why);
        if (r == SH_LOOK_OOM) { c->oom = 1; return 0; }
        if (r == SH_LOOK_MISTAKE) FAIL(c, g->line, "%s", why);
        name = sdk_register("material", r == SH_LOOK_CHANGED ? compiled : text);
        free(compiled);
        if (!name) { c->oom = 1; return 0; }
        snprintf(out, cap, "%s", name);
    }
    return 1;
}

/* Compiles a particles item into a particle decl (registered) and writes the fx item's particleParms. */
static int item_particles(ectx *c, const sdk_group *g, ev *e)
{
    static const sdk_word_id OWN[] = { SDK_F_IMAGE, SDK_F_MATERIAL, SDK_F_LIGHTING, SDK_F_GLOW, SDK_F_FRAMES,
                                       SDK_F_FRAMERATE, SDK_F_COUNT, SDK_F_LIFE, SDK_F_BURST, SDK_F_FROM,
                                       SDK_F_DIRECTION, SDK_F_SPREAD, SDK_F_SPEED, SDK_F_FALL, SDK_F_DRAG, SDK_F_SIZE,
                                       SDK_F_COLOUR, SDK_F_OPACITY, SDK_F_BRIGHTNESS, SDK_F_ANGLE, SDK_F_FACES,
                                       SDK_F_BOUNCE, SDK_F_USE, SDK_F_ONSCREEN };
    sdk_buf s = { 0 }, decl = { 0 };
    char mtr[200], tables[16][96];
    int ntables = 0, have, burst = 0, cycles = 1, hs, hc, ho, outward = 0, simulate = 0, bounce = 0, stick = 0;
    double count = 30, life0 = 1, life1 = 1, spread = 0, sp0 = 0, sp1 = 0, fall = 0, drag = 0, elastic = 0.5;
    double frames[2] = { 1, 1 }, framerate = 0, ang0 = 0, ang1 = 0, bright = 1, opacity = 1;
    double from_r[3] = { 0, 0, 0 }, sz_peak = 4, fin = 0.1, fout = 0.25;
    const char *dist = NULL;
    sdk_value size, colour;
    const sdk_item *it;
    if (!fields(c, g, "particles", OWN, (int)(sizeof OWN / sizeof OWN[0]))) return 0;
    if (!common(c, g, e, -1)) return 0;
    e->type = "FX_PARTICLE";
    {
        /* one of the game's particle decls (or the mod's) played as it is: the item's size scales it */
        char use[200];
        int hu;
        if (!get_name(c, g, SDK_F_USE, use, sizeof use, &hu)) return 0;
        if (hu) {
            size_t q;
            for (q = 0; q < g->count; q++) {
                const sdk_item *x = &g->items[q];
                int ok = 0;
                size_t k;
                if (!x->count) continue;
                for (k = 0; k < (size_t)NCOMMON; k++) if (sdk_is(&x->atoms[0], sdk_word(COMMON[k]))) ok = 1;
                if (sdk_is(&x->atoms[0], SDK_W(F_USE)) || sdk_is(&x->atoms[0], SDK_W(F_SIZE)) || sdk_is(&x->atoms[0], SDK_W(F_COLOUR)) ||
                    sdk_is(&x->atoms[0], SDK_W(F_ONSCREEN))) ok = 1;
                if (!ok) {
                    char w[64];
                    sdk_text(&x->atoms[0], w, sizeof w);
                    FAIL(c, x->line, "particles that '%s' a decl take only its timing, '%s' and '%s'; '%s' describes particles of their own.",
                         SDK_W(F_USE), SDK_W(F_SIZE), SDK_W(F_COLOUR), w);
                }
            }
            if (!get_num(c, g, SDK_F_SIZE, &e->size, &e->has_size)) return 0;
            if (!get_colour(c, g, e)) return 0;
            {
                int on, hon;
                if (!get_flag(c, g, SDK_F_ONSCREEN, &on, &hon)) return 0;
                put_open(&e->parms, 4, "particleParms");
                put_str(&e->parms, 5, "declPrt", use);
                if (hon && on) put_word(&e->parms, 5, "isScreenPrt", "true");
                put_close(&e->parms, 4);
            }
            return 1;
        }
    }
    /* the common fadein / fadeout of particles are fractions of each particle's life */
    if (e->has_fin) { fin = e->fin; e->has_fin = 0; }
    if (e->has_fout) { fout = e->fout; e->has_fout = 0; }
    if (e->loop) { cycles = 0; e->loop = 0; }
    if (!particle_material(c, g, mtr, sizeof mtr)) return 0;
    if (!get_num(c, g, SDK_F_COUNT, &count, &have)) return 0;
    if (count < 1) count = 1;
    if (!get_range(c, g, SDK_F_LIFE, &life0, &life1, &have)) return 0;
    if (!get_flag(c, g, SDK_F_BURST, &burst, &have)) return 0;
    if (!get_vec(c, g, SDK_F_FRAMES, 2, 0, frames, &have)) return 0;
    if (!get_num(c, g, SDK_F_FRAMERATE, &framerate, &have)) return 0;
    if ((it = fld(g, SDK_F_FROM)) != NULL) {
        size_t a = 1;
        int surface = 0;
        double r[3];
        int n;
        if (it->count >= 2 && sdk_is(&it->atoms[1], SDK_W(V_SURFACE))) { surface = 1; a = 2; }
        if (it->count == a + 1 && sdk_is(&it->atoms[a], SDK_W(V_POINT))) dist = NULL;
        else if (it->count == a + 2 && (n = sdk_numbers(&it->atoms[a + 1], r, 3)) >= 1) {
            if (sdk_is(&it->atoms[a], SDK_W(V_SPHERE)) && n == 1) { dist = surface ? "PDIST_SPHERE_SURFACE" : "PDIST_SPHERE"; from_r[0] = from_r[1] = from_r[2] = r[0]; }
            else if (sdk_is(&it->atoms[a], SDK_W(V_RING)) && n == 1) { dist = "PDIST_CYLINDER_SURFACE"; from_r[0] = from_r[1] = r[0]; }
            else if (sdk_is(&it->atoms[a], SDK_W(V_DISC)) && n == 1) { dist = surface ? "PDIST_CYLINDER_SURFACE" : "PDIST_CYLINDER"; from_r[0] = from_r[1] = r[0]; }
            else if (sdk_is(&it->atoms[a], SDK_W(V_BOX)) && n == 3) { dist = surface ? "PDIST_RECT_SURFACE" : "PDIST_RECT"; memcpy(from_r, r, sizeof r); }
            else goto bad_from;
        } else {
        bad_from:
            FAIL(c, it->line, "'%s' is %s, %s r, %s r, %s r or %s { x, y, z }, with '%s' before it for the surface only.",
                 SDK_W(F_FROM), SDK_W(V_POINT), SDK_W(V_SPHERE), SDK_W(V_RING), SDK_W(V_DISC), SDK_W(V_BOX), SDK_W(V_SURFACE));
        }
    }
    if ((it = fld(g, SDK_F_DIRECTION)) != NULL) {
        if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_OUTWARD))) outward = 1;
        else if (!(it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_UP))))
            FAIL(c, it->line, "'%s' is %s or %s; turn the item with '%s' to aim it elsewhere.", SDK_W(F_DIRECTION),
                 SDK_W(V_UP), SDK_W(V_OUTWARD), SDK_W(F_TURN));
    }
    if (!get_num(c, g, SDK_F_SPREAD, &spread, &have)) return 0;
    if (!get_range(c, g, SDK_F_SPEED, &sp0, &sp1, &hs)) return 0;
    if (!get_num(c, g, SDK_F_FALL, &fall, &have)) return 0;
    if (!get_num(c, g, SDK_F_DRAG, &drag, &have)) return 0;
    if ((it = fld(g, SDK_F_BOUNCE)) != NULL) {
        double x[1];
        if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_STICK))) { bounce = 1; stick = 1; elastic = 0; }
        else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_NONE))) bounce = 0;
        else if (it->count == 2 && sdk_numbers(&it->atoms[1], x, 1) == 1) { bounce = 1; elastic = x[0]; }
        else FAIL(c, it->line, "'%s' is how much a particle keeps of its speed (0 to 1), %s or %s.", SDK_W(F_BOUNCE),
                  SDK_W(V_STICK), SDK_W(V_NONE));
    }
    /* size: one number is constant, two a range per particle, a value form over the life */
    {
        const sdk_item *si = fld(g, SDK_F_SIZE);
        double r[2];
        int nr = si && si->count == 2 ? sdk_numbers(&si->atoms[1], r, 2) : 0;
        memset(&size, 0, sizeof size);
        size.form = SDK_VAL_CONST; size.dims = 1; size.c[0] = 4;
        if (si && nr == 2) { size.c[0] = r[0]; sz_peak = r[0] > r[1] ? r[0] : r[1]; }
        else if (!get_value(c, g, SDK_F_SIZE, 1, 1, &size, &have)) return 0;
        if (!(si && nr == 2)) sz_peak = value_peak(&size);
        if (si && nr == 2) { /* a range */ size.form = -1; size.low[0] = r[0] < r[1] ? r[0] : r[1]; size.high[0] = r[0] < r[1] ? r[1] : r[0]; }
    }
    memset(&colour, 0, sizeof colour);
    colour.form = SDK_VAL_CONST; colour.dims = 3; colour.c[0] = colour.c[1] = colour.c[2] = 1;
    if (!get_value(c, g, SDK_F_COLOUR, 3, 3, &colour, &hc)) return 0;
    if (!get_num(c, g, SDK_F_OPACITY, &opacity, &ho)) return 0;
    if (!get_num(c, g, SDK_F_BRIGHTNESS, &bright, &have)) return 0;
    if (have && bright > 1) note(c, "particles' 'brightness' above 1 is drawn at 1 by the game; give the image's glow instead ('%s')", SDK_W(F_GLOW));
    if (!get_range(c, g, SDK_F_ANGLE, &ang0, &ang1, &have)) return 0;
    simulate = drag > 0 || bounce || (hs && sp0 != sp1);
    (void)hc;

    /* ---- the stage ---- */
    {
        double lifemax = life1 > life0 ? life1 : life0, speedmax = sp1 > sp0 ? sp1 : sp0;
        double reach = (from_r[0] > from_r[2] ? from_r[0] : from_r[2]) + speedmax * lifemax + fabs(fall) * lifemax * lifemax + sz_peak + 8;
        double bmin[3] = { -reach, -reach, -reach }, bmax[3] = { reach, reach, reach };
        double mean = (sp0 + sp1) / 2;
        int q;
        put_open(&s, 4, "bounds");
        put_open(&s, 5, "b");
        put_vec(&s, 6, "b[0]", "xyz", bmin, 3);
        put_vec(&s, 6, "b[1]", "xyz", bmax, 3);
        put_close(&s, 5);
        put_close(&s, 4);
        put_str(&s, 4, "stageName", "particles");
        put_open(&s, 4, "systemProperties");
        put_str(&s, 5, "material", mtr);
        put_num(&s, 5, "totalParticles", floor(count + 0.5));
        put_num(&s, 5, "cycles", cycles);
        put_range(&s, 5, "particleLife", life0, life1, NULL);
        put_num(&s, 5, "spawnBunching", burst ? 0 : 1);
        put_close(&s, 4);
        if (dist) {
            put_open(&s, 4, "distribution");
            put_str(&s, 5, "type", dist);
            put_open(&s, 5, "size");
            for (q = 0; q < 3; q++) {
                char k[16];
                snprintf(k, sizeof k, "size[%d]", q);
                put_range(&s, 6, k, from_r[q], from_r[q], NULL);
            }
            put_close(&s, 5);
            put_close(&s, 4);
        }
        put_open(&s, 4, "direction");
        if (outward) put_str(&s, 5, "type", "PDIR_OUTWARD");
        else {
            put_open(&s, 5, "parms");
            put_num(&s, 6, "parms[0]", spread);
            put_close(&s, 5);
        }
        put_close(&s, 4);
        if (hs && mean != 0) {
            /* 2016 multiplies the direction's x, y, z by speed[0], [1], [2]: one speed on all three; a range per
             * particle is Snapmap+'s launch speed factor (particlemotion.h) */
            put_open(&s, 4, "speed");
            put_open(&s, 5, "speed");
            for (q = 0; q < 3; q++) {
                char k[16];
                snprintf(k, sizeof k, "speed[%d]", q);
                put_range(&s, 6, k, mean, mean, NULL);
            }
            put_close(&s, 5);
            put_close(&s, 4);
        }
        if (fall && !simulate) {
            /* 2016's origin function adds -gravity x age^2 (0x1419B8080): half the acceleration */
            put_open(&s, 4, "gravity");
            put_range(&s, 5, "gravity", fall / 2, fall / 2, NULL);
            put_word(&s, 5, "world", "true");
            put_close(&s, 4);
        }
        /* colour and opacity: baseColor over the life, fading from and to its own colour at no opacity over the
         * fade fractions (2016 blends fadeColor -> baseColor over fadeInFraction and back over fadeOutFraction) */
        put_open(&s, 4, "colorAttributes");
        put_open(&s, 5, "baseColor");
        for (q = 0; q < 3; q++) {
            char k[20];
            snprintf(k, sizeof k, "baseColor[%d]", q);
            if (!life_parm(c, &s, 6, k, &colour, q, 1, 0, 0, 0, tables, &ntables)) { free(s.p); return 0; }
        }
        put_range(&s, 6, "baseColor[3]", opacity, opacity, "PARTICLE_CALC_GENERIC");
        put_close(&s, 5);
        {
            double fc[4];
            fc[0] = colour.form == SDK_VAL_CONST ? colour.c[0] : 0;
            fc[1] = colour.form == SDK_VAL_CONST ? colour.c[1] : 0;
            fc[2] = colour.form == SDK_VAL_CONST ? colour.c[2] : 0;
            fc[3] = 0;
            put_vec(&s, 5, "fadeColor", "xyzw", fc, 4);
        }
        put_num(&s, 5, "fadeInFraction", fin);
        put_num(&s, 5, "fadeOutFraction", fout);
        if (bright != 1) put_range(&s, 5, "brightness", bright, bright, NULL);
        put_close(&s, 4);
        /* size over the life */
        put_open(&s, 4, "size");
        put_open(&s, 5, "size");
        if (size.form == -1) put_range(&s, 6, "size[0]", size.low[0], size.high[0], "PARTICLE_CALC_GENERIC");
        else if (!life_parm(c, &s, 6, "size[0]", &size, 0, 1, 0, 0, 0, tables, &ntables)) { free(s.p); return 0; }
        put_close(&s, 5);
        put_close(&s, 4);
        if (ang0 || ang1) {
            put_open(&s, 4, "initialRotation");
            put_open(&s, 5, "initialAngle");
            put_range(&s, 6, "initialAngle[0]", ang0, ang1, NULL);
            put_close(&s, 5);
            put_close(&s, 4);
        }
        if (frames[0] * frames[1] > 1) {
            put_open(&s, 4, "texAnimation");
            put_str(&s, 5, "type", "PANIM_TYPE_CYLE_RATE");
            put_num(&s, 5, "numColumns", frames[0]);
            put_num(&s, 5, "numRows", frames[1]);
            put_range(&s, 5, "rate", framerate, framerate, NULL);
            put_close(&s, 4);
        }
        if ((it = fld(g, SDK_F_FACES)) != NULL) {
            if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_CAMERA))) {}
            else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_MOTION))) {
                /* an aimed quad is as long as aspectRatio x segmentLength (0x1419B33C0); 0 draws nothing */
                put_open(&s, 4, "orientation");
                put_str(&s, 5, "type", "POR_AIMED");
                put_num(&s, 5, "segmentLength", sz_peak > 0 ? sz_peak : 4);
                put_close(&s, 4);
            } else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_GROUND))) {
                put_open(&s, 4, "orientation");
                put_str(&s, 5, "type", "POR_Z");
                put_word(&s, 5, "world", "true");
                put_close(&s, 4);
            } else if (it->count == 2 && sdk_is(&it->atoms[1], SDK_W(V_FREE))) {
                put_open(&s, 4, "orientation");
                put_str(&s, 5, "type", "POR_XYZ");
                put_close(&s, 4);
            } else {
                FAIL(c, it->line, "'%s' is %s, %s, %s or %s.", SDK_W(F_FACES), SDK_W(V_CAMERA), SDK_W(V_MOTION),
                     SDK_W(V_GROUND), SDK_W(V_FREE));
            }
        }
        if (simulate) {
            /* Snapmap+'s simulated motion (particlemotion.h): gravity in the acceleration, drag toward still air, a
             * speed per particle, collision with the surfaces under and behind the emitter */
            double acc[3] = { 0, 0, -fall }, be[3] = { 0, 0, 0 }, fcg[4] = { 0, 0, 0, 0 };
            put_open(&s, 4, "vectorFieldGPU");
            put_word(&s, 5, "repeat", "true");
            if (drag > 0) put_num(&s, 5, "tightness", drag);
            put_close(&s, 4);
            if (fall) {
                put_open(&s, 4, "accelerationGPU");
                put_vec(&s, 5, "acceleration", "xyz", acc, 3);
                put_close(&s, 4);
            }
            if (hs && sp0 != sp1 && mean != 0) {
                be[1] = sp0 / mean;
                be[2] = sp1 / mean;
                put_open(&s, 4, "systemPropertiesGPU");
                put_vec(&s, 5, "boundsExpansion", "xyz", be, 3);
                put_close(&s, 4);
            }
            if (bounce) {
                fcg[0] = 2;
                put_open(&s, 4, "colorGPU");
                put_vec(&s, 5, "fadeColor", "xyzw", fcg, 4);
                put_close(&s, 4);
                put_open(&s, 4, "physicsGPU");
                put_num(&s, 5, "elasticity", elastic);
                put_num(&s, 5, "restitution", stick ? 0 : 0.5);
                put_close(&s, 4);
            }
        }
        if (ntables) {
            put_open(&s, 4, "tableDecls");
            put_num(&s, 5, "num", ntables);
            for (q = 0; q < ntables; q++) {
                char k[16];
                snprintf(k, sizeof k, "item[%d]", q);
                put_str(&s, 5, k, tables[q]);
            }
            put_close(&s, 4);
        }
        /* GPU stages are the product's choice (SH_EFFECT_GPU_COUNT); the stage is drawn on the CPU */
        if (count >= SH_EFFECT_GPU_COUNT)
            note(c, "particles: %g particles drawn on the CPU (the GPU stages are not yet chosen by the product)", count);
        if (s.oom) { free(s.p); c->oom = 1; return 0; }
        /* the decl */
        sdk_printf(&decl, "{\n\tedit = {\n\t\tstages = {\n\t\t\tnum = 1;\n\t\t\titem[0] = {\n");
        sh_text_append(&decl, s.p, s.n);
        sdk_printf(&decl, "\t\t\t}\n\t\t}\n");
        {
            sdk_buf tail = { 0 };
            put_open(&tail, 2, "bounds");
            put_open(&tail, 3, "b");
            put_vec(&tail, 4, "b[0]", "xyz", bmin, 3);
            put_vec(&tail, 4, "b[1]", "xyz", bmax, 3);
            put_close(&tail, 3);
            put_close(&tail, 2);
            put_num(&tail, 2, "maxSystemDuration", (lifemax + (burst ? 0 : lifemax)) * 1000 * (cycles ? cycles : 1));
            if (tail.p) sh_text_append(&decl, tail.p, tail.n);
            free(tail.p);
        }
        sdk_printf(&decl, "\t}\n}\n");
        free(s.p);
        if (decl.oom) { free(decl.p); c->oom = 1; return 0; }
        {
            const char *name = sdk_register("particle", decl.p);
            free(decl.p);
            if (!name) { c->oom = 1; return 0; }
            put_open(&e->parms, 4, "particleParms");
            put_str(&e->parms, 5, "declPrt", name);
            put_close(&e->parms, 4);
        }
    }
    return 1;
}

static const sdk_word_id ITEMS[] = { SDK_IT_LIGHT, SDK_IT_DECAL, SDK_IT_SHAKE, SDK_IT_FLASH, SDK_IT_SCREEN,
                                     SDK_IT_BLUR, SDK_IT_FOCUS, SDK_IT_FLARE, SDK_IT_TRAIL, SDK_IT_MODEL, SDK_IT_SOUND,
                                     SDK_IT_GLOW, SDK_IT_ENVIRONMENT, SDK_IT_WIND, SDK_IT_PARTICLES };
#define NITEMS (sizeof ITEMS / sizeof ITEMS[0])

int sh_effect_is(const char *text, size_t len)
{
    char kind[32];
    return sdk_first_type(text, len, kind, sizeof kind) && !strcmp(kind, SDK_W(KIND_EFFECT));
}
int sh_effect_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                      size_t why_len, char **notes, char **needs)
{
    sdk_doc d;
    ectx c;
    sdk_buf b = { 0 };
    ev evs[64];
    int nev = 0, r, k;
    size_t i;
    double lasts = -1;
    *out = NULL;
    if (out_len) *out_len = 0;
    if (notes) *notes = NULL;
    if (needs) *needs = NULL;
    if (why && why_len) why[0] = 0;
    if (!sh_effect_is(text, len)) return SH_LOOK_SAME;
    r = sdk_parse(path, text, len, &d, why, why_len);
    if (r < 0) return SH_LOOK_OOM;
    if (r == 0) return SH_LOOK_MISTAKE;
    {
        const sdk_item *t = d.root->count ? &d.root->items[0] : NULL;
        if (!t || t->count != 2 || !sdk_is(&t->atoms[0], SDK_W(TYPE)) || !sdk_is(&t->atoms[1], SDK_W(KIND_EFFECT))) {
            sdk_doc_free(&d);
            return SH_LOOK_SAME;
        }
    }
    memset(&c, 0, sizeof c);
    c.d = &d;
    c.why = why;
    c.why_len = why_len;
    memset(evs, 0, sizeof evs);
    for (i = 1; i < d.root->count; i++) {
        const sdk_item *it = &d.root->items[i];
        char w[64];
        int which = -1;
        size_t q;
        if (!it->count) continue;
        if (it->atoms[0].kind != SDK_A_WORD) { sdk_say(why, why_len, &d, it->line, "each line of an effect starts with an item or a field."); goto mistake; }
        sdk_text(&it->atoms[0], w, sizeof w);
        if (sdk_is(&it->atoms[0], SDK_W(F_LASTS))) {
            double x[1];
            if (it->count != 2 || sdk_numbers(&it->atoms[1], x, 1) != 1) { sdk_say(why, why_len, &d, it->line, "'%s' needs one number, as in '%s 2'.", w, w); goto mistake; }
            lasts = x[0];
            continue;
        }
        if (sdk_is(&it->atoms[0], SDK_W(TYPE))) { sdk_say(why, why_len, &d, it->line, "'%s' is written once, as the first line.", w); goto mistake; }
        for (q = 0; q < NITEMS; q++) if (sdk_is(&it->atoms[0], sdk_word(ITEMS[q]))) which = (int)ITEMS[q];
        if (which < 0) {
            const char *names[NITEMS + 1], *near;
            for (q = 0; q < NITEMS; q++) names[q] = sdk_word(ITEMS[q]);
            names[NITEMS] = SDK_W(F_LASTS);
            near = sdk_nearest(it->atoms[0].p, it->atoms[0].n, names, NITEMS + 1);
            if (near) sdk_say(why, why_len, &d, it->line, "'%s' is not an item of an effect; the nearest is '%s'.", w, near);
            else sdk_say(why, why_len, &d, it->line, "'%s' is not an item of an effect; write light, decal, shake, flash, "
                         "screen, blur, focus, flare, trail, model, sound, glow, environment, wind or particles.", w);
            goto mistake;
        }
        if (it->count != 2 || it->atoms[1].kind != SDK_A_GROUP) {
            sdk_say(why, why_len, &d, it->line, "'%s' takes its fields in braces, as in '%s { ... }'.", w, w);
            goto mistake;
        }
        if (nev >= 60) {
            note(&c, "an effect plays at most 60 items; the rest are left out");
            break;
        }
        {
            ev *e = &evs[nev];
            const sdk_group *g = it->atoms[1].group;
            int ok = 0, envn = 0;
            sdk_env_parm envp[SDK_ENV_MAX];
            snprintf(e->name, sizeof e->name, "%s%d", w, nev);
            switch (which) {
            case SDK_IT_LIGHT: ok = item_light(&c, g, e); break;
            case SDK_IT_DECAL: ok = item_decal(&c, g, e); break;
            case SDK_IT_SHAKE: ok = item_shake(&c, g, e); break;
            case SDK_IT_FLASH: ok = item_flash(&c, g, e); break;
            case SDK_IT_SCREEN: ok = item_screen(&c, g, e); break;
            case SDK_IT_BLUR: ok = item_blur(&c, g, e); break;
            case SDK_IT_FOCUS: ok = item_focus(&c, g, e); break;
            case SDK_IT_FLARE: ok = item_named(&c, g, e, "a flare", "FX_FLARE", "flareParms", "declFlare", -1); break;
            case SDK_IT_TRAIL: ok = item_named(&c, g, e, "a trail", "FX_RIBBON", "ribbonParms", "declRibbon", -1); break;
            case SDK_IT_MODEL: ok = item_model(&c, g, e); break;
            case SDK_IT_SOUND: ok = item_sound(&c, g, e); break;
            case SDK_IT_GLOW: ok = item_glow(&c, g, e); break;
            case SDK_IT_WIND: ok = item_wind(&c, g, e); break;
            case SDK_IT_PARTICLES: ok = item_particles(&c, g, e); break;
            case SDK_IT_ENVIRONMENT:
                ok = item_environment(&c, g, e, envp, &envn);
                if (ok && envn) {
                    /* 16 values per item: the rest on copies of it */
                    int first = 0;
                    ev base = *e;
                    memset(&base.parms, 0, sizeof base.parms);
                    while (first < envn && nev < 60) {
                        int cnt = envn - first < 16 ? envn - first : 16;
                        ev *x = &evs[nev];
                        if (first) { *x = base; snprintf(x->name, sizeof x->name, "%s%d", w, nev); }
                        env_parms_block(&x->parms, envp + first, cnt);
                        first += cnt;
                        if (first < envn) nev++;
                    }
                }
                break;
            }
            if (c.oom) goto oom;
            if (!ok) goto mistake;
            nev++;
        }
    }
    if (!nev) { sdk_say(why, why_len, &d, d.root->line, "an effect needs at least one item (light, decal, shake, ...)."); goto mistake; }
    if (lasts >= 0)
        for (k = 0; k < nev; k++) if (!evs[k].has_dur && strcmp(evs[k].type, "FX_SOUND") && strcmp(evs[k].type, "FX_PARTICLE")) { evs[k].has_dur = 1; evs[k].dur = lasts; }
    /* the first line marks an SDK effect for the calls that play it (sdk_runtime.c) */
    sdk_printf(&b, SH_EFFECT_MARK "\n{\n\tedit = {\n\t\teditEvents = {\n\t\t\tnum = %d;\n", nev);
    for (k = 0; k < nev; k++) write_ev(&b, k, &evs[k]);
    sdk_printf(&b, "\t\t}\n\t}\n}\n");
    for (k = 0; k < nev; k++) free(evs[k].parms.p);
    if (b.oom || c.notes.oom) { free(b.p); free(c.notes.p); sdk_doc_free(&d); return SH_LOOK_OOM; }
    if (needs) {
        char *name = sh_sdk_decl_name(path), *em = name ? sh_modprog_effect_emitter(name) : NULL;
        if (em) *needs = sh_textf("entityDef %s\n", em);
        free(name);
        free(em);
    }
    if (notes) *notes = c.notes.p; else free(c.notes.p);
    sdk_doc_free(&d);
    *out = b.p;
    if (out_len) *out_len = b.n;
    return SH_LOOK_CHANGED;
mistake:
    for (k = 0; k < 64; k++) free(evs[k].parms.p);
    free(b.p);
    free(c.notes.p);
    sdk_doc_free(&d);
    return SH_LOOK_MISTAKE;
oom:
    for (k = 0; k < 64; k++) free(evs[k].parms.p);
    free(b.p);
    free(c.notes.p);
    sdk_doc_free(&d);
    return SH_LOOK_OOM;
}
