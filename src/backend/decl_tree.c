/* decl_tree.c -- see decl_tree.h. Pure C; no engine or filesystem dependency. */
#include "../common/text.h"
#include "decl_tree.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT_DEPTH 48
#define DT_PATH  64

/* ------------------------------------------------------------------ arena */

typedef struct dt_chunk { struct dt_chunk *next; size_t used, cap; char data[1]; } dt_chunk;

struct sh_dt {
    dt_chunk *arena;
    sh_dt_node *root;
    const char *eol;
    int failed;
};

static void *dt_alloc(sh_dt *t, size_t n)
{
    dt_chunk *c = t->arena;
    n = (n + 7u) & ~(size_t)7u;
    if (!c || c->cap - c->used < n) {
        size_t cap = n > 65536 ? n : 65536;
        c = (dt_chunk *)malloc(sizeof *c + cap);
        if (!c) { t->failed = 1; return NULL; }
        c->next = t->arena;
        c->used = 0;
        c->cap = cap;
        t->arena = c;
    }
    c->used += n;
    return c->data + c->used - n;
}

static char *dt_copy(sh_dt *t, const char *s, size_t n)
{
    char *d = (char *)dt_alloc(t, n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void sh_dt_free(sh_dt *t)
{
    dt_chunk *c, *n;
    if (!t) return;
    for (c = t->arena; c; c = n) { n = c->next; free(c); }
    free(t);
}

sh_dt_node *sh_dt_root(sh_dt *t) { return t ? t->root : NULL; }

static sh_dt_node *dt_node(sh_dt *t)
{
    sh_dt_node *n = (sh_dt_node *)dt_alloc(t, sizeof *n);
    if (n) { memset(n, 0, sizeof *n); n->src = -1; }
    return n;
}

static int dt_add_kid(sh_dt *t, sh_dt_node *parent, sh_dt_node *kid)
{
    if (parent->nkids == parent->ckids) {
        int cap = parent->ckids ? parent->ckids * 2 : 8;
        sh_dt_node **k = (sh_dt_node **)dt_alloc(t, sizeof(sh_dt_node *) * (size_t)cap);
        if (!k) return 0;
        if (parent->nkids) memcpy(k, parent->kids, sizeof(sh_dt_node *) * (size_t)parent->nkids);
        parent->kids = k;
        parent->ckids = cap;
    }
    parent->kids[parent->nkids++] = kid;
    return 1;
}

static void dt_remove_kid(sh_dt_node *parent, int i)
{
    if (i < 0 || i >= parent->nkids) return;
    memmove(parent->kids + i, parent->kids + i + 1, sizeof(sh_dt_node *) * (size_t)(parent->nkids - i - 1));
    parent->nkids--;
}

/* ------------------------------------------------------------------ small text */

typedef struct dt_buf { char *p; size_t n, cap; int oom; } dt_buf;

static void dt_put(dt_buf *b, const char *s, size_t n)
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

static void dt_puts(dt_buf *b, const char *s) { dt_put(b, s, strlen(s)); }

static char *dt_done(dt_buf *b, size_t *len)
{
    if (b->oom) { free(b->p); b->p = NULL; return NULL; }
    if (!b->p) { b->p = (char *)malloc(1); if (!b->p) return NULL; b->p[0] = 0; }
    if (len) *len = b->n;
    return b->p;
}

static int dt_lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

static int dt_ieq(const char *a, size_t an, const char *b, size_t bn)
{
    size_t i;
    if (an != bn) return 0;
    for (i = 0; i < an; ++i) if (dt_lower((unsigned char)a[i]) != dt_lower((unsigned char)b[i])) return 0;
    return 1;
}

static int dt_key_is(const sh_dt_node *n, const char *w) { return dt_ieq(n->key, n->key_n, w, strlen(w)); }

static void dt_cause(char *cause, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!cause || !cap) return;
    va_start(ap, fmt);
    _vsnprintf_s(cause, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ lexer and parser */

enum { DT_END, DT_WORD, DT_STR, DT_LB, DT_RB, DT_EQ, DT_SEMI, DT_GROUP, DT_BAD };
typedef struct dt_tok { int kind; const char *p; size_t n; } dt_tok;
typedef struct dt_lex { const char *t; size_t len, pos; } dt_lex;

static dt_tok dt_next(dt_lex *l)
{
    dt_tok k = { DT_END, NULL, 0 };
    const char *t = l->t;
    size_t i = l->pos, len = l->len;
    for (;;) {
        while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n' || t[i] == '\f')) i++;
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '/') { while (i < len && t[i] != '\n') i++; continue; }
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i = i + 2 <= len ? i + 2 : len;
            continue;
        }
        break;
    }
    k.p = t + i;
    if (i >= len) { l->pos = len; return k; }
    switch (t[i]) {
    case '"':
        for (++i; i < len && t[i] != '"' && t[i] != '\n'; ++i) {}
        if (i >= len || t[i] != '"') { k.kind = DT_BAD; break; }
        ++i;
        k.kind = DT_STR;
        break;
    case '{': k.kind = DT_LB; ++i; break;
    case '}': k.kind = DT_RB; ++i; break;
    case '=': k.kind = DT_EQ; ++i; break;
    case ';': k.kind = DT_SEMI; ++i; break;
    case '(': {
        int depth = 0;
        for (; i < len; ++i) {
            if (t[i] == '(') depth++;
            else if (t[i] == ')' && --depth == 0) { ++i; break; }
            else if (t[i] == '{' || t[i] == '}' || t[i] == ';') { depth = -1; break; }
        }
        k.kind = depth == 0 ? DT_GROUP : DT_BAD;
        break;
    }
    default: {
        size_t s = i;
        while (i < len && !strchr(" \t\r\n\f{}=;\"()", t[i])) i++;
        k.kind = i > s ? DT_WORD : DT_BAD;
        if (i == s) i++;
        break;
    }
    }
    k.n = (size_t)(t + i - k.p);
    l->pos = i;
    return k;
}

static dt_tok dt_peek(dt_lex *l)
{
    size_t at = l->pos;
    dt_tok k = dt_next(l);
    l->pos = at;
    return k;
}

static int dt_line_of(const dt_lex *l, const char *p)
{
    int line = 1;
    const char *q;
    for (q = l->t; q < p && q < l->t + l->len; ++q) if (*q == '\n') line++;
    return line;
}

/* item[<digits>]: its index, or -1. */
static long dt_item_index(const sh_dt_node *n)
{
    size_t i;
    long v = 0;
    if (n->name || n->key_n < 7 || _strnicmp(n->key, "item[", 5) || n->key[n->key_n - 1] != ']') return -1;
    for (i = 5; i + 1 < n->key_n; ++i) {
        if (n->key[i] < '0' || n->key[i] > '9' || v > 100000000L) return -1;
        v = v * 10 + (n->key[i] - '0');
    }
    return v;
}

static int dt_int(const char *s, size_t n, long *out)
{
    size_t i = 0;
    long v = 0;
    int neg = 0;
    if (n && s[0] == '-') { neg = 1; i = 1; }
    if (i >= n) return 0;
    for (; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9' || v > 100000000L) return 0;
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return 1;
}

/* A dense ordered list: one `num = N` scalar and item[0] .. item[k-1] in order. */
static void dt_mark_list(sh_dt_node *n)
{
    int i, nums = 0;
    long items = 0, num = 0;
    n->list = 0;
    n->num_delta = 0;
    if (!n->block || n->name) return;
    for (i = 0; i < n->nkids; ++i) {
        const sh_dt_node *k = n->kids[i];
        if (dt_key_is(k, "num") && !k->block && k->eq) {
            if (!dt_int(k->val, k->val_n, &num)) return;
            nums++;
            continue;
        }
        if (dt_item_index(k) != items) return;
        items++;
    }
    if (nums != 1) return;
    n->list = 1;
    n->num_delta = num - items;
}

/* 1 when a line break lies between the end of one token and the start of the next. */
static int dt_newline_between(const char *from, const char *to)
{
    for (; from < to; ++from) if (*from == '\n') return 1;
    return 0;
}

static int dt_is_value(int kind) { return kind == DT_WORD || kind == DT_STR || kind == DT_GROUP; }

/* A value: one token, then every further word, string or group on the same
 * line (`color = 0 0 1 1;`, `tags far scramble`, `model box( ... )`). */
static void dt_value(dt_lex *l, sh_dt_node *n, dt_tok first)
{
    const char *end = first.p + first.n;
    for (;;) {
        size_t at = l->pos;
        dt_tok k = dt_next(l);
        if (!dt_is_value(k.kind) || dt_newline_between(end, k.p)) { l->pos = at; break; }
        end = k.p + k.n;
    }
    n->val = first.p;
    n->val_n = (size_t)(end - first.p);
}

/* Statements up to the closing brace (consumed). */
static int dt_parse_block(sh_dt *t, dt_lex *l, sh_dt_node *parent, int depth, char *cause, size_t cap)
{
    if (depth > DT_DEPTH) { dt_cause(cause, cap, "blocks nested deeper than %d", DT_DEPTH); return 0; }
    for (;;) {
        dt_tok key = dt_next(l), tk;
        sh_dt_node *n;
        if (key.kind == DT_RB) { dt_mark_list(parent); return 1; }
        if (key.kind == DT_END) { dt_cause(cause, cap, "line %d: a block is not closed", dt_line_of(l, key.p)); return 0; }
        n = dt_node(t);
        if (!n) { dt_cause(cause, cap, "out of memory"); return 0; }
        if (key.kind == DT_LB) {
            /* An anonymous block (a table's `{ { 0, 1 } }`). */
            n->key = key.p;
            n->key_n = 0;
            n->block = 1;
            if (!dt_parse_block(t, l, n, depth + 1, cause, cap)) return 0;
        } else {
            if (!dt_is_value(key.kind)) {
                dt_cause(cause, cap, "line %d: expected a key, found '%.*s'", dt_line_of(l, key.p),
                         (int)(key.n ? (key.n > 20 ? 20 : key.n) : 1), key.p);
                return 0;
            }
            n->key = key.p;
            n->key_n = key.n;
            tk = dt_peek(l);
            if (tk.kind != DT_EQ && tk.kind != DT_LB && dt_is_value(tk.kind) && !dt_newline_between(key.p + key.n, tk.p)) {
                /* `key name {`, `key name = value`, or `key value ...` (md6Def). */
                dt_tok after;
                dt_next(l);
                after = dt_peek(l);
                if (after.kind == DT_LB || after.kind == DT_EQ) {
                    n->name = tk.p;
                    n->name_n = tk.n;
                    tk = dt_peek(l);
                } else {
                    dt_value(l, n, tk);
                    tk.kind = DT_END;   /* done */
                }
            }
            if (!n->val && tk.kind == DT_EQ) {
                dt_next(l);
                n->eq = 1;
                tk = dt_peek(l);
                if (tk.kind != DT_LB) {
                    if (!dt_is_value(tk.kind)) {
                        dt_cause(cause, cap, "line %d: %.*s has no value", dt_line_of(l, key.p), (int)(key.n > 60 ? 60 : key.n), key.p);
                        return 0;
                    }
                    dt_next(l);
                    dt_value(l, n, tk);
                }
            }
            if (!n->val && tk.kind == DT_LB) {
                dt_next(l);
                n->block = 1;
                if (!dt_parse_block(t, l, n, depth + 1, cause, cap)) return 0;
            } else if (!n->val && !n->block) {
                if (n->name) { dt_cause(cause, cap, "line %d: %.*s has no value", dt_line_of(l, key.p), (int)(key.n > 60 ? 60 : key.n), key.p); return 0; }
                n->val = "";                   /* a bare member: `{ "MD6_WEIGHTGROUP_ALL" }`, a joint name on its line */
                n->val_n = 0;
            }
        }
        if (dt_peek(l).kind == DT_SEMI) { dt_next(l); n->semi = 1; }
        if (!dt_add_kid(t, parent, n)) { dt_cause(cause, cap, "out of memory"); return 0; }
    }
}

/* Parse `text` (already in t's arena) as one block into a new root. */
static sh_dt_node *dt_parse_into(sh_dt *t, const char *text, size_t len, char *cause, size_t cap)
{
    dt_lex l;
    dt_tok k;
    sh_dt_node *root;
    l.t = text;
    l.len = len;
    l.pos = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) l.pos = 3;
    root = dt_node(t);
    if (!root) { dt_cause(cause, cap, "out of memory"); return NULL; }
    root->block = 1;
    k = dt_next(&l);
    if (k.kind != DT_LB) { dt_cause(cause, cap, "line %d: the text is not one { ... } decl body", dt_line_of(&l, k.p)); return NULL; }
    if (!dt_parse_block(t, &l, root, 0, cause, cap)) return NULL;
    k = dt_next(&l);
    if (k.kind == DT_SEMI) k = dt_next(&l);
    if (k.kind != DT_END) { dt_cause(cause, cap, "line %d: text after the decl's closing brace", dt_line_of(&l, k.p)); return NULL; }
    return root;
}

sh_dt *sh_dt_parse(const char *text, size_t len, char *cause, size_t cap)
{
    sh_dt *t;
    char *own;
    if (cause && cap) cause[0] = 0;
    if (!text) { dt_cause(cause, cap, "no text"); return NULL; }
    if (memchr(text, 0, len)) { dt_cause(cause, cap, "a NUL byte in the text"); return NULL; }
    t = (sh_dt *)calloc(1, sizeof *t);
    if (!t) { dt_cause(cause, cap, "out of memory"); return NULL; }
    t->eol = "\n";
    {
        size_t i;
        for (i = 0; i + 1 < len; ++i) if (text[i] == '\n') { if (i && text[i - 1] == '\r') t->eol = "\r\n"; break; }
    }
    own = dt_copy(t, text, len);
    t->root = own ? dt_parse_into(t, own, len, cause, cap) : NULL;
    if (!t->root) { if (!own) dt_cause(cause, cap, "out of memory"); sh_dt_free(t); return NULL; }
    return t;
}

/* ------------------------------------------------------------------ writing */

static void dt_indent(dt_buf *b, int depth) { int i; for (i = 0; i < depth; ++i) dt_put(b, "\t", 1); }

static void dt_write_members(dt_buf *b, const sh_dt_node *n, int depth, int canon, const char *eol);

/* One statement; `item` >= 0 writes the key as item[item]. */
static void dt_write_stmt(dt_buf *b, const sh_dt_node *k, int depth, int canon, const char *eol, long item)
{
    char tmp[48];
    if (item >= 0) {
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "item[%ld]", item);
        dt_puts(b, tmp);
    } else dt_put(b, k->key, k->key_n);
    if (k->name) { dt_put(b, " ", 1); dt_put(b, k->name, k->name_n); }
    if (k->block) {
        dt_puts(b, k->eq ? " = {" : " {");
        dt_puts(b, eol);
        dt_write_members(b, k, depth + 1, canon, eol);
        dt_indent(b, depth);
        dt_put(b, "}", 1);
        if (k->semi && !canon) dt_put(b, ";", 1);
    } else if (k->eq) {
        dt_put(b, " = ", 3);
        dt_put(b, k->val, k->val_n);
        if (k->semi || canon) dt_put(b, ";", 1);
    } else if (k->val_n) {
        dt_put(b, " ", 1);
        dt_put(b, k->val, k->val_n);
        if (k->semi && !canon) dt_put(b, ";", 1);
    } else if (k->semi && !canon) dt_put(b, ";", 1);
}

