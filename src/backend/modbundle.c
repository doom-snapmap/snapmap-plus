/* modbundle.c -- see modbundle.h. Mod descriptor, tree walk, hashes, logical
 * names, decl needs, validation and the canonical bundle manifest. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathW */

#include "modbundle.h"
#include "modsalt.h"
#include "strkeys.h"   /* string edits: keys the game or the product defines (DECISIONS 9) */
#include "companion.h"
#include "mp_json.h"
#include "plain_reason.h"
#include "config_json.h"
#include "decl_text.h"
#include "decl_tree.h"
#include "json_pretty.h"
#include "backend_log.h"
#include "sha256.h"
#include "script/script_node.h"   /* the Script node row: its reserved paths, which mods get it */
#include "store_lock.h"

#define MB_MODJSON_MAX (16u * 1024u * 1024u)   /* a mod.json read at build or load */

#define MB_JSON_DEPTH   32u
#define MB_WALK_DEPTH   48
#define MB_SCAN_DEPTH   128
#define MB_READ_CHUNK   (1u << 20)
/* With a stock reader, files over the caps or of a type a bundle cannot carry
 * are hashed before they are refused (a stock copy becomes a pointer), so the
 * walk itself stops at this many listed bytes. */
#define MB_MAX_WALK     (2048ull * 1024u * 1024u)

/* ------------------------------------------------------------ SHA-256 ---- */

void sh_mb_sha256_hex(const void *data, size_t len, char out[SH_MB_HASH_HEX + 1])
{
    sh_sha256 s;
    sh_sha256_init(&s);
    sh_sha256_update(&s, data, len);
    sh_sha256_hex(&s, out, SH_MB_HASH_HEX);
}

/* ------------------------------------------------------------- buffers ---- */

typedef struct mb_buf {
    char *p;
    size_t n, cap;
    int oom;
} mb_buf;

static void mb_put(mb_buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *grown;
        while (b->n + n + 1 > cap) cap *= 2;
        grown = (char *)realloc(b->p, cap);
        if (!grown) { b->oom = 1; return; }
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void mb_puts(mb_buf *b, const char *s) { mb_put(b, s, strlen(s)); }

/* A JSON string in the canonical spelling (JSON.stringify's). */
static void mb_put_jstr(mb_buf *b, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i, run = 0;
    mb_put(b, "\"", 1);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char u[7];
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\b') esc = "\\b";
        else if (c == '\f') esc = "\\f";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) {
            u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
            u[4] = hex[c >> 4]; u[5] = hex[c & 15]; u[6] = 0;
            esc = u;
        }
        if (esc) {
            if (run) mb_put(b, s + i - run, run);
            run = 0;
            mb_puts(b, esc);
        } else {
            run++;
        }
    }
    if (run) mb_put(b, s + n - run, run);
    mb_put(b, "\"", 1);
}

static void mb_put_jcstr(mb_buf *b, const char *s) { mb_put_jstr(b, s, strlen(s)); }

static void mb_put_u64(mb_buf *b, unsigned long long v)
{
    char t[32];
    _snprintf_s(t, sizeof t, _TRUNCATE, "%llu", v);
    mb_puts(b, t);
}

static char *mb_strndup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *mb_strdup(const char *s) { return s ? mb_strndup(s, strlen(s)) : NULL; }

static void mb_lower(char *s)
{
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static int mb_ieq_n(const char *a, size_t an, const char *b)
{
    size_t bn = strlen(b), i;
    if (an != bn) return 0;
    for (i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

static int mb_iends(const char *s, size_t n, const char *suffix)
{
    size_t m = strlen(suffix);
    return n >= m && mb_ieq_n(s + n - m, m, suffix);
}

static int mb_starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int mb_ends(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

/* --------------------------------------------------- wide path helpers ---- */

char *sh_mb_utf8(const wchar_t *wide)
{
    int n;
    char *out;
    if (!wide) return NULL;
    n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    out = (char *)malloc((size_t)n);
    if (!out) return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, n, NULL, NULL) != n) {
        free(out);
        return NULL;
    }
    return out;
}

wchar_t *sh_mb_wpath(const char *utf8)
{
    int n;
    wchar_t *raw, *full, *out;
    DWORD need;
    size_t len;
    if (!utf8 || !utf8[0]) return NULL;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    raw = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!raw) return NULL;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, raw, n);
    for (wchar_t *p = raw; *p; p++) if (*p == L'/') *p = L'\\';
    if (wcsncmp(raw, L"\\\\?\\", 4) == 0) return raw;
    need = GetFullPathNameW(raw, 0, NULL, NULL);
    if (!need) { free(raw); return NULL; }
    full = (wchar_t *)malloc(sizeof(wchar_t) * need);
    if (!full || !GetFullPathNameW(raw, need, full, NULL)) { free(raw); free(full); return NULL; }
    free(raw);
    len = wcslen(full);
    out = (wchar_t *)malloc(sizeof(wchar_t) * (len + 8));
    if (!out) { free(full); return NULL; }
    if (full[0] == L'\\' && full[1] == L'\\') {
        wcscpy_s(out, len + 8, L"\\\\?\\UNC\\");
        wcscat_s(out, len + 8, full + 2);
    } else {
        wcscpy_s(out, len + 8, L"\\\\?\\");
        wcscat_s(out, len + 8, full);
    }
    free(full);
    return out;
}

int sh_mb_user_root(char *out, size_t cap)
{
    wchar_t base[MAX_PATH];
    char *u;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) return 0;
    u = sh_mb_utf8(base);
    if (!u) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus", u);
    free(u);
    return out[0] != 0 && strlen(out) + 1 < cap;
}

static wchar_t *mb_wjoin(const wchar_t *dir, const wchar_t *name)
{
    size_t a = wcslen(dir), b = wcslen(name);
    wchar_t *p = (wchar_t *)malloc(sizeof(wchar_t) * (a + b + 2));
    if (!p) return NULL;
    memcpy(p, dir, sizeof(wchar_t) * a);
    p[a] = L'\\';
    memcpy(p + a + 1, name, sizeof(wchar_t) * (b + 1));
    return p;
}

static unsigned char *mb_read_wfile(const wchar_t *path, size_t max, size_t *out_len)
{
    HANDLE h;
    LARGE_INTEGER size;
    unsigned char *buf;
    DWORD got;
    size_t done = 0;
    if (out_len) *out_len = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || (unsigned long long)size.QuadPart > max) {
        CloseHandle(h);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)size.QuadPart + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    while (done < (size_t)size.QuadPart) {
        DWORD want = (DWORD)(((size_t)size.QuadPart - done) > MB_READ_CHUNK ? MB_READ_CHUNK
                                                                            : ((size_t)size.QuadPart - done));
        if (!ReadFile(h, buf + done, want, &got, NULL) || got == 0) break;
        done += got;
    }
    CloseHandle(h);
    if (done != (size_t)size.QuadPart) { free(buf); return NULL; }
    buf[done] = 0;
    if (out_len) *out_len = done;
    return buf;
}

unsigned char *sh_mb_read_file(const char *path, size_t max, size_t *out_len)
{
    wchar_t *w = sh_mb_wpath(path);
    unsigned char *r;
    if (out_len) *out_len = 0;
    if (!w) return NULL;
    r = mb_read_wfile(w, max, out_len);
    free(w);
    return r;
}

static int mb_hash_wfile(const wchar_t *path, unsigned long long *size, char out[SH_MB_HASH_HEX + 1])
{
    HANDLE h;
    unsigned char *buf;
    DWORD got;
    sh_sha256 s;
    unsigned long long total = 0;
    int ok = 1;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    buf = (unsigned char *)malloc(MB_READ_CHUNK);
    if (!buf) { CloseHandle(h); return 0; }
    sh_sha256_init(&s);
    for (;;) {
        if (!ReadFile(h, buf, MB_READ_CHUNK, &got, NULL)) { ok = 0; break; }
        if (got == 0) break;
        sh_sha256_update(&s, buf, got);
        total += got;
    }
    free(buf);
    CloseHandle(h);
    if (!ok) return 0;
    sh_sha256_hex(&s, out, SH_MB_HASH_HEX);
    if (size) *size = total;
    return 1;
}

int sh_mb_sha256_file(const char *path, unsigned long long *size, char out[SH_MB_HASH_HEX + 1])
{
    wchar_t *w = sh_mb_wpath(path);
    int ok;
    if (!w) return 0;
    ok = mb_hash_wfile(w, size, out);
    free(w);
    return ok;
}

/* ----------------------------------------------------------- JSON tree ---- */
/* Strings are decoded to UTF-8 bytes; numbers keep their text. Input is
 * validated strictly (config_json) before it reaches this parser. */

enum { MBJ_NULL, MBJ_BOOL, MBJ_NUM, MBJ_STR, MBJ_ARR, MBJ_OBJ };

typedef struct mbj {
    int t;
    int b;
    char *s;               /* STR bytes (NUL-terminated, may hold NUL) or NUM text */
    size_t n;
    struct mbj **items;
    char **keys;
    size_t *klen;
    size_t count, cap;
} mbj;

typedef struct mbj_parser {
    const char *p, *end;
    unsigned depth;
    int failed;
} mbj_parser;

static void mbj_free(mbj *v)
{
    size_t i;
    if (!v) return;
    for (i = 0; i < v->count; i++) {
        mbj_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->klen);
    free(v->s);
    free(v);
}

static void mbj_ws(mbj_parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r' || *ps->p == '\n')) ps->p++;
}

static int mbj_hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
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

static int mbj_string(mbj_parser *ps, char **out, size_t *out_len)
{
    mb_buf b = {0};
    if (ps->p >= ps->end || *ps->p != '"') return 0;
    ps->p++;
    while (ps->p < ps->end && *ps->p != '"') {
        if (*ps->p == '\\') {
            char c;
            if (ps->p + 1 >= ps->end) goto fail;
            c = ps->p[1];
            ps->p += 2;
            if (c == 'u') {
                unsigned cp, lo;
                char u[4];
                size_t k = 0;
                if (ps->end - ps->p < 4 || !mbj_hex4(ps->p, &cp)) goto fail;
                ps->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u' ||
                        !mbj_hex4(ps->p + 2, &lo) || lo < 0xDC00 || lo > 0xDFFF) goto fail;
                    ps->p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    goto fail;   /* a lone surrogate has no UTF-8 spelling */
                }
                if (cp < 0x80) u[k++] = (char)cp;
                else if (cp < 0x800) { u[k++] = (char)(0xC0 | (cp >> 6)); u[k++] = (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000) {
                    u[k++] = (char)(0xE0 | (cp >> 12)); u[k++] = (char)(0x80 | ((cp >> 6) & 63));
                    u[k++] = (char)(0x80 | (cp & 63));
                } else {
                    u[k++] = (char)(0xF0 | (cp >> 18)); u[k++] = (char)(0x80 | ((cp >> 12) & 63));
                    u[k++] = (char)(0x80 | ((cp >> 6) & 63)); u[k++] = (char)(0x80 | (cp & 63));
                }
                mb_put(&b, u, k);
            } else {
                char d;
                switch (c) {
                case '"': d = '"'; break;
                case '\\': d = '\\'; break;
                case '/': d = '/'; break;
                case 'b': d = '\b'; break;
                case 'f': d = '\f'; break;
                case 'n': d = '\n'; break;
                case 'r': d = '\r'; break;
                case 't': d = '\t'; break;
                default: goto fail;
                }
                mb_put(&b, &d, 1);
            }
        } else {
            const char *start = ps->p;
            while (ps->p < ps->end && *ps->p != '"' && *ps->p != '\\') ps->p++;
            mb_put(&b, start, (size_t)(ps->p - start));
        }
    }
    if (ps->p >= ps->end || b.oom) goto fail;
    ps->p++;
    if (!b.p) { b.p = (char *)calloc(1, 1); if (!b.p) return 0; }
    *out = b.p;
    *out_len = b.n;
    return 1;
fail:
    free(b.p);
    return 0;
}

static int mbj_push(mbj *v, mbj *item, char *key, size_t klen)
{
    if (v->count == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 8;
        mbj **items = (mbj **)realloc(v->items, sizeof(mbj *) * cap);
        if (!items) return 0;
        v->items = items;
        if (v->t == MBJ_OBJ) {
            char **keys = (char **)realloc(v->keys, sizeof(char *) * cap);
            size_t *kl;
            if (!keys) return 0;
            v->keys = keys;
            kl = (size_t *)realloc(v->klen, sizeof(size_t) * cap);
            if (!kl) return 0;
            v->klen = kl;
        }
        v->cap = cap;
    }
    v->items[v->count] = item;
    if (v->t == MBJ_OBJ) { v->keys[v->count] = key; v->klen[v->count] = klen; }
    v->count++;
    return 1;
}

static mbj *mbj_value(mbj_parser *ps)
{
    mbj *v;
    mbj_ws(ps);
    if (ps->p >= ps->end) return NULL;
    v = (mbj *)calloc(1, sizeof *v);
    if (!v) return NULL;
    switch (*ps->p) {
    case '{':
    case '[': {
        int obj = *ps->p == '{';
        char close = obj ? '}' : ']';
        v->t = obj ? MBJ_OBJ : MBJ_ARR;
        if (++ps->depth > MB_JSON_DEPTH) goto fail;
        ps->p++;
        mbj_ws(ps);
        if (ps->p < ps->end && *ps->p == close) { ps->p++; ps->depth--; return v; }
        for (;;) {
            char *key = NULL;
            size_t klen = 0;
            mbj *item;
            if (obj) {
                mbj_ws(ps);
                if (!mbj_string(ps, &key, &klen)) goto fail;
                mbj_ws(ps);
                if (ps->p >= ps->end || *ps->p != ':') { free(key); goto fail; }
                ps->p++;
            }
            item = mbj_value(ps);
            if (!item || !mbj_push(v, item, key, klen)) { mbj_free(item); free(key); goto fail; }
            mbj_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == close) { ps->p++; break; }
            goto fail;
        }
        ps->depth--;
        return v;
    }
    case '"':
        v->t = MBJ_STR;
        if (!mbj_string(ps, &v->s, &v->n)) goto fail;
        return v;
    case 't':
        if (ps->end - ps->p < 4 || memcmp(ps->p, "true", 4)) goto fail;
        v->t = MBJ_BOOL; v->b = 1; ps->p += 4;
        return v;
    case 'f':
        if (ps->end - ps->p < 5 || memcmp(ps->p, "false", 5)) goto fail;
        v->t = MBJ_BOOL; ps->p += 5;
        return v;
    case 'n':
        if (ps->end - ps->p < 4 || memcmp(ps->p, "null", 4)) goto fail;
        v->t = MBJ_NULL; ps->p += 4;
        return v;
    default: {
        const char *start = ps->p;
        while (ps->p < ps->end && (strchr("+-.eE", *ps->p) || (*ps->p >= '0' && *ps->p <= '9'))) ps->p++;
        if (ps->p == start) goto fail;
        v->t = MBJ_NUM;
        v->s = mb_strndup(start, (size_t)(ps->p - start));
        v->n = (size_t)(ps->p - start);
        if (!v->s) goto fail;
        return v;
    }
    }
fail:
    mbj_free(v);
    return NULL;
}

/* Strict validation (UTF-8, duplicate keys, syntax) then the tree. */
static mbj *mbj_parse(const char *text, size_t len, char *cause, size_t cap)
{
    sh_json_kind kind;
    sh_json_error err = {0};
    mbj_parser ps;
    mbj *v;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) { text += 3; len -= 3; }
    if (!sh_json_validate_ex(text, len, MB_JSON_DEPTH, &kind, &err)) {
        if (cause) _snprintf_s(cause, cap, _TRUNCATE, "invalid JSON at byte %zu: %s", err.offset,
                               err.reason ? err.reason : "syntax error");
        return NULL;
    }
    ps.p = text; ps.end = text + len; ps.depth = 0; ps.failed = 0;
    v = mbj_value(&ps);
    if (v) mbj_ws(&ps);
    if (!v || ps.p != ps.end) {
        mbj_free(v);
        if (cause) _snprintf_s(cause, cap, _TRUNCATE, "JSON could not be read (strings must be valid Unicode)");
        return NULL;
    }
    return v;
}

static mbj *mbj_get(const mbj *o, const char *key)
{
    size_t i, n = strlen(key);
    if (!o || o->t != MBJ_OBJ) return NULL;
    for (i = 0; i < o->count; i++)
        if (o->klen[i] == n && memcmp(o->keys[i], key, n) == 0) return o->items[i];
    return NULL;
}

