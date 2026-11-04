/* modregistry.c -- see modregistry.h. Text generation for the registries and
 * the map's registry choices. No engine calls and no index access: the
 * serving index (modserve.c) collects the sources and serves the result. */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modregistry.h"
#include "modserve.h"
#include "modmanifest.h"
#include "config_json.h"

#define MR_JSON_DEPTH 16u

static const char *const g_requests[SH_MR_FIXED] = { SH_MB_REGISTRY_CONDUCTOR, SH_MB_REGISTRY_BODY, SH_MB_REGISTRY_PLAYER };
static const char *const g_names[SH_MR_FIXED] = { "encounter conductor", "player body", "player entity" };
/* The coop entity field each player member sets (SH_MB_PLAYER_* order). */
static const char *const g_player_keys[SH_MB_PLAYER_FIELDS] = {
    "handsModelMPUseDoomHands", "handsModelMPUseDoomHands_D1", "thirdPersonBodyMPUseDoomHands", "", "handsAnimWeb",
};

const char *sh_modregistry_request(int registry)
{
    return registry >= 0 && registry < SH_MR_FIXED ? g_requests[registry] : NULL;
}

const char *sh_modregistry_player_key(int field)
{
    return field >= 0 && field < SH_MB_PLAYER_FIELDS ? g_player_keys[field] : "";
}

