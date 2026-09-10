/* Shared bounded JSON shard scanning and splicing; payload families supply
 * grammar and policy.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_shards.h"

/* ==================================================================== */
/* SHA-256 (FIPS 180-4) for shard digests. */
/* ==================================================================== */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t fill;
} shard_sha256;

static const uint32_t SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_init(shard_sha256 *s)
{
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85; s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c; s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->bits = 0; s->fill = 0;
}

static void sha256_block(shard_sha256 *s, const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_update(shard_sha256 *s, const unsigned char *p, size_t n)
{
    s->bits += (uint64_t)n * 8;
    while (n) {
        size_t take = 64 - s->fill;
        if (take > n) take = n;
        memcpy(s->block + s->fill, p, take);
        s->fill += take; p += take; n -= take;
        if (s->fill == 64) { sha256_block(s, s->block); s->fill = 0; }
    }
}

/* Finish and write the FIRST 16 lowercase hex chars (the shard digest) + NUL. */
static void sha256_hex16(shard_sha256 *s, char out[SH_SHARD_DIGEST_CHARS + 1])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char tail[72];   /* 0x80, zero padding, 8 big-endian length bytes */
    uint64_t bits = s->bits;  /* captured BEFORE the padding is fed in */
    size_t pad_len = (s->fill < 56) ? (56 - s->fill) : (120 - s->fill);
    int i;
    memset(tail, 0, sizeof tail);
    tail[0] = 0x80;
    for (i = 0; i < 8; i++)
        tail[pad_len + (size_t)i] = (unsigned char)(bits >> (56 - i * 8));
    sha256_update(s, tail, pad_len + 8);   /* s->bits keeps growing; `bits` is already serialized */
    /* 16 hex chars = the first 8 digest bytes = h[0], h[1]. */
    for (i = 0; i < 8; i++) {
        unsigned char byte = (unsigned char)(s->h[i / 4] >> (24 - (i % 4) * 8));
        out[i * 2]     = hex[byte >> 4];
        out[i * 2 + 1] = hex[byte & 0xf];
    }
    out[SH_SHARD_DIGEST_CHARS] = '\0';
}

void sh_shard_digest16(const unsigned char *payload, size_t len,
                       char out[SH_SHARD_DIGEST_CHARS + 1])
{
    shard_sha256 h;
    sha256_init(&h);
    sha256_update(&h, payload, len);
    sha256_hex16(&h, out);
}

/* ==================================================================== */
/* small text helpers                                                    */
/* ==================================================================== */

const char *sh_shard_find(const char *hay, size_t n, const char *needle, size_t m)
{
    const char *end;
    if (!hay || !needle || m == 0 || n < m) return NULL;
    end = hay + n - m;
    for (const char *p = hay; p <= end; p++) {
        p = (const char *)memchr(p, needle[0], (size_t)(end - p) + 1);
        if (!p) return NULL;
        if (memcmp(p, needle, m) == 0) return p;
    }
    return NULL;
}

int sh_shard_is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
int sh_shard_is_digit(char c) { return c >= '0' && c <= '9'; }
int sh_shard_is_hex(char c) { return sh_shard_is_digit(c) || (c >= 'a' && c <= 'f'); }
int sh_shard_is_b64(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || sh_shard_is_digit(c) ||
           c == '+' || c == '/' || c == '=';
}

/* ==================================================================== */
/* base64                                                                */
/* ==================================================================== */

static const char SHARD_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t sh_shard_b64_encode(const unsigned char *p, size_t len, char *out)
{
    size_t i = 0, w = 0;
    while (i + 3 <= len) {
        unsigned v = ((unsigned)p[i] << 16) | ((unsigned)p[i + 1] << 8) | p[i + 2];
        out[w++] = SHARD_B64[(v >> 18) & 63];
        out[w++] = SHARD_B64[(v >> 12) & 63];
        out[w++] = SHARD_B64[(v >> 6) & 63];
        out[w++] = SHARD_B64[v & 63];
        i += 3;
    }
    if (len - i == 1) {
        unsigned v = (unsigned)p[i] << 16;
        out[w++] = SHARD_B64[(v >> 18) & 63];
        out[w++] = SHARD_B64[(v >> 12) & 63];
        out[w++] = '=';
        out[w++] = '=';
    } else if (len - i == 2) {
        unsigned v = ((unsigned)p[i] << 16) | ((unsigned)p[i + 1] << 8);
        out[w++] = SHARD_B64[(v >> 18) & 63];
        out[w++] = SHARD_B64[(v >> 12) & 63];
        out[w++] = SHARD_B64[(v >> 6) & 63];
        out[w++] = '=';
    }
    out[w] = '\0';
    return w;
}