static void dt_write_members(dt_buf *b, const sh_dt_node *n, int depth, int canon, const char *eol)
{
    int i;
    long item = 0;
    if (n->list) {
        const sh_dt_node *num = NULL;
        long count = 0;
        char tmp[48];
        for (i = 0; i < n->nkids; ++i) { if (dt_key_is(n->kids[i], "num") && !n->kids[i]->block) num = n->kids[i]; else count++; }
        dt_indent(b, depth);
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%.*s = %ld%s", num ? (int)num->key_n : 3, num ? num->key : "num",
                    count + n->num_delta, (canon || !num || num->semi) ? ";" : "");
        dt_puts(b, tmp);
        dt_puts(b, eol);
    }
    for (i = 0; i < n->nkids; ++i) {
        const sh_dt_node *k = n->kids[i];
        if (n->list && dt_key_is(k, "num") && !k->block) continue;
        dt_indent(b, depth);
        dt_write_stmt(b, k, depth, canon, eol, n->list ? item++ : -1);
        dt_puts(b, eol);
    }
}

char *sh_dt_text(const sh_dt *t, size_t *len)
{
    dt_buf b = { NULL, 0, 0, 0 };
    if (!t || !t->root) return NULL;
    dt_put(&b, "{", 1);
    dt_puts(&b, t->eol);
    dt_write_members(&b, t->root, 1, 0, t->eol);
    dt_put(&b, "}", 1);
    dt_puts(&b, t->eol);
    return dt_done(&b, len);
}

/* A statement's text at depth 0, without the final line end (malloc'd). */
static char *dt_stmt_text(const sh_dt_node *k, int canon, const char *eol)
{
    dt_buf b = { NULL, 0, 0, 0 };
    dt_write_stmt(&b, k, 0, canon, eol, -1);
    return dt_done(&b, NULL);
}

/* A value's text: a scalar's token, or a block's braces and members. */
static char *dt_value_text(const sh_dt_node *k, const char *eol)
{
    dt_buf b = { NULL, 0, 0, 0 };
    if (!k->block) dt_put(&b, k->val, k->val_n);
    else {
        dt_put(&b, "{", 1);
        dt_puts(&b, eol);
        dt_write_members(&b, k, 1, 0, eol);
        dt_put(&b, "}", 1);
    }
    return dt_done(&b, NULL);
}

/* A value with its runs of blanks and line breaks outside strings as one
 * space: two spellings of `0 0 1 1` compare equal. */