/* A plain integer -?(0|[1-9][0-9]*) of at most 15 digits (exact in a double). */
static int mb_plain_integer(const char *s, size_t n)
{
    size_t i = 0, digits;
    if (i < n && s[i] == '-') i++;
    digits = n - i;
    if (digits == 0 || digits > 15) return 0;
    if (s[i] == '0' && digits != 1) return 0;
    for (; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* A plain integer followed by a point and 1 to 17 fraction digits, with no
 * exponent: the canonical spelling of a fixed decimal such as the owner-local
 * transforms in a companions block. The author's digits are kept verbatim so
 * the canonical text, and therefore the bundle hash, never depends on float
 * formatting. */
static int mb_plain_decimal(const char *s, size_t n)
{
    size_t dot = 0, frac;
    while (dot < n && s[dot] != '.') dot++;
    if (dot == 0 || dot >= n) return 0;
    if (!mb_plain_integer(s, dot)) return 0;
    frac = n - dot - 1;
    if (frac == 0 || frac > 17) return 0;
    for (dot++; dot < n; dot++) if (s[dot] < '0' || s[dot] > '9') return 0;
    return 1;
}

/* qsort_s comparator over member indices of the object passed as context. */
static int mb_key_cmp(void *ctx, const void *a, const void *b)
{
    const mbj *o = (const mbj *)ctx;
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    size_t nx = o->klen[x], ny = o->klen[y];
    int c = memcmp(o->keys[x], o->keys[y], nx < ny ? nx : ny);
    if (c) return c;
    return nx < ny ? -1 : nx > ny ? 1 : 0;
}

/* Canonical text of a value: sorted keys, no whitespace, integer or fixed-decimal numbers kept verbatim. */
static int mbj_canon(const mbj *v, mb_buf *b, char *cause, size_t cap)
{
    size_t i;
    switch (v->t) {
    case MBJ_NULL: mb_puts(b, "null"); return 1;
    case MBJ_BOOL: mb_puts(b, v->b ? "true" : "false"); return 1;
    case MBJ_STR: mb_put_jstr(b, v->s, v->n); return 1;
    case MBJ_NUM:
        if (!mb_plain_integer(v->s, v->n) && !mb_plain_decimal(v->s, v->n)) {
            if (cause) _snprintf_s(cause, cap, _TRUNCATE, "number %.32s is not a plain integer or fixed decimal", v->s);
            return 0;
        }
        mb_puts(b, strcmp(v->s, "-0") == 0 ? "0" : v->s);
        return 1;
    case MBJ_ARR:
        mb_put(b, "[", 1);
        for (i = 0; i < v->count; i++) {
            if (i) mb_put(b, ",", 1);
            if (!mbj_canon(v->items[i], b, cause, cap)) return 0;
        }
        mb_put(b, "]", 1);
        return 1;
    case MBJ_OBJ: {
        size_t *order = (size_t *)malloc(sizeof(size_t) * (v->count ? v->count : 1));
        if (!order) return 0;
        for (i = 0; i < v->count; i++) order[i] = i;
        qsort_s(order, v->count, sizeof(size_t), mb_key_cmp, (void *)v);
        mb_put(b, "{", 1);
        for (i = 0; i < v->count; i++) {
            if (i) mb_put(b, ",", 1);
            mb_put_jstr(b, v->keys[order[i]], v->klen[order[i]]);
            mb_put(b, ":", 1);
            if (!mbj_canon(v->items[order[i]], b, cause, cap)) { free(order); return 0; }
        }
        mb_put(b, "}", 1);
        free(order);
        return 1;
    }
    }
    return 0;
}

/* ------------------------------------------------- decl type spellings ---- */
/* generated/decls/<folder>/ to the engine's type name, from the 13954591
 * snap_gameresources.index and gameresources.index decl rows (278 folders).
 * Sorted by folder for bsearch. `renderparm` carries one renderProg row in the
 * archive; its type is renderParm. */
typedef struct mb_decl_type { const char *folder, *type; } mb_decl_type;
static const mb_decl_type MB_DECL_TYPES[] = {
    {"accolade","accolade"},{"actormodifier","actorModifier"},{"actorpopulation","actorPopulation"},{"advancedscreenviewshake","advancedScreenViewShake"},
    {"aibehavior","aiBehavior"},{"aibehaviorevents","aiBehaviorEvents"},{"aibehaviorvo","aiBehaviorVo"},{"aicomponent_attack","aiComponent_Attack"},
    {"aicomponent_cacodemon","aiComponent_Cacodemon"},{"aicomponent_cyberdemon","aiComponent_Cyberdemon"},{"aicomponent_extendedsense","aiComponent_ExtendedSense"},{"aicomponent_flight","aiComponent_Flight"},
    {"aicomponent_hellknight","aiComponent_Hellknight"},{"aicomponent_jetpack","aiComponent_Jetpack"},{"aicomponent_lasertargeter","aiComponent_LaserTargeter"},{"aicomponent_lostsoul","aiComponent_LostSoul"},
    {"aicomponent_missilebarrage","aiComponent_MissileBarrage"},{"aicomponent_oliviasguard","aiComponent_OliviasGuard"},{"aicomponent_pathmanager","aiComponent_PathManager"},{"aicomponent_positionawareness","aiComponent_PositionAwareness"},
    {"aicomponent_prototypes","aiComponent_Prototypes"},{"aicomponent_resurrector","aiComponent_Resurrector"},{"aicomponent_scythe","aiComponent_Scythe"},{"aicomponent_spidermastermind","aiComponent_SpiderMastermind"},
    {"aicomponent_strategy","aiComponent_Strategy"},{"aicomponent_transientfocus","aiComponent_TransientFocus"},{"aicomponent_waveblast","aiComponent_WaveBlast"},{"aievent","aiEvent"},
    {"aifsmmanager","aiFSMManager"},{"aiglobalsettings","aiGlobalSettings"},{"aimassist","aimAssist"},{"aimonstertypeinheritance","aiMonsterTypeInheritance"},
    {"aimovementgraph","aiMovementGraph"},{"aipaingraph","AIPainGraph"},{"aipoolnumbers","aiPoolNumbers"},{"aipositioningparms","aiPositioningParms"},
    {"aisensorysettings","aiSensorySettings"},{"aispawntimesettings","aiSpawnTimeSettings"},{"aithreatmanagement","aiThreatManagement"},{"aiturnparms","aiTurnParms"},
    {"aivignettegraph","aiVignetteGraph"},{"aivoiceover","aiVoiceOver"},{"ammo","ammo"},{"animweb","animWeb"},
    {"arcadescoring","arcadeScoring"},{"argui","arGui"},{"articulatedfigure","articulatedFigure"},{"attackgraph","attackGraph"},
    {"audiolog","audioLog"},{"audiologstory","audioLogStory"},{"berzerkattack","berzerkattack"},{"boostpack","boostPack"},
    {"botbehaviortreemanager","botBehaviorTreeManager"},{"botdef","botDef"},{"botskilldef","botSkillDef"},{"breakable","breakable"},
    {"cameratrigger","cameraTrigger"},{"challengelist","challengeList"},{"chargeattack","chargeattack"},{"cheatcode","cheatCode"},
    {"cloth","cloth"},{"codex","codex"},{"collectibles","collectibles"},{"combatencounterscoring","combatEncounterScoring"},
    {"commodities","commodities"},{"commodityconversion","commodityconversion"},{"commoditywallet","commodityWallet"},{"credits","credits"},
    {"damage","damage"},{"deathcamdef","deathCamDef"},{"decoyhologram","decoyHologram"},{"designsystem_turret_upgrades","designSystem_turret_upgrades"},
    {"designsystem_upgrades","designSystem_upgrades"},{"devinvloadout","devInvLoadout"},{"devmenuoption","devMenuOption"},{"dynamicchallenge","dynamicChallenge"},
    {"ebolt","ebolt"},{"entitydamage","entityDamage"},{"entitydef","entityDef"},{"entitydeffloatlist","entityDefFloatList"},
    {"env","env"},{"explosion","explosion"},{"faction","faction"},{"factiongraph","factionGraph"},
    {"fkgraph","FKGraph"},{"flare","flare"},{"focusattack","focusattack"},{"footstepevents","footstepEvents"},
    {"fx","fx"},{"gamedifficulty","gameDifficulty"},{"gameeventcallout","gameEventCallout"},{"gamefacts","gamefacts"},
    {"gamemode","gameMode"},{"gorebehavior","goreBehavior"},{"goregraph","goreGraph"},{"gorewounds","gorewounds"},
    {"hackmodule","hackModule"},{"handsbobcycle","handsBobCycle"},{"hotspot","hotSpot"},{"ignoremodeui","ignoreModeUI"},
    {"impactsounds","impactSounds"},{"interaction","interaction"},{"inventoryitem","inventoryItem"},{"jumpboots","jumpBoots"},
    {"layer","layer"},{"loadout","loadout"},{"loadoutarmor","loadoutArmor"},{"lootdrop","lootDrop"},
    {"mapeolstats","mapEOLStats"},{"mapinfo","mapInfo"},{"material","material"},{"md6def","md6Def"},
    {"metric","metric"},{"missionselectinfolist","missionSelectInfoList"},{"nineslice","nineSlice"},{"objective","objective"},
    {"onlinelevel","onlineLevel"},{"outlines","outlines"},{"particle","particle"},{"perkgroups","perkGroups"},
    {"perks","perks"},{"playerprops","playerProps"},{"progressionmanager","progressionManager"},{"projectile","projectile"},
    {"projectileimpacteffect","projectileImpactEffect"},{"proparcademode","propArcadeMode"},{"propattribs","propAttribs"},{"propbecomedemon","propBecomeDemon"},
    {"propbecomedemonthink","propBecomeDemonThink"},{"propbillboard","propBillboard"},{"propbreakable","propBreakable"},{"propcoopammo","propCoopAmmo"},
    {"propcoopbackpack","propCoopBackpack"},{"propcooprotatebob","propCoopRotateBob"},{"propdamage","propDamage"},{"prophealth","propHealth"},
    {"propincomingdemon","propIncomingDemon"},{"propitem","propItem"},{"propmoveable","propMoveable"},{"propstatuseffect","propStatusEffect"},
    {"propthinkfreezetag","propThinkFreezeTag"},{"propthinksouldrop","propThinkSoulDrop"},{"propuniversalammo","propUniversalAmmo"},{"propusefreezetag","propUseFreezeTag"},
    {"propusesouldrop","propUseSoulDrop"},{"pvpmap","pvpMap"},{"renderparm","renderParm"},{"renderprog","renderProg"},
    {"renderprogflag","renderProgFlag"},{"researchdossierorder","researchDossierOrder"},{"researchunlockablegroup","researchUnlockableGroup"},{"rewardtable","rewardTable"},
    {"ribbon","ribbon"},{"rumble","rumble"},{"screenviewshake","screenViewShake"},{"securityunlock","securityUnlock"},
    {"shellmenu","shellMenu"},{"simplehealthcomponents","simpleHealthComponents"},{"skins","skins"},{"snapaiencounter","snapAIEncounter"},
    {"snapaiencountertable","snapAIEncounterTable"},{"snapaiscore","snapAIScore"},{"snapbackgrounddef","snapBackgroundDef"},{"snapcaplist","snapCapList"},
    {"snapcustomicon","snapCustomIcon"},{"snapcustomtileoptions","snapCustomTileOptions"},{"snapdroppable","snapDroppable"},{"snapeditorassetbank","snapEditorAssetBank"},
    {"snapeditorentitybank","snapEditorEntityBank"},{"snapeditorentitydef","snapEditorEntityDef"},{"snapeditorsettings","snapEditorSettings"},{"snapentitystateiconbank","snapEntityStateIconBank"},
    {"snaplayers","snapLayers"},{"snapmoduleinfo","snapModuleInfo"},{"snappalette","snapPalette"},{"snappalettegroup","snapPaletteGroup"},
    {"snappropertyinspector_angle","snapPropertyInspector_Angle"},{"snappropertyinspector_animwebpath","snapPropertyInspector_AnimWebPath"},{"snappropertyinspector_bitflag","snapPropertyInspector_BitFlag"},{"snappropertyinspector_blockingvolumerendermodel","snapPropertyInspector_BlockingVolumeRenderModel"},
    {"snappropertyinspector_bool","snapPropertyInspector_Bool"},{"snappropertyinspector_cachedentity","snapPropertyInspector_CachedEntity"},{"snappropertyinspector_capdecl","snapPropertyInspector_CapDecl"},{"snappropertyinspector_color","snapPropertyInspector_Color"},
    {"snappropertyinspector_colorswatch","snapPropertyInspector_ColorSwatch"},{"snappropertyinspector_decal","snapPropertyInspector_Decal"},{"snappropertyinspector_doorproperties","snapPropertyInspector_DoorProperties"},{"snappropertyinspector_droppabledecl","snapPropertyInspector_DroppableDecl"},
    {"snappropertyinspector_echoproperty","snapPropertyInspector_EchoProperty"},{"snappropertyinspector_enablebobandrotate","snapPropertyInspector_EnableBobAndRotate"},{"snappropertyinspector_encounterdecl","snapPropertyInspector_EncounterDecl"},{"snappropertyinspector_encounterdecllist","snapPropertyInspector_EncounterDeclList"},
    {"snappropertyinspector_entitypicker","snapPropertyInspector_EntityPicker"},{"snappropertyinspector_enum","snapPropertyInspector_Enum"},{"snappropertyinspector_enum_powercorestation","snapPropertyInspector_Enum_PowerCoreStation"},{"snappropertyinspector_environment","snapPropertyInspector_Environment"},
    {"snappropertyinspector_float","snapPropertyInspector_Float"},{"snappropertyinspector_gamecalloutdecl","snapPropertyInspector_GameCalloutDecl"},{"snappropertyinspector_guiscale","snapPropertyInspector_GuiScale"},{"snappropertyinspector_hideatstart","snapPropertyInspector_HideAtStart"},
    {"snappropertyinspector_hudinfo","snapPropertyInspector_HudInfo"},{"snappropertyinspector_icon","snapPropertyInspector_Icon"},{"snappropertyinspector_int","snapPropertyInspector_Int"},{"snappropertyinspector_inventorydecl","snapPropertyInspector_InventoryDecl"},
    {"snappropertyinspector_lightcolor","snapPropertyInspector_LightColor"},{"snappropertyinspector_lightintensity","snapPropertyInspector_LightIntensity"},{"snappropertyinspector_loadoutweapons","snapPropertyInspector_LoadoutWeapons"},{"snappropertyinspector_material","snapPropertyInspector_Material"},
    {"snappropertyinspector_musicdecl","snapPropertyInspector_MusicDecl"},{"snappropertyinspector_percentage","snapPropertyInspector_Percentage"},{"snappropertyinspector_playermodifier","snapPropertyInspector_PlayerModifier"},{"snappropertyinspector_playerresource","snapPropertyInspector_PlayerResource"},
    {"snappropertyinspector_propfx","snapPropertyInspector_PropFX"},{"snappropertyinspector_publishedmap","snapPropertyInspector_PublishedMap"},{"snappropertyinspector_radius","snapPropertyInspector_Radius"},{"snappropertyinspector_range_float","snapPropertyInspector_Range_Float"},
    {"snappropertyinspector_range_int","snapPropertyInspector_Range_Int"},{"snappropertyinspector_range_time","snapPropertyInspector_Range_Time"},{"snappropertyinspector_rendermodel","snapPropertyInspector_RenderModel"},{"snappropertyinspector_rotation","snapPropertyInspector_Rotation"},
    {"snappropertyinspector_spawnerpair","snapPropertyInspector_SpawnerPair"},{"snappropertyinspector_speakerdecl","snapPropertyInspector_SpeakerDecl"},{"snappropertyinspector_statuseffectdecl","snapPropertyInspector_StatusEffectDecl"},{"snappropertyinspector_stringbuilder","snapPropertyInspector_StringBuilder"},
    {"snappropertyinspector_stringparamlist","snapPropertyInspector_StringParamList"},{"snappropertyinspector_text","snapPropertyInspector_Text"},{"snappropertyinspector_time","snapPropertyInspector_Time"},{"snappropertyinspector_vec2","snapPropertyInspector_Vec2"},
    {"snappropertyinspector_vec3","snapPropertyInspector_Vec3"},{"snappropertyinspector_vec4","snapPropertyInspector_Vec4"},{"snappropertyinspector_vodecl","snapPropertyInspector_VoDecl"},{"snappropertyinspector_weaponcomponentfromentity","snapPropertyInspector_WeaponComponentFromEntity"},
    {"snappropertyinspector_weaponfilter","snapPropertyInspector_WeaponFilter"},{"snappropertyinspector_whitelistencounterdecl","snapPropertyInspector_WhiteListEncounterDecl"},{"snappropertyinspector_whitelistencounterdecllist","snapPropertyInspector_WhiteListEncounterDeclList"},{"snaptileimage","snapTileImage"},
    {"snaptutorial","snapTutorial"},{"snaptutorialmaplist","snapTutorialMapList"},{"sound","sound"},{"spawninfluencer","spawnInfluencer"},
    {"staticimage","staticImage"},{"statuseffect","statusEffect"},{"summonweapon","summonweapon"},{"syncinteractions","syncInteractions"},
    {"table","table"},{"targeting","targeting"},{"throwable","throwable"},{"tooltip","tooltip"},
    {"trackingparms","trackingParms"},{"tutorialevent","tutorialEvent"},{"twitchpain","twitchPain"},{"uicallout","uicallout"},
    {"universaltraversaltable","universalTraversalTable"},{"unlock","unlock"},{"unlockable","unlockable"},{"unlockarmor","unlockArmor"},
    {"unlockinventory","unlockInventory"},{"unlockmodel","unlockModel"},{"unlocksnaptile","unlockSnapTile"},{"unlockvanity","unlockVanity"},
    {"upgrade","upgrade"},{"walkik","walkIK"},{"weapon","weapon"},{"weapondataarccannon","weaponDataArcCannon"},
    {"weapondatachaingun","weaponDataChainGun"},{"weapondatachainsaw","weaponDataChainsaw"},{"weapondatachainsawslice","weaponDataChainsawSlice"},{"weapondatachargeablepistol","weaponDataChargeablePistol"},
    {"weapondatachargeball","weaponDataChargeball"},{"weapondatadoublebarrelshotgun","weaponDataDoubleBarrelShotgun"},{"weapondatalightninggun","weaponDataLightningGun"},{"weapondatamancubusgland","weaponDataMancubusGland"},
    {"weapondataplasmarifle","weaponDataPlasmaRifle"},{"weapondatarailgun","weaponDataRailGun"},{"weapondatasalvocannon","weaponDataSalvoCannon"},{"weapondatastaticcannon","weaponDataStaticCannon"},
    {"weaponreticle","weaponReticle"},{"weaponreticleswfinfo","weaponReticleSWFInfo"}
};

static int mb_decl_type_cmp(const void *key, const void *row)
{
    return strcmp((const char *)key, ((const mb_decl_type *)row)->folder);
}

const char *sh_mb_decl_type(const char *folder, size_t folder_len)
{
    char key[64];
    const mb_decl_type *hit;
    size_t i;
    if (!folder || folder_len == 0 || folder_len >= sizeof key) return NULL;
    for (i = 0; i < folder_len; i++) {
        char c = folder[i];
        key[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    key[folder_len] = 0;
    hit = (const mb_decl_type *)bsearch(key, MB_DECL_TYPES, sizeof MB_DECL_TYPES / sizeof MB_DECL_TYPES[0],
                                        sizeof MB_DECL_TYPES[0], mb_decl_type_cmp);
    return hit ? hit->type : NULL;
}

/* ------------------------------------------------------ name rules -------- */

static int mb_path_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("._-$+@~() ", c) != NULL);
}

static int mb_cause(char *cause, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (cause && cap) {
        va_start(ap, fmt);
        _vsnprintf_s(cause, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

int sh_mb_check_request(const char *r, char *cause, size_t cap)
{
    size_t n, i, seg = 0;
    if (!r || !r[0]) return mb_cause(cause, cap, "empty path");
    n = strlen(r);
    if (n > SH_MB_REQUEST_MAX) return mb_cause(cause, cap, "path longer than %u bytes: %.80s...", SH_MB_REQUEST_MAX, r);
    if (r[0] == '/' || r[0] == '\\' || (n > 1 && r[1] == ':'))
        return mb_cause(cause, cap, "absolute path: %s", r);
    for (i = 0; i <= n; i++) {
        char c = r[i];
        if (c == '/' || c == 0) {
            size_t len = i - seg;
            if (len == 0) return mb_cause(cause, cap, "empty path segment: %s", r);
            if ((len == 1 && r[seg] == '.') || (len == 2 && r[seg] == '.' && r[seg + 1] == '.'))
                return mb_cause(cause, cap, "path traversal segment: %s", r);
            if (r[i - 1] == '.' || r[i - 1] == ' ')
                return mb_cause(cause, cap, "path segment ends with '.' or a space: %s", r);
            if (r[seg] == ' ') return mb_cause(cause, cap, "path segment starts with a space: %s", r);
            seg = i + 1;
            continue;
        }
        if (c == '\\') return mb_cause(cause, cap, "backslash in path: %s", r);
        if (!mb_path_char(c)) {
            /* The character itself (a UTF-8 sequence decoded), named in plain words. */
            const unsigned char *u = (const unsigned char *)r + i;
            unsigned long cp = u[0];
            char why[400];
            if (u[0] >= 0xc2 && u[0] < 0xe0 && (u[1] & 0xc0) == 0x80) cp = ((u[0] & 0x1fu) << 6) | (u[1] & 0x3fu);
            else if (u[0] >= 0xe0 && u[0] < 0xf0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80)
                cp = ((u[0] & 0x0fu) << 12) | ((u[1] & 0x3fu) << 6) | (u[2] & 0x3fu);
            else if (u[0] >= 0xf0 && u[0] < 0xf5 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80 && (u[3] & 0xc0) == 0x80)
                cp = ((u[0] & 0x07ul) << 18) | ((u[1] & 0x3ful) << 12) | ((u[2] & 0x3ful) << 6) | (u[3] & 0x3ful);
            return mb_cause(cause, cap, "%s", sh_reason(SH_REASON_NAME, (long)cp, r, 0, why, sizeof why));
        }
    }
    return 1;
}

int sh_mb_extension_allowed(const char *request)
{
    static const char *const allowed[] = { ".decl", ".bmd6model", ".bmd6skl", ".bmd6anim", ".bimage",
                                           ".bmodel", ".bcm", ".bnk", ".json", ".bin", ".vspv",
                                           ".fspv", ".cspv", ".mrpr", ".dmodel", NULL };
    const char *slash = strrchr(request, '/');
    const char *dot = strrchr(slash ? slash : request, '.');
    size_t i;
    if (!dot) return 0;
    for (i = 0; allowed[i]; i++) if (mb_ieq_n(dot, strlen(dot), allowed[i])) return 1;
    /* A cooked movie only where the engine asks for one (modswf.c serves it). */
    if (mb_ieq_n(dot, strlen(dot), ".bswf")) return _strnicmp(request, "generated/swf/", 14) == 0;
    /* A script only in the mod's scripts folder (sh_mb_script_request). */
    if (mb_ieq_n(dot, strlen(dot), ".lua")) return sh_mb_script_request(request, NULL, NULL) > 0;
    /* A virtual-texture family member only at a name the engine's loader builds. */
    if (mb_ieq_n(dot, strlen(dot), ".mega2")) return sh_mb_vt_member(request, NULL, 0, NULL);
    /* .baas_<class>, for example .baas_monster48 */
    if (strlen(dot) > 6 && mb_ieq_n(dot, 6, ".baas_")) {
        for (i = 6; dot[i]; i++)
            if (!((dot[i] >= 'a' && dot[i] <= 'z') || (dot[i] >= 'A' && dot[i] <= 'Z') ||
                  (dot[i] >= '0' && dot[i] <= '9') || dot[i] == '_')) return 0;
        return 1;
    }
    return 0;
}

int sh_mb_pointer_only_kind(const char *request)
{
    static const char *const kinds[] = { ".sbsp", ".ambientsh", ".tome", ".pages", NULL };
    const char *slash, *dot;
    size_t i;
    if (!request) return 0;
    slash = strrchr(request, '/');
    dot = strrchr(slash ? slash : request, '.');
    if (!dot) return 0;
    for (i = 0; kinds[i]; i++) if (mb_ieq_n(dot, strlen(dot), kinds[i])) return 1;
    return 0;
}

/* ------------------------------------------------------- mod scripts ---- */

static int mb_script_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int sh_mb_script_request(const char *request, const char **name, size_t *name_len)
{
    const char *p, *at, *slash;
    size_t n, i, len;
    int kind = SH_MB_SCRIPT_ENTRY;
    if (name) *name = NULL;
    if (name_len) *name_len = 0;
    n = request ? strlen(request) : 0;
    if (n < 4 || !mb_ieq_n(request + n - 4, 4, ".lua")) return 0;
    /* mods/<salt>/scripts/<name>.lua or mods/<salt>/scripts/lib/<name>.lua */
    if (_strnicmp(request, "mods/", 5) != 0) return SH_MB_SCRIPT_MISPLACED;
    slash = strchr(request + 5, '/');
    if (!slash || slash == request + 5 || _strnicmp(slash + 1, "scripts/", 8) != 0) return SH_MB_SCRIPT_MISPLACED;
    p = slash + 9;
    if (_strnicmp(p, "lib/", 4) == 0) { p += 4; kind = SH_MB_SCRIPT_LIB; }
    at = request + n - 4;
    len = (size_t)(at - p);
    if (!len) return SH_MB_SCRIPT_MISPLACED;
    for (i = 0; i < len; i++) if (!mb_script_name_char(p[i])) return SH_MB_SCRIPT_MISPLACED;
    if (name) *name = p;
    if (name_len) *name_len = len;
    return kind;
}

void sh_mb_script_file_name(const char *request, char *out, size_t cap)
{
    const char *name = NULL;
    size_t len = 0;
    int kind = sh_mb_script_request(request, &name, &len);
    if (!cap) return;
    if (kind > 0)
        _snprintf_s(out, cap, _TRUNCATE, "scripts\\%s%.*s.lua", kind == SH_MB_SCRIPT_LIB ? "lib\\" : "", (int)len, name);
    else
        _snprintf_s(out, cap, _TRUNCATE, "%s", request ? request : "");
}

static volatile sh_mb_script_check_fn g_mb_script_check;

void sh_mb_set_script_check(sh_mb_script_check_fn fn)
{
    g_mb_script_check = fn;
}

static sh_mb_extra_check_fn g_mb_extra_check;

void sh_mb_set_extra_check(sh_mb_extra_check_fn fn)
{
    g_mb_extra_check = fn;
}

int sh_mb_sliceable(const sh_mb_bundle *b)
{
    size_t i;
    if (!b) return 0;
    for (i = 0; i < b->file_count; i++)
        if (!b->files[i].stock && sh_mb_script_request(b->files[i].request, NULL, NULL) > 0) return 0;
    return 1;
}

/* The static pass's lexer: just enough of Lua 5.4's to step over comments,
 * strings and numbers and to see the registration calls. */
static int mbs_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int mbs_ident(char c) { return mbs_ident_start(c) || (c >= '0' && c <= '9'); }

/* At t[i] == '[': the level of a long bracket ([[ is 0, [==[ is 2), or -1. */
static int mbs_long_level(const char *t, size_t n, size_t i)
{
    size_t j = i + 1;
    int level = 0;
    while (j < n && t[j] == '=') { level++; j++; }
    return j < n && t[j] == '[' ? level : -1;
}

/* Past the long bracket opening at i (level `level`) and its closing. */
static size_t mbs_skip_long(const char *t, size_t n, size_t i, int level, int *line)
{
    size_t j = i + (size_t)level + 2;
    for (; j < n; j++) {
        if (t[j] == '\n') (*line)++;
        else if (t[j] == ']') {
            size_t k = j + 1;
            int e = 0;
            while (k < n && t[k] == '=') { e++; k++; }
            if (e == level && k < n && t[k] == ']') return k + 1;
        }
    }
    return n;
}

/* Past the short string opening at i; *escaped when it holds a backslash. */
static size_t mbs_skip_short(const char *t, size_t n, size_t i, int *line, int *escaped)
{
    char q = t[i];
    size_t j = i + 1;
    if (escaped) *escaped = 0;
    while (j < n && t[j] != q) {
        if (t[j] == '\n') { (*line)++; return j; }          /* unfinished: the compile check names it */
        if (t[j] == '\\') {
            if (escaped) *escaped = 1;
            if (j + 1 < n && t[j + 1] == '\n') (*line)++;
            j += 2;
            continue;
        }
        j++;
    }
    return j < n ? j + 1 : n;
}

/* Past whitespace and comments from i. */
static size_t mbs_skip_space(const char *t, size_t n, size_t i, int *line)
{
    while (i < n) {
        char c = t[i];
        if (c == '\n') { (*line)++; i++; }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') i++;
        else if (c == '-' && i + 1 < n && t[i + 1] == '-') {
            int level;
            i += 2;
            if (i < n && t[i] == '[' && (level = mbs_long_level(t, n, i)) >= 0) i = mbs_skip_long(t, n, i, level, line);
            else while (i < n && t[i] != '\n') i++;
        } else break;
    }
    return i;
}

/* A string literal at i: 1 with its contents (a short string without escapes,
 * or a long string), 0 for anything else. *end is past what was read. */
static int mbs_literal(const char *t, size_t n, size_t i, int *line, const char **s, size_t *len, size_t *end)
{
    int level, escaped = 0;
    *end = i;
    if (i >= n) return 0;
    if (t[i] == '"' || t[i] == '\'') {
        size_t j = mbs_skip_short(t, n, i, line, &escaped);
        *end = j;
        if (escaped || j < i + 2 || t[j - 1] != t[i]) return 0;
        *s = t + i + 1;
        *len = j - i - 2;
        return 1;
    }
    if (t[i] == '[' && (level = mbs_long_level(t, n, i)) >= 0) {
        size_t open = i + (size_t)level + 2, j = mbs_skip_long(t, n, i, level, line);
        *end = j;
        if (j < open + (size_t)level + 2) return 0;
        if (open < n && t[open] == '\n') open++;               /* Lua drops a first newline */
        *s = t + open;
        *len = j - (size_t)level - 2 - open;
        return 1;
    }
    return 0;
}

/* After the callee at i: "(" literal, or a bare string argument when
 * `bare_ok`. 1 literal, 0 a call with another argument, -1 not a call. */
static int mbs_call_arg(const char *t, size_t n, size_t i, int bare_ok, const char **s, size_t *len)
{
    int line = 0;
    size_t end;
    i = mbs_skip_space(t, n, i, &line);
    if (i >= n) return -1;
    if (t[i] == '(') {
        i = mbs_skip_space(t, n, i + 1, &line);
        if (!mbs_literal(t, n, i, &line, s, len, &end)) return 0;
        end = mbs_skip_space(t, n, end, &line);
        return end < n && (t[end] == ',' || t[end] == ')') ? 1 : 0;
    }
    if (bare_ok && (t[i] == '"' || t[i] == '\'' || t[i] == '[')) return mbs_literal(t, n, i, &line, s, len, &end) ? 1 : 0;
    return -1;
}

void sh_mb_script_scan(const char *t, size_t n, sh_mb_script_call_fn fn, void *context)
{
    size_t i = 0;
    int line = 1, field = 0;   /* field: the previous token was a single '.' or ':' */
    if (!t || !fn) return;
    if (n >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) i = 3;
    if (i + 1 < n && t[i] == '#') while (i < n && t[i] != '\n') i++;   /* a first line "#..." is skipped by Lua */
    while (i < n) {
        char c = t[i];
        size_t next = mbs_skip_space(t, n, i, &line);
        if (next != i) { i = next; continue; }
        if (c == '"' || c == '\'') { i = mbs_skip_short(t, n, i, &line, NULL); field = 0; continue; }
        if (c == '[') {
            int level = mbs_long_level(t, n, i);
            if (level >= 0) { i = mbs_skip_long(t, n, i, level, &line); field = 0; continue; }
        }
        if (c >= '0' && c <= '9') {
            i++;
            while (i < n && (mbs_ident(t[i]) || t[i] == '.' ||
                             ((t[i] == '+' || t[i] == '-') && strchr("eEpP", t[i - 1])))) i++;
            field = 0;
            continue;
        }
        if (mbs_ident_start(c)) {
            size_t s = i, wl;
            const char *name = NULL;
            size_t nlen = 0;
            while (i < n && mbs_ident(t[i])) i++;
            wl = i - s;
            if (!field) {
                if (wl == 2 && !memcmp(t + s, "on", 2)) {
                    int l2 = 0;
                    size_t j = mbs_skip_space(t, n, i, &l2), e;
                    if (j < n && t[j] == '.' && !(j + 1 < n && t[j + 1] == '.')) {
                        j = mbs_skip_space(t, n, j + 1, &l2);
                        e = j;
                        while (e < n && mbs_ident(t[e])) e++;
                        if (e > j && mbs_ident_start(t[j])) {
                            char ev[16];
                            int what = 0, got;
                            size_t el = e - j;
                            if (el < sizeof ev) {
                                memcpy(ev, t + j, el);
                                ev[el] = 0;
                                if (!strcmp(ev, "spawn") || !strcmp(ev, "think") || !strcmp(ev, "remove") ||
                                    !strcmp(ev, "damage") || !strcmp(ev, "death") || !strcmp(ev, "anim") ||
                                    !strcmp(ev, "pain") || !strcmp(ev, "projectile") || !strcmp(ev, "frame_event"))
                                    what = SH_MB_SCRIPT_BIND;
                                else if (!strcmp(ev, "input")) what = SH_MB_SCRIPT_INPUT;
                            }
                            if (what && (got = mbs_call_arg(t, n, e, 0, &name, &nlen)) >= 0)
                                fn(context, got ? what : what | SH_MB_SCRIPT_DYNAMIC, ev, got ? name : NULL,
                                   got ? nlen : 0, line);
                        }
                    }
                } else if (wl == 4 && !memcmp(t + s, "node", 4)) {
                    int got = mbs_call_arg(t, n, i, 0, &name, &nlen);
                    if (got >= 0)
                        fn(context, got ? SH_MB_SCRIPT_OUTPUT : SH_MB_SCRIPT_OUTPUT | SH_MB_SCRIPT_DYNAMIC, "",
                           got ? name : NULL, got ? nlen : 0, line);
                } else if (wl == 7 && !memcmp(t + s, "require", 7)) {
                    int got = mbs_call_arg(t, n, i, 1, &name, &nlen);
                    if (got >= 0)
                        fn(context, got ? SH_MB_SCRIPT_REQUIRE : SH_MB_SCRIPT_REQUIRE | SH_MB_SCRIPT_DYNAMIC, "",
                           got ? name : NULL, got ? nlen : 0, line);
                }
            }
            field = 0;
            continue;
        }
        if (c == '.') {
            if (i + 1 < n && t[i + 1] == '.') { i += 2; while (i < n && t[i] == '.') i++; field = 0; }
            else { i++; field = 1; }
            continue;
        }
        if (c == ':') {
            if (i + 1 < n && t[i + 1] == ':') { i += 2; field = 0; }
            else { i++; field = 1; }
            continue;
        }
        i++;
        field = 0;
    }
}

int sh_mb_stock_restore(const char *request, const char *source)
{
    size_t rn, sn;
    const char *slash;
    if (!request || !source || !request[0] || !source[0]) return 0;
    if (_stricmp(request, source) == 0) return 1;
    /* generated/decls/<type>/<source>.decl, for a source that is itself an
     * own-path decl file of a type this game ships. */
    if (_strnicmp(request, "generated/decls/", 16) != 0) return 0;
    slash = strchr(request + 16, '/');
    if (!slash || slash == request + 16 || !sh_mb_decl_type(request + 16, (size_t)(slash - (request + 16)))) return 0;
    rn = strlen(slash + 1);
    sn = strlen(source);
    return mb_iends(source, sn, ".decl") && _strnicmp(source, "generated/", 10) != 0 &&
           rn == sn + 5 && _strnicmp(slash + 1, source, sn) == 0 && _stricmp(slash + 1 + sn, ".decl") == 0;
}

/* maps/modules/<path>.decl outside maps/modules/palettes/: a snapModule decl. */
static int mb_is_module_request(const char *r)
{
    size_t n = r ? strlen(r) : 0;
    return n > 18 && _strnicmp(r, "maps/modules/", 13) == 0 && _strnicmp(r, "maps/modules/palettes/", 22) != 0 &&
           mb_iends(r, n, ".decl");
}

int sh_mb_module_info_request(const char *request, char *out, size_t cap)
{
    size_t n;
    if (!mb_is_module_request(request)) return 0;
    n = strlen(request);
    /* "maps/modules/" + <path> + ".decl" -> "generated/decls/snapmoduleinfo/" + <path> + ".decl" */
    if (31 + (n - 13) + 1 > cap) return 0;
    memcpy(out, "generated/decls/snapmoduleinfo/", 31);
    memcpy(out + 31, request + 13, n - 13 + 1);
    mb_lower(out);
    return 1;
}

/* The mega2 loader opens <path without extension>_sq1..16.mega2 for the path
 * SetSource is given, and modvt.c gives "virtualtextures/" + the family name +
 * ".pages", so a member's family is the request without "virtualtextures/"
 * and "_sq<N>.mega2". The family must sit in a folder below mods/ and must not
 * contain "_vmtr": a material whose landpagefile contains it binds the stock
 * family instead. */
int sh_mb_vt_member(const char *request, char *family, size_t cap, int *member)
{
    const char *leaf, *digits, *end;
    size_t n, fam_len, i;
    int num;
    if (!request || _strnicmp(request, "virtualtextures/mods/", 21) != 0) return 0;
    n = strlen(request);
    if (n < 28 || _stricmp(request + n - 6, ".mega2") != 0) return 0;
    end = request + n - 6;                                  /* the '.' of .mega2 */
    leaf = strrchr(request, '/');
    if (leaf <= request + 21) return 0;                     /* no <id>/ folder below mods/ */
    for (digits = end; digits > leaf + 1 && digits[-1] >= '0' && digits[-1] <= '9'; --digits) {}
    if (end - digits < 1 || end - digits > 2 || digits[0] == '0') return 0;
    if (digits - 3 <= leaf + 1 || _strnicmp(digits - 3, "_sq", 3) != 0) return 0;
    num = atoi(digits);
    if (num < 1 || num > 16) return 0;
    fam_len = (size_t)(digits - 3 - (request + 16));        /* "mods/<id>/.../<family>" */
    for (i = 16; i + 5 <= 16 + fam_len; ++i)
        if (!_strnicmp(request + i, "_vmtr", 5)) return 0;
    if (family) {
        if (fam_len + 1 > cap) return 0;
        memcpy(family, request + 16, fam_len);
        family[fam_len] = 0;
    }
    if (member) *member = num;
    return 1;
}

static int mb_copy_out(char *dst, size_t cap, const char *src, size_t n)
{
    if (n + 1 > cap) return 0;
    memcpy(dst, src, n);
    dst[n] = 0;
    return 1;
}

/* ----------------------------------------------------- sound banks ---- */

uint32_t sh_mb_fnv1_lower(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        h = h * 16777619u;
        h ^= c;
    }
    return h;
}

int sh_mb_bank_stem(const char *request, char *stem, size_t cap)
{
    static const char dir[] = "sound/soundbanks/pc/";
    const char *leaf;
    size_t n;
    if (!request || _strnicmp(request, dir, sizeof dir - 1) != 0) return 0;
    leaf = request + sizeof dir - 1;
    n = strlen(leaf);
    if (n <= 4 || _stricmp(leaf + n - 4, ".bnk") != 0 || strchr(leaf, '/') || strchr(leaf, 0x5C)) return 0;
    if (stem) {
        if (n - 4 + 1 > cap) return 0;
        memcpy(stem, leaf, n - 4);
        stem[n - 4] = 0;
    }
    return 1;
}

int sh_mb_bank_rule(const char *request, const char *id, char *cause, size_t cap)
{
    char stem[SH_MB_BANK_STEM_MAX + 1], prefix[SH_MB_ID_MAX + 2], low[SH_MB_BANK_STEM_MAX + 1];
    size_t i, n, pn;
    if (!sh_mb_bank_stem(request, stem, sizeof stem))
        return mb_cause(cause, cap, "%s: a sound bank goes in sound/soundbanks/pc/ with a file name of at most %u "
                        "characters; move or rename it", request ? request : "", (unsigned)SH_MB_BANK_STEM_MAX);
    _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "%s_", id);
    pn = strlen(prefix);
    n = strlen(stem);
    for (i = 0; i < n; i++) {
        char c = stem[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return mb_cause(cause, cap, "%s: a sound bank's file name may hold only letters, digits and _; rename it",
                            request);
        low[i] = c;
    }
    low[n] = 0;
    if (n <= pn || strncmp(low, prefix, pn) != 0)
        return mb_cause(cause, cap, "%s: a sound bank's file name must start with this mod folder's name and _ (for "
                        "example <folder name>_music.bnk); rename it and press Refresh", request);
    /* The engine's listed-bank filter skips these in some game mode (doom_*_sp
     * in SnapMap, *_snapmaps outside it, *initial* and Init always), and the
     * product submits banks through that filter. */
    if (strstr(low, "_sp") || strstr(low, "_snapmaps") || strstr(low, "initial") || !strcmp(low, "init"))
        return mb_cause(cause, cap, "%s: the game skips a sound bank whose file name contains _sp, _snapmaps or "
                        "initial; rename it", request);
    return 1;
}

int sh_mb_bank_header_ok(const unsigned char *bytes, size_t len, const char *stem, char *cause, size_t cap)
{
    uint32_t size, version, id, want;
    if (!bytes || len < 16 || memcmp(bytes, "BKHD", 4) != 0)
        return mb_cause(cause, cap, "%s.bnk is not a Wwise sound bank; export it from Wwise again", stem);
    memcpy(&size, bytes + 4, 4);
    memcpy(&version, bytes + 8, 4);
    memcpy(&id, bytes + 12, 4);
    if (size < 8 || (size_t)size + 8 > len)
        return mb_cause(cause, cap, "%s.bnk is cut short; export it from Wwise again", stem);
    if (version != SH_MB_BANK_VERSION)
        return mb_cause(cause, cap, "%s.bnk is a Wwise bank of version %u, and this game reads only version %u; export "
                        "it with the Wwise version DOOM uses", stem, (unsigned)version, (unsigned)SH_MB_BANK_VERSION);
    want = sh_mb_fnv1_lower(stem, strlen(stem));
    if (id != want)
        return mb_cause(cause, cap, "%s.bnk carries bank id %u, but the game loads it by its name as id %u; press "
                        "Refresh and Snapmap+ corrects the id", stem, (unsigned)id, (unsigned)want);

    return 1;
}

/* The logical-name table, on a lower-cased request path. */
int sh_mb_logical_name(const char *r, char *type, size_t tcap, char *name, size_t ncap)
{
    size_t n = strlen(r);
    const char *cls = "file";
    if (mb_starts(r, "generated/decls/") && mb_ends(r, ".decl")) {
        const char *rest = r + 16, *slash = strchr(rest, '/');
        const char *spelled;
        size_t nlen;
        if (!slash) return 0;
        spelled = sh_mb_decl_type(rest, (size_t)(slash - rest));
        if (!spelled) return 0;
        nlen = (size_t)((r + n - 5) - (slash + 1));
        if (nlen == 0) return 0;
        return mb_copy_out(type, tcap, spelled, strlen(spelled)) &&
               mb_copy_out(name, ncap, slash + 1, nlen);
    }
    if (mb_starts(r, "generated/basemodel/") && mb_ends(r, ".bmd6model") && n > 30) {
        size_t s = n - 20 - 10;
        if (!mb_copy_out(type, tcap, "baseModel", 9) || s + 9 > ncap) return 0;
        memcpy(name, r + 20, s); memcpy(name + s, ".md6mesh", 9);
        return 1;
    }
    if (mb_starts(r, "generated/skeleton/") && mb_ends(r, ".bmd6skl") && n > 27) {
        size_t s = n - 19 - 8;
        if (!mb_copy_out(type, tcap, "skeleton", 8) || s + 8 > ncap) return 0;
        memcpy(name, r + 19, s); memcpy(name + s, ".md6skl", 8);
        return 1;
    }
    if (mb_starts(r, "cooked/anim/") && mb_ends(r, ".bmd6anim") && n > 21) {
        size_t s = n - 12 - 9;
        if (!mb_copy_out(type, tcap, "anim", 4) || s + 9 > ncap) return 0;
        memcpy(name, r + 12, s); memcpy(name + s, ".md6anim", 9);
        return 1;
    }
    if (mb_starts(r, "generated/image/") && mb_ends(r, ".bimage") && n > 23) {
        return mb_copy_out(type, tcap, "image", 5) && mb_copy_out(name, ncap, r + 16, n - 16 - 7);
    }
    /* generated/swf/<stem>.bswf is the movie a consumer names swf/<stem>.swf. */
    if (mb_starts(r, "generated/swf/") && mb_ends(r, ".bswf") && n > 19) {
        size_t s = n - 10 - 5;   /* "swf/<stem>" */
        if (!mb_copy_out(type, tcap, "swf", 3) || s + 5 > ncap) return 0;
        memcpy(name, r + 10, s); memcpy(name + s, ".swf", 5);
        return 1;
    }
    /* A SnapMap module decl is named by its own path, as a map's moduleName holds it. */
    if (mb_is_module_request(r)) return mb_copy_out(type, tcap, "snapModule", 10) && mb_copy_out(name, ncap, r, n);
    /* A permutation decl (generated/renderprogs/permutations/<program>_<mask>.decl)
     * is named by its request path: that is the string the permutation lookup
     * formats and passes to DeclFind (VK 0x141910010). It is decl text but not
     * a generated/decls/ decl, so it is never registered; it is served when
     * opened and checked with its program (mb_check_renderprogs). */
    if (sh_mb_bank_stem(r, NULL, 0)) cls = "soundBank";
    else if (sh_mb_script_request(r, NULL, NULL) > 0) cls = "script";
    else if (mb_ends(r, ".bmodel")) cls = "model";
    else if (mb_ends(r, ".bcm")) cls = "cm";
    else if (strstr(r, ".baas_") && !strchr(strstr(r, ".baas_"), '/')) cls = "aas";
    else if (mb_starts(r, "generated/renderprogs/") || mb_starts(r, "generated/spirv/") ||
             mb_ends(r, ".vspv") || mb_ends(r, ".fspv") || mb_ends(r, ".cspv") ||
             mb_ends(r, ".mrpr")) cls = "renderProg";
    else if (mb_starts(r, "generated/discreteanimation/") || mb_ends(r, ".dmodel")) cls = "discreteAnimation";
    else if (mb_starts(r, "generated/anim/")) cls = "anim";
    else if (mb_starts(r, "generated/decalatlas/")) cls = "decalatlas";
    else if (mb_starts(r, "generated/lightatlas/")) cls = "lightatlas";
    else if (mb_starts(r, "generated/transsortatlas/")) cls = "transsortatlas";
    return mb_copy_out(type, tcap, cls, strlen(cls)) && mb_copy_out(name, ncap, r, n);
}

/* The engine's binary name for an extension-differs load (R1 1i):
 * "<root>/<list type>/<stem>.<ext>", stem = name without extension, cut at '$',
 * then every character outside [A-Za-z0-9$./\\] becomes '_' (':' kept at
 * index 1). The product lower-cases what the hook sees. */
static int mb_binary_name(const char *root, const char *list, const char *name, const char *ext,
                          char *out, size_t cap)
{
    const char *slash = strrchr(name, '/');
    const char *dot = strrchr(slash ? slash : name, '.');
    const char *dollar = strchr(name, '$');
    size_t stem = dot ? (size_t)(dot - name) : strlen(name), i, w;
    char full[SH_MB_REQUEST_MAX + 64];
    if (dollar && (size_t)(dollar - name) < stem) stem = (size_t)(dollar - name);
    if (stem == 0) return 0;
    w = (size_t)_snprintf_s(full, sizeof full, _TRUNCATE, "%s/%s/%.*s.%s", root, list, (int)stem, name, ext);
    if (w == (size_t)-1 || w + 1 > cap) return 0;
    for (i = 0; i < w; i++) {
        char c = full[i];
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '$' || c == '.' || c == '/' || c == '\\' || (c == ':' && i == 1);
        c = keep ? c : '_';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[w] = 0;
    return 1;
}

static int mb_own_name(const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    if (n + 1 > cap) return 0;
    memcpy(out, name, n + 1);
    mb_lower(out);
    return 1;
}

int sh_mb_model_key(const char *name, char *out, size_t cap)
{
    size_t n;
    if (!name || !out || !cap) return 0;
    n = strlen(name);
    if (!n || strchr(name, '"') || strchr(name, ' ')) return 0;
    if (mb_iends(name, n, ".bmodel")) return mb_own_name(name, out, cap);
    if (!mb_iends(name, n, ".lwo") && !mb_iends(name, n, ".ase") && !mb_iends(name, n, ".ma") &&
        !mb_iends(name, n, ".obj") && !mb_iends(name, n, ".fbx"))
        return 0;
    return mb_binary_name("cooked", "model", name, "bmodel", out, cap);
}

int sh_mb_model_keys(const char *name, char out[][SH_MB_REQUEST_MAX + 1], int max)
{
    size_t n;
    int k = 0;
    if (!name || !out || max <= 0) return 0;
    n = strlen(name);
    if (!n || strchr(name, '"') || strchr(name, ' ')) return 0;
    if (mb_iends(name, n, ".bmodel")) return mb_own_name(name, out[0], SH_MB_REQUEST_MAX + 1);
    if (!mb_iends(name, n, ".lwo") && !mb_iends(name, n, ".ase") && !mb_iends(name, n, ".ma") &&
        !mb_iends(name, n, ".obj") && !mb_iends(name, n, ".fbx"))
        return 0;
    /* As sh_mb_need_requests resolves a model need: cooked/model/ for
     * authored models, generated/model/ for the md6 collision models the game
     * ships only there. */
    if (mb_binary_name("cooked", "model", name, "bmodel", out[k], SH_MB_REQUEST_MAX + 1)) k++;
    if (k < max && mb_binary_name("generated", "model", name, "bmodel", out[k], SH_MB_REQUEST_MAX + 1)) k++;
    return k;
}

int sh_mb_editor_time(const char *request)
{
    static const char *const prefixes[] = {
        "generated/decls/snapeditor", "generated/decls/snappropertyinspector", "generated/decls/snappalette",
        "generated/swf/snapmaps/snap_editor", "generated/swf/snapmaps/entity_menu", "generated/swf/snapmaps/list_view",
        NULL
    };
    int i;
    if (!request) return 0;
    for (i = 0; prefixes[i]; ++i) if (!_strnicmp(request, prefixes[i], strlen(prefixes[i]))) return 1;
    return 0;
}

int sh_mb_editor_only(const sh_mb_bundle *bundle)
{
    size_t i;
    if (!bundle || !bundle->file_count || bundle->requirements) return 0;
    for (i = 0; i < bundle->file_count; i++)
        if (!sh_mb_editor_time(bundle->files[i].request)) return 0;
    return 1;
}

int sh_mb_need_requests(const char *type, const char *name, char out[][SH_MB_REQUEST_MAX + 1], int max)
{
    size_t tl, nl;
    int k = 0;
    if (!type || !name || !name[0] || max <= 0) return 0;
    tl = strlen(type);
    nl = strlen(name);
    if (name[0] == '_') return mb_own_name(name, out[0], SH_MB_REQUEST_MAX + 1);
    if (mb_ieq_n(type, tl, "baseModel")) {
        if (mb_iends(name, nl, ".bmd6model")) return mb_own_name(name, out[0], SH_MB_REQUEST_MAX + 1);
        return mb_binary_name("generated", "baseModel", name, "bmd6model", out[0], SH_MB_REQUEST_MAX + 1);
    }
    if (mb_ieq_n(type, tl, "skeleton")) {
        if (mb_iends(name, nl, ".bmd6skl")) return mb_own_name(name, out[0], SH_MB_REQUEST_MAX + 1);
        return mb_binary_name("generated", "skeleton", name, "bmd6skl", out[0], SH_MB_REQUEST_MAX + 1);
    }
    if (mb_ieq_n(type, tl, "anim")) {
        if (mb_iends(name, nl, ".bmd6anim")) return mb_own_name(name, out[0], SH_MB_REQUEST_MAX + 1);
        /* cooked/ at load state >= 2, generated/ as the retry form (R1 1i). */
        if (mb_binary_name("cooked", "anim", name, "bmd6anim", out[k], SH_MB_REQUEST_MAX + 1)) k++;
        if (k < max && mb_binary_name("generated", "anim", name, "bmd6anim", out[k], SH_MB_REQUEST_MAX + 1)) k++;
        return k;
    }
    if (mb_ieq_n(type, tl, "image")) {
        if (mb_iends(name, nl, ".bimage")) return mb_own_name(name, out[0], SH_MB_REQUEST_MAX + 1);
        return mb_binary_name("generated", "image", name, "bimage", out[0], SH_MB_REQUEST_MAX + 1);
    }
    if (mb_ieq_n(type, tl, "model")) {
        if (mb_iends(name, nl, ".bmodel")) return mb_own_name(name, out[0], SH_MB_REQUEST_MAX + 1);
        /* cooked/model/ for authored models; the md6 collision models
         * (md6/.../collision/<x>.lwo, named by the death-collision entities)
         * ship only as generated/model/md6/... beside generated/cm/md6/...,
         * and 13954591 holds no cooked/model/md6/ row at all. */
        if (mb_binary_name("cooked", "model", name, "bmodel", out[k], SH_MB_REQUEST_MAX + 1)) k++;
        if (k < max && mb_binary_name("generated", "model", name, "bmodel", out[k], SH_MB_REQUEST_MAX + 1)) k++;
        return k;
    }
    if (mb_ieq_n(type, tl, "swf")) {
        /* A movie named swf/<stem>.swf is served as generated/swf/<stem>.bswf,
         * the inverse of sh_mb_logical_name (no character is replaced). */
        const char *slash = strrchr(name, '/'), *dot = strrchr(slash ? slash : name, '.');
        size_t stem = dot ? (size_t)(dot - name) : nl;
        int w;
        if (stem > 4 && mb_ieq_n(name, 4, "swf/")) { name += 4; stem -= 4; }
        w = _snprintf_s(out[0], SH_MB_REQUEST_MAX + 1, _TRUNCATE, "generated/swf/%.*s.bswf", (int)stem, name);
        if (w < 0) return 0;
        mb_lower(out[0]);
        return 1;
    }
    if (sh_mb_decl_type(type, tl)) {
        int w = _snprintf_s(out[0], SH_MB_REQUEST_MAX + 1, _TRUNCATE, "generated/decls/%s/%s.decl", type, name);
        if (w < 0) return 0;
        mb_lower(out[0]);
        return 1;
    }
    return 0;
}

int sh_mb_strip_key(const char *in, char *out, size_t cap)
{
    size_t n, i;
    if (!in || !out || !cap) return 0;
    out[0] = 0;
    n = strlen(in);
    for (i = 0; i + 5 < n; i++) {
        const char *seg, *end;
        size_t head, tail;
        if (i && in[i - 1] != '/') continue;
        if (_strnicmp(in + i, "mods/", 5) != 0) continue;
        seg = in + i + 5;
        end = strchr(seg, '/');
        if (!end || end == seg || !end[1]) continue;
        head = i;
        tail = strlen(end + 1);
        if (head + tail + 1 > cap) return 0;
        memcpy(out, in, head);
        memcpy(out + head, end + 1, tail + 1);
        mb_lower(out);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------- decl scanning ---- */
/* A lexical pass over decl text that records references; it is not a parser
 * and accepts anything the structural check admits. Tokens: quoted strings
 * (no escapes: the engine's decl lexer keeps backslashes), words, braces,
 * '=', ';', ',' and parenthesised groups (skipped). A statement is
 * `key [=] value`; a block is `key [value] [=] {`.
 *
 * Keys scanned (ASCII case-insensitive), value must be quoted unless noted:
 *   inherit            top level only, and only a "mods/..." parent (stock
 *                      parents are not needs) -> the declaring decl's own type
 *   mesh               md6Def            -> baseModel
 *   skeleton           md6Def            -> skeleton
 *   anim               md6Def, any depth -> anim (events and aliases blocks)
 *   modelInfos, models animWeb: each quoted ".md6" name in the block (the
 *                      engine's `"<md6>" "<alias>"` pairs, or a list)
 *                                        -> md6Def
 *   name               animWeb, any depth, a ".md6anim" value -> anim
 *   declPrt, declRibbon fx           -> particle, ribbon
 *   lightMtr, decalMtr fx            -> material
 *   material           particle, ribbon -> material
 *   any key, a ".swf" value  entityDef (guiData swfFile, guiFile) -> swf
 *   model, renderModelName  any decl     -> md6Def for ".md6", model for
 *                                           .lwo/.ase/.ma/.obj/.fbx/.bmodel
 *   md6Def             any decl          -> md6Def
 *   entityDef          any decl          -> entityDef
 *   item[N] inside a buildGameRefEntityDefs block -> entityDef
 *   previewImage       snapEditorEntityDef -> material
 *   *program           material (quoted or bare word) -> renderProg
 *   *map, *mask, editorimage, qer_editorimage  material -> image
 * Values that are empty or start with '_' (engine-internal names) are skipped.
 *   soundShader, sound any decl, only a "mods/..." name -> sound
 * Not scanned: animWebDecl, fxDecl, stock sounds and sound events, aiVoiceOver,
 * damage and projectile decls, inventory items, an effect's flares, models,
 * sounds and tables, skins, ".break" models, table references inside material
 * expressions, md6Def meshKits, and every other reference key. */

typedef struct mb_tok { int kind; const char *p; size_t n; } mb_tok;
enum { TK_END, TK_WORD, TK_STR, TK_LBRACE, TK_RBRACE, TK_EQ, TK_SEMI, TK_COMMA, TK_GROUP };

static mb_tok mb_next_tok(const unsigned char *t, size_t len, size_t *pos)
{
    mb_tok k = { TK_END, NULL, 0 };
    size_t i = *pos;
    for (;;) {
        while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '/') {
            while (i < len && t[i] != '\n') i++;
            continue;
        }
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i = i + 2 <= len ? i + 2 : len;
            continue;
        }
        break;
    }
    if (i >= len) { *pos = len; return k; }
    switch (t[i]) {
    case '"': {
        size_t s = ++i;
        while (i < len && t[i] != '"' && t[i] != '\n') i++;
        k.kind = TK_STR; k.p = (const char *)t + s; k.n = i - s;
        if (i < len && t[i] == '"') i++;
        break;
    }
    case '{': k.kind = TK_LBRACE; i++; break;
    case '}': k.kind = TK_RBRACE; i++; break;
    case '=': k.kind = TK_EQ; i++; break;
    case ';': k.kind = TK_SEMI; i++; break;
    case ',': k.kind = TK_COMMA; i++; break;
    case '(': {
        int depth = 0;
        while (i < len) {
            if (t[i] == '(') depth++;
            else if (t[i] == ')' && --depth == 0) { i++; break; }
            i++;
        }
        k.kind = TK_GROUP;
        break;
    }
    default: {
        size_t s = i;
        while (i < len && !strchr(" \t\r\n{}=;,\"()", t[i])) i++;
        if (i == s) i++;   /* a stray ')' */
        k.kind = TK_WORD; k.p = (const char *)t + s; k.n = i - s;
        break;
    }
    }
    *pos = i;
    return k;
}

typedef struct mb_scan {
    const char *decl_type;
    int is_md6, is_sedef, is_material, is_animweb, is_fx, is_emitter, is_entitydef;
    sh_mb_need_fn fn;
    void *ctx;
    const char *block[MB_SCAN_DEPTH];
    size_t block_n[MB_SCAN_DEPTH];
    int depth;
} mb_scan;

static void mb_scan_value(mb_scan *sc, const mb_tok *key, const mb_tok *val)
{
    const char *k = key->p, *v = val->p;
    size_t kn = key->n, vn = val->n;
    int quoted = val->kind == TK_STR;
    const char *parent = sc->depth > 0 && sc->depth <= MB_SCAN_DEPTH ? sc->block[sc->depth - 1] : NULL;
    size_t parent_n = sc->depth > 0 && sc->depth <= MB_SCAN_DEPTH ? sc->block_n[sc->depth - 1] : 0;
    if (sc->is_animweb && key->kind == TK_STR && parent &&
        (mb_ieq_n(parent, parent_n, "modelInfos") || mb_ieq_n(parent, parent_n, "models"))) {
        /* modelInfos { "<md6>" "<alias>" ... } pairs; models { "<md6>" ... } a list. */
        if (mb_iends(k, kn, ".md6")) sc->fn(sc->ctx, "md6Def", k, kn);
        if (quoted && vn && mb_ieq_n(parent, parent_n, "models") && mb_iends(v, vn, ".md6")) sc->fn(sc->ctx, "md6Def", v, vn);
        return;
    }
    if (vn == 0 || v[0] == '_') return;
    if (sc->is_material && mb_iends(k, kn, "program") && (quoted || val->kind == TK_WORD)) {
        sc->fn(sc->ctx, "renderProg", v, vn);
        return;
    }
    if (!quoted) return;
    if (sc->depth == 1 && mb_ieq_n(k, kn, "inherit")) {
        /* A mod parent orders registration; a stock parent is always present. */
        if (vn > 5 && mb_ieq_n(v, 5, "mods/")) sc->fn(sc->ctx, sc->decl_type, v, vn);
        return;
    }
    if (sc->is_animweb && mb_ieq_n(k, kn, "name") && mb_iends(v, vn, ".md6anim")) {
        sc->fn(sc->ctx, "anim", v, vn);
        return;
    }
    if (sc->is_fx) {
        /* An effect spawns its particles and ribbons by decl name. */
        if (mb_ieq_n(k, kn, "declPrt")) { sc->fn(sc->ctx, "particle", v, vn); return; }
        if (mb_ieq_n(k, kn, "declRibbon")) { sc->fn(sc->ctx, "ribbon", v, vn); return; }
        if (mb_ieq_n(k, kn, "lightMtr") || mb_ieq_n(k, kn, "decalMtr")) { sc->fn(sc->ctx, "material", v, vn); return; }
    }
    if (sc->is_emitter && mb_ieq_n(k, kn, "material")) { sc->fn(sc->ctx, "material", v, vn); return; }
    if (sc->is_entitydef && mb_iends(v, vn, ".swf")) { sc->fn(sc->ctx, "swf", v, vn); return; }
    if (sc->is_md6) {
        if (mb_ieq_n(k, kn, "mesh")) { sc->fn(sc->ctx, "baseModel", v, vn); return; }
        if (mb_ieq_n(k, kn, "skeleton")) { sc->fn(sc->ctx, "skeleton", v, vn); return; }
        if (mb_ieq_n(k, kn, "anim")) { sc->fn(sc->ctx, "anim", v, vn); return; }
    }
    if (mb_ieq_n(k, kn, "model") || mb_ieq_n(k, kn, "renderModelName")) {
        if (mb_iends(v, vn, ".md6")) sc->fn(sc->ctx, "md6Def", v, vn);
        else if (mb_iends(v, vn, ".lwo") || mb_iends(v, vn, ".ase") || mb_iends(v, vn, ".ma") ||
                 mb_iends(v, vn, ".obj") || mb_iends(v, vn, ".fbx") || mb_iends(v, vn, ".bmodel"))
            sc->fn(sc->ctx, "model", v, vn);
        return;
    }
    if (mb_ieq_n(k, kn, "md6Def")) { sc->fn(sc->ctx, "md6Def", v, vn); return; }
    /* A sound decl a mod provides: stock names are Wwise events registered
     * from the events file, not files, so only a mods/ name is a need. */
    if ((mb_ieq_n(k, kn, "soundShader") || mb_ieq_n(k, kn, "sound")) && vn > 5 && mb_ieq_n(v, 5, "mods/")) {
        sc->fn(sc->ctx, "sound", v, vn);
        return;
    }
    /* renderModelInfo customMaterial names the material the decl's model is
     * drawn with, so the plan registers it before the entityDef that names it. */
    if (mb_ieq_n(k, kn, "customMaterial")) { sc->fn(sc->ctx, "material", v, vn); return; }
    if (mb_ieq_n(k, kn, "entityDef")) { sc->fn(sc->ctx, "entityDef", v, vn); return; }
    if (parent && mb_ieq_n(parent, parent_n, "buildGameRefEntityDefs") && kn > 5 && mb_ieq_n(k, 5, "item[")) {
        sc->fn(sc->ctx, "entityDef", v, vn);
        return;
    }
    if (sc->is_sedef && mb_ieq_n(k, kn, "previewImage")) { sc->fn(sc->ctx, "material", v, vn); return; }
    if (sc->is_material && (mb_iends(k, kn, "map") || mb_iends(k, kn, "mask") ||
                            mb_ieq_n(k, kn, "editorimage") || mb_ieq_n(k, kn, "qer_editorimage"))) {
        sc->fn(sc->ctx, "image", v, vn);
    }
}

void sh_mb_scan_needs(const char *decl_type, const unsigned char *text, size_t len,
                      sh_mb_need_fn fn, void *ctx)
{
    mb_scan sc;
    mb_tok key = { TK_END, NULL, 0 }, t;
    size_t pos = 0;
    enum { EXPECT_KEY, AFTER_KEY, AFTER_VALUE } state = EXPECT_KEY;
    if (!decl_type || !text || !fn) return;
    memset(&sc, 0, sizeof sc);
    sc.decl_type = decl_type;
    sc.is_md6 = strcmp(decl_type, "md6Def") == 0;
    sc.is_sedef = strcmp(decl_type, "snapEditorEntityDef") == 0;
    sc.is_material = strcmp(decl_type, "material") == 0 || strcmp(decl_type, "material2") == 0;
    sc.is_animweb = strcmp(decl_type, "animWeb") == 0;
    sc.is_fx = strcmp(decl_type, "fx") == 0;
    sc.is_emitter = strcmp(decl_type, "particle") == 0 || strcmp(decl_type, "ribbon") == 0;
    sc.is_entitydef = strcmp(decl_type, "entityDef") == 0;
    sc.fn = fn;
    sc.ctx = ctx;
    for (;;) {
        t = mb_next_tok(text, len, &pos);
        if (t.kind == TK_END) break;
        switch (t.kind) {
        case TK_LBRACE:
            if (sc.depth < MB_SCAN_DEPTH) {
                int named = state != EXPECT_KEY && key.p != NULL;
                sc.block[sc.depth] = named ? key.p : "";
                sc.block_n[sc.depth] = named ? key.n : 0;
            }
            sc.depth++;
            state = EXPECT_KEY;
            key.p = NULL;
            break;
        case TK_RBRACE:
            if (state == AFTER_KEY && key.p) {
                mb_tok none = { TK_STR, key.p + key.n, 0 };
                mb_scan_value(&sc, &key, &none);
            }
            if (sc.depth > 0) sc.depth--;
            state = EXPECT_KEY;
            key.p = NULL;
            break;
        case TK_EQ:
            break;
        case TK_SEMI:
        case TK_COMMA:
            state = EXPECT_KEY;
            key.p = NULL;
            break;
        case TK_GROUP:
            if (state == AFTER_KEY) state = AFTER_VALUE;
            break;
        case TK_WORD:
        case TK_STR:
            if (state == AFTER_KEY && key.p) {
                mb_scan_value(&sc, &key, &t);
                state = AFTER_VALUE;
            } else {
                key = t;
                state = AFTER_KEY;
            }
            break;
        }
    }
}

/* A bare (unquoted) word naming a mod resource whose name holds '-'. The
 * engine's breakable and material lexers end a bare word at '-', so
 * `fx mods/<salt>/fx/x-y` reaches the game as two tokens and the decl falls
 * back to defaults. A quoted token reaches the decl lookup whole, so a decl
 * name is quoted; a raw file (an image, a model) may instead use '_', the
 * spelling the engine derives its path with. A salt itself never has '-'. */
int sh_mb_scan_bare_hyphen(const unsigned char *text, size_t len, sh_mb_bare_fn fn, void *ctx)
{
    size_t pos = 0, counted = 0;
    int line = 1, found = 0;
    if (!text) return 0;
    for (;;) {
        mb_tok t = mb_next_tok(text, len, &pos);
        size_t at;
        if (t.kind == TK_END) break;
        if (t.kind != TK_WORD || t.n <= 5 || !mb_ieq_n(t.p, 5, "mods/") || !memchr(t.p, '-', t.n)) continue;
        at = (size_t)((const unsigned char *)t.p - text);
        for (; counted < at; counted++) if (text[counted] == '\n') line++;
        found++;
        if (fn) fn(ctx, line, t.p, t.n);
    }
    return found;
}

/* ------------------------------------------------------ bundle helpers ---- */

static void mb_error(sh_mb_bundle *b, const char *fmt, ...)
{
    va_list ap;
    if (b->errors++ == 0) {
        va_start(ap, fmt);
        _vsnprintf_s(b->first_cause, sizeof b->first_cause, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
}

typedef struct mb_bare_ctx { sh_mb_bundle *b; const sh_mb_file *f; } mb_bare_ctx;

static void mb_bare_cb(void *context, int line, const char *name, size_t len)
{
    mb_bare_ctx *c = (mb_bare_ctx *)context;
    mb_error(c->b, "%s line %d: '%.*s' is written without quotes and contains '-', which the game splits at the "
                   "hyphen; quote it (a decl name), or for a raw file use '_'", c->f->path, line, (int)len, name);
}

static void mb_warn(sh_mb_bundle *b, const char *fmt, ...)
{
    char line[SH_MB_CAUSE_MAX];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (b->warnings++ == 0) strcpy_s(b->first_warning, sizeof b->first_warning, line);
    if (b->warning_list_count < SH_MB_WARNINGS_KEPT) {
        char **grown;
        if (!b->warning_list) {
            grown = (char **)calloc(SH_MB_WARNINGS_KEPT, sizeof(char *));
            if (!grown) return;
            b->warning_list = grown;
        }
        if ((b->warning_list[b->warning_list_count] = _strdup(line)) != NULL) b->warning_list_count++;
    }
}

static void mb_file_free(sh_mb_file *f)
{
    size_t i;
    free(f->path);
    free(f->request);
    free(f->ptype);
    free(f->pname);
    free(f->stock_source);
    free(f->edit);
    for (i = 0; i < f->need_count; i++) { free(f->needs[i].type); free(f->needs[i].name); }
    free(f->needs);
}

void sh_mb_free(sh_mb_bundle *b)
{
    size_t i;
    if (!b) return;
    for (i = 0; i < b->file_count; i++) mb_file_free(&b->files[i]);
    free(b->files);
    for (i = 0; i < b->shadow_count; i++) free(b->shadows[i]);
    free(b->shadows);
    free(b->name);
    free(b->author);
    free(b->homepage);
    free(b->requirements);
    free(b->extra);
    free(b->canonical);
    free(b->registries);
    free(b->scripts);
    free(b->edits);
    for (i = 0; i < b->note_count; i++) free(b->notes[i]);
    free(b->notes);
    for (i = 0; i < b->warning_list_count; i++) free(b->warning_list[i]);
    free(b->warning_list);
    memset(b, 0, sizeof *b);
}

/* One report line of the build (sh_mb_bundle.notes). */
static void mb_note(sh_mb_bundle *b, const char *fmt, ...)
{
    char line[SH_MB_CAUSE_MAX];
    va_list ap;
    char *copy;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (b->note_count == b->note_cap) {
        size_t cap = b->note_cap ? b->note_cap * 2 : 8;
        char **grown = (char **)realloc(b->notes, sizeof(char *) * cap);
        if (!grown) return;
        b->notes = grown;
        b->note_cap = cap;
    }
    copy = _strdup(line);
    if (copy) b->notes[b->note_count++] = copy;
}


void sh_mb_classes_text(const sh_mb_classes *c, char *out, size_t cap)
{
    if (!out || !cap) return;
    if (!c) { out[0] = 0; return; }
    _snprintf_s(out, cap, _TRUNCATE, "%u adds, %u borrows, %u edits, %u replaces, %u editor, %u surfaces", c->adds,
                c->borrows, c->edits, c->replaces, c->editor, c->surfaces);
}

/* A build on this thread accepts a binary at a game name: content the
 * product received (sh_mb_keep_replacements_here). */

static __declspec(thread) int g_mb_keep_here;
void sh_mb_keep_replacements_here(int on) { g_mb_keep_here = on ? 1 : 0; }

/* The engine's cooked spelling of a name (mb_binary_name): every character
 * outside [A-Za-z0-9$./\] becomes '_', lower-cased. */
static void mb_cooked_key(const char *in, char *out, size_t cap)
{
    size_t i;
    for (i = 0; in[i] && i + 1 < cap; i++) {
        char c = in[i];
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' || c == '.' ||
                   c == '/' || c == '\\';
        c = keep ? c : '_';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[i] = 0;
}

static int mb_name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.' || c == '$';
}

/* At t[i]: "mods/<f>" in any case, not inside a longer name on either side
 * ('/' or any character that cannot continue a name follows). The matched
 * length, or 0. */
static size_t mb_rekey_at(const char *t, size_t n, size_t i, const char *f)
{
    size_t fl = strlen(f), end = i + 5 + fl;
    if (!fl || end > n || _strnicmp(t + i, "mods/", 5) != 0 || _strnicmp(t + i + 5, f, fl) != 0) return 0;
    if (i > 0 && mb_name_char((unsigned char)t[i - 1])) return 0;
    if (end < n && t[end] != '/' && mb_name_char((unsigned char)t[end])) return 0;
    return 5 + fl;
}

size_t sh_mb_rekey_text(const char *t, size_t n, const char *folder, const char *key, char **out, size_t *out_len)
{
    char cooked[SH_MB_ID_MAX * 4 + 1];
    const char *forms[2];
    size_t i, count = 0, kl;
    int nf = 0, k;
    mb_buf o = {0};
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!t || !folder || !folder[0] || !key) return 0;
    kl = strlen(key);
    mb_cooked_key(folder, cooked, sizeof cooked);
    forms[nf++] = folder;
    if (_stricmp(cooked, folder) != 0) forms[nf++] = cooked;
    for (i = 0; i < n;) {
        size_t m = 0;
        for (k = 0; k < nf && !m; k++) m = mb_rekey_at(t, n, i, forms[k]);
        if (!m) {
            if (out) mb_put(&o, t + i, 1);
            i++;
            continue;
        }
        count++;
        if (out) { mb_put(&o, t + i, 5); mb_put(&o, key, kl); }
        i += m;
    }
    if (!out) return count;
    if (!count) { free(o.p); return 0; }
    mb_put(&o, "", 1);   /* the terminator, not counted in out_len */
    if (o.oom) { free(o.p); return (size_t)-1; }
    *out = o.p;
    if (out_len) *out_len = o.n - 1;
    return count;
}

int sh_mb_rekey_path(const char *rel, const char *folder, const char *key, char *out, size_t cap)
{
    char cooked[SH_MB_ID_MAX * 4 + 1];
    const char *seg = rel, *prev = NULL;
    size_t prev_len = 0, w = 0, fl, cl;
    int changed = 0;
    if (!rel || !folder || !folder[0] || !key || !out || !cap) return -1;
    mb_cooked_key(folder, cooked, sizeof cooked);
    fl = strlen(folder);
    cl = strlen(cooked);
    out[0] = 0;
    while (*seg) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        const char *put = seg;
        size_t plen = len;
        /* A folder segment right after a "mods" segment. */
        if (slash && prev && prev_len == 4 && _strnicmp(prev, "mods", 4) == 0 &&
            ((len == fl && _strnicmp(seg, folder, len) == 0) || (len == cl && _strnicmp(seg, cooked, len) == 0))) {
            put = key;
            plen = strlen(key);
            changed = 1;
        }
        if (w + plen + 2 > cap) return -1;
        memcpy(out + w, put, plen);
        w += plen;
        if (slash) out[w++] = '/';
        out[w] = 0;
        prev = seg;
        prev_len = len;
        seg = slash ? slash + 1 : seg + len;
    }
    return changed;
}

/* The retired author keys (ONE-SHAPE rule 6): the first one `root` holds at
 * its top level, or "hud.schema"; NULL when none. */
static const char *mb_retired_key(const mbj *root)
{
    static const char *const retired[] = { "id", "key", "shadows", "stock", "scope", "player", "extends", "replaces",
                                           "legacy", "composes", "schema", NULL };
    const mbj *hud;
    int k;
    if (!root || root->t != MBJ_OBJ) return NULL;
    for (k = 0; retired[k]; k++) if (mbj_get(root, retired[k])) return retired[k];
    hud = mbj_get(root, "hud");
    if (hud && hud->t == MBJ_OBJ && mbj_get(hud, "schema")) return "hud.schema";
    return NULL;
}

int sh_mb_descriptor_state_why(const char *folder, char *cause, size_t cap)
{
    wchar_t *root = folder ? sh_mb_wpath(folder) : NULL, *p;
    unsigned char *text = NULL;
    size_t len = 0;
    mbj *j;
    const mbj *block, *fmt, *salt;
    const char *retired;
    int state, pkg = 0;
    if (cause && cap) cause[0] = 0;
    if (!root) { mb_cause(cause, cap, "This mod folder could not be found; press Refresh"); return SH_MB_DESC_UNREADABLE; }
    p = mb_wjoin(root, L"mod.json");
    if (p) text = mb_read_wfile(p, MB_MODJSON_MAX, &len);
    free(p);
    if (!text) {
        p = mb_wjoin(root, L"package.json");
        pkg = p && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
        free(p);
    }
    free(root);
    if (!text && pkg) {
        mb_cause(cause, cap, SH_MB_EARLIER);
        return SH_MB_DESC_EARLIER;
    }
    if (!text) { mb_cause(cause, cap, "This mod folder's mod.json is missing or cannot be read; close any program holding it and press Refresh"); return SH_MB_DESC_UNREADABLE; }
    j = mbj_parse((const char *)text, len, NULL, 0);
    free(text);
    if (!j || j->t != MBJ_OBJ) {
        mbj_free(j);
        mb_cause(cause, cap, "mod.json is not valid JSON (one object, { ... }); correct it and press Refresh");
        return SH_MB_DESC_UNREADABLE;
    }
    block = mbj_get(j, SH_MB_BLOCK_KEY);
    fmt = block && block->t == MBJ_OBJ ? mbj_get(block, "format") : NULL;
    salt = block && block->t == MBJ_OBJ ? mbj_get(block, "salt") : NULL;
    retired = mb_retired_key(j);
    if (block && (block->t != MBJ_OBJ || !fmt || fmt->t != MBJ_NUM || strcmp(fmt->s, "2") != 0)) {
        mb_cause(cause, cap, SH_MB_EARLIER);
        state = SH_MB_DESC_EARLIER;
    } else if (retired) {
        (void)retired;
        mb_cause(cause, cap, SH_MB_EARLIER);
        state = SH_MB_DESC_RETIRED_KEY;
    } else if (block && (!salt || salt->t != MBJ_STR || !sh_salt_valid(salt->s, salt->n))) {
        mb_cause(cause, cap, "The part of mod.json that Snapmap+ writes (\"" SH_MB_BLOCK_KEY "\") is damaged, so this mod cannot be told apart; put back mod.json from a backup, or make the mod again with New mod");
        state = SH_MB_DESC_UNREADABLE;
    } else {
        state = block ? SH_MB_DESC_BUILT : SH_MB_DESC_FRESH;
    }
    mbj_free(j);
    return state;
}

int sh_mb_descriptor_state(const char *folder)
{
    return sh_mb_descriptor_state_why(folder, NULL, 0);
}

static sh_mb_file *mb_add_file(sh_mb_bundle *b)
{
    if (b->file_count == b->file_cap) {
        size_t cap = b->file_cap ? b->file_cap * 2 : 32;
        sh_mb_file *grown = (sh_mb_file *)realloc(b->files, sizeof(sh_mb_file) * cap);
        if (!grown) return NULL;
        b->files = grown;
        b->file_cap = cap;
    }
    memset(&b->files[b->file_count], 0, sizeof(sh_mb_file));
    return &b->files[b->file_count++];
}

static int mb_add_need(sh_mb_file *f, const char *type, const char *name, size_t nlen)
{
    size_t i;
    for (i = 0; i < f->need_count; i++)
        if (strcmp(f->needs[i].type, type) == 0 && strlen(f->needs[i].name) == nlen &&
            memcmp(f->needs[i].name, name, nlen) == 0) return 1;
    if (f->need_count == f->need_cap) {
        size_t cap = f->need_cap ? f->need_cap * 2 : 8;
        sh_mb_need *grown = (sh_mb_need *)realloc(f->needs, sizeof(sh_mb_need) * cap);
        if (!grown) return 0;
        f->needs = grown;
        f->need_cap = cap;
    }
    f->needs[f->need_count].type = mb_strdup(type);
    f->needs[f->need_count].name = mb_strndup(name, nlen);
    if (!f->needs[f->need_count].type || !f->needs[f->need_count].name) {
        free(f->needs[f->need_count].type);
        free(f->needs[f->need_count].name);
        return 0;
    }
    f->need_count++;
    return 1;
}

static int mb_need_cmp(const void *a, const void *b)
{
    const sh_mb_need *x = (const sh_mb_need *)a, *y = (const sh_mb_need *)b;
    int c = strcmp(x->type, y->type);
    return c ? c : strcmp(x->name, y->name);
}

/* A tile's declared companions (the descriptor's "companions" block, kept in
 * extra) are needs of the tile's own entityDef: the serving closure then
 * registers them with the owner and the validator checks that they resolve. */
static void mb_companion_needs(sh_mb_bundle *b)
{
    sh_companion_table t;
    size_t i;
    int k;
    if (!b->extra) return;
    memset(&t, 0, sizeof t);
    if (sh_companion_add_bundle(&t, b->id, b->extra, strlen(b->extra), NULL, NULL) > 0) {
        for (i = 0; i < b->file_count; i++) {
            sh_mb_file *f = &b->files[i];
            const sh_companion_owner *owner;
            if (!f->is_decl || !f->ptype || !f->pname || _stricmp(f->ptype, "entityDef") != 0) continue;
            owner = sh_companion_find(&t, f->pname);
            if (!owner) continue;
            for (k = 0; k < owner->count; k++)
                if (!mb_add_need(f, "entityDef", owner->entries[k].entitydef, strlen(owner->entries[k].entitydef)))
                    mb_error(b, "out of memory");
            qsort(f->needs, f->need_count, sizeof(sh_mb_need), mb_need_cmp);
        }
    }
    sh_companion_table_free(&t);
}

/* What a scene node's "scenes" entry spawns (modscene_decl.h, kept in extra)
 * is a need of the node's own entityDef, like a companion: the serving closure
 * registers it with the node and the validator checks that it resolves. The
 * block's grammar is the registered extra check's (sh_mb_set_extra_check);
 * here only well-formed spawn names count. */

static void mb_scene_needs(sh_mb_bundle *b)
{
    mpj *root, *block;
    size_t i;
    int k, j;
    if (!b->extra) return;
    root = mpj_parse(b->extra, strlen(b->extra));
    block = (root && root->type == MPJ_OBJ) ? mpj_get(root, "scenes") : NULL;
    if (block && block->type == MPJ_OBJ) {
        for (i = 0; i < b->file_count; i++) {
            sh_mb_file *f = &b->files[i];
            int added = 0;
            if (!f->is_decl || !f->ptype || !f->pname || _stricmp(f->ptype, "entityDef") != 0) continue;
            for (k = 0; k < block->n; k++) {
                const mpj *spawn;
                if (_stricmp(block->keys[k], f->pname) != 0 || !block->items[k] || block->items[k]->type != MPJ_OBJ)
                    continue;
                spawn = mpj_get(block->items[k], "spawn");
                if (!spawn || spawn->type != MPJ_ARR) continue;
                for (j = 0; j < spawn->n; j++) {
                    const mpj *def = (spawn->items[j] && spawn->items[j]->type == MPJ_OBJ)
                                         ? mpj_get(spawn->items[j], "entityDef") : NULL;
                    if (!def || def->type != MPJ_STR || !def->text[0]) continue;
                    if (!mb_add_need(f, "entityDef", def->text, strlen(def->text))) mb_error(b, "out of memory");
                    added = 1;
                }
            }
            if (added) qsort(f->needs, f->need_count, sizeof(sh_mb_need), mb_need_cmp);
        }
    }
    mpj_free(root);
}

static int mb_file_cmp(const void *a, const void *b)
{
    return strcmp(((const sh_mb_file *)a)->path, ((const sh_mb_file *)b)->path);
}

static int mb_str_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

typedef struct mb_need_ctx { sh_mb_file *file; int oom; } mb_need_ctx;
static void mb_need_cb(void *ctx, const char *type, const char *name, size_t n)
{
    mb_need_ctx *c = (mb_need_ctx *)ctx;
    if (!mb_add_need(c->file, type, name, n)) c->oom = 1;
}

/* Count UTF-8 code points (input already validated as UTF-8). */
static size_t mb_codepoints(const char *s, size_t n)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) k++;
    return k;
}

static char *mb_trimmed_dup(const mbj *v)
{
    size_t a = 0, z = v->n;
    while (a < z && strchr(" \t\r\n", v->s[a])) a++;
    while (z > a && strchr(" \t\r\n", v->s[z - 1])) z--;
    return mb_strndup(v->s + a, z - a);
}

/* ----------------------------------------------------------- descriptor ---- */

/* requirements: {"cvars": {<name>: <value>}, "commands": {"activate": [...],
 * "deactivate": [...]}} -> canonical text. No cvar or command is named here:
 * a cvar is any engine identifier, a command any single console line, and the
 * player accepted them by installing the mod. The canonical text is
 *   {"commands":{"activate":[...],"deactivate":[...]},"cvars":{...}}
 * with "cvars" always present (sorted names, string values) and "commands",
 * and each of its lists, only when not empty (lines in the author's order), so
 * a bundle that carries only cvars keeps the text and hash it had when cvars
 * were the only key. */
static int mb_req_cvar_name(const char *s, size_t n)
{
    size_t i;
    if (n < 1 || n > SH_MB_REQ_NAME_MAX) return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return 0;
    return 1;
}

static int mb_req_printable(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return 0;
    return 1;
}

/* A cvar value as its canonical string: a string as written, an integer as
 * its digits, a boolean as "1" or "0". NULL with a cause otherwise. */
static const char *mb_req_value(const mbj *v, const char *name, char *num, size_t num_cap, size_t *len,
                                char *cause, size_t cap)
{
    if (v->t == MBJ_STR) {
        if (v->n > SH_MB_REQ_VALUE_MAX || strlen(v->s) != v->n || !mb_req_printable(v->s, v->n)) {
            mb_cause(cause, cap, "requirements.cvars.%.64s must be printable ASCII of at most %u characters", name,
                     SH_MB_REQ_VALUE_MAX);
            return NULL;
        }
        *len = v->n;
        return v->s;
    }
    if (v->t == MBJ_BOOL) { *len = 1; return v->b ? "1" : "0"; }
    if (v->t == MBJ_NUM && mb_plain_integer(v->s, v->n)) {
        strcpy_s(num, num_cap, strcmp(v->s, "-0") == 0 ? "0" : v->s);
        *len = strlen(num);
        return num;
    }
    mb_cause(cause, cap, "requirements.cvars.%.64s must be a string, an integer or a boolean (write a decimal as a string)",
             name);
    return NULL;
}

/* One commands list: an array of single console lines, trimmed of spaces. */
static int mb_req_lines(const mbj *arr, const char *which, mb_buf *out, char *cause, size_t cap)
{
    size_t i;
    if (arr->t != MBJ_ARR) { mb_cause(cause, cap, "requirements.commands.%s must be an array of console lines", which); return 0; }
    if (arr->count > SH_MB_REQ_COMMANDS_MAX) {
        mb_cause(cause, cap, "requirements.commands.%s has more than %u lines", which, SH_MB_REQ_COMMANDS_MAX);
        return 0;
    }
    for (i = 0; i < arr->count; i++) {
        const mbj *v = arr->items[i];
        size_t a = 0, z;
        if (v->t != MBJ_STR || strlen(v->s) != v->n) {
            mb_cause(cause, cap, "requirements.commands.%s[%zu] must be a string", which, i);
            return 0;
        }
        z = v->n;
        while (a < z && v->s[a] == ' ') a++;
        while (z > a && v->s[z - 1] == ' ') z--;
        if (a == z || z - a > SH_MB_REQ_LINE_MAX || !mb_req_printable(v->s + a, z - a) || memchr(v->s + a, ';', z - a)) {
            mb_cause(cause, cap, "requirements.commands.%s[%zu] must be one console command: printable ASCII without ';', "
                     "1 to %u characters", which, i, SH_MB_REQ_LINE_MAX);
            return 0;
        }
        if (out->n) mb_put(out, ",", 1);
        mb_put_jstr(out, v->s + a, z - a);
    }
    return 1;
}

static char *mb_requirements(const mbj *req, char *cause, size_t cap)
{
    const mbj *cvars, *commands;
    mb_buf out = {0}, act = {0}, deact = {0};
    size_t i, *order = NULL;
    if (req->t != MBJ_OBJ) { mb_cause(cause, cap, "requirements must be an object"); return NULL; }
    for (i = 0; i < req->count; i++)
        if (!(req->klen[i] == 5 && memcmp(req->keys[i], "cvars", 5) == 0) &&
            !(req->klen[i] == 8 && memcmp(req->keys[i], "commands", 8) == 0)) {
            mb_cause(cause, cap, "requirements key %.64s is not supported (cvars, commands)", req->keys[i]);
            return NULL;
        }
    cvars = mbj_get(req, "cvars");
    commands = mbj_get(req, "commands");
    if (cvars && cvars->t != MBJ_OBJ) { mb_cause(cause, cap, "requirements.cvars must be an object"); return NULL; }
    if (commands && commands->t != MBJ_OBJ) {
        mb_cause(cause, cap, "requirements.commands must be an object of activate and deactivate lists");
        return NULL;
    }
    if (cvars && cvars->count > SH_MB_REQ_CVARS_MAX) {
        mb_cause(cause, cap, "requirements.cvars names more than %u cvars", SH_MB_REQ_CVARS_MAX);
        return NULL;
    }
    for (i = 0; commands && i < commands->count; i++) {
        const char *k = commands->keys[i];
        size_t n = commands->klen[i];
        int is_act = n == 8 && memcmp(k, "activate", 8) == 0, is_deact = n == 10 && memcmp(k, "deactivate", 10) == 0;
        if (!is_act && !is_deact) {
            mb_cause(cause, cap, "requirements.commands key %.64s is not supported (activate, deactivate)", k);
            goto fail;
        }
        if (!mb_req_lines(commands->items[i], is_act ? "activate" : "deactivate", is_act ? &act : &deact, cause, cap))
            goto fail;
    }
    mb_puts(&out, "{");
    if (act.n || deact.n) {
        mb_puts(&out, "\"commands\":{");
        if (act.n) { mb_puts(&out, "\"activate\":["); mb_put(&out, act.p, act.n); mb_puts(&out, "]"); }
        if (act.n && deact.n) mb_put(&out, ",", 1);
        if (deact.n) { mb_puts(&out, "\"deactivate\":["); mb_put(&out, deact.p, deact.n); mb_puts(&out, "]"); }
        mb_puts(&out, "},");
    }
    mb_puts(&out, "\"cvars\":{");
    if (cvars && cvars->count) {
        order = (size_t *)malloc(sizeof(size_t) * cvars->count);
        if (!order) goto fail;
        for (i = 0; i < cvars->count; i++) order[i] = i;
        qsort_s(order, cvars->count, sizeof(size_t), mb_key_cmp, (void *)cvars);
        for (i = 0; i < cvars->count; i++) {
            const char *name = cvars->keys[order[i]];
            size_t n = cvars->klen[order[i]], len = 0, k;
            char num[32];
            const char *value;
            if (!mb_req_cvar_name(name, n) || strlen(name) != n) {
                mb_cause(cause, cap, "requirements.cvars key %.64s is not a cvar name (a letter or '_', then letters, "
                         "digits or '_', at most %u)", name, SH_MB_REQ_NAME_MAX);
                goto fail;
            }
            for (k = 0; k < i; k++)
                if (_stricmp(name, cvars->keys[order[k]]) == 0) {
                    mb_cause(cause, cap, "requirements.cvars names %.64s twice", name);
                    goto fail;
                }
            value = mb_req_value(cvars->items[order[i]], name, num, sizeof num, &len, cause, cap);
            if (!value) goto fail;
            if (i) mb_put(&out, ",", 1);
            mb_put_jstr(&out, name, n);
            mb_put(&out, ":", 1);
            mb_put_jstr(&out, value, len);
        }
    }
    mb_puts(&out, "}}");
    if (out.oom || act.oom || deact.oom) { mb_cause(cause, cap, "out of memory"); goto fail; }
    free(order);
    free(act.p);
    free(deact.p);
    return out.p;
fail:
    free(order);
    free(act.p);
    free(deact.p);
    free(out.p);
    return NULL;
}

char *sh_mb_requirements_canonical(const char *text, size_t len, char *cause, size_t cause_cap)
{
    mbj *root;
    char *out;
    if (cause && cause_cap) cause[0] = 0;
    if (!text) return NULL;
    root = mbj_parse(text, len, cause, cause_cap);
    if (!root) return NULL;
    out = mb_requirements(root, cause, cause_cap);
    mbj_free(root);
    return out;
}

/* The derived "shadows" a manifest or a built block records (already
 * lower-cased and sorted; the caller checks the order). */
static int mb_set_shadows(sh_mb_bundle *b, const mbj *arr)
{
    size_t i;
    char cause[SH_MB_CAUSE_MAX];
    if (arr->t != MBJ_ARR) { mb_error(b, "shadows must be an array of request paths"); return 0; }
    b->shadows = (char **)calloc(arr->count ? arr->count : 1, sizeof(char *));
    if (!b->shadows) { mb_error(b, "out of memory"); return 0; }
    for (i = 0; i < arr->count; i++) {
        const mbj *v = arr->items[i];
        char *s;
        if (v->t != MBJ_STR || strlen(v->s) != v->n) { mb_error(b, "shadows entries must be strings"); return 0; }
        s = mb_strdup(v->s);
        if (!s) { mb_error(b, "out of memory"); return 0; }
        if (mb_starts(s, "assets/")) {
            mb_error(b, "shadows entry %s must be a request path, without assets/", s);
            free(s);
            return 0;
        }
        if (!sh_mb_check_request(s, cause, sizeof cause)) {
            mb_error(b, "shadows entry: %s", cause);
            free(s);
            return 0;
        }
        b->shadows[b->shadow_count++] = s;
    }
    return 1;
}

/* ------------------------------------------------------ stock pointers ---- */

static sh_mb_stock_ops g_stock;
static volatile LONG g_stock_set;

void sh_mb_set_stock_ops(const sh_mb_stock_ops *ops)
{
    InterlockedExchange(&g_stock_set, 0);
    if (ops) memcpy(&g_stock, ops, sizeof g_stock);
    else memset(&g_stock, 0, sizeof g_stock);
    if (ops) InterlockedExchange(&g_stock_set, 1);
}

int sh_mb_stock_available(void)
{
    return InterlockedCompareExchange(&g_stock_set, 0, 0) != 0;
}

static const sh_mb_stock_ops *mb_stock(void)
{
    return sh_mb_stock_available() ? &g_stock : NULL;
}

static void mb_stock_cause(int r, const char *source, const char *path, char *cause, size_t cap)
{
    const char *who = path && path[0] ? path : source;
    if (!cause || !cap) return;
    if (r == SH_MB_STOCK_UNAVAILABLE)
        _snprintf_s(cause, cap, _TRUNCATE, "%s uses the game's file %s, and Snapmap+ could not read the game's files "
                    "to check it; start DOOM normally and press Refresh", who, source);
    else if (r == SH_MB_STOCK_MISMATCH)
        _snprintf_s(cause, cap, _TRUNCATE, "%s uses the game's file %s, which your copy of DOOM holds with other bytes "
                    "(another game version); verify the game's files in Steam", who, source);
    else
        _snprintf_s(cause, cap, _TRUNCATE, "%s uses the game's file %s, which your copy of DOOM does not have; verify "
                    "the game's files in Steam", who, source);
}

int sh_mb_stock_verify(const char *source, const char *sha256, unsigned long long *size,
                       const char *path, char *cause, size_t cap)
{
    const sh_mb_stock_ops *ops = mb_stock();
    unsigned long long got = 0;
    int r = ops && ops->verify ? ops->verify(ops->context, source, sha256, &got) : SH_MB_STOCK_UNAVAILABLE;
    if (size) *size = r == SH_MB_STOCK_OK ? got : 0;
    if (r != SH_MB_STOCK_OK) mb_stock_cause(r, source, path, cause, cap);
    return r;
}

int sh_mb_stock_identify(const char *source, char sha256[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    const sh_mb_stock_ops *ops = mb_stock();
    if (sha256) sha256[0] = 0;
    if (size) *size = 0;
    if (!source || !sha256 || !size) return SH_MB_STOCK_ABSENT;
    return ops && ops->identify ? ops->identify(ops->context, source, sha256, size) : SH_MB_STOCK_UNAVAILABLE;
}

int sh_mb_stock_read(const char *source, const char *sha256, unsigned char **bytes, size_t *len,
                     const char *path, char *cause, size_t cap)
{
    const sh_mb_stock_ops *ops = mb_stock();
    int r;
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (!bytes || !len) return SH_MB_STOCK_UNAVAILABLE;
    r = ops && ops->read ? ops->read(ops->context, source, sha256, bytes, len) : SH_MB_STOCK_UNAVAILABLE;
    if (r != SH_MB_STOCK_OK) {
        if (*bytes) HeapFree(GetProcessHeap(), 0, *bytes);
        *bytes = NULL;
        *len = 0;
        mb_stock_cause(r, source, path, cause, cap);
    }
    return r;
}

int sh_mb_stock_container(const char *source, const char *sha256, char *out, size_t cap)
{
    const sh_mb_stock_ops *ops = mb_stock();
    if (out && cap) out[0] = 0;
    if (!source || !out || !cap || !ops || !ops->container) return 0;
    if (ops->container(ops->context, source, sha256, out, cap) == SH_MB_STOCK_OK && out[0]) return 1;
    out[0] = 0;
    return 0;
}

int sh_mb_check_source(const char *source, char *cause, size_t cap)
{
    if (!source || !source[0]) return mb_cause(cause, cap, "the stock source is empty");
    return sh_mb_check_request(source, cause, cap);
}

/* Lower-case, '/' separators: the spelling a request or a source is kept in. */
static char *mb_path_dup(const char *s, size_t n)
{
    char *d = mb_strndup(s, n), *p;
    if (!d) return NULL;
    for (p = d; *p; p++) if (*p == '\\') *p = '/';
    mb_lower(d);
    return d;
}

/* The author's fields of mod.json: name, author, homepage, requirements. */
static int mb_common_fields(sh_mb_bundle *b, const mbj *root)
{
    const mbj *name = mbj_get(root, "name");
    const mbj *author = mbj_get(root, "author"), *home = mbj_get(root, "homepage");
    const mbj *req = mbj_get(root, "requirements");
    const char *what = "mod.json";
    char cause[SH_MB_CAUSE_MAX];

    if (!name || name->t != MBJ_STR || strlen(name->s) != name->n) {
        mb_error(b, "%s is missing its required name", what);
        return 0;
    }
    b->name = mb_trimmed_dup(name);
    if (!b->name || !b->name[0] || mb_codepoints(b->name, strlen(b->name)) > SH_MB_NAME_MAX) {
        mb_error(b, "%s name must be 1-%u characters", what, SH_MB_NAME_MAX);
        return 0;
    }
    if (author) {
        if (author->t != MBJ_STR || strlen(author->s) != author->n) { mb_error(b, "%s author must be a string", what); return 0; }
        b->author = mb_trimmed_dup(author);
        if (!b->author || mb_codepoints(b->author, strlen(b->author)) > SH_MB_AUTHOR_MAX) {
            mb_error(b, "%s author must be at most %u characters", what, SH_MB_AUTHOR_MAX);
            return 0;
        }
        if (!b->author[0]) { free(b->author); b->author = NULL; }
    }
    if (home) {
        if (home->t != MBJ_STR || strlen(home->s) != home->n) { mb_error(b, "%s homepage must be a string", what); return 0; }
        b->homepage = mb_trimmed_dup(home);
        if (!b->homepage) { mb_error(b, "out of memory"); return 0; }
        if (!b->homepage[0]) { free(b->homepage); b->homepage = NULL; }
        else if (mb_codepoints(b->homepage, strlen(b->homepage)) > SH_MB_HOMEPAGE_MAX ||
                 !(mb_starts(b->homepage, "https://") || mb_starts(b->homepage, "http://")) ||
                 strpbrk(b->homepage, " \t\r\n\"<>")) {
            mb_error(b, "%s homepage must be an http:// or https:// address of at most %u characters", what, SH_MB_HOMEPAGE_MAX);
            return 0;
        }
    }
    if (req) {
        b->requirements = mb_requirements(req, cause, sizeof cause);
        if (!b->requirements) { mb_error(b, "%s %s", what, cause); return 0; }
    }
    return 1;
}

/* The author block of mod.json (the file without its "snapmap-plus" member).
 * `block` is the builder block of the same file, or NULL. A retired author
 * key refuses the folder (ONE-SHAPE rule 6); every other key that is not a
 * known field is kept verbatim in "extra" (strings, hud, companions,
 * description, ...). */
static int mb_descriptor_root(sh_mb_bundle *b, const mbj *root, const mbj *block, int building)
{
    char cause[SH_MB_CAUSE_MAX];
    mb_buf extra = {0};
    size_t i, kept = 0;
    const char *retired;
    if (root->t != MBJ_OBJ) { mb_error(b, "mod.json must contain one JSON object"); return 0; }
    if ((retired = mb_retired_key(root)) != NULL) {
        mb_error(b, SH_MB_EARLIER);
        return 0;
    }
    if (mbj_get(root, "files") || mbj_get(root, "bundle") || mbj_get(root, "build")) {
        mb_error(b, "mod.json has \"files\", \"build\" or \"bundle\" at its top level, which Snapmap+ writes itself; "
                 "remove it and press Refresh");
        return 0;
    }
    if (block && block->t != MBJ_OBJ) { mb_error(b, "mod.json \"" SH_MB_BLOCK_KEY "\" must be an object"); return 0; }
    if (!mb_common_fields(b, root)) return 0;
    {
        mbj tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.t = MBJ_OBJ;
        for (i = 0; i < root->count; i++) {
            static const char *const known[] = { "name", "author", "homepage", "requirements",
                                                 SH_MB_BLOCK_KEY, NULL };
            int k, is_known = 0;
            for (k = 0; known[k]; k++)
                if (strlen(known[k]) == root->klen[i] && memcmp(known[k], root->keys[i], root->klen[i]) == 0) is_known = 1;
            if (is_known) continue;
            if ((root->klen[i] == 7 && memcmp(root->keys[i], "strings", 7) == 0 && root->items[i]->t != MBJ_OBJ) ||
                (root->klen[i] == 3 && memcmp(root->keys[i], "hud", 3) == 0 && root->items[i]->t != MBJ_OBJ)) {
                mb_error(b, "mod.json %s must be an object", root->keys[i]);
                free(tmp.items); free(tmp.keys); free(tmp.klen);
                return 0;
            }
            if (!mbj_push(&tmp, root->items[i], root->keys[i], root->klen[i])) {
                free(tmp.items); free(tmp.keys); free(tmp.klen);
                mb_error(b, "out of memory");
                return 0;
            }
            kept++;
        }
        if (kept) {
            if (!mbj_canon(&tmp, &extra, cause, sizeof cause)) {
                mb_error(b, "mod.json: %s", cause);
                free(tmp.items); free(tmp.keys); free(tmp.klen);
                free(extra.p);
                return 0;
            }
            b->extra = extra.p;
        }
        free(tmp.items); free(tmp.keys); free(tmp.klen);   /* items and keys belong to root */
    }
    return 1;
}

/* ---------------------------------------------------------------- walk ---- */

typedef struct mb_walk {
    sh_mb_bundle *b;
    wchar_t *root;          /* the mod folder, long form */
    int stopped;            /* a cap was hit; stop collecting */
    int stock;              /* a stock reader is registered: defer type and size refusals */
} mb_walk;

static const char *mb_rp_forbidden(const char *request);

static void mb_walk_dir(mb_walk *w, const wchar_t *dir, const char *rel, int depth)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mb_wjoin(dir, L"*");
    HANDLE h;
    if (!pattern) { mb_error(w->b, "out of memory"); return; }
    if (depth > MB_WALK_DEPTH) { mb_error(w->b, "folder nesting deeper than %d: %s", MB_WALK_DEPTH, rel); free(pattern); return; }
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) mb_error(w->b, "cannot list %s: %s", rel, sh_reason_win32(GetLastError(), rel, 0));
        return;
    }
    do {
        char *name, child[SH_MB_REQUEST_MAX + 16];
        int wn;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        name = sh_mb_utf8(fd.cFileName);
        if (!name) { mb_error(w->b, "a file name under %s is not valid Unicode", rel); continue; }
        wn = _snprintf_s(child, sizeof child, _TRUNCATE, "%s/%s", rel, name);
        if (wn < 0) { mb_error(w->b, "path too long under %s", rel); free(name); continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            mb_error(w->b, "symbolic link or junction not allowed: %s", child);
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t *sub = mb_wjoin(dir, fd.cFileName);
            if (!sub) mb_error(w->b, "out of memory");
            else { mb_walk_dir(w, sub, child, depth + 1); free(sub); }
        } else if (!w->stopped) {
            unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            char cause[SH_MB_CAUSE_MAX];
            const char *request = child + 7;   /* below "assets/" */
            char *req_lower = mb_strdup(request);
            sh_mb_file *f;
            /* With a stock reader a file of a type a bundle cannot carry, or
             * over 64 MiB, is refused only after it is found not to be a copy
             * of installed content; the 256 MiB total is checked on the
             * carried files after the matching. */
            int defer = w->stock;
            int deferred = 0;
            if (!req_lower) { mb_error(w->b, "out of memory"); free(name); break; }
            mb_lower(req_lower);
            if (!sh_mb_check_request(req_lower, cause, sizeof cause)) {
                mb_error(w->b, "%s: %s", child, cause);
            } else if (mb_rp_forbidden(req_lower)) {
                mb_error(w->b, "%s may not ship: %s", child, mb_rp_forbidden(req_lower));
            } else if (sh_mb_script_request(req_lower, NULL, NULL) == SH_MB_SCRIPT_MISPLACED) {
                mb_error(w->b, "%s: " SH_MB_SCRIPT_WHERE, child);
            } else if (sh_script_node_reserved(w->b->id, req_lower)) {
                mb_error(w->b, "%s: " SH_MB_SCRIPT_NODE_RESERVED, child);
            } else if (!sh_mb_extension_allowed(req_lower) && !(defer && (deferred = 1))) {
                if (sh_mb_pointer_only_kind(req_lower))
                    mb_error(w->b, "%s can only be an unchanged copy of the game's own file, and Snapmap+ could not "
                             "read the game's files to check it; start DOOM normally and press Refresh", child);
                else mb_error(w->b, "file type not allowed: %s", child);
            } else if (size == 0) {
                mb_error(w->b, "empty file: %s", child);
            } else if (size > SH_MB_MAX_FILE && !(defer && (deferred = deferred ? deferred : 2))) {
                mb_error(w->b, "file larger than 64 MiB: %s", child);
            } else if (w->b->file_count >= SH_MB_MAX_FILES) {
                mb_error(w->b, "more than %u files under assets/", SH_MB_MAX_FILES);
                w->stopped = 1;
            } else if (!defer && w->b->total_size + size > SH_MB_MAX_TOTAL) {
                mb_error(w->b, "assets/ larger than 256 MiB");
                w->stopped = 1;
            } else if (defer && w->b->total_size + size > MB_MAX_WALK) {
                mb_error(w->b, "assets is larger than %llu MiB, counting copies of game files; split the mod into two",
                         MB_MAX_WALK >> 20);
                w->stopped = 1;
            } else if ((f = mb_add_file(w->b)) == NULL) {
                mb_error(w->b, "out of memory");
            } else {
                /* DECISIONS 13: the recorded path is the game name, lower
                 * case with '/', whatever the spelling on disk (Windows
                 * opens it either way); archives and installs use it. */
                f->path = (char *)malloc(strlen(req_lower) + 8);
                if (f->path) { memcpy(f->path, "assets/", 7); strcpy_s(f->path + 7, strlen(req_lower) + 1, req_lower); }
                f->request = req_lower;
                req_lower = NULL;
                f->size = size;
                f->deferred = deferred;
                w->b->total_size += size;
                if (!f->path) mb_error(w->b, "out of memory");
            }
            free(req_lower);
        }
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void mb_walk_top(mb_walk *w)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mb_wjoin(w->root, L"*");
    HANDLE h;
    int have_assets = 0;
    if (!pattern) { mb_error(w->b, "out of memory"); return; }
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) { mb_error(w->b, "cannot list the mod folder: %s", sh_reason_win32(GetLastError(), "the mod folder", 0)); return; }
    do {
        char *name;
        /* Only mod.json and assets\ are the mod (DECISIONS 16): anything else
         * beside them (a README, desktop.ini, a .git folder) is ignored and
         * never carried. */
        if (_wcsicmp(fd.cFileName, L"assets") != 0 || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        name = sh_mb_utf8(fd.cFileName);
        if (!name) { mb_error(w->b, "out of memory"); continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            mb_error(w->b, "assets is a link to another folder; make it a real folder and press Refresh");
        } else {
            wchar_t *sub = mb_wjoin(w->root, fd.cFileName);
            have_assets = 1;
            if (sub) { mb_walk_dir(w, sub, "assets", 1); free(sub); }
            else mb_error(w->b, "out of memory");
        }
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!have_assets) mb_error(w->b, "the mod folder has no assets folder; put the mod's files in assets and press Refresh");
}

/* ------------------------------------------------------ needs, namespace ---- */

typedef struct mb_req_index { const char **req; size_t n; } mb_req_index;

static int mb_req_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static int mb_req_has(const mb_req_index *ix, const char *r)
{
    return ix->n && bsearch(&r, ix->req, ix->n, sizeof(char *), mb_req_cmp) != NULL;
}

static int mb_is_shadow(const sh_mb_bundle *b, const char *request)
{
    return b->shadow_count &&
           bsearch(&request, b->shadows, b->shadow_count, sizeof(char *), mb_str_cmp) != NULL;
}

/* 1 resolved, 0 absent, -1 cannot tell. */
static int mb_resolve_one(sh_mb_bundle *b, const mb_req_index *own, const sh_mb_resolver *r,
                          const sh_mb_need *need)
{
    char cand[2][SH_MB_REQUEST_MAX + 1];
    int n = sh_mb_need_requests(need->type, need->name, cand, 2), i, unknown = 0;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) if (mb_req_has(own, cand[i])) { b->needs.local++; return 1; }
    if (r && r->installed)
        for (i = 0; i < n; i++) if (r->installed(r->installed_context, cand[i]) == 1) { b->needs.installed++; return 1; }
    if (!r || !r->stock) unknown = 1;
    else
        for (i = 0; i < n; i++) {
            int got = r->stock(r->stock_context, cand[i]);
            if (got == 1) { b->needs.stock++; return 1; }
            if (got < 0) unknown = 1;
        }
    return unknown ? -1 : 0;
}

/* The installed text a file came from, for needs the game's own text leaves
 * dangling. A pointer's text is the installed text (source: the pointer's).
 * A carried decl under the key is compared with the installed decl at its
 * request with the mods/<salt>/ segment cut (a copy of stock text after the
 * namespace rewrite), loaded once per file on its first unresolved need. */
typedef struct mb_twin {
    int state;                          /* 0 not looked up, 1 loaded, -1 none */
    sh_mb_file text;                    /* only needs, need_count used */
    char source[SH_MB_REQUEST_MAX + 1];
} mb_twin;

static void mb_twin_reset(mb_twin *t)
{
    size_t i;
    for (i = 0; i < t->text.need_count; i++) { free(t->text.needs[i].type); free(t->text.needs[i].name); }
    free(t->text.needs);
    memset(t, 0, sizeof *t);
}

static void mb_twin_load(mb_twin *t, const sh_mb_file *f)
{
    const sh_mb_stock_ops *ops = mb_stock();
    char sha[SH_MB_HASH_HEX + 1], cause[SH_MB_CAUSE_MAX];
    unsigned long long size = 0;
    unsigned char *text = NULL;
    size_t len = 0;
    t->state = -1;
    if (!f->is_decl || !f->ptype || !ops || !ops->identify) return;
    if (!sh_mb_strip_key(f->request, t->source, sizeof t->source)) return;
    if (ops->identify(ops->context, t->source, sha, &size) != SH_MB_STOCK_OK || strlen(sha) != SH_MB_HASH_HEX) return;
    if (sh_mb_stock_read(t->source, sha, &text, &len, f->path, cause, sizeof cause) != SH_MB_STOCK_OK) return;
    if (sh_decl_text_well_formed(text, len)) {
        mb_need_ctx nc = { &t->text, 0 };
        sh_mb_scan_needs(f->ptype, text, len, mb_need_cb, &nc);
        if (!nc.oom) t->state = 1;
    }
    HeapFree(GetProcessHeap(), 0, text);
}

/* The installed decl that names this unresolved need too, or NULL. */
static const char *mb_stock_names_too(mb_twin *t, const sh_mb_file *f, const sh_mb_need *need)
{
    char cut[SH_MB_REQUEST_MAX + 1];
    int has_cut;
    size_t i;
    if (f->stock && f->stock_source) return f->stock_source;
    if (!t->state) mb_twin_load(t, f);
    if (t->state != 1) return NULL;
    has_cut = sh_mb_strip_key(need->name, cut, sizeof cut);
    for (i = 0; i < t->text.need_count; i++) {
        const sh_mb_need *s = &t->text.needs[i];
        if (strcmp(s->type, need->type) != 0) continue;
        if (_stricmp(s->name, need->name) == 0 || (has_cut && _stricmp(s->name, cut) == 0)) return t->source;
    }
    return NULL;
}

/* ----------------------------------------------------- render programs ---- */
/* A bundle that carries a render program must carry what the engine checks:
 * the cooked program's checksum is MD5fold of the served decl text, each
 * SPIR-V header names the stage text it was compiled from, and every stage
 * and permutation the engine binds is present (a missing stage file is a
 * fatal error at bind). MD5fold is MD5 with its four 32-bit words XORed
 * together (VK 0x1402d6a90). */

static const uint32_t MB_MD5_K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};
static const unsigned char MB_MD5_S[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void mb_md5_block(uint32_t h[4], const unsigned char *p)
{
    uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    int i;
    for (i = 0; i < 16; i++)
        m[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) |
               ((uint32_t)p[4 * i + 3] << 24);
    for (i = 0; i < 64; i++) {
        uint32_t f, t;
        int g, s = MB_MD5_S[(i >> 4) * 4 + (i & 3)];
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        t = f + a + MB_MD5_K[i] + m[g];
        a = d; d = c; c = b;
        b = b + ((t << s) | (t >> (32 - s)));
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

void sh_mb_md5(const void *data, size_t len, unsigned char digest[16])
{
    uint32_t h[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    const unsigned char *p = (const unsigned char *)data;
    unsigned char tail[128];
    uint64_t bits = (uint64_t)len * 8u;
    size_t done = 0, rest, pad, i;
    for (; done + 64 <= len; done += 64) mb_md5_block(h, p + done);
    rest = len - done;
    memcpy(tail, p + done, rest);
    tail[rest] = 0x80;
    pad = rest < 56 ? 64 : 128;
    memset(tail + rest + 1, 0, pad - rest - 1);
    for (i = 0; i < 8; i++) tail[pad - 8 + i] = (unsigned char)(bits >> (8 * i));
    mb_md5_block(h, tail);
    if (pad == 128) mb_md5_block(h, tail + 64);
    for (i = 0; i < 16; i++) digest[i] = (unsigned char)(h[i / 4] >> (8 * (i % 4)));
}

unsigned sh_mb_md5fold(const void *data, size_t len)
{
    unsigned char d[16];
    uint32_t w[4];
    int i;
    sh_mb_md5(data, len, d);
    for (i = 0; i < 4; i++)
        w[i] = (uint32_t)d[4 * i] | ((uint32_t)d[4 * i + 1] << 8) | ((uint32_t)d[4 * i + 2] << 16) |
               ((uint32_t)d[4 * i + 3] << 24);
    return w[0] ^ w[1] ^ w[2] ^ w[3];
}

static uint32_t mb_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t mb_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int mb_stage_text_ok(const unsigned char *t, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!(t[i] == '\t' || t[i] == '\n' || t[i] == '\r' || (t[i] >= 0x20 && t[i] < 0x7f))) return 0;
    return 1;
}

/* The body reader (VK 0x14190f4a0, GL 0x141904030) ends with three stage
 * records -- u32, u32, u16 (big-endian), then the stage text as a
 * little-endian u32 length and that many bytes -- and a 36-byte trailer of
 * nine u32 fields. The fields before the records depend on the renderparm
 * types the engine resolves while reading, so the records are found from the
 * end: the one offset whose three records end exactly at the trailer. */
int sh_mb_rp_stages(const unsigned char *bin, size_t len, size_t off[3], size_t n[3])
{
    size_t p, found = 0, o[3] = { 0, 0, 0 }, c[3] = { 0, 0, 0 };
    if (!bin || len < 8 + 3 * 14 + 36) return 0;
    for (p = 8; p + 3 * 14 + 36 <= len; p++) {
        size_t q = p, to[3], tn[3];
        int k;
        for (k = 0; k < 3; k++) {
            uint32_t ln;
            if (q + 14 > len - 36) break;
            ln = mb_le32(bin + q + 10);
            if (ln > len - 36 - (q + 14)) break;
            if (!mb_stage_text_ok(bin + q + 14, ln)) break;
            to[k] = q + 14; tn[k] = ln;
            q += 14 + ln;
        }
        if (k == 3 && q == len - 36) {
            if (++found > 1) return -1;
            memcpy(o, to, sizeof o); memcpy(c, tn, sizeof c);
        }
    }
    if (!found) return 0;
    if (off) memcpy(off, o, sizeof o);
    if (n) memcpy(n, c, sizeof c);
    return 1;
}

/* The stage loader's file name: "generated/spirv/" + the program's name with
 * its extension replaced, where a basename (extension included) of 38 or more
 * characters is cut into 30-character components. */
int sh_mb_spirv_request(const char *name, const char *ext, char *out, size_t cap)
{
    char stem[SH_MB_REQUEST_MAX + 1];
    const char *slash = strrchr(name, '/'), *base = slash ? slash + 1 : name, *dot = strrchr(base, '.');
    size_t dir_n = (size_t)(base - name), base_n = dot ? (size_t)(dot - base) : strlen(base), ext_n = strlen(ext);
    size_t pos = 0, i;
    if (dir_n + base_n + ext_n + 1 > sizeof stem) return 0;
    if (16 + dir_n + base_n + base_n / 30 + ext_n + 1 > cap) return 0;
    memcpy(stem, name, dir_n + base_n);
    stem[dir_n + base_n] = 0;
    memcpy(out, "generated/spirv/", 16); pos = 16;
    memcpy(out + pos, stem, dir_n); pos += dir_n;
    if (base_n + ext_n < 38) {
        memcpy(out + pos, stem + dir_n, base_n); pos += base_n;
    } else {
        for (i = 0; i < base_n; i++) {
            if (i && i % 30 == 0) out[pos++] = '/';
            out[pos++] = stem[dir_n + i];
        }
    }
    memcpy(out + pos, ext, ext_n + 1);
    return 1;
}

/* generated/renderprogs/permutations/<program>_<16 hex digits>.decl: the name
 * the permutation lookup formats (VK 0x141910010) and passes to DeclFind. */
int sh_mb_rp_permutation(const char *request, char *program, size_t cap, char mask[17])
{
    static const char prefix[] = "generated/renderprogs/permutations/";
    size_t n = strlen(request), pn = sizeof prefix - 1, i, prog_n;
    const char *m;
    if (n < pn + 1 + 1 + 16 + 5 || strncmp(request, prefix, pn) != 0 || strcmp(request + n - 5, ".decl") != 0)
        return 0;
    m = request + n - 5 - 16;
    if (m[-1] != '_') return 0;
    for (i = 0; i < 16; i++)
        if (!((m[i] >= '0' && m[i] <= '9') || (m[i] >= 'a' && m[i] <= 'f'))) return 0;
    prog_n = (size_t)(m - 1 - (request + pn));
    if (prog_n == 0 || prog_n + 1 > cap) return 0;
    memcpy(program, request + pn, prog_n);
    program[prog_n] = 0;
    if (mask) { memcpy(mask, m, 16); mask[16] = 0; }
    return 1;
}

/* Files the engine writes or shares between every program (check 7). */
static const char *mb_rp_forbidden(const char *r)
{
    const char *slash = strrchr(r, '/');
    if (mb_starts(r, "decls/renderprogs/"))
        return "a shared render-program include (decls/renderprogs/) changes every program that includes it";
    if (!strcmp(slash ? slash + 1 : r, "vulkan_pipelines.bin"))
        return "vulkan_pipelines.bin is the engine's pipeline warm-up list";
    if (mb_starts(r, "generated/glslbinaries/"))
        return "generated/glslBinaries/ is the OpenGL driver's binary cache";
    if (mb_starts(r, "generated/glsl/"))
        return "generated/glsl/ is the engine's debug dump of generated GLSL";
    return NULL;
}

typedef struct mb_rp {
    sh_mb_bundle *b;
    const wchar_t *root;
    unsigned char *used;
} mb_rp;

#define MB_RP_ERR(strict, ...) do { if (strict) mb_error(rp->b, __VA_ARGS__); else mb_warn(rp->b, __VA_ARGS__); } while (0)

static sh_mb_file *mb_rp_find(mb_rp *rp, const char *request)
{
    size_t i;
    for (i = 0; i < rp->b->file_count; i++)
        if (!strcmp(rp->b->files[i].request, request)) { rp->used[i] = 1; return &rp->b->files[i]; }
    return NULL;
}

/* The file's bytes: a carried file from the folder, a pointer from the
 * installed game. Free with mb_rp_release. */
static unsigned char *mb_rp_bytes(mb_rp *rp, const sh_mb_file *f, size_t *len, int *heap)
{
    *heap = 0;
    *len = 0;
    if (f->stock) {
        unsigned char *bytes = NULL;
        char cause[SH_MB_CAUSE_MAX];
        if (!f->stock_source || sh_mb_stock_read(f->stock_source, f->blob, &bytes, len, f->path, cause, sizeof cause) != SH_MB_STOCK_OK) {
            mb_error(rp->b, "cannot read %s to check its render program", f->path);
            return NULL;
        }
        *heap = 1;
        return bytes;
    } else {
        char *rel = mb_strdup(f->path);
        wchar_t *wrel = NULL, *full;
        unsigned char *bytes;
        int wn;
        if (!rel) return NULL;
        for (char *p = rel; *p; p++) if (*p == '/') *p = '\\';
        wn = MultiByteToWideChar(CP_UTF8, 0, rel, -1, NULL, 0);
        if (wn > 0 && (wrel = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wn)) != NULL)
            MultiByteToWideChar(CP_UTF8, 0, rel, -1, wrel, wn);
        free(rel);
        full = wrel ? mb_wjoin(rp->root, wrel) : NULL;
        free(wrel);
        if (!full) return NULL;
        bytes = mb_read_wfile(full, SH_MB_MAX_FILE, len);
        free(full);
        if (!bytes) mb_error(rp->b, "cannot read %s to check its render program", f->path);
        return bytes;
    }
}

static void mb_rp_release(unsigned char *p, int heap)
{
    if (!p) return;
    if (heap) HeapFree(GetProcessHeap(), 0, p);
    else free(p);
}

/* Check 1: magic 0xB68800AD, then the checksum SetText (VK 0x141563030)
 * stores: MD5fold of the decl text without trailing spaces, CRs and LFs.
 * Returns the cooked bytes on success (the caller releases them). */
static unsigned char *mb_rp_check_bin(mb_rp *rp, const sh_mb_file *decl, const sh_mb_file *bin, int strict,
                                      size_t *bin_len, int *bin_heap)
{
    size_t dlen = 0, blen = 0;
    int dheap = 0, bheap = 0;
    unsigned char *d = mb_rp_bytes(rp, decl, &dlen, &dheap), *c;
    unsigned want, got;
    if (!d) return NULL;
    while (dlen && (d[dlen - 1] == ' ' || d[dlen - 1] == '\r' || d[dlen - 1] == '\n')) dlen--;
    want = sh_mb_md5fold(d, dlen);
    mb_rp_release(d, dheap);
    c = mb_rp_bytes(rp, bin, &blen, &bheap);
    if (!c) return NULL;
    if (blen < 8 || mb_be32(c) != 0xB68800ADu) {
        MB_RP_ERR(strict, "%s is not a cooked render program (magic 0xB68800AD missing)", bin->path);
        mb_rp_release(c, bheap);
        return NULL;
    }
    got = mb_be32(c + 4);
    if (got != want) {
        MB_RP_ERR(strict, "%s has checksum %08x but MD5fold of %s is %08x: the engine ignores it and compiles the decl text",
                  bin->path, got, decl->path, want);
        mb_rp_release(c, bheap);
        return NULL;
    }
    *bin_len = blen;
    *bin_heap = bheap;
    return c;
}

/* Check 2 on one SPIR-V file: header {4, hashA, hashB, length}, big-endian;
 * hashA is MD5fold of the stage text when it is known (stage != NULL). */
static void mb_rp_check_spirv(mb_rp *rp, const sh_mb_file *f, const unsigned char *stage, size_t stage_n, int strict)
{
    size_t len = 0;
    int heap = 0;
    unsigned char *s = mb_rp_bytes(rp, f, &len, &heap);
    if (!s) return;
    if (len < 16 || mb_be32(s) != 4u)
        MB_RP_ERR(strict, "%s: SPIR-V header version is not 4 (the engine stops with \"out of date\" at a map load)", f->path);
    else if (mb_be32(s + 12) != len - 16)
        MB_RP_ERR(strict, "%s: SPIR-V header length %u but %llu bytes follow it", f->path, mb_be32(s + 12),
                  (unsigned long long)(len - 16));
    else if (stage && mb_be32(s + 4) != sh_mb_md5fold(stage, stage_n))
        MB_RP_ERR(strict, "%s: SPIR-V hash %08x is not MD5fold of its stage text in the cooked program (%08x): "
                  "the engine stops with \"out of date\" at a map load",
                  f->path, mb_be32(s + 4), sh_mb_md5fold(stage, stage_n));
    mb_rp_release(s, heap);
}

/* One program unit (the program or one permutation): checks 1 to 3 for the
 * Vulkan files, check 1 for an OpenGL _pc.bin if one ships. `name` is what
 * the engine builds the SPIR-V names from. */
static void mb_rp_unit(mb_rp *rp, const sh_mb_file *decl, const char *stem, const char *name, int strict)
{
    static const char *const ext[3] = { ".vspv", ".fspv", ".cspv" };
    static const char *const stage_name[3] = { "vertex", "fragment", "compute" };
    char req[SH_MB_REQUEST_MAX + 32];
    const sh_mb_file *bin, *gl;
    unsigned char *c;
    size_t blen = 0, off[3], n[3];
    int heap = 0, k, r;

    _snprintf_s(req, sizeof req, _TRUNCATE, "%s_pc.bin", stem);
    gl = mb_rp_find(rp, req);
    if (gl) {
        size_t gl_len = 0;
        int gl_heap = 0;
        mb_rp_release(mb_rp_check_bin(rp, decl, gl, strict, &gl_len, &gl_heap), gl_heap);
    }
    _snprintf_s(req, sizeof req, _TRUNCATE, "%s_pc_vulkan.bin", stem);
    bin = mb_rp_find(rp, req);
    if (!bin) {
        MB_RP_ERR(strict, "%s ships no assets/%s: Vulkan needs the cooked program", decl->path, req);
        return;
    }
    c = mb_rp_check_bin(rp, decl, bin, strict, &blen, &heap);
    if (!c) return;
    r = sh_mb_rp_stages(c, blen, off, n);
    for (k = 0; k < 3; k++) {
        const sh_mb_file *s;
        if (r == 1 && n[k] == 0) continue;
        if (!sh_mb_spirv_request(name, ext[k], req, sizeof req)) { mb_error(rp->b, "%s: name too long", decl->path); break; }
        s = mb_rp_find(rp, req);
        if (!s) {
            /* With the stage texts unknown, only the two stages every
             * drawing program has are required. */
            if (r == 1 || k < 2)
                MB_RP_ERR(strict, "%s needs assets/%s (its %s stage): the engine stops with \"not found\" when it binds",
                          decl->path, req, stage_name[k]);
            continue;
        }
        mb_rp_check_spirv(rp, s, r == 1 ? c + off[k] : NULL, r == 1 ? n[k] : 0, strict);
    }
    if (r != 1)
        mb_warn(rp->b, "%s: the stage texts were not found, so SPIR-V hashes were not checked (version and length only)",
                bin->path);
    mb_rp_release(c, heap);
}

/* A permutation decl is decl text the engine parses like the program's. */
static int mb_rp_decl_ok(mb_rp *rp, const sh_mb_file *f, int strict)
{
    size_t len = 0;
    int heap = 0, ok;
    unsigned char *t = mb_rp_bytes(rp, f, &len, &heap);
    if (!t) return 0;
    ok = sh_decl_text_well_formed(t, len);
    if (!ok) MB_RP_ERR(strict, "%s is not a well-formed decl", f->path);
    mb_rp_release(t, heap);
    return ok;
}

static int mb_rp_last_has_dot(const char *name, size_t n)
{
    size_t i = n;
    while (i > 0 && name[i - 1] != '/') i--;
    return memchr(name + i, '.', n - i) != NULL;
}

/* Check 6: a material names a program in quotes (the lexer cuts a bare name
 * at '/', '.' or '-'), and a mod program has no '.' in its last component
 * (the cooked-name builder strips from the last '.'). */
static void mb_rp_check_material(mb_rp *rp, const sh_mb_file *f)
{
    static const char *const keys[] = { "addProgram", "ambientProgram", "blendProgram", "depthFixupProgram",
                                        "fadePerturbProgram", "interactionProgram", "particleLightAtlasProgram",
                                        "shadowProgram", "stageProgram", "zprepassProgram", NULL };
    size_t len = 0, pos = 0;
    int heap = 0, strict = 1;
    unsigned char *t = mb_rp_bytes(rp, f, &len, &heap);
    mb_tok prev = { TK_END, NULL, 0 }, k;
    if (!t) return;
    for (;;) {
        k = mb_next_tok(t, len, &pos);
        if (k.kind == TK_END) break;
        if (prev.kind == TK_WORD && (k.kind == TK_WORD || k.kind == TK_STR)) {
            int i;
            for (i = 0; keys[i]; i++) if (mb_ieq_n(prev.p, prev.n, keys[i])) break;
            if (keys[i]) {
                size_t j;
                int bare_bad = 0;
                if (k.kind == TK_WORD)
                    for (j = 0; j < k.n; j++) if (strchr("/\\:.$-", k.p[j])) { bare_bad = 1; break; }
                if (bare_bad)
                    MB_RP_ERR(strict, "%s: %s %.*s must be quoted (the engine reads a bare program name only up to '/', '.' or '-')",
                              f->path, keys[i], (int)k.n, k.p);
                else if (k.n > 5 && mb_ieq_n(k.p, 5, "mods/") && mb_rp_last_has_dot(k.p, k.n))
                    MB_RP_ERR(strict, "%s: program %.*s has a '.' in its last component, which the engine's file names cut",
                              f->path, (int)k.n, k.p);
                prev.kind = TK_END;
                continue;
            }
        }
        prev = k;
    }
    mb_rp_release(t, heap);
}

static int mb_rp_relevant(const char *r)
{
    return mb_starts(r, "generated/renderprogs/") || mb_starts(r, "generated/spirv/") ||
           mb_starts(r, "generated/decls/renderprog/");
}

static void mb_check_renderprogs(sh_mb_bundle *b, const wchar_t *root)
{
    mb_rp ctx, *rp = &ctx;
    size_t i, j, any = 0;
    char program[SH_MB_REQUEST_MAX + 1], mask[17], stem[SH_MB_REQUEST_MAX + 64];
    ctx.b = b;
    ctx.root = root;
    ctx.used = (unsigned char *)calloc(b->file_count ? b->file_count : 1, 1);
    if (!ctx.used) { mb_error(b, "out of memory"); return; }

    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        if (!f->ptype || !f->pname) continue;
        if (f->is_decl && (!strcmp(f->ptype, "material") || !strcmp(f->ptype, "material2"))) mb_rp_check_material(rp, f);
        if (mb_rp_relevant(f->request)) any = 1;
    }
    if (!any) { free(ctx.used); return; }

    /* Each program decl, its permutations and their files. A shadow replaces
     * a stock program, whose other files the game may still provide, so its
     * missing files only warn. */
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        int strict, perms = 0;
        if (!f->is_decl || !f->ptype || !f->pname || strcmp(f->ptype, "renderProg") != 0) continue;
        ctx.used[i] = 1;
        strict = !f->shadowed;
        if (f->shadowed)
            mb_warn(b, "%s replaces the built-in render program %s for every material that names it", f->path, f->pname);
        if (mb_rp_last_has_dot(f->pname, strlen(f->pname))) {
            MB_RP_ERR(1, "%s: program %s has a '.' in its last component, which the engine's file names cut",
                      f->path, f->pname);
            continue;
        }
        _snprintf_s(stem, sizeof stem, _TRUNCATE, "generated/renderprogs/%s", f->pname);
        mb_rp_unit(rp, f, stem, f->pname, strict);
        for (j = 0; j < b->file_count; j++) {
            const sh_mb_file *p = &b->files[j];
            size_t sn;
            if (!sh_mb_rp_permutation(p->request, program, sizeof program, mask) || strcmp(program, f->pname) != 0) continue;
            ctx.used[j] = 1;
            perms++;
            if (!mb_rp_decl_ok(rp, p, strict)) continue;
            sn = strlen(p->request) - 5;   /* the stem is the request without ".decl" */
            _snprintf_s(stem, sizeof stem, _TRUNCATE, "%.*s", (int)sn, p->request);
            mb_rp_unit(rp, p, stem, p->request, strict);
        }
        if (!perms)
            MB_RP_ERR(strict, "%s ships no permutation decl (generated/renderprogs/permutations/%s_<16 hex digits>.decl): "
                      "on GPUs with the AMD shader extensions the program binds no shaders",
                      f->path, f->pname);
    }

    /* Render-program files no program of the bundle reads. */
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        const char *r = f->request;
        if (ctx.used[i] || !f->ptype) continue;
        if (mb_starts(r, "generated/renderprogs/permutations/") && mb_ends(r, ".decl")) {
            if (sh_mb_rp_permutation(r, program, sizeof program, mask))
                MB_RP_ERR(1, "%s is a permutation of %s, which this mod does not include; add the program's decl or delete the permutation", f->path, program);
            else
                MB_RP_ERR(1, "%s: a permutation decl is named <program>_<16 hex digits>.decl", f->path);
        } else if (mb_starts(r, "generated/renderprogs/") && mb_ends(r, ".bin")) {
            MB_RP_ERR(1, "%s: no render program of this mod reads it; add its decl or delete the file", f->path);
        } else if (mb_starts(r, "generated/spirv/")) {
            mb_rp_check_spirv(rp, f, NULL, 0, 1);
            mb_warn(b, "%s: no render program of this mod uses it; it can be deleted", f->path);
        }
    }
    free(ctx.used);
}