unsigned char *sh_shard_b64_decode(const sh_shard_chunk *chunks, unsigned total,
                                   size_t max_payload, size_t *out_len, int *reason)
{
    static signed char table[256];
    static volatile LONG table_ready = 0;
    size_t total_chars = 0, cap, produced = 0;
    unsigned char *out;
    uint32_t acc = 0;
    int acc_n = 0, pad = 0;
    unsigned t;
    size_t j;

    if (reason) *reason = SH_SHARD_B64_MALFORMED;
    if (!chunks || !out_len) return NULL;
    *out_len = 0;

    if (!InterlockedCompareExchange(&table_ready, 0, 0)) {
        signed char tmp[256];
        int i;
        for (i = 0; i < 256; i++) tmp[i] = -1;
        for (i = 'A'; i <= 'Z'; i++) tmp[i] = (signed char)(i - 'A');
        for (i = 'a'; i <= 'z'; i++) tmp[i] = (signed char)(i - 'a' + 26);
        for (i = '0'; i <= '9'; i++) tmp[i] = (signed char)(i - '0' + 52);
        tmp['+'] = 62; tmp['/'] = 63;
        memcpy(table, tmp, sizeof table);
        InterlockedExchange(&table_ready, 1);
    }

    for (t = 0; t < total; t++) total_chars += chunks[t].n;
    if (total_chars % 4 != 0) return NULL;
    cap = total_chars / 4 * 3;
    if (cap > max_payload + 2) {
        if (reason) *reason = SH_SHARD_B64_TOO_BIG;
        return NULL;
    }
    out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, cap ? cap : 1);
    if (!out) {
        if (reason) *reason = SH_SHARD_B64_NOMEM;
        return NULL;
    }

    for (t = 0; t < total; t++) {
        for (j = 0; j < chunks[t].n; j++) {
            unsigned char c = (unsigned char)chunks[t].p[j];
            if (c == '=') {
                if (acc_n < 2) goto bad;   /* '=' only legal in the last quantum */
                pad++;
                if (pad > 2) goto bad;
                acc = (acc << 6);
                acc_n++;
            } else {
                signed char v = table[c];
                if (v < 0 || pad) goto bad;   /* data after padding = malformed */
                acc = (acc << 6) | (uint32_t)v;
                acc_n++;
            }
            if (acc_n == 4) {
                int emit = 3 - pad;
                if (produced + (size_t)emit > cap) goto bad;
                out[produced]     = (unsigned char)(acc >> 16);
                if (emit > 1) out[produced + 1] = (unsigned char)(acc >> 8);
                if (emit > 2) out[produced + 2] = (unsigned char)acc;
                produced += (size_t)emit;
                acc = 0; acc_n = 0;
                if (pad) { t = total; break; }   /* padding ends the stream */
            }
        }
    }
    if (acc_n != 0) goto bad;
    if (produced > max_payload) {
        if (reason) *reason = SH_SHARD_B64_TOO_BIG;
        HeapFree(GetProcessHeap(), 0, out);
        return NULL;
    }
    if (reason) *reason = SH_SHARD_B64_OK;
    *out_len = produced;
    return out;
bad:
    if (reason) *reason = SH_SHARD_B64_MALFORMED;
    HeapFree(GetProcessHeap(), 0, out);
    return NULL;
}

/* ==================================================================== */
/* the shard scanner                                                     */
/* ==================================================================== */

/* Bound the name-to-initialValue search window; engine layouts usually
 * separate them by 100-350 bytes.
 */