static void dt_put_flat(dt_buf *b, const char *v, size_t n)
{
    size_t i;
    int in_str = 0, gap = 0;
    for (i = 0; i < n; ++i) {
        char c = v[i];
        if (c == '"') in_str = !in_str;
        if (!in_str && (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f')) { gap = 1; continue; }
        if (gap) { dt_put(b, " ", 1); gap = 0; }
        dt_put(b, &c, 1);
    }
}

static int dt_val_eq(const char *a, size_t an, const char *b, size_t bn)
{
    dt_buf x = { NULL, 0, 0, 0 }, y = { NULL, 0, 0, 0 };
    int eq;
    if (an == bn && !memcmp(a, b, an)) return 1;
    dt_put_flat(&x, a, an);
    dt_put_flat(&y, b, bn);
    eq = !x.oom && !y.oom && x.n == y.n && (!x.n || !memcmp(x.p, y.p, x.n));
    free(x.p);
    free(y.p);
    return eq;
}

/* ------------------------------------------------------------------ semantic form */

static int dt_str_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* Order-free text: fields and entries sorted, list items in order, keys
 * lower case, ';' and the list's renumbering normalised. `with_key` 0
 * writes the value only (a list item). */
static void dt_sorted(dt_buf *b, const sh_dt_node *n, int with_key)
{
    size_t i;
    if (with_key) {
        for (i = 0; i < n->key_n; ++i) { char c = (char)dt_lower((unsigned char)n->key[i]); dt_put(b, &c, 1); }
        if (n->name) { dt_put(b, " ", 1); dt_put(b, n->name, n->name_n); }
        dt_put(b, n->eq ? "=" : " ", 1);
    }
    if (!n->block) { dt_put_flat(b, n->val, n->val_n); return; }
    dt_put(b, "{", 1);
    if (n->list) {
        char tmp[48];
        long count = 0;
        int k;
        for (k = 0; k < n->nkids; ++k) if (!(dt_key_is(n->kids[k], "num") && !n->kids[k]->block)) count++;
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "#%ld", count + n->num_delta);
        dt_puts(b, tmp);
        for (k = 0; k < n->nkids; ++k) {
            if (dt_key_is(n->kids[k], "num") && !n->kids[k]->block) continue;
            dt_put(b, "\x1e", 1);
            dt_sorted(b, n->kids[k], 0);
        }
    } else if (n->nkids) {
        char **parts = (char **)malloc(sizeof(char *) * (size_t)n->nkids);
        int k, np = 0;
        if (!parts) { b->oom = 1; return; }
        for (k = 0; k < n->nkids; ++k) {
            dt_buf one = { NULL, 0, 0, 0 };
            dt_sorted(&one, n->kids[k], 1);
            if (one.oom) { free(one.p); b->oom = 1; continue; }
            parts[np++] = dt_done(&one, NULL);
        }
        qsort(parts, (size_t)np, sizeof *parts, dt_str_cmp);
        for (k = 0; k < np; ++k) { dt_put(b, "\x1f", 1); dt_puts(b, parts[k] ? parts[k] : ""); free(parts[k]); }
        free(parts);
    }
    dt_put(b, "}", 1);
}

static char *dt_sorted_text(const sh_dt_node *n, int with_key)
{
    dt_buf b = { NULL, 0, 0, 0 };
    dt_sorted(&b, n, with_key);
    return dt_done(&b, NULL);
}

static int dt_same(const sh_dt_node *a, const sh_dt_node *b, int with_key)
{
    char *x, *y;
    int eq;
    if (a->block != b->block) return 0;
    if (!a->block) {
        if (with_key && (!dt_ieq(a->key, a->key_n, b->key, b->key_n) || a->eq != b->eq)) return 0;
        return dt_val_eq(a->val, a->val_n, b->val, b->val_n);
    }
    x = dt_sorted_text(a, with_key);
    y = dt_sorted_text(b, with_key);
    eq = x && y && !strcmp(x, y);
    free(x);
    free(y);
    return eq;
}

int sh_dt_equal(const sh_dt *a, const sh_dt *b)
{
    if (!a || !b || !a->root || !b->root) return 0;
    return dt_same(a->root, b->root, 0);
}

int sh_dt_texts_equal(const char *a, size_t alen, const char *b, size_t blen)
{
    sh_dt *x = sh_dt_parse(a, alen, NULL, 0), *y = x ? sh_dt_parse(b, blen, NULL, 0) : NULL;
    int eq = x && y && sh_dt_equal(x, y);
    sh_dt_free(x);
    sh_dt_free(y);
    return eq;
}

/* ------------------------------------------------------------------ identity */

/* The canonical head of a member: its key, plus ` <name>` for a named block. */
static char *dt_head(const sh_dt_node *n)
{
    dt_buf b = { NULL, 0, 0, 0 };
    dt_put(&b, n->key, n->key_n);
    if (n->name) { dt_put(&b, " ", 1); dt_put(&b, n->name, n->name_n); }
    return dt_done(&b, NULL);
}

static int dt_head_is(const sh_dt_node *n, const char *seg)
{
    size_t sl = strlen(seg), w = n->key_n;
    if (!n->name) return dt_ieq(n->key, n->key_n, seg, sl);
    if (sl != w + 1 + n->name_n || seg[w] != ' ') return 0;
    return dt_ieq(n->key, w, seg, w) && dt_ieq(n->name, n->name_n, seg + w + 1, n->name_n);
}

/* The value of a block's name field: a `name` member, or one under
 * `object`, or under `object/object` (aifsmmanager graphs and states). */
static int dt_name_field(const sh_dt_node *n, const char **p, size_t *len)
{
    int depth, i;
    const sh_dt_node *at = n;
    for (depth = 0; at && at->block && depth < 3; ++depth) {
        const sh_dt_node *obj = NULL;
        int objs = 0;
        for (i = 0; i < at->nkids; ++i) {
            const sh_dt_node *k = at->kids[i];
            if (!k->block && dt_key_is(k, "name") && k->val_n) {
                *p = k->val;
                *len = k->val_n;
                if (*len >= 2 && (*p)[0] == '"') { (*p)++; *len -= 2; }
                return 1;
            }
            if (k->block && !k->name && dt_key_is(k, "object")) { obj = k; objs++; }
        }
        at = objs == 1 ? obj : NULL;
    }
    return 0;
}

static int dt_word_eq(const sh_dt_node *a, const sh_dt_node *b) { return dt_ieq(a->key, a->key_n, b->key, b->key_n); }

/* ------------------------------------------------------------------ JSON out */

static void dt_jstr(dt_buf *b, const char *s, size_t n)
{
    size_t i;
    dt_put(b, "\"", 1);
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        char esc[8];
        if (c == '"') dt_put(b, "\\\"", 2);
        else if (c == '\\') dt_put(b, "\\\\", 2);
        else if (c == '\n') dt_put(b, "\\n", 2);
        else if (c == '\r') dt_put(b, "\\r", 2);
        else if (c == '\t') dt_put(b, "\\t", 2);
        else if (c < 0x20) { _snprintf_s(esc, sizeof esc, _TRUNCATE, "\\u%04x", c); dt_puts(b, esc); }
        else dt_put(b, (const char *)&s[i], 1);
    }
    dt_put(b, "\"", 1);
}

static void dt_jcstr(dt_buf *b, const char *s) { dt_jstr(b, s, strlen(s)); }

/* ------------------------------------------------------------------ JSON in */

enum { DJ_NULL, DJ_STR, DJ_NUM, DJ_ARR, DJ_OBJ, DJ_LIT };
typedef struct dj {
    int t;
    char *s; size_t n;                 /* string (decoded) or number text */
    struct dj **items; char **keys;
    int count, cap;
} dj;

typedef struct dj_ps { const char *p, *e; int depth; } dj_ps;

static void dj_free(dj *v)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->count; ++i) { dj_free(v->items[i]); if (v->keys) free(v->keys[i]); }
    free(v->items);
    free(v->keys);
    free(v->s);
    free(v);
}

static void dj_ws(dj_ps *ps) { while (ps->p < ps->e && strchr(" \t\r\n", *ps->p)) ps->p++; }