/* ------------------------------------------------------ classification ---- */

static size_t mb_text_end(const unsigned char *t, size_t n)
{
    while (n && (t[n - 1] == '\n' || t[n - 1] == '\r')) n--;
    return n;
}

int sh_mb_text_same(const unsigned char *a, size_t an, const unsigned char *b, size_t bn)
{
    size_t i = 0, j = 0;
    if (!a || !b) return 0;
    an = mb_text_end(a, an);
    bn = mb_text_end(b, bn);
    for (;;) {
        if (i + 1 < an && a[i] == '\r' && a[i + 1] == '\n') i++;
        if (j + 1 < bn && b[j] == '\r' && b[j + 1] == '\n') j++;
        if (i == an || j == bn) return i == an && j == bn;
        if (a[i] != b[j]) return 0;
        i++;
        j++;
    }
}

/* Decl-grammar text: a decl under generated/decls/, a snapModule decl, or a
 * render program's permutation decl (generated/renderprogs/permutations/
 * <program>_<mask>.decl, `{ permutations { } parms { } state { } hlsl_vp { }
 * hlsl_fp { } }`), which the engine parses like its program's decl. Its
 * identity is its text and a copy at a game name is never a binary. */
static int mb_decl_text(const sh_mb_file *f)
{
    char program[SH_MB_REQUEST_MAX + 1];
    return f->is_decl || f->is_module || sh_mb_rp_permutation(f->request, program, sizeof program, NULL);
}