#define SHARD_VALUE_WINDOW 4096

/* Is the string starting at `quote` (the opening '"' of the header) the value
 * of a "name" key?  ..."name" <ws> : <ws> "smpkg... */
static int shard_is_name_value(const char *json, const char *quote)
{
    const char *r = quote - 1;
    while (r >= json && sh_shard_is_ws(*r)) r--;
    if (r < json || *r != ':') return 0;
    r--;
    while (r >= json && sh_shard_is_ws(*r)) r--;
    if (r - 5 < json) return 0;
    return memcmp(r - 5, "\"name\"", 6) == 0;
}

/* Find the base64 span after a header. Return 1 with chunk and chunk_len,
 * including empty chunks; 0 means unreadable.
 */
static int shard_find_chunk(const char *hdr_end, const char *end,
                            const char **chunk, size_t *chunk_len)
{
    size_t window = (size_t)(end - hdr_end);
    const char *key, *v;
    size_t n = 0;
    if (window > SHARD_VALUE_WINDOW) window = SHARD_VALUE_WINDOW;
    key = sh_shard_find(hdr_end, window, "\"initialValue\"", 14);
    if (!key) return 0;
    v = key + 14;
    while (v < end && sh_shard_is_ws(*v)) v++;
    if (v >= end || *v != ':') return 0;
    v++;
    while (v < end && sh_shard_is_ws(*v)) v++;
    if (v >= end || *v != '"') return 0;
    v++;
    *chunk = v;
    while (v + n < end && n <= SH_SHARD_MAX_CHUNK) {
        char c = v[n];
        if (c == '"') { *chunk_len = n; return 1; }
        if (!sh_shard_is_b64(c)) return 0;   /* incl '\\': never legal base64 */
        n++;
    }
    return 0;   /* no closing quote within the cap */
}

int sh_shard_next(const char *json, size_t len, const char *magic, size_t magic_len,
                  size_t *pos, const char **hdr, size_t *hdr_len,
                  const char **chunk, size_t *chunk_len)
{
    const char *end;
    if (!json || !magic || !pos || !hdr || !hdr_len || !chunk || !chunk_len) return 0;
    end = json + len;
    while (*pos < len) {
        const char *p = sh_shard_find(json + *pos, len - *pos, magic, magic_len);
        const char *q;
        size_t window, n;
        if (!p) return 0;
        *pos = (size_t)(p - json) + 1;   /* resume past this occurrence next time */
        if (p == json || p[-1] != '"') continue;    /* magic must start the string */
        if (!shard_is_name_value(json, p - 1)) continue;

        /* the closing quote, within the header cap */
        window = (size_t)(end - p);
        if (window > SH_SHARD_HEADER_MAX) window = SH_SHARD_HEADER_MAX;
        q = (const char *)memchr(p, '"', window);
        if (!q) continue;
        n = (size_t)(q - p);

        /* Require the nearby snapVarInfo_t marker to reject ordinary entity
         * names resembling shard headers.
         */
        {
            size_t w = (size_t)(end - (q + 1));
            if (w > 256) w = 256;
            if (!sh_shard_find(q + 1, w, "snapVarInfo_t", 13)) continue;
        }

        *hdr = p;
        *hdr_len = n;
        *chunk = NULL;
        *chunk_len = 0;
        if (!shard_find_chunk(q + 1, end, chunk, chunk_len)) *chunk = NULL;
        *pos = (size_t)(q + 1 - json);
        return 1;
    }
    return 0;
}

/* ==================================================================== */
/* document structure                                                    */
/* ==================================================================== */

