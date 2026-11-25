/* modregistry.c -- see modregistry.h. The per-map apply of the declared
 * bundles' derived edits (decl_tree.c) and the conductor's spawn-type bits.
 * No engine calls and no index access: the serving index (modserve.c)
 * collects the sources and serves the result. */
#include <windows.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/text.h"
#include "modregistry.h"
#include "decl_tree.h"

int sh_modregistry_stock_bit(unsigned value)
{
    return (value && value <= (1u << 18) && !(value & (value - 1))) || value == (1u << 30);
}

const char *const SH_MR_GROWN_LISTS[SH_MR_GROWN_LIST_COUNT] = {
    "generated/decls/snappropertyinspector_whitelistencounterdecl/single_encounters_white_list_inspector.decl",
    "generated/decls/snappropertyinspector_whitelistencounterdecllist/single_encounters_white_list_list_inspector.decl",
    "generated/decls/snappropertyinspector_whitelistencounterdecllist/boss_encounters_white_list_list_inspector.decl",
};

/* spawnAIType_t, 13954591 (reference/enumvals_schema.json; the four readers
 * of an AI's type treat it as 32 bits). */
static const struct { const char *name; unsigned bit; } MR_SPAWN_FLAGS[] = {
    { "SPAWN_AI_TYPE_ZOMBIE_SCIENTIST", 1u << 0 }, { "SPAWN_AI_TYPE_ZOMBIE_SECURITY", 1u << 1 },
    { "SPAWN_AI_TYPE_IMP", 1u << 2 }, { "SPAWN_AI_TYPE_HELL_SOLDIER", 1u << 3 },
    { "SPAWN_AI_TYPE_SHOTGUN_SOLDIER", 1u << 4 }, { "SPAWN_AI_TYPE_BEAM_DEMON", 1u << 5 },
    { "SPAWN_AI_TYPE_HELL_KNIGHT", 1u << 6 }, { "SPAWN_AI_TYPE_BARON", 1u << 7 },
    { "SPAWN_AI_TYPE_MANCUBUS", 1u << 8 }, { "SPAWN_AI_TYPE_CYBER_MANCUBUS", 1u << 9 },
    { "SPAWN_AI_TYPE_ARCHVILE", 1u << 10 }, { "SPAWN_AI_TYPE_REVENANT", 1u << 11 },
    { "SPAWN_AI_TYPE_CACODEMON", 1u << 12 }, { "SPAWN_AI_TYPE_PINKY", 1u << 13 },
    { "SPAWN_AI_TYPE_SPECTRE", 1u << 14 }, { "SPAWN_AI_TYPE_LOST_SOUL", 1u << 15 },
    { "SPAWN_AI_TYPE_ZOMBIE_WELDER", 1u << 16 }, { "SPAWN_AI_TYPE_ANY_ZOMBIE", 1u << 17 },
    { "SPAWN_AI_TYPE_ZOMBIE_UNWILLING", 1u << 18 }, { "SPAWN_AI_TYPE_ANY", 1u << 30 },
};

int sh_modregistry_spawn_flag(const char *word, size_t n, unsigned *bit)
{
    size_t i;
    for (i = 0; word && i < sizeof MR_SPAWN_FLAGS / sizeof MR_SPAWN_FLAGS[0]; ++i)
        if (strlen(MR_SPAWN_FLAGS[i].name) == n && !_strnicmp(word, MR_SPAWN_FLAGS[i].name, n)) {
            if (bit) *bit = MR_SPAWN_FLAGS[i].bit;
            return 1;
        }
    return 0;
}