int sh_modregistry_weapon_request(const char *slot, char *out, size_t cap)
{
    int w;
    char *p;
    if (!slot || !slot[0] || !out || !cap || strpbrk(slot, " \t\r\n\"\\")) return 0;
    w = _snprintf_s(out, cap, _TRUNCATE, "generated/decls/weapon/%s.decl", slot);
    if (w < 0) return 0;
    for (p = out; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    return 1;
}

/* The list-shaped stock decls the product extends (DESIGN-additive 2.4):
 * one table with the builder (sh_mb_extend_target, modbundle.c), so a
 * target the builder accepts is one the generator extends. */
#define MR_CONDUCTOR_TARGET "entitydef/ai/conductor/coop/snapmap_default"
#define MR_BODY_TARGET      "md6def/player/mp/tp_body_mp.md6"
#define MR_BODY_CONTAINERS  "events;aliases"

int sh_modregistry_extend_target(const char *target, int *kind, const char **containers, int *fixed,
                                 char *cause, size_t cap)
{
    return sh_mb_extend_target(target, kind, containers, fixed, cause, cap);
}

int sh_modregistry_extend_request(const char *target, char *out, size_t cap)
{
    int w;
    char *p;
    if (!target || !target[0] || !out || !cap) return 0;
    w = _snprintf_s(out, cap, _TRUNCATE, "generated/decls/%s.decl", target);
    if (w < 0) return 0;
    for (p = out; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    return 1;
}

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

static const char *mr_eol(const char *t, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; ++i) if (t[i] == '\r' && t[i + 1] == '\n') return "\r\n";
    return "\n";
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

/* The start of the line holding `at` when only blanks precede it there, else `at`. */
static size_t mr_line_start(const char *t, size_t at)
{
    size_t i = at;
    while (i > 0 && (t[i - 1] == ' ' || t[i - 1] == '\t')) i--;
    return i == 0 || t[i - 1] == '\n' ? i : at;
}

/* The indentation of the line holding `at`. */
static void mr_indent_of(const char *t, size_t at, char *out, size_t cap)
{
    size_t s = at, w = 0;
    while (s > 0 && t[s - 1] != '\n') s--;
    while (s < at && (t[s] == ' ' || t[s] == '\t') && w + 1 < cap) out[w++] = t[s++];
    out[w] = 0;
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

char *sh_modregistry_conductor_text(const char *stock, size_t len, const char *const *defs, const unsigned *bits,
                                    int n, size_t *out_len)
{
    size_t open, close, pos, num_at = 0, num_n = 0, cut;
    long num = -1;
    int depth = 0, i;
    char indent[64], tmp[64];
    const char *eol;
    mr_buf b = { NULL, 0, 0, 0 };
    mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
    if (!stock || !mr_find_block(stock, len, 0, len, "aiTypeList", 2, &open, &close)) return NULL;
    pos = open + 1;
    for (;;) {
        mr_tok k = mr_next(stock, close, &pos);
        if (k.kind == MT_END) break;
        if (k.kind == MT_LB) depth++;
        else if (k.kind == MT_RB) depth--;
        else if (depth == 0 && k.kind == MT_WORD && prev.kind == MT_EQ && mr_word_is(stock, &prev2, "num") && num < 0) {
            char nb[32];
            size_t m = k.n < sizeof nb - 1 ? k.n : sizeof nb - 1;
            memcpy(nb, stock + k.at, m);
            nb[m] = 0;
            num = strtol(nb, NULL, 10);
            num_at = k.at;
            num_n = k.n;
        }
        prev2 = prev;
        prev = k;
    }
    if (num < 0) return NULL;
    eol = mr_eol(stock, len);
    cut = mr_line_start(stock, close);
    mr_indent_of(stock, close, indent, sizeof indent - 2);
    if (cut == close && close > 0 && stock[close - 1] != '\n') indent[0] = 0;   /* a brace on a member's line */
    mr_put(&b, stock, num_at);
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%ld", num + n);
    mr_puts(&b, tmp);
    mr_put(&b, stock + num_at + num_n, cut - (num_at + num_n));
    for (i = 0; i < n; ++i) {
        mr_puts(&b, indent); mr_puts(&b, "\t");
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "item[%ld] = {", num + i);
        mr_puts(&b, tmp); mr_puts(&b, eol);
        mr_puts(&b, indent); mr_puts(&b, "\t\t");
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "aiType = \"%u\";", bits[i]);
        mr_puts(&b, tmp); mr_puts(&b, eol);
        mr_puts(&b, indent); mr_puts(&b, "\t\tentityDef = \""); mr_puts(&b, defs[i]); mr_puts(&b, "\";"); mr_puts(&b, eol);
        mr_puts(&b, indent); mr_puts(&b, "\t}"); mr_puts(&b, eol);
    }
    mr_put(&b, stock + cut, len - cut);
    return mr_done(&b, out_len);
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

/* ------------------------------------------------------------------ player entity */

typedef struct mr_edit { size_t at, cut; const char *ins; char *owned; } mr_edit;

static int mr_edit_cmp(const void *a, const void *b)
{
    const mr_edit *x = (const mr_edit *)a, *y = (const mr_edit *)b;
    return x->at < y->at ? -1 : x->at > y->at;
}

char *sh_modregistry_player_text(const char *base, size_t len, const char *const values[SH_MB_PLAYER_FIELDS],
                                 size_t *out_len)
{
    size_t open, close, at = 0;
    mr_edit edits[SH_MB_PLAYER_FIELDS];
    int ne = 0, f, i;
    char indent[64];
    const char *eol;
    mr_buf b = { NULL, 0, 0, 0 };
    if (!base || !mr_find_block(base, len, 0, len, "edit", 1, &open, &close)) return NULL;
    eol = mr_eol(base, len);
    /* The indentation of the block's members: its first member's, else one tab deeper than its line. */
    {
        size_t p = open + 1;
        mr_tok k = mr_next(base, close, &p);
        if (k.kind != MT_END && mr_line_start(base, k.at) != k.at) mr_indent_of(base, k.at, indent, sizeof indent);
        else {
            mr_indent_of(base, open, indent, sizeof indent - 1);
            strcat_s(indent, sizeof indent, "\t");
        }
    }
    for (f = 0; f < SH_MB_PLAYER_FIELDS; ++f) {
        const char *key = g_player_keys[f];
        size_t pos = open + 1;
        int depth = 0, done = 0;
        mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
        if (!values || !values[f] || !key[0]) continue;
        for (;;) {
            mr_tok k = mr_next(base, close, &pos);
            if (k.kind == MT_END) break;
            if (k.kind == MT_LB) depth++;
            else if (k.kind == MT_RB) depth--;
            else if (depth == 0 && k.kind == MT_STR && prev.kind == MT_EQ && mr_word_is(base, &prev2, key)) {
                mr_buf v = { NULL, 0, 0, 0 };
                mr_puts(&v, "\""); mr_puts(&v, values[f]); mr_puts(&v, "\"");
                edits[ne].at = k.at;
                edits[ne].cut = k.n;
                edits[ne].owned = mr_done(&v, NULL);
                edits[ne].ins = edits[ne].owned;
                if (!edits[ne].owned) goto oom;
                ne++;
                done = 1;
                break;
            }
            prev2 = prev;
            prev = k;
        }
        if (!done) {
            /* New members go first in the block, one per line, in field order. */
            mr_buf v = { NULL, 0, 0, 0 };
            mr_puts(&v, eol); mr_puts(&v, indent); mr_puts(&v, key); mr_puts(&v, " = \"");
            mr_puts(&v, values[f]); mr_puts(&v, "\";");
            edits[ne].at = open + 1;
            edits[ne].cut = 0;
            edits[ne].owned = mr_done(&v, NULL);
            edits[ne].ins = edits[ne].owned;
            if (!edits[ne].owned) goto oom;
            ne++;
        }
    }
    /* Stable by offset: insertions at one offset keep field order. */
    for (i = 1; i < ne; ++i) {
        int q;
        for (q = i; q > 0 && mr_edit_cmp(&edits[q - 1], &edits[q]) > 0; --q) {
            mr_edit t = edits[q]; edits[q] = edits[q - 1]; edits[q - 1] = t;
        }
    }
    for (i = 0; i < ne; ++i) {
        mr_put(&b, base + at, edits[i].at - at);
        mr_puts(&b, edits[i].ins);
        at = edits[i].at + edits[i].cut;
    }
    mr_put(&b, base + at, len - at);
    for (i = 0; i < ne; ++i) free(edits[i].owned);
    return mr_done(&b, out_len);
oom:
    for (i = 0; i < ne; ++i) free(edits[i].owned);
    free(b.p);
    return NULL;
}

/* ------------------------------------------------------------------ intent */

static char *mr_decode(const char *raw)
{
    size_t n = raw ? strlen(raw) : 0, got = 0;
    char *out;
    if (n < 2 || raw[0] != '"') return NULL;
    out = (char *)malloc(n);
    if (!out) return NULL;
    if (!sh_json_decode_string(raw, n, out, n, &got) || strlen(out) != got || !got) { free(out); return NULL; }
    return out;
}

/* A string-to-string object into parallel arrays. */
static int mr_pairs(const char *raw, char ***keys, char ***vals, size_t *count)
{
    sh_json_object o = { 0 };
    size_t i;
    *keys = *vals = NULL;
    *count = 0;
    if (!raw || *raw != '{' || !sh_json_parse_object(raw, strlen(raw), MR_JSON_DEPTH, &o)) return 0;
    if (o.count) {
        *keys = (char **)calloc(o.count, sizeof(char *));
        *vals = (char **)calloc(o.count, sizeof(char *));
        if (!*keys || !*vals) { free(*keys); free(*vals); *keys = *vals = NULL; sh_json_object_free(&o); return 0; }
    }
    for (i = 0; i < o.count; ++i) {
        char *v = mr_decode(o.members[i].value_json), *k;
        if (!v) continue;
        k = (char *)malloc(o.members[i].key_length + 1);
        if (!k) { free(v); continue; }
        memcpy(k, o.members[i].key, o.members[i].key_length);
        k[o.members[i].key_length] = 0;
        (*keys)[*count] = k;
        (*vals)[*count] = v;
        (*count)++;
    }
    sh_json_object_free(&o);
    return 1;
}

/* Add extends entries from a "<target>": "<partial>" object (lower case; a target given twice keeps the first). */
static void mr_extends(sh_mr_intent *out, const char *raw)
{
    char **t = NULL, **v = NULL;
    size_t n = 0, i, k;
    if (!mr_pairs(raw, &t, &v, &n)) return;
    for (i = 0; i < n; ++i) {
        char *c;
        int dup = 0;
        for (c = t[i]; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        for (c = v[i]; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        for (k = 0; k < out->ext_count && !dup; ++k) dup = !strcmp(out->ext_targets[k], t[i]);
        if (!dup) {
            char **gt = (char **)realloc(out->ext_targets, sizeof(char *) * (out->ext_count + 1));
            char **gv = gt ? (char **)realloc(out->ext_partials, sizeof(char *) * (out->ext_count + 1)) : NULL;
            if (gt) out->ext_targets = gt;
            if (gv) {
                out->ext_partials = gv;
                out->ext_targets[out->ext_count] = t[i];
                out->ext_partials[out->ext_count++] = v[i];
                continue;
            }
        }
        free(t[i]);
        free(v[i]);
    }
    free(t);
    free(v);
}

int sh_modregistry_intent(const char *registries, const char *extra, sh_mr_intent *out)
{
    sh_json_object root = { 0 }, player = { 0 };
    const char *p;
    int k;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (registries && *registries) {
        sh_json_object reg = { 0 };
        if (!sh_json_parse_object(registries, strlen(registries), MR_JSON_DEPTH, &reg)) return 0;
        p = sh_json_object_get(&reg, "extends");
        if (p) mr_extends(out, p);
        sh_json_object_free(&reg);
    }
    if (!extra || !*extra) return 1;
    if (!sh_json_parse_object(extra, strlen(extra), MR_JSON_DEPTH, &root)) return 0;
    p = sh_json_object_get(&root, "player");
    if (p && *p == '{' && sh_json_parse_object(p, strlen(p), MR_JSON_DEPTH, &player)) {
        for (k = 0; k < SH_MB_PLAYER_FIELDS; ++k) {
            const char *v = sh_json_object_get(&player, sh_mb_player_fields[k]);
            if (v) out->player[k] = mr_decode(v);
        }
        p = sh_json_object_get(&player, "weapons");
        if (p) (void)mr_pairs(p, &out->weapon_slots, &out->weapon_decls, &out->weapon_count);
        sh_json_object_free(&player);
    }
    p = sh_json_object_get(&root, "extends");
    if (p) mr_extends(out, p);
    sh_json_object_free(&root);
    return 1;
}

void sh_modregistry_intent_free(sh_mr_intent *in)
{
    size_t i;
    int k;
    if (!in) return;
    for (k = 0; k < SH_MB_PLAYER_FIELDS; ++k) free(in->player[k]);
    for (i = 0; i < in->weapon_count; ++i) { free(in->weapon_slots[i]); free(in->weapon_decls[i]); }
    for (i = 0; i < in->ext_count; ++i) { free(in->ext_targets[i]); free(in->ext_partials[i]); }
    free(in->weapon_slots); free(in->weapon_decls); free(in->ext_targets); free(in->ext_partials);
    memset(in, 0, sizeof *in);
}

int sh_modregistry_intent_any(const sh_mr_intent *in)
{
    int k;
    if (!in) return 0;
    for (k = 0; k < SH_MB_PLAYER_FIELDS; ++k) if (in->player[k]) return 1;
    return in->weapon_count > 0 || in->ext_count > 0;
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

/* A source's extends partial for `target`, or NULL. */
static const char *mr_ext_text(const sh_mr_source *s, const char *target, size_t *len)
{
    size_t i;
    if (len) *len = 0;
    if (!s->intent || !s->ext_texts) return NULL;
    for (i = 0; i < s->intent->ext_count; ++i)
        if (!_stricmp(s->intent->ext_targets[i], target)) {
            if (len) *len = s->ext_lens[i];
            return s->ext_texts[i];
        }
    return NULL;
}

typedef struct mr_rows_ctx { sh_mr_result *r; int source; const sh_mr_source *src; int ok; int skipped; } mr_rows_ctx;

static void mr_row(void *context, const char *ai, const char *def)
{
    mr_rows_ctx *c = (mr_rows_ctx *)context;
    sh_mr_entry *e;
    char *end;
    unsigned long v;
    int d;
    if (!ai[0] || strspn(ai, "0123456789") != strlen(ai)) return;   /* a SPAWN_AI_TYPE_* name: the game's own */
    v = strtoul(ai, &end, 10);
    if (sh_modregistry_stock_bit((unsigned)v)) return;
    if (!def[0]) { c->skipped++; return; }
    for (d = 0; d < c->r->nentries; ++d)
        if (c->r->entries[d].source == c->source && c->r->entries[d].authored == (unsigned)v) return;   /* listed twice */
    if (!mr_grow((void **)&c->r->entries, c->r->nentries, sizeof *e)) { c->ok = 0; return; }
    e = &c->r->entries[c->r->nentries++];
    memset(e, 0, sizeof *e);
    e->source = c->source;
    e->authored = (unsigned)v;
    strncpy_s(e->def, sizeof e->def, def, _TRUNCATE);
    /* The encounter that names the bit orders the entry. */
    for (d = 0; d < c->src->ndecls; ++d) {
        const sh_mr_decl *dc = &c->src->decls[d];
        unsigned nums[8];
        int q, nn;
        if (dc->score) continue;
        nn = mr_custom_numbers(dc->text, dc->len, nums, 8);
        for (q = 0; q < nn; ++q)
            if (nums[q] == (unsigned)v && (!e->encounter[0] || _stricmp(dc->name, e->encounter) < 0))
                strncpy_s(e->encounter, sizeof e->encounter, dc->name, _TRUNCATE);
    }
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

static int mr_conductor(const sh_mr_source *src, int n, const char *stock, size_t stock_len, sh_mr_result *r)
{
    int s, i;
    for (s = 0; s < n; ++s) {
        const sh_mr_source *so = &src[s];
        if (so->carried[SH_MR_CONDUCTOR]) {
            mr_rows_ctx c = { r, s, so, 1, 0 };
            if (sh_modregistry_conductor_rows(so->carried[SH_MR_CONDUCTOR], so->carried_len[SH_MR_CONDUCTOR], mr_row, &c) < 0)
                mr_line(r, "conductor: %s carries a copy with no aiTypeList; nothing taken from it", so->who);
            if (!c.ok) return 0;
            if (c.skipped) mr_line(r, "conductor: %s: %d aiTypeList item(s) with a number but no entityDef skipped", so->who, c.skipped);
        }
        {
            size_t plen = 0;
            const char *part = mr_ext_text(so, MR_CONDUCTOR_TARGET, &plen);
            if (part) {
                mr_rows_ctx c = { r, s, so, 1, 0 };
                if (sh_modregistry_conductor_rows(part, plen, mr_row, &c) < 0)
                    mr_line(r, "conductor: %s's extends partial has no edit.aiTypeList; nothing taken from it", so->who);
                if (!c.ok) return 0;
                if (c.skipped) mr_line(r, "conductor: %s: %d aiTypeList item(s) with a number but no entityDef skipped", so->who, c.skipped);
            }
        }
    }
    if (!r->nentries) return 1;
    qsort(r->entries, (size_t)r->nentries, sizeof *r->entries, mr_entry_cmp);
    /* Bits: the lowest free above the stock ones, in that order. */
    for (i = 0; i < r->nentries; ++i) {
        if (SH_MR_BIT_FIRST + i > SH_MR_BIT_LAST) {
            mr_line(r, "conductor: no free spawn-type bit for %s (%s) of %s: a map holds at most %d custom encounter types; "
                    "skipped", r->entries[i].encounter[0] ? r->entries[i].encounter : "its encounter", r->entries[i].def,
                    src[r->entries[i].source].who, SH_MR_BIT_LAST - SH_MR_BIT_FIRST + 1);
            r->nentries = i;
            break;
        }
        r->entries[i].bit = 1u << (SH_MR_BIT_FIRST + i);
    }
    if (!stock) {
        mr_line(r, "conductor: the game's %s could not be read; the game's own file serves and %d encounter type(s) are "
                "not listed", g_requests[SH_MR_CONDUCTOR], r->nentries);
        r->nentries = 0;
        return 1;
    }
    {
        const char **defs = (const char **)malloc(sizeof(char *) * (size_t)r->nentries);
        unsigned *bits = (unsigned *)malloc(sizeof(unsigned) * (size_t)r->nentries);
        if (!defs || !bits) { free((void *)defs); free(bits); return 0; }
        for (i = 0; i < r->nentries; ++i) { defs[i] = r->entries[i].def; bits[i] = r->entries[i].bit; }
        r->text[SH_MR_CONDUCTOR] = sh_modregistry_conductor_text(stock, stock_len, defs, bits, r->nentries,
                                                                 &r->len[SH_MR_CONDUCTOR]);
        free((void *)defs);
        free(bits);
        if (!r->text[SH_MR_CONDUCTOR]) {
            mr_line(r, "conductor: the game's text has no aiTypeList; the game's own file serves");
            r->nentries = 0;
            return 1;
        }
    }
    for (i = 0; i < r->nentries; ++i)
        mr_line(r, "conductor: aiType %u -> %s (%s, %s%s%u)", r->entries[i].bit, r->entries[i].def,
                src[r->entries[i].source].who, r->entries[i].encounter[0] ? r->entries[i].encounter : "no encounter",
                r->entries[i].encounter[0] ? ", authored " : ", authored ", r->entries[i].authored);
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
            char *t = sh_modregistry_rewrite_bits(src[s].decls[d].text, src[s].decls[d].len, from, to, m, &len);
            if (!t) continue;
            if (!mr_grow((void **)&r->rewrites, r->nrewrites, sizeof *r->rewrites)) { free(t); return 0; }
            r->rewrites[r->nrewrites].id = src[s].decls[d].id;
            r->rewrites[r->nrewrites].text = t;
            r->rewrites[r->nrewrites].len = len;
            r->nrewrites++;
            mr_line(r, "conductor: %s's %s %s served with the assigned bit", src[s].who,
                    src[s].decls[d].score ? "score" : "encounter", src[s].decls[d].name);
        }
    }
    return 1;
}

/* Occurrences of `needle` in the text (the body's alias rows, "alias {"). */
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

/* One line per clip whose rows two sources (or the game and a source) give
 * differently: the clip is served once with the union of its rows. */
typedef struct mr_merge_ctx { sh_mr_result *r; const char *who; int clips; } mr_merge_ctx;

static void mr_body_merged(void *context, const char *key, size_t key_n, int rows, int kept)
{
    mr_merge_ctx *m = (mr_merge_ctx *)context;
    m->clips++;
    if (!rows && !kept)
        mr_line(m->r, "player body: %.*s: the clip already holds every row %s gives", (int)key_n, key, m->who);
    else
        mr_line(m->r, "player body: %.*s: %d event row%s from %s merged into the clip the body already holds%s",
                (int)key_n, key, rows, rows == 1 ? "" : "s", m->who,
                kept ? " (a value both set otherwise keeps the earlier one)" : "");
}

static int mr_body(const sh_mr_source *src, int n, const char *stock, size_t stock_len, sh_mr_result *r)
{
    char *cur = NULL;
    size_t cur_len = 0;
    int s, any = 0;
    for (s = 0; s < n; ++s) any |= src[s].body_events || src[s].carried[SH_MR_BODY] ||
                                   (src[s].intent && src[s].intent->player[SH_MB_PLAYER_BODY_EVENTS]) ||
                                   mr_ext_text(&src[s], MR_BODY_TARGET, NULL);
    if (!any) return 1;
    if (!stock) {
        mr_line(r, "player body: the game's %s could not be read; the game's own file serves", g_requests[SH_MR_BODY]);
        return 1;
    }
    for (s = 0; s < n; ++s) {
        const sh_mr_source *so = &src[s];
        const char *base = cur ? cur : stock;
        size_t base_len = cur ? cur_len : stock_len, len = 0;
        int added = 0;
        char *next = NULL;
        size_t plen = 0;
        const char *part = so->body_events ? so->body_events : mr_ext_text(so, MR_BODY_TARGET, &plen);
        if (so->body_events) plen = so->body_events_len;
        if (so->intent && so->intent->player[SH_MB_PLAYER_BODY_EVENTS] && !so->body_events) {
            mr_line(r, "player body: %s names bodyEvents %s, which it does not serve; skipped", so->who,
                    so->intent->player[SH_MB_PLAYER_BODY_EVENTS]);
        }
        if (part) {
            mr_merge_ctx mc = { r, so->who, 0 };
            next = sh_modserve_compose_extend_rows(base, base_len, part, plen, MR_BODY_CONTAINERS, &len, &added,
                                                   mr_body_merged, &mc);
            if (next) r->body_merged += mc.clips;
            if (!next)
                mr_line(r, "player body: %s's bodyEvents %s do not append to the body (a text outside the md6Def "
                        "grammar, or members outside events and aliases); skipped", so->who,
                        so->intent ? so->intent->player[SH_MB_PLAYER_BODY_EVENTS] : "");
        } else if (so->carried[SH_MR_BODY]) {
            const char *texts[2];
            size_t lens[2];
            texts[0] = base; lens[0] = base_len;
            texts[1] = so->carried[SH_MR_BODY]; lens[1] = so->carried_len[SH_MR_BODY];
            next = sh_modserve_compose_texts(texts, lens, 2, &len, &added);
            if (!next)
                mr_line(r, "player body: %s carries a copy of the body that is not the game's plus entries; skipped "
                        "(rebuild it with player.bodyEvents)", so->who);
        }
        if (!next) continue;
        free(cur);
        cur = next;
        cur_len = len;
        r->body_added += added;
        r->body_sources++;
        mr_line(r, "player body: %d entr%s from %s%s", added, added == 1 ? "y" : "ies", so->who,
                part ? "" : " (its carried copy)");
    }
    if (cur) {
        /* The merged body's totals, so the doctor shows what the packs made. */
        int clips = sh_modregistry_body_clips(cur, cur_len), aliases = mr_count(cur, cur_len, "alias {");
        mr_line(r, "player body: the merged body holds %d clip%s and %d alias%s, %zu bytes, from %d source%s", clips,
                clips == 1 ? "" : "s", aliases, aliases == 1 ? "" : "es", cur_len, r->body_sources,
                r->body_sources == 1 ? "" : "s");
    }
    r->text[SH_MR_BODY] = cur;
    r->len[SH_MR_BODY] = cur_len;
    return 1;
}

/* The extends targets other than the fixed registries: each source's partial appended in order. */
static int mr_extend(const sh_mr_source *src, int n, const sh_mr_stock *ext, int next, sh_mr_result *r)
{
    int t, s;
    size_t q;
    /* A target that is not on the list, or a partial that is not served, is named once. */
    for (s = 0; s < n; ++s)
        for (q = 0; src[s].intent && q < src[s].intent->ext_count; ++q) {
            char cause[300];
            if (!sh_modregistry_extend_target(src[s].intent->ext_targets[q], NULL, NULL, NULL, cause, sizeof cause))
                mr_line(r, "extends: %s: %s; skipped", src[s].who, cause);
            else if (!src[s].ext_texts || !src[s].ext_texts[q])
                mr_line(r, "extends %s: %s names %s, which it does not serve; skipped", src[s].intent->ext_targets[q],
                        src[s].who, src[s].intent->ext_partials[q]);
        }
    if (next <= 0) return 1;
    r->ext_text = (char **)calloc((size_t)next, sizeof(char *));
    r->ext_len = (size_t *)calloc((size_t)next, sizeof(size_t));
    if (!r->ext_text || !r->ext_len) return 0;
    r->next = next;
    for (t = 0; t < next; ++t) {
        int kind = 0, fixed = -1, total = 0;
        const char *containers = NULL;
        char *cur = NULL;
        size_t cur_len = 0;
        if (!sh_modregistry_extend_target(ext[t].target, &kind, &containers, &fixed, NULL, 0) || fixed >= 0) continue;
        for (s = 0; s < n; ++s) {
            size_t plen = 0, len = 0;
            const char *part = mr_ext_text(&src[s], ext[t].target, &plen);
            int added = 0;
            char *nx;
            if (!part) continue;
            if (!ext[t].text) {
                mr_line(r, "extends %s: the game's text could not be read; the game's own file serves", ext[t].target);
                break;
            }
            nx = sh_modserve_compose_extend(cur ? cur : ext[t].text, cur ? cur_len : ext[t].len, part, plen, containers,
                                            kind, &len, &added);
            if (!nx) {
                mr_line(r, "extends %s: %s's partial does not extend it (members outside %s, an entry the game's text "
                        "holds with other content, or a text outside the decl grammar); skipped", ext[t].target,
                        src[s].who, containers);
                continue;
            }
            free(cur);
            cur = nx;
            cur_len = len;
            total += added;
            mr_line(r, "extends %s: %d entr%s from %s", ext[t].target, added, added == 1 ? "y" : "ies", src[s].who);
        }
        r->ext_text[t] = cur;
        r->ext_len[t] = cur_len;
        (void)total;
    }
    return 1;
}

static const char *mr_choice(const sh_mr_choice *choices, int nchoices, const char *slot)
{
    int i;
    for (i = 0; i < nchoices; ++i) if (choices[i].slot && choices[i].key && !_stricmp(choices[i].slot, slot)) return choices[i].key;
    return NULL;
}

/* The source serving a slot several sources set: the map's choice, else the first. */
static int mr_pick(const sh_mr_source *src, const int *setters, int ns, const char *slot, const sh_mr_choice *choices,
                   int nchoices, sh_mr_result *r)
{
    const char *want = mr_choice(choices, nchoices, slot);
    char who[400];
    size_t at = 0;
    int i, pick = setters[0], chosen = 0;
    if (ns == 1) return pick;
    if (want)
        for (i = 0; i < ns; ++i) if (!_stricmp(src[setters[i]].key, want)) { pick = setters[i]; chosen = 1; break; }
    who[0] = 0;
    for (i = 0; i < ns && at + 2 < sizeof who; ++i) {
        int w = _snprintf_s(who + at, sizeof who - at, _TRUNCATE, "%s%s", i ? ", " : "", src[setters[i]].who);
        if (w < 0) break;
        at += (size_t)w;
    }
    if (chosen) mr_line(r, "%s: set by %s; serving %s (the map's choice)", slot, who, src[pick].who);
    else if (want)
        mr_line(r, "%s: set by %s; the map chose %s, which does not set it; serving %s (row order)", slot, who, want,
                src[pick].who);
    else
        mr_line(r, "%s: set by %s; serving %s (row order; choose with sh_mods choose \"%s\" \"<key>\")", slot, who,
                src[pick].who, slot);
    return pick;
}

static int mr_player(const sh_mr_source *src, int n, const char *stock, size_t stock_len, const sh_mr_choice *choices,
                     int nchoices, sh_mr_result *r)
{
    const char *values[SH_MB_PLAYER_FIELDS];
    const char *base = stock;
    size_t base_len = stock_len;
    int s, f, carried = -1;
    int *setters = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
    if (!setters) return 0;
    memset((void *)values, 0, sizeof values);
    for (f = 0; f < SH_MB_PLAYER_FIELDS; ++f) r->player_from[f] = -1;
    for (s = 0; s < n; ++s) {
        if (!src[s].carried[SH_MR_PLAYER]) continue;
        if (carried < 0) {
            carried = s;
            base = src[s].carried[SH_MR_PLAYER];
            base_len = src[s].carried_len[SH_MR_PLAYER];
            mr_line(r, "player entity: %s carries a whole copy; it is served as the base the other player fields edit "
                    "(rebuild it with player intent)", src[s].who);
        } else
            mr_line(r, "player entity: %s also carries a whole copy; %s's serves (row order)", src[s].who,
                    src[carried].who);
    }
    for (f = 0; f < SH_MB_PLAYER_FIELDS; ++f) {
        char slot[64];
        int ns = 0, pick;
        if (!g_player_keys[f][0]) continue;
        for (s = 0; s < n; ++s) if (src[s].intent && src[s].intent->player[f]) setters[ns++] = s;
        if (!ns) continue;
        _snprintf_s(slot, sizeof slot, _TRUNCATE, "player.%s", sh_mb_player_fields[f]);
        pick = mr_pick(src, setters, ns, slot, choices, nchoices, r);
        values[f] = src[pick].intent->player[f];
        r->player_value[f] = values[f];
        r->player_from[f] = pick;
        r->player_set++;
    }
    free(setters);
    if (!r->player_set && carried < 0) return 1;
    if (!base) {
        mr_line(r, "player entity: the game's %s could not be read; the game's own file serves", g_requests[SH_MR_PLAYER]);
        r->player_set = 0;
        return 1;
    }
    if (!r->player_set) {
        r->text[SH_MR_PLAYER] = (char *)malloc(base_len + 1);
        if (!r->text[SH_MR_PLAYER]) return 0;
        memcpy(r->text[SH_MR_PLAYER], base, base_len);
        r->text[SH_MR_PLAYER][base_len] = 0;
        r->len[SH_MR_PLAYER] = base_len;
        return 1;
    }
    r->text[SH_MR_PLAYER] = sh_modregistry_player_text(base, base_len, values, &r->len[SH_MR_PLAYER]);
    if (!r->text[SH_MR_PLAYER]) {
        mr_line(r, "player entity: its text has no edit block; the game's own file serves");
        r->player_set = 0;
        return 1;
    }
    for (f = 0; f < SH_MB_PLAYER_FIELDS; ++f)
        if (values[f]) mr_line(r, "player entity: %s = \"%s\" (%s)", g_player_keys[f], values[f], src[r->player_from[f]].who);
    return 1;
}

static int mr_weapons(const sh_mr_source *src, int n, const sh_mr_choice *choices, int nchoices, sh_mr_result *r)
{
    int s, *setters, *idx;
    size_t q;
    setters = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
    idx = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
    if (!setters || !idx) { free(setters); free(idx); return 0; }
    for (s = 0; s < n; ++s) {
        if (!src[s].intent) continue;
        for (q = 0; q < src[s].intent->weapon_count; ++q) {
            const char *slot = src[s].intent->weapon_slots[q];
            char key[SH_MR_SLOT_MAX];
            int k, ns = 0, pick, t, done = 0;
            for (k = 0; k < r->nweapons && !done; ++k) done = !_stricmp(r->weapons[k].slot + 8, slot);
            if (done) continue;
            for (t = s; t < n; ++t) {
                size_t u;
                if (!src[t].intent) continue;
                for (u = 0; u < src[t].intent->weapon_count; ++u)
                    if (!_stricmp(src[t].intent->weapon_slots[u], slot)) {
                        if (!src[t].weapon_files || src[t].weapon_files[u] < 0) {
                            mr_line(r, "weapons/%s: %s names %s, which it does not serve; skipped", slot, src[t].who,
                                    src[t].intent->weapon_decls[u]);
                            break;
                        }
                        setters[ns] = t;
                        idx[ns++] = (int)u;
                        break;
                    }
            }
            if (!ns) continue;
            _snprintf_s(key, sizeof key, _TRUNCATE, "weapons/%s", slot);
            pick = mr_pick(src, setters, ns, key, choices, nchoices, r);
            for (k = 0; k < ns; ++k) if (setters[k] == pick) break;
            if (!mr_grow((void **)&r->weapons, r->nweapons, sizeof *r->weapons)) { free(setters); free(idx); return 0; }
            strncpy_s(r->weapons[r->nweapons].slot, sizeof r->weapons[0].slot, key, _TRUNCATE);
            r->weapons[r->nweapons].source = pick;
            r->weapons[r->nweapons].file = src[pick].weapon_files[idx[k]];
            r->nweapons++;
            mr_line(r, "%s: served from %s's %s", key, src[pick].who, src[pick].intent->weapon_decls[idx[k]]);
        }
    }
    free(setters);
    free(idx);
    return 1;
}

int sh_modregistry_generate(const sh_mr_source *src, int n, const char *const stock[SH_MR_FIXED],
                            const size_t stock_len[SH_MR_FIXED], const sh_mr_stock *ext, int next,
                            const sh_mr_choice *choices, int nchoices, sh_mr_result *out)
{
    static const char *const none[SH_MR_FIXED] = { NULL, NULL, NULL };
    static const size_t zero[SH_MR_FIXED] = { 0, 0, 0 };
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!stock) stock = none;
    if (!stock_len) stock_len = zero;
    if (!mr_conductor(src, n, stock[SH_MR_CONDUCTOR], stock_len[SH_MR_CONDUCTOR], out) ||
        !mr_body(src, n, stock[SH_MR_BODY], stock_len[SH_MR_BODY], out) ||
        !mr_player(src, n, stock[SH_MR_PLAYER], stock_len[SH_MR_PLAYER], choices, nchoices, out) ||
        !mr_weapons(src, n, choices, nchoices, out) || !mr_extend(src, n, ext, next, out)) {
        sh_modregistry_result_free(out);
        return 0;
    }
    (void)g_names;
    return 1;
}

void sh_modregistry_result_free(sh_mr_result *r)
{
    int i;
    if (!r) return;
    for (i = 0; i < SH_MR_FIXED; ++i) free(r->text[i]);
    for (i = 0; i < r->nrewrites; ++i) free(r->rewrites[i].text);
    for (i = 0; i < r->nlines; ++i) free(r->lines[i]);
    for (i = 0; i < r->next; ++i) free(r->ext_text[i]);
    free(r->ext_text); free(r->ext_len);
    free(r->entries); free(r->weapons); free(r->rewrites); free(r->lines);
    memset(r, 0, sizeof *r);
}

/* ------------------------------------------------------------------ choices */

int sh_modregistry_slot_valid(const char *slot)
{
    int k;
    if (!slot || strlen(slot) >= SH_MR_SLOT_MAX) return 0;
    if (!strncmp(slot, "weapons/", 8)) {
        char req[SH_MB_REQUEST_MAX + 1];
        return slot[8] && sh_modregistry_weapon_request(slot + 8, req, sizeof req);
    }
    if (strncmp(slot, "player.", 7)) return 0;
    for (k = 0; k < SH_MB_PLAYER_FIELDS; ++k)
        if (g_player_keys[k][0] && !strcmp(slot + 7, sh_mb_player_fields[k])) return 1;
    return 0;
}