int sh_shard_doc_build(const char *json, size_t len, sh_shard_doc *doc)
{
    sh_shard_container *c;
    unsigned stack[SH_SHARD_MAX_DEPTH];
    size_t count = 0, i, cap;
    unsigned depth = 0;
    int in_string = 0;

    if (!doc) return 0;
    doc->c = NULL;
    doc->count = 0;
    if (!json) return 0;
    cap = SH_SHARD_CONTAINERS_MIN;
    c = (sh_shard_container *)HeapAlloc(GetProcessHeap(), 0,
                                        cap * sizeof(sh_shard_container));
    if (!c) return 0;

    for (i = 0; i < len; i++) {
        char ch = json[i];
        if (in_string) {
            if (ch == '\\') { i++; continue; }
            if (ch == '"') in_string = 0;
            continue;
        }
        if (ch == '"') { in_string = 1; continue; }
        if (ch == '{' || ch == '[') {
            if (count >= SH_SHARD_MAX_CONTAINERS || depth >= SH_SHARD_MAX_DEPTH) goto fail;
            if (count == cap) {
                sh_shard_container *bigger;
                size_t ncap = cap * 2;
                if (ncap > SH_SHARD_MAX_CONTAINERS) ncap = SH_SHARD_MAX_CONTAINERS;
                bigger = (sh_shard_container *)HeapReAlloc(
                    GetProcessHeap(), 0, c, ncap * sizeof(sh_shard_container));
                if (!bigger) goto fail;
                c = bigger;
                cap = ncap;
            }
            c[count].open = i;
            c[count].close = 0;
            c[count].kind = ch;
            c[count].parent = depth ? (int)stack[depth - 1] : -1;
            stack[depth++] = (unsigned)count;
            count++;
            continue;
        }
        if (ch == '}' || ch == ']') {
            unsigned idx;
            if (depth == 0) goto fail;
            idx = stack[--depth];
            if (c[idx].kind != (ch == '}' ? '{' : '[')) goto fail;
            c[idx].close = i;
        }
    }
    if (depth != 0 || in_string || count == 0) goto fail;

    doc->c = c;
    doc->count = count;
    return 1;

fail:
    HeapFree(GetProcessHeap(), 0, c);
    return 0;
}

void sh_shard_doc_free(sh_shard_doc *doc)
{
    if (!doc || !doc->c) return;
    HeapFree(GetProcessHeap(), 0, doc->c);
    doc->c = NULL;
    doc->count = 0;
}

int sh_shard_doc_innermost(const sh_shard_doc *doc, size_t off)
{
    int best = -1;
    size_t i;
    for (i = 0; i < doc->count; i++) {
        if (doc->c[i].open < off && off < doc->c[i].close) best = (int)i;
    }
    return best;
}

int sh_shard_doc_array_element(const sh_shard_doc *doc, int idx)
{
    int guard = 0;
    while (idx >= 0 && guard++ < (int)SH_SHARD_MAX_DEPTH) {
        int p = doc->c[idx].parent;
        if (doc->c[idx].kind == '{' && p >= 0 && doc->c[p].kind == '[') return idx;
        idx = p;
    }
    return -1;
}

int sh_shard_doc_member(const char *json, size_t len, const sh_shard_doc *doc,
                        int parent, const char *key)
{
    size_t klen = strlen(key);
    size_t at, stop;

    if (parent < 0 || (size_t)parent >= doc->count) return -1;
    at = doc->c[parent].open;
    stop = doc->c[parent].close;

    while (at < stop) {
        const char *q = sh_shard_find(json + at, stop - at, key, klen);
        size_t koff, v;
        size_t i;
        int found = -1;
        if (!q) return -1;
        koff = (size_t)(q - json);
        at = koff + 1;
        if (koff == 0 || json[koff - 1] != '"') continue;
        if (koff + klen >= len || json[koff + klen] != '"') continue;
        v = koff + klen + 1;
        while (v < stop && sh_shard_is_ws(json[v])) v++;
        if (v >= stop || json[v] != ':') continue;
        v++;
        while (v < stop && sh_shard_is_ws(json[v])) v++;
        if (v >= stop || (json[v] != '{' && json[v] != '[')) continue;
        for (i = 0; i < doc->count; i++) {
            if (doc->c[i].open == v) { found = (int)i; break; }
        }
        if (found < 0) continue;
        /* the key itself must be a DIRECT child of `parent`, not of something nested in it */
        if (sh_shard_doc_innermost(doc, koff) != parent) continue;
        return found;
    }
    return -1;
}