/* A decl-grammar copy at a game name whose text is the game's but for line
 * ends (an export that turned CRLF into LF) is the game's file: a pointer
 * recording the game's bytes, served from the player's disk. Anything else
 * is left as it was (an edited copy, or a name the game does not hold). */
static wchar_t *mb_file_wpath(const wchar_t *root, const char *path);
static void mb_text_pointer(sh_mb_bundle *b, sh_mb_file *f, const wchar_t *root)
{
    char sha[SH_MB_HASH_HEX + 1], cause[SH_MB_CAUSE_MAX];
    unsigned long long size = 0;
    unsigned char *game = NULL, *copy;
    size_t glen = 0, clen = 0;
    wchar_t *full;
    if (sh_mb_stock_identify(f->request, sha, &size) != SH_MB_STOCK_OK || size > SH_MB_MAX_FILE) return;
    if (sh_mb_stock_read(f->request, sha, &game, &glen, f->path, cause, sizeof cause) != SH_MB_STOCK_OK) return;
    full = mb_file_wpath(root, f->path);
    copy = full ? mb_read_wfile(full, SH_MB_MAX_FILE, &clen) : NULL;
    free(full);
    if (copy && sh_mb_text_same(game, glen, copy, clen)) {
        char *source = mb_strdup(f->request);
        if (!source) {
            mb_error(b, "out of memory");
        } else {
            f->stock = 1;
            f->stock_source = source;
            memcpy(f->blob, sha, sizeof sha);
            f->size = size;
        }
    }
    free(copy);
    HeapFree(GetProcessHeap(), 0, game);
}

