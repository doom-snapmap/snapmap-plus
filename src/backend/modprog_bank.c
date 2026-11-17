/* modprog_bank.c -- a mod's material decl in the bank's names (modprog_bank.h). */
#include <stdlib.h>
#include <string.h>

#include "modprog.h"
#include "modprog_bank.h"

/* One line of the decl's outermost block that starts with a key. */
typedef struct mb_key { size_t at, n, line; } mb_key;

typedef struct mb_scan {
    mb_key *keys;
    size_t nkeys, cap;
    size_t close;                  /* the brace that ends the outermost block, or (size_t)-1 */
    int oom;
} mb_scan;

static int mb_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int mb_ieq(const char *a, size_t n, const char *b)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (!y) return 0;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return b[n] == 0;
}

/* The keys of the outermost block's lines (outside comments and strings) and its closing brace. */
static void mb_scan_text(const char *t, size_t len, mb_scan *s)
{
    size_t i = 0;
    int depth = 0, block = 0;
    s->close = (size_t)-1;
    while (i < len) {
        size_t line = i, k = i;
        if (!block && depth == 1) {
            while (k < len && (t[k] == ' ' || t[k] == '\t')) k++;
            if (k < len && mb_word(t[k])) {
                size_t e = k;
                while (e < len && mb_word(t[e])) e++;
                if (e == len || t[e] == ' ' || t[e] == '\t' || t[e] == '\r' || t[e] == '\n' || t[e] == '{' || t[e] == '"') {
                    if (s->nkeys == s->cap) {
                        size_t cap = s->cap ? s->cap * 2 : 32;
                        mb_key *g = (mb_key *)realloc(s->keys, cap * sizeof *g);
                        if (!g) { s->oom = 1; return; }
                        s->keys = g;
                        s->cap = cap;
                    }
                    s->keys[s->nkeys].at = k;
                    s->keys[s->nkeys].n = e - k;
                    s->keys[s->nkeys].line = line;
                    s->nkeys++;
                }
            }
        }
        /* the rest of the line: braces outside comments and strings */
        for (; i < len && t[i] != '\n'; i++) {
            if (block) {
                if (t[i] == '*' && i + 1 < len && t[i + 1] == '/') { block = 0; i++; }
                continue;
            }
            if (t[i] == '"') {
                for (i++; i < len && t[i] != '"' && t[i] != '\n'; i++) {}
                if (i == len || t[i] == '\n') break;
                continue;
            }
            if (t[i] == '/' && i + 1 < len && t[i + 1] == '/') {
                while (i < len && t[i] != '\n') i++;
                break;
            }
            if (t[i] == '/' && i + 1 < len && t[i + 1] == '*') { block = 1; i++; continue; }
            if (t[i] == '{') depth++;
            else if (t[i] == '}' && --depth == 0) s->close = i;
        }
        if (i < len) i++;          /* the line's '\n' */
    }
}

typedef struct mb_prog { const char *const *bank, *const *names, *const *defaults; unsigned n; } mb_prog;

static void mb_prog_of(int which, mb_prog *p)
{
    if (which == SH_MODPROG_BANK_MULTILAYER) {
        p->bank = SH_MODPROG_ML_PARMS; p->names = SH_MODPROG_ML_NAMES; p->defaults = SH_MODPROG_ML_DEFAULTS;
        p->n = SH_MODPROG_ML_PARM_COUNT;
    } else {
        p->bank = SH_MODPROG_EP_PARMS; p->names = SH_MODPROG_EP_NAMES; p->defaults = SH_MODPROG_EP_DEFAULTS;
        p->n = SH_MODPROG_EP_PARM_COUNT;
    }
}

/* The parameter a key is of program `p`: k for its readable name (*readable 1) or its bank name, else -1. */
static int mb_param(const mb_prog *p, const char *key, size_t n, int *readable)
{
    unsigned k;
    for (k = 0; k < p->n; k++) {
        if (mb_ieq(key, n, p->names[k])) { if (readable) *readable = 1; return (int)k; }
        if (mb_ieq(key, n, p->bank[k])) { if (readable) *readable = 0; return (int)k; }
    }
    return -1;
}