int sh_shard_doc_flat_element(const char *json, const sh_shard_doc *doc, int arr,
                              unsigned index, size_t *from, size_t *to)
{
    size_t p, stop;
    unsigned n = 0;
    if (arr < 0 || (size_t)arr >= doc->count) return 0;
    p = doc->c[arr].open + 1;
    stop = doc->c[arr].close;
    while (p < stop) {
        size_t start;
        while (p < stop && sh_shard_is_ws(json[p])) p++;
        start = p;
        while (p < stop && json[p] != ',') {
            if (json[p] == '{' || json[p] == '[' || json[p] == '"') return 0;
            p++;
        }
        if (n == index) {
            size_t e = p;
            while (e > start && sh_shard_is_ws(json[e - 1])) e--;
            *from = start;
            *to = e;
            return 1;
        }
        n++;
        p++;   /* past the comma */
    }
    return 0;
}

unsigned sh_shard_doc_array_count(const char *json, const sh_shard_doc *doc, int arr)
{
    size_t p, stop;
    unsigned n = 0;
    int depth = 0, in_string = 0, any = 0;
    if (arr < 0 || (size_t)arr >= doc->count) return 0;
    p = doc->c[arr].open + 1;
    stop = doc->c[arr].close;
    for (; p < stop; p++) {
        char ch = json[p];
        if (in_string) {
            if (ch == '\\') p++;
            else if (ch == '"') in_string = 0;
            continue;
        }
        if (ch == '"') { in_string = 1; any = 1; continue; }
        if (ch == '{' || ch == '[') { depth++; any = 1; continue; }
        if (ch == '}' || ch == ']') { depth--; continue; }
        if (ch == ',' && depth == 0) { n++; continue; }
        if (!sh_shard_is_ws(ch)) any = 1;
    }
    return any ? n + 1 : 0;
}

/* ==================================================================== */
/* strip                                                                 */
/* ==================================================================== */

typedef struct shard_cut { size_t from, to; } shard_cut;

static int shard_cut_cmp(const void *a, const void *b)
{
    const shard_cut *x = (const shard_cut *)a, *y = (const shard_cut *)b;
    if (x->from < y->from) return -1;
    if (x->from > y->from) return 1;
    return 0;
}