/* Whether f's name is under the mod's own namespace: 1 yes; 0 no, with the rule it misses in `why`. A decl is named
 * mods/<salt>/...; a skins decl by the mesh it skins and a raw file by its
 * path, either containing /mods/<salt>/ or the engine's '_' spelling of it; a
 * module lives in maps/modules/mods/<salt>/; a bank's stem starts with the
 * key. Pure: the sub-rules of a name under it are the caller's. */
static int mb_ns_test(const sh_mb_bundle *b, const sh_mb_file *f, char *why, size_t cap)
{
    char ns_decl[SH_MB_ID_MAX + 8], ns_raw[SH_MB_ID_MAX + 8];
    char slashed[SH_MB_REQUEST_MAX + 18];
    if (why && cap) why[0] = 0;
    _snprintf_s(ns_decl, sizeof ns_decl, _TRUNCATE, "mods/%s/", b->id);
    _snprintf_s(ns_raw, sizeof ns_raw, _TRUNCATE, "/mods/%s/", b->id);
    if (f->is_module) {
        char mod_ns[SH_MB_ID_MAX + 32];
        _snprintf_s(mod_ns, sizeof mod_ns, _TRUNCATE, "maps/modules/mods/%s/", b->id);
        if (mb_starts(f->request, mod_ns)) return 1;
        return mb_cause(why, cap, "a snapModule decl must be maps/modules/mods/%s/<name>.decl", b->id);
    }
    if (f->is_decl && f->ptype && _stricmp(f->ptype, "skins") == 0) {
        char *p;
        _snprintf_s(slashed, sizeof slashed, _TRUNCATE, "/%s", f->pname ? f->pname : "");
        for (p = slashed; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(slashed, ns_raw)) return 1;
        return mb_cause(why, cap, "skins decl %s does not name a mesh under /mods/%s/", f->pname ? f->pname : "", b->id);
    }
    if (f->is_decl) {
        if (f->pname && mb_starts(f->pname, ns_decl)) return 1;
        return mb_cause(why, cap, "decl %s %s is outside mods/%s/", f->ptype ? f->ptype : "", f->pname ? f->pname : "", b->id);
    }
    if (mb_ends(f->request, ".bnk")) {
        char cause[SH_MB_CAUSE_MAX];
        if (sh_mb_bank_rule(f->request, b->id, cause, sizeof cause)) return 1;
        return mb_cause(why, cap, "%s", cause);
    }
    _snprintf_s(slashed, sizeof slashed, _TRUNCATE, "/%s", f->request);
    if (strstr(slashed, ns_raw)) return 1;
    return mb_cause(why, cap, "it is not under a mods folder named like this mod folder");
}

/* SH_MB_CLASS_* of one file of a built or loaded bundle. */
static int mb_class_of(const sh_mb_bundle *b, const sh_mb_file *f)
{
    int restore = f->stock && f->stock_source && sh_mb_stock_restore(f->request, f->stock_source);
    int in_ns = !restore && mb_ns_test(b, f, NULL, 0);
    /* An editor type outside the key is a local tweak; under the key it is a
     * Toybox row the mod adds and travels with it */
    if (sh_mb_editor_time(f->request) && !in_ns) return SH_MB_CLASS_EDITOR;
    if (restore) return SH_MB_CLASS_SURFACE;
    if (in_ns) return f->stock ? SH_MB_CLASS_BORROW : SH_MB_CLASS_ADD;
    return SH_MB_CLASS_REPLACE;
}

static void mb_classify_all(sh_mb_bundle *b)
{
    size_t i;
    memset(&b->classes, 0, sizeof b->classes);
    for (i = 0; i < b->file_count; i++) {
        sh_mb_file *f = &b->files[i];
        f->edited = 0;
        f->cls = mb_class_of(b, f);
        switch (f->cls) {
        case SH_MB_CLASS_ADD: b->classes.adds++; break;
        case SH_MB_CLASS_BORROW: b->classes.borrows++; break;
        case SH_MB_CLASS_REPLACE: b->classes.replaces++; break;
        case SH_MB_CLASS_EDITOR: b->classes.editor++; break;
        case SH_MB_CLASS_SURFACE: b->classes.surfaces++; break;
        default: break;
        }
    }
}

/* The canonical shadows of a bundle that is not grandfathered: every file
 * the classification put at a game name outside the namespace. */
static void mb_derive_shadows(sh_mb_bundle *b)
{
    size_t i, n = 0;
    for (i = 0; i < b->shadow_count; i++) free(b->shadows[i]);
    free(b->shadows);
    b->shadows = NULL;
    b->shadow_count = 0;
    for (i = 0; i < b->file_count; i++) if (b->files[i].shadowed) n++;
    if (!n) return;
    b->shadows = (char **)calloc(n, sizeof(char *));
    if (!b->shadows) { mb_error(b, "out of memory"); return; }
    for (i = 0; i < b->file_count; i++) {
        if (!b->files[i].shadowed) continue;
        b->shadows[b->shadow_count] = mb_strdup(b->files[i].request);
        if (!b->shadows[b->shadow_count]) { mb_error(b, "out of memory"); return; }
        b->shadow_count++;
    }
    qsort(b->shadows, b->shadow_count, sizeof(char *), mb_str_cmp);
}

static int mb_cstr_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static sh_mb_bank_need_fn volatile g_bank_need;
void sh_mb_set_bank_need(sh_mb_bank_need_fn fn) { InterlockedExchangePointer((void *volatile *)&g_bank_need, (void *)fn); }

/* registries "banks": the stock banks the mod's sound decls need loaded on
 * demand (each once, byte order), with one note per bank naming the first
 * decl that needs it. */
static void mb_registry_banks(sh_mb_bundle *b, mb_buf *o, int *first)
{
    enum { MB_BANKS_MAX = 16 };
    sh_mb_bank_need_fn need = g_bank_need;
    char banks[MB_BANKS_MAX][80], *ord[MB_BANKS_MAX];
    const char *why[MB_BANKS_MAX];
    size_t i, n = 0, k;
    if (!need) return;
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        char bank[80];
        if (!f->ptype || !f->pname || _stricmp(f->ptype, "sound") != 0) continue;
        if (!need(f->pname, bank, sizeof bank) || !bank[0]) continue;
        for (k = 0; k < n && strcmp(banks[k], bank); k++) {}
        if (k < n || n == MB_BANKS_MAX) continue;
        strcpy_s(banks[n], sizeof banks[n], bank);
        why[n] = f->pname;
        n++;
    }
    if (!n) return;
    for (i = 0; i < n; i++) {
        ord[i] = banks[i];
        mb_note(b, "sound %s plays a game event only %s holds, a bank SnapMap skips; it is loaded once per session "
                "for a map that declares this mod", why[i], banks[i]);
    }
    qsort(ord, n, sizeof ord[0], mb_cstr_cmp);
    mb_puts(o, *first ? "\"banks\":[" : ",\"banks\":[");
    for (i = 0; i < n; i++) {
        if (i) mb_put(o, ",", 1);
        mb_put_jcstr(o, ord[i]);
    }
    mb_put(o, "]", 1);
    *first = 0;
}

/* The product block's "registries" object: what the
 * product will generate from this bundle when a map declares it. Keys in
 * byte order; members only when not empty. */
/* A strings key is `<salt>_<name>` (DECISIONS 5): the salt, an underscore, and
 * a name of letters, digits and underscores. No '/': the engine's string ids
 * were only ever exercised with [a-z0-9_]. Keys compare without case. */
static int mb_string_key_ok(const char *key, const char *salt)
{
    size_t n = strlen(salt), i;
    if (!n || _strnicmp(key, salt, n) != 0 || key[n] != '_' || !key[n + 1]) return 0;
    for (i = n + 1; key[i]; i++)
        if (!((key[i] >= 'a' && key[i] <= 'z') || (key[i] >= 'A' && key[i] <= 'Z') ||
              (key[i] >= '0' && key[i] <= '9') || key[i] == '_')) return 0;
    return 1;
}

static void mb_registries(sh_mb_bundle *b)
{
    mb_buf o = {0};
    size_t i;
    int any, first = 1;
    mbj *extra = NULL;
    const mbj *strings = NULL, *en = NULL;
    mb_put(&o, "{", 1);
    mb_registry_banks(b, &o, &first);   /* "banks" sorts first */
    for (i = 0, any = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        if (!f->ptype || !f->pname || _stricmp(f->ptype, "snapAIEncounter") != 0) continue;
        mb_puts(&o, any ? "," : first ? "\"conductor\":[" : ",\"conductor\":[");
        mb_put_jcstr(&o, f->pname);
        any = 1;
    }
    if (any) { mb_put(&o, "]", 1); first = 0; }
    if (b->extra) extra = mbj_parse(b->extra, strlen(b->extra), NULL, 0);
    strings = extra ? mbj_get(extra, "strings") : NULL;
    en = strings && strings->t == MBJ_OBJ ? mbj_get(strings, "en") : NULL;
    if (en && en->t == MBJ_OBJ && en->count) {
        /* DECISIONS 9: a key the game or the product defines is an edit of
         * that string ("stringEdits"); every other key is the mod's own,
         * named under its salt ("strings"). */
        char **keys = (char **)calloc(en->count, sizeof(char *)), **edits = (char **)calloc(en->count, sizeof(char *));
        size_t n = 0, ne = 0;
        for (i = 0; keys && edits && i < en->count; i++) {
            char *k = mb_strndup(en->keys[i], en->klen[i]);
            if (!k) continue;
            if (sh_strkey_is_edit(k, strlen(k))) edits[ne++] = k;
            else keys[n++] = k;
        }
        if (edits && ne) {
            qsort(edits, ne, sizeof(char *), mb_cstr_cmp);
            mb_puts(&o, first ? "\"stringEdits\":[" : ",\"stringEdits\":[");
            for (i = 0; i < ne; i++) {
                if (i) mb_put(&o, ",", 1);
                mb_put_jcstr(&o, edits[i]);
            }
            mb_put(&o, "]", 1);
            first = 0;
            mb_note(b, "changes the text of %zu string%s the game or Snapmap+ defines: they apply %s", ne,
                    ne == 1 ? "" : "s", "where this mod's other edits apply");
        }
        if (keys && n) {
            qsort(keys, n, sizeof(char *), mb_cstr_cmp);
            mb_puts(&o, first ? "\"strings\":[" : ",\"strings\":[");
            for (i = 0; i < n; i++) {
                if (i) mb_put(&o, ",", 1);
                mb_put_jcstr(&o, keys[i]);
                if (!mb_string_key_ok(keys[i], b->id))
                    mb_error(b, "mod.json strings key %s does not start with %s_; press Refresh and Snapmap+ names it "
                             "so (a key the game already defines changes that game string instead)", keys[i], b->id);
            }
            mb_put(&o, "]", 1);
        }
        for (i = 0; keys && i < n; i++) free(keys[i]);
        for (i = 0; edits && i < ne; i++) free(edits[i]);
        free(keys);
        free(edits);
    }
    mbj_free(extra);
    mb_put(&o, "}", 1);
    free(b->registries);
    b->registries = o.oom ? NULL : o.p;
    if (o.oom) { free(o.p); mb_error(b, "out of memory"); }
}

static void mb_counts_cb(void *context, const char *file, const sh_dt_counts *counts)
{
    (void)file;
    *(sh_dt_counts *)context = *counts;
}

/* The wide path of a file under the folder. */
static wchar_t *mb_file_wpath(const wchar_t *root, const char *path)
{
    wchar_t *w, *full, *q;
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wn <= 0 || !(w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wn))) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, wn);
    for (q = w; *q; q++) if (*q == L'/') *q = L'\\';
    full = mb_wjoin(root, w);
    free(w);
    return full;
}

/* Derived edits: every decl the mod carries at a game
 * name with other bytes is an edited copy. Its structural diff against the
 * game's text of the same decl is recorded (b->edits); the product applies
 * it per map and never serves the copy. A copy or a game text outside the
 * decl grammar is refused. Then a partial a retired intent key names. */
/* The edited copies (sh_mb_file.edited): each REPLACE file the bundle's edits
 * name moves from classes.replaces to classes.edits. After mb_classify_all,
 * once the edits are derived (a build) or read (a load). */
static void mb_mark_edited_cb(void *context, const char *file, const sh_dt_counts *counts)
{
    sh_mb_bundle *b = (sh_mb_bundle *)context;
    size_t i;
    (void)counts;
    for (i = 0; file && i < b->file_count; i++) {
        sh_mb_file *f = &b->files[i];
        if (f->cls != SH_MB_CLASS_REPLACE || f->edited || _stricmp(f->request, file)) continue;
        f->edited = 1;
        if (b->classes.replaces) b->classes.replaces--;
        b->classes.edits++;
    }
}

static void mb_mark_edited(sh_mb_bundle *b)
{
    if (b->edits) (void)sh_dt_edits_files(b->edits, strlen(b->edits), mb_mark_edited_cb, b);
}

static void mb_derive_edits(sh_mb_bundle *b, const wchar_t *root)
{
    mb_buf o = {0};
    size_t i;
    int received = g_mb_keep_here;
    free(b->edits);
    b->edits = NULL;
    b->edit_files = 0;
    for (i = 0; i < b->file_count; i++) {
        sh_mb_file *f = &b->files[i];
        unsigned char *copy = NULL, *stock = NULL;
        size_t clen = 0, slen = 0;
        unsigned long long size = 0;
        char sha[SH_MB_HASH_HEX + 1], cause[SH_MB_CAUSE_MAX];
        wchar_t *full;
        sh_dt_counts counts;
        char *json;
        free(f->edit);
        f->edit = NULL;
        if (f->shadowed && !f->stock && !f->is_decl && !f->is_module && !received && mb_decl_text(f)) {
            /* modserve generates edited game files under generated/decls/
             * only (ms_registry_files): an edit here would build and never
             * apply. */
            mb_error(b, "%s edits the game's render-program permutation %s, which the product cannot generate per map "
                     "(it generates edited decls under generated/decls/ only, and a permutation is opened with its "
                     "program's cooked .bin and SPIR-V); delete your copy and the mod borrows the game's, or ship your "
                     "program whole under mods/%s/ (decl, .bin, SPIR-V and permutations) and point your material at it",
                     f->path, f->request, b->id);
            continue;
        }
        if (!f->shadowed || !f->is_decl || f->stock || f->is_module || sh_mb_editor_time(f->request)) continue;
        full = mb_file_wpath(root, f->path);
        copy = full ? mb_read_wfile(full, SH_MB_MAX_FILE, &clen) : NULL;
        free(full);
        if (!copy) { mb_error(b, "cannot read %s", f->path); continue; }
        if (sh_mb_stock_identify(f->request, sha, &size) != SH_MB_STOCK_OK ||
            sh_mb_stock_read(f->request, sha, &stock, &slen, f->path, cause, sizeof cause) != SH_MB_STOCK_OK) {
            if (received)
                mb_note(b, "replaces %s: the game's text cannot be read here, so a folder the product received keeps it "
                        "as a replacement", f->request);
            else
                mb_error(b, "%s: the game's %s cannot be read, so the edits this copy makes cannot be derived", f->path,
                         f->request);
            free(copy);
            continue;
        }
        json = sh_dt_diff(f->request, (const char *)stock, slen, (const char *)copy, clen, &counts, cause, sizeof cause);
        HeapFree(GetProcessHeap(), 0, stock);
        free(copy);
        if (!json && received) {
            mb_note(b, "replaces %s: it does not parse as a decl (%s); a folder the product received keeps it as a "
                    "replacement", f->request, cause);
            continue;
        }
        if (!json) {
            if (!strncmp(cause, "the game's text", 15))
                mb_error(b, "%s: %s, so the edits this copy makes cannot be derived", f->path, cause);
            else
                mb_error(b, "%s does not parse as a decl (%s); the product derives the edits from the whole edited copy "
                         "of %s, so the copy must be a whole decl", f->path, cause, f->request);
            continue;
        }
        f->edit = json;
        mb_put(&o, b->edit_files ? "," : "[", 1);
        mb_puts(&o, json);
        b->edit_files++;
    }
    if (b->edit_files) {
        mb_put(&o, "]", 1);
        if (o.oom) { free(o.p); mb_error(b, "out of memory"); }
        else b->edits = o.p;
    }
    mb_mark_edited(b);
}

/* The build's report lines about the files (sh_mb_bundle.notes). */
static void mb_report(sh_mb_bundle *b)
{
    size_t i;
    unsigned declared = 0;
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        if (f->cls == SH_MB_CLASS_REPLACE && f->edit) {
            /* an edited copy: its diff is applied per map, the copy never served */
            sh_dt_counts c;
            int ops;
            size_t el = strlen(f->edit);
            char *arr = (char *)malloc(el + 3);
            memset(&c, 0, sizeof c);
            if (arr) {
                arr[0] = '[';
                memcpy(arr + 1, f->edit, el);
                arr[el + 1] = ']';
                arr[el + 2] = 0;
                (void)sh_dt_edits_files(arr, el + 2, mb_counts_cb, &c);
                free(arr);
            }
            ops = sh_dt_counts_total(&c);
            mb_note(b, "edits %s: %d operation%s (%d set, %d removed, %d list entr%s added, %d keyed, %d bag entr%s added, "
                    "%d entr%s removed); generated per map from the game's text, the copy is never served", f->request, ops,
                    ops == 1 ? "" : "s", c.set, c.remove, c.list, c.list == 1 ? "y" : "ies", c.keyed, c.bag,
                    c.bag == 1 ? "y" : "ies", c.drop, c.drop == 1 ? "y" : "ies");
            declared++;
        } else if (f->cls == SH_MB_CLASS_REPLACE) mb_note(b, "replaces %s (a folder the product received)", f->request);
    }
    if (b->classes.surfaces) mb_note(b, "surfaces %u game file%s", b->classes.surfaces, b->classes.surfaces == 1 ? "" : "s");
    if (b->edit_files)
        mb_note(b, "edits %u game file%s: maps that declare this mod get the game's text with these edits", b->edit_files,
                b->edit_files == 1 ? "" : "s");
    (void)declared;
}

/* -------------------------------------------- campaign-only closure ---- */
/* SnapMap's session loads snap_gameresources, never the campaign's
 * gameresources, so a served decl naming content only the campaign archive
 * holds leaves a hole the engine logs and plays on with (a med_station animWeb
 * whose wallhealthstation.md6 is missing, a model whose skeleton is not
 * there, an effect whose particle is not there, a panel without its movie).
 * Every such name becomes a pointer of the bundle at the game's own path (a
 * surface: served from the player's disk, never carried or uploaded), and a
 * decl brought along this way is scanned for its own needs until nothing
 * campaign-only is left. A movie brings its atlas (generated/swf/<stem>.bimage)
 * with it; an image no archive holds as a standalone image may be a
 * transparency or decal atlas of that name; an image a virtual-textured
 * material names in a source field is sampled from the installed
 * virtual-texture pages when the material has a row there, so it needs
 * nothing. The set is derived from this build's needs alone: the product
 * block marks these pointers ("closure": true) and the next build drops them
 * and derives them again. Bounded by the file count and the manifest size;
 * the first name past either fails the build. */

static void mb_emit_file(mb_buf *o, const sh_mb_file *f);
static char *mb_emit(const sh_mb_bundle *b, int with_bundle, size_t *out_len);

typedef struct mb_closure {
    sh_mb_bundle *b;
    const sh_mb_resolver *r;
    const sh_mb_stock_ops *ops;
    const wchar_t *root;
    size_t *by_req;        /* file indices sorted by request */
    size_t n;
    size_t manifest;       /* canonical bytes so far (estimated before the hash and number) */
    unsigned borrowed;     /* files this build added */
    const char *first;     /* the first of them, for the build note */
    int stopped;
    char **vt_rows;        /* material names of the installed virtual-texture rows, sorted */
    size_t vt_n;
    int vt_state;          /* 0 not read, 1 read */
} mb_closure;

static size_t mb_closure_find(const mb_closure *c, const char *request, int *found)
{
    size_t lo = 0, hi = c->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int d = strcmp(c->b->files[c->by_req[mid]].request, request);
        if (d == 0) { *found = 1; return mid; }
        if (d < 0) lo = mid + 1; else hi = mid;
    }
    *found = 0;
    return lo;
}

/* The installed virtual-texture tables (virtualtextures/_vmtr.vmtr and
 * _vmtr_dlc<N>.vmtr, loose files at the install root): every quoted string
 * of a table is the material name of one row. Read once per build, and only
 * when a virtual-textured material's image would otherwise be added. */
static void mb_closure_vt_load(mb_closure *c)
{
    unsigned u;
    c->vt_state = 1;
    for (u = 0; u < 16; u++) {
        char src[64], sha[SH_MB_HASH_HEX + 1];
        unsigned long long size = 0;
        unsigned char *text = NULL;
        size_t len = 0, i;
        if (u == 0) strcpy_s(src, sizeof src, "virtualtextures/_vmtr.vmtr");
        else _snprintf_s(src, sizeof src, _TRUNCATE, "virtualtextures/_vmtr_dlc%u.vmtr", u);
        if (c->ops->identify(c->ops->context, src, sha, &size) != SH_MB_STOCK_OK) {
            if (u == 0) continue;
            break;
        }
        if (c->ops->read(c->ops->context, src, sha, &text, &len) != SH_MB_STOCK_OK) continue;
        for (i = 0; i < len; i++) {
            size_t s, n;
            char **grown, *name;
            if (text[i] != '"') continue;
            s = ++i;
            while (i < len && text[i] != '"' && text[i] != '\n') i++;
            n = i - s;
            if (!n || i >= len || text[i] != '"') continue;
            grown = (char **)realloc(c->vt_rows, sizeof(char *) * (c->vt_n + 1));
            if (grown) c->vt_rows = grown;
            name = grown ? (char *)malloc(n + 1) : NULL;
            if (!name) break;
            memcpy(name, text + s, n);
            name[n] = 0;
            mb_lower(name);
            c->vt_rows[c->vt_n++] = name;
        }
        HeapFree(GetProcessHeap(), 0, text);
    }
    if (c->vt_n > 1) qsort(c->vt_rows, c->vt_n, sizeof(char *), mb_str_cmp);
}

/* 1 when material `f` sets usevirtualmapping, names `image` in a
 * virtual-texture source field and has an installed virtual-texture row: the
 * engine samples the baked pages, not a standalone image. */
static int mb_closure_vt_served(mb_closure *c, const sh_mb_file *f, const char *image)
{
    static const char *const fields[] = { "diffusemap", "bumpmap", "powermap", "specularmap", "bloommask", "sssmap",
                                          "colormaskmap", NULL };
    unsigned char *text = NULL;
    size_t len = 0, pos = 0, in = strlen(image);
    int heap = 0, virt = 0, named = 0, after = 0, hit = 0;
    mb_tok t;
    if (!f->ptype || (strcmp(f->ptype, "material") && strcmp(f->ptype, "material2")) || !f->pname) return 0;
    if (f->stock) {
        char cause[SH_MB_CAUSE_MAX];
        if (!f->stock_source || sh_mb_stock_read(f->stock_source, f->blob, &text, &len, f->path, cause, sizeof cause) != SH_MB_STOCK_OK)
            return 0;
        heap = 1;
    } else {
        char *rel = mb_strdup(f->path), *p;
        wchar_t *wrel = NULL, *full = NULL;
        int wn;
        if (!rel || !c->root) { free(rel); return 0; }
        for (p = rel; *p; p++) if (*p == '/') *p = '\\';
        wn = MultiByteToWideChar(CP_UTF8, 0, rel, -1, NULL, 0);
        if (wn > 0 && (wrel = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wn)) != NULL)
            MultiByteToWideChar(CP_UTF8, 0, rel, -1, wrel, wn);
        free(rel);
        if (wrel) full = mb_wjoin(c->root, wrel);
        free(wrel);
        if (full) text = mb_read_wfile(full, SH_MB_MAX_FILE, &len);
        free(full);
        if (!text) return 0;
    }
    /* `key [=] value` at any depth: a source field naming the image, and
     * usevirtualmapping with a non-zero value. */
    for (;;) {
        t = mb_next_tok(text, len, &pos);
        if (t.kind == TK_END) break;
        if (t.kind == TK_EQ) continue;
        if (after == 1) {
            if ((t.kind == TK_WORD || t.kind == TK_STR) && t.n) {
                char num[32];
                _snprintf_s(num, sizeof num, _TRUNCATE, "%.*s", (int)t.n, t.p);
                virt = atof(num) != 0.0;
            }
            after = 0;
            continue;
        }
        if (after == 2) {
            if ((t.kind == TK_WORD || t.kind == TK_STR) && t.n == in && mb_ieq_n(t.p, t.n, image)) named = 1;
            after = 0;
            continue;
        }
        if (t.kind == TK_WORD && mb_ieq_n(t.p, t.n, "usevirtualmapping")) { after = 1; continue; }
        if (t.kind == TK_WORD) {
            int k;
            for (k = 0; fields[k]; k++)
                if (mb_ieq_n(t.p, t.n, fields[k])) { after = 2; break; }
        }
    }
    if (heap) HeapFree(GetProcessHeap(), 0, text); else free(text);
    if (!virt || !named) return 0;
    if (!c->vt_state) mb_closure_vt_load(c);
    if (c->vt_n) {
        char *name = mb_strdup(f->pname);
        if (!name) return 0;
        mb_lower(name);
        hit = bsearch(&name, c->vt_rows, c->vt_n, sizeof(char *), mb_str_cmp) != NULL;
        free(name);
    }
    return hit;
}

/* ----------------------------------------------------- whole models ---- */
/* A model is one unit: the engine loads an md6Def's mesh, then the skeleton
 * whose name the cooked mesh carries (a little-endian u32 length at 0x0C and
 * the name, "MM" at 2), then that skeleton's default clip
 * <skeleton>/_default.md6anim. An md6Def names only the mesh, so the builder
 * reads the skeleton's name from the mesh bytes (the bundle's own copy, else
 * the installed one SnapMap loads first) and records the skeleton and the
 * default clip as needs of the md6Def beside the mesh. Needs are names: a
 * kept md6Def keeps all three for the session (sh_modserve_holds_raise), and
 * only a name only the campaign archives hold becomes a pointer. A name
 * nothing holds is not recorded (the engine's own hole, as before). */

#define MB_MESH_PREFIX 4096u

/* The first bytes of a carried file of the bundle (malloc'd). */
static unsigned char *mb_read_prefix(const wchar_t *root, const char *path, size_t max, size_t *len)
{
    char *rel = mb_strdup(path), *p;
    wchar_t *wrel = NULL, *full = NULL;
    unsigned char *buf = NULL;
    HANDLE h;
    DWORD got = 0;
    int wn;
    *len = 0;
    if (!rel || !root) { free(rel); return NULL; }
    for (p = rel; *p; p++) if (*p == '/') *p = '\\';
    wn = MultiByteToWideChar(CP_UTF8, 0, rel, -1, NULL, 0);
    if (wn > 0 && (wrel = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wn)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, rel, -1, wrel, wn);
    free(rel);
    if (wrel) full = mb_wjoin(root, wrel);
    free(wrel);
    if (!full) return NULL;
    h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(full);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    buf = (unsigned char *)malloc(max);
    if (buf && (!ReadFile(h, buf, (DWORD)max, &got, NULL) || !got)) { free(buf); buf = NULL; }
    CloseHandle(h);
    if (buf) *len = got;
    return buf;
}

/* The skeleton name a cooked mesh carries, or 0. */
static int mb_mesh_skeleton(const unsigned char *d, size_t len, char *out, size_t cap)
{
    uint32_t n, i;
    if (len < 16 || d[2] != 'M' || d[3] != 'M') return 0;
    n = (uint32_t)d[12] | ((uint32_t)d[13] << 8) | ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);
    if (n < 8 || n >= cap || n > len - 16) return 0;
    for (i = 0; i < n; i++)
        if (d[16 + i] < 0x21 || d[16 + i] > 0x7e) return 0;
    memcpy(out, d + 16, n);
    out[n] = 0;
    mb_lower(out);
    return mb_ends(out, ".md6skl");
}

static const sh_mb_file *mb_file_at(const sh_mb_bundle *b, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++)
        if (b->files[i].request && strcmp(b->files[i].request, request) == 0) return &b->files[i];
    return NULL;
}

/* 1 when a need of (type, name) names something: a file of the bundle,
 * another installed bundle's, or an installed archive's. */
static int mb_need_held(const sh_mb_bundle *b, const sh_mb_resolver *r, const char *type, const char *name)
{
    const sh_mb_stock_ops *ops = mb_stock();
    char cand[2][SH_MB_REQUEST_MAX + 1], where[32];
    int n = sh_mb_need_requests(type, name, cand, 2), i;
    for (i = 0; i < n; i++) {
        if (mb_file_at(b, cand[i])) return 1;
        if (r && r->installed && r->installed(r->installed_context, cand[i]) == 1) return 1;
        if (ops && ops->container && ops->container(ops->context, cand[i], NULL, where, sizeof where) == SH_MB_STOCK_OK)
            return 1;
    }
    return 0;
}

static int mb_has_need(const sh_mb_file *f, const char *type, const char *name)
{
    size_t i;
    for (i = 0; i < f->need_count; i++)
        if (strcmp(f->needs[i].type, type) == 0 && _stricmp(f->needs[i].name, name) == 0) return 1;
    return 0;
}

/* Record the skeleton and the default clip of each mesh md6Def `file`
 * names. Returns 0 only when memory ran out. */
static int mb_model_needs(sh_mb_bundle *b, size_t file, const sh_mb_resolver *r, const wchar_t *root)
{
    const sh_mb_stock_ops *ops = mb_stock();
    size_t k, count;
    int added = 0;
    if (!b->files[file].ptype || strcmp(b->files[file].ptype, "md6Def") != 0) return 1;
    count = b->files[file].need_count;
    for (k = 0; k < count; k++) {
        char mesh[SH_MB_REQUEST_MAX + 1], skl[SH_MB_REQUEST_MAX + 1], clip[SH_MB_REQUEST_MAX + 32];
        char cand[1][SH_MB_REQUEST_MAX + 1];
        const sh_mb_need *need = &b->files[file].needs[k];
        const sh_mb_file *own;
        unsigned char *bytes = NULL;
        size_t len = 0;
        int heap = 0, got;
        if (strcmp(need->type, "baseModel") != 0 || sh_mb_need_requests("baseModel", need->name, cand, 1) != 1) continue;
        strcpy_s(mesh, sizeof mesh, cand[0]);
        own = mb_file_at(b, mesh);
        if (own && own->stock) {
            char cause[SH_MB_CAUSE_MAX];
            if (own->stock_source &&
                sh_mb_stock_read(own->stock_source, own->blob, &bytes, &len, own->path, cause, sizeof cause) == SH_MB_STOCK_OK)
                heap = 1;
        } else if (own) {
            bytes = mb_read_prefix(root, own->path, MB_MESH_PREFIX, &len);
        } else if (ops && ops->identify && ops->read) {
            char sha[SH_MB_HASH_HEX + 1];
            unsigned long long size = 0;
            if (ops->identify(ops->context, mesh, sha, &size) == SH_MB_STOCK_OK &&
                ops->read(ops->context, mesh, sha, &bytes, &len) == SH_MB_STOCK_OK)
                heap = 1;
        }
        if (!bytes) continue;
        got = mb_mesh_skeleton(bytes, len, skl, sizeof skl);
        if (heap) HeapFree(GetProcessHeap(), 0, bytes); else free(bytes);
        if (!got) continue;
        _snprintf_s(clip, sizeof clip, _TRUNCATE, "%s/_default.md6anim", skl);
        if (!mb_has_need(&b->files[file], "skeleton", skl) && mb_need_held(b, r, "skeleton", skl)) {
            if (!mb_add_need(&b->files[file], "skeleton", skl, strlen(skl))) return 0;
            added = 1;
        }
        if (!mb_has_need(&b->files[file], "anim", clip) && mb_need_held(b, r, "anim", clip)) {
            if (!mb_add_need(&b->files[file], "anim", clip, strlen(clip))) return 0;
            added = 1;
        }
    }
    if (added) qsort(b->files[file].needs, b->files[file].need_count, sizeof(sh_mb_need), mb_need_cmp);
    return 1;
}

/* Add `request` (installed only in gameresources) as a surface pointer at
 * sorted position `at`; returns 0 when the walk must stop. */
static int mb_closure_add(mb_closure *c, size_t at, const char *request, const sh_mb_need *need, const char *by)
{
    sh_mb_bundle *b = c->b;
    sh_mb_file *f;
    char sha[SH_MB_HASH_HEX + 1], type[64], name[SH_MB_REQUEST_MAX + 16], cause[SH_MB_CAUSE_MAX];
    unsigned long long size = 0;
    size_t idx, n = strlen(request) + 8;
    mb_buf one = {0};
    if (c->ops->identify(c->ops->context, request, sha, &size) != SH_MB_STOCK_OK || strlen(sha) != SH_MB_HASH_HEX ||
        !sh_mb_logical_name(request, type, sizeof type, name, sizeof name))
        return 1;   /* left to the need check, which reports it as before */
    if (b->file_count >= SH_MB_MAX_FILES) {
        mb_error(b, "%s needs %s %s, which only the campaign holds; bringing it along would take this mod past %u "
                 "files, so split the mod into two", by, need->type, need->name, SH_MB_MAX_FILES);
        return 0;
    }
    f = mb_add_file(b);
    if (!f) { mb_error(b, "out of memory"); return 0; }
    idx = b->file_count - 1;
    f->path = (char *)malloc(n);
    f->request = mb_strdup(request);
    f->stock_source = mb_strdup(request);
    f->ptype = mb_strdup(type);
    f->pname = mb_strdup(name);
    if (!f->path || !f->request || !f->stock_source || !f->ptype || !f->pname) { mb_error(b, "out of memory"); return 0; }
    _snprintf_s(f->path, n, _TRUNCATE, "assets/%s", request);
    memcpy(f->blob, sha, sizeof sha);
    f->size = size;
    f->stock = 1;
    f->absent = 1;
    f->closure = 1;
    f->is_decl = mb_starts(request, "generated/decls/");
    if (f->is_decl) {
        unsigned char *text = NULL;
        size_t len = 0;
        if (sh_mb_stock_read(request, sha, &text, &len, f->path, cause, sizeof cause) == SH_MB_STOCK_OK) {
            if (sh_decl_text_well_formed(text, len)) {
                mb_need_ctx nc = { f, 0 };
                sh_mb_scan_needs(f->ptype, text, len, mb_need_cb, &nc);
                if (nc.oom) { HeapFree(GetProcessHeap(), 0, text); mb_error(b, "out of memory"); return 0; }
                qsort(f->needs, f->need_count, sizeof(sh_mb_need), mb_need_cmp);
            }
            HeapFree(GetProcessHeap(), 0, text);
        }
        if (!mb_model_needs(b, idx, c->r, c->root)) { mb_error(b, "out of memory"); return 0; }
        f = &b->files[idx];
    }
    mb_emit_file(&one, f);
    if (one.oom) { free(one.p); mb_error(b, "out of memory"); return 0; }
    c->manifest += one.n + 1;
    free(one.p);
    if (c->manifest > SH_MB_MANIFEST_MAX) {
        mb_error(b, "%s needs %s %s, which only the campaign holds; this mod already brings so much campaign content "
                 "that its file list would pass 1 MiB, so split the mod into two", by, need->type, need->name);
        return 0;
    }
    memmove(c->by_req + at + 1, c->by_req + at, sizeof(size_t) * (c->n - at));
    c->by_req[at] = idx;
    c->n++;
    c->borrowed++;
    if (!c->first) c->first = f->request;
    return 1;
}

/* One need of file `file`, met by any of the `n` (at most 2) candidate
 * requests. Returns 1 when it is settled (in the bundle, loaded by SnapMap on
 * its own, or added now), 0 when no archive holds any candidate. Another
 * installed mod pointing at the same campaign file does not settle it: that
 * mod is served only where a map declares it, so this bundle carries its own
 * pointer. */
