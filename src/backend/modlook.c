/* modlook.c -- the Graphics SDK's look compiler (modlook.h, docs/graphics-sdk.md). Pure text work; the only effect is
 * the images it registers with modprog.c. */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modlook.h"
#include "modprog.h"
#include "text.h"

typedef struct lk_buf { char *p; size_t n, cap; int oom; } lk_buf;

static void lk_put(lk_buf *b, const char *d, size_t n)
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
    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void lk_puts(lk_buf *b, const char *s) { lk_put(b, s, strlen(s)); }
static void lk_printf(lk_buf *b, const char *fmt, ...)
{
    char t[1200];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    lk_put(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}

static int lk_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static char lk_lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* `a` (n chars) equals `b`, ignoring case. */
static int lk_ieq(const char *a, size_t n, const char *b)
{
    size_t i;
    for (i = 0; i < n; i++) if (!b[i] || lk_lc(a[i]) != lk_lc(b[i])) return 0;
    return b[n] == 0;
}

/* The shortest text that reads back as the same float. */
static void lk_fmt(char *out, size_t cap, float v)
{
    int p;
    for (p = 1; p <= 9; p++) {
        snprintf(out, cap, "%.*g", p, (double)v);
        if (strtof(out, NULL) == v) return;
    }
}

static const char *lk_base(const char *path)
{
    const char *b = path ? path : "", *p;
    for (p = b; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* The edit distance of two names (lower case, at most 40 characters each). */
static unsigned lk_dist(const char *a, size_t an, const char *b)
{
    unsigned d[41], i, j, bn = (unsigned)strlen(b);
    if (an > 40 || bn > 40) return 99;
    for (j = 0; j <= bn; j++) d[j] = j;
    for (i = 1; i <= an; i++) {
        unsigned prev = d[0];
        d[0] = i;
        for (j = 1; j <= bn; j++) {
            unsigned cur = d[j], cost = lk_lc(a[i - 1]) == b[j - 1] ? 0 : 1, m = prev + cost;
            if (d[j] + 1 < m) m = d[j] + 1;
            if (d[j - 1] + 1 < m) m = d[j - 1] + 1;
            d[j] = m;
            prev = cur;
        }
    }
    return d[bn];
}

typedef struct lk_ln {
    const char *a;                 /* the line, without its terminator */
    size_t an, en;                 /* its length; its terminator's length */
    int key;                       /* a key line of the outermost block */
    size_t ks, kn;                 /* the key */
    size_t v0, vn;                 /* the value: comment cut, trimmed */
    int skip;                      /* a look field: not copied */
} lk_ln;

typedef struct lk_doc {
    const char *text;
    lk_ln *l;
    size_t n;
    const char *eol;
} lk_doc;

static void lk_doc_free(lk_doc *d) { free(d->l); memset(d, 0, sizeof *d); }

static int lk_split(lk_doc *d, const char *text, size_t len)
{
    size_t count = 1, i, at = 0;
    int depth = 0, block = 0;
    memset(d, 0, sizeof *d);
    d->text = text;
    d->eol = "\n";
    for (i = 0; i + 1 < len; i++) if (text[i] == '\r' && text[i + 1] == '\n') { d->eol = "\r\n"; break; }
    for (i = 0; i < len; i++) if (text[i] == '\n') count++;
    if (!(d->l = (lk_ln *)calloc(count, sizeof *d->l))) return 0;
    for (;;) {
        lk_ln *x = &d->l[d->n++];
        size_t e = at, k = 0, p, cut;
        int quoted = 0, first_depth = depth;
        while (e < len && text[e] != '\n') e++;
        x->a = text + at;
        x->an = e - at;
        x->en = e < len ? 1 : 0;
        if (x->an && x->a[x->an - 1] == '\r') { x->an--; x->en = e < len ? 2 : 1; }
        /* braces outside quotes and comments; where the line's own comment starts */
        cut = x->an;
        for (p = 0; p < x->an; p++) {
            char c = x->a[p];
            if (block) { if (c == '*' && p + 1 < x->an && x->a[p + 1] == '/') { block = 0; p++; } continue; }
            if (quoted) { if (c == '"') quoted = 0; continue; }
            if (c == '"') quoted = 1;
            else if (c == '/' && p + 1 < x->an && x->a[p + 1] == '/') { cut = p; break; }
            else if (c == '/' && p + 1 < x->an && x->a[p + 1] == '*') { block = 1; p++; if (cut == x->an) { /* keep */ } }
            else if (c == '{') depth++;
            else if (c == '}') depth--;
        }
        while (k < x->an && lk_blank(x->a[k])) k++;
        if (first_depth == 1 && k < x->an && x->a[k] != '{' && x->a[k] != '}' && x->a[k] != '/') {
            size_t end = cut;
            x->key = 1;
            x->ks = k;
            while (k < cut && !lk_blank(x->a[k])) k++;
            x->kn = k - x->ks;
            while (k < cut && lk_blank(x->a[k])) k++;
            x->v0 = k;
            while (end > k && lk_blank(x->a[end - 1])) end--;
            x->vn = end > k ? end - k : 0;
        }
        if (e >= len) break;
        at = e + 1;
    }
    return 1;
}

enum { K_NONE = 0, K_NUM, K_VEC, K_IMG, K_BAD };
typedef struct lk_val {
    int kind, n;
    float v[8];
    const char *s;                 /* an image path (K_IMG), unquoted, into the text */
    size_t sn;
} lk_val;

static int lk_number(const char *s, size_t n, float *out)
{
    char t[64], *e;
    double d;
    if (!n || n >= sizeof t) return 0;
    memcpy(t, s, n);
    t[n] = 0;
    d = strtod(t, &e);
    if (e == t || *e || !isfinite(d)) return 0;
    *out = (float)d;
    return 1;
}

static void lk_parse(const char *s, size_t n, lk_val *o)
{
    memset(o, 0, sizeof *o);
    o->kind = K_BAD;
    while (n && lk_blank(*s)) { s++; n--; }
    while (n && lk_blank(s[n - 1])) n--;
    if (!n) return;
    if (*s == '{') {
        const char *p = s + 1, *end = s + n - 1;
        if (s[n - 1] != '}') return;
        for (;;) {
            const char *q;
            while (p < end && (lk_blank(*p) || *p == ',')) p++;
            if (p >= end) break;
            q = p;
            while (q < end && !lk_blank(*q) && *q != ',') q++;
            if (o->n >= 8 || !lk_number(p, (size_t)(q - p), &o->v[o->n])) return;
            o->n++;
            p = q;
        }
        if (!o->n) return;
        o->kind = K_VEC;
        return;
    }
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s++;
        n -= 2;
        if (!n) return;
        o->kind = K_IMG;
        o->s = s;
        o->sn = n;
        return;
    }
    {
        size_t i;
        for (i = 0; i < n; i++) if (lk_blank(s[i]) || s[i] == '{' || s[i] == '}' || s[i] == '"') return;
    }
    if (lk_number(s, n, &o->v[0])) { o->kind = K_NUM; o->n = 1; return; }
    o->kind = K_IMG;
    o->s = s;
    o->sn = n;
}

enum { T_NUM, T_IMG, T_IMGNUM, T_IMGC3, T_V2, T_V3, T_V4, T_V5, T_V6, T_YESNO, T_LIGHT };

typedef struct lk_field { const char *name; int type; const char *example; } lk_field;

#define IMG_EX "mods/abcdefgh/tiles/pores.tga"

/* Layered: the fields of the whole surface, then the fields each layer has (named layer<i><suffix>). */
enum { G_WOUNDS, G_LAYERMASK, G_BASEHEIGHT, G_BASEHEIGHTBLEND, G_BASETILING, G_BASESCROLL, G_BASEGLOW, G_BURN,
       G_BURNGLOW, G_BURNBLOOD, G_BURNTILING, G_BURNNOISE, G_BURNHEAT, G_COUNT };
static const lk_field LK_G[G_COUNT] = {
    { "wounds", T_YESNO, "yes" }, { "layermask", T_IMG, "mods/abcdefgh/demon/body_mask.tga" },
    { "baseheight", T_IMGNUM, "0.5" }, { "baseheightblend", T_NUM, "1" },
    { "basetiling", T_V2, "{ 2, 2 }" }, { "basescroll", T_V2, "{ 0, 0.1 }" }, { "baseglow", T_V3, "{ 1, 1, 1 }" },
    { "burn", T_IMG, "mods/abcdefgh/tiles/burnt.tga" }, { "burnglow", T_IMG, IMG_EX }, { "burnblood", T_IMG, IMG_EX },
    { "burntiling", T_V2, "{ 4, 4 }" }, { "burnnoise", T_V4, "{ 1, 4, 0.7, 0.4 }" }, { "burnheat", T_V3, "{ 1, 0.07, 0 }" },
};
enum { L_COLOUR, L_NORMAL, L_SMOOTHNESS, L_HEIGHT, L_SPECULAR, L_GLOW, L_REPEAT, L_TILING, L_SCROLL, L_COVERAGE,
       L_HEIGHTBLEND, L_SOFTNESS, L_WEIGHTS, L_COUNT };
static const lk_field LK_L[L_COUNT] = {
    { "", T_IMGC3, "{ 0.35, 0.05, 0.03 }" }, { "normal", T_IMG, IMG_EX }, { "smoothness", T_IMGNUM, "0.5" },
    { "height", T_IMGNUM, "0.5" }, { "specular", T_IMGC3, "{ 0.2, 0.2, 0.2 }" }, { "glow", T_IMGC3, "{ 1, 0.3, 0 }" },
    { "repeat", T_NUM, "3" }, { "tiling", T_V2, "{ 2, 2 }" }, { "scroll", T_V2, "{ 0, 0.1 }" }, { "coverage", T_NUM, "1" },
    { "heightblend", T_NUM, "1" }, { "softness", T_NUM, "0.25" }, { "weights", T_V5, "{ 1, 1, 1, 1, 1 }" },
};

/* Particle. */
enum { P_LIGHTING, P_GLOW, P_GLOWMASK, P_GLOWMASKCOLOUR, P_GLOWMASKSTRENGTH, P_GRADIENT, P_NEARFADE, P_SOFTDISTANCE,
       P_DEPTHOFFSET, P_LITSCALE, P_FARFADE, P_GLOWMASKCOLOUR2, P_COUNT };
static const lk_field LK_P[P_COUNT] = {
    { "lighting", T_LIGHT, "lit" }, { "glow", T_NUM, "4" }, { "glowmask", T_IMG, "mods/abcdefgh/fx/embers_mask.tga" },
    { "glowmaskcolour", T_V3, "{ 1, 0.4, 0.1 }" }, { "glowmaskstrength", T_NUM, "1" },
    { "gradient", T_V6, "{ 1, 0.5, 0, 1, 0.1, 0 }" }, { "nearfade", T_V2, "{ 40, 8 }" }, { "softdistance", T_NUM, "5" },
    { "depthoffset", T_NUM, "1" }, { "litscale", T_NUM, "1" },
    { "farfade", T_V2, "{ 128, 256 }" }, { "glowmaskcolour2", T_V3, "{ 1, 0.8, 0.3 }" },
};

/* Both looks' own field names, for telling a field of the other look. */
static int lk_is_layered_name(const char *k, size_t n)
{
    size_t i;
    if (n > 5 && !_strnicmp(k, "layer", 5) && k[5] >= '0' && k[5] <= '9') return 1;
    for (i = 0; i < G_COUNT; i++) if (lk_ieq(k, n, LK_G[i].name)) return 1;
    return 0;
}
static int lk_is_particle_name(const char *k, size_t n)
{
    size_t i;
    for (i = 0; i < P_COUNT; i++) if (lk_ieq(k, n, LK_P[i].name)) return 1;
    return 0;
}

static const char *lk_count_word(int t)
{
    switch (t) {
    case T_V2: return "two";
    case T_V3: return "three";
    case T_V4: return "four";
    case T_V5: return "five";
    default: return "six";
    }
}
static int lk_count_of(int t)
{
    switch (t) { case T_V2: return 2; case T_V3: return 3; case T_V4: return 4; case T_V5: return 5; case T_V6: return 6; default: return 1; }
}

/* 1 when the value has the shape the field's type asks for. */
static int lk_fits(int type, const lk_val *v)
{
    switch (type) {
    case T_NUM: return v->kind == K_NUM;
    case T_IMG: return v->kind == K_IMG;
    case T_IMGNUM: return v->kind == K_NUM || v->kind == K_IMG;
    case T_IMGC3: return v->kind == K_IMG || (v->kind == K_VEC && v->n == 3);
    case T_V2: case T_V3: case T_V4: case T_V5: case T_V6: return v->kind == K_VEC && v->n == lk_count_of(type);
    default: return v->kind == K_IMG || v->kind == K_NUM;
    }
}

typedef struct lk_ctx {
    const char *base;
    char *why;
    size_t why_len;
    int mistake;
    int oom;
    lk_buf notes;
} lk_ctx;

static void lk_fail(lk_ctx *c, size_t line, const char *fmt, ...)
{
    char t[600];
    va_list ap;
    if (c->mistake) return;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    c->mistake = 1;
    if (c->why && c->why_len) snprintf(c->why, c->why_len, "%s line %u: %s", c->base, (unsigned)line + 1, t);
}

static void lk_shape(lk_ctx *c, size_t line, const char *name, const lk_field *f)
{
    switch (f->type) {
    case T_NUM: lk_fail(c, line, "'%s' needs one number, as in '%s %s'.", name, name, f->example); break;
    case T_IMG: lk_fail(c, line, "'%s' needs an image path, as in '%s %s'.", name, name, f->example); break;
    case T_IMGNUM: lk_fail(c, line, "'%s' needs an image path or one number, as in '%s %s'.", name, name, f->example); break;
    case T_IMGC3:
        lk_fail(c, line, "'%s' needs an image path or three numbers in braces, as in '%s %s'.", name, name, f->example);
        break;
    default:
        lk_fail(c, line, "'%s' needs %s numbers in braces, as in '%s %s'.", name, lk_count_word(f->type), name, f->example);
        break;
    }
}

/* The raw value as a short word for a sentence. */
static void lk_word(const lk_ln *x, char *out, size_t cap)
{
    size_t n = x->vn;
    const char *s = x->a + x->v0;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

#define LK_VALID 1
#define LK_PASS 0
#define LK_UNKNOWN (-1)
#define LK_LAYER (-2)
#define LK_OTHER (-3)

typedef struct lk_hit { int layer, fid; char near[48]; } lk_hit;

static void lk_nearest(const char *k, size_t n, int layered, int layer, char *out, size_t cap)
{
    unsigned best = 999, d;
    size_t i;
    char name[48];
    out[0] = 0;
    if (layered) {
        for (i = 0; i < G_COUNT; i++) {
            d = lk_dist(k, n, LK_G[i].name);
            if (d < best) { best = d; snprintf(out, cap, "%s", LK_G[i].name); }
        }
        for (i = 0; i < L_COUNT; i++) {
            int a;
            for (a = 0; a < 4; a++) {
                if (layer >= 1 && layer != a + 1) continue;
                snprintf(name, sizeof name, "layer%d%s", a + 1, LK_L[i].name);
                d = lk_dist(k, n, name);
                if (d < best) { best = d; snprintf(out, cap, "%s", name); }
            }
        }
    } else {
        for (i = 0; i < P_COUNT; i++) {
            d = lk_dist(k, n, LK_P[i].name);
            if (d < best) { best = d; snprintf(out, cap, "%s", LK_P[i].name); }
        }
    }
}

/* Does the key belong to a family of the look's fields? Only such a key is checked against the fields (a near miss is a
 * mistake); every other key is the game's own and passes through, whatever it resembles (woundbloodblend is a render
 * parm, not a misspelt `wounds`). A family is named by a stem, so a misspelling of a family member is still inside it. */
static int lk_close(const char *k, size_t n, int layered)
{
    static const char *const lay[] = { "layerm", "basehe", "basetil", "basescr", "baseglo", "burn" };
    static const char *const par[] = { "glowmas", "nearfa", "softdis", "depthoff", "litsc", "gradi", "lightin" };
    /* the game's own keys that open with a stem */
    static const char *const stock[] = { "depthoffield", "lightingmodel" };
    const char *const *pre = layered ? lay : par;
    size_t i, np = layered ? sizeof lay / sizeof lay[0] : sizeof par / sizeof par[0];
    for (i = 0; i < sizeof stock / sizeof stock[0]; i++)
        if (lk_ieq(k, n, stock[i])) return 0;
    for (i = 0; i < np; i++)
        if (n >= strlen(pre[i]) && !_strnicmp(k, pre[i], strlen(pre[i]))) return 1;
    return layered ? lk_ieq(k, n, "wounds") : lk_ieq(k, n, "glow");
}

static int lk_classify(const char *k, size_t n, int layered, lk_hit *h)
{
    size_t i;
    memset(h, 0, sizeof *h);
    h->layer = -1;
    if (layered) {
        if (n > 5 && !_strnicmp(k, "layer", 5) && k[5] >= '0' && k[5] <= '9') {
            size_t p = 5;
            unsigned num = 0;
            while (p < n && k[p] >= '0' && k[p] <= '9' && num < 1000) num = num * 10 + (unsigned)(k[p++] - '0');
            if (num < 1 || num > 4) return LK_LAYER;
            h->layer = (int)num - 1;
            for (i = 0; i < L_COUNT; i++)
                if (lk_ieq(k + p, n - p, LK_L[i].name)) { h->fid = (int)i; return LK_VALID; }
            lk_nearest(k, n, 1, (int)num, h->near, sizeof h->near);
            return LK_UNKNOWN;
        }
        for (i = 0; i < G_COUNT; i++)
            if (lk_ieq(k, n, LK_G[i].name)) { h->fid = (int)i; return LK_VALID; }
        if (lk_is_particle_name(k, n)) return LK_OTHER;
        if (lk_close(k, n, 1)) { lk_nearest(k, n, 1, 0, h->near, sizeof h->near); return LK_UNKNOWN; }
        return LK_PASS;
    }
    for (i = 0; i < P_COUNT; i++)
        if (lk_ieq(k, n, LK_P[i].name)) { h->fid = (int)i; return LK_VALID; }
    if (lk_is_layered_name(k, n)) return LK_OTHER;
    if (lk_close(k, n, 0)) { lk_nearest(k, n, 0, 0, h->near, sizeof h->near); return LK_UNKNOWN; }
    return LK_PASS;
}

typedef struct lk_set { lk_val v; int has; size_t line; } lk_set;

/* Reads every key of a look into `g` (layered globals), `l` (layer fields) or `p` (particle). 1 when all fields read. */
static int lk_read(lk_ctx *c, lk_doc *d, size_t first, int layered, lk_set *g, lk_set l[4][L_COUNT], lk_set *p)
{
    size_t i;
    for (i = 0; i < d->n; i++) {
        lk_ln *x = &d->l[i];
        const char *k;
        lk_hit h;
        int cl;
        lk_set *dst;
        const lk_field *f;
        char name[64], word[64];
        if (!x->key || i == first) continue;
        k = x->a + x->ks;
        cl = lk_classify(k, x->kn, layered, &h);
        if (cl == LK_PASS) continue;
        if (x->kn >= sizeof name) { lk_fail(c, i, "'%.*s' is not a field of a %s material.", 40, k, layered ? "layered" : "particle"); return 0; }
        memcpy(name, k, x->kn);
        name[x->kn] = 0;
        if (cl == LK_LAYER) {
            lk_fail(c, i, "'%s' is not a field of a layered material; a surface has layers 1 to 4.", name);
            return 0;
        }
        if (cl == LK_OTHER) {
            lk_fail(c, i, "'%s' is not a field of a %s material; it belongs to a %s material.", name, layered ? "layered" : "particle",
                    layered ? "particle" : "layered");
            return 0;
        }
        if (cl == LK_UNKNOWN) {
            lk_fail(c, i, "'%s' is not a field of a %s material; the nearest is '%s'.", name, layered ? "layered" : "particle", h.near);
            return 0;
        }
        if (layered) {
            if (h.layer >= 0) { dst = &l[h.layer][h.fid]; f = &LK_L[h.fid]; }
            else { dst = &g[h.fid]; f = &LK_G[h.fid]; }
        } else {
            dst = &p[h.fid];
            f = &LK_P[h.fid];
        }
        lk_parse(x->a + x->v0, x->vn, &dst->v);
        if (f->type == T_YESNO) {
            lk_word(x, word, sizeof word);
            if (!lk_ieq(word, strlen(word), "yes") && !lk_ieq(word, strlen(word), "no")) {
                lk_fail(c, i, "'%s' is '%s'; write yes or no.", name, word);
                return 0;
            }
        } else if (f->type == T_LIGHT) {
            lk_word(x, word, sizeof word);
            if (!lk_ieq(word, strlen(word), "unlit") && !lk_ieq(word, strlen(word), "lit") &&
                !lk_ieq(word, strlen(word), "explosion")) {
                lk_fail(c, i, "'%s' is '%s'; write unlit, lit or explosion.", name, word);
                return 0;
            }
        } else if (!lk_fits(f->type, &dst->v)) {
            lk_shape(c, i, name, f);
            return 0;
        }
        dst->has = 1;
        dst->line = i;
        x->skip = 1;
    }
    return 1;
}

/* The material's name for a registered image path: generated/image/<name>.bimage is <name>.tga. */
static char *lk_image_name(const char *path)
{
    size_t pn = strlen(path), a = strlen("generated/image/"), e = strlen(".bimage");
    if (pn <= a + e) return NULL;
    return sh_textf("%.*s.tga", (int)(pn - a - e), path + a);
}

static void lk_key(lk_buf *b, const char *eol, const char *key, const char *value)
{
    lk_printf(b, "%s\t%s%s", key, value, eol);
}
static void lk_key_q(lk_buf *b, const char *eol, const char *key, const char *s, size_t n)
{
    lk_printf(b, "%s\t\"%.*s\"%s", key, (int)n, s, eol);
}
static void lk_key_vec(lk_buf *b, const char *eol, const char *key, const float v[4])
{
    char c[4][40];
    int k;
    for (k = 0; k < 4; k++) lk_fmt(c[k], sizeof c[k], v[k]);
    lk_printf(b, "%s\t{ %s, %s, %s, %s }%s", key, c[0], c[1], c[2], c[3], eol);
}

/* Writes the text: every line copied except the look's own field lines, the `look` line (replaced by `gen`) and the
 * key lines named in `drop` (the keys `gen` writes). */
static char *lk_assemble(const lk_doc *d, size_t first, const char *gen, const char *const *drop, size_t ndrop,
                         size_t *out_len)
{
    lk_buf b = { 0 };
    size_t i, k;
    for (i = 0; i < d->n; i++) {
        const lk_ln *x = &d->l[i];
        int skip = x->skip;
        if (i == first) { lk_puts(&b, gen); continue; }
        if (x->key)
            for (k = 0; k < ndrop && !skip; k++) if (lk_ieq(x->a + x->ks, x->kn, drop[k])) skip = 1;
        if (skip) continue;
        lk_put(&b, x->a, x->an);
        lk_put(&b, x->a + x->an, x->en);
    }
    if (b.oom) { free(b.p); return NULL; }
    if (!b.p && !(b.p = (char *)calloc(1, 1))) return NULL;
    *out_len = b.n;
    return b.p;
}

static int lk_has_key(const lk_doc *d, const char *key)
{
    size_t i;
    for (i = 0; i < d->n; i++) if (d->l[i].key && lk_ieq(d->l[i].a + d->l[i].ks, d->l[i].kn, key)) return 1;
    return 0;
}

/* A registered constant image of a colour: its material name (malloc'd), or NULL. */
static char *lk_const(float r, float g, float b, float a)
{
    float c[4] = { r, g, b, a };
    const char *path = sh_modprog_register_const_image(c);
    return path ? lk_image_name(path) : NULL;
}

#define LK_SLOTS 2

typedef struct lk_pool {
    char name[LK_SLOTS][520];
    int used;
} lk_pool;

/* The next free shared texture place for an image: 1 or 2, or 0 when both are taken. */
static int lk_take(lk_pool *p, const lk_val *v)
{
    if (p->used >= LK_SLOTS || v->sn >= sizeof p->name[0]) return 0;
    memcpy(p->name[p->used], v->s, v->sn);
    p->name[p->used][v->sn] = 0;
    return ++p->used;
}

static void lk_note(lk_ctx *c, const char *field, const char *drawn)
{
    lk_printf(&c->notes, "%s: '%s' is drawn as %s: the surface has more images for the shared places than fit.\n", c->base,
              field, drawn);
}

static int lk_layered(lk_ctx *c, lk_doc *d, size_t first, char **out, size_t *out_len)
{
    static const char *const platform_drop[] = {
        "ambientprogram", "layerparms", "layerblendmask", "multilayerextramap0", "multilayerextramap1",
        "blendlayer1albedomap", "blendlayer1normalmap", "blendlayer1smoothnessmap", "blendlayer1heightmap",
        "blendlayer2albedomap", "blendlayer2normalmap", "blendlayer2smoothnessmap", "blendlayer2heightmap",
        "blendlayer3albedomap", "blendlayer3normalmap", "blendlayer3smoothnessmap", "blendlayer3heightmap",
        "blendlayer4albedomap", "blendlayer4normalmap", "blendlayer4smoothnessmap", "blendlayer4heightmap",
    };
    static const char *const tex[4] = { "albedo", "normal", "smoothness", "height" };
    static const int tex_field[4] = { L_COLOUR, L_NORMAL, L_SMOOTHNESS, L_HEIGHT };
    /* the vectors of a surface that sets nothing: layered.rp's table, with the SDK's defaults (a layer a look does
     * not write moves nothing: its weights 0) */
    static const float defaults[28][4] = {
        { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 0.25f, 0.25f, 0.25f, 0.25f }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
        { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 1, 0, 0.5f, 0 }, { 1, 1, 0, 0 }, { 1, 1, 1, 0 },
        { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
        { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
        { 4, 4, 0, 0 }, { 1, 4, 0.7f, 0.4f }, { 1, 0.07f, 0, 0 },
    };
    lk_set g[G_COUNT], l[4][L_COUNT];
    float v[28][4];
    lk_pool pool;
    lk_buf gen = { 0 };
    const char *eol = d->eol;
    int wounds = 0, present[4] = { 0, 0, 0, 0 }, any = 0, burn, platform, i, f, k, oom = 0;
    char *textures[4][4];
    char key[64], *res = NULL;
    memset(g, 0, sizeof g);
    memset(l, 0, sizeof l);
    memset(textures, 0, sizeof textures);
    memset(&pool, 0, sizeof pool);
    if (!lk_read(c, d, first, 1, g, l, NULL)) return c->mistake ? SH_LOOK_MISTAKE : SH_LOOK_OOM;
    if (g[G_WOUNDS].has) {
        char w[16];
        lk_word(&d->l[g[G_WOUNDS].line], w, sizeof w);
        wounds = lk_ieq(w, strlen(w), "yes");
    }
    for (i = 0; i < 4; i++)
        for (f = 0; f < L_COUNT; f++) {
            if (!l[i][f].has) continue;
            present[i] = 1;
            if (f <= L_GLOW) any = 1;           /* an image or a colour of the layer (the numbers alone show nothing) */
        }
    burn = g[G_BURN].has;
    /* the game's own skinned program draws a surface no layer can show on and that does not burn */
    platform = (g[G_LAYERMASK].has && any) || burn || g[G_BASEGLOW].has;
    if (!platform) {
        lk_key(&gen, eol, "ambientprogram", wounds ? "skinvmtrwounds" : "skinvmtr");
        res = gen.oom ? NULL : lk_assemble(d, first, gen.p ? gen.p : "", platform_drop, 1, out_len);
        free(gen.p);
        if (!res) return SH_LOOK_OOM;
        *out = res;
        return SH_LOOK_CHANGED;
    }
    for (k = 0; k < 28; k++) memcpy(v[k], defaults[k], sizeof v[k]);
    /* each layer's numbers and images */
    for (i = 0; i < 4; i++) {
        float w[5] = { 1, 1, 1, 1, 1 };
        if (!present[i]) continue;
        if (l[i][L_REPEAT].has) v[0][i] = l[i][L_REPEAT].v.v[0];
        if (l[i][L_COVERAGE].has) v[1][i] = l[i][L_COVERAGE].v.v[0];
        if (l[i][L_HEIGHTBLEND].has) v[2][i] = l[i][L_HEIGHTBLEND].v.v[0];
        if (l[i][L_SOFTNESS].has) v[3][i] = l[i][L_SOFTNESS].v.v[0];
        if (l[i][L_WEIGHTS].has) { for (k = 0; k < 5; k++) w[k] = l[i][L_WEIGHTS].v.v[k]; }
        else if (!l[i][L_COLOUR].has) w[0] = 0.0f;      /* no colour of its own: the base colour stays */
        for (k = 0; k < 5; k++) v[4 + k][i] = w[k];
        if (l[i][L_TILING].has) { v[12 + i][0] = l[i][L_TILING].v.v[0]; v[12 + i][1] = l[i][L_TILING].v.v[1]; }
        if (l[i][L_SCROLL].has) { v[12 + i][2] = l[i][L_SCROLL].v.v[0]; v[12 + i][3] = l[i][L_SCROLL].v.v[1]; }
        for (f = 0; f < 4; f++) {
            const lk_set *s = &l[i][tex_field[f]];
            if (s->has && s->v.kind == K_IMG) textures[i][f] = sh_textf("%.*s", (int)s->v.sn, s->v.s);
            else if (s->has && s->v.kind == K_VEC) textures[i][f] = lk_const(s->v.v[0], s->v.v[1], s->v.v[2], 1.0f);
            else if (s->has) textures[i][f] = lk_const(s->v.v[0], s->v.v[0], s->v.v[0], 1.0f);
            else if (f == 2) textures[i][f] = lk_const(0.5f, 0.5f, 0.5f, 1.0f);       /* smoothness 0.5 */
            if (!textures[i][f] && (s->has || f == 2)) oom = 1;
        }
    }
    /* the base */
    if (g[G_BASEHEIGHTBLEND].has) v[9][0] = g[G_BASEHEIGHTBLEND].v.v[0];
    if (g[G_BASEHEIGHT].has && g[G_BASEHEIGHT].v.kind == K_NUM) v[9][2] = g[G_BASEHEIGHT].v.v[0];
    if (g[G_BASETILING].has) { v[10][0] = g[G_BASETILING].v.v[0]; v[10][1] = g[G_BASETILING].v.v[1]; }
    if (g[G_BASESCROLL].has) { v[10][2] = g[G_BASESCROLL].v.v[0]; v[10][3] = g[G_BASESCROLL].v.v[1]; }
    if (g[G_BASEGLOW].has) for (k = 0; k < 3; k++) v[11][k] = g[G_BASEGLOW].v.v[k];
    /* the two shared texture places, in order: the burn's tile, its base glow and blood, the base height, then each
     * layer's specular and glow image */
    if (burn) {
        int s = lk_take(&pool, &g[G_BURN].v);
        v[24][0] = 1.0f;
        v[24][2] = (float)s;
        if (g[G_BURNGLOW].has) {
            s = lk_take(&pool, &g[G_BURNGLOW].v);
            if (s) { v[24][1] = (float)s; v[11][3] = 1.0f; }
            else lk_note(c, "burnglow", "the base's own glow");
        }
        if (g[G_BURNBLOOD].has) {
            s = lk_take(&pool, &g[G_BURNBLOOD].v);
            if (s) v[24][3] = (float)s;
            else lk_note(c, "burnblood", "no blood");
        }
        if (g[G_BURNTILING].has) { v[25][0] = g[G_BURNTILING].v.v[0]; v[25][1] = g[G_BURNTILING].v.v[1]; }
        if (g[G_BURNNOISE].has) for (k = 0; k < 4; k++) v[26][k] = g[G_BURNNOISE].v.v[k];
        if (g[G_BURNHEAT].has) for (k = 0; k < 3; k++) v[27][k] = g[G_BURNHEAT].v.v[k];
    }
    if (g[G_BASEHEIGHT].has && g[G_BASEHEIGHT].v.kind == K_IMG) {
        int s = lk_take(&pool, &g[G_BASEHEIGHT].v);
        if (s) { v[9][1] = (float)s; v[9][2] = 0.0f; }
        else lk_note(c, "baseheight", "a flat height of 0.5");
    }
    for (i = 0; i < 4; i++)
        for (f = L_SPECULAR; f <= L_GLOW; f++) {
            const lk_set *s = &l[i][f];
            float *dst = v[(f == L_SPECULAR ? 16 : 20) + i];
            if (!s->has) continue;
            if (s->v.kind == K_VEC) {
                dst[0] = s->v.v[0]; dst[1] = s->v.v[1]; dst[2] = s->v.v[2]; dst[3] = 0.0f;
            } else {
                int slot = lk_take(&pool, &s->v);
                if (slot) { dst[0] = dst[1] = dst[2] = 1.0f; dst[3] = (float)slot; }
                else {
                    snprintf(key, sizeof key, "layer%d%s", i + 1, LK_L[f].name);
                    lk_note(c, key, "black");
                }
            }
        }
    /* the material's keys */
    lk_key(&gen, eol, "ambientprogram", wounds ? "\"snapmapplus/layered/skinvmtrwounds\"" : "\"snapmapplus/layered/skinvmtr\"");
    if (!lk_has_key(d, "packedspec")) lk_key(&gen, eol, "packedspec", "0");    /* layer 0's pages are read unpacked */
    if (g[G_LAYERMASK].has) lk_key_q(&gen, eol, "layerblendmask", g[G_LAYERMASK].v.s, g[G_LAYERMASK].v.sn);
    for (i = 0; i < 4; i++)
        for (f = 0; f < 4; f++) {
            if (!textures[i][f]) continue;
            snprintf(key, sizeof key, "blendlayer%d%smap", i + 1, tex[f]);
            lk_key_q(&gen, eol, key, textures[i][f], strlen(textures[i][f]));
        }
    for (i = 0; i < pool.used; i++) {
        snprintf(key, sizeof key, "multilayerextramap%d", i);
        lk_key_q(&gen, eol, key, pool.name[i], strlen(pool.name[i]));
    }
    {
        const char *img = oom ? NULL : sh_modprog_register_param_image((const float (*)[4])v, 28);
        char *name = img ? lk_image_name(img) : NULL;
        if (name) lk_key_q(&gen, eol, "layerparms", name, strlen(name));
        else oom = 1;
        free(name);
    }
    if (!oom && !gen.oom) res = lk_assemble(d, first, gen.p ? gen.p : "", platform_drop,
                                            sizeof platform_drop / sizeof platform_drop[0], out_len);
    free(gen.p);
    for (i = 0; i < 4; i++) for (f = 0; f < 4; f++) free(textures[i][f]);
    if (!res) return SH_LOOK_OOM;
    *out = res;
    return SH_LOOK_CHANGED;
}

static int lk_particle(lk_ctx *c, lk_doc *d, size_t first, char **out, size_t *out_len)
{
    lk_set p[P_COUNT];
    lk_buf gen = { 0 };
    const char *eol = d->eol;
    static const char *const drop[] = { "stageprogram", "particleuberparms1", "particleuberparms2", "particleuberparms3",
                                        "emissive", "smokebrightness", "particleemissivemap" };
    char word[16];
    int mode = 0, platform;
    float glow = 1.0f, strength = 0.0f, lit = 1.0f, depth = 0.0f, soft = 0.0f, begin = 0.0f, end = 0.0f;
    float colour[3] = { 1, 1, 1 }, grad[6] = { 1, 1, 1, 1, 1, 1 };
    char *res;
    memset(p, 0, sizeof p);
    if (!lk_read(c, d, first, 0, NULL, NULL, p)) return c->mistake ? SH_LOOK_MISTAKE : SH_LOOK_OOM;
    if (p[P_LIGHTING].has) {
        lk_word(&d->l[p[P_LIGHTING].line], word, sizeof word);
        mode = lk_ieq(word, strlen(word), "lit") ? 1 : lk_ieq(word, strlen(word), "explosion") ? 4 : 0;
    }
    if (p[P_GLOW].has) glow = p[P_GLOW].v.v[0];
    if (p[P_GLOWMASKSTRENGTH].has) strength = p[P_GLOWMASKSTRENGTH].v.v[0];
    if (p[P_LITSCALE].has) lit = p[P_LITSCALE].v.v[0];
    if (p[P_DEPTHOFFSET].has) depth = p[P_DEPTHOFFSET].v.v[0];
    if (p[P_SOFTDISTANCE].has) soft = p[P_SOFTDISTANCE].v.v[0];
    if (p[P_NEARFADE].has) { begin = p[P_NEARFADE].v.v[0]; end = p[P_NEARFADE].v.v[1]; }
    if (p[P_GLOWMASKCOLOUR].has) memcpy(colour, p[P_GLOWMASKCOLOUR].v.v, sizeof colour);
    if (p[P_GRADIENT].has) memcpy(grad, p[P_GRADIENT].v.v, sizeof grad);
    platform = mode == 4 || p[P_GLOWMASK].has || p[P_GRADIENT].has || p[P_NEARFADE].has || p[P_SOFTDISTANCE].has ||
               p[P_DEPTHOFFSET].has || p[P_FARFADE].has || (mode == 1 && (strength != 0.0f || lit != 1.0f));
    if (!platform) {
        /* the game's own shading: its program, and the brightness in the material's own key */
        lk_key(&gen, eol, "stageprogram", mode == 1 ? "particleshighq" : "transsortblend");
        if (p[P_GLOW].has) {
            char t[40];
            lk_fmt(t, sizeof t, glow);
            lk_key(&gen, eol, "emissive", t);
        }
    } else {
        long at = -1;
        size_t i;
        float u1[4], u2[4], u3[4], em[4], sb[4] = { 0, 0, 0, 0 };
        float own = 1.0f;
        for (i = 0; i < d->n; i++)
            if (d->l[i].key && lk_ieq(d->l[i].a + d->l[i].ks, d->l[i].kn, "emissive")) at = (long)i;
        if (at >= 0) {
            lk_val ev;
            lk_parse(d->l[at].a + d->l[at].v0, d->l[at].vn, &ev);
            if ((ev.kind == K_NUM || ev.kind == K_VEC) && ev.n > 0) own = ev.v[0];
        }
        u1[0] = 0; u1[1] = 0; u1[2] = (float)mode + (p[P_GLOWMASK].has ? 16.0f : 0.0f); u1[3] = depth;
        if (mode == 4) {
            u2[0] = grad[0]; u2[1] = grad[1]; u2[2] = grad[2];
            u3[0] = grad[3]; u3[1] = grad[4]; u3[2] = grad[5];
        } else {
            u2[0] = colour[0]; u2[1] = colour[1]; u2[2] = colour[2];
            u3[0] = u3[1] = u3[2] = 0.0f;
            if (mode == 1 && p[P_GLOWMASKCOLOUR2].has) {
                memcpy(u3, p[P_GLOWMASKCOLOUR2].v.v, 3 * sizeof(float));
                sb[3] = 1;
            }
        }
        u2[3] = begin;
        u3[3] = glow;
        em[0] = own; em[1] = end; em[2] = soft; em[3] = lit;
        sb[0] = strength;
        if (p[P_FARFADE].has) {
            sb[1] = p[P_FARFADE].v.v[0];
            sb[2] = p[P_FARFADE].v.v[1];
        }
        lk_key(&gen, eol, "stageprogram", "\"snapmapplus/particles/uberparticles\"");
        lk_key_vec(&gen, eol, "particleuberparms1", u1);
        lk_key_vec(&gen, eol, "particleuberparms2", u2);
        lk_key_vec(&gen, eol, "particleuberparms3", u3);
        lk_key_vec(&gen, eol, "emissive", em);
        lk_key_vec(&gen, eol, "smokebrightness", sb);
        if (p[P_GLOWMASK].has) lk_key_q(&gen, eol, "particleemissivemap", p[P_GLOWMASK].v.s, p[P_GLOWMASK].v.sn);
    }
    /* the game allocates a lit particle's light atlas tile from this key; unlit stock materials carry none */
    if ((mode == 1 || mode == 4) && !lk_has_key(d, "particlelightatlasprogram"))
        lk_key(&gen, eol, "particlelightatlasprogram", "particlelightatlas");
    if (gen.oom) { free(gen.p); return SH_LOOK_OOM; }
    res = lk_assemble(d, first, gen.p ? gen.p : "", drop, sizeof drop / sizeof drop[0], out_len);
    free(gen.p);
    if (!res) return SH_LOOK_OOM;
    *out = res;
    return SH_LOOK_CHANGED;
}

static int lk_ribbon(lk_ctx *c, lk_doc *d, size_t first, char **out, size_t *out_len)
{
    static const char *const drop[] = { "stageprogram" };
    lk_buf gen = { 0 };
    char *image = NULL;
    size_t i;
    for (i = 0; i < d->n; i++) {
        lk_ln *line = &d->l[i];
        lk_val v;
        const char *path;
        if (!line->key || !lk_ieq(line->a + line->ks, line->kn, "lengthfade")) continue;
        if (image || lk_has_key(d, "ribbonmask")) {
            free(image);
            lk_fail(c, i, "write one lengthfade or ribbonmask for the trail.");
            return SH_LOOK_MISTAKE;
        }
        lk_parse(line->a + line->v0, line->vn, &v);
        if (v.kind != K_VEC || v.n != 2 || !(v.v[0] >= 0 && v.v[0] <= 1 && v.v[1] >= 0 && v.v[1] <= 1)) {
            lk_fail(c, i, "lengthfade takes two fractions from 0 to 1, for example { 0.1, 0.2 }.");
            return SH_LOOK_MISTAKE;
        }
        path = sh_modprog_register_ribbon_mask(v.v[0], v.v[1]);
        if (!path || !(image = lk_image_name(path))) return SH_LOOK_OOM;
        line->skip = 1;
    }
    lk_key(&gen, d->eol, "stageprogram", "ribbonblend");
    if (image) lk_key(&gen, d->eol, "ribbonmask", image);
    free(image);
    if (!gen.oom) *out = lk_assemble(d, first, gen.p, drop, 1, out_len);
    free(gen.p);
    return *out ? SH_LOOK_CHANGED : SH_LOOK_OOM;
}

int sh_look_compile_notes(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                          size_t why_len, char **notes)
{
    lk_ctx c;
    lk_doc d;
    size_t i, first = (size_t)-1;
    int r, layered, ribbon;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (notes) *notes = NULL;
    if (why && why_len) why[0] = 0;
    if (!text || !out || !out_len) return SH_LOOK_OOM;
    memset(&c, 0, sizeof c);
    c.base = lk_base(path);
    c.why = why;
    c.why_len = why_len;
    if (!lk_split(&d, text, len)) return SH_LOOK_OOM;
    for (i = 0; i < d.n; i++) if (d.l[i].key) { first = i; break; }
    if (first == (size_t)-1 || !lk_ieq(d.l[first].a + d.l[first].ks, d.l[first].kn, "type")) {
        for (i = 0; i < d.n; i++)
            if (d.l[i].key && lk_ieq(d.l[i].a + d.l[i].ks, d.l[i].kn, "type")) {
                lk_fail(&c, i, "'type' must be the first key of the material; move it to the top.");
                lk_doc_free(&d);
                return SH_LOOK_MISTAKE;
            }
        lk_doc_free(&d);
        return SH_LOOK_SAME;   /* a material of the game's own keys: served as it is */
    }
    {
        char word[32];
        lk_word(&d.l[first], word, sizeof word);
        layered = lk_ieq(word, strlen(word), "layered");
        ribbon = lk_ieq(word, strlen(word), "ribbon");
        if (!layered && !ribbon && !lk_ieq(word, strlen(word), "particle")) {
            lk_fail(&c, first, "'type' is '%s'; write layered, particle or ribbon.", word);
            lk_doc_free(&d);
            return SH_LOOK_MISTAKE;
        }
    }
    if (ribbon) r = lk_ribbon(&c, &d, first, out, out_len);
    else if (layered) r = lk_layered(&c, &d, first, out, out_len);
    else r = lk_particle(&c, &d, first, out, out_len);
    lk_doc_free(&d);
    if (r == SH_LOOK_CHANGED && notes && c.notes.p) *notes = c.notes.p; else free(c.notes.p);
    if (r == SH_LOOK_MISTAKE) { free(*out); *out = NULL; }
    return r;
}

int sh_look_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                    size_t why_len)
{
    return sh_look_compile_notes(path, text, len, out, out_len, why, why_len, NULL);
}