char *sh_shard_strip(const char *json, size_t len, const char *magic, size_t magic_len,
                     sh_shard_filter_fn filter, void *ctx, size_t max_cuts,
                     size_t *out_len, unsigned *elements_out, unsigned *runs_out,
                     int *doc_failed)
{
    sh_shard_doc doc;
    shard_cut *cuts = NULL;
    size_t cut_count = 0, pos = 0, i, w = 0, elements = 0;
    const char *hdr, *chunk;
    size_t hdr_len, chunk_len;
    char *out = NULL;

    if (out_len) *out_len = 0;
    if (elements_out) *elements_out = 0;
    if (runs_out) *runs_out = 0;
    if (doc_failed) *doc_failed = 0;
    if (!json || len == 0 || max_cuts == 0) return NULL;

    /* Skip the structural pass for payload-free maps. */
    if (!sh_shard_find(json, len, magic, magic_len)) return NULL;
    if (!sh_shard_doc_build(json, len, &doc)) {
        if (doc_failed) *doc_failed = 1;
        return NULL;
    }

    cuts = (shard_cut *)HeapAlloc(GetProcessHeap(), 0, max_cuts * sizeof(shard_cut));
    if (!cuts) { sh_shard_doc_free(&doc); return NULL; }

    while (cut_count < max_cuts &&
           sh_shard_next(json, len, magic, magic_len, &pos, &hdr, &hdr_len, &chunk, &chunk_len)) {
        int el;
        size_t from, to;
        int dup = 0;
        if (filter && !filter(hdr, hdr_len, ctx)) continue;
        el = sh_shard_doc_array_element(&doc, sh_shard_doc_innermost(&doc, pos));
        if (el < 0) continue;
        from = doc.c[el].open;
        to   = doc.c[el].close + 1;
        for (i = 0; i < cut_count; i++) if (cuts[i].from == from) { dup = 1; break; }
        if (dup) continue;

        cuts[cut_count].from = from;
        cuts[cut_count].to = to;
        cut_count++;
        elements++;
    }

    if (cut_count == 0) {
        HeapFree(GetProcessHeap(), 0, cuts);
        sh_shard_doc_free(&doc);
        return NULL;
    }

    qsort(cuts, cut_count, sizeof(shard_cut), shard_cut_cmp);

    /* Merge adjacent removal spans so each run claims one delimiter. */
    {
        size_t w2 = 0;
        for (i = 1; i < cut_count; i++) {
            size_t g = cuts[w2].to;
            while (g < len && sh_shard_is_ws(json[g])) g++;
            if (g < len && json[g] == ',') {
                g++;
                while (g < len && sh_shard_is_ws(json[g])) g++;
                if (g == cuts[i].from) { cuts[w2].to = cuts[i].to; continue; }
            }
            cuts[++w2] = cuts[i];
        }
        cut_count = w2 + 1;
    }

    /* Remove the preceding comma, or the following comma for an initial run. */
    for (i = 0; i < cut_count; i++) {
        size_t b = cuts[i].from;
        while (b > 0 && sh_shard_is_ws(json[b - 1])) b--;
        if (b > 0 && json[b - 1] == ',') {
            cuts[i].from = b - 1;
        } else {
            size_t a = cuts[i].to;
            while (a < len && sh_shard_is_ws(json[a])) a++;
            if (a < len && json[a] == ',') cuts[i].to = a + 1;
        }
    }
    /* Reject overlapping cuts rather than corrupting JSON. */
    for (i = 1; i < cut_count; i++) {
        if (cuts[i].from < cuts[i - 1].to) {
            if (doc_failed) *doc_failed = 1;
            HeapFree(GetProcessHeap(), 0, cuts);
            sh_shard_doc_free(&doc);
            return NULL;
        }
    }

    out = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (!out) {
        HeapFree(GetProcessHeap(), 0, cuts);
        sh_shard_doc_free(&doc);
        return NULL;
    }
    pos = 0;
    for (i = 0; i < cut_count; i++) {
        size_t run = cuts[i].from - pos;
        memcpy(out + w, json + pos, run);
        w += run;
        pos = cuts[i].to;
    }
    memcpy(out + w, json + pos, len - pos);
    w += len - pos;
    out[w] = '\0';

    HeapFree(GetProcessHeap(), 0, cuts);
    sh_shard_doc_free(&doc);
    if (out_len) *out_len = w;
    if (elements_out) *elements_out = (unsigned)elements;
    if (runs_out) *runs_out = (unsigned)cut_count;
    return out;
}

/* ==================================================================== */
/* insert                                                                */
/* ==================================================================== */

/* Write the compact snapVarString_t envelope. */
static size_t shard_write_var(char *out, const char *header, const char *chunk, size_t chunk_len)
{
    static const char PRE[] =
        "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,"
        "\"~type\":\"|pointer\"},\"name\":\"";
    static const char MID[] = "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"";
    static const char POST[] = "\",\"~type\":\"snapVarString_t\"}";
    size_t w = 0, n;
    n = sizeof PRE - 1;      memcpy(out + w, PRE, n);      w += n;
    n = strlen(header);      memcpy(out + w, header, n);   w += n;
    n = sizeof MID - 1;      memcpy(out + w, MID, n);      w += n;
    memcpy(out + w, chunk, chunk_len);                     w += chunk_len;
    n = sizeof POST - 1;     memcpy(out + w, POST, n);     w += n;
    return w;
}

static void shard_err(char *err, size_t cap, const char *msg)
{
    if (!err || cap == 0) return;
    strncpy_s(err, cap, msg, _TRUNCATE);
}