static int dj_hex(const char *p, unsigned *out)
{
    int i;
    unsigned v = 0;
    for (i = 0; i < 4; ++i) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *dj_string(dj_ps *ps, size_t *len)
{
    dt_buf b = { NULL, 0, 0, 0 };
    if (ps->p >= ps->e || *ps->p != '"') return NULL;
    ps->p++;
    while (ps->p < ps->e && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') { dt_put(&b, &c, 1); continue; }
        if (ps->p >= ps->e) { free(b.p); return NULL; }
        c = *ps->p++;
        switch (c) {
        case 'n': dt_put(&b, "\n", 1); break;
        case 'r': dt_put(&b, "\r", 1); break;
        case 't': dt_put(&b, "\t", 1); break;
        case 'b': dt_put(&b, "\b", 1); break;
        case 'f': dt_put(&b, "\f", 1); break;
        case 'u': {
            unsigned u;
            char out[4];
            if (ps->e - ps->p < 4 || !dj_hex(ps->p, &u)) { free(b.p); return NULL; }
            ps->p += 4;
            if (u < 0x80) { out[0] = (char)u; dt_put(&b, out, 1); }
            else if (u < 0x800) { out[0] = (char)(0xC0 | (u >> 6)); out[1] = (char)(0x80 | (u & 0x3F)); dt_put(&b, out, 2); }
            else { out[0] = (char)(0xE0 | (u >> 12)); out[1] = (char)(0x80 | ((u >> 6) & 0x3F)); out[2] = (char)(0x80 | (u & 0x3F)); dt_put(&b, out, 3); }
            break;
        }
        default: dt_put(&b, &c, 1); break;
        }
    }
    if (ps->p >= ps->e) { free(b.p); return NULL; }
    ps->p++;
    return dt_done(&b, len);
}

static int dj_push(dj *v, dj *item, char *key)
{
    if (v->count == v->cap) {
        int cap = v->cap ? v->cap * 2 : 8;
        dj **g = (dj **)realloc(v->items, sizeof(dj *) * (size_t)cap);
        char **gk;
        if (!g) return 0;
        v->items = g;
        if (v->t == DJ_OBJ) {
            gk = (char **)realloc(v->keys, sizeof(char *) * (size_t)cap);
            if (!gk) return 0;
            v->keys = gk;
        }
        v->cap = cap;
    }
    if (v->t == DJ_OBJ) v->keys[v->count] = key;
    v->items[v->count++] = item;
    return 1;
}

static dj *dj_value(dj_ps *ps)
{
    dj *v;
    dj_ws(ps);
    if (ps->p >= ps->e || ++ps->depth > 32) return NULL;
    v = (dj *)calloc(1, sizeof *v);
    if (!v) return NULL;
    if (*ps->p == '"') {
        v->t = DJ_STR;
        v->s = dj_string(ps, &v->n);
        if (!v->s) { free(v); return NULL; }
    } else if (*ps->p == '{' || *ps->p == '[') {
        int obj = *ps->p == '{';
        v->t = obj ? DJ_OBJ : DJ_ARR;
        ps->p++;
        dj_ws(ps);
        if (ps->p < ps->e && *ps->p == (obj ? '}' : ']')) ps->p++;
        else for (;;) {
            char *key = NULL;
            dj *item;
            dj_ws(ps);
            if (obj) {
                key = dj_string(ps, NULL);
                dj_ws(ps);
                if (!key || ps->p >= ps->e || *ps->p != ':') { free(key); dj_free(v); return NULL; }
                ps->p++;
            }
            item = dj_value(ps);
            if (!item || !dj_push(v, item, key)) { free(key); dj_free(item); dj_free(v); return NULL; }
            dj_ws(ps);
            if (ps->p < ps->e && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->e && *ps->p == (obj ? '}' : ']')) { ps->p++; break; }
            dj_free(v);
            return NULL;
        }
    } else {
        const char *s = ps->p;
        while (ps->p < ps->e && !strchr(",]} \t\r\n", *ps->p)) ps->p++;
        if (ps->p == s) { free(v); return NULL; }
        v->t = (*s == '-' || (*s >= '0' && *s <= '9')) ? DJ_NUM : DJ_LIT;
        v->n = (size_t)(ps->p - s);
        v->s = (char *)malloc(v->n + 1);
        if (!v->s) { free(v); return NULL; }
        memcpy(v->s, s, v->n);
        v->s[v->n] = 0;
    }
    ps->depth--;
    return v;
}

static dj *dj_parse(const char *text, size_t len)
{
    dj_ps ps;
    dj *v;
    if (!text) return NULL;
    ps.p = text;
    ps.e = text + len;
    ps.depth = 0;
    v = dj_value(&ps);
    dj_ws(&ps);
    if (v && ps.p != ps.e) { dj_free(v); return NULL; }
    return v;
}

static const dj *dj_get(const dj *o, const char *key)
{
    int i;
    if (!o || o->t != DJ_OBJ) return NULL;
    for (i = 0; i < o->count; ++i) if (!strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}

static const char *dj_str(const dj *o, const char *key)
{
    const dj *v = dj_get(o, key);
    return v && v->t == DJ_STR ? v->s : NULL;
}

static int dj_int(const dj *o, const char *key)
{
    const dj *v = dj_get(o, key);
    return v && v->t == DJ_NUM ? atoi(v->s) : 0;
}

/* ------------------------------------------------------------------ diff */

typedef struct dt_diff {
    dt_buf out;                        /* the ops array's members */
    int first;
    sh_dt_counts counts;
    const char *eol;
    const char *segs[DT_PATH];
    int nseg;
    int failed;
} dt_diff;

static void dt_op_begin(dt_diff *d, const char *op, const char *last_seg)
{
    int i;
    if (!d->first) dt_put(&d->out, ",", 1);
    d->first = 0;
    dt_puts(&d->out, "{\"op\":");
    dt_jcstr(&d->out, op);
    dt_puts(&d->out, ",\"path\":[");
    for (i = 0; i < d->nseg; ++i) { if (i) dt_put(&d->out, ",", 1); dt_jcstr(&d->out, d->segs[i]); }
    if (last_seg) { if (d->nseg) dt_put(&d->out, ",", 1); dt_jcstr(&d->out, last_seg); }
    dt_put(&d->out, "]", 1);
}

static void dt_op_member(dt_diff *d, const char *member, const char *text)
{
    dt_put(&d->out, ",", 1);
    dt_jcstr(&d->out, member);
    dt_put(&d->out, ":", 1);
    dt_jcstr(&d->out, text ? text : "");
    if (!text) d->failed = 1;
}

static void dt_op_end(dt_diff *d) { dt_put(&d->out, "}", 1); }

static void dt_op_stmt(dt_diff *d, const char *op, const sh_dt_node *n, const char *last_seg)
{
    char *t = dt_stmt_text(n, 0, "\n");
    dt_op_begin(d, op, last_seg);
    dt_op_member(d, "entry", t);
    dt_op_end(d);
    free(t);
}

static void dt_op_set(dt_diff *d, const sh_dt_node *n)
{
    char *seg = dt_head(n);
    if (!seg) { d->failed = 1; return; }
    dt_op_stmt(d, "set", n, seg);
    free(seg);
    d->counts.set++;
}

static void dt_op_remove(dt_diff *d, const sh_dt_node *n)
{
    char *seg = dt_head(n);
    if (!seg) { d->failed = 1; return; }
    dt_op_begin(d, "remove", seg);
    dt_op_end(d);
    free(seg);
    d->counts.remove++;
}

static void dt_op_keyed(dt_diff *d, const char *op, const sh_dt_node *n, const char *name, size_t name_n)
{
    char *word = sh_textn(n->key, n->key_n), *nm = sh_textn(name, name_n);
    char *t = op[0] == 'k' ? dt_stmt_text(n, 0, "\n") : NULL;
    if (!word || !nm || (op[0] == 'k' && !t)) { free(word); free(nm); free(t); d->failed = 1; return; }
    dt_op_begin(d, op, NULL);
    dt_op_member(d, "word", word);
    dt_op_member(d, "name", nm);
    if (op[0] == 'k') dt_op_member(d, "entry", t);
    dt_op_end(d);
    free(word);
    free(nm);
    free(t);
    if (op[0] == 'k') d->counts.keyed++; else d->counts.drop++;
}

static void dt_diff_block(dt_diff *d, const sh_dt_node *s, const sh_dt_node *c);

/* A whole list or field written over the game's (the copy's text). */
static void dt_diff_member(dt_diff *d, const sh_dt_node *s, const sh_dt_node *c)
{
    if (dt_same(s, c, 1)) return;
    if (s->block && c->block && s->list == c->list && s->eq == c->eq && !s->name && !c->name) {
        char *seg = dt_head(c);
        if (!seg || d->nseg >= DT_PATH) { free(seg); d->failed = 1; return; }
        d->segs[d->nseg++] = seg;
        if (s->list) {
            /* Ordered list: the copy is the game's items (some removed) then new ones. */
            int ns = 0, nc = 0, i, k, ok = s->num_delta == c->num_delta, nr = 0;
            const sh_dt_node **si = (const sh_dt_node **)malloc(sizeof(void *) * (size_t)(s->nkids + 1));
            const sh_dt_node **ci = (const sh_dt_node **)malloc(sizeof(void *) * (size_t)(c->nkids + 1));
            char **st = (char **)calloc((size_t)(s->nkids + 1), sizeof(char *));
            char **ct = (char **)calloc((size_t)(c->nkids + 1), sizeof(char *));
            unsigned char *kept = (unsigned char *)calloc((size_t)(s->nkids + 1), 1);
            int *match = (int *)malloc(sizeof(int) * (size_t)(c->nkids + 1));
            if (!si || !ci || !st || !ct || !kept || !match) { d->failed = 1; ok = 0; }
            for (i = 0; ok && i < s->nkids; ++i) if (!(dt_key_is(s->kids[i], "num") && !s->kids[i]->block)) { si[ns] = s->kids[i]; st[ns++] = dt_sorted_text(s->kids[i], 0); }
            for (i = 0; ok && i < c->nkids; ++i) if (!(dt_key_is(c->kids[i], "num") && !c->kids[i]->block)) { ci[nc] = c->kids[i]; ct[nc++] = dt_sorted_text(c->kids[i], 0); }
            /* Each copy item to the first unused game item of the same text. */
            for (k = 0; ok && k < nc; ++k) {
                match[k] = -1;
                for (i = 0; i < ns && match[k] < 0; ++i)
                    if (!kept[i] && st[i] && ct[k] && !strcmp(st[i], ct[k])) { kept[i] = 1; match[k] = i; }
            }
            /* In order: matched copy items first, in the game's order, then the new ones. */
            if (ok) {
                int last = -1, seen_new = 0;
                for (k = 0; k < nc && ok; ++k) {
                    if (match[k] < 0) { seen_new = 1; continue; }
                    if (seen_new || match[k] < last) ok = 0;
                    last = match[k];
                }
            }
            if (ok) {
                for (i = 0; i < ns; ++i) if (!kept[i]) {
                    char *v = dt_value_text(si[i], "\n");
                    dt_op_begin(d, "unlist", NULL);
                    dt_op_member(d, "entry", v);
                    dt_op_end(d);
                    free(v);
                    d->counts.drop++;
                    nr++;
                }
                for (k = 0; k < nc; ++k) if (match[k] < 0) {
                    char *v = dt_value_text(ci[k], "\n");
                    dt_op_begin(d, "list", NULL);
                    dt_op_member(d, "entry", v);
                    dt_op_end(d);
                    free(v);
                    d->counts.list++;
                }
            }
            for (i = 0; st && i < ns; ++i) free(st[i]);
            for (i = 0; ct && i < nc; ++i) free(ct[i]);
            free((void *)si); free((void *)ci); free(st); free(ct); free(kept); free(match);
            d->nseg--;
            free(seg);
            if (!ok && !d->failed) dt_op_set(d, c);
            return;
        }
        dt_diff_block(d, s, c);
        d->nseg--;
        free(seg);
        return;
    }
    dt_op_set(d, c);
}

/* Members of one key word, both sides. */
typedef struct dt_group { const sh_dt_node **s, **c; int ns, nc; } dt_group;

static void dt_diff_entries(dt_diff *d, const sh_dt_node *sb, dt_group *g)
{
    int i, k, named = 0, keyed = 1, unique = 1;
    unsigned char *used_s = (unsigned char *)calloc((size_t)g->ns + 1, 1);
    char **st = (char **)calloc((size_t)g->ns + 1, sizeof(char *));
    (void)sb;
    if (!used_s || !st) { free(used_s); free(st); d->failed = 1; return; }
    for (i = 0; i < g->ns; ++i) named |= g->s[i]->name != NULL;
    for (i = 0; i < g->nc; ++i) named |= g->c[i]->name != NULL;
    if (named) {
        /* Named blocks, unique by name on each side. */
        for (i = 0; i < g->ns && unique; ++i)
            for (k = i + 1; k < g->ns && unique; ++k)
                if (!g->s[i]->name || !g->s[k]->name || dt_ieq(g->s[i]->name, g->s[i]->name_n, g->s[k]->name, g->s[k]->name_n)) unique = 0;
        for (i = 0; i < g->nc && unique; ++i) {
            if (!g->c[i]->name) unique = 0;
            for (k = i + 1; k < g->nc && unique; ++k)
                if (!g->c[k]->name || dt_ieq(g->c[i]->name, g->c[i]->name_n, g->c[k]->name, g->c[k]->name_n)) unique = 0;
        }
        if (unique) {
            for (k = 0; k < g->nc; ++k) {
                const sh_dt_node *c = g->c[k], *s = NULL;
                for (i = 0; i < g->ns && !s; ++i)
                    if (dt_ieq(g->s[i]->name, g->s[i]->name_n, c->name, c->name_n)) { s = g->s[i]; used_s[i] = 1; }
                if (!s) { dt_op_stmt(d, "bag", c, NULL); d->counts.bag++; continue; }
                if (dt_same(s, c, 1)) continue;
                if (s->block && c->block && !s->list && !c->list && s->eq == c->eq) {
                    char *seg = dt_head(c);
                    if (!seg || d->nseg >= DT_PATH) { free(seg); d->failed = 1; continue; }
                    d->segs[d->nseg++] = seg;
                    dt_diff_block(d, s, c);
                    d->nseg--;
                    free(seg);
                } else {
                    dt_op_stmt(d, "unbag", s, NULL); d->counts.drop++;
                    dt_op_stmt(d, "bag", c, NULL); d->counts.bag++;
                }
            }
            for (i = 0; i < g->ns; ++i) if (!used_s[i]) { dt_op_stmt(d, "unbag", g->s[i], NULL); d->counts.drop++; }
            free(used_s);
            free(st);
            return;
        }
    } else {
        /* Keyed: every one a block with a name field, unique on each side. */
        const char *np[2]; size_t nn[2];
        for (i = 0; i < g->ns && keyed; ++i) keyed = g->s[i]->block && dt_name_field(g->s[i], &np[0], &nn[0]);
        for (i = 0; i < g->nc && keyed; ++i) keyed = g->c[i]->block && dt_name_field(g->c[i], &np[0], &nn[0]);
        for (i = 0; i < g->ns && keyed; ++i)
            for (k = i + 1; k < g->ns && keyed; ++k) {
                dt_name_field(g->s[i], &np[0], &nn[0]);
                dt_name_field(g->s[k], &np[1], &nn[1]);
                if (dt_ieq(np[0], nn[0], np[1], nn[1])) keyed = 0;
            }
        for (i = 0; i < g->nc && keyed; ++i)
            for (k = i + 1; k < g->nc && keyed; ++k) {
                dt_name_field(g->c[i], &np[0], &nn[0]);
                dt_name_field(g->c[k], &np[1], &nn[1]);
                if (dt_ieq(np[0], nn[0], np[1], nn[1])) keyed = 0;
            }
        if (keyed && g->ns + g->nc > 0) {
            for (k = 0; k < g->nc; ++k) {
                const sh_dt_node *c = g->c[k], *s = NULL;
                dt_name_field(c, &np[0], &nn[0]);
                for (i = 0; i < g->ns && !s; ++i) {
                    dt_name_field(g->s[i], &np[1], &nn[1]);
                    if (dt_ieq(np[0], nn[0], np[1], nn[1])) { s = g->s[i]; used_s[i] = 1; }
                }
                if (s && dt_same(s, c, 1)) continue;
                dt_op_keyed(d, "keyed", c, np[0], nn[0]);
            }
            for (i = 0; i < g->ns; ++i) if (!used_s[i]) {
                dt_name_field(g->s[i], &np[1], &nn[1]);
                dt_op_keyed(d, "unkey", g->s[i], np[1], nn[1]);
            }
            free(used_s);
            free(st);
            return;
        }
    }
    /* Bag: matched by the whole text. */
    for (i = 0; i < g->ns; ++i) st[i] = dt_sorted_text(g->s[i], 1);
    for (k = 0; k < g->nc; ++k) {
        char *ct = dt_sorted_text(g->c[k], 1);
        int hit = -1;
        for (i = 0; i < g->ns && hit < 0; ++i) if (!used_s[i] && st[i] && ct && !strcmp(st[i], ct)) hit = i;
        free(ct);
        if (hit >= 0) { used_s[hit] = 1; continue; }
        dt_op_stmt(d, "bag", g->c[k], NULL);
        d->counts.bag++;
    }
    for (i = 0; i < g->ns; ++i) if (!used_s[i]) { dt_op_stmt(d, "unbag", g->s[i], NULL); d->counts.drop++; }
    for (i = 0; i < g->ns; ++i) free(st[i]);
    free(st);
    free(used_s);
}

static void dt_diff_block(dt_diff *d, const sh_dt_node *s, const sh_dt_node *c)
{
    /* Key words in order of appearance: the copy's, then the game's alone. */
    int total = s->nkids + c->nkids, nw = 0, i, w;
    const sh_dt_node **words = (const sh_dt_node **)malloc(sizeof(void *) * (size_t)(total + 1));
    dt_group g;
    g.s = (const sh_dt_node **)malloc(sizeof(void *) * (size_t)(s->nkids + 1));
    g.c = (const sh_dt_node **)malloc(sizeof(void *) * (size_t)(c->nkids + 1));
    if (!words || !g.s || !g.c) { free((void *)words); free((void *)g.s); free((void *)g.c); d->failed = 1; return; }
    for (i = 0; i < total; ++i) {
        const sh_dt_node *n = i < c->nkids ? c->kids[i] : s->kids[i - c->nkids];
        int seen = 0;
        for (w = 0; w < nw && !seen; ++w) seen = dt_word_eq(words[w], n);
        if (!seen) words[nw++] = n;
    }
    for (w = 0; w < nw && !d->failed; ++w) {
        int named = 0, entry;
        g.ns = g.nc = 0;
        for (i = 0; i < s->nkids; ++i) if (dt_word_eq(s->kids[i], words[w])) { g.s[g.ns++] = s->kids[i]; named |= s->kids[i]->name != NULL; }
        for (i = 0; i < c->nkids; ++i) if (dt_word_eq(c->kids[i], words[w])) { g.c[g.nc++] = c->kids[i]; named |= c->kids[i]->name != NULL; }
        /* Entries: a key repeated on either side, a named block, or members
         * of a block the game leaves empty. */
        entry = g.ns > 1 || g.nc > 1 || named || (s->nkids == 0 && g.nc > 0);
        if (entry) { dt_diff_entries(d, s, &g); continue; }
        if (g.ns && !g.nc) dt_op_remove(d, g.s[0]);
        else if (!g.ns && g.nc) dt_op_set(d, g.c[0]);
        else dt_diff_member(d, g.s[0], g.c[0]);
    }
    free((void *)words);
    free((void *)g.s);
    free((void *)g.c);
}

int sh_dt_counts_total(const sh_dt_counts *c)
{
    return c ? c->set + c->remove + c->list + c->keyed + c->bag + c->drop : 0;
}

/* The SHA-256 is the caller's (modbundle has it); here a stable digest of the stock text. */
static unsigned long long dt_fnv(const char *s, size_t n)
{
    unsigned long long h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; ++i) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h;
}

char *sh_dt_diff(const char *request, const char *stock, size_t stock_len, const char *copy, size_t copy_len,
                 sh_dt_counts *counts, char *cause, size_t cap)
{
    sh_dt *s, *c;
    dt_diff d;
    dt_buf o = { NULL, 0, 0, 0 };
    char why[300], tmp[160];
    if (counts) memset(counts, 0, sizeof *counts);
    if (cause && cap) cause[0] = 0;
    s = sh_dt_parse(stock, stock_len, why, sizeof why);
    if (!s) { dt_cause(cause, cap, "the game's text does not parse: %s", why); return NULL; }
    c = sh_dt_parse(copy, copy_len, why, sizeof why);
    if (!c) { sh_dt_free(s); dt_cause(cause, cap, "%s", why); return NULL; }
    memset(&d, 0, sizeof d);
    d.first = 1;
    d.eol = "\n";
    dt_diff_block(&d, s->root, c->root);
    sh_dt_free(s);
    sh_dt_free(c);
    if (d.failed || d.out.oom) { free(d.out.p); dt_cause(cause, cap, "out of memory"); return NULL; }
    dt_puts(&o, "{\"file\":");
    dt_jcstr(&o, request ? request : "");
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, ",\"stock\":\"%016llx\"", dt_fnv(stock, stock_len));
    dt_puts(&o, tmp);
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, ",\"counts\":{\"set\":%d,\"remove\":%d,\"list\":%d,\"keyed\":%d,\"bag\":%d,\"drop\":%d}",
                d.counts.set, d.counts.remove, d.counts.list, d.counts.keyed, d.counts.bag, d.counts.drop);
    dt_puts(&o, tmp);
    dt_puts(&o, ",\"ops\":[");
    if (d.out.p) dt_put(&o, d.out.p, d.out.n);
    dt_puts(&o, "]}");
    free(d.out.p);
    if (counts) *counts = d.counts;
    if (o.oom) { free(o.p); dt_cause(cause, cap, "out of memory"); return NULL; }
    return dt_done(&o, NULL);
}