static int mb_closure_offer(mb_closure *c, size_t file, const sh_mb_need *need,
                            char cand[][SH_MB_REQUEST_MAX + 1], int n)
{
    sh_mb_bundle *b = c->b;
    char by[SH_MB_REQUEST_MAX + 16];
    size_t at[2] = { 0, 0 };
    int i, found, campaign = -1;
    for (i = 0; i < n; i++) {
        size_t pos = mb_closure_find(c, cand[i], &found);
        if (found) return 1;
        at[i] = pos;
    }
    for (i = 0; i < n; i++) {
        char where[32];
        if (c->ops->container(c->ops->context, cand[i], NULL, where, sizeof where) != SH_MB_STOCK_OK) continue;
        if (strcmp(where, "gameresources") != 0) return 1;   /* SnapMap loads it on its own */
        if (campaign < 0) campaign = i;
    }
    if (campaign < 0) return 0;
    if (strcmp(need->type, "image") == 0 && mb_closure_vt_served(c, &b->files[file], need->name)) return 1;
    strncpy_s(by, sizeof by, b->files[file].path, _TRUNCATE);
    if (!mb_closure_add(c, at[campaign], cand[campaign], need, by)) c->stopped = 1;
    return 1;
}

/* One member of a program, by its request path. */
static int mb_closure_member(mb_closure *c, size_t file, const char *request)
{
    char cand[1][SH_MB_REQUEST_MAX + 1];
    sh_mb_need need;
    if (strlen(request) > SH_MB_REQUEST_MAX) return 0;
    memcpy(cand[0], request, strlen(request) + 1);
    need.type = "renderProg";
    need.name = cand[0];
    return mb_closure_offer(c, file, &need, cand, 1);
}

static void mb_closure_missing(mb_closure *c, size_t file, const char *program, const char *member)
{
    mb_error(c->b, "%s uses the campaign render program %s, but no archive holds its %s; a program is served whole or "
             "not at all, since the game stops when it binds a missing one (check the game's files in Steam)",
             c->b->files[file].path, program, member);
    c->stopped = 1;
}

/* A program unit (the program or one permutation): its cooked Vulkan .bin
 * (required), an OpenGL .bin and the SPIR-V stages the archive holds. Which
 * stages a unit needs is checked afterwards from the .bin itself
 * (mb_check_renderprogs). */
static void mb_closure_unit(mb_closure *c, size_t file, const char *program, const char *stem, const char *spirv_name)
{
    static const char *const ext[3] = { ".vspv", ".fspv", ".cspv" };
    char req[SH_MB_REQUEST_MAX + 32];
    int k;
    _snprintf_s(req, sizeof req, _TRUNCATE, "%s_pc_vulkan.bin", stem);
    if (!mb_closure_member(c, file, req)) {
        if (!c->stopped) mb_closure_missing(c, file, program, req);
        return;
    }
    if (c->stopped) return;
    _snprintf_s(req, sizeof req, _TRUNCATE, "%s_pc.bin", stem);
    (void)mb_closure_member(c, file, req);
    for (k = 0; k < 3 && !c->stopped; k++)
        if (sh_mb_spirv_request(spirv_name, ext[k], req, sizeof req)) (void)mb_closure_member(c, file, req);
}

typedef struct mb_perm_list {
    const char *program;
    char (*req)[SH_MB_REQUEST_MAX + 1];
    size_t n, cap;
    int oom;
} mb_perm_list;

static void mb_perm_cb(void *context, const char *path)
{
    mb_perm_list *l = (mb_perm_list *)context;
    char program[SH_MB_REQUEST_MAX + 1];
    size_t i;
    if (l->oom || strlen(path) > SH_MB_REQUEST_MAX || !sh_mb_rp_permutation(path, program, sizeof program, NULL) ||
        strcmp(program, l->program) != 0)
        return;
    for (i = 0; i < l->n; i++) if (!strcmp(l->req[i], path)) return;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        void *g = realloc(l->req, cap * sizeof *l->req);
        if (!g) { l->oom = 1; return; }
        l->req = (char (*)[SH_MB_REQUEST_MAX + 1])g;
        l->cap = cap;
    }
    memcpy(l->req[l->n++], path, strlen(path) + 1);
}

/* A material's program is one unit with its files (archive_renderprog.py's
 * closure, reproduced for all 923 programs of 13954591): the decl, the cooked
 * .bin and SPIR-V, and every permutation's decl, .bin and SPIR-V, at the
 * paths the archive stores (split SPIR-V names included; the shared includes
 * under decls/renderprogs/ never). Shipping a program without its permutation
 * files is fatal (0xE06D7363 when the permutation open fails), so a
 * campaign-only program comes whole or the build fails naming the first
 * missing member. SnapMap's own programs add nothing. */
static void mb_closure_program(mb_closure *c, size_t file, const char *name)
{
    char program[SH_MB_REQUEST_MAX + 1], decl[SH_MB_REQUEST_MAX + 48], stem[SH_MB_REQUEST_MAX + 48], where[32];
    mb_perm_list perms;
    size_t i, n = strlen(name);
    if (n == 0 || n > SH_MB_REQUEST_MAX - 40 || (n > 5 && mb_ieq_n(name, 5, "mods/"))) return;
    memcpy(program, name, n + 1);
    mb_lower(program);
    _snprintf_s(decl, sizeof decl, _TRUNCATE, "generated/decls/renderprog/%s.decl", program);
    if (c->ops->container(c->ops->context, decl, NULL, where, sizeof where) != SH_MB_STOCK_OK ||
        strcmp(where, "gameresources") != 0 || !c->ops->list)
        return;
    _snprintf_s(stem, sizeof stem, _TRUNCATE, "generated/renderprogs/%s", program);
    mb_closure_unit(c, file, program, stem, program);
    if (c->stopped) return;
    memset(&perms, 0, sizeof perms);
    perms.program = program;
    _snprintf_s(stem, sizeof stem, _TRUNCATE, "generated/renderprogs/permutations/%s_", program);
    if (c->ops->list(c->ops->context, stem, mb_perm_cb, &perms) != SH_MB_STOCK_OK) {
        mb_error(c->b, "%s uses render program %s, and Snapmap+ could not read the game's files to find its parts; "
                 "start DOOM normally and press Refresh", c->b->files[file].path, program);
        c->stopped = 1;
    } else if (perms.oom) {
        mb_error(c->b, "out of memory");
        c->stopped = 1;
    } else if (!perms.n) {
        char member[SH_MB_REQUEST_MAX + 80];
        _snprintf_s(member, sizeof member, _TRUNCATE, "permutation (generated/renderprogs/permutations/%s_<16 hex "
                    "digits>.decl)", program);
        mb_closure_missing(c, file, program, member);
    }
    for (i = 0; i < perms.n && !c->stopped; i++) {
        if (!mb_closure_member(c, file, perms.req[i]) || c->stopped) continue;
        _snprintf_s(stem, sizeof stem, _TRUNCATE, "%.*s", (int)(strlen(perms.req[i]) - 5), perms.req[i]);
        mb_closure_unit(c, file, program, stem, perms.req[i]);
    }
    free(perms.req);
}

static void mb_closure_need(mb_closure *c, size_t file, size_t k)
{
    char cand[2][SH_MB_REQUEST_MAX + 1];
    sh_mb_need need = c->b->files[file].needs[k];
    int n = sh_mb_need_requests(need.type, need.name, cand, 2), held;
    if (n <= 0) return;
    held = mb_closure_offer(c, file, &need, cand, n);
    if (c->stopped) return;
    if (_stricmp(need.type, "renderProg") == 0) {
        char name[SH_MB_REQUEST_MAX + 1];
        strncpy_s(name, sizeof name, need.name, _TRUNCATE);   /* the files array may move while the closure grows */
        mb_closure_program(c, file, name);
    } else if (strcmp(need.type, "swf") == 0) {
        /* The movie's atlas is its sibling: generated/swf/<stem>.bimage. */
        size_t m = strlen(cand[0]);
        if (m > 5 && m + 3 <= SH_MB_REQUEST_MAX && !strcmp(cand[0] + m - 5, ".bswf")) {
            memcpy(cand[0] + m - 5, ".bimage", 8);
            mb_closure_offer(c, file, &need, cand, 1);
        }
    } else if (!held && strcmp(need.type, "image") == 0) {
        /* No standalone image: a transparency or decal atlas of that name. */
        if (mb_binary_name("generated", "transsortatlas", need.name, "bimage", cand[0], SH_MB_REQUEST_MAX + 1) &&
            mb_binary_name("generated", "decalatlas", need.name, "bimage", cand[1], SH_MB_REQUEST_MAX + 1))
            mb_closure_offer(c, file, &need, cand, 2);
    }
}

static void mb_close_campaign(sh_mb_bundle *b, const sh_mb_resolver *r, const wchar_t *root)
{
    mb_closure c;
    size_t i, k, len = 0;
    char *text;
    memset(&c, 0, sizeof c);
    c.b = b;
    c.r = r;
    c.root = root;
    c.ops = mb_stock();
    if (!c.ops || !c.ops->container || !c.ops->identify || !c.ops->read) return;
    c.by_req = (size_t *)malloc(sizeof(size_t) * (SH_MB_MAX_FILES + 1));
    if (!c.by_req) { mb_error(b, "out of memory"); return; }
    for (i = 0; i < b->file_count && i < SH_MB_MAX_FILES; i++) c.by_req[c.n++] = i;
    for (i = 1; i < c.n; i++) {
        size_t v = c.by_req[i], j = i;
        while (j > 0 && strcmp(b->files[c.by_req[j - 1]].request, b->files[v].request) > 0) { c.by_req[j] = c.by_req[j - 1]; j--; }
        c.by_req[j] = v;
    }
    text = mb_emit(b, 1, &len);
    c.manifest = len + SH_MB_HASH_HEX + 20;
    free(text);
    /* The bundle grows while it is walked: every file added is walked too. */
    for (i = 0; i < b->file_count && !c.stopped; i++)
        for (k = 0; k < b->files[i].need_count && !c.stopped; k++) mb_closure_need(&c, i, k);
    free(c.by_req);
    for (i = 0; i < c.vt_n; i++) free(c.vt_rows[i]);
    free(c.vt_rows);
    if (c.borrowed)
        mb_note(b, "%u game file%s borrowed for completeness: campaign content a served decl needs, which SnapMap does "
                "not load on its own (first %s)", c.borrowed, c.borrowed == 1 ? "" : "s", c.first);
    qsort(b->files, b->file_count, sizeof(sh_mb_file), mb_file_cmp);
}

/* ------------------------------------------------- scripts at build ---- */

/* The event words of on.<event>(name, ...) that bind an entityDef, in the
 * order the builder block lists them. */
static const char *const mb_script_events[] = { "spawn", "think", "remove", "damage", "death", "anim", "pain",
                                                 "projectile", "frame_event", NULL };

typedef struct mb_script_use {
    int what;              /* SH_MB_SCRIPT_BIND .. SH_MB_SCRIPT_REQUIRE */
    unsigned events;       /* BIND: the bit of its event in mb_script_events */
    char *name;            /* as written */
    char *file;            /* the script's name as an author sees it (sh_mb_script_file_name) */
    int line;
} mb_script_use;

typedef struct mb_scripts {
    mb_script_use *u;
    size_t n, cap;
    char cur[SH_MB_REQUEST_MAX + 16];   /* the file being scanned */
    unsigned files, dynamic;
    unsigned long long bytes;
    int node;              /* a script registers an input: the mod gets its Script node row */
    int oom;
} mb_scripts;

static void mb_scripts_free(mb_scripts *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) { free(s->u[i].name); free(s->u[i].file); }
    free(s->u);
    memset(s, 0, sizeof *s);
}

static void mb_script_use_cb(void *context, int what, const char *event, const char *name, size_t len, int line)
{
    mb_scripts *s = (mb_scripts *)context;
    mb_script_use *u;
    unsigned k;
    if (what & SH_MB_SCRIPT_DYNAMIC) {
        /* A module chosen while running is the runtime's to check. */
        if ((what & 0xff) != SH_MB_SCRIPT_REQUIRE) s->dynamic++;
        return;
    }
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 32;
        mb_script_use *grown = (mb_script_use *)realloc(s->u, cap * sizeof *grown);
        if (!grown) { s->oom = 1; return; }
        s->u = grown;
        s->cap = cap;
    }
    u = &s->u[s->n];
    memset(u, 0, sizeof *u);
    u->what = what;
    for (k = 0; what == SH_MB_SCRIPT_BIND && mb_script_events[k]; k++)
        if (!strcmp(event, mb_script_events[k])) u->events = 1u << k;
    u->name = mb_strndup(name, len);
    u->file = mb_strdup(s->cur);
    if (!u->name || !u->file) { free(u->name); free(u->file); s->oom = 1; return; }
    u->line = line;
    s->n++;
}

