/* sdkparse.c -- the Graphics SDK's author grammar (sdkparse.h). Pure text work. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkparse.h"

void sdk_put(sdk_buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *p;
        while (cap < b->n + n + 1) cap *= 2;
        if (!(p = (char *)realloc(b->p, cap))) { b->oom = 1; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void sdk_printf(sdk_buf *b, const char *fmt, ...)
{
    char small[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) { b->oom = 1; return; }
    if ((size_t)n < sizeof small) { sdk_put(b, small, (size_t)n); return; }
    {
        char *big = (char *)malloc((size_t)n + 1);
        if (!big) { b->oom = 1; return; }
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        sdk_put(b, big, (size_t)n);
        free(big);
    }
}

void sdk_fmt(char *out, size_t cap, double v)
{
    char *e;
    if (v > -5e-8 && v < 5e-8) { snprintf(out, cap, "0"); return; }
    snprintf(out, cap, "%.9g", v);
    if (strchr(out, 'e')) { snprintf(out, cap, "%.10f", v); }
    if (strchr(out, '.')) {
        e = out + strlen(out) - 1;
        while (e > out && *e == '0') *e-- = 0;
        if (*e == '.') *e = 0;
    }
}

typedef struct ar_block { struct ar_block *next; size_t used, cap; double align[1]; } ar_block;

static void *ar_alloc(sdk_doc *d, size_t n)
{
    ar_block *b = (ar_block *)d->arena;
    n = (n + 15) & ~(size_t)15;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > 16384 ? n : 16384;
        ar_block *nb = (ar_block *)malloc(sizeof *nb + cap);
        if (!nb) return NULL;
        nb->next = b;
        nb->used = 0;
        nb->cap = cap;
        d->arena = nb;
        b = nb;
    }
    {
        void *p = (char *)(b + 1) + b->used;
        b->used += n;
        memset(p, 0, n);
        return p;
    }
}

void sdk_doc_free(sdk_doc *doc)
{
    ar_block *b;
    if (!doc) return;
    b = (ar_block *)doc->arena;
    while (b) { ar_block *n = b->next; free(b); b = n; }
    doc->arena = NULL;
    doc->root = NULL;
}

/* Grows an array held in the arena (the previous storage stays in the arena). */
static int ar_grow(sdk_doc *d, void **arr, size_t *cap, size_t count, size_t elem)
{
    size_t nc;
    void *p;
    if (count < *cap) return 1;
    nc = *cap ? *cap * 2 : 8;
    if (!(p = ar_alloc(d, nc * elem))) return 0;
    if (*arr && count) memcpy(p, *arr, count * elem);
    *arr = p;
    *cap = nc;
    return 1;
}

typedef struct lx { const char *t; size_t len, pos, line; sdk_doc *d; char *why; size_t why_len; int bad; } lx;