/* ------------------------------------------------------------------ navigation */

sh_dt_node *sh_dt_child(sh_dt_node *n, const char *key)
{
    int i, hits = 0;
    sh_dt_node *hit = NULL;
    if (!n || !n->block) return NULL;
    for (i = 0; i < n->nkids; ++i) if (dt_key_is(n->kids[i], key)) { hit = n->kids[i]; hits++; }
    return hits == 1 ? hit : NULL;
}

/* The member whose head is `seg`: its index, -1 none, -2 several. */
static int dt_find_seg(const sh_dt_node *n, const char *seg)
{
    int i, hit = -1;
    for (i = 0; i < n->nkids; ++i)
        if (dt_head_is(n->kids[i], seg)) { if (hit >= 0) return -2; hit = i; }
    return hit;
}

sh_dt_node *sh_dt_at(sh_dt_node *n, const char *path)
{
    const char *p = path;
    while (n && p && *p) {
        const char *e = strchr(p, '/');
        size_t sl = e ? (size_t)(e - p) : strlen(p);
        char *seg = sh_textn(p, sl);
        int k;
        if (!seg) return NULL;
        k = dt_find_seg(n, seg);
        free(seg);
        n = k >= 0 && n->kids[k]->block ? n->kids[k] : NULL;
        p = e ? e + 1 : NULL;
    }
    return n;
}