/* Well-formed UTF-8 (no overlong form, surrogate or code point past U+10FFFF). */
static int mb_utf8_ok(const unsigned char *t, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = t[i];
        size_t k, more;
        unsigned long cp;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { more = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { more = 2; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { more = 3; cp = c & 0x07u; }
        else return 0;
        if (i + more >= n) return 0;
        for (k = 1; k <= more; k++) {
            if ((t[i + k] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (t[i + k] & 0x3Fu);
        }
        if ((more == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (more == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            return 0;
        i += more + 1;
    }
    return 1;
}

/* A script message: "scripts\x.lua line N: what. Todo." (without the line
 * when there is none), as the runtime's stop line words a file and line. */
static void mb_script_error(sh_mb_bundle *b, const char *file, int line, const char *what, const char *todo)
{
    char where[SH_MB_REQUEST_MAX + 32];
    char first[8];
    if (line > 0) _snprintf_s(where, sizeof where, _TRUNCATE, "%s line %d", file, line);
    else _snprintf_s(where, sizeof where, _TRUNCATE, "%s", file);
    if (todo && todo[0]) {
        first[0] = (char)toupper((unsigned char)todo[0]);
        first[1] = 0;
        mb_error(b, "%s: %s. %s%s.", where, what, first, todo + 1);
    } else {
        mb_error(b, "%s: %s.", where, what);
    }
}

/* One script's text: the checks of modbundle.h, the compile check, then the
 * static pass (only for a file that compiles). */
static void mb_script_file(sh_mb_bundle *b, size_t fi, const unsigned char *text, size_t len, mb_scripts *s)
{
    const sh_mb_file *f = &b->files[fi];
    sh_mb_script_check_fn check = g_mb_script_check;
    char file[SH_MB_REQUEST_MAX + 16], what[400], todo[200];
    int line = 0;
    sh_mb_script_file_name(f->request, file, sizeof file);
    if (len >= 4 && !memcmp(text, "\x1bLua", 4)) {
        mb_script_error(b, file, 0, "the file is compiled Lua, not script text", "put the .lua source text in the file");
        return;
    }
    if (memchr(text, 0, len) || !mb_utf8_ok(text, len)) {
        mb_script_error(b, file, 0, "the file is not UTF-8 text", "save it as UTF-8 text");
        return;
    }
    if (!check) {
        mb_script_error(b, file, 0, "the file cannot be checked by this program", "build the mod in Snapmap+");
        return;
    }
    what[0] = todo[0] = 0;
    if (!check(file, sh_mb_script_request(f->request, NULL, NULL) == SH_MB_SCRIPT_ENTRY, (const char *)text, len,
               &line, what, sizeof what, todo, sizeof todo)) {
        mb_script_error(b, file, line, what[0] ? what : "does not compile", todo);
        return;
    }
    strcpy_s(s->cur, sizeof s->cur, file);
    sh_mb_script_scan((const char *)text, len, mb_script_use_cb, s);
    if (sh_script_node_registers_input((const char *)text, len)) s->node = 1;
}

/* A use's name with this mod's own "mods/<salt>/" taken off: 1 with the
 * mod-relative name in out; 0 when it names another mod's namespace. */
static int mb_script_own_name(const sh_mb_bundle *b, const char *name, char *out, size_t cap)
{
    size_t sl = strlen(b->id);
    if (_strnicmp(name, "mods/", 5) != 0) { strncpy_s(out, cap, name, _TRUNCATE); return 1; }
    if (_strnicmp(name + 5, b->id, sl) != 0 || name[5 + sl] != '/' || !name[6 + sl]) return 0;
    strncpy_s(out, cap, name + 6 + sl, _TRUNCATE);
    return 1;
}

static int mb_script_name_cmp(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int c = _stricmp(x, y);
    return c ? c : strcmp(x, y);
}

/* Sorted, one of each spelling without case, as a JSON array member. */
static void mb_script_put_names(mb_buf *o, const char *key, char **names, size_t n)
{
    size_t i;
    int first = 1;
    if (!n) return;
    qsort(names, n, sizeof(char *), mb_script_name_cmp);
    mb_puts(o, ",\"");
    mb_puts(o, key);
    mb_puts(o, "\":[");
    for (i = 0; i < n; i++) {
        if (i && !_stricmp(names[i - 1], names[i])) continue;
        if (!first) mb_put(o, ",", 1);
        first = 0;
        mb_put_jcstr(o, names[i]);
    }
    mb_put(o, "]", 1);
}

static int mb_script_entity_owned(const sh_mb_bundle *b, const char *rel)
{
    char full[SH_MB_REQUEST_MAX + 1];
    size_t i;
    _snprintf_s(full, sizeof full, _TRUNCATE, "mods/%s/%s", b->id, rel);
    for (i = 0; i < b->file_count; i++)
        if (b->files[i].ptype && b->files[i].pname && !_stricmp(b->files[i].ptype, "entityDef") &&
            !_stricmp(b->files[i].pname, full)) return 1;
    return 0;
}

/* The mod's scripts as one: the per-mod caps, each literal require and name
 * checked against the mod's own files, and the builder block's "scripts". */
static void mb_scripts_finish(sh_mb_bundle *b, const mb_req_index *own, mb_scripts *s)
{
    char **ent = NULL, **in = NULL, **outn = NULL;
    unsigned *ev = NULL;
    size_t nent = 0, nin = 0, nout = 0, i, k;
    mb_buf o = {0};
    char num[64];
    b->script_files = s->files;
    if (!s->files) return;
    if (s->oom) { mb_error(b, "out of memory"); return; }
    ent = (char **)calloc(s->n + 1, sizeof(char *));
    ev = (unsigned *)calloc(s->n + 1, sizeof(unsigned));
    in = (char **)calloc(s->n + 1, sizeof(char *));
    outn = (char **)calloc(s->n + 1, sizeof(char *));
    if (!ent || !ev || !in || !outn) { mb_error(b, "out of memory"); goto done; }
    for (i = 0; i < s->n; i++) {
        const mb_script_use *u = &s->u[i];
        const char *file = u->file;
        char rel[SH_MB_REQUEST_MAX + 1], what[SH_MB_CAUSE_MAX];
        if (u->what == SH_MB_SCRIPT_REQUIRE) {
            size_t n = strlen(u->name), c;
            int ok = n > 0;
            for (c = 0; ok && c < n; c++) ok = mb_script_name_char(u->name[c]);
            if (!ok) {
                _snprintf_s(what, sizeof what, _TRUNCATE, "\"require\" was given \"%s\", which is not a file of this "
                            "mod's scripts\\lib", u->name);
                mb_script_error(b, file, u->line, what, "use the file's name in scripts\\lib without folders or "
                                "\".lua\", as require(\"fsm\") for scripts\\lib\\fsm.lua; a script loads only its own "
                                "mod's files");
                continue;
            }
            _snprintf_s(rel, sizeof rel, _TRUNCATE, "mods/%s/scripts/lib/%s.lua", b->id, u->name);
            mb_lower(rel);
            if (!mb_req_has(own, rel)) {
                _snprintf_s(what, sizeof what, _TRUNCATE, "\"require\" found no scripts\\lib\\%s.lua in this mod",
                            u->name);
                mb_script_error(b, file, u->line, what, "add the file to scripts\\lib or correct the name; a script "
                                "loads only its own mod's files");
            }
            continue;
        }
        if (!mb_script_own_name(b, u->name, rel, sizeof rel)) {
            _snprintf_s(what, sizeof what, _TRUNCATE, "\"%s\" is another mod's name", u->name);
            mb_script_error(b, file, u->line, what, "name this mod's own content by its path inside the mod, as \"ai/imp\"");
            continue;
        }
        if (u->what == SH_MB_SCRIPT_BIND) {
            for (k = 0; k < nent && _stricmp(ent[k], rel); k++) {}
            if (k == nent) {
                if (!mb_script_entity_owned(b, rel)) {
                    const char *word = "spawn";
                    unsigned e;
                    for (e = 0; mb_script_events[e]; e++) if (u->events == 1u << e) word = mb_script_events[e];
                    mb_warn(b, "%s line %d: this mod has no entityDef \"%s\" for on.%s; check the name", file, u->line,
                            rel, word);
                }
                if (!(ent[nent] = mb_strdup(rel))) { mb_error(b, "out of memory"); goto done; }
                nent++;
            }
            ev[k] |= u->events;
        } else if (u->what == SH_MB_SCRIPT_INPUT) {
            if (!(in[nin++] = mb_strdup(rel))) { mb_error(b, "out of memory"); goto done; }
        } else if (u->what == SH_MB_SCRIPT_OUTPUT) {
            if (!(outn[nout++] = mb_strdup(rel))) { mb_error(b, "out of memory"); goto done; }
        }
    }
    _snprintf_s(num, sizeof num, _TRUNCATE, "{\"files\":%u", s->files);
    mb_puts(&o, num);
    if (nent) {
        /* Sort the names with their events. */
        for (i = 1; i < nent; i++) {
            char *nm = ent[i];
            unsigned e = ev[i];
            k = i;
            while (k > 0 && mb_script_name_cmp(&ent[k - 1], &nm) > 0) { ent[k] = ent[k - 1]; ev[k] = ev[k - 1]; k--; }
            ent[k] = nm;
            ev[k] = e;
        }
        mb_puts(&o, ",\"entities\":[");
        for (i = 0; i < nent; i++) {
            unsigned e;
            int first = 1;
            if (i) mb_put(&o, ",", 1);
            mb_puts(&o, "{\"name\":");
            mb_put_jcstr(&o, ent[i]);
            mb_puts(&o, ",\"events\":[");
            for (e = 0; mb_script_events[e]; e++) {
                if (!(ev[i] & (1u << e))) continue;
                if (!first) mb_put(&o, ",", 1);
                first = 0;
                mb_put_jcstr(&o, mb_script_events[e]);
            }
            mb_puts(&o, "]}");
        }
        mb_put(&o, "]", 1);
    }
    mb_script_put_names(&o, "inputs", in, nin);
    mb_script_put_names(&o, "outputs", outn, nout);
    if (s->node) mb_puts(&o, ",\"node\":true");
    if (s->dynamic) {
        _snprintf_s(num, sizeof num, _TRUNCATE, ",\"dynamic\":%u", s->dynamic);
        mb_puts(&o, num);
    }
    mb_put(&o, "}", 1);
    if (o.oom) { free(o.p); mb_error(b, "out of memory"); goto done; }
    {
        /* Canonical (keys in byte order), as a load of the block reads it. */
        mbj *parsed = mbj_parse(o.p, o.n, NULL, 0);
        mb_buf canon = {0};
        free(o.p);
        if (!parsed || !mbj_canon(parsed, &canon, NULL, 0) || canon.oom) {
            mbj_free(parsed);
            free(canon.p);
            mb_error(b, "out of memory");
            goto done;
        }
        mbj_free(parsed);
        free(b->scripts);
        b->scripts = canon.p;
    }
    mb_note(b, "carries %u script file%s; a build with scripts is never sliced", s->files, s->files == 1 ? "" : "s");
done:
    for (i = 0; ent && i < nent; i++) free(ent[i]);
    for (i = 0; in && i < nin; i++) free(in[i]);
    for (i = 0; outn && i < nout; i++) free(outn[i]);
    free(ent);
    free(ev);
    free(in);
    free(outn);
}

/* Requirements the build can see (DECISIONS 17): a mod that brings campaign
 * content (a closure pointer, or a surface restoring installed content
 * SnapMap's provider cannot see) needs the game's cut-content blacklists off,
 * g_useResourceBlackList 0 and g_useImageBlackList 0, so the author never
 * writes them. An author value for either is replaced, with a warning
 * (`report`: a build; a load derives the same silently, so the hash agrees). */
static void mb_derive_requirements(sh_mb_bundle *b, int report)
{
    static const char *const gates[] = { "g_useResourceBlackList", "g_useImageBlackList" };
    char cause[SH_MB_CAUSE_MAX], *canon;
    mbj *req = NULL;
    const mbj *cvars = NULL, *cmds = NULL;
    mb_buf o = {0};
    size_t i;
    int need = 0, k;
    for (i = 0; i < b->file_count && !need; i++)
        need = b->files[i].closure || b->files[i].cls == SH_MB_CLASS_SURFACE;
    if (!need) return;
    if (b->requirements) {
        req = mbj_parse(b->requirements, strlen(b->requirements), NULL, 0);
        cvars = req ? mbj_get(req, "cvars") : NULL;
        cmds = req ? mbj_get(req, "commands") : NULL;
    }
    mb_puts(&o, "{\"cvars\":{");
    for (k = 0; k < 2; k++) {
        if (k) mb_puts(&o, ",");
        mb_put_jcstr(&o, gates[k]);
        mb_puts(&o, ":\"0\"");
    }
    for (i = 0; cvars && cvars->t == MBJ_OBJ && i < cvars->count; i++) {
        const mbj *v = cvars->items[i];
        int gate = 0;
        for (k = 0; k < 2; k++) if (mb_ieq_n(cvars->keys[i], cvars->klen[i], gates[k])) gate = 1;
        if (gate) {
            if (report && (v->t != MBJ_STR || strcmp(v->s, "0")))
                mb_warn(b, "mod.json requirements set %.*s, but this mod brings campaign content, which needs it 0; "
                        "Snapmap+ sets it 0, so remove it from requirements", (int)cvars->klen[i], cvars->keys[i]);
            continue;
        }
        mb_puts(&o, ",");
        mb_put_jstr(&o, cvars->keys[i], cvars->klen[i]);
        mb_puts(&o, ":");
        if (!mbj_canon(v, &o, NULL, 0)) o.oom = 1;
    }
    mb_puts(&o, "}");
    if (cmds) {
        mb_puts(&o, ",\"commands\":");
        if (!mbj_canon(cmds, &o, NULL, 0)) o.oom = 1;
    }
    mb_puts(&o, "}");
    canon = o.oom ? NULL : sh_mb_requirements_canonical(o.p, o.n, cause, sizeof cause);
    if (!canon) mb_error(b, "the requirements this mod needs could not be recorded (%s)", o.oom ? "out of memory" : cause);
    else {
        free(b->requirements);
        b->requirements = canon;
        if (report) mb_note(b, "requirements: g_useResourceBlackList 0 and g_useImageBlackList 0, derived: the mod brings campaign "
                "content");
    }
    free(o.p);
    mbj_free(req);
}

static void mb_check_files(sh_mb_bundle *b, const wchar_t *root, const sh_mb_resolver *r)
{
    char ns_decl[SH_MB_ID_MAX + 8], ns_raw[SH_MB_ID_MAX + 8];
    mb_req_index own = { NULL, 0 };
    const sh_mb_stock_ops *stock = mb_stock();
    mb_scripts scripts;
    size_t i, k;

    memset(&scripts, 0, sizeof scripts);
    _snprintf_s(ns_decl, sizeof ns_decl, _TRUNCATE, "mods/%s/", b->id);
    _snprintf_s(ns_raw, sizeof ns_raw, _TRUNCATE, "/mods/%s/", b->id);

    qsort(b->files, b->file_count, sizeof(sh_mb_file), mb_file_cmp);
    own.req = (const char **)malloc(sizeof(char *) * (b->file_count ? b->file_count : 1));
    if (!own.req) { mb_error(b, "out of memory"); return; }
    for (i = 0; i < b->file_count; i++) own.req[i] = b->files[i].request;
    own.n = b->file_count;
    qsort(own.req, own.n, sizeof(char *), mb_req_cmp);
    for (i = 1; i < own.n; i++)
        if (strcmp(own.req[i - 1], own.req[i]) == 0) mb_error(b, "two files of this mod land on one game path, %s (they differ only in letter case); delete one",
                                                          own.req[i]);

    for (i = 0; i < b->file_count; i++) {
        sh_mb_file *f = &b->files[i];
        char type[64], name[SH_MB_REQUEST_MAX + 16], *rel;
        wchar_t *wrel, *full;
        unsigned long long got = 0;
        const char *forbidden = mb_rp_forbidden(f->request);

        if (forbidden) {
            mb_error(b, "%s may not ship: %s", f->path, forbidden);
            continue;
        }
        if (!sh_mb_logical_name(f->request, type, sizeof type, name, sizeof name)) {
            if (f->deferred == 1) mb_error(b, "file type not allowed: %s", f->path);
            else mb_error(b, "%s: generated/decls/<type>/ names no decl type this game ships, or the name is empty", f->path);
            continue;
        }
        f->ptype = mb_strdup(type);
        f->pname = mb_strdup(name);
        f->is_decl = mb_starts(f->request, "generated/decls/");
        f->is_module = strcmp(f->ptype ? f->ptype : type, "snapModule") == 0;
        f->shadowed = mb_is_shadow(b, f->request);
        if (!f->ptype || !f->pname) { mb_error(b, "out of memory"); continue; }

        rel = mb_strdup(f->path);
        if (!rel) { mb_error(b, "out of memory"); continue; }
        for (char *p = rel; *p; p++) if (*p == '/') *p = '\\';
        wrel = NULL;
        {
            int wn = MultiByteToWideChar(CP_UTF8, 0, rel, -1, NULL, 0);
            if (wn > 0 && (wrel = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wn)) != NULL)
                MultiByteToWideChar(CP_UTF8, 0, rel, -1, wrel, wn);
        }
        free(rel);
        full = wrel ? mb_wjoin(root, wrel) : NULL;
        free(wrel);
        if (!full) { mb_error(b, "out of memory"); continue; }

        if (f->is_decl || f->is_module) {
            size_t len = 0;
            unsigned char *text = mb_read_wfile(full, SH_MB_MAX_FILE, &len);
            if (!text) { mb_error(b, "cannot read %s", f->path); free(full); continue; }
            sh_mb_sha256_hex(text, len, f->blob);
            got = len;
            if (!sh_decl_text_well_formed(text, len)) {
                mb_error(b, "%s is not a well-formed decl (unbalanced braces, quotes or comments, or a NUL byte)", f->path);
            } else if (f->is_module) {
                /* A module's entity texts are quoted strings: nothing to scan. */
            } else {
                mb_need_ctx nc = { f, 0 };
                mb_bare_ctx bc = { b, f };
                sh_mb_scan_needs(f->ptype, text, len, mb_need_cb, &nc);
                if (nc.oom) mb_error(b, "out of memory");
                qsort(f->needs, f->need_count, sizeof(sh_mb_need), mb_need_cmp);
                sh_mb_scan_bare_hyphen(text, len, mb_bare_cb, &bc);
            }
            free(text);
        } else if (mb_ends(f->request, ".bnk")) {
            /* A bank is read whole: its header must name the id the game will
             * load it as. */
            size_t len = 0;
            unsigned char *bytes = mb_read_wfile(full, SH_MB_MAX_FILE, &len);
            char stem[SH_MB_BANK_STEM_MAX + 1], cause[SH_MB_CAUSE_MAX];
            if (!bytes) { mb_error(b, "cannot read %s", f->path); free(full); continue; }
            sh_mb_sha256_hex(bytes, len, f->blob);
            got = len;
            if (sh_mb_bank_stem(f->request, stem, sizeof stem) && !sh_mb_bank_header_ok(bytes, len, stem, cause, sizeof cause))
                mb_error(b, "%s: %s", f->path, cause);
            free(bytes);
        } else if (sh_mb_script_request(f->request, NULL, NULL) > 0) {
            /* A script is read whole: checked, compiled and scanned (mb_script_file). */
            size_t len = 0;
            unsigned char *text = mb_read_wfile(full, (size_t)-1, &len);
            if (!text) { mb_error(b, "cannot read %s", f->path); free(full); continue; }
            sh_mb_sha256_hex(text, len, f->blob);
            got = len;
            scripts.files++;
            scripts.bytes += len;
            if (got == f->size) mb_script_file(b, i, text, len, &scripts);
            free(text);
        } else if (!mb_hash_wfile(full, &got, f->blob)) {
            mb_error(b, "cannot read %s", f->path);
            free(full);
            continue;
        }
        free(full);
        if (got != f->size) { mb_error(b, "%s changed while Snapmap+ was building the mod; save your work and press Refresh", f->path); continue; }

        /* A carried copy of installed content becomes a stock pointer,
         * matched by size and hash (path first, then renamed copies). */
        if (stock && stock->match) {
            char source[SH_MB_REQUEST_MAX + 1], cause[SH_MB_CAUSE_MAX];
            if (stock->match(stock->context, f->request, f->size, f->blob, source, sizeof source) == SH_MB_STOCK_OK &&
                sh_mb_check_source(source, cause, sizeof cause)) {
                f->stock = 1;
                f->stock_source = mb_strdup(source);
                if (!f->stock_source) { mb_error(b, "out of memory"); continue; }
            }
        }
        /* Identity of decl text is textual: line ends are not content. */
        if (!f->stock && stock && mb_decl_text(f)) mb_text_pointer(b, f, root);
        if (!f->stock && f->deferred) {
            if (f->deferred == 1 && sh_mb_pointer_only_kind(f->request))
                mb_error(b, "%s is not the game's own file: a .sbsp, .ambientsh, .tome or .pages file can only be an "
                         "unchanged copy of the game's; delete it", f->path);
            else if (f->deferred == 1) mb_error(b, "file type not allowed: %s", f->path);
            else mb_error(b, "file larger than 64 MiB: %s", f->path);
            continue;
        }

        {
            /* Additive mods: the file's path and bytes decide what it is
             * (mb_class_of). A pointer restoring installed content under the
             * game's own name surfaces it; a name under the key is an addition
             * or a borrow; any other name must be a game name, and the file
             * replaces it (a shadow, derived, never declared) or, for an
             * editor type, is an editor tweak. A decl there is an edited copy
             * whose edits are derived (mb_derive_edits); a binary is refused. */
            char why[SH_MB_CAUSE_MAX];
            int restore = f->stock && f->stock_source && sh_mb_stock_restore(f->request, f->stock_source);
            int in_ns = !restore && mb_ns_test(b, f, why, sizeof why);
            if (restore) {
                /* a surface: the game's own bytes at the game's own name */
            } else if (mb_ends(f->request, ".bnk")) {
                if (in_ns && f->stock) {
                    mb_error(b, "%s is a copy of a game sound bank; a mod's bank must be its own, so delete it or rebuild it under "
                                "its own name", f->path);
                } else if (!in_ns) {
                    int got = r && r->stock ? r->stock(r->stock_context, f->request) : -1;
                    if (got == 1)
                        mb_error(b, "%s: the game loads its own sound banks before mods, so a mod cannot replace one; "
                                 "delete it and put your sounds in a bank of your own", f->path);
                    else mb_error(b, "%s", why);
                }
            } else if (in_ns) {
                /* A family member is named by its family, which must be the mod's own. */
                char slashed[SH_MB_REQUEST_MAX + 2];
                _snprintf_s(slashed, sizeof slashed, _TRUNCATE, "/%s", f->request);
                if (!f->is_decl && sh_mb_vt_member(f->request, NULL, 0, NULL) &&
                    _strnicmp(slashed + 16, ns_raw, strlen(ns_raw)) != 0)
                    mb_error(b, "%s: a virtual-texture family member must be virtualtextures/mods/%s/<family>_sq<N>.mega2",
                             f->path, b->id);
            } else {
                int got = r && r->stock ? r->stock(r->stock_context, f->request) : -1;
                /* A decl at a game name is an edited copy (mb_derive_edits);
                 * a binary has no structure to diff and the game loads it by
                 * its fixed name, so it is refused, except in a folder the
                 * product received. The game ships the file, so the copy can
                 * also simply go: the mod then borrows the game's. */
                if (got == 1 && !mb_decl_text(f) && !sh_mb_editor_time(f->request) && !g_mb_keep_here) {
                    mb_error(b, "%s: the game ships this file and a mod cannot replace it with other bytes; delete your "
                             "copy and the mod borrows the game's, or put your version under a mods folder named like "
                             "this mod folder and point your decls at it", f->path);
                } else if (got == 1) {
                    f->shadowed = 1;
                } else if (got == 0) {
                    mb_error(b, "%s: %s, and the game has no file of that name for it to replace; move it under "
                             "assets\\<its game folder>\\mods\\<this folder's name>\\ and press Refresh", f->path, why);
                } else {
                    mb_error(b, "%s: %s, and Snapmap+ could not read the game's files to tell whether it replaces "
                             "one; start DOOM normally and press Refresh", f->path, why);
                }
            }
        }
    }

    mb_companion_needs(b);
    mb_scene_needs(b);

    /* A module is placed through its snapModuleInfo, which must travel with it. */
    for (i = 0; i < b->file_count; i++) {
        char info[SH_MB_REQUEST_MAX + 32];
        if (!b->files[i].is_module) continue;
        if (!sh_mb_module_info_request(b->files[i].request, info, sizeof info) || !mb_req_has(&own, info))
            mb_error(b, "%s: a snapModule needs its snapModuleInfo assets/%s in the same bundle", b->files[i].path,
                     sh_mb_module_info_request(b->files[i].request, info, sizeof info) ? info : "?");
    }

    /* Pointers carry nothing: the caps count the carried files only. */
    b->total_size = 0;
    b->stock_count = 0;
    b->stock_bytes = 0;
    for (i = 0; i < b->file_count; i++) {
        if (b->files[i].stock) { b->stock_count++; b->stock_bytes += b->files[i].size; }
        else b->total_size += b->files[i].size;
    }
    if (b->total_size > SH_MB_MAX_TOTAL) mb_error(b, "assets/ larger than 256 MiB");

    /* Additive mods: the shadows are derived from the files, so the
     * canonical manifest of a mod that listed exactly its replacements keeps
     * its text and hash. */
    mb_derive_shadows(b);

    /* Each md6Def's model is whole: its mesh's skeleton and default clip. */
    for (i = 0; i < b->file_count; i++)
        if (b->files[i].is_decl && !mb_model_needs(b, i, r, root)) mb_error(b, "out of memory");

    /* Campaign-only content the served decls need joins the bundle as
     * pointers before the needs are resolved. */
    if (!b->errors) {
        size_t before = b->file_count;
        mb_close_campaign(b, r, root);
        if (b->file_count != before) {
            for (i = 0; i < b->file_count; i++)
                if (b->files[i].closure) {
                    b->stock_count++;
                    b->stock_bytes += b->files[i].size;
                }
            free(own.req);
            own.req = (const char **)malloc(sizeof(char *) * b->file_count);
            own.n = 0;
            if (!own.req) { mb_error(b, "out of memory"); mb_scripts_free(&scripts); return; }
            for (i = 0; i < b->file_count; i++) own.req[i] = b->files[i].request;
            own.n = b->file_count;
            qsort(own.req, own.n, sizeof(char *), mb_req_cmp);
        }
    }

    /* Needs: own files, then installed bundles, then the game. */
    for (i = 0; i < b->file_count; i++) {
        sh_mb_file *f = &b->files[i];
        mb_twin twin;
        memset(&twin, 0, sizeof twin);
        for (k = 0; k < f->need_count; k++) {
            int got = mb_resolve_one(b, &own, r, &f->needs[k]);
            const char *also;
            b->needs.total++;
            if (got == 1) continue;
            b->needs.unresolved++;
            /* Game text brought along for completeness: what it names and no
             * archive holds, the game already plays without. */
            if (f->closure) continue;
            /* Stock materials name virtual-texture source images that have no
             * standalone image resource, so an unresolved image is reported,
             * not refused. */
            if (strcmp(f->needs[k].type, "image") == 0)
                mb_warn(b, "%s needs image %s, which resolves to nothing", f->path, f->needs[k].name);
            /* A shadow replaces a stock file whose own text may name content
             * the game ships without (the stock hands model names six
             * animations no archive holds); the engine already tolerates
             * those names, so a replacement that keeps them is reported, not
             * refused. */
            else if (f->shadowed && got == 0)
                mb_warn(b, "%s needs %s %s, which resolves to nothing (a replaced stock file: reported, not refused)",
                        f->path, f->needs[k].type, f->needs[k].name);
            /* The game's own text of the decl this file is or was copied
             * from names the same missing resource (stock md6Defs keep event
             * and alias blocks of clips no archive holds): the engine already
             * tolerates it, so a copy that keeps it is reported, not refused.
             * A need only the mod introduces stays an error. */
            else if (got == 0 && (also = mb_stock_names_too(&twin, f, &f->needs[k])) != NULL)
                mb_warn(b, "%s names %s %s, which no archive holds; the game's own %s names it too", f->path,
                        f->needs[k].type, f->needs[k].name, also);
            /* An animWeb clip no archive holds is reported, not refused (the
             * engine tolerates it). */
            else if (got == 0 && f->ptype && strcmp(f->ptype, "animWeb") == 0 && strcmp(f->needs[k].type, "anim") == 0)
                mb_warn(b, "%s names anim %s, which no archive holds (an animWeb clip: reported, not refused)", f->path,
                        f->needs[k].name);
            /* Effects, particles, ribbons and movies likewise. */
            else if (got == 0 && (!strcmp(f->needs[k].type, "particle") || !strcmp(f->needs[k].type, "ribbon") ||
                                  !strcmp(f->needs[k].type, "swf") ||
                                  (f->ptype && (!strcmp(f->ptype, "fx") || !strcmp(f->ptype, "particle") ||
                                                !strcmp(f->ptype, "ribbon")))))
                mb_warn(b, "%s names %s %s, which no archive holds (an effect's or a movie's reference: reported, not "
                        "refused)", f->path, f->needs[k].type, f->needs[k].name);
            else if (got < 0)
                mb_error(b, "%s needs %s %s, which this mod does not include, and Snapmap+ could not read the game's "
                         "files to look for it; start DOOM normally and press Refresh",
                         f->path, f->needs[k].type, f->needs[k].name);
            else
                mb_error(b, "%s needs %s %s, which resolves to nothing", f->path, f->needs[k].type, f->needs[k].name);
        }
        mb_twin_reset(&twin);
    }
    mb_scripts_finish(b, &own, &scripts);
    mb_scripts_free(&scripts);
    mb_classify_all(b);
    mb_derive_edits(b, root);
    mb_registries(b);
    mb_report(b);
    free(own.req);
    if (!b->errors) mb_check_renderprogs(b, root);
}

/* ------------------------------------------------------------ manifest ---- */

static void mb_key(mb_buf *o, int *first, const char *key)
{
    if (!*first) mb_put(o, ",", 1);
    *first = 0;
    mb_put_jcstr(o, key);
    mb_put(o, ":", 1);
}

/* Keys in byte order: author bundle extra files format homepage id name
 * requirements shadows; file keys: blob needs path provides request size,
 * or for a stock pointer needs path provides request stock{sha256 source}.
 * "bundle" (the hash itself) only in the manifest form a reader checks. */
/* One file element of the canonical manifest's "files" array. */
static void mb_emit_file(mb_buf *o, const sh_mb_file *f)
{
    size_t k;
    if (f->stock) mb_puts(o, "{\"needs\":[");
    else {
        mb_puts(o, "{\"blob\":");
        mb_put_jcstr(o, f->blob);
        mb_puts(o, ",\"needs\":[");
    }
    for (k = 0; k < f->need_count; k++) {
        if (k) mb_put(o, ",", 1);
        mb_puts(o, "{\"name\":");
        mb_put_jcstr(o, f->needs[k].name);
        mb_puts(o, ",\"type\":");
        mb_put_jcstr(o, f->needs[k].type);
        mb_put(o, "}", 1);
    }
    mb_puts(o, "],\"path\":");
    mb_put_jcstr(o, f->path);
    mb_puts(o, ",\"provides\":{\"name\":");
    mb_put_jcstr(o, f->pname ? f->pname : "");
    mb_puts(o, ",\"type\":");
    mb_put_jcstr(o, f->ptype ? f->ptype : "");
    mb_puts(o, "},\"request\":");
    mb_put_jcstr(o, f->request);
    if (f->stock) {
        mb_puts(o, ",\"stock\":{\"sha256\":");
        mb_put_jcstr(o, f->blob);
        mb_puts(o, ",\"source\":");
        mb_put_jcstr(o, f->stock_source ? f->stock_source : "");
        mb_put(o, "}", 1);
    } else {
        mb_puts(o, ",\"size\":");
        mb_put_u64(o, f->size);
    }
    mb_put(o, "}", 1);
}

static char *mb_emit(const sh_mb_bundle *b, int with_bundle, size_t *out_len)
{
    mb_buf o = {0};
    int first = 1;
    size_t i;
    mb_put(&o, "{", 1);
    if (b->author) { mb_key(&o, &first, "author"); mb_put_jcstr(&o, b->author); }
    if (with_bundle) { mb_key(&o, &first, "bundle"); mb_put_jcstr(&o, b->hash); }
    if (b->extra) { mb_key(&o, &first, "extra"); mb_puts(&o, b->extra); }
    mb_key(&o, &first, "files");
    mb_put(&o, "[", 1);
    for (i = 0; i < b->file_count; i++) {
        if (i) mb_put(&o, ",", 1);
        mb_emit_file(&o, &b->files[i]);
    }
    mb_put(&o, "]", 1);
    mb_key(&o, &first, "format");
    mb_puts(&o, "2");
    if (b->homepage) { mb_key(&o, &first, "homepage"); mb_put_jcstr(&o, b->homepage); }
    mb_key(&o, &first, "id");
    mb_put_jcstr(&o, b->id);
    mb_key(&o, &first, "name");
    mb_put_jcstr(&o, b->name ? b->name : "");
    if (b->requirements) { mb_key(&o, &first, "requirements"); mb_puts(&o, b->requirements); }
    if (b->shadow_count) {
        mb_key(&o, &first, "shadows");
        mb_put(&o, "[", 1);
        for (i = 0; i < b->shadow_count; i++) {
            if (i) mb_put(&o, ",", 1);
            mb_put_jcstr(&o, b->shadows[i]);
        }
        mb_put(&o, "]", 1);
    }
    mb_put(&o, "}", 1);
    if (o.oom) { free(o.p); return NULL; }
    if (out_len) *out_len = o.n;
    return o.p;
}

static void mb_finish(sh_mb_bundle *b)
{
    free(b->canonical);
    b->canonical = mb_emit(b, 0, &b->canonical_len);
    if (!b->canonical) { mb_error(b, "out of memory"); return; }
    if (b->canonical_len > SH_MB_MANIFEST_MAX)
        mb_error(b, "this mod lists too many files for one mod (its file list is over 1 MiB); split it into two");
    sh_mb_sha256_hex(b->canonical, b->canonical_len, b->hash);
}

/* Write through a temporary file and replace atomically. */
static int mb_write_wfile(const wchar_t *path, const void *data, size_t len)
{
    size_t n = wcslen(path);
    wchar_t *tmp = (wchar_t *)malloc(sizeof(wchar_t) * (n + 16));
    HANDLE h;
    DWORD put = 0;
    int ok;
    if (!tmp) return 0;
    _snwprintf_s(tmp, n + 16, _TRUNCATE, L"%s.tmp%lu", path, GetCurrentProcessId());
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(tmp); return 0; }
    ok = WriteFile(h, data, (DWORD)len, &put, NULL) && put == (DWORD)len && FlushFileBuffers(h);
    CloseHandle(h);
    ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    free(tmp);
    return ok;
}


/* ---------------------------------------------------------------- load ---- */

static int mb_is_hex64(const mbj *v)
{
    size_t i;
    if (!v || v->t != MBJ_STR || v->n != SH_MB_HASH_HEX) return 0;
    for (i = 0; i < SH_MB_HASH_HEX; i++)
        if (!((v->s[i] >= '0' && v->s[i] <= '9') || (v->s[i] >= 'a' && v->s[i] <= 'f'))) return 0;
    return 1;
}

static int mb_only_keys(const mbj *o, const char *const *keys)
{
    size_t i;
    for (i = 0; i < o->count; i++) {
        int k, hit = 0;
        for (k = 0; keys[k]; k++)
            if (strlen(keys[k]) == o->klen[i] && memcmp(keys[k], o->keys[i], o->klen[i]) == 0) hit = 1;
        if (!hit) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------ mod.json ---- */

#define MB_BLOCK_EDITED "The part of mod.json that Snapmap+ writes was changed by hand; press Refresh in the " \
                        "Mods tab to build the mod again"

static long long mb_now(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ull) / 10000000ull);
}

/* One entry of the product block's "files" (pointer 0) or "stock" (pointer 1) list.
 * Every refusal of the product's own section is MB_BLOCK_EDITED: the next
 * scan builds the folder again and writes the section anew. */
static int mb_load_block_file(sh_mb_bundle *b, const mbj *e, size_t index, int pointer)
{
    static const char *const keys[] = { "needs", "path", "provides", "sha256", "size", NULL };
    static const char *const pkeys[] = { "closure", "needs", "path", "provides", "sha256", "source", NULL };
    const char *list = pointer ? "stock" : "files";
    const mbj *sha, *needs, *path, *prov, *size, *src, *closure, *pt, *pn;
    char cause[SH_MB_CAUSE_MAX], type[64], name[SH_MB_REQUEST_MAX + 16], *expect;
    sh_mb_file *f;
    size_t k;
    unsigned long long sz = 0;
    if (e->t != MBJ_OBJ || !mb_only_keys(e, pointer ? pkeys : keys)) {
        mb_error(b, MB_BLOCK_EDITED);
        return 0;
    }
    sha = mbj_get(e, "sha256");
    needs = mbj_get(e, "needs");
    path = mbj_get(e, "path");
    prov = mbj_get(e, "provides");
    size = mbj_get(e, "size");
    src = mbj_get(e, "source");
    closure = mbj_get(e, "closure");
    if (!sha || !needs || !path || !prov || (pointer ? !src : !size)) {
        mb_error(b, MB_BLOCK_EDITED);
        return 0;
    }
    if (path->t != MBJ_STR || strlen(path->s) != path->n || !mb_starts(path->s, "assets/")) {
        mb_error(b, MB_BLOCK_EDITED);
        return 0;
    }
    expect = mb_strdup(path->s + 7);
    if (!expect) { mb_error(b, "out of memory"); return 0; }
    mb_lower(expect);
    if (!sh_mb_check_request(expect, cause, sizeof cause)) { mb_error(b, MB_BLOCK_EDITED); free(expect); return 0; }
    if (!pointer && !sh_mb_extension_allowed(expect)) { mb_error(b, MB_BLOCK_EDITED); free(expect); return 0; }
    pt = prov->t == MBJ_OBJ ? mbj_get(prov, "type") : NULL;
    pn = prov->t == MBJ_OBJ ? mbj_get(prov, "name") : NULL;
    if (!sh_mb_logical_name(expect, type, sizeof type, name, sizeof name) || !pt || !pn || prov->count != 2 ||
        pt->t != MBJ_STR || pn->t != MBJ_STR || strcmp(pt->s, type) != 0 || strcmp(pn->s, name) != 0) {
        mb_error(b, MB_BLOCK_EDITED);
        free(expect);
        return 0;
    }
    if (!mb_is_hex64(sha)) { mb_error(b, MB_BLOCK_EDITED); free(expect); return 0; }
    if (pointer) {
        char *lower;
        int same;
        if (src->t != MBJ_STR || strlen(src->s) != src->n || !sh_mb_check_source(src->s, cause, sizeof cause)) {
            mb_error(b, MB_BLOCK_EDITED);
            free(expect);
            return 0;
        }
        lower = mb_path_dup(src->s, src->n);
        same = lower && strcmp(lower, src->s) == 0;
        free(lower);
        if (!same) { mb_error(b, MB_BLOCK_EDITED); free(expect); return 0; }
        if (closure && closure->t != MBJ_BOOL) { mb_error(b, MB_BLOCK_EDITED); free(expect); return 0; }
    } else if (size->t != MBJ_NUM || !mb_plain_integer(size->s, size->n) || size->s[0] == '-' ||
               (sz = _strtoui64(size->s, NULL, 10)) == 0 || sz > SH_MB_MAX_FILE) {
        mb_error(b, MB_BLOCK_EDITED);
        free(expect);
        return 0;
    }
    if (needs->t != MBJ_ARR) { mb_error(b, MB_BLOCK_EDITED); free(expect); return 0; }
    f = mb_add_file(b);
    if (!f) { mb_error(b, "out of memory"); free(expect); return 0; }
    f->path = mb_strdup(path->s);
    f->request = expect;
    memcpy(f->blob, sha->s, SH_MB_HASH_HEX + 1);
    f->size = sz;
    f->ptype = mb_strdup(type);
    f->pname = mb_strdup(name);
    f->is_decl = mb_starts(expect, "generated/decls/");
    f->is_module = strcmp(type, "snapModule") == 0;
    if (pointer) {
        f->stock = 1;
        f->stock_source = mb_strdup(src->s);
        f->closure = closure && closure->b;
        f->absent = f->closure;   /* derived each build, never a file under assets/ */
        b->stock_count++;
        if (!f->stock_source) { mb_error(b, "out of memory"); return 0; }
    }
    b->total_size += sz;
    for (k = 0; k < needs->count; k++) {
        const mbj *n = needs->items[k], *nt = mbj_get(n, "type"), *nn = mbj_get(n, "name");
        if (n->t != MBJ_OBJ || n->count != 2 || !nt || !nn || nt->t != MBJ_STR || nn->t != MBJ_STR ||
            !nt->n || !nn->n || strlen(nn->s) != nn->n || strlen(nt->s) != nt->n) {
            mb_error(b, MB_BLOCK_EDITED);
            return 0;
        }
        if (f->need_count) {
            sh_mb_need probe;
            probe.type = nt->s;
            probe.name = nn->s;
            if (mb_need_cmp(&f->needs[f->need_count - 1], &probe) >= 0) {
                mb_error(b, MB_BLOCK_EDITED);
                return 0;
            }
        }
        if (!mb_add_need(f, nt->s, nn->s, nn->n)) { mb_error(b, "out of memory"); return 0; }
    }
    if (!f->path || !f->ptype || !f->pname) { mb_error(b, "out of memory"); return 0; }
    return 1;
}

/* A built mod.json: the author block, then the builder block (format 2).
 * A block without "build" is a failed build (its "cause") or a folder given
 * its salt and not built yet: the load fails with that, after reading the
 * salt, "built", "rules" and "tree" so the store still knows the folder. */
static int mb_load_root(sh_mb_bundle *out, const mbj *root, int check)
{
    static const char *const bkeys[] = { "build", "built", "cause", "classes", "edits", "editor", "files", "format",
                                         "registries", "rules", "salt", "scripts", "shadows", "stock", "tree", "warnings", NULL };
    const mbj *block, *v, *files, *stock, *bundle, *sh;
    size_t i, total;
    if (root->t != MBJ_OBJ) { mb_error(out, MB_BLOCK_EDITED); return 0; }
    block = mbj_get(root, SH_MB_BLOCK_KEY);
    if (!block || block->t != MBJ_OBJ) {
        mb_error(out, "This mod has not been built yet; press Refresh in the Mods tab");
        return 0;
    }
    v = mbj_get(block, "format");
    if (!v || v->t != MBJ_NUM || strcmp(v->s, "2") != 0) {
        mb_error(out, SH_MB_EARLIER);
        return 0;
    }
    if (!mb_only_keys(block, bkeys)) { mb_error(out, MB_BLOCK_EDITED); return 0; }
    v = mbj_get(block, "salt");
    if (!v || v->t != MBJ_STR || !sh_salt_valid(v->s, v->n)) {
        mb_error(out, "The part of mod.json that Snapmap+ writes (\"" SH_MB_BLOCK_KEY "\") is damaged, so this mod cannot be told apart; put back mod.json from a backup, or make the mod again with New mod");
        return 0;
    }
    memcpy(out->id, v->s, SH_SALT_LEN);
    out->id[SH_SALT_LEN] = 0;
    v = mbj_get(block, "built");
    if (v && (v->t != MBJ_NUM || !mb_plain_integer(v->s, v->n) || v->s[0] == '-')) {
        mb_error(out, MB_BLOCK_EDITED);
        return 0;
    }
    out->built = v ? _strtoi64(v->s, NULL, 10) : 0;
    v = mbj_get(block, "rules");
    if (v && (v->t != MBJ_NUM || !mb_plain_integer(v->s, v->n) || v->s[0] == '-')) {
        mb_error(out, MB_BLOCK_EDITED);
        return 0;
    }
    out->rules = v ? atoi(v->s) : 0;
    v = mbj_get(block, "tree");
    if (v && !mb_is_hex64(v)) { mb_error(out, MB_BLOCK_EDITED); return 0; }
    if (v) memcpy(out->tree, v->s, SH_MB_HASH_HEX + 1);
    v = mbj_get(block, "cause");
    if (v && v->t != MBJ_STR) { mb_error(out, MB_BLOCK_EDITED); return 0; }
    if (v) strncpy_s(out->state_cause, sizeof out->state_cause, v->s, _TRUNCATE);
    out->validated = v == NULL;
    bundle = mbj_get(block, "build");
    if (!bundle) {
        if (out->state_cause[0]) mb_error(out, "not built: %s", out->state_cause);
        else mb_error(out, "not built yet");
        return 0;
    }
    if (!mb_is_hex64(bundle)) { mb_error(out, MB_BLOCK_EDITED); return 0; }
    if (!mb_descriptor_root(out, root, block, 0)) return 0;
    v = mbj_get(block, "registries");
    if (v) {
        mb_buf rb = {0};
        if (v->t != MBJ_OBJ || !mbj_canon(v, &rb, NULL, 0)) {
            free(rb.p);
            mb_error(out, MB_BLOCK_EDITED);
            return 0;
        }
        out->registries = rb.p;
    }
    v = mbj_get(block, "edits");
    if (v) {
        mb_buf eb = {0};
        if (v->t != MBJ_ARR || !mbj_canon(v, &eb, NULL, 0)) {
            free(eb.p);
            mb_error(out, MB_BLOCK_EDITED);
            return 0;
        }
        out->edits = eb.p;
        out->edit_files = (unsigned)v->count;
    }
    v = mbj_get(block, "scripts");
    if (v) {
        mb_buf sb = {0};
        if (v->t != MBJ_OBJ || !mbj_canon(v, &sb, NULL, 0)) {
            free(sb.p);
            mb_error(out, MB_BLOCK_EDITED);
            return 0;
        }
        out->scripts = sb.p;
    }
    sh = mbj_get(block, "shadows");
    if (sh) {
        if (!mb_set_shadows(out, sh)) return 0;
        for (i = 1; i < out->shadow_count; i++)
            if (strcmp(out->shadows[i - 1], out->shadows[i]) >= 0) {
                mb_error(out, MB_BLOCK_EDITED);
                return 0;
            }
    }
    files = mbj_get(block, "files");
    stock = mbj_get(block, "stock");
    if ((files && files->t != MBJ_ARR) || (stock && stock->t != MBJ_ARR)) {
        mb_error(out, MB_BLOCK_EDITED);
        return 0;
    }
    total = (files ? files->count : 0) + (stock ? stock->count : 0);
    if (total > SH_MB_MAX_FILES) { mb_error(out, MB_BLOCK_EDITED); return 0; }
    for (i = 0; files && i < files->count; i++) if (!mb_load_block_file(out, files->items[i], i, 0)) return 0;
    for (i = 0; stock && i < stock->count; i++) if (!mb_load_block_file(out, stock->items[i], i, 1)) return 0;
    qsort(out->files, out->file_count, sizeof(sh_mb_file), mb_file_cmp);
    for (i = 1; i < out->file_count; i++) {
        size_t k;
        if (strcmp(out->files[i - 1].path, out->files[i].path) == 0) {
            mb_error(out, MB_BLOCK_EDITED);
            return 0;
        }
        for (k = 0; k < i; k++)
            if (strcmp(out->files[k].request, out->files[i].request) == 0) {
                mb_error(out, MB_BLOCK_EDITED);
                return 0;
            }
    }
    if (out->total_size > SH_MB_MAX_TOTAL) { mb_error(out, MB_BLOCK_EDITED); return 0; }
    for (i = 0; i < out->file_count; i++) {
        out->files[i].shadowed = mb_is_shadow(out, out->files[i].request);
        if (!out->files[i].stock && sh_mb_script_request(out->files[i].request, NULL, NULL) > 0) out->script_files++;
    }
    mb_classify_all(out);
    mb_mark_edited(out);
    mb_derive_requirements(out, 0);
    mb_finish(out);
    if (!check) memcpy(out->prev_hash, bundle->s, SH_MB_HASH_HEX + 1);   /* the hash its block records */
    if (!out->errors && check && strcmp(out->hash, bundle->s) != 0)
        mb_error(out, "mod.json was edited after its last build; press Refresh in the Mods tab to build it again");
    return out->errors == 0;
}

static int mb_load_modjson(const char *text, size_t len, sh_mb_bundle *out, int check)
{
    char cause[SH_MB_CAUSE_MAX];
    mbj *root;
    int ok;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!text) { mb_error(out, "cannot read mod.json"); return 0; }
    root = mbj_parse(text, len, cause, sizeof cause);
    if (!root) { mb_error(out, "mod.json: %s", cause); return 0; }
    ok = mb_load_root(out, root, check);
    mbj_free(root);
    return ok;
}

int sh_mb_load_text(const char *text, size_t len, sh_mb_bundle *out)
{
    return mb_load_modjson(text, len, out, 1);
}

int sh_mb_load(const char *path, sh_mb_bundle *out)
{
    size_t len = 0;
    unsigned char *text = sh_mb_read_file(path, MB_MODJSON_MAX, &len);
    int ok = mb_load_modjson((const char *)text, len, out, 1);
    free(text);
    return ok;
}

int sh_mb_load_folder(const char *folder, sh_mb_bundle *out)
{
    char path[MAX_PATH * 4];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", folder ? folder : "");
    return sh_mb_load(path, out);
}

/* ----------------------------------------------------------- splicing ---- */

typedef struct mb_member { size_t key_at, key_end, value_at, value_end; } mb_member;

static size_t mb_skip_ws(const char *t, size_t n, size_t i)
{
    while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
    return i;
}

/* Index after the string starting at t[i] == '"', or 0. */
static size_t mb_skip_string(const char *t, size_t n, size_t i)
{
    for (i++; i < n; i++) {
        if (t[i] == '\\') { i++; continue; }
        if (t[i] == '"') return i + 1;
    }
    return 0;
}

/* Index after the value starting at t[i], or 0. */
static size_t mb_skip_value(const char *t, size_t n, size_t i)
{
    int depth = 0;
    if (i >= n) return 0;
    if (t[i] == '"') return mb_skip_string(t, n, i);
    if (t[i] != '{' && t[i] != '[') {
        while (i < n && !strchr(",}] \t\r\n", t[i])) i++;
        return i;
    }
    while (i < n) {
        if (t[i] == '"') { i = mb_skip_string(t, n, i); if (!i) return 0; continue; }
        if (t[i] == '{' || t[i] == '[') depth++;
        else if ((t[i] == '}' || t[i] == ']') && --depth == 0) return i + 1;
        i++;
    }
    return 0;
}

/* The top-level members of a JSON object text (malloc'd array, *count of
 * them); *open is the index of the object's '{'. NULL when the text is not
 * an object. */
static mb_member *mb_members(const char *t, size_t n, size_t *count, size_t *open)
{
    size_t i = 0, cap = 16;
    mb_member *m = (mb_member *)malloc(sizeof *m * cap);
    *count = 0;
    if (!m) return NULL;
    if (n >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) i = 3;
    i = mb_skip_ws(t, n, i);
    if (i >= n || t[i] != '{') { free(m); return NULL; }
    *open = i;
    i = mb_skip_ws(t, n, i + 1);
    if (i < n && t[i] == '}') return m;
    for (;;) {
        mb_member one;
        if (i >= n || t[i] != '"') break;
        one.key_at = i;
        one.key_end = mb_skip_string(t, n, i);
        if (!one.key_end) break;
        i = mb_skip_ws(t, n, one.key_end);
        if (i >= n || t[i] != ':') break;
        one.value_at = mb_skip_ws(t, n, i + 1);
        one.value_end = mb_skip_value(t, n, one.value_at);
        if (!one.value_end) break;
        if (*count == cap) {
            mb_member *grown = (mb_member *)realloc(m, sizeof *m * cap * 2);
            if (!grown) break;
            m = grown;
            cap *= 2;
        }
        m[(*count)++] = one;
        i = mb_skip_ws(t, n, one.value_end);
        if (i < n && t[i] == ',') { i = mb_skip_ws(t, n, i + 1); continue; }
        if (i < n && t[i] == '}') return m;
        break;
    }
    free(m);
    return NULL;
}

static int mb_member_is(const char *t, const mb_member *m, const char *key)
{
    size_t k = strlen(key);
    return m->key_end - m->key_at == k + 2 && memcmp(t + m->key_at + 1, key, k) == 0;
}

char *sh_mb_author_bytes(const char *t, size_t n, size_t *out_len, int *had_block)
{
    size_t count = 0, open = 0, i, cut_from = 0, cut_to = 0;
    mb_member *m = t ? mb_members(t, n, &count, &open) : NULL;
    char *out;
    if (had_block) *had_block = 0;
    if (!m) return NULL;
    for (i = 0; i < count; i++) {
        if (!mb_member_is(t, &m[i], SH_MB_BLOCK_KEY)) continue;
        if (had_block) *had_block = 1;
        if (i > 0) { cut_from = m[i - 1].value_end; cut_to = m[i].value_end; }
        else if (count > 1) { cut_from = m[0].key_at; cut_to = m[1].key_at; }
        else { cut_from = open + 1; cut_to = m[0].value_end; }
        break;
    }
    free(m);
    out = (char *)malloc(n - (cut_to - cut_from) + 1);
    if (!out) return NULL;
    memcpy(out, t, cut_from);
    memcpy(out + cut_from, t + cut_to, n - cut_to);
    out[n - (cut_to - cut_from)] = 0;
    if (out_len) *out_len = n - (cut_to - cut_from);
    return out;
}

/* Insert ,NL NL "snapmap-plus": <block> after the last member of `own`. The
 * block text is json_pretty output; its lines are indented by two more
 * spaces. NL is CRLF when the author's file uses CRLF. */
static char *mb_splice(const char *own, size_t n, const char *block, size_t *out_len)
{
    size_t count = 0, open = 0, at, i;
    mb_member *m = mb_members(own, n, &count, &open);
    const char *nl;
    mb_buf o = {0};
    size_t bl = strlen(block);
    if (!m) return NULL;
    nl = strstr(own, "\r\n") ? "\r\n" : "\n";
    at = count ? m[count - 1].value_end : open + 1;
    free(m);
    while (bl && (block[bl - 1] == '\n' || block[bl - 1] == '\r')) bl--;
    mb_put(&o, own, at);
    if (count) { mb_puts(&o, ","); mb_puts(&o, nl); }
    mb_puts(&o, nl);
    mb_puts(&o, "  \"" SH_MB_BLOCK_KEY "\": ");
    for (i = 0; i < bl; i++) {
        if (block[i] == '\n') { mb_puts(&o, nl); mb_puts(&o, "  "); }
        else mb_put(&o, &block[i], 1);
    }
    if (!count) mb_puts(&o, nl);
    mb_put(&o, own + at, n - at);
    if (o.oom) { free(o.p); return NULL; }
    if (out_len) *out_len = o.n;
    return o.p;
}

static char *mb_pretty(const char *compact, size_t n)
{
    size_t need = json_pretty(compact, n, NULL, 0);
    char *p;
    if (!need) return NULL;
    p = (char *)malloc(need + 1);
    if (!p) return NULL;
    json_pretty(compact, n, p, need);
    p[need] = 0;
    return p;
}

char *sh_mb_salt_modjson(const char *text, size_t n, const char *from, const char *salt, size_t *out_len)
{
    char *own, *moved = NULL, *block, *out;
    size_t own_len = 0, moved_len = 0, count = 0;
    mb_buf o = {0};
    if (!salt || !sh_salt_valid(salt, strlen(salt))) return NULL;
    own = text ? sh_mb_author_bytes(text, n, &own_len, NULL) : NULL;
    if (!own) return NULL;
    /* The author's own references follow the salt: strings keys, hud decl
     * names, any value naming mods/<from>/. */
    if (from && from[0]) count = sh_mb_rekey_text(own, own_len, from, salt, &moved, &moved_len);
    if (count == (size_t)-1) { free(own); return NULL; }
    if (count && moved) { free(own); own = moved; own_len = moved_len; }
    mb_puts(&o, "{\"format\":2,\"salt\":");
    mb_put_jcstr(&o, salt);
    mb_puts(&o, "}");
    block = o.oom ? NULL : mb_pretty(o.p, o.n);
    free(o.p);
    out = block ? mb_splice(own, own_len, block, out_len) : NULL;
    free(own);
    free(block);
    return out;
}


/* ------------------------------------------- a new mod's strings keys ---- */
/* At every scan that moves a folder's own content under its salt, its strings
 * keys without the "<salt>_" prefix (string edits aside) gain it and their
 * "#str_<key>" references follow (sh_ms_assign_salt). A key is found by a
 * byte-exact walk of the author's text: the top-level "strings" member, each
 * language object in it, each of its keys. A new key that would equal one the
 * object already holds takes "_2", "_3", ... */


/* Past one JSON value at p (whitespace before it skipped); NULL when it does
 * not read. Only the extent is checked, not the grammar of literals. */
static const char *mb_sk_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *mb_sk_skip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"') return NULL;
    for (p++; p < end; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *mb_sk_skip_value(const char *p, const char *end, int depth)
{
    p = mb_sk_skip_ws(p, end);
    if (p >= end || depth > 64) return NULL;
    if (*p == '"') return mb_sk_skip_string(p, end);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        int obj = *p == '{';
        p = mb_sk_skip_ws(p + 1, end);
        if (p < end && *p == close) return p + 1;
        for (;;) {
            if (obj) {
                p = mb_sk_skip_string(mb_sk_skip_ws(p, end), end);
                p = p ? mb_sk_skip_ws(p, end) : NULL;
                if (!p || p >= end || *p != ':') return NULL;
                p++;
            }
            p = mb_sk_skip_value(p, end, depth + 1);
            p = p ? mb_sk_skip_ws(p, end) : NULL;
            if (!p || p >= end) return NULL;
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    return p;
}

/* Walk the members of the object at p, calling fn with each key's span
 * (quotes included) and value start. Returns the end of the object, or NULL. */
typedef int (*mb_sk_member_fn)(void *ctx, const char *key, size_t key_len, const char *value, const char *value_end);
static const char *mb_sk_members(const char *p, const char *end, mb_sk_member_fn fn, void *ctx)
{
    p = mb_sk_skip_ws(p, end);
    if (p >= end || *p != '{') return NULL;
    p = mb_sk_skip_ws(p + 1, end);
    if (p < end && *p == '}') return p + 1;
    for (;;) {
        const char *k = mb_sk_skip_ws(p, end), *ke = mb_sk_skip_string(k, end), *v, *ve;
        if (!ke) return NULL;
        p = mb_sk_skip_ws(ke, end);
        if (p >= end || *p != ':') return NULL;
        v = mb_sk_skip_ws(p + 1, end);
        ve = mb_sk_skip_value(v, end, 1);
        if (!ve) return NULL;
        if (fn && !fn(ctx, k, (size_t)(ke - k), v, ve)) return NULL;
        p = mb_sk_skip_ws(ve, end);
        if (p >= end) return NULL;
        if (*p == '}') return p + 1;
        if (*p != ',') return NULL;
        p++;
    }
}

typedef struct mb_sk_span { size_t at, len; char *to; } mb_sk_span;

typedef struct mb_sk_ctx {
    const char *base, *salt, *from;
    mb_sk_span *spans;
    size_t n, cap;
    sh_mb_strkey *keys;
    size_t nkeys, kcap;
    const char *lang, *lang_end;   /* the language object being walked */
    int failed;
} mb_sk_ctx;

/* Whether the language object [lang, end) already has the key `name`. */
typedef struct mb_sk_find { const char *name; int hit; } mb_sk_find;
static int mb_sk_find_cb(void *vctx, const char *key, size_t key_len, const char *value, const char *value_end)
{
    mb_sk_find *f = (mb_sk_find *)vctx;
    (void)value; (void)value_end;
    if (key_len - 2 == strlen(f->name) && !_strnicmp(key + 1, f->name, key_len - 2)) f->hit = 1;
    return 1;
}
static int mb_sk_has_key(const mb_sk_ctx *c, const char *name)
{
    mb_sk_find f;
    f.name = name;
    f.hit = 0;
    if (c->lang) (void)mb_sk_members(c->lang, c->lang_end, mb_sk_find_cb, &f);
    return f.hit;
}

static int mb_sk_is_name(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* The prefixed key: "<salt>_<key>", a leading "mods/<salt>/" of an earlier
 * spelling dropped and every other character outside [A-Za-z0-9_] made '_'. */
static char *mb_sk_new_key(const char *key, size_t n, const char *salt, const char *from)
{
    size_t sl = strlen(salt), fl = from ? strlen(from) : 0, i, w;
    char *out;
    if (n > 5 + sl + 1 && !_strnicmp(key, "mods/", 5) && !_strnicmp(key + 5, salt, sl) && key[5 + sl] == '/') {
        key += 6 + sl;
        n -= 6 + sl;
    } else if (fl && n > fl + 1 && sh_salt_valid(from, fl) && !_strnicmp(key, from, fl) && key[fl] == '_') {
        /* A copy's keys carry the salt it is leaving. */
        key += fl + 1;
        n -= fl + 1;
    }
    out = (char *)malloc(sl + 1 + n + 1);
    if (!out) return NULL;
    memcpy(out, salt, sl);
    out[sl] = '_';
    for (i = 0, w = sl + 1; i < n; i++) out[w++] = mb_sk_is_name(key[i]) ? key[i] : '_';
    out[w] = 0;
    return out;
}

static int mb_sk_key(void *vctx, const char *key, size_t key_len, const char *value, const char *value_end)
{
    mb_sk_ctx *c = (mb_sk_ctx *)vctx;
    const char *k = key + 1;
    size_t kn = key_len - 2, i, sl = strlen(c->salt);
    char *to = NULL, *from;
    (void)value;
    (void)value_end;
    if (memchr(k, '\\', kn)) return 1;   /* an escaped key is left as written (the build names it) */
    if (kn > sl && !_strnicmp(k, c->salt, sl) && k[sl] == '_') return 1;   /* already prefixed */
    if (sh_strkey_is_edit(k, kn)) return 1;   /* a string edit stays as the game names it (DECISIONS 9) */
    for (i = 0; i < c->nkeys && !(strlen(c->keys[i].from) == kn && !memcmp(c->keys[i].from, k, kn)); i++) {}
    if (i == c->nkeys) {
        if (c->nkeys == c->kcap) {
            size_t cap = c->kcap ? c->kcap * 2 : 32;
            sh_mb_strkey *g = (sh_mb_strkey *)realloc(c->keys, cap * sizeof *g);
            if (!g) { c->failed = 1; return 0; }
            c->keys = g;
            c->kcap = cap;
        }
        from = mb_strndup(k, kn);
        to = mb_sk_new_key(k, kn, c->salt, c->from);
        if (!from || !to) { free(from); free(to); c->failed = 1; return 0; }
        {
            /* Two keys that differ only in characters a key cannot hold
             * ("a b", "a_b") stay two keys: "_2", "_3", ... */
            size_t j, tl = strlen(to);
            unsigned dup = 1;
            for (;;) {
                int clash = 0;
                for (j = 0; j < c->nkeys && !clash; j++) clash = !_stricmp(c->keys[j].to, to);
                if (!clash) clash = mb_sk_has_key(c, to);   /* a key the mod already has */
                if (!clash) break;
                {
                    char *next = (char *)malloc(tl + 16);
                    if (!next) { free(from); free(to); c->failed = 1; return 0; }
                    _snprintf_s(next, tl + 16, _TRUNCATE, "%.*s_%u", (int)tl, to, ++dup);
                    free(to);
                    to = next;
                }
            }
        }
        c->keys[c->nkeys].from = from;
        c->keys[c->nkeys].to = to;
        c->nkeys++;
    }
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 32;
        mb_sk_span *g = (mb_sk_span *)realloc(c->spans, cap * sizeof *g);
        if (!g) { c->failed = 1; return 0; }
        c->spans = g;
        c->cap = cap;
    }
    c->spans[c->n].at = (size_t)(k - c->base);
    c->spans[c->n].len = kn;
    c->spans[c->n].to = c->keys[i].to;
    c->n++;
    return 1;
}

static int mb_sk_language(void *vctx, const char *key, size_t key_len, const char *value, const char *value_end)
{
    mb_sk_ctx *c = (mb_sk_ctx *)vctx;
    (void)key; (void)key_len;
    if (*value != '{') return 1;
    c->lang = value;
    c->lang_end = value_end;
    return mb_sk_members(value, value_end, mb_sk_key, vctx) != NULL;
}

static int mb_sk_top(void *vctx, const char *key, size_t key_len, const char *value, const char *value_end)
{
    if (key_len != 9 || memcmp(key, "\"strings\"", 9) || *value != '{') return 1;
    return mb_sk_members(value, value_end, mb_sk_language, vctx) != NULL;
}

void sh_mb_strkeys_free(sh_mb_strkey *keys, size_t count)
{
    size_t i;
    for (i = 0; keys && i < count; i++) { free(keys[i].from); free(keys[i].to); }
    free(keys);
}

char *sh_mb_salt_strings(const char *text, size_t n, const char *from, const char *salt, sh_mb_strkey **keys, size_t *count,
                         size_t *out_len)
{
    mb_sk_ctx c;
    mb_buf o = {0};
    size_t i, at = 0, skip = 0;
    if (keys) *keys = NULL;
    if (count) *count = 0;
    if (out_len) *out_len = 0;
    if (!text || !salt || !sh_salt_valid(salt, strlen(salt))) return NULL;
    memset(&c, 0, sizeof c);
    if (n >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) skip = 3;
    c.base = text;
    c.salt = salt;
    c.from = from;
    if (!mb_sk_members(text + skip, text + n, mb_sk_top, &c) || c.failed) {
        free(c.spans);
        sh_mb_strkeys_free(c.keys, c.nkeys);
        return NULL;
    }
    for (i = 0; i < c.n; i++) {
        mb_put(&o, text + at, c.spans[i].at - at);
        mb_puts(&o, c.spans[i].to);
        at = c.spans[i].at + c.spans[i].len;
    }
    mb_put(&o, text + at, n - at);
    free(c.spans);
    if (o.oom || !o.p) { free(o.p); sh_mb_strkeys_free(c.keys, c.nkeys); return NULL; }
    if (keys) *keys = c.keys;
    else sh_mb_strkeys_free(c.keys, c.nkeys);
    if (count) *count = c.nkeys;
    if (out_len) *out_len = o.n;
    return o.p;
}

size_t sh_mb_restr_text(const char *text, size_t n, const sh_mb_strkey *keys, size_t count, char **out, size_t *out_len)
{
    mb_buf o = {0};
    size_t i = 0, at = 0, hits = 0, k;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!text || !count) return 0;
    while (i + 5 <= n) {
        const char *p = (const char *)memchr(text + i, '#', n - i);
        size_t pos, best = (size_t)-1, best_len = 0;
        if (!p) break;
        pos = (size_t)(p - text);
        i = pos + 1;
        if (pos + 5 > n || _strnicmp(p, "#str_", 5)) continue;
        /* The longest key that ends at a name boundary. */
        for (k = 0; k < count; k++) {
            size_t kl = strlen(keys[k].from);
            if (pos + 5 + kl > n || kl <= best_len || _strnicmp(p + 5, keys[k].from, kl)) continue;
            if (pos + 5 + kl < n && mb_sk_is_name(text[pos + 5 + kl])) continue;
            best = k;
            best_len = kl;
        }
        if (best == (size_t)-1) continue;
        hits++;
        if (out) {
            mb_put(&o, text + at, pos + 5 - at);
            mb_puts(&o, keys[best].to);
        }
        at = pos + 5 + best_len;
        i = at;
    }
    if (!out || !hits) return hits;
    mb_put(&o, text + at, n - at);
    if (o.oom) { free(o.p); return (size_t)-1; }
    *out = o.p;
    if (out_len) *out_len = o.n;
    return hits;
}
/* The canonical text of `key` in the bundle's extra, or NULL. */
static char *mb_extra_member(const sh_mb_bundle *b, const char *key)
{
    mbj *extra;
    const mbj *v;
    mb_buf o = {0};
    if (!b->extra) return NULL;
    extra = mbj_parse(b->extra, strlen(b->extra), NULL, 0);
    v = extra ? mbj_get(extra, key) : NULL;
    if (v && !mbj_canon(v, &o, NULL, 0)) { free(o.p); o.p = NULL; }
    mbj_free(extra);
    return v ? o.p : NULL;
}

static void mb_put_needs(mb_buf *o, const sh_mb_file *f)
{
    size_t k;
    mb_puts(o, ",\"provides\":{\"type\":");
    mb_put_jcstr(o, f->ptype ? f->ptype : "");
    mb_puts(o, ",\"name\":");
    mb_put_jcstr(o, f->pname ? f->pname : "");
    mb_puts(o, "},\"needs\":[");
    for (k = 0; k < f->need_count; k++) {
        if (k) mb_put(o, ",", 1);
        mb_puts(o, "{\"type\":");
        mb_put_jcstr(o, f->needs[k].type);
        mb_puts(o, ",\"name\":");
        mb_put_jcstr(o, f->needs[k].name);
        mb_put(o, "}", 1);
    }
    mb_put(o, "]", 1);
}

/* The builder block, pretty-printed (modbundle.h, mod.json format 2). */
static char *mb_block_text(const sh_mb_bundle *b)
{
    mb_buf o = {0};
    char num[32], *pretty;
    size_t i, pointers = 0;
    int first;
    mb_puts(&o, "{\"format\":2,\"salt\":");
    mb_put_jcstr(&o, b->id);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"rules\":%d", SH_MB_RULES);
    mb_puts(&o, num);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"built\":%lld", b->built > 0 ? b->built : mb_now());
    mb_puts(&o, num);
    if (b->tree[0]) { mb_puts(&o, ",\"tree\":"); mb_put_jcstr(&o, b->tree); }
    mb_puts(&o, ",\"build\":");
    mb_put_jcstr(&o, b->hash);
    if (!b->validated && b->state_cause[0]) { mb_puts(&o, ",\"cause\":"); mb_put_jcstr(&o, b->state_cause); }
    if (b->warning_list_count) {
        mb_puts(&o, ",\"warnings\":[");
        for (i = 0; i < b->warning_list_count; i++) {
            if (i) mb_put(&o, ",", 1);
            mb_put_jcstr(&o, b->warning_list[i]);
        }
        mb_put(&o, "]", 1);
    }
    /* Derived facts the Mods tab and the doctor read;
     * not part of the canonical manifest. */
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"classes\":{\"adds\":%u", b->classes.adds);
    mb_puts(&o, num);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"borrows\":%u", b->classes.borrows);
    mb_puts(&o, num);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"edits\":%u", b->classes.edits);
    mb_puts(&o, num);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"replaces\":%u", b->classes.replaces);
    mb_puts(&o, num);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"editor\":%u", b->classes.editor);
    mb_puts(&o, num);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"surfaces\":%u}", b->classes.surfaces);
    mb_puts(&o, num);
    mb_puts(&o, ",\"editor\":[");
    for (i = 0, first = 1; i < b->file_count; i++) {
        if (b->files[i].cls != SH_MB_CLASS_EDITOR) continue;
        if (!first) mb_put(&o, ",", 1);
        first = 0;
        mb_put_jcstr(&o, b->files[i].request);
    }
    mb_put(&o, "]", 1);
    mb_puts(&o, ",\"registries\":");
    mb_puts(&o, b->registries ? b->registries : "{}");
    if (b->edits) { mb_puts(&o, ",\"edits\":"); mb_puts(&o, b->edits); }
    if (b->scripts) { mb_puts(&o, ",\"scripts\":"); mb_puts(&o, b->scripts); }
    if (b->shadow_count) {
        mb_puts(&o, ",\"shadows\":[");
        for (i = 0; i < b->shadow_count; i++) {
            if (i) mb_put(&o, ",", 1);
            mb_put_jcstr(&o, b->shadows[i]);
        }
        mb_put(&o, "]", 1);
    }
    mb_puts(&o, ",\"files\":[");
    for (i = 0, first = 1; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        if (f->stock) { pointers++; continue; }
        if (!first) mb_put(&o, ",", 1);
        first = 0;
        mb_puts(&o, "{\"path\":");
        mb_put_jcstr(&o, f->path);
        mb_puts(&o, ",\"sha256\":");
        mb_put_jcstr(&o, f->blob);
        mb_puts(&o, ",\"size\":");
        mb_put_u64(&o, f->size);
        mb_put_needs(&o, f);
        mb_put(&o, "}", 1);
    }
    mb_put(&o, "]", 1);
    if (pointers) {
        mb_puts(&o, ",\"stock\":[");
        for (i = 0, first = 1; i < b->file_count; i++) {
            const sh_mb_file *f = &b->files[i];
            if (!f->stock) continue;
            if (!first) mb_put(&o, ",", 1);
            first = 0;
            mb_puts(&o, "{\"path\":");
            mb_put_jcstr(&o, f->path);
            mb_puts(&o, ",\"sha256\":");
            mb_put_jcstr(&o, f->blob);
            mb_puts(&o, ",\"source\":");
            mb_put_jcstr(&o, f->stock_source ? f->stock_source : "");
            mb_put_needs(&o, f);
            if (f->closure) mb_puts(&o, ",\"closure\":true");
            mb_put(&o, "}", 1);
        }
        mb_put(&o, "]", 1);
    }
    mb_put(&o, "}", 1);
    if (o.oom) { free(o.p); return NULL; }
    pretty = mb_pretty(o.p, o.n);
    free(o.p);
    return pretty;
}