static const char *base_name(const char *path)
{
    const char *b = path ? path : "";
    const char *s;
    for (s = b; *s; s++) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

void sdk_say(char *why, size_t why_len, const sdk_doc *d, size_t line, const char *fmt, ...)
{
    char text[400];
    va_list ap;
    if (!why || !why_len) return;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    snprintf(why, why_len, "%s line %u: %s", d ? d->file : "", (unsigned)line, text);
}

/* Skips blanks and comments; stops at a newline (returned as 1) or the next token. */
static int lx_space(lx *x)
{
    int nl = 0;
    while (x->pos < x->len) {
        char c = x->t[x->pos];
        if (c == '\n') { nl = 1; x->line++; x->pos++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { x->pos++; continue; }
        if (c == '/' && x->pos + 1 < x->len && x->t[x->pos + 1] == '/') {
            while (x->pos < x->len && x->t[x->pos] != '\n') x->pos++;
            continue;
        }
        if (c == '/' && x->pos + 1 < x->len && x->t[x->pos + 1] == '*') {
            x->pos += 2;
            while (x->pos + 1 < x->len && !(x->t[x->pos] == '*' && x->t[x->pos + 1] == '/')) {
                if (x->t[x->pos] == '\n') x->line++;
                x->pos++;
            }
            x->pos = x->pos + 2 <= x->len ? x->pos + 2 : x->len;
            continue;
        }
        break;
    }
    return nl;
}

static int word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '/' || c == '.' || c == '-' || c == '\\' || c == ':' ||
           c == '$' || c == '#' || c == '@' || c == '+';
}

static int parse_group(lx *x, sdk_group *g);

/* One atom at x->pos (after blanks). 1 read, 0 none (a separator or '}'), -1 failed. */
static int lx_atom(lx *x, sdk_atom *a)
{
    char c = x->t[x->pos];
    memset(a, 0, sizeof *a);
    a->line = x->line;
    if (c == '{') {
        x->pos++;
        if (!(a->group = (sdk_group *)ar_alloc(x->d, sizeof *a->group))) return -1;
        a->group->line = x->line;
        a->kind = SDK_A_GROUP;
        return parse_group(x, a->group) ? 1 : -1;
    }
    if (c == '"') {
        size_t s = ++x->pos;
        while (x->pos < x->len && x->t[x->pos] != '"' && x->t[x->pos] != '\n') x->pos++;
        if (x->pos >= x->len || x->t[x->pos] != '"') {
            sdk_say(x->why, x->why_len, x->d, x->line, "a name in quotes is not closed; end it with \".");
            x->bad = 1;
            return -1;
        }
        a->kind = SDK_A_STRING;
        a->p = x->t + s;
        a->n = x->pos - s;
        x->pos++;
        return 1;
    }
    if (word_char(c)) {
        size_t s = x->pos;
        char *end = NULL;
        while (x->pos < x->len && word_char(x->t[x->pos])) x->pos++;
        a->p = x->t + s;
        a->n = x->pos - s;
        /* native("name") */
        if (a->n == 6 && !_strnicmp(a->p, "native", 6) && x->pos < x->len && x->t[x->pos] == '(') {
            size_t q = x->pos + 1, qs;
            while (q < x->len && (x->t[q] == ' ' || x->t[q] == '\t')) q++;
            if (q < x->len && x->t[q] == '"') {
                qs = ++q;
                while (q < x->len && x->t[q] != '"' && x->t[q] != '\n') q++;
                if (q < x->len && x->t[q] == '"') {
                    size_t qe = q++;
                    while (q < x->len && (x->t[q] == ' ' || x->t[q] == '\t')) q++;
                    if (q < x->len && x->t[q] == ')') {
                        a->kind = SDK_A_STRING;
                        a->game = 1;
                        a->p = x->t + qs;
                        a->n = qe - qs;
                        x->pos = q + 1;
                        return 1;
                    }
                }
            }
            sdk_say(x->why, x->why_len, x->d, x->line, "write one of DOOM's names as native(\"name\").");
            x->bad = 1;
            return -1;
        }
        /* native:name, DOOM's own name */
        if (a->n > 7 && !_strnicmp(a->p, "native:", 7)) {
            a->kind = SDK_A_STRING;
            a->game = 1;
            a->p += 7;
            a->n -= 7;
            return 1;
        }
        {
            char tmp[64];
            if (a->n < sizeof tmp) {
                memcpy(tmp, a->p, a->n);
                tmp[a->n] = 0;
                a->num = strtod(tmp, &end);
                if (end && end != tmp && *end == 0 && (isdigit((unsigned char)tmp[0]) || tmp[0] == '-' || tmp[0] == '+' || tmp[0] == '.')) {
                    a->kind = SDK_A_NUMBER;
                    return 1;
                }
            }
        }
        a->kind = SDK_A_WORD;
        return 1;
    }
    return 0;
}

static int add_item(lx *x, sdk_group *g, sdk_atom *atoms, size_t n, size_t line)
{
    sdk_item *it;
    if (!ar_grow(x->d, (void **)&g->items, &g->cap, g->count, sizeof *g->items)) return 0;
    it = &g->items[g->count++];
    it->count = n;
    it->line = line;
    if (n) {
        if (!(it->atoms = (sdk_atom *)ar_alloc(x->d, n * sizeof *it->atoms))) return 0;
        memcpy(it->atoms, atoms, n * sizeof *atoms);
    }
    return 1;
}

/* A group's items, after its '{', up to and including its '}'. */
static int parse_group(lx *x, sdk_group *g)
{
    sdk_atom row[24];
    size_t n = 0, line = x->line;
    for (;;) {
        int nl = lx_space(x);
        if (nl && n) {
            if (!add_item(x, g, row, n, line)) return 0;
            n = 0;
        }
        if (x->pos >= x->len) {
            sdk_say(x->why, x->why_len, x->d, g->line, "a '{' here is never closed; add the '}' that ends it.");
            x->bad = 1;
            return 0;
        }
        if (x->t[x->pos] == '}') {
            x->pos++;
            if (n && !add_item(x, g, row, n, line)) return 0;
            return 1;
        }
        if (x->t[x->pos] == ',') {
            x->pos++;
            if (n && !add_item(x, g, row, n, line)) return 0;
            n = 0;
            continue;
        }
        {
            sdk_atom a;
            int r;
            if (!n) line = x->line;
            r = lx_atom(x, &a);
            if (r < 0) return 0;
            if (r == 0) {
                sdk_say(x->why, x->why_len, x->d, x->line, "'%c' is not part of the SDK's grammar; remove it.",
                        x->t[x->pos]);
                x->bad = 1;
                return 0;
            }
            if (n == sizeof row / sizeof row[0]) {
                sdk_say(x->why, x->why_len, x->d, line, "this line holds too many values; put one field on a line.");
                x->bad = 1;
                return 0;
            }
            row[n++] = a;
        }
    }
}

int sdk_parse(const char *path, const char *text, size_t len, sdk_doc *doc, char *why, size_t why_len)
{
    lx x;
    memset(doc, 0, sizeof *doc);
    snprintf(doc->file, sizeof doc->file, "%s", base_name(path));
    if (why && why_len) why[0] = 0;
    memset(&x, 0, sizeof x);
    x.t = text;
    x.len = len;
    x.line = 1;
    x.d = doc;
    x.why = why;
    x.why_len = why_len;
    lx_space(&x);
    if (x.pos >= len || text[x.pos] != '{') {
        sdk_say(why, why_len, doc, x.line, "the file starts with '{' and ends with '}'.");
        return 0;
    }
    x.pos++;
    if (!(doc->root = (sdk_group *)ar_alloc(doc, sizeof *doc->root))) { sdk_doc_free(doc); return -1; }
    doc->root->line = x.line;
    if (!parse_group(&x, doc->root)) {
        int bad = x.bad;
        sdk_doc_free(doc);
        return bad ? 0 : -1;
    }
    lx_space(&x);
    if (x.pos < len) {
        sdk_say(why, why_len, doc, x.line, "there is text after the file's last '}'; remove it.");
        sdk_doc_free(doc);
        return 0;
    }
    return 1;
}

int sdk_is(const sdk_atom *a, const char *w)
{
    size_t n = strlen(w);
    return a && a->kind == SDK_A_WORD && a->n == n && !_strnicmp(a->p, w, n);
}

void sdk_text(const sdk_atom *a, char *out, size_t cap)
{
    size_t n;
    if (!cap) return;
    if (!a) { out[0] = 0; return; }
    if (a->kind == SDK_A_GROUP) { snprintf(out, cap, "{ ... }"); return; }
    n = a->n < cap - 1 ? a->n : cap - 1;
    memcpy(out, a->p, n);
    out[n] = 0;
}

const sdk_item *sdk_find(const sdk_group *g, const char *key)
{
    size_t i;
    if (!g) return NULL;
    for (i = 0; i < g->count; i++)
        if (g->items[i].count && sdk_is(&g->items[i].atoms[0], key)) return &g->items[i];
    return NULL;
}

static unsigned edit_distance(const char *a, size_t an, const char *b, size_t bn)
{
    unsigned row[64], i, j;
    if (an >= 63 || bn >= 63) return 99;
    for (j = 0; j <= bn; j++) row[j] = j;
    for (i = 1; i <= an; i++) {
        unsigned prev = row[0], cur;
        row[0] = i;
        for (j = 1; j <= bn; j++) {
            unsigned sub = prev + (tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]));
            cur = row[j];
            row[j] = sub;
            if (row[j - 1] + 1 < row[j]) row[j] = row[j - 1] + 1;
            if (cur + 1 < row[j]) row[j] = cur + 1;
            prev = cur;
        }
    }
    return row[bn];
}