int sh_modregistry_type_words(const char *value, size_t n, unsigned *own, unsigned *standin)
{
    size_t i = 0;
    unsigned mine = 0, like = 0;
    while (value && i < n) {
        size_t s, m;
        while (i < n && (value[i] == ' ' || value[i] == '\t')) i++;
        s = i;
        while (i < n && value[i] != ' ' && value[i] != '\t') i++;
        m = i - s;
        if (!m) continue;
        if (m < 11 && strspn(value + s, "0123456789") >= m) {
            char nb[16];
            unsigned v;
            memcpy(nb, value + s, m);
            nb[m] = 0;
            v = (unsigned)strtoul(nb, NULL, 10);
            if (sh_modregistry_stock_bit(v)) like |= v;
            else if (v && !mine) mine = v;
        } else {
            unsigned b = 0;
            if (sh_modregistry_spawn_flag(value + s, m, &b)) like |= b;
        }
    }
    if (own) *own = mine;
    if (standin) *standin = like;
    return mine != 0;
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
    char *ai = NULL, *def = NULL;
    mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
    if (!text || !mr_find_block(text, len, 0, len, "aiTypeList", 2, &open, &close)) return -1;
    pos = open + 1;
    for (;;) {
        mr_tok k = mr_next(text, close, &pos);
        if (k.kind == MT_END) break;
        if (k.kind == MT_LB) { if (++depth == 1) { free(ai); free(def); ai = def = NULL; } }
        else if (k.kind == MT_RB) {
            if (depth-- == 1) { if (fn) fn(context, ai ? ai : "", def ? def : ""); count++; }
        } else if (k.kind == MT_STR && depth == 1 && prev.kind == MT_EQ && prev2.kind == MT_WORD) {
            size_t n = k.n >= 2 ? k.n - 2 : 0;
            char **dst = NULL;
            /* The values whole, whatever their length. */
            if (prev2.n == 6 && !_strnicmp(text + prev2.at, "aiType", 6)) dst = &ai;
            else if (prev2.n == 9 && !_strnicmp(text + prev2.at, "entityDef", 9)) dst = &def;
            if (dst) { free(*dst); *dst = sh_textn(text + k.at + 1, n); }
        }
        prev2 = prev;
        prev = k;
    }
    free(ai);
    free(def);
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

/* Inside a JSON string: the length of a blank at `i` (a space, or an escaped
 * tab, newline or return), 0 for none. */
static size_t mr_json_blank(const char *t, size_t len, size_t i)
{
    if (i < len && t[i] == ' ') return 1;
    if (i + 1 < len && t[i] == '\\' && (t[i + 1] == 't' || t[i + 1] == 'n' || t[i + 1] == 'r')) return 2;
    return 0;
}

char *sh_modregistry_rewrite_edits(const char *edits, size_t len, const unsigned *from, const unsigned *to, int n,
                                   size_t *out_len)
{
    size_t i = 0, copied = 0;
    int changed = 0;
    mr_buf b = { NULL, 0, 0, 0 };
    if (!edits || n <= 0) return NULL;
    while (i < len) {
        static const char *const keys[] = { "memberType", "aiType" };
        size_t kn = 0, j, w;
        int q;
        for (q = 0; q < 2 && !kn; ++q) {
            size_t l = strlen(keys[q]);
            if (i + l <= len && !_strnicmp(edits + i, keys[q], l) &&
                (i == 0 || !(isalnum((unsigned char)edits[i - 1]) || edits[i - 1] == '_') ||
                 (i >= 2 && edits[i - 2] == '\\' && strchr("tnr", edits[i - 1]))) &&
                (i + l == len || !(isalnum((unsigned char)edits[i + l]) || edits[i + l] == '_')))
                kn = l;
        }
        if (!kn) { i++; continue; }
        j = i + kn;
        while ((w = mr_json_blank(edits, len, j)) != 0) j += w;
        if (j >= len || edits[j] != '=') { i = j > i ? j : i + 1; continue; }
        j++;
        while ((w = mr_json_blank(edits, len, j)) != 0) j += w;
        if (j + 1 >= len || edits[j] != '\\' || edits[j + 1] != '"') { i = j; continue; }
        j += 2;
        /* The value's words up to its closing \" */
        while (j < len && !(edits[j] == '\\' && j + 1 < len && edits[j + 1] == '"')) {
            size_t s = j, m;
            if ((w = mr_json_blank(edits, len, j)) != 0) { j += w; continue; }
            while (j < len && edits[j] != ' ' && edits[j] != '\\') j++;
            m = j - s;
            if (m && m < 11 && strspn(edits + s, "0123456789") >= m) {
                char nb[16];
                unsigned v;
                int k;
                memcpy(nb, edits + s, m);
                nb[m] = 0;
                v = (unsigned)strtoul(nb, NULL, 10);
                for (k = 0; k < n; ++k)
                    if (from[k] == v && to[k] != v) {
                        char out[16];
                        mr_put(&b, edits + copied, s - copied);
                        _snprintf_s(out, sizeof out, _TRUNCATE, "%u", to[k]);
                        mr_puts(&b, out);
                        copied = j;
                        changed = 1;
                        break;
                    }
            }
            if (!m && j < len && edits[j] == '\\') j += 2;   /* another escape inside the value */
        }
        i = j;
    }
    if (!changed) { free(b.p); return NULL; }
    mr_put(&b, edits + copied, len - copied);
    return mr_done(&b, out_len);
}

char *sh_modregistry_append_list(const char *text, size_t len, const char *list, const char *const *names, int n,
                                 size_t *out_len)
{
    size_t open, close, pos, line_start, num_at = 0, num_len = 0;
    int count = 0, added = 0, k, num = -1;
    char indent[64];
    mr_buf b = { NULL, 0, 0, 0 };
    mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
    if (!text || !list || n <= 0 || !mr_find_block(text, len, 0, len, list, 2, &open, &close)) return NULL;
    /* The list's num, its items' count and their indentation. */
    indent[0] = 0;
    pos = open + 1;
    for (;;) {
        mr_tok t = mr_next(text, close, &pos);
        if (t.kind == MT_END) break;
        if (t.kind == MT_WORD && prev.kind == MT_EQ && mr_word_is(text, &prev2, "num")) {
            num_at = t.at;
            num_len = t.n;
            num = atoi(text + t.at);
        }
        if (t.kind == MT_WORD && t.n > 5 && !_strnicmp(text + t.at, "item[", 5)) {
            size_t s = t.at;
            count++;
            while (s > open && text[s - 1] != '\n') s--;
            if (t.at - s < sizeof indent) { memcpy(indent, text + s, t.at - s); indent[t.at - s] = 0; }
        }
        prev2 = prev;
        prev = t;
    }
    if (!indent[0]) {          /* an empty list: one level deeper than its closing brace */
        size_t s = close;
        while (s > open && text[s - 1] != '\n') s--;
        if (close - s + 1 < sizeof indent) { memcpy(indent, text + s, close - s); indent[close - s] = '\t'; indent[close - s + 1] = 0; }
    }
    /* New items go on their own lines before the closing brace's line, or just
     * before the brace when the list closes on a line of its own content. */
    line_start = close;
    while (line_start > open && text[line_start - 1] != '\n') line_start--;
    for (pos = line_start; pos < close; ++pos)
        if (text[pos] != ' ' && text[pos] != '\t') { line_start = close; break; }
    if (num < 0) num = count;
    {
        mr_buf items = { NULL, 0, 0, 0 };
        int inline_close = line_start == close && (close == 0 || text[close - 1] != '\n');
        for (k = 0; k < n; ++k) {
            const char *nm = names[k];
            size_t l = nm ? strlen(nm) : 0, at;
            int held = 0, d;
            char line[640];
            if (!l) continue;
            for (at = open; at + l + 2 <= close && !held; ++at)
                held = text[at] == '"' && text[at + l + 1] == '"' && !_strnicmp(text + at + 1, nm, l);
            for (d = 0; d < k && !held; ++d) held = names[d] && !_stricmp(names[d], nm);
            if (held) continue;
            if (inline_close) _snprintf_s(line, sizeof line, _TRUNCATE, "item[%d] = \"%s\"; ", num + added, nm);
            else _snprintf_s(line, sizeof line, _TRUNCATE, "%sitem[%d] = \"%s\";\n", indent, num + added, nm);
            mr_puts(&items, line);
            added++;
        }
        if (!added) { free(items.p); return NULL; }
        if (num_at && num_at < line_start) {
            char nb[16];
            _snprintf_s(nb, sizeof nb, _TRUNCATE, "%d", num + added);
            mr_put(&b, text, num_at);
            mr_puts(&b, nb);
            mr_put(&b, text + num_at + num_len, line_start - (num_at + num_len));
        } else mr_put(&b, text, line_start);
        mr_put(&b, items.p, items.n);
        free(items.p);
    }
    mr_put(&b, text + line_start, len - line_start);
    return mr_done(&b, out_len);
}

const char *sh_modregistry_flag_name(unsigned bit, char *buf, size_t cap)
{
    int b;
    for (b = SH_MR_BIT_FIRST; b <= SH_MR_BIT_LAST; ++b)
        if (bit == (1u << b)) {
            if (!buf || !cap) return NULL;
            _snprintf_s(buf, cap, _TRUNCATE, "SPAWN_AI_TYPE_MOD_%d", b);
            return buf;
        }
    return NULL;
}

int sh_modregistry_decl_string(const char *text, size_t len, const char *key, char *buf, size_t cap)
{
    size_t pos = 0;
    mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
    if (buf && cap) buf[0] = 0;
    if (!text || !key || !buf || !cap) return 0;
    for (;;) {
        mr_tok k = mr_next(text, len, &pos);
        if (k.kind == MT_END) return 0;
        if (k.kind == MT_STR && prev.kind == MT_EQ && mr_word_is(text, &prev2, key)) {
            size_t n = k.n >= 2 ? k.n - 2 : 0;
            if (n >= cap) n = cap - 1;
            memcpy(buf, text + k.at + 1, n);
            buf[n] = 0;
            return 1;
        }
        prev2 = prev;
        prev = k;
    }
}

int sh_modregistry_placeable_encounter(const char *text, size_t len, char *buf, size_t cap)
{
    char cls[64];
    if (buf && cap) buf[0] = 0;
    if (!sh_modregistry_decl_string(text, len, "class", cls, sizeof cls) || _stricmp(cls, "idPlaceableSnapAIEncounter"))
        return 0;
    return sh_modregistry_decl_string(text, len, "encounterToSpawn", buf, cap) && buf[0];
}

char *sh_modregistry_append_flags(const char *text, size_t len, const sh_mr_flag *flags, int n, size_t *out_len)
{
    size_t open, close, pos, line_start, num_at = 0, num_len = 0;
    int count = 0, added = 0, k, num = -1;
    char indent[64];
    mr_buf b = { NULL, 0, 0, 0 }, items = { NULL, 0, 0, 0 };
    mr_tok prev = { MT_END, 0, 0 }, prev2 = { MT_END, 0, 0 };
    if (!text || !flags || n <= 0 || !mr_find_block(text, len, 0, len, "values", 2, &open, &close)) return NULL;
    indent[0] = 0;
    pos = open + 1;
    for (;;) {
        mr_tok t = mr_next(text, close, &pos);
        if (t.kind == MT_END) break;
        if (t.kind == MT_WORD && prev.kind == MT_EQ && mr_word_is(text, &prev2, "num")) {
            num_at = t.at;
            num_len = t.n;
            num = atoi(text + t.at);
        }
        if (t.kind == MT_WORD && t.n > 5 && !_strnicmp(text + t.at, "item[", 5)) {
            size_t s = t.at;
            count++;
            while (s > open && text[s - 1] != '\n') s--;
            if (t.at - s < sizeof indent) { memcpy(indent, text + s, t.at - s); indent[t.at - s] = 0; }
        }
        prev2 = prev;
        prev = t;
    }
    if (!indent[0]) {          /* an empty list: one level deeper than its closing brace */
        size_t s = close;
        while (s > open && text[s - 1] != '\n') s--;
        if (close - s + 1 < sizeof indent) { memcpy(indent, text + s, close - s); indent[close - s] = '\t'; indent[close - s + 1] = 0; }
    }
    /* New items go on their own lines before the closing brace's line. */
    line_start = close;
    while (line_start > open && text[line_start - 1] != '\n') line_start--;
    for (pos = line_start; pos < close; ++pos)
        if (text[pos] != ' ' && text[pos] != '\t') return NULL;
    if (num < 0) num = count;
    for (k = 0; k < n; ++k) {
        const char *v = flags[k].value;
        size_t l = v ? strlen(v) : 0, at;
        int held = 0, d;
        char line[1024];
        if (!l) continue;
        for (at = open; at + l + 2 <= close && !held; ++at)
            held = text[at] == '"' && text[at + l + 1] == '"' && !_strnicmp(text + at + 1, v, l);
        for (d = 0; d < k && !held; ++d) held = flags[d].value && !_stricmp(flags[d].value, v);
        if (held) continue;
        _snprintf_s(line, sizeof line, _TRUNCATE, "%sitem[%d] = {\n%s\tenumValue = \"%s\";\n", indent, num + added,
                    indent, v);
        mr_puts(&items, line);
        if (flags[k].label && flags[k].label[0]) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "%s\tlabel = \"%s\";\n", indent, flags[k].label);
            mr_puts(&items, line);
        }
        if (flags[k].description && flags[k].description[0]) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "%s\tdescription = \"%s\";\n", indent, flags[k].description);
            mr_puts(&items, line);
        }
        _snprintf_s(line, sizeof line, _TRUNCATE, "%s}\n", indent);
        mr_puts(&items, line);
        added++;
    }
    if (!added || items.oom) { free(items.p); return NULL; }
    if (num_at && num_at < line_start) {
        char nb[16];
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%d", num + added);
        mr_put(&b, text, num_at);
        mr_puts(&b, nb);
        mr_put(&b, text + num_at + num_len, line_start - (num_at + num_len));
    } else mr_put(&b, text, line_start);
    mr_put(&b, items.p, items.n);
    free(items.p);
    mr_put(&b, text + line_start, len - line_start);
    return mr_done(&b, out_len);
}