/* An author block for a bundle known only by its manifest: the display
 * keys, requirements and every extra key; shadows live in the builder
 * block only. */
static char *mb_author_synth(const sh_mb_bundle *b, size_t *out_len)
{
    mb_buf o = {0};
    mbj *extra = NULL;
    char *pretty;
    size_t i;
    mb_puts(&o, "{\"name\":");
    mb_put_jcstr(&o, b->name ? b->name : b->id);
    if (b->author) { mb_puts(&o, ",\"author\":"); mb_put_jcstr(&o, b->author); }
    if (b->homepage) { mb_puts(&o, ",\"homepage\":"); mb_put_jcstr(&o, b->homepage); }
    if (b->requirements) { mb_puts(&o, ",\"requirements\":"); mb_puts(&o, b->requirements); }
    if (b->extra) extra = mbj_parse(b->extra, strlen(b->extra), NULL, 0);
    for (i = 0; extra && extra->t == MBJ_OBJ && i < extra->count; i++) {
        mb_put(&o, ",", 1);
        mb_put_jstr(&o, extra->keys[i], extra->klen[i]);
        mb_put(&o, ":", 1);
        if (!mbj_canon(extra->items[i], &o, NULL, 0)) o.oom = 1;
    }
    mbj_free(extra);
    mb_put(&o, "}", 1);
    if (o.oom) { free(o.p); return NULL; }
    pretty = mb_pretty(o.p, o.n);
    free(o.p);
    if (pretty && out_len) *out_len = strlen(pretty);
    return pretty;
}

static char *mb_modjson(const sh_mb_bundle *b, const char *author, size_t alen, size_t *out_len)
{
    char *own, *block, *out;
    size_t own_len = 0;
    if (!b || !b->canonical) return NULL;
    own = author ? sh_mb_author_bytes(author, alen, &own_len, NULL) : mb_author_synth(b, &own_len);
    if (!own) return NULL;
    block = mb_block_text(b);
    out = block ? mb_splice(own, own_len, block, out_len) : NULL;
    free(own);
    free(block);
    return out;
}

char *sh_mb_modjson_text(const sh_mb_bundle *b, const char *author_text, size_t author_len, size_t *out_len)
{
    return mb_modjson(b, author_text, author_len, out_len);
}

int sh_mb_write(const sh_mb_bundle *b, const char *folder, char *cause, size_t cap)
{
    wchar_t *root, *mod_path = NULL;
    unsigned char *existing = NULL;
    char *text = NULL;
    size_t elen = 0, len = 0;
    int ok;
    if (!b || b->errors || !b->canonical) return mb_cause(cause, cap, "the bundle has errors and is not written");
    root = folder ? sh_mb_wpath(folder) : NULL;
    if (root) mod_path = mb_wjoin(root, L"mod.json");
    if (!mod_path) { free(root); return mb_cause(cause, cap, "out of memory"); }
    existing = mb_read_wfile(mod_path, MB_MODJSON_MAX, &elen);
    text = mb_modjson(b, (const char *)existing, elen, &len);
    ok = text && mb_write_wfile(mod_path, text, len);
    if (!ok && text) mb_cause(cause, cap, "cannot write mod.json: %s", sh_reason_win32(GetLastError(), "mod.json", len));
    else if (!ok) mb_cause(cause, cap, "mod.json could not be composed (is it a JSON object?)");
    free(text);
    free(existing);
    free(root);
    free(mod_path);
    return ok;
}

int sh_mb_write_failed(const sh_mb_bundle *b, const char *folder, char *why, size_t cap)
{
    wchar_t *root, *mod_path = NULL;
    unsigned char *existing = NULL;
    char *own = NULL, *block = NULL, *text = NULL, num[32];
    size_t elen = 0, own_len = 0, len = 0;
    mb_buf o = {0};
    int ok = 0;
    if (!b || !sh_salt_valid(b->id, strlen(b->id))) return mb_cause(why, cap, "This mod folder has not been scanned yet; press Refresh in the Mods tab");
    root = folder ? sh_mb_wpath(folder) : NULL;
    if (root) mod_path = mb_wjoin(root, L"mod.json");
    if (!mod_path) { free(root); return mb_cause(why, cap, "out of memory"); }
    existing = mb_read_wfile(mod_path, MB_MODJSON_MAX, &elen);
    own = existing ? sh_mb_author_bytes((const char *)existing, elen, &own_len, NULL) : NULL;
    if (!own) { mb_cause(why, cap, "mod.json is missing or is not a JSON object"); goto done; }
    mb_puts(&o, "{\"format\":2,\"salt\":");
    mb_put_jcstr(&o, b->id);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"rules\":%d", SH_MB_RULES);
    mb_puts(&o, num);
    if (b->built > 0) {
        _snprintf_s(num, sizeof num, _TRUNCATE, ",\"built\":%lld", b->built);
        mb_puts(&o, num);
    }
    if (b->tree[0]) { mb_puts(&o, ",\"tree\":"); mb_put_jcstr(&o, b->tree); }
    mb_puts(&o, ",\"cause\":");
    mb_put_jcstr(&o, b->first_cause[0] ? b->first_cause : "the build failed");
    mb_puts(&o, "}");
    block = o.oom ? NULL : mb_pretty(o.p, o.n);
    text = block ? mb_splice(own, own_len, block, &len) : NULL;
    ok = text && mb_write_wfile(mod_path, text, len);
    if (!ok && text) mb_cause(why, cap, "cannot write mod.json: %s", sh_reason_win32(GetLastError(), "mod.json", len));
    else if (!ok) mb_cause(why, cap, "out of memory");
done:
    free(o.p);
    free(block);
    free(text);
    free(own);
    free(existing);
    free(root);
    free(mod_path);
    return ok;
}

int sh_mb_set_state(const char *folder, int validated, const char *cause, char *why, size_t cap)
{
    sh_mb_bundle b;
    int ok;
    if (!sh_mb_load_folder(folder, &b)) {
        mb_cause(why, cap, "%s", b.first_cause);
        sh_mb_free(&b);
        return 0;
    }
    if (b.validated == (validated != 0) && strcmp(b.state_cause, validated ? "" : (cause ? cause : "")) == 0) {
        sh_mb_free(&b);
        return 1;
    }
    b.validated = validated != 0;
    strncpy_s(b.state_cause, sizeof b.state_cause, validated ? "" : (cause ? cause : ""), _TRUNCATE);
    ok = sh_mb_write(&b, folder, why, cap);
    sh_mb_free(&b);
    return ok;
}

/* -------------------------------------------------------- salt paths ---- */

int sh_mb_salt_paths_check(const sh_mb_bundle *b, char *cause, size_t cap)
{
    size_t i;
    int pass;
    if (cause && cap) cause[0] = 0;
    if (!b || !sh_salt_valid(b->id, strlen(b->id))) return mb_cause(cause, cap, "This mod folder has not been scanned yet; press Refresh in the Mods tab");
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        for (pass = 0; pass < 2; pass++) {
            const char *t = pass ? f->pname : f->request;
            size_t at;
            if (!t) continue;
            for (at = 0; t[at]; at++) {
                const char *seg, *end;
                if (_strnicmp(t + at, "mods/", 5) != 0 || (at && t[at - 1] != '/')) continue;
                seg = t + at + 5;
                end = strchr(seg, '/');
                if (!end || end == seg) continue;
                if ((size_t)(end - seg) == SH_SALT_LEN && _strnicmp(seg, b->id, SH_SALT_LEN) == 0) continue;
                return mb_cause(cause, cap, "%s is under mods/%.*s/, which is neither this mod folder's name nor its "
                                "salt; move it under a mods folder named like this folder and press Refresh",
                                f->path ? f->path : t, (int)(end - seg), seg);

            }
        }
    }
    return 1;
}

/* -------------------------------------------------------------- build ---- */

int sh_mb_build(const char *folder, const char *salt, const sh_mb_resolver *resolver, sh_mb_bundle *out)
{
    wchar_t *root, *desc_mod, *desc_pkg;
    DWORD attr;
    int has_mod, has_pkg;
    unsigned char *text = NULL;
    size_t len = 0;
    long long prev_built = 0;
    mbj *rootj = NULL;
    mb_walk w;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    root = folder ? sh_mb_wpath(folder) : NULL;
    if (!root) { mb_error(out, "no mod folder given"); return 0; }
    attr = GetFileAttributesW(root);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        mb_error(out, "%s is not a folder", folder);
        free(root);
        return 0;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        mb_error(out, "the mod folder is a symbolic link or junction");
        free(root);
        return 0;
    }
    /* The rebuild rule's key, recorded in the block (failed or not). */
    if (!sh_mb_tree_hash(folder, out->tree)) out->tree[0] = 0;
    desc_mod = mb_wjoin(root, L"mod.json");
    desc_pkg = mb_wjoin(root, L"package.json");
    has_mod = desc_mod && GetFileAttributesW(desc_mod) != INVALID_FILE_ATTRIBUTES;
    has_pkg = desc_pkg && GetFileAttributesW(desc_pkg) != INVALID_FILE_ATTRIBUTES;
    if (!has_mod && has_pkg) {
        mb_error(out, SH_MB_EARLIER);
    } else if (!has_mod) {
        mb_error(out, "the mod folder has no mod.json");
    } else {
        char cause[SH_MB_CAUSE_MAX];
        text = mb_read_wfile(desc_mod, MB_MODJSON_MAX, &len);
        if (!text) mb_error(out, "cannot read mod.json (missing, unreadable or over %u MiB)", MB_MODJSON_MAX >> 20);
        else if (!(rootj = mbj_parse((const char *)text, len, cause, sizeof cause))) mb_error(out, "mod.json: %s", cause);
        else {
            const mbj *block = mbj_get(rootj, SH_MB_BLOCK_KEY), *fmt = NULL, *bs = NULL, *pt;
            if (block && block->t == MBJ_OBJ) {
                fmt = mbj_get(block, "format");
                bs = mbj_get(block, "salt");
            }
            if (block && (block->t != MBJ_OBJ || !fmt || fmt->t != MBJ_NUM || strcmp(fmt->s, "2") != 0)) {
                mb_error(out, SH_MB_EARLIER);
            } else if (bs && (bs->t != MBJ_STR || !sh_salt_valid(bs->s, bs->n))) {
                mb_error(out, "The part of mod.json that Snapmap+ writes (\"" SH_MB_BLOCK_KEY "\") is damaged, so this mod cannot be told apart; put back mod.json from a backup, or make the mod again with New mod");
            } else if (bs && salt && (strlen(salt) != bs->n || memcmp(salt, bs->s, bs->n) != 0)) {
                mb_error(out, "mod.json's salt is %s, not %s", bs->s, salt);
            } else if (!bs && !(salt && sh_salt_valid(salt, strlen(salt)))) {
                mb_error(out, "This mod folder has not been scanned yet; press Refresh in the Mods tab");
            } else {
                const char *id = bs ? bs->s : salt;
                memcpy(out->id, id, SH_SALT_LEN);
                out->id[SH_SALT_LEN] = 0;
                if (mb_descriptor_root(out, rootj, block, 1) && block) {
                    /* "built": the first successful build of this salt in this folder. */
                    pt = mbj_get(block, "built");
                    if (pt && pt->t == MBJ_NUM && mb_plain_integer(pt->s, pt->n) && pt->s[0] != '-')
                        prev_built = _strtoi64(pt->s, NULL, 10);
                    pt = mbj_get(block, "build");
                    if (mb_is_hex64(pt)) memcpy(out->prev_hash, pt->s, SH_MB_HASH_HEX + 1);
                }
            }
        }
        free(text);
    }
    mbj_free(rootj);
    free(desc_mod);
    free(desc_pkg);
    out->built = prev_built;
    out->rules = SH_MB_RULES;
    if (out->errors) goto finish;

    w.b = out;
    w.root = root;
    w.stopped = 0;
    w.stock = mb_stock() != NULL && mb_stock()->match != NULL;
    mb_walk_top(&w);
    if (!out->errors) {
        /* Identity: the folder's files live under mods/<its salt>/. */
        char cause[SH_MB_CAUSE_MAX];
        sh_mb_bundle names;
        memset(&names, 0, sizeof names);
        memcpy(names.id, out->id, sizeof names.id);
        names.files = out->files;
        names.file_count = out->file_count;
        if (!sh_mb_salt_paths_check(&names, cause, sizeof cause)) mb_error(out, "%s", cause);
    }
    if (!out->errors && out->extra && g_mb_extra_check) {
        /* The content blocks the game reads later ("scenes", "hud"),
         * checked now so a mistake is the build's cause. */
        char cause[SH_MB_CAUSE_MAX];
        if (!g_mb_extra_check(out->id, out->extra, strlen(out->extra), cause, sizeof cause)) mb_error(out, "%s", cause);
    }
    if (!out->errors) mb_check_files(out, root, resolver);

    if (!out->errors) mb_derive_requirements(out, 1);
    mb_finish(out);
    out->validated = out->errors == 0;
    if (out->validated && out->built <= 0) out->built = mb_now();
finish:
    free(root);
    return out->errors == 0;
}

/* ---------------------------------------------------------- tree hash ---- */

typedef struct mb_tent { char *rel; unsigned long long size, mtime; } mb_tent;
typedef struct mb_tlist { mb_tent *e; size_t n, cap; int failed; } mb_tlist;

static void mb_tree_walk(mb_tlist *l, const wchar_t *dir, const char *rel, int depth)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mb_wjoin(dir, L"*");
    HANDLE h;
    if (!pattern || depth > MB_WALK_DEPTH) { free(pattern); l->failed = 1; return; }
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *name, *child;
        size_t cn;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        name = sh_mb_utf8(fd.cFileName);
        if (!name) { l->failed = 1; continue; }
        cn = strlen(rel) + strlen(name) + 2;
        child = (char *)malloc(cn);
        if (!child) { free(name); l->failed = 1; continue; }
        _snprintf_s(child, cn, _TRUNCATE, "%s/%s", rel, name);
        free(name);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            wchar_t *sub = mb_wjoin(dir, fd.cFileName);
            if (sub) { mb_tree_walk(l, sub, child, depth + 1); free(sub); }
            free(child);
            continue;
        }
        if (l->n == l->cap) {
            size_t cap = l->cap ? l->cap * 2 : 256;
            mb_tent *grown = (mb_tent *)realloc(l->e, sizeof *grown * cap);
            if (!grown) { free(child); l->failed = 1; continue; }
            l->e = grown;
            l->cap = cap;
        }
        l->e[l->n].rel = child;
        l->e[l->n].size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        l->e[l->n].mtime = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        l->n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int mb_tent_cmp(const void *a, const void *b)
{
    return strcmp(((const mb_tent *)a)->rel, ((const mb_tent *)b)->rel);
}

int sh_mb_tree_hash(const char *folder, char out[SH_MB_HASH_HEX + 1])
{
    wchar_t *root = folder ? sh_mb_wpath(folder) : NULL, *p, *assets;
    unsigned char *text = NULL;
    char *own = NULL;
    size_t len = 0, own_len = 0, i;
    mb_tlist l;
    sh_sha256 s;
    int ok = 0;
    out[0] = 0;
    if (!root) return 0;
    memset(&l, 0, sizeof l);
    p = mb_wjoin(root, L"mod.json");
    if (p) text = mb_read_wfile(p, MB_MODJSON_MAX, &len);
    free(p);
    if (text) own = sh_mb_author_bytes((const char *)text, len, &own_len, NULL);
    if (!own) goto done;
    sh_sha256_init(&s);
    sh_sha256_update(&s, "snapmap-plus tree 1\n", 20);
    sh_sha256_update(&s, own, own_len);
    sh_sha256_update(&s, "\n", 1);
    assets = mb_wjoin(root, L"assets");
    if (assets) { mb_tree_walk(&l, assets, "assets", 1); free(assets); }
    if (l.failed) goto done;
    if (l.n > 1) qsort(l.e, l.n, sizeof *l.e, mb_tent_cmp);
    for (i = 0; i < l.n; i++) {
        char meta[64];
        int n = _snprintf_s(meta, sizeof meta, _TRUNCATE, " %llu %llu\n", l.e[i].size, l.e[i].mtime);
        sh_sha256_update(&s, l.e[i].rel, strlen(l.e[i].rel));
        if (n > 0) sh_sha256_update(&s, meta, (size_t)n);
    }
    sh_sha256_hex(&s, out, SH_MB_HASH_HEX);
    ok = 1;
done:
    for (i = 0; i < l.n; i++) free(l.e[i].rel);
    free(l.e);
    free(own);
    free(text);
    free(root);
    return ok;
}

/* ---------------------------------------------------- product record ---- */

/* The mods folder's named lock (store_lock.h), shared by every process that
 * writes the folder or its record. One mutex per folder serves both the
 * store lock (modstore_lock) and the record lock below; it is recursive
 * within a thread, so a store step that writes the record holds it once
 * more. Handles are opened once per folder and kept. */
static SRWLOCK g_lock_open = SRWLOCK_INIT;
static struct { wchar_t name[64]; HANDLE h; } g_lock_handles[32];
static unsigned g_lock_next;
static volatile LONG g_lock_waiting;   /* threads of this process waiting for it now */
static volatile LONG g_lock_waits;     /* waits so far */

void *sh_mb_store_mutex(const char *mods_root)
{
    wchar_t *w = mods_root && mods_root[0] ? sh_mb_wpath(mods_root) : NULL, name[64];
    HANDLE h = NULL;
    unsigned i;
    int named = w && sh_store_lock_name(w, name, 64);
    free(w);
    if (!named) return NULL;
    AcquireSRWLockExclusive(&g_lock_open);
    for (i = 0; i < 32 && !h; i++) if (g_lock_handles[i].h && !wcscmp(g_lock_handles[i].name, name)) h = g_lock_handles[i].h;
    if (!h && (h = CreateMutexW(NULL, FALSE, name)) != NULL) {
        /* Only a test walks more than 32 folders; an older handle stays open,
         * since a thread may still hold it. */
        i = g_lock_next++ % 32;
        wcscpy_s(g_lock_handles[i].name, 64, name);
        g_lock_handles[i].h = h;
    }
    ReleaseSRWLockExclusive(&g_lock_open);
    return h;
}

void sh_mb_store_wait(void *h)
{
    DWORD w = WaitForSingleObject(h, 0);
    if (w == WAIT_TIMEOUT) {
        /* The waiting count is raised before the wait count, so a holder that
         * sees a new wait always finds the waiter (modstore.c's yield). */
        InterlockedIncrement(&g_lock_waiting);
        InterlockedIncrement(&g_lock_waits);
        backend_log("MODS: store: waiting for another store operation to finish (an install, a download or a build)");
        w = WaitForSingleObject(h, INFINITE);
        InterlockedDecrement(&g_lock_waiting);
    }
    if (w == WAIT_ABANDONED)
        backend_log("MODS: store: another Snapmap+ process ended while it was changing the mods folder; continuing");
}

long sh_mb_store_waiting(void) { return InterlockedCompareExchange(&g_lock_waiting, 0, 0); }
unsigned sh_mb_store_waits(void) { return (unsigned)InterlockedCompareExchange(&g_lock_waits, 0, 0); }

static __declspec(thread) HANDLE t_rec_held;
static __declspec(thread) LONG   t_rec_depth;

void sh_mb_record_lock(const char *mods_root)
{
    HANDLE h;
    if (t_rec_depth > 0) { t_rec_depth++; return; }
    h = sh_mb_store_mutex(mods_root);
    if (h) sh_mb_store_wait(h);
    else backend_log("MODS: record: the mods folder's lock could not be opened; the record is read and written unguarded");
    t_rec_held = h;
    t_rec_depth = 1;
}

void sh_mb_record_unlock(void)
{
    if (t_rec_depth <= 0) return;
    if (--t_rec_depth == 0 && t_rec_held) {
        HANDLE h = t_rec_held;
        t_rec_held = NULL;
        ReleaseMutex(h);
    }
}

static wchar_t *mb_record_path(const char *mods_root)
{
    char path[MAX_PATH * 4];
    if (!mods_root || !mods_root[0]) return NULL;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" SH_MB_RECORD_FILE, mods_root);
    return sh_mb_wpath(path);
}

/* The record's root object: *state 1 read, 0 absent, -1 the file cannot be
 * read (`why` says why), -2 it is not a JSON object. Another program holding
 * the file without read sharing (a backup tool, an editor, a scanner) is
 * waited for, up to a second. */
static mbj *mb_record_read(const wchar_t *path, int *state, char *why, size_t why_cap)
{
    unsigned char *text = NULL;
    size_t len = 0;
    mbj *root;
    DWORD err = 0;
    int attempt;
    *state = 0;
    if (why && why_cap) why[0] = 0;
    if (!path || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return NULL;
    for (attempt = 0; attempt < 20; attempt++) {
        SetLastError(ERROR_SUCCESS);
        text = mb_read_wfile(path, 64u * 1024u * 1024u, &len);
        if (text) break;
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) break;
        Sleep(50);
    }
    if (!text) {
        *state = -1;
        if (why && why_cap) {
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
                _snprintf_s(why, why_cap, _TRUNCATE, "another program has it open");
            else if (err) _snprintf_s(why, why_cap, _TRUNCATE, "Windows error %lu", err);
            else _snprintf_s(why, why_cap, _TRUNCATE, "it is larger than 64 MiB");
        }
        return NULL;
    }
    root = mbj_parse((const char *)text, len, NULL, 0);
    free(text);
    if (!root || root->t != MBJ_OBJ) { mbj_free(root); *state = -2; return NULL; }
    *state = 1;
    return root;
}

static void mb_rec_log(const char *fmt, ...)
{
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

/* The sentence a caller shows the player about the last record read or write
 * this thread could not make: the file and the next step. */
static __declspec(thread) char g_rec_why[MAX_PATH * 4 + 240];

static void mb_rec_why(const char *name, const char *why)
{
    _snprintf_s(g_rec_why, sizeof g_rec_why, _TRUNCATE,
                "%s could not be used (%s); close the program holding it (a backup, sync or antivirus tool, or an "
                "editor) and try again", name, why && why[0] ? why : "it could not be read");
}

void sh_mb_record_why_clear(void) { g_rec_why[0] = 0; }

int sh_mb_record_why(char *out, size_t cap)
{
    if (!g_rec_why[0]) return 0;
    if (out && cap) strncpy_s(out, cap, g_rec_why, _TRUNCATE);
    return 1;
}

/* The record's path for a log line: UTF-8, without the \\?\ prefix. */
static void mb_record_name(const wchar_t *path, char *out, size_t cap)
{
    char *u = sh_mb_utf8(path && !wcsncmp(path, L"\\\\?\\", 4) ? path + 4 : path);
    strncpy_s(out, cap, u ? u : SH_MB_RECORD_FILE, _TRUNCATE);
    free(u);
}

/* Sections whose last read failed (root and section): a section read as
 * absent after a failure and written back would drop what it holds, so it
 * is not written until a read of it succeeds again. Under the record lock. */
#define MB_REC_FAILED_MAX 16
static unsigned long long g_rec_failed[MB_REC_FAILED_MAX];
static unsigned g_rec_failed_next;

static unsigned long long mb_rec_key(const char *mods_root, const char *section)
{
    unsigned long long h = 1469598103934665603ull;
    const char *p;
    for (p = mods_root; *p; p++) { h ^= (unsigned char)tolower((unsigned char)*p); h *= 1099511628211ull; }
    h ^= '|'; h *= 1099511628211ull;
    for (p = section; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ull; }
    return h ? h : 1;
}

static int mb_rec_failed_at(unsigned long long key)
{
    int i;
    for (i = 0; i < MB_REC_FAILED_MAX; i++) if (g_rec_failed[i] == key) return i;
    return -1;
}

int sh_mb_record_get(const char *mods_root, const char *section, char **json_out)
{
    wchar_t *path = mb_record_path(mods_root);
    mbj *root;
    const mbj *v;
    mb_buf o = {0};
    int state, r = 0, at;
    char why[96];
    unsigned long long key;
    if (json_out) *json_out = NULL;
    if (!path || !section) { free(path); return -1; }
    key = mb_rec_key(mods_root, section);
    sh_mb_record_lock(mods_root);
    root = mb_record_read(path, &state, why, sizeof why);
    at = mb_rec_failed_at(key);
    if (state == -1) {
        char name[MAX_PATH * 4];
        mb_record_name(path, name, sizeof name);
        if (at < 0) g_rec_failed[g_rec_failed_next++ % MB_REC_FAILED_MAX] = key;
        mb_rec_log("MODS: record: cannot read %s (%s); its \"%s\" section is not written until it reads again",
                        name, why, section);
        mb_rec_why(name, why);
    } else if (at >= 0) g_rec_failed[at] = 0;
    sh_mb_record_unlock();
    free(path);
    if (state < 0) return -1;
    v = root ? mbj_get(root, section) : NULL;
    if (v && mbj_canon(v, &o, NULL, 0) && !o.oom) {
        r = 1;
        if (json_out) { *json_out = o.p; o.p = NULL; }
    }
    free(o.p);
    mbj_free(root);
    return r;
}

int sh_mb_record_put(const char *mods_root, const char *section, const char *json)
{
    wchar_t *path = mb_record_path(mods_root), *dir;
    mbj *root, *value = NULL;
    mb_buf o = {0};
    char *pretty = NULL;
    size_t i;
    int state, placed = 0, ok = 0;
    char why[96], name[MAX_PATH * 4];
    if (!path || !section || !section[0] || !strcmp(section, "format")) { free(path); return 0; }
    if (json && !(value = mbj_parse(json, strlen(json), NULL, 0))) { free(path); return 0; }
    sh_mb_record_lock(mods_root);
    mb_record_name(path, name, sizeof name);
    if (mb_rec_failed_at(mb_rec_key(mods_root, section)) >= 0) {
        /* The caller's text was built from a failed read: writing it would
         * replace what the section holds with less. */
        mb_rec_log("MODS: record: the \"%s\" section of %s was not written: its last read failed, and the write would "
                   "drop what it holds; it is written once it reads again", section, name);
        if (!g_rec_why[0]) mb_rec_why(name, "it could not be read a moment ago");
        sh_mb_record_unlock();
        mbj_free(value);
        free(path);
        return 0;
    }
    root = mb_record_read(path, &state, why, sizeof why);
    if (state == -1) {
        mb_rec_log("MODS: record: the \"%s\" section of %s was not written: the record cannot be read (%s)", section, name,
                   why);
        mb_rec_why(name, why);
        sh_mb_record_unlock();
        mbj_free(value);
        free(path);
        return 0;
    }
    if (state < 0) {
        /* Never silently lost: the unreadable record is kept beside it. */
        wchar_t aside[MAX_PATH * 4];
        char aname[MAX_PATH * 4];
        _snwprintf_s(aside, MAX_PATH * 4, _TRUNCATE, L"%s.unreadable-%lld", path, mb_now());
        mb_record_name(aside, aname, sizeof aname);
        if (MoveFileExW(path, aside, MOVEFILE_REPLACE_EXISTING)) {
            mb_rec_log("MODS: record: %s is not valid JSON; it was kept as %s and a new record started (the published "
                       "maps' links and claims it held are in that file)", name, aname);

        } else {
            mb_rec_log("MODS: record: the \"%s\" section of %s was not written: the record is not valid JSON and could not "
                       "be moved aside (Windows error %lu)", section, name, GetLastError());
            mb_rec_why(name, "it is not valid JSON and could not be moved aside");
            sh_mb_record_unlock();
            mbj_free(value);
            free(path);
            return 0;
        }
    }
    mb_puts(&o, "{\"format\":1");
    for (i = 0; root && i < root->count; i++) {
        const mbj *v = root->items[i];
        if (root->klen[i] == 6 && memcmp(root->keys[i], "format", 6) == 0) continue;
        if (root->klen[i] == strlen(section) && memcmp(root->keys[i], section, root->klen[i]) == 0) {
            placed = 1;
            if (!value) continue;
            v = value;
        }
        mb_put(&o, ",", 1);
        mb_put_jstr(&o, root->keys[i], root->klen[i]);
        mb_put(&o, ":", 1);
        if (!mbj_canon(v, &o, NULL, 0)) o.oom = 1;
    }
    if (!placed && value) {
        mb_put(&o, ",", 1);
        mb_put_jcstr(&o, section);
        mb_put(&o, ":", 1);
        if (!mbj_canon(value, &o, NULL, 0)) o.oom = 1;
    }
    mb_put(&o, "}", 1);
    if (!o.oom) pretty = mb_pretty(o.p, o.n);
    if (pretty) {
        /* The store folder may not exist yet (a claim before any install). */
        dir = (wchar_t *)malloc(sizeof(wchar_t) * (wcslen(path) + 1));
        if (dir) {
            wchar_t *slash;
            wcscpy_s(dir, wcslen(path) + 1, path);
            slash = wcsrchr(dir, L'\\');
            if (slash) { *slash = 0; CreateDirectoryW(dir, NULL); }
            free(dir);
        }
        ok = mb_write_wfile(path, pretty, strlen(pretty));
        if (!ok) {
            DWORD err = GetLastError();
            _snprintf_s(why, sizeof why, _TRUNCATE, err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED
                        ? "another program has it open" : "Windows error %lu", err);
            mb_rec_log("MODS: record: the \"%s\" section of %s was not written (%s)", section, name, why);
            mb_rec_why(name, why);
        }
    }
    sh_mb_record_unlock();
    free(pretty);
    free(o.p);
    mbj_free(root);
    mbj_free(value);
    free(path);
    return ok;
}

