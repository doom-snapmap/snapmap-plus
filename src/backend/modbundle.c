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
    if (mb_ends(r, ".bmodel")) cls = "model";
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
 * Not scanned: animWebDecl, fxDecl, sounds and sound events, aiVoiceOver,
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

static int mb_parse_descriptor(sh_mb_bundle *b, const char *text, size_t len)
{
    char cause[SH_MB_CAUSE_MAX];
    mbj *root = mbj_parse(text, len, cause, sizeof cause);
    mb_buf extra = {0};
    size_t i, kept = 0;
    int ok = 0;
    if (!root) { mb_error(b, "%s: %s", b->descriptor, cause); return 0; }
    if (root->t != MBJ_OBJ) { mb_error(b, "%s must contain one JSON object", b->descriptor); goto done; }
    if (mbj_get(root, "files") || mbj_get(root, "bundle")) {
        mb_error(b, "%s may not contain files or bundle; they are computed when the bundle is built", b->descriptor);
        goto done;
    }
    if (!mb_common_fields(b, root, 0)) goto done;
    {
        /* A converted first-generation package (modlegacy.c) carries a
         * "legacy" object; it stays in "extra" and grandfathers its names. */
        const mbj *lg = mbj_get(root, "legacy");
        if (lg && lg->t != MBJ_OBJ) { mb_error(b, "%s legacy must be an object", b->descriptor); goto done; }
        b->legacy = lg != NULL;
    }
    /* Everything else (the legacy strings and hud keys, composes, description,
     * an era B schema) is preserved verbatim under "extra". */
    {
        mbj tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.t = MBJ_OBJ;
        for (i = 0; i < root->count; i++) {
            static const char *const known[] = { "format", "id", "version", "name", "author", "homepage",
                                                 "requirements", "shadows", NULL };
            int k, is_known = 0;
            for (k = 0; known[k]; k++)
                if (strlen(known[k]) == root->klen[i] && memcmp(known[k], root->keys[i], root->klen[i]) == 0) is_known = 1;
            if (is_known) continue;
            if ((root->klen[i] == 7 && memcmp(root->keys[i], "strings", 7) == 0 && root->items[i]->t != MBJ_OBJ) ||
                (root->klen[i] == 3 && memcmp(root->keys[i], "hud", 3) == 0 && root->items[i]->t != MBJ_OBJ)) {
                mb_error(b, "%s %s must be an object", b->descriptor, root->keys[i]);
                free(tmp.items); free(tmp.keys); free(tmp.klen);
                goto done;
            }
            if (!mbj_push(&tmp, root->items[i], root->keys[i], root->klen[i])) {
                free(tmp.items); free(tmp.keys); free(tmp.klen);
                mb_error(b, "out of memory");
                goto done;
            }
            kept++;
        }
        if (kept) {
            if (!mbj_canon(&tmp, &extra, cause, sizeof cause)) {
                mb_error(b, "%s: %s", b->descriptor, cause);
                free(tmp.items); free(tmp.keys); free(tmp.klen);
                free(extra.p);
                goto done;
            }
            b->extra = extra.p;
        }
        free(tmp.items); free(tmp.keys); free(tmp.klen);   /* items and keys belong to root */
    }
    ok = 1;
done:
    mbj_free(root);
    return ok;
}

/* ---------------------------------------------------------------- walk ---- */

typedef struct mb_walk {
    sh_mb_bundle *b;
    wchar_t *root;          /* the mod folder, long form */
    int stopped;            /* a cap was hit; stop collecting */
} mb_walk;

