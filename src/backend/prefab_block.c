/* prefab_block.c -- see prefab_block.h. */
#include <stdlib.h>
#include <string.h>

#include "prefab_block.h"

static const char PB_KEY[] = "\"" SH_PREFAB_BLOCK_KEY "\"";
#define PB_KEY_LEN (sizeof PB_KEY - 1)

static int pb_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Whitespace and comments from i; returns the next token's position (len at the end). */
static size_t pb_skip(const char *s, size_t len, size_t i)
{
    while (i < len) {
        if (pb_ws(s[i])) { i++; continue; }
        if (s[i] == '/' && i + 1 < len && s[i + 1] == '*') {
            size_t k = i + 2;
            while (k + 1 < len && !(s[k] == '*' && s[k + 1] == '/')) k++;
            if (k + 1 >= len) return len;
            i = k + 2;
            continue;
        }
        if (s[i] == '/' && i + 1 < len && s[i + 1] == '/') {
            while (i < len && s[i] != '\n') i++;
            continue;
        }
        break;
    }
    return i;
}

/* A string starting at s[i] == '"': the position after its closing quote, or 0. */
static size_t pb_string_end(const char *s, size_t len, size_t i)
{
    for (i++; i < len; i++) {
        if (s[i] == '\\') { i++; continue; }
        if (s[i] == '"') return i + 1;
    }
    return 0;
}

/* One value starting at s[i]: the position after it, or 0 when it does not end. */
static size_t pb_value_end(const char *s, size_t len, size_t i)
{
    int depth = 0;
    if (i >= len) return 0;
    if (s[i] == '"') return pb_string_end(s, len, i);
    if (s[i] != '{' && s[i] != '[') {
        while (i < len && !pb_ws(s[i]) && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != '/') i++;
        return i;
    }
    while (i < len) {
        char c = s[i];
        if (c == '"') { size_t e = pb_string_end(s, len, i); if (!e) return 0; i = e; continue; }
        if (c == '/' && i + 1 < len && (s[i + 1] == '*' || s[i + 1] == '/')) { i = pb_skip(s, len, i); continue; }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') { if (--depth == 0) return i + 1; }
        i++;
    }
    return 0;
}

typedef struct pb_scan {
    int found;
    sh_prefab_block_span span;
    size_t members;
    size_t last_value_end;     /* after the last member that is not the block, else after '{' */
    size_t close;              /* the top-level '}' */
    size_t type_at, type_len;  /* the "~type" string's body */
    int has_type;
} pb_scan;

/* Walk the top-level members. 1 complete object, 0 not. */
static int pb_walk(const char *s, size_t len, pb_scan *sc)
{
    size_t i = 0, prev_sep = (size_t)-1;   /* the comma before the current member */
    memset(sc, 0, sizeof *sc);
    if (!s) return 0;
    if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) i = 3;
    i = pb_skip(s, len, i);
    if (i >= len || s[i] != '{') return 0;
    i++;
    sc->last_value_end = i;
    for (;;) {
        size_t key_at, key_end, val_at, val_end;
        i = pb_skip(s, len, i);
        if (i >= len) return 0;
        if (s[i] == '}') { sc->close = i; return 1; }
        if (s[i] != '"') return 0;
        key_at = i;
        key_end = pb_string_end(s, len, i);
        if (!key_end) return 0;
        i = pb_skip(s, len, key_end);
        if (i >= len || s[i] != ':') return 0;
        val_at = pb_skip(s, len, i + 1);
        val_end = pb_value_end(s, len, val_at);
        if (!val_end || val_end == val_at) return 0;
        sc->members++;
        if (key_end - key_at == PB_KEY_LEN && !memcmp(s + key_at, PB_KEY, PB_KEY_LEN)) {
            sc->found = 1;
            sc->span.value_at = val_at;
            sc->span.value_len = val_end - val_at;
            if (prev_sep != (size_t)-1) {
                sc->span.cut_at = prev_sep;
                sc->span.cut_len = val_end - prev_sep;
            } else {
                /* The first member: it goes with the separator after it, if any. */
                size_t k = pb_skip(s, len, val_end);
                k = (k < len && s[k] == ',') ? pb_skip(s, len, k + 1) : val_end;
                sc->span.cut_at = key_at;
                sc->span.cut_len = k - key_at;
            }
        } else {
            sc->last_value_end = val_end;
            if (key_end - key_at == 7 && !memcmp(s + key_at, "\"~type\"", 7) && s[val_at] == '"') {
                sc->has_type = 1;
                sc->type_at = val_at + 1;
                sc->type_len = val_end - val_at - 2;
            }
        }
        i = pb_skip(s, len, val_end);
        if (i >= len) return 0;
        if (s[i] == ',') { prev_sep = i; i++; continue; }
        if (s[i] == '}') { sc->close = i; return 1; }
        return 0;
    }
}