int sh_dt_set_value(sh_dt *t, sh_dt_node *n, const char *val, size_t len)
{
    char *v;
    if (!t || !n || n->block) return 0;
    v = dt_copy(t, val, len);
    if (!v) return 0;
    n->val = v;
    n->val_n = len;
    return 1;
}

/* ------------------------------------------------------------------ apply */

enum { OP_SET, OP_REMOVE, OP_LIST, OP_UNLIST, OP_KEYED, OP_UNKEY, OP_BAG, OP_UNBAG, OP_BAD };

typedef struct dt_op {
    int kind;
    const char *path[DT_PATH];         /* into the parsed JSON */
    int npath;
    const char *word, *name, *entry;
    char *slot;                        /* malloc'd (dt_src_ops_free) */
    unsigned long long digest;
    int skip;                          /* lost a clash */
    int lost_to, lost_by_choice;       /* with skip: the source that serves the slot, and whether the map chose it */
} dt_op;

typedef struct dt_src_ops { dj *json; dt_op *ops; int n; } dt_src_ops;

static void dt_src_ops_free(dt_src_ops *so)
{
    int k;
    for (k = 0; so->ops && k < so->n; ++k) free(so->ops[k].slot);
    free(so->ops);
    dj_free(so->json);
    so->ops = NULL;
    so->json = NULL;
    so->n = 0;
}

static int dt_op_kind(const char *s)
{
    static const char *const names[] = { "set", "remove", "list", "unlist", "keyed", "unkey", "bag", "unbag" };
    int i;
    for (i = 0; s && i < 8; ++i) if (!strcmp(s, names[i])) return i;
    return OP_BAD;
}

char *sh_dt_game_name(const char *request)
{
    size_t n;
    if (!request) return NULL;
    n = strlen(request);
    if (n > 21 && !_strnicmp(request, "generated/decls/", 16) && !_stricmp(request + n - 5, ".decl"))
        return sh_textn(request + 16, n - 21);
    return sh_text(request);
}

/* One slot segment: the key, or `word:name` for a named block. */
static void dt_slot_seg(dt_buf *b, const char *seg)
{
    const char *sp = strchr(seg, ' ');
    if (!sp) { dt_puts(b, seg); return; }
    dt_put(b, seg, (size_t)(sp - seg));
    dt_put(b, ":", 1);
    sp++;
    if (*sp == '"') { size_t n = strlen(sp); dt_put(b, sp + 1, n >= 2 ? n - 2 : 0); }
    else dt_puts(b, sp);
}

static void dt_op_slot(dt_op *op, const char *game)
{
    dt_buf b = { NULL, 0, 0, 0 };
    int i;
    char *s;
    dt_puts(&b, game);
    for (i = 0; i < op->npath; ++i) { dt_put(&b, "/", 1); dt_slot_seg(&b, op->path[i]); }
    if (op->kind == OP_KEYED || op->kind == OP_UNKEY) { dt_put(&b, "/", 1); dt_puts(&b, op->name ? op->name : ""); }
    s = dt_done(&b, NULL);
    free(op->slot);
    op->slot = s ? s : sh_text("");
}

/* The digest of what an op sets its slot to (removal is its own value). */
static unsigned long long dt_op_digest(const dt_op *op)
{
    sh_dt *t;
    unsigned long long h;
    char *txt;
    if (op->kind == OP_REMOVE || op->kind == OP_UNKEY || !op->entry) return 0x52454d4f564544ull;
    t = sh_dt_parse("{}", 2, NULL, 0);
    h = dt_fnv(op->entry, strlen(op->entry));
    if (t) {
        dt_buf w = { NULL, 0, 0, 0 };
        sh_dt_node *root;
        char *in;
        dt_puts(&w, "{\n");
        dt_puts(&w, op->entry);
        dt_puts(&w, "\n}");
        in = w.p ? dt_copy(t, w.p, w.n) : NULL;
        root = in ? dt_parse_into(t, in, w.n, NULL, 0) : NULL;
        if (root && root->nkids == 1 && (txt = dt_sorted_text(root->kids[0], 1)) != NULL) { h = dt_fnv(txt, strlen(txt)); free(txt); }
        free(w.p);
        sh_dt_free(t);
    }
    return h;
}

/* A source's operations for `request`. 1 with ops (possibly none). */
static int dt_src_load(const sh_dt_source *s, const char *request, const char *game, dt_src_ops *out)
{
    const dj *arr;
    int i, k;
    memset(out, 0, sizeof *out);
    if (!s->edits || !s->edits_len) return 1;
    out->json = dj_parse(s->edits, s->edits_len);
    if (!out->json || out->json->t != DJ_ARR) return 0;
    arr = out->json;
    for (i = 0; i < arr->count; ++i) {
        const dj *e = arr->items[i], *ops;
        const char *file = dj_str(e, "file");
        if (!file || _stricmp(file, request)) continue;
        ops = dj_get(e, "ops");
        if (!ops || ops->t != DJ_ARR) return 0;
        out->ops = (dt_op *)calloc((size_t)ops->count + 1, sizeof(dt_op));
        if (!out->ops) return 0;
        for (k = 0; k < ops->count; ++k) {
            const dj *o = ops->items[k], *path = dj_get(o, "path");
            dt_op *op = &out->ops[out->n];
            int p;
            op->kind = dt_op_kind(dj_str(o, "op"));
            if (op->kind == OP_BAD || !path || path->t != DJ_ARR || path->count > DT_PATH) continue;
            for (p = 0; p < path->count; ++p) {
                if (path->items[p]->t != DJ_STR) break;
                op->path[op->npath++] = path->items[p]->s;
            }
            if (p < path->count) continue;
            op->word = dj_str(o, "word");
            op->name = dj_str(o, "name");
            op->entry = dj_str(o, "entry");
            if ((op->kind == OP_SET || op->kind == OP_REMOVE) && !op->npath) continue;
            if (op->kind != OP_REMOVE && op->kind != OP_UNKEY && !op->entry) continue;
            if ((op->kind == OP_KEYED || op->kind == OP_UNKEY) && (!op->word || !op->name)) continue;
            dt_op_slot(op, game);
            out->n++;
        }
        break;
    }
    return 1;
}

static void dt_line(sh_dt_report *r, const char *fmt, ...)
{
    char *buf;
    va_list ap;
    if (!r || !r->line) return;
    va_start(ap, fmt);
    buf = sh_vtextf(fmt, ap);
    va_end(ap);
    if (buf) r->line(r->context, buf);
    free(buf);
}

/* Parse an op's statement into the tree: the one member of `{ <entry> }`. */
static sh_dt_node *dt_entry_stmt(sh_dt *t, const char *entry)
{
    size_t n = strlen(entry);
    char *in = (char *)dt_alloc(t, n + 6);
    sh_dt_node *root;
    if (!in) return NULL;
    memcpy(in, "{\n", 2);
    memcpy(in + 2, entry, n);
    memcpy(in + 2 + n, "\n}", 3);
    root = dt_parse_into(t, in, n + 4, NULL, 0);
    return root && root->nkids == 1 ? root->kids[0] : NULL;
}

/* A list item from its value text. */
static sh_dt_node *dt_entry_item(sh_dt *t, const char *entry)
{
    size_t n = strlen(entry);
    char *in = (char *)dt_alloc(t, n + 16);
    sh_dt_node *root;
    if (!in) return NULL;
    memcpy(in, "{\nitem[0] = ", 12);
    memcpy(in + 12, entry, n);
    memcpy(in + 12 + n, "\n}", 3);
    root = dt_parse_into(t, in, n + 14, NULL, 0);
    return root && root->nkids == 1 ? root->kids[0] : NULL;
}

