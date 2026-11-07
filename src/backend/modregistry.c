/* modregistry.c -- see modregistry.h. The per-map apply of the declared
 * bundles' derived edits (decl_tree.c) and the conductor's spawn-type bits.
 * No engine calls and no index access: the serving index (modserve.c)
 * collects the sources and serves the result. */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modregistry.h"
#include "decl_tree.h"

int sh_modregistry_stock_bit(unsigned value)
{
    return (value && value <= (1u << 18) && !(value & (value - 1))) || value == (1u << 30);
}
/* ------------------------------------------------------------------ small text */

typedef struct mr_buf { char *p; size_t n, cap; int oom; } mr_buf;

static void mr_put(mr_buf *b, const char *s, size_t n)
{
    if (b->oom || !n) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        char *g;
        while (cap < b->n + n + 1) cap *= 2;
        g = (char *)realloc(b->p, cap);
        if (!g) { b->oom = 1; return; }
        b->p = g;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void mr_puts(mr_buf *b, const char *s) { mr_put(b, s, strlen(s)); }

static char *mr_done(mr_buf *b, size_t *out_len)
{
    if (b->oom) { free(b->p); return NULL; }
    if (!b->p) { b->p = (char *)malloc(1); if (!b->p) return NULL; b->p[0] = 0; }
    if (out_len) *out_len = b->n;
    return b->p;
}

static char *mr_strdup(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *d = (char *)malloc(n + 1);
    if (d) { if (n) memcpy(d, s, n); d[n] = 0; }
    return d;
}

/* Decl text tokens: words, strings, braces, '=' and ';'; comments skipped. */
enum { MT_END, MT_WORD, MT_STR, MT_LB, MT_RB, MT_EQ, MT_SEMI, MT_OTHER };
typedef struct mr_tok { int kind; size_t at, n; } mr_tok;

static mr_tok mr_next(const char *t, size_t len, size_t *pos)
{
    mr_tok k = { MT_END, 0, 0 };
    size_t i = *pos;
    for (;;) {
        while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '/') { while (i < len && t[i] != '\n') i++; continue; }
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i = i + 2 <= len ? i + 2 : len;
            continue;
        }
        break;
    }
    k.at = i;
    if (i >= len) { *pos = len; k.at = len; return k; }
    if (t[i] == '"') {
        for (++i; i < len && t[i] != '"'; ++i) if (t[i] == '\\' && i + 1 < len) ++i;
        if (i < len) ++i;
        k.kind = MT_STR;
    } else if (t[i] == '{') { k.kind = MT_LB; ++i; }
    else if (t[i] == '}') { k.kind = MT_RB; ++i; }
    else if (t[i] == '=') { k.kind = MT_EQ; ++i; }
    else if (t[i] == ';') { k.kind = MT_SEMI; ++i; }
    else if (strchr("()", t[i])) { k.kind = MT_OTHER; ++i; }
    else {
        while (i < len && !strchr(" \t\r\n{}=;\"()", t[i])) i++;
        k.kind = MT_WORD;
    }
    k.n = i - k.at;
    *pos = i;
    return k;
}

static int mr_word_is(const char *t, const mr_tok *k, const char *w)
{
    size_t n = strlen(w);
    return k->kind == MT_WORD && k->n == n && !_strnicmp(t + k->at, w, n);
}

/* The block `name = {` (or `name {`) at `depth` below the outermost brace:
 * *open is the '{' offset and *close the matching '}' offset. 1 found. */
static int mr_find_block(const char *t, size_t len, size_t from, size_t to, const char *name, int want_depth,
                         size_t *open, size_t *close)
{
    size_t pos = from;
    int depth = 0;
    mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
    for (;;) {
        mr_tok k = mr_next(t, to < len ? to : len, &pos);
        if (k.kind == MT_END) return 0;
        if (k.kind == MT_LB) {
            int named = (prev.kind == MT_EQ && mr_word_is(t, &prev2, name)) || mr_word_is(t, &prev, name);
            depth++;
            if (named && depth == want_depth + 1) {
                int d = 1;
                *open = k.at;
                for (;;) {
                    mr_tok q = mr_next(t, to < len ? to : len, &pos);
                    if (q.kind == MT_END) return 0;
                    if (q.kind == MT_LB) d++;
                    else if (q.kind == MT_RB && --d == 0) { *close = q.at; return 1; }
                }
            }
        } else if (k.kind == MT_RB) depth--;
        prev2 = prev;
        prev = k;
    }
}

