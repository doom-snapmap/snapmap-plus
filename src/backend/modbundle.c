/* modbundle.c -- see modbundle.h. Mod descriptor, tree walk, hashes, logical
 * names, decl needs, validation and the canonical bundle manifest. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "modbundle.h"
#include "config_json.h"
#include "decl_text.h"
#include "json_pretty.h"
#include "backend_log.h"

#define MB_JSON_DEPTH   32u
#define MB_WALK_DEPTH   48
#define MB_SCAN_DEPTH   128
#define MB_READ_CHUNK   (1u << 20)
/* With a stock reader, files over the caps or of a type a bundle cannot carry
 * are hashed before they are refused (a stock copy becomes a pointer), so the
 * walk itself stops at this many listed bytes. */
#define MB_MAX_WALK     (2048ull * 1024u * 1024u)

/* ------------------------------------------------------------ SHA-256 ---- */

typedef struct mb_sha256 {
    uint32_t h[8];
    unsigned char block[64];
    size_t fill;
    uint64_t bits;
} mb_sha256;

static const uint32_t MB_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define MB_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void mb_sha_init(mb_sha256 *s)
{
    static const uint32_t iv[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(s->h, iv, sizeof iv);
    s->fill = 0;
    s->bits = 0;
}

static void mb_sha_block(mb_sha256 *s, const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = MB_ROR(w[i - 15], 7) ^ MB_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = MB_ROR(w[i - 2], 17) ^ MB_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = MB_ROR(e, 6) ^ MB_ROR(e, 11) ^ MB_ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + MB_K[i] + w[i];
        uint32_t S0 = MB_ROR(a, 2) ^ MB_ROR(a, 13) ^ MB_ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void mb_sha_update(mb_sha256 *s, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    s->bits += (uint64_t)n * 8u;
    while (n) {
        if (s->fill == 0 && n >= 64) { mb_sha_block(s, p); p += 64; n -= 64; continue; }
        s->block[s->fill++] = *p++;
        n--;
        if (s->fill == 64) { mb_sha_block(s, s->block); s->fill = 0; }
    }
}

static void mb_sha_hex(mb_sha256 *s, char out[SH_MB_HASH_HEX + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint64_t bits = s->bits;
    unsigned char pad = 0x80, zero = 0, len[8];
    int i;
    mb_sha_update(s, &pad, 1);
    while (s->fill != 56) mb_sha_update(s, &zero, 1);
    for (i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    mb_sha_update(s, len, 8);
    for (i = 0; i < 32; i++) {
        unsigned char byte = (unsigned char)(s->h[i / 4] >> (24 - 8 * (i % 4)));
        out[i * 2] = hex[byte >> 4];
        out[i * 2 + 1] = hex[byte & 15];
    }
    out[SH_MB_HASH_HEX] = 0;
}

void sh_mb_sha256_hex(const void *data, size_t len, char out[SH_MB_HASH_HEX + 1])
{
    mb_sha256 s;
    mb_sha_init(&s);
    mb_sha_update(&s, data, len);
    mb_sha_hex(&s, out);
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
    mb_sha256 s;
    unsigned long long total = 0;
    int ok = 1;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    buf = (unsigned char *)malloc(MB_READ_CHUNK);
    if (!buf) { CloseHandle(h); return 0; }
    mb_sha_init(&s);
    for (;;) {
        if (!ReadFile(h, buf, MB_READ_CHUNK, &got, NULL)) { ok = 0; break; }
        if (got == 0) break;
        mb_sha_update(&s, buf, got);
        total += got;
    }
    free(buf);
    CloseHandle(h);
    if (!ok) return 0;
    mb_sha_hex(&s, out);
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

/* Canonical text of a value: sorted keys, no whitespace, integer numbers. */
static int mbj_canon(const mbj *v, mb_buf *b, char *cause, size_t cap)
{
    size_t i;
    switch (v->t) {
    case MBJ_NULL: mb_puts(b, "null"); return 1;
    case MBJ_BOOL: mb_puts(b, v->b ? "true" : "false"); return 1;
    case MBJ_STR: mb_put_jstr(b, v->s, v->n); return 1;
    case MBJ_NUM:
        if (!mb_plain_integer(v->s, v->n)) {
            if (cause) _snprintf_s(cause, cap, _TRUNCATE, "number %.32s is not a plain integer", v->s);
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

int sh_mb_id_normalize(const char *in, size_t len, char out[SH_MB_ID_MAX + 1])
{
    size_t i, n;
    char buf[SH_MB_ID_MAX + 1];
    if (!in) return 0;
    while (len && strchr(" \t\r\n", *in)) { in++; len--; }
    while (len && strchr(" \t\r\n", in[len - 1])) len--;
    if (len == 0 || len > SH_MB_ID_MAX) return 0;
    for (i = 0; i < len; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    buf[len] = 0;
    n = len;
    for (i = 0; i < n; i++) {
        char c = buf[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) continue;
        if (c != '.' && c != '_' && c != '-') return 0;
        if (i == 0 || i + 1 == n) return 0;
        if (c == '.' && buf[i - 1] == '.') return 0;
    }
    memcpy(out, buf, n + 1);
    return 1;
}

void sh_mb_id_sanitized(const char *id, char out[SH_MB_ID_MAX + 1])
{
    size_t i;
    for (i = 0; id[i] && i < SH_MB_ID_MAX; i++) {
        char c = id[i];
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '$' || c == '.' || c == '/' || c == '\\';
        out[i] = keep ? c : '_';
    }
    out[i] = 0;
}

static size_t mb_codepoints(const char *s, size_t n);

/* The store's rule: 1-32 code points, none of / \ : * ? " < > |, no control
 * characters, not ending in '.'. A leading or trailing space is also refused
 * because the version names a folder. Assumes valid UTF-8. */
int sh_mb_version_valid(const char *v)
{
    size_t i, n;
    if (!v) return 0;
    n = strlen(v);
    if (n == 0 || n > 4 * SH_MB_VERSION_MAX || mb_codepoints(v, n) > SH_MB_VERSION_MAX) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c < 0x20 || c == 0x7F || strchr("<>:\"/\\|?*", c)) return 0;
    }
    return v[n - 1] != '.' && v[n - 1] != ' ' && v[0] != ' ';
}

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
        if (!mb_path_char(c)) return mb_cause(cause, cap, "character 0x%02x not allowed in path: %s", (unsigned char)c, r);
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
        return mb_cause(cause, cap, "%s: a sound bank must be sound/soundbanks/pc/<stem>.bnk with a stem of at most %u "
                        "characters (Wwise opens banks by file name in that one folder)", request ? request : "",
                        (unsigned)SH_MB_BANK_STEM_MAX);
    for (i = 0; id[i] && i < SH_MB_ID_MAX; i++) prefix[i] = (id[i] == '.' || id[i] == '-') ? '_' : id[i];
    prefix[i] = '_';
    prefix[i + 1] = 0;
    pn = i + 1;
    n = strlen(stem);
    for (i = 0; i < n; i++) {
        char c = stem[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return mb_cause(cause, cap, "%s: a sound bank stem may hold only letters, digits and '_'", request);
        low[i] = c;
    }
    low[n] = 0;
    if (n <= pn || strncmp(low, prefix, pn) != 0)
        return mb_cause(cause, cap, "%s: a sound bank stem must start with '%s' (the mod id with '.' and '-' as '_') and "
                        "name something after it", request, prefix);
    /* The engine's listed-bank filter skips these in some game mode (doom_*_sp
     * in SnapMap, *_snapmaps outside it, *initial* and Init always), and the
     * product submits banks through that filter. */
    if (strstr(low, "_sp") || strstr(low, "_snapmaps") || strstr(low, "initial") || !strcmp(low, "init"))
        return mb_cause(cause, cap, "%s: a sound bank stem must not contain '_sp', '_snapmaps' or 'initial' (the "
                        "engine skips such banks)", request);
    return 1;
}

int sh_mb_bank_header_ok(const unsigned char *bytes, size_t len, const char *stem, char *cause, size_t cap)
{
    uint32_t size, version, id, want;
    if (!bytes || len < 16 || memcmp(bytes, "BKHD", 4) != 0)
        return mb_cause(cause, cap, "%s.bnk does not start with a Wwise BKHD chunk", stem);
    memcpy(&size, bytes + 4, 4);
    memcpy(&version, bytes + 8, 4);
    memcpy(&id, bytes + 12, 4);
    if (size < 8 || (size_t)size + 8 > len)
        return mb_cause(cause, cap, "%s.bnk has a truncated BKHD chunk", stem);
    if (version != SH_MB_BANK_VERSION)
        return mb_cause(cause, cap, "%s.bnk is Wwise bank version %u; this game reads only version %u", stem,
                        (unsigned)version, (unsigned)SH_MB_BANK_VERSION);
    want = sh_mb_fnv1_lower(stem, strlen(stem));
    if (id != want)
        return mb_cause(cause, cap, "%s.bnk carries bank id %u, but the game loads it by name as id %u (FNV-1 of the "
                        "stem); rebuild it under this name", stem, (unsigned)id, (unsigned)want);
    return 1;
}

/* Contract 1 logical-name table, on a lower-cased request path. */
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
        return mb_binary_name("cooked", "model", name, "bmodel", out[0], SH_MB_REQUEST_MAX + 1);
    }
    if (sh_mb_decl_type(type, tl)) {
        int w = _snprintf_s(out[0], SH_MB_REQUEST_MAX + 1, _TRUNCATE, "generated/decls/%s/%s.decl", type, name);
        if (w < 0) return 0;
        mb_lower(out[0]);
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
 * damage and projectile decls, inventory items, particles, skins, ".break"
 * models, table references inside material expressions, md6Def meshKits, and
 * every other reference key. */

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
    int is_md6, is_sedef, is_material;
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

static void mb_warn(sh_mb_bundle *b, const char *fmt, ...)
{
    va_list ap;
    if (b->warnings++ == 0) {
        va_start(ap, fmt);
        _vsnprintf_s(b->first_warning, sizeof b->first_warning, _TRUNCATE, fmt, ap);
        va_end(ap);
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
    for (i = 0; i < b->stock_decl_count; i++) { free(b->stock_decls[i].request); free(b->stock_decls[i].source); }
    free(b->stock_decls);
    free(b->name);
    free(b->author);
    free(b->homepage);
    free(b->requirements);
    free(b->extra);
    free(b->canonical);
    memset(b, 0, sizeof *b);
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

static const char *const MB_GATES[] = { "g_useImageBlackList", "g_useResourceBlackList", NULL };

/* requirements: {"cvars": {<supported gate>: "0" | 0}} -> canonical text. */
static char *mb_requirements(const mbj *req, char *cause, size_t cap)
{
    const mbj *cvars;
    mb_buf out = {0};
    size_t i, *order;
    if (req->t != MBJ_OBJ) { mb_cause(cause, cap, "requirements must be an object"); return NULL; }
    for (i = 0; i < req->count; i++)
        if (!(req->klen[i] == 5 && memcmp(req->keys[i], "cvars", 5) == 0)) {
            mb_cause(cause, cap, "requirements key %.64s is not supported (only cvars)", req->keys[i]);
            return NULL;
        }
    cvars = mbj_get(req, "cvars");
    if (!cvars) { mb_cause(cause, cap, "requirements has no cvars"); return NULL; }
    if (cvars->t != MBJ_OBJ) { mb_cause(cause, cap, "requirements.cvars must be an object"); return NULL; }
    for (i = 0; i < cvars->count; i++) {
        const mbj *v = cvars->items[i];
        int gate = 0, k;
        for (k = 0; MB_GATES[k]; k++)
            if (strlen(MB_GATES[k]) == cvars->klen[i] && memcmp(MB_GATES[k], cvars->keys[i], cvars->klen[i]) == 0) gate = 1;
        if (!gate) {
            mb_cause(cause, cap, "requirements.cvars.%.64s is not a supported gate (g_useImageBlackList, g_useResourceBlackList)",
                     cvars->keys[i]);
            return NULL;
        }
        if (!((v->t == MBJ_STR && strcmp(v->s, "0") == 0) || (v->t == MBJ_NUM && strcmp(v->s, "0") == 0))) {
            mb_cause(cause, cap, "requirements.cvars.%.64s must be \"0\"", cvars->keys[i]);
            return NULL;
        }
    }
    order = (size_t *)malloc(sizeof(size_t) * (cvars->count ? cvars->count : 1));
    if (!order) return NULL;
    for (i = 0; i < cvars->count; i++) order[i] = i;
    qsort_s(order, cvars->count, sizeof(size_t), mb_key_cmp, (void *)cvars);
    mb_puts(&out, "{\"cvars\":{");
    for (i = 0; i < cvars->count; i++) {
        if (i) mb_put(&out, ",", 1);
        mb_put_jstr(&out, cvars->keys[order[i]], cvars->klen[order[i]]);
        mb_puts(&out, ":\"0\"");
    }
    mb_puts(&out, "}}");
    free(order);
    if (out.oom) { free(out.p); return NULL; }
    return out.p;
}

static int mb_set_shadows(sh_mb_bundle *b, const mbj *arr, int from_manifest)
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
        if (!from_manifest) {
            for (char *p = s; *p; p++) if (*p == '\\') *p = '/';
            mb_lower(s);
        }
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
    if (!from_manifest) {
        size_t w = 0;
        qsort(b->shadows, b->shadow_count, sizeof(char *), mb_str_cmp);
        for (i = 0; i < b->shadow_count; i++) {
            if (w && strcmp(b->shadows[w - 1], b->shadows[i]) == 0) { free(b->shadows[i]); continue; }
            b->shadows[w++] = b->shadows[i];
        }
        b->shadow_count = w;
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
        _snprintf_s(cause, cap, _TRUNCATE, "%s points at installed content %s, which cannot be checked (the "
                    "installed-content reader is unavailable)", who, source);
    else if (r == SH_MB_STOCK_MISMATCH)
        _snprintf_s(cause, cap, _TRUNCATE, "%s points at installed content %s, which this game holds with other "
                    "bytes (a different game revision)", who, source);
    else
        _snprintf_s(cause, cap, _TRUNCATE, "%s points at installed content %s, which this game does not have",
                    who, source);
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

static int mb_hex64_text(const char *s, size_t n, char out[SH_MB_HASH_HEX + 1])
{
    size_t i;
    if (n != SH_MB_HASH_HEX) return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
        out[i] = c;
    }
    out[n] = 0;
    return 1;
}

static int mb_decl_cmp(const void *a, const void *b)
{
    return strcmp(((const sh_mb_stock_decl *)a)->request, ((const sh_mb_stock_decl *)b)->request);
}

/* The descriptor's "stock" object: {"<request>": {"source": "...", "sha256": "..."}}. */
static int mb_set_stock_decls(sh_mb_bundle *b, const mbj *o)
{
    static const char *const keys[] = { "sha256", "source", NULL };
    char cause[SH_MB_CAUSE_MAX];
    size_t i, k;
    if (o->t != MBJ_OBJ) { mb_error(b, "%s stock must be an object of request paths", b->descriptor); return 0; }
    if (o->count > SH_MB_MAX_FILES) { mb_error(b, "%s stock names more than %u files", b->descriptor, SH_MB_MAX_FILES); return 0; }
    b->stock_decls = (sh_mb_stock_decl *)calloc(o->count ? o->count : 1, sizeof(sh_mb_stock_decl));
    if (!b->stock_decls) { mb_error(b, "out of memory"); return 0; }
    for (i = 0; i < o->count; i++) {
        const mbj *v = o->items[i], *src, *sha;
        sh_mb_stock_decl *d = &b->stock_decls[b->stock_decl_count];
        size_t kk;
        int bad = 0;
        if (v->t != MBJ_OBJ) { mb_error(b, "%s stock.%.200s must be {\"source\", \"sha256\"}", b->descriptor, o->keys[i]); return 0; }
        for (kk = 0; kk < v->count; kk++) {
            int hit = 0;
            for (k = 0; keys[k]; k++)
                if (strlen(keys[k]) == v->klen[kk] && memcmp(keys[k], v->keys[kk], v->klen[kk]) == 0) hit = 1;
            if (!hit) bad = 1;
        }
        src = mbj_get(v, "source");
        sha = mbj_get(v, "sha256");
        if (bad || !src || src->t != MBJ_STR || strlen(src->s) != src->n || (sha && sha->t != MBJ_STR)) {
            mb_error(b, "%s stock.%.200s must be {\"source\", \"sha256\"} with string values", b->descriptor, o->keys[i]);
            return 0;
        }
        d->request = mb_path_dup(o->keys[i], o->klen[i]);
        d->source = mb_path_dup(src->s, src->n);
        b->stock_decl_count++;
        if (!d->request || !d->source) { mb_error(b, "out of memory"); return 0; }
        if (mb_starts(d->request, "assets/")) {
            mb_error(b, "%s stock entry %s must be a request path, without assets/", b->descriptor, d->request);
            return 0;
        }
        if (!sh_mb_check_request(d->request, cause, sizeof cause)) { mb_error(b, "%s stock entry: %s", b->descriptor, cause); return 0; }
        if (!sh_mb_check_source(d->source, cause, sizeof cause)) {
            mb_error(b, "%s stock.%s source: %s", b->descriptor, d->request, cause);
            return 0;
        }
        if (sha && !mb_hex64_text(sha->s, sha->n, d->sha256)) {
            mb_error(b, "%s stock.%s sha256 must be 64 hex characters", b->descriptor, d->request);
            return 0;
        }
    }
    qsort(b->stock_decls, b->stock_decl_count, sizeof(sh_mb_stock_decl), mb_decl_cmp);
    for (i = 1; i < b->stock_decl_count; i++)
        if (strcmp(b->stock_decls[i - 1].request, b->stock_decls[i].request) == 0) {
            mb_error(b, "%s stock names %s twice", b->descriptor, b->stock_decls[i].request);
            return 0;
        }
    return 1;
}

/* Fields shared by descriptors and manifests. kind 0 = descriptor, 1 = manifest. */
static int mb_common_fields(sh_mb_bundle *b, const mbj *root, int manifest)
{
    const mbj *id = mbj_get(root, "id"), *ver = mbj_get(root, "version"), *name = mbj_get(root, "name");
    const mbj *author = mbj_get(root, "author"), *home = mbj_get(root, "homepage");
    const mbj *req = mbj_get(root, "requirements"), *sh = mbj_get(root, "shadows");
    const mbj *fmt = mbj_get(root, "format");
    const char *what = manifest ? "bundle.json" : b->descriptor;
    char cause[SH_MB_CAUSE_MAX];

    if (fmt || manifest) {
        if (!fmt || fmt->t != MBJ_NUM || !mb_plain_integer(fmt->s, fmt->n)) {
            mb_error(b, "%s format must be the integer %d", what, SH_MB_FORMAT);
            return 0;
        }
        if (strcmp(fmt->s, "1") != 0) {
            mb_error(b, "%s format %s is not supported by this release (it reads format %d)", what, fmt->s, SH_MB_FORMAT);
            return 0;
        }
    }
    if (!id) { mb_error(b, "%s is missing its required id", what); return 0; }
    if (id->t != MBJ_STR || strlen(id->s) != id->n || !sh_mb_id_normalize(id->s, id->n, b->id) ||
        (manifest && strcmp(id->s, b->id) != 0)) {
        mb_error(b, "%s id must be 1-64 characters of a-z, 0-9 and internal '.', '_' or '-', without \"..\"", what);
        return 0;
    }
    if (ver) {
        char *v;
        if (ver->t != MBJ_STR || strlen(ver->s) != ver->n) { mb_error(b, "%s version must be a string", what); return 0; }
        v = manifest ? mb_strdup(ver->s) : mb_trimmed_dup(ver);
        if (!v || !sh_mb_version_valid(v)) {
            mb_error(b, "%s version must be 1-32 characters without /\\:*?\"<>| or control characters, not ending in '.' or a space", what);
            free(v);
            return 0;
        }
        strcpy_s(b->version, sizeof b->version, v);
        free(v);
    } else if (manifest) {
        mb_error(b, "bundle.json has no version");
        return 0;
    } else {
        strcpy_s(b->version, sizeof b->version, "0.0.0");
    }
    if (!name || name->t != MBJ_STR || strlen(name->s) != name->n) {
        mb_error(b, "%s is missing its required name", what);
        return 0;
    }
    b->name = manifest ? mb_strdup(name->s) : mb_trimmed_dup(name);
    if (!b->name || !b->name[0] || mb_codepoints(b->name, strlen(b->name)) > SH_MB_NAME_MAX) {
        mb_error(b, "%s name must be 1-%u characters", what, SH_MB_NAME_MAX);
        return 0;
    }
    if (author) {
        if (author->t != MBJ_STR || strlen(author->s) != author->n) { mb_error(b, "%s author must be a string", what); return 0; }
        b->author = manifest ? mb_strdup(author->s) : mb_trimmed_dup(author);
        if (!b->author || mb_codepoints(b->author, strlen(b->author)) > SH_MB_AUTHOR_MAX) {
            mb_error(b, "%s author must be at most %u characters", what, SH_MB_AUTHOR_MAX);
            return 0;
        }
        if (!b->author[0]) { free(b->author); b->author = NULL; }
    }
    if (home) {
        if (home->t != MBJ_STR || strlen(home->s) != home->n) { mb_error(b, "%s homepage must be a string", what); return 0; }
        b->homepage = manifest ? mb_strdup(home->s) : mb_trimmed_dup(home);
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
    if (sh && !mb_set_shadows(b, sh, manifest)) return 0;
    return 1;
}

/* The author block of a descriptor (mod.json without its "snapmap-plus"
 * member, or package.json). `block` is the product block of the same file, or
 * NULL: a "legacy" object there (a converted first-generation package, whose
 * author keys are the package's own) joins "extra" as if the author had
 * written it, and its "shadows" join the author's, so a rebuild keeps the
 * grandfathered names. */
static int mb_descriptor_root(sh_mb_bundle *b, const mbj *root, const mbj *block)
{
    char cause[SH_MB_CAUSE_MAX];
    mb_buf extra = {0};
    size_t i, kept = 0;
    const mbj *block_legacy = NULL;
    if (root->t != MBJ_OBJ) { mb_error(b, "%s must contain one JSON object", b->descriptor); return 0; }
    if (mbj_get(root, "files") || mbj_get(root, "bundle")) {
        mb_error(b, "%s may not contain files or bundle; they are computed when the bundle is built", b->descriptor);
        return 0;
    }
    if (block && block->t != MBJ_OBJ) { mb_error(b, "%s \"" SH_MB_BLOCK_KEY "\" must be an object", b->descriptor); return 0; }
    if (!mb_common_fields(b, root, 0)) return 0;
    {
        /* A converted first-generation package (modlegacy.c) carries a
         * "legacy" object; it stays in "extra" and grandfathers its names. */
        const mbj *lg = mbj_get(root, "legacy");
        if (lg && lg->t != MBJ_OBJ) { mb_error(b, "%s legacy must be an object", b->descriptor); return 0; }
        b->legacy = lg != NULL;
        if (!lg && block) {
            block_legacy = mbj_get(block, "legacy");
            if (block_legacy && block_legacy->t != MBJ_OBJ) {
                mb_error(b, "%s \"" SH_MB_BLOCK_KEY "\".legacy must be an object", b->descriptor);
                return 0;
            }
            if (block_legacy) { b->legacy = 1; b->block_legacy = 1; }
        }
    }
    if (block_legacy) {
        /* The grandfathered shadows the product recorded join the author's. */
        const mbj *own = mbj_get(root, "shadows"), *kept_list = mbj_get(block, "shadows");
        if (kept_list && kept_list->t == MBJ_ARR && kept_list->count) {
            mbj merged;
            int set;
            memset(&merged, 0, sizeof merged);
            merged.t = MBJ_ARR;
            for (i = 0; own && own->t == MBJ_ARR && i < own->count; i++)
                if (!mbj_push(&merged, own->items[i], NULL, 0)) { free(merged.items); mb_error(b, "out of memory"); return 0; }
            for (i = 0; i < kept_list->count; i++)
                if (!mbj_push(&merged, kept_list->items[i], NULL, 0)) { free(merged.items); mb_error(b, "out of memory"); return 0; }
            for (i = 0; i < b->shadow_count; i++) free(b->shadows[i]);
            free(b->shadows);
            b->shadows = NULL;
            b->shadow_count = 0;
            set = mb_set_shadows(b, &merged, 0);
            free(merged.items);   /* the items belong to root and block */
            if (!set) return 0;
        }
    }
    {
        const mbj *st = mbj_get(root, "stock");
        if (st && !mb_set_stock_decls(b, st)) return 0;
    }
    /* Everything else (the legacy strings and hud keys, composes, description,
     * an era B schema) is preserved verbatim under "extra". */
    {
        mbj tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.t = MBJ_OBJ;
        for (i = 0; i < root->count; i++) {
            static const char *const known[] = { "format", "id", "version", "name", "author", "homepage",
                                                 "requirements", "shadows", "stock", SH_MB_BLOCK_KEY, NULL };
            int k, is_known = 0;
            for (k = 0; known[k]; k++)
                if (strlen(known[k]) == root->klen[i] && memcmp(known[k], root->keys[i], root->klen[i]) == 0) is_known = 1;
            if (is_known) continue;
            if ((root->klen[i] == 7 && memcmp(root->keys[i], "strings", 7) == 0 && root->items[i]->t != MBJ_OBJ) ||
                (root->klen[i] == 3 && memcmp(root->keys[i], "hud", 3) == 0 && root->items[i]->t != MBJ_OBJ)) {
                mb_error(b, "%s %s must be an object", b->descriptor, root->keys[i]);
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
        if (block_legacy) {
            if (!mbj_push(&tmp, (mbj *)block_legacy, (char *)"legacy", 6)) {
                free(tmp.items); free(tmp.keys); free(tmp.klen);
                mb_error(b, "out of memory");
                return 0;
            }
            kept++;
        }
        if (kept) {
            if (!mbj_canon(&tmp, &extra, cause, sizeof cause)) {
                mb_error(b, "%s: %s", b->descriptor, cause);
                free(tmp.items); free(tmp.keys); free(tmp.klen);
                free(extra.p);
                return 0;
            }
            b->extra = extra.p;
        }
        free(tmp.items); free(tmp.keys); free(tmp.klen);   /* items and keys belong to root and block */
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

static int mb_is_top_allowed(const char *name)
{
    /* The descriptor and mod.json's write-through temporary. The retired
     * bundle.json, bundle.canonical.json and .state.json of a folder the store
     * upgrade has not reached yet are tolerated. */
    return _stricmp(name, "mod.json") == 0 || _stricmp(name, "package.json") == 0 ||
           _strnicmp(name, "mod.json.tmp", 12) == 0 ||
           _stricmp(name, "bundle.json") == 0 || _stricmp(name, "bundle.canonical.json") == 0 ||
           _stricmp(name, ".state.json") == 0;
}

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
        if (GetLastError() != ERROR_FILE_NOT_FOUND) mb_error(w->b, "cannot list %s (error %lu)", rel, GetLastError());
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
            } else if (!sh_mb_extension_allowed(req_lower) && !(defer && (deferred = 1))) {
                if (sh_mb_pointer_only_kind(req_lower))
                    mb_error(w->b, "%s can only be a stock pointer to the installed game, never carried "
                             "(the game's resource reader is unavailable)", child);
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
                mb_error(w->b, "assets/ larger than %llu MiB, counting copies of installed content", MB_MAX_WALK >> 20);
                w->stopped = 1;
            } else if ((f = mb_add_file(w->b)) == NULL) {
                mb_error(w->b, "out of memory");
            } else {
                f->path = mb_strdup(child);
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
    if (h == INVALID_HANDLE_VALUE) { mb_error(w->b, "cannot list the mod folder (error %lu)", GetLastError()); return; }
    do {
        char *name;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        name = sh_mb_utf8(fd.cFileName);
        if (!name) { mb_error(w->b, "a file name in the mod folder is not valid Unicode"); continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            mb_error(w->b, "symbolic link or junction not allowed: %s", name);
        } else if (_stricmp(name, "assets") == 0 && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t *sub = mb_wjoin(w->root, fd.cFileName);
            have_assets = 1;
            if (sub) { mb_walk_dir(w, sub, "assets", 1); free(sub); }
            else mb_error(w->b, "out of memory");
        } else if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !mb_is_top_allowed(name)) {
            mb_error(w->b, "only the descriptor and assets/ may be in the mod folder: %s", name);
        }
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!have_assets) mb_error(w->b, "the mod folder has no assets/ folder");
}

/* The descriptor's stock pointers join the walked files: a pointer whose path
 * is also carried marks that file (its bytes must be the installed ones), any
 * other adds a file with nothing on disk. */
static void mb_add_declared(sh_mb_bundle *b)
{
    size_t k, i;
    for (k = 0; k < b->stock_decl_count; k++) {
        const sh_mb_stock_decl *d = &b->stock_decls[k];
        sh_mb_file *f = NULL;
        for (i = 0; i < b->file_count; i++)
            if (strcmp(b->files[i].request, d->request) == 0) { f = &b->files[i]; break; }
        if (!f) {
            size_t n = strlen(d->request) + 8;
            if (b->file_count >= SH_MB_MAX_FILES) { mb_error(b, "more than %u files under assets/", SH_MB_MAX_FILES); return; }
            f = mb_add_file(b);
            if (!f) { mb_error(b, "out of memory"); return; }
            f->path = (char *)malloc(n);
            f->request = mb_strdup(d->request);
            if (!f->path || !f->request) { mb_error(b, "out of memory"); return; }
            _snprintf_s(f->path, n, _TRUNCATE, "assets/%s", d->request);
            f->stock = 1;
            f->absent = 1;
        }
        f->declared = (int)k + 1;
    }
}

/* Resolve a declared pointer against the installed game: its hash (checked
 * when the author gave one) and size. Returns 1 when resolved. */
static int mb_resolve_declared(sh_mb_bundle *b, sh_mb_file *f, const sh_mb_stock_decl *d,
                               char sha[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    const sh_mb_stock_ops *ops = mb_stock();
    char cause[SH_MB_CAUSE_MAX];
    int r;
    *size = 0;
    sha[0] = 0;
    if (d->sha256[0]) {
        r = sh_mb_stock_verify(d->source, d->sha256, size, f->path, cause, sizeof cause);
        if (r == SH_MB_STOCK_OK) { memcpy(sha, d->sha256, SH_MB_HASH_HEX + 1); return 1; }
    } else {
        r = ops && ops->identify ? ops->identify(ops->context, d->source, sha, size) : SH_MB_STOCK_UNAVAILABLE;
        if (r == SH_MB_STOCK_OK && strlen(sha) == SH_MB_HASH_HEX) return 1;
        if (r == SH_MB_STOCK_OK) r = SH_MB_STOCK_UNAVAILABLE;
        mb_stock_cause(r, d->source, f->path, cause, sizeof cause);
    }
    mb_error(b, "%s", cause);
    return 0;
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
    int heap = 0, strict = !rp->b->legacy;
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
     * missing files only warn; a legacy bundle's refusals only warn. */
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        int strict, perms = 0;
        if (!f->is_decl || !f->ptype || !f->pname || strcmp(f->ptype, "renderProg") != 0) continue;
        ctx.used[i] = 1;
        strict = !b->legacy && !f->shadowed;
        if (f->shadowed)
            mb_warn(b, "%s replaces the built-in render program %s for every material that names it", f->path, f->pname);
        if (mb_rp_last_has_dot(f->pname, strlen(f->pname))) {
            MB_RP_ERR(!b->legacy, "%s: program %s has a '.' in its last component, which the engine's file names cut",
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
                MB_RP_ERR(!b->legacy, "%s is a permutation of %s, which the bundle does not ship", f->path, program);
            else
                MB_RP_ERR(!b->legacy, "%s: a permutation decl is named <program>_<16 hex digits>.decl", f->path);
        } else if (mb_starts(r, "generated/renderprogs/") && mb_ends(r, ".bin")) {
            MB_RP_ERR(!b->legacy, "%s: no render program in the bundle reads it (ship its decl)", f->path);
        } else if (mb_starts(r, "generated/spirv/")) {
            mb_rp_check_spirv(rp, f, NULL, 0, !b->legacy);
            mb_warn(b, "%s: no render program in the bundle binds it", f->path);
        }
    }
    free(ctx.used);
}

static void mb_check_files(sh_mb_bundle *b, const wchar_t *root, const sh_mb_resolver *r)
{
    char sanitized[SH_MB_ID_MAX + 1], ns_decl[SH_MB_ID_MAX + 8], ns_raw[SH_MB_ID_MAX + 8],
         ns_raw_s[SH_MB_ID_MAX + 8];
    mb_req_index own = { NULL, 0 };
    sh_mb_resolver no_stock = { NULL, NULL, NULL, NULL };
    const sh_mb_stock_ops *stock = mb_stock();
    size_t i, k;

    sh_mb_id_sanitized(b->id, sanitized);
    _snprintf_s(ns_decl, sizeof ns_decl, _TRUNCATE, "mods/%s/", b->id);
    _snprintf_s(ns_raw, sizeof ns_raw, _TRUNCATE, "/mods/%s/", b->id);
    _snprintf_s(ns_raw_s, sizeof ns_raw_s, _TRUNCATE, "/mods/%s/", sanitized);

    qsort(b->files, b->file_count, sizeof(sh_mb_file), mb_file_cmp);
    own.req = (const char **)malloc(sizeof(char *) * (b->file_count ? b->file_count : 1));
    if (!own.req) { mb_error(b, "out of memory"); return; }
    for (i = 0; i < b->file_count; i++) own.req[i] = b->files[i].request;
    own.n = b->file_count;
    qsort(own.req, own.n, sizeof(char *), mb_req_cmp);
    for (i = 1; i < own.n; i++)
        if (strcmp(own.req[i - 1], own.req[i]) == 0) mb_error(b, "two files have the request path %s", own.req[i]);

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

        if (f->stock) {
            /* A declared pointer with nothing carried: the installed bytes
             * are the file (a decl's text is read to scan its needs). */
            const sh_mb_stock_decl *d = &b->stock_decls[f->declared - 1];
            unsigned long long sz = 0;
            if (!mb_resolve_declared(b, f, d, f->blob, &sz)) continue;
            f->size = sz;
            f->stock_source = mb_strdup(d->source);
            if (!f->stock_source) { mb_error(b, "out of memory"); continue; }
            if (f->is_decl || f->is_module) {
                unsigned char *text = NULL;
                size_t len = 0;
                char cause[SH_MB_CAUSE_MAX];
                if (sh_mb_stock_read(d->source, f->blob, &text, &len, f->path, cause, sizeof cause) != SH_MB_STOCK_OK) {
                    mb_error(b, "%s", cause);
                    continue;
                }
                if (!sh_decl_text_well_formed(text, len)) {
                    mb_error(b, "%s points at %s, which is not a well-formed decl", f->path, d->source);
                } else if (f->is_module) {
                    /* A module's entity texts are quoted strings: nothing to scan. */
                } else {
                    mb_need_ctx nc = { f, 0 };
                    sh_mb_scan_needs(f->ptype, text, len, mb_need_cb, &nc);
                    if (nc.oom) mb_error(b, "out of memory");
                    qsort(f->needs, f->need_count, sizeof(sh_mb_need), mb_need_cmp);
                }
                HeapFree(GetProcessHeap(), 0, text);
            }
            goto names;
        }

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
                sh_mb_scan_needs(f->ptype, text, len, mb_need_cb, &nc);
                if (nc.oom) mb_error(b, "out of memory");
                qsort(f->needs, f->need_count, sizeof(sh_mb_need), mb_need_cmp);
            }
            free(text);
        } else if (mb_ends(f->request, ".bnk") && !b->legacy) {
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
        } else if (!mb_hash_wfile(full, &got, f->blob)) {
            mb_error(b, "cannot read %s", f->path);
            free(full);
            continue;
        }
        free(full);
        if (got != f->size) { mb_error(b, "%s changed while the bundle was built", f->path); continue; }

        /* A carried copy of installed content becomes a stock pointer: one the
         * descriptor declares must hold exactly the installed bytes, any other
         * is matched by size and hash (path first, then renamed copies). */
        if (f->declared) {
            const sh_mb_stock_decl *d = &b->stock_decls[f->declared - 1];
            char sha[SH_MB_HASH_HEX + 1];
            unsigned long long sz = 0;
            if (!mb_resolve_declared(b, f, d, sha, &sz)) continue;
            if (sz != f->size || strcmp(sha, f->blob) != 0) {
                mb_error(b, "%s is carried with other bytes than the installed %s its stock pointer names", f->path, d->source);
                continue;
            }
            f->stock = 1;
            f->stock_source = mb_strdup(d->source);
            if (!f->stock_source) { mb_error(b, "out of memory"); continue; }
        } else if (stock && stock->match) {
            char source[SH_MB_REQUEST_MAX + 1], cause[SH_MB_CAUSE_MAX];
            if (stock->match(stock->context, f->request, f->size, f->blob, source, sizeof source) == SH_MB_STOCK_OK &&
                sh_mb_check_source(source, cause, sizeof cause)) {
                f->stock = 1;
                f->stock_source = mb_strdup(source);
                if (!f->stock_source) { mb_error(b, "out of memory"); continue; }
            }
        }
        if (!f->stock && f->deferred) {
            if (f->deferred == 1 && sh_mb_pointer_only_kind(f->request))
                mb_error(b, "%s is not installed content: a .sbsp, .ambientsh, .tome or .pages file can only be a "
                         "stock pointer to the installed game, never carried", f->path);
            else if (f->deferred == 1) mb_error(b, "file type not allowed: %s", f->path);
            else mb_error(b, "file larger than 64 MiB: %s", f->path);
            continue;
        }

names:
        /* Namespace: new names live under the mod's own id. A skins decl is
         * named by the mesh path it skins, so it passes the way a raw file
         * does: the name contains /mods/<id>/ or the engine's '_' spelling.
         * A pointer restoring installed content under the game's own name
         * replaces nothing and is exempt. A module lives in maps/modules/mods/<id>/. */
        if (f->shadowed && mb_ends(f->request, ".bnk") && !b->legacy)
            mb_error(b, "%s: a stock sound bank cannot be replaced (the game loads every stock bank before mods are read)",
                     f->path);
        if (f->stock && f->stock_source && sh_mb_stock_restore(f->request, f->stock_source)) {
            /* restored stock content */
        } else if (f->is_module && !f->shadowed) {
            char mod_ns[SH_MB_ID_MAX + 32];
            _snprintf_s(mod_ns, sizeof mod_ns, _TRUNCATE, "maps/modules/mods/%s/", b->id);
            if (!mb_starts(f->request, mod_ns))
                mb_error(b, "%s: a snapModule decl must be maps/modules/mods/%s/<name>.decl", f->path, b->id);
        } else if (!f->shadowed) {
            if (f->is_decl && _stricmp(f->ptype, "skins") == 0) {
                char slashed[SH_MB_REQUEST_MAX + 18];
                _snprintf_s(slashed, sizeof slashed, _TRUNCATE, "/%s", f->pname);
                for (char *p = slashed; *p; p++) *p = (char)tolower((unsigned char)*p);
                if (!strstr(slashed, ns_raw) && !strstr(slashed, ns_raw_s))
                    mb_error(b, "%s: skins decl %s does not name a mesh under /mods/%s/ and is not listed in shadows",
                             f->path, f->pname, b->id);
            } else if (f->is_decl) {
                if (!mb_starts(f->pname, ns_decl))
                    mb_error(b, "%s: decl %s %s is outside mods/%s/ and is not listed in shadows",
                             f->path, f->ptype, f->pname, b->id);
            } else if (mb_ends(f->request, ".bnk") && !b->legacy) {
                /* A converted first-generation package keeps the raw-file rule
                 * below: its banks are carried, never delivered to Wwise. */
                char cause[SH_MB_CAUSE_MAX];
                if (f->stock) mb_error(b, "%s: a sound bank is carried, never a pointer to installed content", f->path);
                else if (!sh_mb_bank_rule(f->request, b->id, cause, sizeof cause)) mb_error(b, "%s", cause);
            } else {
                char slashed[SH_MB_REQUEST_MAX + 2];
                _snprintf_s(slashed, sizeof slashed, _TRUNCATE, "/%s", f->request);
                if (!strstr(slashed, ns_raw) && !strstr(slashed, ns_raw_s))
                    mb_error(b, "%s: the path does not contain /mods/%s/ and is not listed in shadows", f->path, b->id);
                /* A family member is named by its family, which must be the mod's own. */
                else if (sh_mb_vt_member(f->request, NULL, 0, NULL) &&
                         _strnicmp(slashed + 16, ns_raw, strlen(ns_raw)) != 0 &&
                         _strnicmp(slashed + 16, ns_raw_s, strlen(ns_raw_s)) != 0)
                    mb_error(b, "%s: a virtual-texture family member must be virtualtextures/mods/%s/<family>_sq<N>.mega2",
                             f->path, b->id);
            }
        }
    }

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

    /* Every shadow is a stock resource this bundle ships. A legacy bundle's
     * shadows are every name its first-generation package carried, new names
     * included (DESIGN.md section 11), so they need not be stock. */
    for (k = 0; k < b->shadow_count; k++) {
        int got;
        if (!mb_req_has(&own, b->shadows[k])) {
            mb_error(b, "shadows names %s but assets/%s is not in the bundle", b->shadows[k], b->shadows[k]);
            continue;
        }
        if (b->legacy) continue;
        got = r && r->stock ? r->stock(r->stock_context, b->shadows[k]) : -1;
        if (got == 0) mb_error(b, "shadows names %s, which is not a stock resource of this game", b->shadows[k]);
        else if (got < 0) mb_error(b, "cannot confirm that shadow %s is a stock resource (the game's resource reader is unavailable)",
                                   b->shadows[k]);
    }

    /* Needs: own files, then installed bundles, then the game. A legacy
     * bundle is built while its map loads and its unresolved needs only warn,
     * so the game's archives are not read for it. */
    if (b->legacy) {
        if (r) { no_stock.installed = r->installed; no_stock.installed_context = r->installed_context; }
        r = &no_stock;
    }
    for (i = 0; i < b->file_count; i++) {
        sh_mb_file *f = &b->files[i];
        for (k = 0; k < f->need_count; k++) {
            int got = mb_resolve_one(b, &own, r, &f->needs[k]);
            b->needs.total++;
            if (got == 1) continue;
            b->needs.unresolved++;
            /* Stock materials name virtual-texture source images that have no
             * standalone image resource, so an unresolved image is reported,
             * not refused. */
            if (strcmp(f->needs[k].type, "image") == 0)
                mb_warn(b, "%s needs image %s, which resolves to nothing", f->path, f->needs[k].name);
            /* A converted first-generation package loaded before it was
             * converted; a heuristic need it cannot satisfy is reported, and
             * the carrier is converted rather than cleaned. */
            else if (b->legacy)
                mb_warn(b, "%s needs %s %s, which %s (legacy bundle: reported, not refused)", f->path,
                        f->needs[k].type, f->needs[k].name,
                        got < 0 ? "cannot be checked against the game" : "resolves to nothing");
            else if (got < 0)
                mb_error(b, "%s needs %s %s, which is not in the bundle and cannot be checked against the game (resource reader unavailable)",
                         f->path, f->needs[k].type, f->needs[k].name);
            else
                mb_error(b, "%s needs %s %s, which resolves to nothing", f->path, f->needs[k].type, f->needs[k].name);
        }
    }
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

/* Keys in byte order: author build bundle extra files format homepage id name
 * requirements shadows version; file keys: blob needs path provides request size,
 * or for a stock pointer needs path provides request stock{sha256 source}.
 * with_build 0 gives the content compared to decide whether build advances. */
static char *mb_emit(const sh_mb_bundle *b, int with_bundle, int with_build, size_t *out_len)
{
    mb_buf o = {0};
    int first = 1;
    size_t i, k;
    mb_put(&o, "{", 1);
    if (b->author) { mb_key(&o, &first, "author"); mb_put_jcstr(&o, b->author); }
    if (with_build) { mb_key(&o, &first, "build"); mb_put_u64(&o, b->build); }
    if (with_bundle) { mb_key(&o, &first, "bundle"); mb_put_jcstr(&o, b->hash); }
    if (b->extra) { mb_key(&o, &first, "extra"); mb_puts(&o, b->extra); }
    mb_key(&o, &first, "files");
    mb_put(&o, "[", 1);
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        if (i) mb_put(&o, ",", 1);
        if (f->stock) mb_puts(&o, "{\"needs\":[");
        else {
            mb_puts(&o, "{\"blob\":");
            mb_put_jcstr(&o, f->blob);
            mb_puts(&o, ",\"needs\":[");
        }
        for (k = 0; k < f->need_count; k++) {
            if (k) mb_put(&o, ",", 1);
            mb_puts(&o, "{\"name\":");
            mb_put_jcstr(&o, f->needs[k].name);
            mb_puts(&o, ",\"type\":");
            mb_put_jcstr(&o, f->needs[k].type);
            mb_put(&o, "}", 1);
        }
        mb_puts(&o, "],\"path\":");
        mb_put_jcstr(&o, f->path);
        mb_puts(&o, ",\"provides\":{\"name\":");
        mb_put_jcstr(&o, f->pname ? f->pname : "");
        mb_puts(&o, ",\"type\":");
        mb_put_jcstr(&o, f->ptype ? f->ptype : "");
        mb_puts(&o, "},\"request\":");
        mb_put_jcstr(&o, f->request);
        if (f->stock) {
            mb_puts(&o, ",\"stock\":{\"sha256\":");
            mb_put_jcstr(&o, f->blob);
            mb_puts(&o, ",\"source\":");
            mb_put_jcstr(&o, f->stock_source ? f->stock_source : "");
            mb_put(&o, "}", 1);
        } else {
            mb_puts(&o, ",\"size\":");
            mb_put_u64(&o, f->size);
        }
        mb_put(&o, "}", 1);
    }
    mb_put(&o, "]", 1);
    mb_key(&o, &first, "format");
    mb_puts(&o, "1");
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
    mb_key(&o, &first, "version");
    mb_put_jcstr(&o, b->version);
    mb_put(&o, "}", 1);
    if (o.oom) { free(o.p); return NULL; }
    if (out_len) *out_len = o.n;
    return o.p;
}

static void mb_mark_delivered(sh_mb_bundle *b);

static void mb_finish(sh_mb_bundle *b)
{
    free(b->canonical);
    b->canonical = mb_emit(b, 0, 1, &b->canonical_len);
    if (!b->canonical) { mb_error(b, "out of memory"); return; }
    if (b->canonical_len > SH_MB_MANIFEST_MAX) mb_error(b, "the bundle manifest is larger than 1 MiB");
    sh_mb_sha256_hex(b->canonical, b->canonical_len, b->hash);
}

char *sh_mb_bundle_json(const sh_mb_bundle *b, size_t *out_len)
{
    size_t n = 0, need;
    char *compact = mb_emit(b, 1, 1, &n), *pretty;
    if (!compact) return NULL;
    need = json_pretty(compact, n, NULL, 0);
    if (!need) { free(compact); return NULL; }
    pretty = (char *)malloc(need + 1);
    if (!pretty) { free(compact); return NULL; }
    json_pretty(compact, n, pretty, need);
    pretty[need] = 0;
    free(compact);
    if (out_len) *out_len = need;
    return pretty;
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

static int mb_load_file(sh_mb_bundle *b, const mbj *e, size_t index)
{
    static const char *const keys[] = { "blob", "needs", "path", "provides", "request", "size", NULL };
    static const char *const stock_keys[] = { "needs", "path", "provides", "request", "stock", NULL };
    static const char *const pointer_keys[] = { "sha256", "source", NULL };
    const mbj *blob = mbj_get(e, "blob"), *needs = mbj_get(e, "needs"), *path = mbj_get(e, "path"),
              *prov = mbj_get(e, "provides"), *req = mbj_get(e, "request"), *size = mbj_get(e, "size");
    const mbj *pt = prov ? mbj_get(prov, "type") : NULL, *pn = prov ? mbj_get(prov, "name") : NULL;
    const mbj *stock = e->t == MBJ_OBJ ? mbj_get(e, "stock") : NULL, *ssha = NULL, *ssrc = NULL;
    char cause[SH_MB_CAUSE_MAX], type[64], name[SH_MB_REQUEST_MAX + 16], *expect;
    sh_mb_file *f;
    size_t k;
    unsigned long long sz = 0;

    if (stock) {
        /* A stock pointer: no blob and no size. */
        if (!mb_only_keys(e, stock_keys) || !needs || !path || !prov || !req) {
            mb_error(b, "bundle.json files[%zu] is a stock pointer and must have exactly needs, path, provides, request "
                     "and stock", index);
            return 0;
        }
        ssha = stock->t == MBJ_OBJ ? mbj_get(stock, "sha256") : NULL;
        ssrc = stock->t == MBJ_OBJ ? mbj_get(stock, "source") : NULL;
        if (stock->t != MBJ_OBJ || !mb_only_keys(stock, pointer_keys) || !mb_is_hex64(ssha) || !ssrc ||
            ssrc->t != MBJ_STR || strlen(ssrc->s) != ssrc->n || !sh_mb_check_source(ssrc->s, cause, sizeof cause)) {
            mb_error(b, "bundle.json files[%zu].stock must be {\"sha256\": 64 lowercase hex, \"source\": an installed path}",
                     index);
            return 0;
        }
        {
            char *lower = mb_path_dup(ssrc->s, ssrc->n);
            int same = lower && strcmp(lower, ssrc->s) == 0;
            free(lower);
            if (!same) {
                mb_error(b, "bundle.json files[%zu].stock.source must be lower-case with '/' separators", index);
                return 0;
            }
        }
    } else if (e->t != MBJ_OBJ || !mb_only_keys(e, keys) || !blob || !needs || !path || !prov || !req || !size) {
        mb_error(b, "bundle.json files[%zu] must have exactly blob, needs, path, provides, request and size", index);
        return 0;
    }
    if (path->t != MBJ_STR || strlen(path->s) != path->n || !mb_starts(path->s, "assets/")) {
        mb_error(b, "bundle.json files[%zu].path must start with assets/", index);
        return 0;
    }
    expect = mb_strdup(path->s + 7);
    if (!expect) { mb_error(b, "out of memory"); return 0; }
    mb_lower(expect);
    if (!sh_mb_check_request(expect, cause, sizeof cause)) { mb_error(b, "bundle.json %s: %s", path->s, cause); free(expect); return 0; }
    if (!stock && !sh_mb_extension_allowed(expect)) { mb_error(b, "bundle.json: file type not allowed: %s", path->s); free(expect); return 0; }
    if (req->t != MBJ_STR || strcmp(req->s, expect) != 0) {
        mb_error(b, "bundle.json %s: request must be %s", path->s, expect);
        free(expect);
        return 0;
    }
    if (!sh_mb_logical_name(expect, type, sizeof type, name, sizeof name) || !pt || !pn ||
        prov->t != MBJ_OBJ || prov->count != 2 || pt->t != MBJ_STR || pn->t != MBJ_STR ||
        strcmp(pt->s, type) != 0 || strcmp(pn->s, name) != 0) {
        mb_error(b, "bundle.json %s: provides does not match the request path", path->s);
        free(expect);
        return 0;
    }
    if (!stock && !mb_is_hex64(blob)) { mb_error(b, "bundle.json %s: blob must be 64 lowercase hex characters", path->s); free(expect); return 0; }
    if (!stock && (size->t != MBJ_NUM || !mb_plain_integer(size->s, size->n) || size->s[0] == '-' ||
        (sz = _strtoui64(size->s, NULL, 10)) == 0 || sz > SH_MB_MAX_FILE)) {
        mb_error(b, "bundle.json %s: size must be 1 to 64 MiB", path->s);
        free(expect);
        return 0;
    }
    if (needs->t != MBJ_ARR) { mb_error(b, "bundle.json %s: needs must be an array", path->s); free(expect); return 0; }
    f = mb_add_file(b);
    if (!f) { mb_error(b, "out of memory"); free(expect); return 0; }
    f->path = mb_strdup(path->s);
    f->request = expect;
    memcpy(f->blob, stock ? ssha->s : blob->s, SH_MB_HASH_HEX + 1);
    f->size = sz;
    f->ptype = mb_strdup(type);
    f->pname = mb_strdup(name);
    f->is_decl = mb_starts(expect, "generated/decls/");
    f->is_module = strcmp(type, "snapModule") == 0;
    f->shadowed = 0;
    if (stock) {
        f->stock = 1;
        f->stock_source = mb_strdup(ssrc->s);
        b->stock_count++;
        if (!f->stock_source) { mb_error(b, "out of memory"); return 0; }
    }
    b->total_size += sz;
    for (k = 0; k < needs->count; k++) {
        const mbj *n = needs->items[k], *nt = mbj_get(n, "type"), *nn = mbj_get(n, "name");
        if (n->t != MBJ_OBJ || n->count != 2 || !nt || !nn || nt->t != MBJ_STR || nn->t != MBJ_STR ||
            !nt->n || !nn->n || strlen(nn->s) != nn->n || strlen(nt->s) != nt->n) {
            mb_error(b, "bundle.json %s: needs[%zu] must be {\"name\",\"type\"}", path->s, k);
            return 0;
        }
        if (f->need_count) {
            sh_mb_need probe;
            probe.type = nt->s;
            probe.name = nn->s;
            if (mb_need_cmp(&f->needs[f->need_count - 1], &probe) >= 0) {
                mb_error(b, "bundle.json %s: needs are not sorted and unique", path->s);
                return 0;
            }
        }
        if (!mb_add_need(f, nt->s, nn->s, nn->n)) { mb_error(b, "out of memory"); return 0; }
    }
    if (!f->path || !f->ptype || !f->pname) { mb_error(b, "out of memory"); return 0; }
    return 1;
}

/* The bundle.json form (Contract 1 with "bundle"): a download, a rawmap ZIP,
 * a folder of the retired layout. Its pointers have no file in any folder the
 * product creates. */
int sh_mb_load_manifest_text(const char *text, size_t len, sh_mb_bundle *out)
{
    static const char *const keys[] = { "author", "build", "bundle", "extra", "files", "format", "homepage", "id",
                                        "name", "requirements", "shadows", "version", NULL };
    char cause[SH_MB_CAUSE_MAX];
    mbj *root;
    const mbj *files, *bundle, *extra;
    size_t i;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    strcpy_s(out->descriptor, sizeof out->descriptor, "bundle.json");
    if (!text || len > SH_MB_MANIFEST_MAX + 65536) { mb_error(out, "bundle.json is missing or too large"); return 0; }
    root = mbj_parse(text, len, cause, sizeof cause);
    if (!root) { mb_error(out, "bundle.json: %s", cause); return 0; }
    if (root->t != MBJ_OBJ) { mb_error(out, "bundle.json must contain one JSON object"); goto done; }
    if (!mb_only_keys(root, keys)) { mb_error(out, "bundle.json has a key this release does not know"); goto done; }
    {
        const mbj *build = mbj_get(root, "build");
        if (!build || build->t != MBJ_NUM || !mb_plain_integer(build->s, build->n) || build->s[0] == '-' ||
            (out->build = _strtoui64(build->s, NULL, 10)) == 0) {
            out->build = 0;
            mb_error(out, "bundle.json build must be a positive integer");
            goto done;
        }
    }
    if (!mb_common_fields(out, root, 1)) goto done;
    for (i = 1; i < out->shadow_count; i++)
        if (strcmp(out->shadows[i - 1], out->shadows[i]) >= 0) { mb_error(out, "bundle.json shadows are not sorted and unique"); goto done; }
    extra = mbj_get(root, "extra");
    if (extra) {
        mb_buf e = {0};
        if (extra->t != MBJ_OBJ || !mbj_canon(extra, &e, cause, sizeof cause)) {
            mb_error(out, "bundle.json extra must be an object of plain values");
            free(e.p);
            goto done;
        }
        out->extra = e.p;
        {
            const mbj *lg = mbj_get(extra, "legacy");
            out->legacy = lg && lg->t == MBJ_OBJ;
        }
    }
    bundle = mbj_get(root, "bundle");
    if (!mb_is_hex64(bundle)) { mb_error(out, "bundle.json bundle must be 64 lowercase hex characters"); goto done; }
    files = mbj_get(root, "files");
    if (!files || files->t != MBJ_ARR) { mb_error(out, "bundle.json files must be an array"); goto done; }
    if (files->count > SH_MB_MAX_FILES) { mb_error(out, "bundle.json lists more than %u files", SH_MB_MAX_FILES); goto done; }
    for (i = 0; i < files->count; i++) {
        if (!mb_load_file(out, files->items[i], i)) goto done;
        if (i && strcmp(out->files[i - 1].path, out->files[i].path) >= 0) {
            mb_error(out, "bundle.json files are not sorted by path and unique");
            goto done;
        }
    }
    for (i = 1; i < out->file_count; i++) {
        size_t k;
        for (k = 0; k < i; k++)   /* paths differ only in case would collide on disk */
            if (strcmp(out->files[k].request, out->files[i].request) == 0) {
                mb_error(out, "bundle.json has two files with request %s", out->files[i].request);
                goto done;
            }
    }
    if (out->total_size > SH_MB_MAX_TOTAL) { mb_error(out, "bundle.json files exceed 256 MiB"); goto done; }
    for (i = 0; i < out->file_count; i++) {
        out->files[i].shadowed = mb_is_shadow(out, out->files[i].request);
        out->files[i].absent = out->files[i].stock;
    }
    mb_finish(out);
    if (!out->errors && strcmp(out->hash, bundle->s) != 0)
        mb_error(out, "bundle.json does not match its bundle hash (edited or damaged)");
    out->validated = 1;
    mb_mark_delivered(out);
done:
    mbj_free(root);
    return out->errors == 0;
}


/* ------------------------------------------------------------ mod.json ---- */

static long long mb_now(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ull) / 10000000ull);
}

/* A "legacy" object without "source": "overrides" (the retired package folder) is a package a map
 * delivered (a converted carrier, or a beta.16-19 delivery the overrides
 * migration moved). */
static void mb_mark_delivered(sh_mb_bundle *b)
{
    mbj *extra;
    const mbj *lg, *src;
    b->delivered = 0;
    if (!b->extra) return;
    extra = mbj_parse(b->extra, strlen(b->extra), NULL, 0);
    lg = extra ? mbj_get(extra, "legacy") : NULL;
    if (lg && lg->t == MBJ_OBJ) {
        src = mbj_get(lg, "source");
        b->delivered = !(src && src->t == MBJ_STR && strcmp(src->s, "overrides") == 0);   /* the retired package folder */
    }
    mbj_free(extra);
}

/* One entry of the product block's "files" (pointer 0) or "stock" (pointer 1) list. */
static int mb_load_block_file(sh_mb_bundle *b, const mbj *e, size_t index, int pointer)
{
    static const char *const keys[] = { "needs", "path", "provides", "sha256", "size", NULL };
    static const char *const pkeys[] = { "inAssets", "needs", "path", "provides", "sha256", "source", NULL };
    const char *list = pointer ? "stock" : "files";
    const mbj *sha, *needs, *path, *prov, *size, *src, *in_assets, *pt, *pn;
    char cause[SH_MB_CAUSE_MAX], type[64], name[SH_MB_REQUEST_MAX + 16], *expect;
    sh_mb_file *f;
    size_t k;
    unsigned long long sz = 0;
    if (e->t != MBJ_OBJ || !mb_only_keys(e, pointer ? pkeys : keys)) {
        mb_error(b, "mod.json " SH_MB_BLOCK_KEY ".%s[%zu] must have exactly %s", list, index,
                 pointer ? "path, sha256, source, provides, needs and inAssets (optional)"
                         : "path, sha256, size, provides and needs");
        return 0;
    }
    sha = mbj_get(e, "sha256");
    needs = mbj_get(e, "needs");
    path = mbj_get(e, "path");
    prov = mbj_get(e, "provides");
    size = mbj_get(e, "size");
    src = mbj_get(e, "source");
    in_assets = mbj_get(e, "inAssets");
    if (!sha || !needs || !path || !prov || (pointer ? !src : !size)) {
        mb_error(b, "mod.json " SH_MB_BLOCK_KEY ".%s[%zu] is missing a member", list, index);
        return 0;
    }
    if (path->t != MBJ_STR || strlen(path->s) != path->n || !mb_starts(path->s, "assets/")) {
        mb_error(b, "mod.json " SH_MB_BLOCK_KEY ".%s[%zu].path must start with assets/", list, index);
        return 0;
    }
    expect = mb_strdup(path->s + 7);
    if (!expect) { mb_error(b, "out of memory"); return 0; }
    mb_lower(expect);
    if (!sh_mb_check_request(expect, cause, sizeof cause)) { mb_error(b, "mod.json %s: %s", path->s, cause); free(expect); return 0; }
    if (!pointer && !sh_mb_extension_allowed(expect)) { mb_error(b, "mod.json: file type not allowed: %s", path->s); free(expect); return 0; }
    pt = prov->t == MBJ_OBJ ? mbj_get(prov, "type") : NULL;
    pn = prov->t == MBJ_OBJ ? mbj_get(prov, "name") : NULL;
    if (!sh_mb_logical_name(expect, type, sizeof type, name, sizeof name) || !pt || !pn || prov->count != 2 ||
        pt->t != MBJ_STR || pn->t != MBJ_STR || strcmp(pt->s, type) != 0 || strcmp(pn->s, name) != 0) {
        mb_error(b, "mod.json %s: provides does not match the request path", path->s);
        free(expect);
        return 0;
    }
    if (!mb_is_hex64(sha)) { mb_error(b, "mod.json %s: sha256 must be 64 lowercase hex characters", path->s); free(expect); return 0; }
    if (pointer) {
        char *lower;
        int same;
        if (src->t != MBJ_STR || strlen(src->s) != src->n || !sh_mb_check_source(src->s, cause, sizeof cause)) {
            mb_error(b, "mod.json %s: source must be an installed path", path->s);
            free(expect);
            return 0;
        }
        lower = mb_path_dup(src->s, src->n);
        same = lower && strcmp(lower, src->s) == 0;
        free(lower);
        if (!same) { mb_error(b, "mod.json %s: source must be lower-case with '/' separators", path->s); free(expect); return 0; }
        if (in_assets && in_assets->t != MBJ_BOOL) { mb_error(b, "mod.json %s: inAssets must be true or false", path->s); free(expect); return 0; }
    } else if (size->t != MBJ_NUM || !mb_plain_integer(size->s, size->n) || size->s[0] == '-' ||
               (sz = _strtoui64(size->s, NULL, 10)) == 0 || sz > SH_MB_MAX_FILE) {
        mb_error(b, "mod.json %s: size must be 1 to 64 MiB", path->s);
        free(expect);
        return 0;
    }
    if (needs->t != MBJ_ARR) { mb_error(b, "mod.json %s: needs must be an array", path->s); free(expect); return 0; }
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
        f->absent = in_assets && !in_assets->b;
        b->stock_count++;
        if (!f->stock_source) { mb_error(b, "out of memory"); return 0; }
    }
    b->total_size += sz;
    for (k = 0; k < needs->count; k++) {
        const mbj *n = needs->items[k], *nt = mbj_get(n, "type"), *nn = mbj_get(n, "name");
        if (n->t != MBJ_OBJ || n->count != 2 || !nt || !nn || nt->t != MBJ_STR || nn->t != MBJ_STR ||
            !nt->n || !nn->n || strlen(nn->s) != nn->n || strlen(nt->s) != nt->n) {
            mb_error(b, "mod.json %s: needs[%zu] must be {\"type\",\"name\"}", path->s, k);
            return 0;
        }
        if (f->need_count) {
            sh_mb_need probe;
            probe.type = nt->s;
            probe.name = nn->s;
            if (mb_need_cmp(&f->needs[f->need_count - 1], &probe) >= 0) {
                mb_error(b, "mod.json %s: needs are not sorted and unique", path->s);
                return 0;
            }
        }
        if (!mb_add_need(f, nt->s, nn->s, nn->n)) { mb_error(b, "out of memory"); return 0; }
    }
    if (!f->path || !f->ptype || !f->pname) { mb_error(b, "out of memory"); return 0; }
    return 1;
}

/* A built mod.json: the author block, then the product block's manifest. */
static int mb_load_root(sh_mb_bundle *out, const mbj *root, int check)
{
    static const char *const bkeys[] = { "build", "built", "bundle", "cause", "files", "format", "legacy", "origin",
                                         "shadows", "stock", "validated", NULL };
    const mbj *block, *v, *files, *stock, *bundle, *sh;
    size_t i, total;
    if (root->t != MBJ_OBJ) { mb_error(out, "mod.json must contain one JSON object"); return 0; }
    block = mbj_get(root, SH_MB_BLOCK_KEY);
    if (!block || block->t != MBJ_OBJ || !mbj_get(block, "bundle")) {
        mb_error(out, "mod.json has not been built yet (no \"" SH_MB_BLOCK_KEY "\" block with a bundle)");
        return 0;
    }
    if (!mb_only_keys(block, bkeys)) { mb_error(out, "mod.json \"" SH_MB_BLOCK_KEY "\" has a key this release does not know"); return 0; }
    if (!mb_descriptor_root(out, root, block)) return 0;
    v = mbj_get(block, "format");
    if (!v || v->t != MBJ_NUM || strcmp(v->s, "1") != 0) {
        mb_error(out, "mod.json \"" SH_MB_BLOCK_KEY "\" format %s is not supported (this release reads format 1)",
                 v && v->t == MBJ_NUM ? v->s : "(missing)");
        return 0;
    }
    v = mbj_get(block, "build");
    if (!v || v->t != MBJ_NUM || !mb_plain_integer(v->s, v->n) || v->s[0] == '-' ||
        (out->build = _strtoui64(v->s, NULL, 10)) == 0) {
        out->build = 0;
        mb_error(out, "mod.json \"" SH_MB_BLOCK_KEY "\" build must be a positive integer");
        return 0;
    }
    v = mbj_get(block, "built");
    if (v && (v->t != MBJ_NUM || !mb_plain_integer(v->s, v->n) || v->s[0] == '-')) {
        mb_error(out, "mod.json \"" SH_MB_BLOCK_KEY "\" built must be a time in seconds");
        return 0;
    }
    out->built = v ? _strtoi64(v->s, NULL, 10) : 0;
    v = mbj_get(block, "validated");
    if (v && v->t != MBJ_BOOL) { mb_error(out, "mod.json \"" SH_MB_BLOCK_KEY "\" validated must be true or false"); return 0; }
    out->validated = v ? v->b : 1;
    v = mbj_get(block, "cause");
    if (v && v->t == MBJ_STR) strncpy_s(out->state_cause, sizeof out->state_cause, v->s, _TRUNCATE);
    v = mbj_get(block, "origin");
    if (v && v->t == MBJ_STR) strncpy_s(out->origin, sizeof out->origin, v->s, _TRUNCATE);
    /* The built shadows replace the author's input list. */
    for (i = 0; i < out->shadow_count; i++) free(out->shadows[i]);
    free(out->shadows);
    out->shadows = NULL;
    out->shadow_count = 0;
    sh = mbj_get(block, "shadows");
    if (sh) {
        if (!mb_set_shadows(out, sh, 1)) return 0;
        for (i = 1; i < out->shadow_count; i++)
            if (strcmp(out->shadows[i - 1], out->shadows[i]) >= 0) {
                mb_error(out, "mod.json \"" SH_MB_BLOCK_KEY "\" shadows are not sorted and unique");
                return 0;
            }
    }
    bundle = mbj_get(block, "bundle");
    if (!mb_is_hex64(bundle)) { mb_error(out, "mod.json \"" SH_MB_BLOCK_KEY "\" bundle must be 64 lowercase hex characters"); return 0; }
    files = mbj_get(block, "files");
    stock = mbj_get(block, "stock");
    if ((files && files->t != MBJ_ARR) || (stock && stock->t != MBJ_ARR)) {
        mb_error(out, "mod.json \"" SH_MB_BLOCK_KEY "\" files and stock must be arrays");
        return 0;
    }
    total = (files ? files->count : 0) + (stock ? stock->count : 0);
    if (total > SH_MB_MAX_FILES) { mb_error(out, "mod.json lists more than %u files", SH_MB_MAX_FILES); return 0; }
    for (i = 0; files && i < files->count; i++) if (!mb_load_block_file(out, files->items[i], i, 0)) return 0;
    for (i = 0; stock && i < stock->count; i++) if (!mb_load_block_file(out, stock->items[i], i, 1)) return 0;
    qsort(out->files, out->file_count, sizeof(sh_mb_file), mb_file_cmp);
    for (i = 1; i < out->file_count; i++) {
        size_t k;
        if (strcmp(out->files[i - 1].path, out->files[i].path) == 0) {
            mb_error(out, "mod.json lists %s twice", out->files[i].path);
            return 0;
        }
        for (k = 0; k < i; k++)
            if (strcmp(out->files[k].request, out->files[i].request) == 0) {
                mb_error(out, "mod.json has two files with request %s", out->files[i].request);
                return 0;
            }
    }
    if (out->total_size > SH_MB_MAX_TOTAL) { mb_error(out, "mod.json files exceed 256 MiB"); return 0; }
    for (i = 0; i < out->file_count; i++) out->files[i].shadowed = mb_is_shadow(out, out->files[i].request);
    mb_finish(out);
    if (!out->errors && check && strcmp(out->hash, bundle->s) != 0)
        mb_error(out, "mod.json changed since it was built (the author block or the product block was edited); "
                 "a rescan of the Mods tab or 'sh_mods build' rebuilds it");
    mb_mark_delivered(out);
    return out->errors == 0;
}

static int mb_load_modjson(const char *text, size_t len, sh_mb_bundle *out, int check)
{
    char cause[SH_MB_CAUSE_MAX];
    mbj *root;
    int ok;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    strcpy_s(out->descriptor, sizeof out->descriptor, "mod.json");
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

#define MB_MODJSON_MAX (16u * 1024u * 1024u)

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

/* The product block, pretty-printed. `root` (optional) is the folder whose
 * assets/ decides each pointer's inAssets; else the files' absent flags. */
static char *mb_block_text(const sh_mb_bundle *b, const wchar_t *root, int with_legacy)
{
    mb_buf o = {0};
    char num[32], *legacy = NULL, *pretty;
    size_t i, pointers = 0;
    int first;
    mb_puts(&o, "{\"format\":1,\"build\":");
    mb_put_u64(&o, b->build);
    mb_puts(&o, ",\"bundle\":");
    mb_put_jcstr(&o, b->hash);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"built\":%lld", b->built > 0 ? b->built : mb_now());
    mb_puts(&o, num);
    mb_puts(&o, b->validated ? ",\"validated\":true" : ",\"validated\":false");
    if (!b->validated && b->state_cause[0]) { mb_puts(&o, ",\"cause\":"); mb_put_jcstr(&o, b->state_cause); }
    if (b->origin[0]) { mb_puts(&o, ",\"origin\":"); mb_put_jcstr(&o, b->origin); }
    if (with_legacy && (legacy = mb_extra_member(b, "legacy")) != NULL) { mb_puts(&o, ",\"legacy\":"); mb_puts(&o, legacy); }
    free(legacy);
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
            int absent = f->absent;
            if (!f->stock) continue;
            if (root) {
                wchar_t *p = NULL, *w;
                int wn = MultiByteToWideChar(CP_UTF8, 0, f->path, -1, NULL, 0);
                w = wn > 0 ? (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wn) : NULL;
                if (w) {
                    wchar_t *q;
                    MultiByteToWideChar(CP_UTF8, 0, f->path, -1, w, wn);
                    for (q = w; *q; q++) if (*q == L'/') *q = L'\\';
                    p = mb_wjoin(root, w);
                    free(w);
                }
                absent = !p || GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES;
                free(p);
            }
            if (!first) mb_put(&o, ",", 1);
            first = 0;
            mb_puts(&o, "{\"path\":");
            mb_put_jcstr(&o, f->path);
            mb_puts(&o, ",\"sha256\":");
            mb_put_jcstr(&o, f->blob);
            mb_puts(&o, ",\"source\":");
            mb_put_jcstr(&o, f->stock_source ? f->stock_source : "");
            mb_put_needs(&o, f);
            if (absent) mb_puts(&o, ",\"inAssets\":false");
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

/* An author block for a bundle known only by its manifest (a download, a
 * laid-out folder of the retired layout): identity, the optional display
 * keys, requirements, shadows (a converted package keeps them in the product
 * block), and every extra key but "legacy". */
static char *mb_author_synth(const sh_mb_bundle *b, size_t *out_len)
{
    mb_buf o = {0};
    mbj *extra = NULL;
    char *pretty;
    size_t i;
    mb_puts(&o, "{\"id\":");
    mb_put_jcstr(&o, b->id);
    mb_puts(&o, ",\"name\":");
    mb_put_jcstr(&o, b->name ? b->name : b->id);
    mb_puts(&o, ",\"version\":");
    mb_put_jcstr(&o, b->version);
    if (b->author) { mb_puts(&o, ",\"author\":"); mb_put_jcstr(&o, b->author); }
    if (b->homepage) { mb_puts(&o, ",\"homepage\":"); mb_put_jcstr(&o, b->homepage); }
    if (b->requirements) { mb_puts(&o, ",\"requirements\":"); mb_puts(&o, b->requirements); }
    if (b->shadow_count && !b->legacy) {
        mb_puts(&o, ",\"shadows\":[");
        for (i = 0; i < b->shadow_count; i++) {
            if (i) mb_put(&o, ",", 1);
            mb_put_jcstr(&o, b->shadows[i]);
        }
        mb_put(&o, "]", 1);
    }
    if (b->extra) extra = mbj_parse(b->extra, strlen(b->extra), NULL, 0);
    for (i = 0; extra && extra->t == MBJ_OBJ && i < extra->count; i++) {
        if (extra->klen[i] == 6 && memcmp(extra->keys[i], "legacy", 6) == 0) continue;
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

/* The author block of `own` has a top-level "legacy" member. */
static int mb_author_has(const char *own, size_t n, const char *key)
{
    size_t count = 0, open = 0, i;
    mb_member *m = mb_members(own, n, &count, &open);
    int hit = 0;
    for (i = 0; m && i < count && !hit; i++) hit = mb_member_is(own, &m[i], key);
    free(m);
    return hit;
}

static char *mb_modjson(const sh_mb_bundle *b, const char *author, size_t alen, const wchar_t *root, size_t *out_len)
{
    char *own, *block, *out;
    size_t own_len = 0;
    if (!b || !b->canonical) return NULL;
    own = author ? sh_mb_author_bytes(author, alen, &own_len, NULL) : mb_author_synth(b, &own_len);
    if (!own) return NULL;
    block = mb_block_text(b, root, b->legacy && !mb_author_has(own, own_len, "legacy"));
    out = block ? mb_splice(own, own_len, block, out_len) : NULL;
    free(own);
    free(block);
    return out;
}

void sh_mb_renumber(sh_mb_bundle *b, unsigned long long build)
{
    if (!b || !build || b->errors) return;
    b->build = build;
    mb_finish(b);
}

char *sh_mb_modjson_text(const sh_mb_bundle *b, const char *author_text, size_t author_len, size_t *out_len)
{
    return mb_modjson(b, author_text, author_len, NULL, out_len);
}

int sh_mb_write(const sh_mb_bundle *b, const char *folder, char *cause, size_t cap)
{
    wchar_t *root, *mod_path = NULL, *pkg_path = NULL;
    unsigned char *existing = NULL;
    char *text = NULL;
    size_t elen = 0, len = 0;
    int from_pkg = 0, ok;
    if (!b || b->errors || !b->canonical) return mb_cause(cause, cap, "the bundle has errors and is not written");
    root = folder ? sh_mb_wpath(folder) : NULL;
    if (root) { mod_path = mb_wjoin(root, L"mod.json"); pkg_path = mb_wjoin(root, L"package.json"); }
    if (!mod_path || !pkg_path) { free(root); free(mod_path); free(pkg_path); return mb_cause(cause, cap, "out of memory"); }
    existing = mb_read_wfile(mod_path, MB_MODJSON_MAX, &elen);
    if (!existing) {
        existing = mb_read_wfile(pkg_path, SH_MB_DESCRIPTOR_MAX, &elen);
        from_pkg = existing != NULL;
    }
    text = mb_modjson(b, (const char *)existing, elen, root, &len);
    ok = text && mb_write_wfile(mod_path, text, len);
    if (!ok) mb_cause(cause, cap, text ? "cannot write mod.json (error %lu)" : "mod.json could not be composed (is it a JSON object?)",
                      GetLastError());
    if (ok && from_pkg) {
        /* package.json became mod.json, with the same author keys and bytes. */
        SetFileAttributesW(pkg_path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(pkg_path);
    }
    free(text);
    free(existing);
    free(root);
    free(mod_path);
    free(pkg_path);
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

/* -------------------------------------------------------------- build ---- */

int sh_mb_build(const char *folder, const sh_mb_resolver *resolver, sh_mb_bundle *out)
{
    wchar_t *root, *desc_mod, *desc_pkg;
    DWORD attr;
    int has_mod, has_pkg, have_prev = 0;
    unsigned char *text = NULL;
    size_t len = 0, i, k, ncarry = 0;
    unsigned long long prev_build = 0;
    sh_mb_bundle prev;
    sh_mb_stock_decl *carry = NULL;
    mbj *rootj = NULL;
    mb_walk w;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    memset(&prev, 0, sizeof prev);
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
    desc_mod = mb_wjoin(root, L"mod.json");
    desc_pkg = mb_wjoin(root, L"package.json");
    has_mod = desc_mod && GetFileAttributesW(desc_mod) != INVALID_FILE_ATTRIBUTES;
    has_pkg = desc_pkg && GetFileAttributesW(desc_pkg) != INVALID_FILE_ATTRIBUTES;
    if (has_mod && has_pkg) {
        mb_error(out, "the mod folder has both mod.json and package.json; keep one");
    } else if (!has_mod && !has_pkg) {
        mb_error(out, "the mod folder has no mod.json or package.json");
    } else {
        char cause[SH_MB_CAUSE_MAX];
        strcpy_s(out->descriptor, sizeof out->descriptor, has_mod ? "mod.json" : "package.json");
        text = mb_read_wfile(has_mod ? desc_mod : desc_pkg, has_mod ? MB_MODJSON_MAX : SH_MB_DESCRIPTOR_MAX, &len);
        if (!text) mb_error(out, "cannot read %s (missing, unreadable or over %u MiB)", out->descriptor,
                            (has_mod ? MB_MODJSON_MAX : SH_MB_DESCRIPTOR_MAX) >> 20);
        else if (!(rootj = mbj_parse((const char *)text, len, cause, sizeof cause))) mb_error(out, "%s: %s", out->descriptor, cause);
        else {
            const mbj *block = has_mod ? mbj_get(rootj, SH_MB_BLOCK_KEY) : NULL;
            if (mb_descriptor_root(out, rootj, block) && block && block->t == MBJ_OBJ) {
                const mbj *pb = mbj_get(block, "build"), *st = mbj_get(block, "stock"), *po = mbj_get(block, "origin");
                if (pb && pb->t == MBJ_NUM && mb_plain_integer(pb->s, pb->n) && pb->s[0] != '-')
                    prev_build = _strtoui64(pb->s, NULL, 10);
                /* Where the folder came from is the product's memory, kept across builds. */
                if (po && po->t == MBJ_STR) strncpy_s(out->origin, sizeof out->origin, po->s, _TRUNCATE);
                {
                    /* The previous build's identity: its hash stands for its
                     * content, since the author block may have changed since. */
                    const mbj *ph = mbj_get(block, "bundle"), *pt = mbj_get(block, "built");
                    if (mb_is_hex64(ph) && prev_build) {
                        memcpy(prev.hash, ph->s, SH_MB_HASH_HEX + 1);
                        have_prev = 1;
                    }
                    if (pt && pt->t == MBJ_NUM && mb_plain_integer(pt->s, pt->n) && pt->s[0] != '-')
                        prev.built = _strtoi64(pt->s, NULL, 10);
                }
                /* Pointers the block holds without a file under assets/ (a
                 * download, a migration) stay pointers unless a file now takes
                 * their place. */
                if (st && st->t == MBJ_ARR && st->count)
                    carry = (sh_mb_stock_decl *)calloc(st->count, sizeof *carry);
                for (i = 0; carry && i < st->count; i++) {
                    const mbj *e = st->items[i], *in = mbj_get(e, "inAssets"), *p = mbj_get(e, "path"),
                              *src = mbj_get(e, "source"), *sha = mbj_get(e, "sha256");
                    sh_mb_stock_decl *d = &carry[ncarry];
                    if (!(in && in->t == MBJ_BOOL && !in->b) || !p || p->t != MBJ_STR || !mb_starts(p->s, "assets/") ||
                        !src || src->t != MBJ_STR || !mb_is_hex64(sha)) continue;
                    d->request = mb_path_dup(p->s + 7, p->n - 7);
                    d->source = mb_path_dup(src->s, src->n);
                    memcpy(d->sha256, sha->s, SH_MB_HASH_HEX + 1);
                    if (!d->request || !d->source) { free(d->request); free(d->source); continue; }
                    ncarry++;
                }
            }
        }
        free(text);
    }
    mbj_free(rootj);
    free(desc_mod);
    free(desc_pkg);
    if (out->errors) goto finish;

    w.b = out;
    w.root = root;
    w.stopped = 0;
    w.stock = mb_stock() != NULL && mb_stock()->match != NULL;
    mb_walk_top(&w);
    /* Carried-forward pointers join the declared ones when nothing under
     * assets/ and nothing the author declared names their request. */
    for (k = 0; k < ncarry; k++) {
        int taken = 0;
        for (i = 0; i < out->file_count && !taken; i++) taken = strcmp(out->files[i].request, carry[k].request) == 0;
        for (i = 0; i < out->stock_decl_count && !taken; i++) taken = strcmp(out->stock_decls[i].request, carry[k].request) == 0;
        if (taken) continue;
        {
            sh_mb_stock_decl *grown = (sh_mb_stock_decl *)realloc(out->stock_decls,
                                                                  sizeof *grown * (out->stock_decl_count + 1));
            if (!grown) { mb_error(out, "out of memory"); break; }
            out->stock_decls = grown;
            out->stock_decls[out->stock_decl_count++] = carry[k];
            memset(&carry[k], 0, sizeof carry[k]);
        }
    }
    if (out->stock_decl_count > 1) qsort(out->stock_decls, out->stock_decl_count, sizeof(sh_mb_stock_decl), mb_decl_cmp);
    if (out->stock_decl_count && !mb_stock())
        mb_error(out, "%s names stock pointers, which cannot be resolved (the installed-content reader is unavailable)",
                 out->descriptor);
    mb_add_declared(out);
    mb_check_files(out, root, resolver);
    /* Contract 1 "build": equal content (ignoring build and bundle) keeps the
     * previous number, changed content takes the next one, a folder without a
     * previous product block starts at 1. The content is equal exactly when
     * the canonical text with the previous number hashes to the previous
     * bundle hash the block records. */
    out->build = 1;
    if (!out->errors && prev_build) {
        out->build = prev_build;
        mb_finish(out);
        if (!have_prev || strcmp(out->hash, prev.hash) != 0) out->build = prev_build + 1;
    }
    mb_finish(out);
    out->validated = out->errors == 0;
    out->built = (have_prev && !strcmp(prev.hash, out->hash) && prev.built > 0) ? prev.built : mb_now();
    if (have_prev) memcpy(out->prev_hash, prev.hash, SH_MB_HASH_HEX + 1);
finish:
    for (k = 0; k < ncarry; k++) { free(carry[k].request); free(carry[k].source); }
    free(carry);
    sh_mb_free(&prev);
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
    mb_sha256 s;
    int ok = 0;
    out[0] = 0;
    if (!root) return 0;
    memset(&l, 0, sizeof l);
    p = mb_wjoin(root, L"mod.json");
    if (p) text = mb_read_wfile(p, MB_MODJSON_MAX, &len);
    free(p);
    if (text) own = sh_mb_author_bytes((const char *)text, len, &own_len, NULL);
    else {
        p = mb_wjoin(root, L"package.json");
        if (p) text = mb_read_wfile(p, SH_MB_DESCRIPTOR_MAX, &len);
        free(p);
        if (text) { own = mb_strndup((const char *)text, len); own_len = len; }
    }
    if (!own) goto done;
    mb_sha_init(&s);
    mb_sha_update(&s, "snapmap-plus tree 1\n", 20);
    mb_sha_update(&s, own, own_len);
    mb_sha_update(&s, "\n", 1);
    assets = mb_wjoin(root, L"assets");
    if (assets) { mb_tree_walk(&l, assets, "assets", 1); free(assets); }
    if (l.failed) goto done;
    if (l.n > 1) qsort(l.e, l.n, sizeof *l.e, mb_tent_cmp);
    for (i = 0; i < l.n; i++) {
        char meta[64];
        int n = _snprintf_s(meta, sizeof meta, _TRUNCATE, " %llu %llu\n", l.e[i].size, l.e[i].mtime);
        mb_sha_update(&s, l.e[i].rel, strlen(l.e[i].rel));
        if (n > 0) mb_sha_update(&s, meta, (size_t)n);
    }
    mb_sha_hex(&s, out);
    ok = 1;
done:
    for (i = 0; i < l.n; i++) free(l.e[i].rel);
    free(l.e);
    free(own);
    free(text);
    free(root);
    return ok;
}

void sh_mb_content_key(const sh_mb_bundle *b, char out[SH_MB_HASH_HEX + 1])
{
    mb_buf o = {0};
    mbj *extra = NULL;
    size_t i;
    out[0] = 0;
    if (!b) return;
    mb_puts(&o, b->id);
    mb_puts(&o, "\n");
    if (b->requirements) mb_puts(&o, b->requirements);
    mb_puts(&o, "\n");
    for (i = 0; i < b->shadow_count; i++) { mb_puts(&o, b->shadows[i]); mb_puts(&o, "\n"); }
    mb_puts(&o, "--\n");
    if (b->extra) extra = mbj_parse(b->extra, strlen(b->extra), NULL, 0);
    for (i = 0; extra && extra->t == MBJ_OBJ && i < extra->count; i++) {
        if (extra->klen[i] == 6 && memcmp(extra->keys[i], "legacy", 6) == 0) continue;
        mb_put_jstr(&o, extra->keys[i], extra->klen[i]);
        mb_put(&o, ":", 1);
        if (!mbj_canon(extra->items[i], &o, NULL, 0)) o.oom = 1;
        mb_puts(&o, "\n");
    }
    mbj_free(extra);
    mb_puts(&o, "--\n");
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        char size[32];
        mb_puts(&o, f->request);
        mb_puts(&o, f->stock ? "\tstock " : "\tcarried ");
        if (f->stock) mb_puts(&o, f->stock_source ? f->stock_source : "");
        mb_puts(&o, "\t");
        mb_puts(&o, f->blob);
        _snprintf_s(size, sizeof size, _TRUNCATE, "\t%llu\n", f->stock ? 0ull : f->size);
        mb_puts(&o, size);
    }
    if (!o.oom) sh_mb_sha256_hex(o.p, o.n, out);
    free(o.p);
}

int sh_mb_folder_name(const char *mods_root, const char *id, const char *version, const char *hash,
                      char *out, size_t cap)
{
    int round;
    if (!mods_root || !id || !id[0] || !out || !cap) return 0;
    for (round = 0; round < 4; round++) {
        char leaf[SH_MB_ID_MAX + 4 * SH_MB_VERSION_MAX + 32], path[MAX_PATH * 4];
        wchar_t *w;
        DWORD a;
        if (round == 0) strcpy_s(leaf, sizeof leaf, id);
        else if (round == 1) _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s@%s", id, version ? version : "0.0.0");
        else _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s@%s-%.*s", id, version ? version : "0.0.0",
                         round == 2 ? 8 : 16, hash ? hash : "0000000000000000");
        if (round && (!version || !version[0])) continue;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", mods_root, leaf);
        w = sh_mb_wpath(path);
        a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
        free(w);
        if (a == INVALID_FILE_ATTRIBUTES) { strncpy_s(out, cap, leaf, _TRUNCATE); return 1; }
        if (hash && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            sh_mb_bundle have;
            int same = sh_mb_load_folder(path, &have) && strcmp(have.hash, hash) == 0;
            sh_mb_free(&have);
            if (same) { strncpy_s(out, cap, leaf, _TRUNCATE); return 2; }
        }
    }
    return 0;
}

/* ---------------------------------------------------- product record ---- */

static INIT_ONCE        g_rec_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_rec_lock;

static BOOL CALLBACK mb_rec_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_rec_lock);
    return TRUE;
}

void sh_mb_record_lock(void)
{
    InitOnceExecuteOnce(&g_rec_once, mb_rec_init, NULL, NULL);
    EnterCriticalSection(&g_rec_lock);
}

void sh_mb_record_unlock(void)
{
    LeaveCriticalSection(&g_rec_lock);
}

static wchar_t *mb_record_path(const char *mods_root)
{
    char path[MAX_PATH * 4];
    if (!mods_root || !mods_root[0]) return NULL;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" SH_MB_RECORD_FILE, mods_root);
    return sh_mb_wpath(path);
}

/* The record's root object: *state 1 read, 0 absent, -1 unreadable. */
static mbj *mb_record_read(const wchar_t *path, int *state)
{
    unsigned char *text;
    size_t len = 0;
    mbj *root;
    *state = 0;
    if (!path || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return NULL;
    text = mb_read_wfile(path, 64u * 1024u * 1024u, &len);
    root = text ? mbj_parse((const char *)text, len, NULL, 0) : NULL;
    free(text);
    if (!root || root->t != MBJ_OBJ) { mbj_free(root); *state = -1; return NULL; }
    *state = 1;
    return root;
}

int sh_mb_record_get(const char *mods_root, const char *section, char **json_out)
{
    wchar_t *path = mb_record_path(mods_root);
    mbj *root;
    const mbj *v;
    mb_buf o = {0};
    int state, r = 0;
    if (json_out) *json_out = NULL;
    if (!path || !section) { free(path); return -1; }
    sh_mb_record_lock();
    root = mb_record_read(path, &state);
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
    if (!path || !section || !section[0] || !strcmp(section, "format")) { free(path); return 0; }
    if (json && !(value = mbj_parse(json, strlen(json), NULL, 0))) { free(path); return 0; }
    sh_mb_record_lock();
    root = mb_record_read(path, &state);
    if (state < 0) {
        /* Never silently lost: the unreadable record is kept beside it. */
        wchar_t aside[MAX_PATH * 4];
        _snwprintf_s(aside, MAX_PATH * 4, _TRUNCATE, L"%s.unreadable-%lld", path, mb_now());
        MoveFileExW(path, aside, MOVEFILE_REPLACE_EXISTING);
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
    }
    sh_mb_record_unlock();
    free(pretty);
    free(o.p);
    mbj_free(root);
    mbj_free(value);
    free(path);
    return ok;
}

/* ---------------------------------------------------------------- state ---- */

int sh_mb_state_parse(const char *text, size_t len, int *validated, long long *installed_at,
                      char *cause, size_t cap)
{
    mbj *root = mbj_parse(text, len, NULL, 0);
    const mbj *v, *at, *fc;
    int ok = 0;
    if (validated) *validated = 0;
    if (installed_at) *installed_at = 0;
    if (cause && cap) cause[0] = 0;
    if (!root || root->t != MBJ_OBJ) goto done;
    v = mbj_get(root, "validated");
    at = mbj_get(root, "installedAt");
    fc = mbj_get(root, "firstCause");
    if (!v || v->t != MBJ_BOOL) goto done;
    if (validated) *validated = v->b;
    if (at && at->t == MBJ_NUM && installed_at) *installed_at = _strtoi64(at->s, NULL, 10);
    if (fc && fc->t == MBJ_STR && cause && cap) strncpy_s(cause, cap, fc->s, _TRUNCATE);
    ok = 1;
done:
    mbj_free(root);
    return ok;
}