int sh_prefab_block_find(const char *text, size_t len, sh_prefab_block_span *out)
{
    pb_scan sc;
    if (out) memset(out, 0, sizeof *out);
    if (!pb_walk(text, len, &sc)) return -1;
    if (!sc.found) return 0;
    if (out) *out = sc.span;
    return 1;
}

int sh_prefab_block_find_tail(const char *s, size_t len, sh_prefab_block_span *out)
{
    size_t at = len;
    if (out) memset(out, 0, sizeof *out);
    if (!s || len < PB_KEY_LEN) return 0;
    while (at-- > 0) {
        size_t p, v, e, k;
        if (at + PB_KEY_LEN > len || memcmp(s + at, PB_KEY, PB_KEY_LEN)) continue;
        if (at > 0 && s[at - 1] == '\\') continue;          /* inside a string */
        p = at;
        while (p > 0 && pb_ws(s[p - 1])) p--;
        if (p == 0 || (s[p - 1] != ',' && s[p - 1] != '{')) continue;
        k = pb_skip(s, len, at + PB_KEY_LEN);
        if (k >= len || s[k] != ':') continue;
        v = pb_skip(s, len, k + 1);
        if (v >= len || s[v] != '{') continue;
        e = pb_value_end(s, len, v);
        if (!e) continue;
        k = pb_skip(s, len, e);
        if (k >= len || s[k] != '}') continue;
        {
            /* Only whitespace or NULs may follow the top-level object. */
            size_t q = k + 1;
            while (q < len && (pb_ws(s[q]) || s[q] == 0)) q++;
            if (q < len) continue;
        }
        if (out) {
            out->value_at = v;
            out->value_len = e - v;
            if (s[p - 1] == ',') { out->cut_at = p - 1; out->cut_len = e - (p - 1); }
            else { out->cut_at = at; out->cut_len = e - at; }
        }
        return 1;
    }
    return 0;
}

size_t sh_prefab_block_strip(char *text, size_t len)
{
    pb_scan sc;
    size_t n;
    if (!text || !pb_walk(text, len, &sc) || !sc.found) return len;
    memmove(text + sc.span.cut_at, text + sc.span.cut_at + sc.span.cut_len, len - sc.span.cut_at - sc.span.cut_len);
    n = len - sc.span.cut_len;
    text[n] = 0;   /* room: n < len */
    return n;
}

char *sh_prefab_block_put(const char *text, size_t len, const char *block, size_t block_len, size_t *out_len)
{
    static const char SEP[] = ",\n" "\"" SH_PREFAB_BLOCK_KEY "\":";
    pb_scan sc;
    char *base, *out;
    size_t blen, at, head, need;
    if (out_len) *out_len = 0;
    if (!text || !block || !block_len || !pb_walk(text, len, &sc)) return NULL;
    base = (char *)malloc(len + 1);
    if (!base) return NULL;
    memcpy(base, text, len);
    base[len] = 0;
    blen = sh_prefab_block_strip(base, len);
    if (!pb_walk(base, blen, &sc)) { free(base); return NULL; }
    at = sc.last_value_end;
    /* The first member has no separator before it. */
    head = sc.members ? sizeof SEP - 1 : sizeof SEP - 3;
    need = blen + head + block_len;
    out = (char *)malloc(need + 1);
    if (!out) { free(base); return NULL; }
    memcpy(out, base, at);
    memcpy(out + at, sc.members ? SEP : SEP + 2, head);
    memcpy(out + at + head, block, block_len);
    memcpy(out + at + head + block_len, base + at, blen - at);
    out[need] = 0;
    free(base);
    if (out_len) *out_len = need;
    return out;
}

int sh_prefab_block_top_type(const char *text, size_t len, char *out, size_t cap)
{
    pb_scan sc;
    size_t n;
    if (out && cap) out[0] = 0;
    if (!pb_walk(text, len, &sc)) return -1;
    if (!sc.has_type) return 0;
    if (out && cap) {
        n = sc.type_len < cap - 1 ? sc.type_len : cap - 1;
        memcpy(out, text + sc.type_at, n);
        out[n] = 0;
    }
    return 1;
}