static int mb_program(const char *t, size_t len, const mb_scan *s)
{
    static const char ml[] = SH_MODPROG_PREFIX;
    mb_prog ep;
    size_t i;
    for (i = 0; i < s->nkeys; i++) {
        size_t v = s->keys[i].at + s->keys[i].n;
        if (!mb_ieq(t + s->keys[i].at, s->keys[i].n, "ambientprogram")) continue;
        while (v < len && (t[v] == ' ' || t[v] == '\t')) v++;
        if (v < len && t[v] == '"') v++;
        if (len - v >= sizeof ml - 1 && mb_ieq(t + v, sizeof ml - 1, ml)) return SH_MODPROG_BANK_MULTILAYER;
    }
    mb_prog_of(SH_MODPROG_BANK_PARTICLE, &ep);
    for (i = 0; i < s->nkeys; i++)
        if (mb_param(&ep, t + s->keys[i].at, s->keys[i].n, NULL) >= 0) return SH_MODPROG_BANK_PARTICLE;
    return SH_MODPROG_BANK_NONE;
}

int sh_modprog_bank_program(const char *text, size_t len)
{
    mb_scan s;
    int r;
    memset(&s, 0, sizeof s);
    if (!text) return SH_MODPROG_BANK_NONE;
    mb_scan_text(text, len, &s);
    r = s.oom ? SH_MODPROG_BANK_NONE : mb_program(text, len, &s);
    free(s.keys);
    return r;
}

typedef struct mb_out { char *p; size_t n, cap; int oom; } mb_out;

static void mb_put(mb_out *o, const char *d, size_t n)
{
    if (o->oom || !n) return;
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 256;
        char *g;
        while (cap < o->n + n + 1) cap *= 2;
        if (!(g = (char *)realloc(o->p, cap))) { o->oom = 1; return; }
        o->p = g;
        o->cap = cap;
    }
    memcpy(o->p + o->n, d, n);
    o->n += n;
    o->p[o->n] = 0;
}
static void mb_puts(mb_out *o, const char *s) { mb_put(o, s, strlen(s)); }

int sh_modprog_bank(const char *text, size_t len, char **out, size_t *out_len)
{
    mb_scan s;
    mb_prog p;
    mb_out o = { 0 };
    unsigned char written[SH_MODPROG_BANK_COUNT];
    const char *nl;
    size_t i, from = 0, missing = 0;
    int which, renames = 0;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!text || !out) return SH_MODPROG_BANK_SAME;
    memset(&s, 0, sizeof s);
    memset(written, 0, sizeof written);
    mb_scan_text(text, len, &s);
    if (s.oom) { free(s.keys); return SH_MODPROG_BANK_OOM; }
    which = mb_program(text, len, &s);
    if (which == SH_MODPROG_BANK_NONE) { free(s.keys); return SH_MODPROG_BANK_SAME; }
    mb_prog_of(which, &p);
    for (i = 0; i < s.nkeys; i++) {
        int readable = 0, k = mb_param(&p, text + s.keys[i].at, s.keys[i].n, &readable);
        if (k < 0) continue;
        written[k] = 1;
        renames += readable;
    }
    for (i = 0; i < p.n; i++) missing += !written[i];
    if (s.close == (size_t)-1) missing = 0;      /* no closing brace to add before (not a decl the builder admits) */
    if (!renames && !missing) { free(s.keys); return SH_MODPROG_BANK_SAME; }
    nl = "\n";
    for (i = 0; i + 1 < len; i++) if (text[i] == '\r' && text[i + 1] == '\n') { nl = "\r\n"; break; }
    /* the keys, readable names under their bank names */
    for (i = 0; i < s.nkeys; i++) {
        int readable = 0, k = mb_param(&p, text + s.keys[i].at, s.keys[i].n, &readable);
        if (k < 0 || !readable) continue;
        mb_put(&o, text + from, s.keys[i].at - from);
        mb_puts(&o, p.bank[k]);
        from = s.keys[i].at + s.keys[i].n;
    }
    if (missing) {
        /* the keys it does not write, before the closing brace (on its own line when the brace's line holds more) */
        size_t ls = s.close;
        int alone;
        while (ls > 0 && text[ls - 1] != '\n') ls--;
        alone = 1;
        for (i = ls; i < s.close; i++) if (text[i] != ' ' && text[i] != '\t') alone = 0;
        mb_put(&o, text + from, (alone ? ls : s.close) - from);
        if (!alone) mb_puts(&o, nl);
        for (i = 0; i < p.n; i++) {
            if (written[i]) continue;
            mb_puts(&o, p.bank[i]);
            mb_puts(&o, "\t");
            mb_puts(&o, p.defaults[i]);
            mb_puts(&o, nl);
        }
        from = alone ? ls : s.close;
    }
    mb_put(&o, text + from, len - from);
    free(s.keys);
    if (o.oom) { free(o.p); return SH_MODPROG_BANK_OOM; }
    if (!o.p && !(o.p = (char *)calloc(1, 1))) return SH_MODPROG_BANK_OOM;
    *out = o.p;
    if (out_len) *out_len = o.n;
    return SH_MODPROG_BANK_CHANGED;
}