/* ------------------------------------------------------------------ conductor */

int sh_modregistry_conductor_rows(const char *text, size_t len,
                                  void (*fn)(void *context, const char *ai_type, const char *entity_def),
                                  void *context)
{
    size_t open, close, pos;
    int depth = 0, count = 0;
    char ai[256], def[SH_MB_REQUEST_MAX + 1], key[64];
    mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
    if (!text || !mr_find_block(text, len, 0, len, "aiTypeList", 2, &open, &close)) return -1;
    pos = open + 1;
    ai[0] = def[0] = key[0] = 0;
    for (;;) {
        mr_tok k = mr_next(text, close, &pos);
        if (k.kind == MT_END) break;
        if (k.kind == MT_LB) { if (++depth == 1) ai[0] = def[0] = 0; }
        else if (k.kind == MT_RB) {
            if (depth-- == 1) { if (fn) fn(context, ai, def); count++; }
        } else if (k.kind == MT_STR && depth == 1 && prev.kind == MT_EQ && prev2.kind == MT_WORD) {
            size_t n = k.n >= 2 ? k.n - 2 : 0;
            char *dst = NULL;
            size_t cap = 0;
            if (prev2.n < sizeof key) { memcpy(key, text + prev2.at, prev2.n); key[prev2.n] = 0; }
            else key[0] = 0;
            if (!_stricmp(key, "aiType")) { dst = ai; cap = sizeof ai; }
            else if (!_stricmp(key, "entityDef")) { dst = def; cap = sizeof def; }
            if (dst) {
                if (n >= cap) n = cap - 1;
                memcpy(dst, text + k.at + 1, n);
                dst[n] = 0;
            }
        }
        prev2 = prev;
        prev = k;
    }
    return count;
}

char *sh_modregistry_rewrite_bits(const char *text, size_t len, const unsigned *from, const unsigned *to, int n,
                                  size_t *out_len)
{
    size_t pos = 0, copied = 0;
    int changed = 0;
    mr_buf b = { NULL, 0, 0, 0 };
    mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
    if (!text) return NULL;
    for (;;) {
        mr_tok k = mr_next(text, len, &pos);
        if (k.kind == MT_END) break;
        if (k.kind == MT_STR && prev.kind == MT_EQ && (mr_word_is(text, &prev2, "memberType") || mr_word_is(text, &prev2, "aiType"))) {
            /* Space-separated flag names and numbers inside the quotes. */
            size_t i = k.at + 1, e = k.at + k.n - 1;
            while (i < e) {
                size_t s = i, m;
                char nb[16];
                while (i < e && text[i] != ' ') i++;
                m = i - s;
                if (m && m < sizeof nb && strspn(text + s, "0123456789") >= m) {
                    unsigned v;
                    int q;
                    memcpy(nb, text + s, m);
                    nb[m] = 0;
                    v = (unsigned)strtoul(nb, NULL, 10);
                    for (q = 0; q < n; ++q)
                        if (from[q] == v && to[q] != v) {
                            char w[16];
                            mr_put(&b, text + copied, s - copied);
                            _snprintf_s(w, sizeof w, _TRUNCATE, "%u", to[q]);
                            mr_puts(&b, w);
                            copied = i;
                            changed = 1;
                            break;
                        }
                }
                while (i < e && text[i] == ' ') i++;
            }
        }
        prev2 = prev;
        prev = k;
    }
    if (!changed) { free(b.p); return NULL; }
    mr_put(&b, text + copied, len - copied);
    return mr_done(&b, out_len);
}

/* ------------------------------------------------------------------ generation */

static void mr_line(sh_mr_result *r, const char *fmt, ...)
{
    char buf[700];
    va_list ap;
    char **g;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    g = (char **)realloc(r->lines, sizeof(char *) * (size_t)(r->nlines + 1));
    if (!g) return;
    r->lines = g;
    r->lines[r->nlines] = mr_strdup(buf);
    if (r->lines[r->nlines]) r->nlines++;
}