/* ------------------------------------------------------------------ generation */

static void mr_line(sh_mr_result *r, const char *fmt, ...)
{
    char *buf;
    va_list ap;
    char **g;
    va_start(ap, fmt);
    buf = sh_vtextf(fmt, ap);
    va_end(ap);
    if (!buf) return;
    g = (char **)realloc(r->lines, sizeof(char *) * (size_t)(r->nlines + 1));
    if (!g) { free(buf); return; }
    r->lines = g;
    r->lines[r->nlines++] = buf;
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
    c = _stricmp(x->encounter ? x->encounter : x->def, y->encounter ? y->encounter : y->def);
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
    k = &c->r->clashes[c->r->nclashes];
    memset(k, 0, sizeof *k);
    if (!(k->slot = mr_strdup(slot))) return;
    c->r->nclashes++;
    k->target = c->target;
    for (i = 0; i < n && i < SH_MR_CLASH_SOURCES; ++i) k->sources[k->n++] = sources[i];
    k->serving = serving;
    k->by_choice = by_choice;
}

/* A scalar's value without its quotes, whole (malloc'd), or NULL. */
static char *mr_unquote(const sh_dt_node *n)
{
    const char *v = n && !n->block ? n->val : "";
    size_t len = n && !n->block ? n->val_n : 0;
    if (len >= 2 && v[0] == '"' && v[len - 1] == '"') { v++; len -= 2; }
    return sh_textn(v, len);
}