static void dt_mark_src(sh_dt_node *n, int src)
{
    int i;
    n->src = src;
    for (i = 0; i < n->nkids; ++i) dt_mark_src(n->kids[i], src);
}

static int dt_count_word(const sh_dt_node *n, const sh_dt_node *like)
{
    int i, c = 0;
    for (i = 0; i < n->nkids; ++i) if (dt_word_eq(n->kids[i], like) && (!like->name || (n->kids[i]->name &&
        dt_ieq(n->kids[i]->name, n->kids[i]->name_n, like->name, like->name_n)))) c++;
    return c;
}

/* Two mods' blocks of one named entry: the union of their members, the
 * held block's first; a scalar both set otherwise keeps the held value.
 * Returns the members appended, -1 out of memory. */
static int dt_union(sh_dt *t, sh_dt_node *held, const sh_dt_node *o, int src, int *kept)
{
    int i, k, rows = 0;
    for (i = 0; i < o->nkids; ++i) {
        sh_dt_node *m = o->kids[i];
        int dup = 0, same = -1, nsame = 0;
        for (k = 0; k < held->nkids && !dup; ++k) {
            const sh_dt_node *h = held->kids[k];
            if (!dt_word_eq(h, m)) continue;
            if (dt_same(h, m, 1)) { dup = 1; break; }
            if ((!h->name && !m->name) || (h->name && m->name && dt_ieq(h->name, h->name_n, m->name, m->name_n))) { same = k; nsame++; }
        }
        if (dup) continue;
        if (nsame == 1 && dt_count_word(o, m) == 1) {
            sh_dt_node *h = held->kids[same];
            if (!h->block && !m->block) { (*kept)++; continue; }
            if (h->block && m->block && h->name && m->name && !h->list && !m->list) {
                int r = dt_union(t, h, m, src, kept);
                if (r < 0) return -1;
                rows += r;
                continue;
            }
        }
        dt_mark_src(m, src);
        if (!dt_add_kid(t, held, m)) return -1;
        rows++;
    }
    return rows;
}

static sh_dt_node *dt_nav(sh_dt_node *n, const char *const *path, int npath, const char **why)
{
    int i;
    for (i = 0; n && i < npath; ++i) {
        int k = dt_find_seg(n, path[i]);
        if (k == -2) { *why = "the path names several members"; return NULL; }
        if (k < 0) { *why = "the path is not in the text"; return NULL; }
        if (!n->kids[k]->block) { *why = "the path ends at a value, not a block"; return NULL; }
        n = n->kids[k];
    }
    return n;
}

/* The op's path, "/"-joined (malloc'd), or NULL. */
static char *dt_path_text(const dt_op *op, int npath)
{
    dt_buf b = { NULL, 0, 0, 0 };
    int i;
    if (!npath) return sh_text("(the decl)");
    for (i = 0; i < npath; ++i) { if (i) dt_put(&b, "/", 1); dt_puts(&b, op->path[i]); }
    return dt_done(&b, NULL);
}

/* Apply one op of source `s`. 1 changed, 0 merged (nothing to do), -1 skipped (*why). */
static int dt_apply_op(sh_dt *t, const dt_op *op, int s, const char **why)
{
    static const char *const kinds[] = { "set", "remove", "list", "unlist", "keyed", "unkey", "bag", "unbag" };
    int nav = op->kind == OP_SET || op->kind == OP_REMOVE ? op->npath - 1 : op->npath;
    sh_dt_node *box = dt_nav(t->root, op->path, nav, why), *e;
    int i, hit = -1, hits = 0;
    (void)kinds;
    if (!box) return -1;
    switch (op->kind) {
    case OP_SET:
        e = dt_entry_stmt(t, op->entry);
        if (!e) { *why = "its entry does not parse"; return -1; }
        i = dt_find_seg(box, op->path[op->npath - 1]);
        if (i == -2) { *why = "the field appears several times"; return -1; }
        dt_mark_src(e, s);
        if (i >= 0) {
            if (dt_same(box->kids[i], e, 1)) return 0;
            box->kids[i] = e;
            return 1;
        }
        return dt_add_kid(t, box, e) ? 1 : -1;
    case OP_REMOVE:
        i = dt_find_seg(box, op->path[op->npath - 1]);
        if (i == -2) { *why = "the field appears several times"; return -1; }
        if (i < 0) return 0;
        dt_remove_kid(box, i);
        return 1;
    case OP_LIST:
    case OP_UNLIST: {
        char *et, *kt;
        if (!box->list) { *why = "the container is not an ordered list"; return -1; }
        e = dt_entry_item(t, op->entry);
        if (!e) { *why = "its entry does not parse"; return -1; }
        et = dt_sorted_text(e, 0);
        for (i = 0; et && i < box->nkids && hit < 0; ++i) {
            if (dt_key_is(box->kids[i], "num") && !box->kids[i]->block) continue;
            kt = dt_sorted_text(box->kids[i], 0);
            if (kt && !strcmp(kt, et)) hit = i;
            free(kt);
        }
        free(et);
        if (op->kind == OP_UNLIST) {
            if (hit < 0) return 0;
            dt_remove_kid(box, hit);
            return 1;
        }
        if (hit >= 0) return 0;
        dt_mark_src(e, s);
        return dt_add_kid(t, box, e) ? 1 : -1;
    }
    case OP_KEYED:
    case OP_UNKEY:
        for (i = 0; i < box->nkids; ++i) {
            const char *np;
            size_t nn;
            if (!dt_ieq(box->kids[i]->key, box->kids[i]->key_n, op->word, strlen(op->word))) continue;
            if (!box->kids[i]->block || !dt_name_field(box->kids[i], &np, &nn)) continue;
            if (dt_ieq(np, nn, op->name, strlen(op->name))) { hit = i; hits++; }
        }
        if (hits > 1) { *why = "several entries have that name"; return -1; }
        if (op->kind == OP_UNKEY) {
            if (hit < 0) return 0;
            dt_remove_kid(box, hit);
            return 1;
        }
        e = dt_entry_stmt(t, op->entry);
        if (!e) { *why = "its entry does not parse"; return -1; }
        dt_mark_src(e, s);
        if (hit >= 0) {
            if (dt_same(box->kids[hit], e, 1)) return 0;
            box->kids[hit] = e;
            return 1;
        }
        return dt_add_kid(t, box, e) ? 1 : -1;
    case OP_BAG:
    case OP_UNBAG: {
        char *et;
        e = dt_entry_stmt(t, op->entry);
        if (!e) { *why = "its entry does not parse"; return -1; }
        if (op->kind == OP_BAG && e->name) {
            for (i = 0; i < box->nkids; ++i)
                if (dt_word_eq(box->kids[i], e) && box->kids[i]->name &&
                    dt_ieq(box->kids[i]->name, box->kids[i]->name_n, e->name, e->name_n)) { hit = i; hits++; }
            if (hits == 1) {
                int kept = 0, rows;
                if (dt_same(box->kids[hit], e, 1)) return 0;
                if (!box->kids[hit]->block || !e->block) { *why = "the named entry is not a block on both sides"; return -1; }
                rows = dt_union(t, box->kids[hit], e, s, &kept);
                if (rows < 0) return -1;
                return rows > 0 ? 2 : 0;   /* 2: merged into the entry another source holds */
            }
            hit = -1;
        }
        et = dt_sorted_text(e, 1);
        for (i = 0; et && i < box->nkids && hit < 0; ++i) {
            char *kt = dt_sorted_text(box->kids[i], 1);
            if (kt && !strcmp(kt, et)) hit = i;
            free(kt);
        }
        free(et);
        if (op->kind == OP_UNBAG) {
            if (hit < 0) return 0;
            dt_remove_kid(box, hit);
            return 1;
        }
        if (hit >= 0) return 0;
        dt_mark_src(e, s);
        return dt_add_kid(t, box, e) ? 1 : -1;
    }
    default:
        *why = "an operation this release does not know";
        return -1;
    }
}

static const sh_dt_choice *dt_choice(const sh_dt_choice *ch, int n, const char *slot)
{
    int i;
    for (i = 0; ch && i < n; ++i) if (ch[i].slot && ch[i].key && !_stricmp(ch[i].slot, slot)) return &ch[i];
    return NULL;
}