char *sh_shard_insert(const char *json, size_t len,
                      const sh_shard_out *shards, size_t count,
                      size_t *out_len, char *err, size_t err_cap)
{
    sh_shard_doc doc;
    char *out = NULL;
    size_t i, insert, w = 0, need, chunk_total = 0, header_total = 0;
    int vars, bucket, alloc;
    unsigned existing;

    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = '\0';
    if (!json || len == 0 || !shards || count == 0) {
        shard_err(err, err_cap, "insert called with nothing to insert");
        return NULL;
    }

    if (!sh_shard_doc_build(json, len, &doc)) {
        shard_err(err, err_cap, "map JSON did not read cleanly; nothing embedded");
        return NULL;
    }
    vars = sh_shard_doc_member(json, len, &doc, 0, "variables");
    if (vars < 0 || doc.c[vars].kind != '{') {
        shard_err(err, err_cap, "map has no variables block");
        goto fail;
    }
    bucket = sh_shard_doc_member(json, len, &doc, vars, "string");
    if (bucket < 0 || doc.c[bucket].kind != '[') {
        shard_err(err, err_cap, "map has no variables.string list");
        goto fail;
    }
    alloc = sh_shard_doc_member(json, len, &doc, vars, "allocCount");
    if (alloc < 0 || doc.c[alloc].kind != '[') {
        shard_err(err, err_cap, "map has no variables.allocCount list");
        goto fail;
    }

    for (i = 0; i < count; i++) {
        if (!shards[i].header || !shards[i].chunk) {
            shard_err(err, err_cap, "a shard is missing its header or its chunk");
            goto fail;
        }
        header_total += strlen(shards[i].header);
        chunk_total += shards[i].chunk_len;
    }

    existing = sh_shard_doc_array_count(json, &doc, bucket);

    /* Worst case: everything before the insert point, every shard with its
     * wrapper and comma, everything after, and room for allocCount growing by a
     * few digits. */
    need = len + chunk_total + header_total + count * 256 + 64;
    out = (char *)HeapAlloc(GetProcessHeap(), 0, need + 1);
    if (!out) { shard_err(err, err_cap, "out of memory building the map"); goto fail; }

    insert = doc.c[bucket].close;       /* just before the ']' */
    memcpy(out, json, insert);
    w = insert;
    for (i = 0; i < count; i++) {
        if (existing || i) out[w++] = ',';
        w += shard_write_var(out + w, shards[i].header, shards[i].chunk, shards[i].chunk_len);
    }
    memcpy(out + w, json + insert, len - insert);
    w += len - insert;
    out[w] = '\0';
    sh_shard_doc_free(&doc);

    /* Update allocCount[STRING] after insertion, which moves its byte offset. */
    {
        sh_shard_doc doc2;
        int vars2, alloc2;
        size_t from, to;
        if (!sh_shard_doc_build(out, w, &doc2)) {
            shard_err(err, err_cap, "the embedded map did not read back cleanly");
            HeapFree(GetProcessHeap(), 0, out);
            return NULL;
        }
        vars2 = sh_shard_doc_member(out, w, &doc2, 0, "variables");
        alloc2 = vars2 >= 0 ? sh_shard_doc_member(out, w, &doc2, vars2, "allocCount") : -1;
        if (alloc2 < 0 || !sh_shard_doc_flat_element(out, &doc2, alloc2, 4, &from, &to)) {
            sh_shard_doc_free(&doc2);
            shard_err(err, err_cap, "map has no variables.allocCount[4] slot to update");
            HeapFree(GetProcessHeap(), 0, out);
            return NULL;
        }
        {
            char text[16];
            int n = _snprintf_s(text, sizeof text, _TRUNCATE, "%u",
                                (unsigned)(existing + count));
            size_t tail = w - to;
            char *fin = (char *)HeapAlloc(GetProcessHeap(), 0, from + (size_t)n + tail + 1);
            if (!fin) {
                sh_shard_doc_free(&doc2);
                shard_err(err, err_cap, "out of memory writing the variable count");
                HeapFree(GetProcessHeap(), 0, out);
                return NULL;
            }
            memcpy(fin, out, from);
            memcpy(fin + from, text, (size_t)n);
            memcpy(fin + from + n, out + to, tail);
            w = from + (size_t)n + tail;
            fin[w] = '\0';
            HeapFree(GetProcessHeap(), 0, out);
            out = fin;
        }
        sh_shard_doc_free(&doc2);
    }

    if (out_len) *out_len = w;
    return out;

fail:
    sh_shard_doc_free(&doc);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    return NULL;
}