/* The conductor's rule: each aiTypeList item a source added whose aiType
 * holds a number that is not a stock bit gets a bit assigned per map (its
 * stock words are the demon it stands in for), and that source's encounters
 * and scores are served with it. */
/* The bit a map pinned for a source's encounter (a choice SH_MR_PIN "<bit>/<encounter>" keyed by the source's
 * salt), 0 for none. */
static unsigned mr_pin(const sh_mr_choice *choices, int nchoices, const char *key, const char *encounter)
{
    size_t pl = strlen(SH_MR_PIN);
    int i;
    for (i = 0; encounter && key && i < nchoices; ++i) {
        const char *s = choices[i].slot;
        char *end;
        long b;
        if (!s || !choices[i].key || _strnicmp(s, SH_MR_PIN, pl) || _stricmp(choices[i].key, key)) continue;
        b = strtol(s + pl, &end, 10);
        if (*end != '/' || b < SH_MR_BIT_FIRST || b > SH_MR_BIT_LAST || _stricmp(end + 1, encounter)) continue;
        return 1u << b;
    }
    return 0;
}

/* Every member of this encounter requests exactly this custom type. Mixed encounters retain their other members. */
static int mr_only_member(const sh_mr_decl *d, unsigned bit)
{
    size_t pos = 0;
    mr_tok prev = { MT_END, 0, 0 }, field = prev;
    int seen = 0;
    char value[16];
    int len = _snprintf_s(value, sizeof value, _TRUNCATE, "%u", bit);
    for (;;) {
        mr_tok t = mr_next(d->text, d->len, &pos);
        if (t.kind == MT_END) return seen;
        if (t.kind == MT_STR && prev.kind == MT_EQ && mr_word_is(d->text, &field, "memberType")) {
            if (t.n != (size_t)len + 2 || memcmp(d->text + t.at + 1, value, (size_t)len)) return 0;
            seen = 1;
        }
        field = prev; prev = t;
    }
}