static int mr_grow(void **arr, int n, size_t elem)
{
    void *g = realloc(*arr, elem * (size_t)(n + 1));
    if (!g) return 0;
    *arr = g;
    return 1;
}

/* The numbers in a memberType value that are not stock bits. */
static int mr_custom_numbers(const char *text, size_t len, unsigned *out, int cap)
{
    size_t pos = 0;
    int n = 0;
    mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
    for (;;) {
        mr_tok k = mr_next(text, len, &pos);
        if (k.kind == MT_END) break;
        if (k.kind == MT_STR && prev.kind == MT_EQ && mr_word_is(text, &prev2, "memberType")) {
            size_t i = k.at + 1, e = k.at + k.n - 1;
            while (i < e) {
                size_t s = i, m;
                while (i < e && text[i] != ' ') i++;
                m = i - s;
                if (m && m < 11 && strspn(text + s, "0123456789") >= m) {
                    char nb[16];
                    unsigned v;
                    int q, seen = 0;
                    memcpy(nb, text + s, m);
                    nb[m] = 0;
                    v = (unsigned)strtoul(nb, NULL, 10);
                    for (q = 0; q < n; ++q) seen |= out[q] == v;
                    if (!seen && !sh_modregistry_stock_bit(v) && n < cap) out[n++] = v;
                }
                while (i < e && text[i] == ' ') i++;
            }
        }
        prev2 = prev;
        prev = k;
    }
    return n;
}

static int mr_entry_cmp(const void *a, const void *b)
{
    const sh_mr_entry *x = (const sh_mr_entry *)a, *y = (const sh_mr_entry *)b;
    int c;
    if (x->source != y->source) return x->source < y->source ? -1 : 1;
    c = _stricmp(x->encounter[0] ? x->encounter : x->def, y->encounter[0] ? y->encounter : y->def);
    if (c) return c;
    c = _stricmp(x->def, y->def);
    return c ? c : (x->authored < y->authored ? -1 : x->authored > y->authored);
}

static int mr_count(const char *text, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    int c = 0;
    for (i = 0; n && i + n <= len; i++) {
        if (text[i] != needle[0] || memcmp(text + i, needle, n)) continue;
        c++;
        i += n - 1;
    }
    return c;
}

/* The body's clip blocks, counted by structure: an `anim "<clip>" {` block
 * directly inside the top-level `events` block. An alias row names its clip
 * with the same word but has no block, and the composer may write it across
 * several lines, so a line test miscounts it (V2c: 228 for 227). Quoted text
 * is skipped; `//` comments run to the line end. */