static int mb_is_top_allowed(const char *name)
{
    return _stricmp(name, "mod.json") == 0 || _stricmp(name, "package.json") == 0 ||
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
            if (!req_lower) { mb_error(w->b, "out of memory"); free(name); break; }
            mb_lower(req_lower);
            if (!sh_mb_check_request(req_lower, cause, sizeof cause)) {
                mb_error(w->b, "%s: %s", child, cause);
            } else if (!sh_mb_extension_allowed(req_lower)) {
                mb_error(w->b, "file type not allowed: %s", child);
            } else if (size == 0) {
                mb_error(w->b, "empty file: %s", child);
            } else if (size > SH_MB_MAX_FILE) {
                mb_error(w->b, "file larger than 64 MiB: %s", child);
            } else if (w->b->file_count >= SH_MB_MAX_FILES) {
                mb_error(w->b, "more than %u files under assets/", SH_MB_MAX_FILES);
                w->stopped = 1;
            } else if (w->b->total_size + size > SH_MB_MAX_TOTAL) {
                mb_error(w->b, "assets/ larger than 256 MiB");
                w->stopped = 1;
            } else if ((f = mb_add_file(w->b)) == NULL) {
                mb_error(w->b, "out of memory");
            } else {
                f->path = mb_strdup(child);
                f->request = req_lower;
                req_lower = NULL;
                f->size = size;
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

static void mb_check_files(sh_mb_bundle *b, const wchar_t *root, const sh_mb_resolver *r)
{
    char sanitized[SH_MB_ID_MAX + 1], ns_decl[SH_MB_ID_MAX + 8], ns_raw[SH_MB_ID_MAX + 8],
         ns_raw_s[SH_MB_ID_MAX + 8];
    mb_req_index own = { NULL, 0 };
    sh_mb_resolver no_stock = { NULL, NULL, NULL, NULL };
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

        if (!sh_mb_logical_name(f->request, type, sizeof type, name, sizeof name)) {
            mb_error(b, "%s: generated/decls/<type>/ names no decl type this game ships, or the name is empty", f->path);
            continue;
        }
        f->ptype = mb_strdup(type);
        f->pname = mb_strdup(name);
        f->is_decl = mb_starts(f->request, "generated/decls/");
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

        if (f->is_decl) {
            size_t len = 0;
            unsigned char *text = mb_read_wfile(full, SH_MB_MAX_FILE, &len);
            if (!text) { mb_error(b, "cannot read %s", f->path); free(full); continue; }
            sh_mb_sha256_hex(text, len, f->blob);
            got = len;
            if (!sh_decl_text_well_formed(text, len)) {
                mb_error(b, "%s is not a well-formed decl (unbalanced braces, quotes or comments, or a NUL byte)", f->path);
            } else {
                mb_need_ctx nc = { f, 0 };
                sh_mb_scan_needs(f->ptype, text, len, mb_need_cb, &nc);
                if (nc.oom) mb_error(b, "out of memory");
                qsort(f->needs, f->need_count, sizeof(sh_mb_need), mb_need_cmp);
            }
            free(text);
        } else if (!mb_hash_wfile(full, &got, f->blob)) {
            mb_error(b, "cannot read %s", f->path);
            free(full);
            continue;
        }
        free(full);
        if (got != f->size) { mb_error(b, "%s changed while the bundle was built", f->path); continue; }

        /* Namespace: new names live under the mod's own id. A skins decl is
         * named by the mesh path it skins, so it passes the way a raw file
         * does: the name contains /mods/<id>/ or the engine's '_' spelling. */
        if (!f->shadowed) {
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
 * requirements shadows version; file keys: blob needs path provides request size.
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
        mb_puts(&o, "{\"blob\":");
        mb_put_jcstr(&o, f->blob);
        mb_puts(&o, ",\"needs\":[");
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
        mb_puts(&o, ",\"size\":");
        mb_put_u64(&o, f->size);
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

static void mb_finish(sh_mb_bundle *b)
{
    free(b->canonical);
    b->canonical = mb_emit(b, 0, 1, &b->canonical_len);
    if (!b->canonical) { mb_error(b, "out of memory"); return; }
    if (b->canonical_len > SH_MB_MANIFEST_MAX) mb_error(b, "the bundle manifest is larger than 1 MiB");
    sh_mb_sha256_hex(b->canonical, b->canonical_len, b->hash);
}

/* Contract 1 "build": the previous bundle.json in the folder decides it. Equal
 * content (ignoring build and bundle) keeps its number, changed content takes
 * the next one, and a folder without a readable previous build starts at 1. A
 * previous manifest that fails its own checks but still names a build counts
 * as changed, so a number is never reused for different content. */
static void mb_number_build(sh_mb_bundle *b, const char *folder)
{
    char path[MAX_PATH * 4];
    sh_mb_bundle prev;
    char *now, *then;
    size_t n_now = 0, n_then = 0;
    b->build = 1;
    if (b->errors) return;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.json", folder);
    if (!sh_mb_load(path, &prev)) {
        if (prev.build) b->build = prev.build + 1;
        sh_mb_free(&prev);
        return;
    }
    now = mb_emit(b, 0, 0, &n_now);
    then = mb_emit(&prev, 0, 0, &n_then);
    b->build = (now && then && n_now == n_then && memcmp(now, then, n_now) == 0) ? prev.build : prev.build + 1;
    free(now);
    free(then);
    sh_mb_free(&prev);
}

int sh_mb_build(const char *folder, const sh_mb_resolver *resolver, sh_mb_bundle *out)
{
    wchar_t *root, *desc_mod, *desc_pkg;
    DWORD attr;
    int has_mod, has_pkg;
    unsigned char *text;
    size_t len = 0;
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
    desc_mod = mb_wjoin(root, L"mod.json");
    desc_pkg = mb_wjoin(root, L"package.json");
    has_mod = desc_mod && GetFileAttributesW(desc_mod) != INVALID_FILE_ATTRIBUTES;
    has_pkg = desc_pkg && GetFileAttributesW(desc_pkg) != INVALID_FILE_ATTRIBUTES;
    if (has_mod && has_pkg) {
        mb_error(out, "the mod folder has both mod.json and package.json; keep one");
    } else if (!has_mod && !has_pkg) {
        mb_error(out, "the mod folder has no mod.json or package.json");
    } else {
        strcpy_s(out->descriptor, sizeof out->descriptor, has_mod ? "mod.json" : "package.json");
        text = mb_read_wfile(has_mod ? desc_mod : desc_pkg, SH_MB_DESCRIPTOR_MAX, &len);
        if (!text) mb_error(out, "cannot read %s (missing, unreadable or over 1 MiB)", out->descriptor);
        else { mb_parse_descriptor(out, (const char *)text, len); free(text); }
    }
    free(desc_mod);
    free(desc_pkg);
    if (out->errors) { free(root); return 0; }

    w.b = out;
    w.root = root;
    w.stopped = 0;
    mb_walk_top(&w);
    mb_check_files(out, root, resolver);
    free(root);
    mb_number_build(out, folder);
    mb_finish(out);
    return out->errors == 0;
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

int sh_mb_write(const sh_mb_bundle *b, const char *folder, char *cause, size_t cap)
{
    wchar_t *root, *path;
    char *text;
    size_t len = 0;
    int ok;
    if (!b || b->errors || !b->canonical) return mb_cause(cause, cap, "the bundle has errors and is not written");
    root = sh_mb_wpath(folder);
    path = root ? mb_wjoin(root, L"bundle.json") : NULL;
    free(root);
    text = sh_mb_bundle_json(b, &len);
    if (!path || !text) { free(path); free(text); return mb_cause(cause, cap, "out of memory"); }
    ok = mb_write_wfile(path, text, len);
    free(path);
    free(text);
    if (!ok) return mb_cause(cause, cap, "cannot write bundle.json (error %lu)", GetLastError());
    /* The exact bytes whose SHA-256 is the bundle hash: the body the store receives. */
    root = sh_mb_wpath(folder);
    path = root ? mb_wjoin(root, L"bundle.canonical.json") : NULL;
    free(root);
    ok = path && mb_write_wfile(path, b->canonical, b->canonical_len);
    free(path);
    if (!ok) return mb_cause(cause, cap, "cannot write bundle.canonical.json (error %lu)", GetLastError());
    return 1;
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
    const mbj *blob = mbj_get(e, "blob"), *needs = mbj_get(e, "needs"), *path = mbj_get(e, "path"),
              *prov = mbj_get(e, "provides"), *req = mbj_get(e, "request"), *size = mbj_get(e, "size");
    const mbj *pt = prov ? mbj_get(prov, "type") : NULL, *pn = prov ? mbj_get(prov, "name") : NULL;
    char cause[SH_MB_CAUSE_MAX], type[64], name[SH_MB_REQUEST_MAX + 16], *expect;
    sh_mb_file *f;
    size_t k;
    unsigned long long sz;

    if (e->t != MBJ_OBJ || !mb_only_keys(e, keys) || !blob || !needs || !path || !prov || !req || !size) {
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
    if (!sh_mb_extension_allowed(expect)) { mb_error(b, "bundle.json: file type not allowed: %s", path->s); free(expect); return 0; }
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
    if (!mb_is_hex64(blob)) { mb_error(b, "bundle.json %s: blob must be 64 lowercase hex characters", path->s); free(expect); return 0; }
    if (size->t != MBJ_NUM || !mb_plain_integer(size->s, size->n) || size->s[0] == '-' ||
        (sz = _strtoui64(size->s, NULL, 10)) == 0 || sz > SH_MB_MAX_FILE) {
        mb_error(b, "bundle.json %s: size must be 1 to 64 MiB", path->s);
        free(expect);
        return 0;
    }
    if (needs->t != MBJ_ARR) { mb_error(b, "bundle.json %s: needs must be an array", path->s); free(expect); return 0; }
    f = mb_add_file(b);
    if (!f) { mb_error(b, "out of memory"); free(expect); return 0; }
    f->path = mb_strdup(path->s);
    f->request = expect;
    memcpy(f->blob, blob->s, SH_MB_HASH_HEX + 1);
    f->size = sz;
    f->ptype = mb_strdup(type);
    f->pname = mb_strdup(name);
    f->is_decl = mb_starts(expect, "generated/decls/");
    f->shadowed = 0;
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

int sh_mb_load_text(const char *text, size_t len, sh_mb_bundle *out)
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
    for (i = 0; i < out->file_count; i++) out->files[i].shadowed = mb_is_shadow(out, out->files[i].request);
    mb_finish(out);
    if (!out->errors && strcmp(out->hash, bundle->s) != 0)
        mb_error(out, "bundle.json does not match its bundle hash (edited or damaged)");
done:
    mbj_free(root);
    return out->errors == 0;
}

int sh_mb_load(const char *path, sh_mb_bundle *out)
{
    size_t len = 0;
    unsigned char *text = sh_mb_read_file(path, SH_MB_MANIFEST_MAX + 65536, &len);
    int ok;
    if (!text) {
        if (out) {
            memset(out, 0, sizeof *out);
            strcpy_s(out->descriptor, sizeof out->descriptor, "bundle.json");
            mb_error(out, "cannot read bundle.json");
        }
        return 0;
    }
    ok = sh_mb_load_text((const char *)text, len, out);
    free(text);
    return ok;
}

/* ---------------------------------------------------------------- state ---- */

char *sh_mb_state_format(int validated, const char *first_cause, long long installed_at)
{
    mb_buf o = {0};
    char t[32];
    mb_puts(&o, "{\"firstCause\":");
    mb_put_jcstr(&o, first_cause ? first_cause : "");
    _snprintf_s(t, sizeof t, _TRUNCATE, ",\"installedAt\":%lld", installed_at);
    mb_puts(&o, t);
    mb_puts(&o, validated ? ",\"validated\":true}\n" : ",\"validated\":false}\n");
    if (o.oom) { free(o.p); return NULL; }
    return o.p;
}

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