/* Keep native registrations and remove only added entries whose selected entity cannot load. */
static void mr_drop_unavailable(sh_dt *t, const sh_mr_source *src, int n, int authored)
{
    sh_dt_node *list = sh_dt_at(sh_dt_root(t), "edit/aiTypeList");
    int i;
    if (!list) return;
    for (i = list->nkids - 1; i >= 0; --i) {
        sh_dt_node *it = list->kids[i];
        char *name;
        if (!it->block || it->src < 0 || it->src >= n || !src[it->src].entity_unavailable) continue;
        name = mr_unquote(sh_dt_child(it, "entityDef"));
        if (name && src[it->src].entity_unavailable(src[it->src].context, name)) {
            const sh_mr_source *s = &src[it->src];
            if (authored && s->single_actor) {
                char *a = mr_unquote(sh_dt_child(it, "aiType"));
                unsigned bit = 0, like = 0;
                if (a && sh_modregistry_type_words(a, strlen(a), &bit, &like) && bit) {
                    int k;
                    for (k = 0; k < s->ndecls; ++k)
                        if (!s->decls[k].score && mr_only_member(&s->decls[k], bit))
                            s->single_actor(s->context, s->decls[k].id, name);
                }
                free(a);
            }
            sh_dt_remove_child(list, i);
        }
        free(name);
    }
}