const char *sdk_nearest(const char *word, size_t n, const char *const *cands, size_t count)
{
    const char *best = NULL;
    unsigned bd = 99, limit = (unsigned)(n / 3 + 1), i;
    for (i = 0; i < count; i++) {
        unsigned d = edit_distance(word, n, cands[i], strlen(cands[i]));
        if (d < bd) { bd = d; best = cands[i]; }
    }
    return bd <= limit ? best : NULL;
}

int sdk_numbers(const sdk_atom *a, double *out, int max)
{
    size_t i;
    int n = 0;
    if (!a) return 0;
    if (a->kind == SDK_A_NUMBER) {
        if (max > 0) out[0] = a->num;
        return 1;
    }
    if (a->kind != SDK_A_GROUP) return 0;
    for (i = 0; i < a->group->count; i++) {
        const sdk_item *it = &a->group->items[i];
        if (it->count != 1 || it->atoms[0].kind != SDK_A_NUMBER) return 0;
        if (n < max) out[n] = it->atoms[0].num;
        n++;
    }
    return n <= max ? n : 0;
}

int sdk_first_type(const char *text, size_t len, char *kind, size_t cap)
{
    size_t i = 0, n = strlen(SDK_W(TYPE)), k = 0;
    if (cap) kind[0] = 0;
    for (;;) {
        while (i < len && isspace((unsigned char)text[i])) i++;
        if (i + 1 < len && text[i] == '/' && text[i + 1] == '/') { while (i < len && text[i] != '\n') i++; continue; }
        break;
    }
    if (i >= len || text[i] != '{') return 0;
    i++;
    for (;;) {
        while (i < len && isspace((unsigned char)text[i])) i++;
        if (i + 1 < len && text[i] == '/' && text[i + 1] == '/') { while (i < len && text[i] != '\n') i++; continue; }
        if (i + 1 < len && text[i] == '/' && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        break;
    }
    if (!(i + n < len && !_strnicmp(text + i, SDK_W(TYPE), n) && (text[i + n] == ' ' || text[i + n] == '\t'))) return 0;
    i += n;
    while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
    while (i < len && isalnum((unsigned char)text[i]) && k + 1 < cap) kind[k++] = (char)tolower((unsigned char)text[i++]);
    if (cap) kind[k] = 0;
    return 1;
}