int sh_modregistry_body_clips(const char *text, size_t len)
{
    size_t i = 0;
    int depth = 0, in_events = 0, c = 0;
    while (i < len) {
        char ch = text[i];
        if (ch == '"') {
            for (i++; i < len && text[i] != '"'; i++) if (text[i] == '\\' && i + 1 < len) i++;
            i++;
            continue;
        }
        if (ch == '/' && i + 1 < len && text[i + 1] == '/') {
            while (i < len && text[i] != '\n') i++;
            continue;
        }
        if (ch == '{') { depth++; i++; continue; }
        if (ch == '}') { if (depth == 2) in_events = 0; if (depth > 0) depth--; i++; continue; }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
            size_t w = i, j;
            while (i < len && ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') ||
                               (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) i++;
            j = i;
            while (j < len && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) j++;
            if (depth == 1 && i - w == 6 && !memcmp(text + w, "events", 6) && j < len && text[j] == '{') in_events = 1;
            else if (depth == 2 && in_events && i - w == 4 && !memcmp(text + w, "anim", 4) && j < len && text[j] == '"') {
                size_t k = j + 1;
                while (k < len && text[k] != '"') { if (text[k] == '\\' && k + 1 < len) k++; k++; }
                k++;
                while (k < len && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r' || text[k] == '\n')) k++;
                if (k < len && text[k] == '{') c++;
            }
            continue;
        }
        i++;
    }
    return c;
}


/* ------------------------------------------------------------------ apply */

typedef struct mr_apply_ctx { sh_mr_result *r; int target; } mr_apply_ctx;

static void mr_apply_line(void *context, const char *text)
{
    mr_apply_ctx *c = (mr_apply_ctx *)context;
    mr_line(c->r, "%s", text);
}

static void mr_apply_clash(void *context, const char *slot, const int *sources, int n, int serving, int by_choice)
{
    mr_apply_ctx *c = (mr_apply_ctx *)context;
    sh_mr_clash *k;
    int i;
    if (!mr_grow((void **)&c->r->clashes, c->r->nclashes, sizeof *c->r->clashes)) return;
    k = &c->r->clashes[c->r->nclashes++];
    memset(k, 0, sizeof *k);
    strncpy_s(k->slot, sizeof k->slot, slot, _TRUNCATE);
    k->target = c->target;
    for (i = 0; i < n && i < SH_MR_CLASH_SOURCES; ++i) k->sources[k->n++] = sources[i];
    k->serving = serving;
    k->by_choice = by_choice;
}

/* A scalar's value without its quotes, into out. */
static void mr_unquote(const sh_dt_node *n, char *out, size_t cap)
{
    const char *v = n && !n->block ? n->val : "";
    size_t len = n && !n->block ? n->val_n : 0;
    if (len >= 2 && v[0] == '"' && v[len - 1] == '"') { v++; len -= 2; }
    if (len >= cap) len = cap - 1;
    memcpy(out, v, len);
    out[len] = 0;
}

/* The conductor's rule: each aiTypeList item a source added whose aiType is
 * a number that is not a stock bit gets a bit assigned per map, and that
 * source's encounters and scores are served with it. */
static int mr_conductor(sh_dt *t, const sh_mr_source *src, int n, sh_mr_result *r)
{
    sh_dt_node *list = sh_dt_at(sh_dt_root(t), "edit/aiTypeList");
    sh_dt_node **items = NULL;
    int i, s, nitems = 0;
    if (!list) { mr_line(r, "conductor: the generated text has no edit/aiTypeList; no spawn-type bit is assigned"); return 1; }
    items = (sh_dt_node **)calloc((size_t)(list->nkids + 1), sizeof *items);
    if (!items) return 0;
    for (i = 0; i < list->nkids; ++i) {
        sh_dt_node *it = list->kids[i], *ai, *def;
        char a[64], d[SH_MB_REQUEST_MAX + 1], *end;
        unsigned long v;
        int q, dup = -1;
        if (!it->block || it->src < 0) continue;
        ai = sh_dt_child(it, "aiType");
        def = sh_dt_child(it, "entityDef");
        mr_unquote(ai, a, sizeof a);
        mr_unquote(def, d, sizeof d);
        if (!a[0] || strspn(a, "0123456789") != strlen(a)) continue;   /* a SPAWN_AI_TYPE_* name: the game's own */
        v = strtoul(a, &end, 10);
        if (sh_modregistry_stock_bit((unsigned)v)) continue;
        if (!d[0]) {
            mr_line(r, "conductor: %s adds an aiTypeList item with aiType %s and no entityDef; it keeps that number",
                    src[it->src].who, a);
            continue;
        }
        for (q = 0; q < r->nentries && dup < 0; ++q)
            if (r->entries[q].source == it->src && r->entries[q].authored == (unsigned)v) dup = q;
        items[nitems++] = it;
        if (dup >= 0) continue;   /* one number listed twice by a source: one bit */
        if (!mr_grow((void **)&r->entries, r->nentries, sizeof *r->entries)) { free(items); return 0; }
        {
            sh_mr_entry *e = &r->entries[r->nentries++];
            const sh_mr_source *so = &src[it->src];
            int dd;
            memset(e, 0, sizeof *e);
            e->source = it->src;
            e->authored = (unsigned)v;
            strncpy_s(e->def, sizeof e->def, d, _TRUNCATE);
            /* The encounter that names the number orders the entry. */
            for (dd = 0; dd < so->ndecls; ++dd) {
                unsigned nums[8];
                int k, nn;
                if (so->decls[dd].score) continue;
                nn = mr_custom_numbers(so->decls[dd].text, so->decls[dd].len, nums, 8);
                for (k = 0; k < nn; ++k)
                    if (nums[k] == (unsigned)v && (!e->encounter[0] || _stricmp(so->decls[dd].name, e->encounter) < 0))
                        strncpy_s(e->encounter, sizeof e->encounter, so->decls[dd].name, _TRUNCATE);
            }
        }
    }
    if (r->nentries) qsort(r->entries, (size_t)r->nentries, sizeof *r->entries, mr_entry_cmp);
    for (i = 0; i < r->nentries; ++i) {
        if (SH_MR_BIT_FIRST + i > SH_MR_BIT_LAST) {
            mr_line(r, "conductor: no free spawn-type bit for %s (%s) of %s: a map holds at most %d custom encounter types; "
                    "it keeps the number its author wrote", r->entries[i].encounter[0] ? r->entries[i].encounter : "its encounter",
                    r->entries[i].def, src[r->entries[i].source].who, SH_MR_BIT_LAST - SH_MR_BIT_FIRST + 1);
            r->nentries = i;
            break;
        }
        r->entries[i].bit = 1u << (SH_MR_BIT_FIRST + i);
    }
    /* The items take their bits. */
    for (i = 0; i < nitems; ++i) {
        sh_dt_node *ai = sh_dt_child(items[i], "aiType");
        char a[64], bit[24];
        unsigned long v;
        int q;
        mr_unquote(ai, a, sizeof a);
        v = strtoul(a, NULL, 10);
        for (q = 0; q < r->nentries; ++q)
            if (r->entries[q].source == items[i]->src && r->entries[q].authored == (unsigned)v) {
                _snprintf_s(bit, sizeof bit, _TRUNCATE, "\"%u\"", r->entries[q].bit);
                if (!sh_dt_set_value(t, ai, bit, strlen(bit))) { free(items); return 0; }
                break;
            }
    }
    free(items);
    for (i = 0; i < r->nentries; ++i)
        mr_line(r, "conductor: aiType %u -> %s (%s, %s, authored %u)", r->entries[i].bit, r->entries[i].def,
                src[r->entries[i].source].who, r->entries[i].encounter[0] ? r->entries[i].encounter : "no encounter",
                r->entries[i].authored);
    /* Each source's encounters and scores, rewritten to the bits assigned. */
    for (s = 0; s < n; ++s) {
        unsigned from[SH_MR_BIT_LAST - SH_MR_BIT_FIRST + 1], to[SH_MR_BIT_LAST - SH_MR_BIT_FIRST + 1];
        int m = 0, d;
        for (i = 0; i < r->nentries; ++i)
            if (r->entries[i].source == s && m < (int)(sizeof from / sizeof from[0])) {
                from[m] = r->entries[i].authored;
                to[m++] = r->entries[i].bit;
            }
        if (!m) continue;
        for (d = 0; d < src[s].ndecls; ++d) {
            size_t len = 0;
            char *tx = sh_modregistry_rewrite_bits(src[s].decls[d].text, src[s].decls[d].len, from, to, m, &len);
            if (!tx) continue;
            if (!mr_grow((void **)&r->rewrites, r->nrewrites, sizeof *r->rewrites)) { free(tx); return 0; }
            r->rewrites[r->nrewrites].id = src[s].decls[d].id;
            r->rewrites[r->nrewrites].text = tx;
            r->rewrites[r->nrewrites].len = len;
            r->nrewrites++;
            mr_line(r, "conductor: %s's %s %s served with the assigned bit", src[s].who,
                    src[s].decls[d].score ? "score" : "encounter", src[s].decls[d].name);
        }
    }
    return 1;
}

int sh_modregistry_generate(const sh_mr_source *src, int n, const sh_mr_target *targets, int ntargets,
                            const sh_mr_choice *choices, int nchoices, sh_mr_result *out)
{
    sh_dt_source *ds = NULL;
    sh_dt_choice *dc = NULL;
    int i, k;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->ntargets = ntargets;
    out->nsources = n;
    if (ntargets <= 0) return 1;
    out->text = (char **)calloc((size_t)ntargets, sizeof(char *));
    out->len = (size_t *)calloc((size_t)ntargets, sizeof(size_t));
    out->used = (unsigned char **)calloc((size_t)ntargets, sizeof(unsigned char *));
    ds = (sh_dt_source *)calloc((size_t)(n > 0 ? n : 1), sizeof *ds);
    dc = (sh_dt_choice *)calloc((size_t)(nchoices > 0 ? nchoices : 1), sizeof *dc);
    if (!out->text || !out->len || !out->used || !ds || !dc) goto oom;
    for (i = 0; i < n; ++i) {
        ds[i].key = src[i].key;
        ds[i].who = src[i].who;
        ds[i].edits = src[i].edits;
        ds[i].edits_len = src[i].edits_len;
    }
    for (i = 0; i < nchoices; ++i) { dc[i].slot = choices[i].slot; dc[i].key = choices[i].key; }
    for (k = 0; k < ntargets; ++k) {
        mr_apply_ctx ctx;
        sh_dt_report rep;
        sh_dt *t;
        char game[SH_MB_REQUEST_MAX + 1];
        (void)sh_dt_game_name(targets[k].request, game, sizeof game);
        out->used[k] = (unsigned char *)calloc((size_t)(n > 0 ? n : 1), 1);
        if (!out->used[k]) goto oom;
        if (!targets[k].text) {
            for (i = 0; i < n; ++i)
                if (src[i].edits && strstr(src[i].edits, targets[k].request)) {
                    mr_line(out, "%s: the game's text could not be read; the game's own file serves and no mod's edits "
                            "apply", game);
                    break;
                }
            continue;
        }
        ctx.r = out;
        ctx.target = k;
        memset(&rep, 0, sizeof rep);
        rep.line = mr_apply_line;
        rep.clash = mr_apply_clash;
        rep.context = &ctx;
        rep.used = out->used[k];
        t = sh_dt_apply(targets[k].request, targets[k].text, targets[k].len, ds, n, dc, nchoices, &rep);
        if (!t) continue;
        if (!_stricmp(targets[k].request, SH_MB_REGISTRY_CONDUCTOR) && !mr_conductor(t, src, n, out)) { sh_dt_free(t); goto oom; }
        out->text[k] = sh_dt_text(t, &out->len[k]);
        sh_dt_free(t);
        if (!out->text[k]) goto oom;
        if (!_stricmp(targets[k].request, SH_MB_REGISTRY_BODY)) {
            int srcs = 0;   /* sources whose edits name the body (identical ones merge) */
            for (i = 0; i < n; ++i) srcs += src[i].edits && strstr(src[i].edits, SH_MB_REGISTRY_BODY) != NULL;
            out->body_clips = sh_modregistry_body_clips(out->text[k], out->len[k]);
            out->body_aliases = mr_count(out->text[k], out->len[k], "alias {");
            mr_line(out, "player body: the merged body holds %d clip%s and %d alias%s, %zu bytes, from %d source%s",
                    out->body_clips, out->body_clips == 1 ? "" : "s", out->body_aliases, out->body_aliases == 1 ? "" : "es",
                    out->len[k], srcs, srcs == 1 ? "" : "s");
        }
    }
    free(ds);
    free(dc);
    return 1;
oom:
    free(ds);
    free(dc);
    sh_modregistry_result_free(out);
    return 0;
}

void sh_modregistry_result_free(sh_mr_result *r)
{
    int i;
    if (!r) return;
    for (i = 0; i < r->ntargets; ++i) {
        if (r->text) free(r->text[i]);
        if (r->used) free(r->used[i]);
    }
    for (i = 0; i < r->nrewrites; ++i) free(r->rewrites[i].text);
    for (i = 0; i < r->nlines; ++i) free(r->lines[i]);
    free(r->text); free(r->len); free(r->used);
    free(r->entries); free(r->rewrites); free(r->clashes); free(r->lines);
    memset(r, 0, sizeof *r);
}

/* ------------------------------------------------------------------ choices */

int sh_modregistry_slot_valid(const char *slot)
{
    size_t i, n = slot ? strlen(slot) : 0;
    int parts = 1;
    if (!n || n >= SH_MR_SLOT_MAX || slot[0] == '/' || slot[n - 1] == '/') return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)slot[i];
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') return 0;
        if (c == '/') { if (slot[i + 1] == '/') return 0; parts++; }
    }
    return parts >= 3;
}