static int mr_conductor(sh_dt *t, const sh_mr_source *src, int n, const sh_mr_choice *choices, int nchoices,
                        sh_mr_result *r)
{
    unsigned taken = 0;
    sh_dt_node *list = sh_dt_at(sh_dt_root(t), "edit/aiTypeList");
    sh_dt_node **items = NULL;
    int i, s, nitems = 0;
    if (!list) { mr_line(r, "conductor: the generated text has no edit/aiTypeList; no spawn-type bit is assigned"); return 1; }
    items = (sh_dt_node **)calloc((size_t)(list->nkids + 1), sizeof *items);
    if (!items) return 0;
    for (i = 0; i < list->nkids; ++i) {
        sh_dt_node *it = list->kids[i], *ai, *def;
        char *a, *d;
        unsigned v, own = 0, like = 0;
        int q, dup = -1;
        if (!it->block || it->src < 0) continue;
        ai = sh_dt_child(it, "aiType");
        def = sh_dt_child(it, "entityDef");
        a = mr_unquote(ai);
        d = mr_unquote(def);
        if (!a || !d) { free(a); free(d); free(items); return 0; }
        /* SPAWN_AI_TYPE_* names or stock bits alone: the game's own type */
        if (!sh_modregistry_type_words(a, strlen(a), &own, &like)) { free(a); free(d); continue; }
        v = own;
        if (!d[0]) {
            mr_line(r, "conductor: %s adds an aiTypeList item with aiType %s and no entityDef; it keeps that number",
                    src[it->src].who, a);
            free(a); free(d);
            continue;
        }
        free(a);
        for (q = 0; q < r->nentries && dup < 0; ++q)
            if (r->entries[q].source == it->src && r->entries[q].authored == (unsigned)v) dup = q;
        items[nitems++] = it;
        if (dup >= 0) { free(d); continue; }   /* one number listed twice by a source: one bit */
        if (!mr_grow((void **)&r->entries, r->nentries, sizeof *r->entries)) { free(d); free(items); return 0; }
        {
            sh_mr_entry *e = &r->entries[r->nentries++];
            const sh_mr_source *so = &src[it->src];
            int dd;
            memset(e, 0, sizeof *e);
            e->source = it->src;
            e->authored = (unsigned)v;
            e->standin = like;
            e->def = d;
            /* The encounter that names the number orders the entry. */
            for (dd = 0; dd < so->ndecls; ++dd) {
                unsigned nums[8];
                int k, nn;
                if (so->decls[dd].score) continue;
                nn = mr_custom_numbers(so->decls[dd].text, so->decls[dd].len, nums, 8);
                for (k = 0; k < nn; ++k)
                    if (nums[k] == (unsigned)v && (!e->encounter || _stricmp(so->decls[dd].name, e->encounter) < 0)) {
                        char *name = mr_strdup(so->decls[dd].name);
                        if (name) { free(e->encounter); e->encounter = name; }
                    }
            }
        }
    }
    if (r->nentries) qsort(r->entries, (size_t)r->nentries, sizeof *r->entries, mr_entry_cmp);
    /* The map's pins first: a pinned bit stays with its demon while it is free. */
    for (i = 0; i < r->nentries; ++i) {
        sh_mr_entry *e = &r->entries[i];
        unsigned pin = mr_pin(choices, nchoices, src[e->source].key, e->encounter);
        if (pin && !(taken & pin)) { e->bit = pin; e->pinned = 1; taken |= pin; }
    }
    /* A pin no listed demon claims is a demon of this map whose content is not here (its mod's folder is absent,
     * modmanifest.c keeps its pin at a save): its type stays out of the others' reach while a free one is left, so
     * the map's AI Filters and the demon keep their type when the content is back (R-LB1). */
    {
        unsigned reserved = 0;
        size_t pl = strlen(SH_MR_PIN);
        int c, b;
        for (c = 0; c < nchoices; ++c) {
            const char *slot = choices[c].slot;
            char *end;
            long bit;
            int claimed = 0;
            if (!slot || !choices[c].key || _strnicmp(slot, SH_MR_PIN, pl)) continue;
            bit = strtol(slot + pl, &end, 10);
            if (*end != '/' || bit < SH_MR_BIT_FIRST || bit > SH_MR_BIT_LAST) continue;
            for (i = 0; i < r->nentries && !claimed; ++i)
                claimed = r->entries[i].encounter && src[r->entries[i].source].key &&
                          !_stricmp(src[r->entries[i].source].key, choices[c].key) &&
                          !_stricmp(r->entries[i].encounter, end + 1);
            if (!claimed && !(taken & (1u << bit))) reserved |= 1u << bit;
        }
        for (i = 0; i < r->nentries && reserved; ++i) {
            sh_mr_entry *e = &r->entries[i];
            if (e->pinned) continue;
            for (b = SH_MR_BIT_FIRST; b <= SH_MR_BIT_LAST && !e->bit; ++b)
                if (!((taken | reserved) & (1u << b))) { e->bit = 1u << b; taken |= e->bit; }
        }
        if (reserved) {
            int kept = 0;
            for (b = SH_MR_BIT_FIRST; b <= SH_MR_BIT_LAST; ++b) kept += (reserved >> b) & 1;
            mr_line(r, "conductor: %d type(s) kept for the map's demons whose content is not here", kept);
        }
    }
    for (i = 0; i < r->nentries; ++i) {
        sh_mr_entry *e = &r->entries[i];
        int q, b, with = -1;
        if (e->pinned || e->bit) continue;
        for (b = SH_MR_BIT_FIRST; b <= SH_MR_BIT_LAST && !e->bit; ++b)
            if (!(taken & (1u << b))) { e->bit = 1u << b; taken |= e->bit; }
        if (e->bit) continue;
        /* No free type left: the first ported demon standing in for the same stock demon shares it, else the
         * first ported demon (a degrade, never a refusal). */
        for (q = 0; q < r->nentries && with < 0; ++q)
            if (q != i && !r->entries[q].shared && r->entries[q].bit && e->standin &&
                r->entries[q].standin == e->standin) with = q;
        for (q = 0; q < r->nentries && with < 0; ++q)
            if (q != i && !r->entries[q].shared && r->entries[q].bit) with = q;
        if (with < 0) with = 0;
        e->bit = r->entries[with].bit;
        e->shared = 1;
        mr_line(r, "conductor: a map gives at most %d ported demons a spawn type of their own; %s (%s) of %s shares the "
                "type of %s, so the AI Filter, the kill score and the encounters do not tell the two apart",
                SH_MR_BIT_LAST - SH_MR_BIT_FIRST + 1, e->encounter ? e->encounter : "its encounter", e->def,
                src[e->source].who, r->entries[with].def);
    }
    /* The items take their bits. */
    for (i = 0; i < nitems; ++i) {
        sh_dt_node *ai = sh_dt_child(items[i], "aiType");
        char *a, bit[24];
        unsigned v;
        int q;
        if (!(a = mr_unquote(ai))) { free(items); return 0; }
        (void)sh_modregistry_type_words(a, strlen(a), &v, NULL);
        free(a);
        for (q = 0; q < r->nentries; ++q)
            if (r->entries[q].source == items[i]->src && r->entries[q].authored == (unsigned)v) {
                _snprintf_s(bit, sizeof bit, _TRUNCATE, "\"%u\"", r->entries[q].bit);
                if (!sh_dt_set_value(t, ai, bit, strlen(bit))) { free(items); return 0; }
                break;
            }
    }
    free(items);
    for (i = 0; i < r->nentries; ++i) {
        const char *like = NULL;
        size_t f;
        for (f = 0; f < sizeof MR_SPAWN_FLAGS / sizeof MR_SPAWN_FLAGS[0] && !like; ++f)
            if (r->entries[i].standin & MR_SPAWN_FLAGS[f].bit) like = MR_SPAWN_FLAGS[f].name;
        mr_line(r, "conductor: aiType %u -> %s (%s, %s, authored %u%s%s%s)", r->entries[i].bit, r->entries[i].def,
                src[r->entries[i].source].who, r->entries[i].encounter ? r->entries[i].encounter : "no encounter",
                r->entries[i].authored, like ? ", stands in for " : "", like ? like : "",
                r->entries[i].pinned ? ", pinned by the map" : "");
    }
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

/* The conductor as served: each aiTypeList item a source added holds its
 * assigned type alone (the stand-in's words beside it are the product's to
 * read, never the game's: a served stock word would make the port that demon). */
static int mr_conductor_serve(sh_dt *t)
{
    sh_dt_node *list = sh_dt_at(sh_dt_root(t), "edit/aiTypeList");
    int i;
    for (i = 0; list && i < list->nkids; ++i) {
        sh_dt_node *it = list->kids[i], *ai;
        char *a, bit[24];
        unsigned own = 0;
        if (!it->block || it->src < 0 || !(ai = sh_dt_child(it, "aiType"))) continue;
        if (!(a = mr_unquote(ai))) return 0;
        if (sh_modregistry_type_words(a, strlen(a), &own, NULL)) {
            _snprintf_s(bit, sizeof bit, _TRUNCATE, "\"%u\"", own);
            if (!sh_dt_set_value(t, ai, bit, strlen(bit))) { free(a); return 0; }
        }
        free(a);
    }
    return 1;
}

/* The flag picker offers each entry's type (one value per bit), labelled with its encounter's strings. */
static int mr_grow_flags(sh_mr_result *out, const sh_mr_source *src, int k, const sh_mr_target *targets)
{
    typedef struct { char name[32], label[256], desc[256]; } mr_flag_text;
    sh_mr_flag *flags = (sh_mr_flag *)calloc((size_t)out->nentries, sizeof *flags);
    mr_flag_text *txt = (mr_flag_text *)calloc((size_t)out->nentries, sizeof *txt);
    int i, nf = 0;
    if (!flags || !txt) { free(flags); free(txt); return 0; }
    for (i = 0; i < out->nentries; ++i) {
        const sh_mr_entry *e = &out->entries[i];
        const sh_mr_source *s = &src[e->source];
        int d;
        if (e->shared || !sh_modregistry_flag_name(e->bit, txt[nf].name, sizeof txt[nf].name)) continue;
        for (d = 0; e->encounter && d < s->ndecls; ++d)
            if (!s->decls[d].score && s->decls[d].name && !_stricmp(s->decls[d].name, e->encounter)) {
                (void)sh_modregistry_decl_string(s->decls[d].text, s->decls[d].len, "displayStringId", txt[nf].label,
                                                 sizeof txt[nf].label);
                (void)sh_modregistry_decl_string(s->decls[d].text, s->decls[d].len, "descriptionStringId",
                                                 txt[nf].desc, sizeof txt[nf].desc);
                break;
            }
        flags[nf].value = txt[nf].name;
        flags[nf].label = txt[nf].label;
        flags[nf].description = txt[nf].desc;
        nf++;
    }
    if (nf) {
        size_t glen = 0;
        const char *base = out->text[k] ? out->text[k] : targets[k].text;
        size_t blen = out->text[k] ? out->len[k] : targets[k].len;
        char *g = base ? sh_modregistry_append_flags(base, blen, flags, nf, &glen) : NULL, *game;
        if (g) {
            free(out->text[k]);
            out->text[k] = g;
            out->len[k] = glen;
            game = sh_dt_game_name(targets[k].request);
            mr_line(out, "%s: the AI Filter's Include AI offers the map's %d ported demon type(s) too",
                    game ? game : targets[k].request, nf);
            free(game);
        }
    }
    free(flags);
    free(txt);
    return 1;
}

static int mr_is_grown_list(const char *request)
{
    int i;
    for (i = 0; request && i < SH_MR_GROWN_LIST_COUNT; ++i) if (!_stricmp(request, SH_MR_GROWN_LISTS[i])) return 1;
    return 0;
}

int sh_modregistry_generate(const sh_mr_source *src, int n, const sh_mr_target *targets, int ntargets,
                            const sh_mr_choice *choices, int nchoices, const char *const *grown, int ngrown,
                            sh_mr_result *out)
{
    sh_dt_source *ds = NULL;
    sh_dt_choice *dc = NULL;
    char **rewritten = NULL;   /* per source: its edits with the assigned types (malloc'd), or NULL */
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
    rewritten = (char **)calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
    if (!out->text || !out->len || !out->used || !ds || !dc || !rewritten) goto oom;
    for (i = 0; i < n; ++i) {
        ds[i].key = src[i].key;
        ds[i].who = src[i].who;
        ds[i].edits = src[i].edits;
        ds[i].edits_len = src[i].edits_len;
    }
    for (i = 0; i < nchoices; ++i) { dc[i].slot = choices[i].slot; dc[i].key = choices[i].key; }
    /* The types first: the conductor with the sources' own edits assigns them (its lines are said once, below). */
    for (k = 0; k < ntargets; ++k) {
        sh_dt *t;
        if (!targets[k].text || _stricmp(targets[k].request, SH_MB_REGISTRY_CONDUCTOR)) continue;
        t = sh_dt_apply(targets[k].request, targets[k].text, targets[k].len, ds, n, dc, nchoices, NULL);
        if (t) mr_drop_unavailable(t, src, n, 1);
        if (t && !mr_conductor(t, src, n, choices, nchoices, out)) { sh_dt_free(t); goto oom; }
        sh_dt_free(t);
    }
    /* Then every source's edits speak of the types assigned to the numbers it wrote. */
    for (i = 0; i < n && out->nentries; ++i) {
        unsigned from[SH_MR_BIT_LAST - SH_MR_BIT_FIRST + 2], to[SH_MR_BIT_LAST - SH_MR_BIT_FIRST + 2];
        int m = 0, e;
        size_t len = 0;
        for (e = 0; e < out->nentries && m < (int)(sizeof from / sizeof from[0]); ++e)
            if (out->entries[e].source == i) { from[m] = out->entries[e].authored; to[m++] = out->entries[e].bit; }
        if (!m || !src[i].edits) continue;
        rewritten[i] = sh_modregistry_rewrite_edits(src[i].edits, src[i].edits_len, from, to, m, &len);
        if (rewritten[i]) { ds[i].edits = rewritten[i]; ds[i].edits_len = len; }
    }
    for (k = 0; k < ntargets; ++k) {
        mr_apply_ctx ctx;
        sh_dt_report rep;
        sh_dt *t;
        out->used[k] = (unsigned char *)calloc((size_t)(n > 0 ? n : 1), 1);
        if (!out->used[k]) goto oom;
        if (!targets[k].text) {
            char *game = sh_dt_game_name(targets[k].request);
            for (i = 0; i < n; ++i)
                if (src[i].edits && strstr(src[i].edits, targets[k].request)) {
                    mr_line(out, "%s: the game's text could not be read; the game's own file serves and no mod's edits "
                            "apply", game ? game : targets[k].request);
                    break;
                }
            free(game);
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
        if (t && !_stricmp(targets[k].request, SH_MB_REGISTRY_CONDUCTOR)) {
            mr_drop_unavailable(t, src, n, 0);
            if (!mr_conductor_serve(t)) { sh_dt_free(t); goto oom; }
        }
        if (t) {
            out->text[k] = sh_dt_text(t, &out->len[k]);
            sh_dt_free(t);
            if (!out->text[k]) goto oom;
        }
        if (ngrown > 0 && mr_is_grown_list(targets[k].request)) {
            size_t glen = 0;
            const char *base = out->text[k] ? out->text[k] : targets[k].text;
            size_t blen = out->text[k] ? out->len[k] : targets[k].len;
            char *g = sh_modregistry_append_list(base, blen, "validEncounters", grown, ngrown, &glen), *game;
            if (g) {
                free(out->text[k]);
                out->text[k] = g;
                out->len[k] = glen;
                game = sh_dt_game_name(targets[k].request);
                mr_line(out, "%s: offers the installed mods' single-demon encounters too (%d listed)",
                        game ? game : targets[k].request, ngrown);
                free(game);
            }
        }
        if (grown && out->nentries && !_stricmp(targets[k].request, SH_MR_FLAG_PICKER) &&
            !mr_grow_flags(out, src, k, targets)) goto oom;
        if (!out->text[k]) continue;
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
    for (i = 0; rewritten && i < n; ++i) free(rewritten[i]);
    free(rewritten);
    free(ds);
    free(dc);
    return 1;
oom:
    for (i = 0; rewritten && i < n; ++i) free(rewritten[i]);
    free(rewritten);
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
    for (i = 0; i < r->nentries; ++i) { free(r->entries[i].def); free(r->entries[i].encounter); }
    for (i = 0; i < r->nclashes; ++i) free(r->clashes[i].slot);
    free(r->text); free(r->len); free(r->used);
    free(r->entries); free(r->rewrites); free(r->clashes); free(r->lines);
    memset(r, 0, sizeof *r);
}

/* ------------------------------------------------------------------ choices */

int sh_modregistry_slot_valid(const char *slot)
{
    size_t i, n = slot ? strlen(slot) : 0;
    int parts = 1;
    if (!n || slot[0] == '/' || slot[n - 1] == '/') return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)slot[i];
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') return 0;
        if (c == '/') { if (slot[i + 1] == '/') return 0; parts++; }
    }
    return parts >= 3;
}