sh_dt *sh_dt_apply(const char *request, const char *stock, size_t stock_len, const sh_dt_source *src, int n,
                   const sh_dt_choice *choices, int nchoices, sh_dt_report *report)
{
    char *game, why[SH_DT_CAUSE_CAP];
    dt_src_ops *so;
    sh_dt *t = NULL;
    int s, k, any = 0;
    sh_dt_report none;
    if (!report) { memset(&none, 0, sizeof none); report = &none; }
    report->applied = report->merged = report->skipped = report->clashes = report->sources = 0;
    if (!request || !stock || !(game = sh_dt_game_name(request))) return NULL;
    so = (dt_src_ops *)calloc((size_t)(n > 0 ? n : 1), sizeof *so);
    if (!so) { free(game); return NULL; }
    for (s = 0; s < n; ++s) {
        if (!dt_src_load(&src[s], request, game, &so[s]))
            dt_line(report, "%s: %s's edits do not read; nothing taken from them", game, src[s].who ? src[s].who : "?");
        if (so[s].n) { any = 1; report->sources++; }
    }
    if (!any) goto done;
    t = sh_dt_parse(stock, stock_len, why, sizeof why);
    if (!t) { dt_line(report, "%s: the game's text does not parse (%s); the game's own file serves", game, why); goto done; }
    /* Slots several sources set to different values: one serves. */
    for (s = 0; s < n; ++s)
        for (k = 0; k < so[s].n; ++k) {
            dt_op *op = &so[s].ops[k];
            if (op->kind == OP_SET || op->kind == OP_REMOVE || op->kind == OP_KEYED || op->kind == OP_UNKEY)
                op->digest = dt_op_digest(op);
        }
    for (s = 0; s < n; ++s)
        for (k = 0; k < so[s].n; ++k) {
            dt_op *op = &so[s].ops[k];
            int who[64], nwho = 0, differ = 0, serving, by_choice = 0, s2, k2, w;
            unsigned long long first_digest = op->digest;
            const sh_dt_choice *ch;
            dt_buf list = { NULL, 0, 0, 0 };
            const char *names;
            if (op->skip || !(op->kind == OP_SET || op->kind == OP_REMOVE || op->kind == OP_KEYED || op->kind == OP_UNKEY)) continue;
            /* The first claim of this slot (earlier claims were handled with their slot). */
            {
                int earlier = 0;
                for (s2 = 0; s2 < s && !earlier; ++s2)
                    for (k2 = 0; k2 < so[s2].n && !earlier; ++k2)
                        earlier = (so[s2].ops[k2].kind == OP_SET || so[s2].ops[k2].kind == OP_REMOVE ||
                                   so[s2].ops[k2].kind == OP_KEYED || so[s2].ops[k2].kind == OP_UNKEY) &&
                                  !_stricmp(so[s2].ops[k2].slot, op->slot);
                if (earlier) continue;
            }
            for (s2 = s; s2 < n; ++s2)
                for (k2 = 0; k2 < so[s2].n; ++k2) {
                    dt_op *o2 = &so[s2].ops[k2];
                    if (!(o2->kind == OP_SET || o2->kind == OP_REMOVE || o2->kind == OP_KEYED || o2->kind == OP_UNKEY)) continue;
                    if (_stricmp(o2->slot, op->slot)) continue;
                    if (o2->digest != first_digest) differ = 1;
                    if (nwho < 64 && (!nwho || who[nwho - 1] != s2)) who[nwho++] = s2;
                    break;
                }
            if (!differ || nwho < 2) continue;
            serving = who[0];
            ch = dt_choice(choices, nchoices, op->slot);
            if (ch)
                for (w = 0; w < nwho; ++w) if (src[who[w]].key && !_stricmp(src[who[w]].key, ch->key)) { serving = who[w]; by_choice = 1; break; }
            /* The serving source's value: every source whose value differs loses. */
            {
                unsigned long long win = 0;
                for (k2 = 0; k2 < so[serving].n; ++k2) if (!_stricmp(so[serving].ops[k2].slot, op->slot)) { win = so[serving].ops[k2].digest; break; }
                for (w = 0; w < nwho; ++w)
                    for (k2 = 0; k2 < so[who[w]].n; ++k2) {
                        dt_op *o2 = &so[who[w]].ops[k2];
                        if (!_stricmp(o2->slot, op->slot) && (o2->kind == OP_SET || o2->kind == OP_REMOVE ||
                            o2->kind == OP_KEYED || o2->kind == OP_UNKEY) && o2->digest != win) {
                            o2->skip = 1;
                            o2->lost_to = serving;
                            o2->lost_by_choice = by_choice;
                        }
                    }
            }
            for (w = 0; w < nwho; ++w) {
                if (w) dt_puts(&list, ", ");
                dt_puts(&list, src[who[w]].who ? src[who[w]].who : "?");
            }
            names = list.p && !list.oom ? list.p : "";
            report->clashes++;
            if (by_choice) dt_line(report, "%s: set by %s; serving %s (the map's choice)", op->slot, names, src[serving].who);
            else if (ch)
                dt_line(report, "%s: set by %s; the map chose %s, which does not set it; serving %s (row order)", op->slot,
                        names, ch->key, src[serving].who);
            else
                dt_line(report, "%s: set by %s; serving %s (row order; choose with sh_mods choose \"%s\" \"<mod name>\")", op->slot,
                        names, src[serving].who, op->slot);
            free(list.p);
            if (report->clash) report->clash(report->context, op->slot, who, nwho, serving, by_choice);
        }
    for (s = 0; s < n; ++s) {
        int changed = 0, merged = 0, skipped = 0, rows = 0, lost = 0, lost_to = -1, lost_choice = 0, lost_mixed = 0;
        char *superseded = NULL;
        for (k = 0; k < so[s].n; ++k) {
            const dt_op *op = &so[s].ops[k];
            const char *w = NULL;
            int r;
            if (op->skip) {
                /* lost a clash: another source's value serves the slot */
                if (lost && (op->lost_to != lost_to || op->lost_by_choice != lost_choice)) lost_mixed = 1;
                lost_to = op->lost_to;
                lost_choice = op->lost_by_choice;
                lost++;
                continue;
            }
            r = dt_apply_op(t, op, s, &w);
            if (r == 1) changed++;
            else if (r == 2) { changed++; rows++; }
            else if (r == 0) merged++;
            else {
                static const char *const kinds[] = { "set", "remove", "list", "unlist", "keyed", "unkey", "bag", "unbag" };
                skipped++;
                if (skipped <= 4) {
                    char *path = dt_path_text(op, op->npath);
                    dt_line(report, "%s: %s: %s %s%s%s: %s; skipped", game, src[s].who, op->kind < OP_BAD ? kinds[op->kind] : "?",
                            path ? path : "", op->name ? " " : "", op->name ? op->name : "", w ? w : "out of memory");
                    free(path);
                }
            }
        }
        if (skipped > 4) dt_line(report, "%s: %s: %d more operation(s) skipped", game, src[s].who, skipped - 4);
        if (lost) {
            const char *by = lost_to >= 0 && lost_to < n ? (src[lost_to].key ? src[lost_to].key : src[lost_to].who) : NULL;
            if (lost_mixed || !by) superseded = sh_textf(", %d superseded by other mods' values", lost);
            else if (lost_choice) superseded = sh_textf(", %d superseded by %s's choice", lost, by);
            else superseded = sh_textf(", %d superseded by %s (row order)", lost, by);
        }
        if (so[s].n)
            dt_line(report, "%s: %d edit(s) from %s: %d applied%s%s, %d already held%s%s", game, so[s].n, src[s].who, changed,
                    rows ? ", " : "", rows ? "some merged into an entry another mod adds" : "", merged,
                    superseded ? superseded : "", skipped ? ", some skipped" : "");
        free(superseded);
        if (changed && report->used) report->used[s] = 1;
        report->applied += changed;
        report->merged += merged;
        report->skipped += skipped;
    }
    if (t->failed) { sh_dt_free(t); t = NULL; }
done:
    for (s = 0; s < n; ++s) dt_src_ops_free(&so[s]);
    free(so);
    free(game);
    return t;
}

/* ------------------------------------------------------------------ edits readers */

static void dt_counts_of(const dj *e, sh_dt_counts *c)
{
    const dj *cn = dj_get(e, "counts");
    memset(c, 0, sizeof *c);
    if (cn) {
        c->set = dj_int(cn, "set"); c->remove = dj_int(cn, "remove"); c->list = dj_int(cn, "list");
        c->keyed = dj_int(cn, "keyed"); c->bag = dj_int(cn, "bag"); c->drop = dj_int(cn, "drop");
    } else {
        const dj *ops = dj_get(e, "ops");
        int k;
        for (k = 0; ops && ops->t == DJ_ARR && k < ops->count; ++k)
            switch (dt_op_kind(dj_str(ops->items[k], "op"))) {
            case OP_SET: c->set++; break;
            case OP_REMOVE: c->remove++; break;
            case OP_LIST: c->list++; break;
            case OP_KEYED: c->keyed++; break;
            case OP_BAG: c->bag++; break;
            case OP_BAD: break;
            default: c->drop++; break;
            }
    }
}

int sh_dt_edits_files(const char *edits, size_t len,
                      void (*fn)(void *context, const char *file, const sh_dt_counts *counts), void *context)
{
    dj *a = dj_parse(edits, len);
    int i, n = 0;
    if (!a || a->t != DJ_ARR) { dj_free(a); return -1; }
    for (i = 0; i < a->count; ++i) {
        const char *file = dj_str(a->items[i], "file");
        sh_dt_counts c;
        if (!file) continue;
        dt_counts_of(a->items[i], &c);
        if (fn) fn(context, file, &c);
        n++;
    }
    dj_free(a);
    return n;
}

int sh_dt_edits_slots(const char *request, const char *edits, size_t len,
                      void (*fn)(void *context, const char *slot, unsigned long long digest), void *context)
{
    sh_dt_source s;
    dt_src_ops so;
    char *game;
    int k, n = 0;
    memset(&s, 0, sizeof s);
    memset(&so, 0, sizeof so);
    s.edits = edits;
    s.edits_len = len;
    if (!(game = sh_dt_game_name(request))) return -1;
    if (!dt_src_load(&s, request, game, &so)) { dt_src_ops_free(&so); free(game); return -1; }
    for (k = 0; k < so.n; ++k) {
        dt_op *op = &so.ops[k];
        if (!(op->kind == OP_SET || op->kind == OP_REMOVE || op->kind == OP_KEYED || op->kind == OP_UNKEY)) continue;
        if (fn) fn(context, op->slot, dt_op_digest(op));
        n++;
    }
    dt_src_ops_free(&so);
    free(game);
    return n;
}
