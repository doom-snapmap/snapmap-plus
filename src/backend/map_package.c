/* map_package.c -- see map_package.h. Map-embedded override packages: the C
 * consumer of the smpkg wire format (reference: dev repo src/map_package.py),
 * the pre-parse load gate, and the consent-gated installer.
 *
 * Everything here operates on a raw JSON text buffer -- no JSON DOM is ever
 * built (the buffer can be megabytes and the no-shard fast path must stay
 * one substring sweep). A shard is only recognised when the full header
 * grammar matches AND the header is the string value of a "name" key, so a
 * map whose prose merely contains "smpkg." is not perturbed.
 *
 * Divergences from the python reference, all crafted-map corners whose
 * outcome class (refusal) is unchanged:
 *   - base64 is decoded strictly (python's b64decode quietly discards
 *     non-alphabet bytes); a shard with junk in it fails here rather than
 *     being repaired -- the sha256 digest would refuse it either way.
 *   - a header with total==0 or an out-of-range index is rejected as
 *     malformed here; python surfaces the same maps as "incomplete".
 *   - zip member paths additionally refuse '\', ':', control chars and
 *     '.' segments (python refuses only absolute and '..'); stricter is
 *     correct for an installer fed a stranger's map.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <shellapi.h>
#pragma comment(lib, "user32.lib")

#include <stdlib.h>
#include "map_package.h"
#include "engine_dialog.h"
#include "packages.h"
#include "decl_server.h"
#include "raw_deflate.h"
#include "backend_log.h"

/* ==================================================================== */
/* sha256 -- standard FIPS 180-4, needed for the shard digest. Self-     */
/* contained; the backend had no hash primitive before this.             */
/* ==================================================================== */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t fill;
} mpkg_sha256;

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

static void sha256_init(mpkg_sha256 *s)
{
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85; s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c; s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->bits = 0; s->fill = 0;
}

static void sha256_block(mpkg_sha256 *s, const unsigned char *p)
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

static void sha256_update(mpkg_sha256 *s, const unsigned char *p, size_t n)
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

/* Finish and write the FIRST 16 lowercase hex chars (the smpkg digest) + NUL. */
static void sha256_hex16(mpkg_sha256 *s, char out[SH_MPKG_DIGEST_CHARS + 1])
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
    out[SH_MPKG_DIGEST_CHARS] = '\0';
}

/* ==================================================================== */
/* small text helpers                                                    */
/* ==================================================================== */

static const char *mpkg_find(const char *hay, size_t n, const char *needle, size_t m)
{
    const char *end;
    if (m == 0 || n < m) return NULL;
    end = hay + n - m;
    for (const char *p = hay; p <= end; p++) {
        p = (const char *)memchr(p, needle[0], (size_t)(end - p) + 1);
        if (!p) return NULL;
        if (memcmp(p, needle, m) == 0) return p;
    }
    return NULL;
}

static int mpkg_is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int mpkg_is_digit(char c) { return c >= '0' && c <= '9'; }
static int mpkg_is_hex(char c) { return mpkg_is_digit(c) || (c >= 'a' && c <= 'f'); }
static int mpkg_is_idc(char c)
{
    return (c >= 'a' && c <= 'z') || mpkg_is_digit(c) || c == '_' || c == '-';
}
static int mpkg_is_b64(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || mpkg_is_digit(c) ||
           c == '+' || c == '/' || c == '=';
}

static void mpkg_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || cap == 0) return;
    va_start(ap, fmt);
    _vsnprintf_s(err, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* ==================================================================== */
/* the shard iterator -- the shared scanner under scan() and extract()   */
/* ==================================================================== */

typedef struct mpkg_hdr {
    char     id[SH_MPKG_ID_CAP];
    char     digest[SH_MPKG_DIGEST_CHARS + 1];
    unsigned idx, total;
} mpkg_hdr;

#define MPKG_HEADER_MAGIC   "smpkg."
#define MPKG_MAGIC_LEN      6
/* How far past a header its initialValue may sit. In both the compact and
 * the pretty engine layouts the gap is ~100-350 bytes (name -> info's
 * "~type" -> "initialValue"); 4096 is generous without letting the search
 * wander into the next variable. */
#define MPKG_VALUE_WINDOW   4096

/* Parse one header at `p` (which points at "smpkg."), bounded by `end`.
 * Returns the char just past the closing quote, or NULL if not a header. */
static const char *mpkg_parse_header(const char *p, const char *end, mpkg_hdr *hdr)
{
    const char *c = p + MPKG_MAGIC_LEN;
    size_t n = 0;
    unsigned long v;
    int digits;

    /* package id: [a-z0-9_-]+ */
    while (c < end && mpkg_is_idc(*c) && n < SH_MPKG_ID_CAP - 1) hdr->id[n++] = *c++;
    if (n == 0 || c >= end || *c != '.') return NULL;
    hdr->id[n] = '\0';
    c++;

    /* index */
    v = 0; digits = 0;
    while (c < end && mpkg_is_digit(*c) && digits < 8) { v = v * 10 + (unsigned)(*c - '0'); c++; digits++; }
    if (digits == 0 || digits >= 8 || c >= end || *c != '.') return NULL;
    hdr->idx = (unsigned)v;
    c++;

    /* total: 1..SH_MPKG_MAX_SHARDS */
    v = 0; digits = 0;
    while (c < end && mpkg_is_digit(*c) && digits < 8) { v = v * 10 + (unsigned)(*c - '0'); c++; digits++; }
    if (digits == 0 || digits >= 8 || v == 0 || v > SH_MPKG_MAX_SHARDS) return NULL;
    if (c >= end || *c != '.') return NULL;
    hdr->total = (unsigned)v;
    c++;

    /* digest: exactly 16 lowercase hex */
    for (n = 0; n < SH_MPKG_DIGEST_CHARS; n++) {
        if (c >= end || !mpkg_is_hex(*c)) return NULL;
        hdr->digest[n] = *c++;
    }
    hdr->digest[SH_MPKG_DIGEST_CHARS] = '\0';
    if (c >= end || *c != '"') return NULL;
    return c + 1;   /* past the closing quote */
}

/* Is the string starting at `quote` (the opening '"' of the header) the
 * value of a "name" key?  ..."name" <ws> : <ws> "smpkg... */
static int mpkg_is_name_value(const char *json, const char *quote)
{
    const char *r = quote - 1;
    while (r >= json && mpkg_is_ws(*r)) r--;
    if (r < json || *r != ':') return 0;
    r--;
    while (r >= json && mpkg_is_ws(*r)) r--;
    if (r - 5 < json) return 0;
    return memcmp(r - 5, "\"name\"", 6) == 0;
}

/* Find the shard's base64 chunk after its header. Returns 1 with
 * *chunk/*chunk_len (chunk may legally be empty), 0 = unreadable. */
static int mpkg_find_chunk(const char *hdr_end, const char *end,
                           const char **chunk, size_t *chunk_len)
{
    size_t window = (size_t)(end - hdr_end);
    const char *key, *v;
    size_t n = 0;
    if (window > MPKG_VALUE_WINDOW) window = MPKG_VALUE_WINDOW;
    key = mpkg_find(hdr_end, window, "\"initialValue\"", 14);
    if (!key) return 0;
    v = key + 14;
    while (v < end && mpkg_is_ws(*v)) v++;
    if (v >= end || *v != ':') return 0;
    v++;
    while (v < end && mpkg_is_ws(*v)) v++;
    if (v >= end || *v != '"') return 0;
    v++;
    *chunk = v;
    while (v + n < end && n <= SH_MPKG_MAX_CHUNK) {
        char c = v[n];
        if (c == '"') { *chunk_len = n; return 1; }
        if (!mpkg_is_b64(c)) return 0;   /* incl '\\': never legal base64 */
        n++;
    }
    return 0;   /* no closing quote within the cap */
}

/* Advance the iterator: find the next VALID shard at/after *pos. Malformed
 * or non-name "smpkg." occurrences are skipped silently (they are not
 * shards). Returns 1 = found (chunk==NULL means the header parsed but its
 * value is unreadable -- the package is then refused, never guessed). */
static int mpkg_next_shard(const char *json, size_t len, size_t *pos,
                           mpkg_hdr *hdr, const char **chunk, size_t *chunk_len)
{
    const char *end = json + len;
    while (*pos < len) {
        const char *p = mpkg_find(json + *pos, len - *pos, MPKG_HEADER_MAGIC, MPKG_MAGIC_LEN);
        const char *hdr_end;
        if (!p) return 0;
        *pos = (size_t)(p - json) + 1;   /* resume past this occurrence next time */
        if (p == json || p[-1] != '"') continue;
        hdr_end = mpkg_parse_header(p, end, hdr);
        if (!hdr_end) continue;
        if (!mpkg_is_name_value(json, p - 1)) continue;
        /* A shard variable's name lives in a snapVarInfo_t, whose "~type"
         * follows the name within a few dozen bytes in both the compact and
         * pretty layouts. An ENTITY merely named like a header (entity names
         * are author-controlled free text) has no such marker and is not a
         * shard -- the reference implementation reads only variables.string,
         * and this check is what keeps the C scanner equally scoped. */
        {
            size_t w = (size_t)(end - hdr_end);
            if (w > 256) w = 256;
            if (!mpkg_find(hdr_end, w, "snapVarInfo_t", 13)) continue;
        }
        *chunk = NULL;
        *chunk_len = 0;
        if (!mpkg_find_chunk(hdr_end, end, chunk, chunk_len)) *chunk = NULL;
        *pos = (size_t)(hdr_end - json);
        return 1;
    }
    return 0;
}

/* ==================================================================== */
/* strip                                                                 */
/* ==================================================================== */

/* Structural map of the document: for every container ('{' or '[') its open and close offsets
 * and its parent. Built in one forward pass that understands string literals and escapes, so
 * a brace inside an author's map name is never mistaken for structure.
 *
 * This exists because removing a shard means removing the whole ARRAY ELEMENT that holds it,
 * and the element's bounds cannot be found by scanning backwards through JSON -- backwards, you
 * cannot tell whether a '{' you just passed was structure or text. Forwards, you always can. */
#define MPKG_MAX_CONTAINERS 8192u
#define MPKG_MAX_DEPTH      256u

typedef struct mpkg_container {
    size_t open;        /* offset of '{' or '[' */
    size_t close;       /* offset of the matching '}' or ']' */
    int    parent;      /* index into the container array, -1 for the root */
    char   kind;        /* '{' or '[' */
} mpkg_container;

typedef struct mpkg_structure {
    mpkg_container *c;
    size_t          count;
} mpkg_structure;

/* Returns 1 and fills `st` (caller HeapFrees st->c), 0 if the document is not structurally
 * clean or is larger than the caps. A 0 return means "do not touch this buffer". */
static int mpkg_structure_build(const char *json, size_t len, mpkg_structure *st)
{
    mpkg_container *c;
    unsigned stack[MPKG_MAX_DEPTH];
    size_t count = 0, i;
    unsigned depth = 0;
    int in_string = 0;

    st->c = NULL;
    st->count = 0;
    c = (mpkg_container *)HeapAlloc(GetProcessHeap(), 0,
                                    MPKG_MAX_CONTAINERS * sizeof(mpkg_container));
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
            if (count >= MPKG_MAX_CONTAINERS || depth >= MPKG_MAX_DEPTH) goto fail;
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

    st->c = c;
    st->count = count;
    return 1;

fail:
    HeapFree(GetProcessHeap(), 0, c);
    return 0;
}

/* The innermost container holding `off`. Containers are recorded in open order, so the LAST one
 * whose span contains the offset is the innermost. */
static int mpkg_innermost(const mpkg_structure *st, size_t off)
{
    int best = -1;
    size_t i;
    for (i = 0; i < st->count; i++) {
        if (st->c[i].open < off && off < st->c[i].close) best = (int)i;
    }
    return best;
}

/* Walk up from `idx` to the object that is a direct element of an array -- the map variable
 * itself. Returns -1 if there is no such ancestor (the shard is not where we think it is, so
 * nothing is removed). */
static int mpkg_array_element(const mpkg_structure *st, int idx)
{
    int guard = 0;
    while (idx >= 0 && guard++ < (int)MPKG_MAX_DEPTH) {
        int p = st->c[idx].parent;
        if (st->c[idx].kind == '{' && p >= 0 && st->c[p].kind == '[') return idx;
        idx = p;
    }
    return -1;
}

typedef struct mpkg_cut { size_t from, to; } mpkg_cut;

static int mpkg_cut_cmp(const void *a, const void *b)
{
    const mpkg_cut *x = (const mpkg_cut *)a, *y = (const mpkg_cut *)b;
    if (x->from < y->from) return -1;
    if (x->from > y->from) return 1;
    return 0;
}

/* Remove every shard variable from `json`, returning a new NUL-terminated HeapAlloc'd buffer
 * (caller HeapFrees) with *out_len set, or NULL when there is nothing to remove or the document
 * cannot be stripped safely -- in which case the caller uses the original buffer unchanged.
 *
 * Refusing rather than half-stripping is deliberate. A map with a mangled payload still has to
 * load; a map with mangled JSON does not load at all. */
/* Strip shard variables, optionally only those belonging to `pkg_id`.
 *
 * The filter is what makes embedding idempotent WITHOUT being destructive: re-embedding a
 * package must replace its own shards and leave every other package's alone. */
static char *mpkg_strip_scoped(const char *json, size_t len, const char *pkg_id, size_t *out_len)
{
    mpkg_structure st;
    mpkg_cut *cuts = NULL;
    size_t cut_count = 0, pos = 0, i, w = 0, elements = 0;
    mpkg_hdr hdr;
    const char *chunk;
    size_t chunk_len;
    char *out = NULL;
    char line[192];

    if (out_len) *out_len = 0;
    if (!json || len == 0) return NULL;

    /* Cheap reject first: the overwhelming majority of maps carry no payload at all, and they
     * must not pay for the structural pass. */
    if (!mpkg_find(json, len, MPKG_HEADER_MAGIC, MPKG_MAGIC_LEN)) return NULL;
    if (!mpkg_structure_build(json, len, &st)) {
        backend_log("MPKG: payload strip SKIPPED -- the map's JSON structure did not read "
                    "cleanly; handing the engine the original buffer");
        return NULL;
    }

    cuts = (mpkg_cut *)HeapAlloc(GetProcessHeap(), 0, SH_MPKG_MAX_SHARDS * sizeof(mpkg_cut));
    if (!cuts) { HeapFree(GetProcessHeap(), 0, st.c); return NULL; }

    while (cut_count < SH_MPKG_MAX_SHARDS &&
           mpkg_next_shard(json, len, &pos, &hdr, &chunk, &chunk_len)) {
        int el;
        if (pkg_id && strcmp(hdr.id, pkg_id) != 0) continue;   /* another package's payload */
        el = mpkg_array_element(&st, mpkg_innermost(&st, pos));
        size_t from, to;
        int dup = 0;
        if (el < 0) continue;
        from = st.c[el].open;
        to   = st.c[el].close + 1;
        for (i = 0; i < cut_count; i++) if (cuts[i].from == from) { dup = 1; break; }
        if (dup) continue;

        cuts[cut_count].from = from;
        cuts[cut_count].to = to;
        cut_count++;
        elements++;
    }

    if (cut_count == 0) {
        HeapFree(GetProcessHeap(), 0, cuts);
        HeapFree(GetProcessHeap(), 0, st.c);
        return NULL;
    }

    qsort(cuts, cut_count, sizeof(mpkg_cut), mpkg_cut_cmp);

    /* Merge runs of ADJACENT elements -- ones separated by nothing but whitespace and a single
     * comma -- into one cut. Shards are consecutive by construction, and asking each element to
     * claim a comma for itself makes neighbours fight over the one between them. */
    {
        size_t w2 = 0;
        for (i = 1; i < cut_count; i++) {
            size_t g = cuts[w2].to;
            while (g < len && mpkg_is_ws(json[g])) g++;
            if (g < len && json[g] == ',') {
                g++;
                while (g < len && mpkg_is_ws(json[g])) g++;
                if (g == cuts[i].from) { cuts[w2].to = cuts[i].to; continue; }
            }
            cuts[++w2] = cuts[i];
        }
        cut_count = w2 + 1;
    }

    /* Now take ONE adjacent comma per run so the array stays valid: the one BEFORE the run, or,
     * when the run starts the array, the one after it. */
    for (i = 0; i < cut_count; i++) {
        size_t b = cuts[i].from;
        while (b > 0 && mpkg_is_ws(json[b - 1])) b--;
        if (b > 0 && json[b - 1] == ',') {
            cuts[i].from = b - 1;
        } else {
            size_t a = cuts[i].to;
            while (a < len && mpkg_is_ws(json[a])) a++;
            if (a < len && json[a] == ',') cuts[i].to = a + 1;
        }
    }
    /* Overlapping cuts would mean two shards resolved to the same element by different spans;
     * that should be impossible, but splicing overlapping ranges silently corrupts, so refuse. */
    for (i = 1; i < cut_count; i++) {
        if (cuts[i].from < cuts[i - 1].to) {
            backend_log("MPKG: payload strip SKIPPED -- shard elements overlap; handing the "
                        "engine the original buffer");
            HeapFree(GetProcessHeap(), 0, cuts);
            HeapFree(GetProcessHeap(), 0, st.c);
            return NULL;
        }
    }

    out = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (!out) {
        HeapFree(GetProcessHeap(), 0, cuts);
        HeapFree(GetProcessHeap(), 0, st.c);
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

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "MPKG: payload STRIPPED (%s) -- %u shard variable(s) removed in %u contiguous "
                "run(s), %zu -> %zu bytes; the delivery envelope never becomes map state",
                pkg_id ? pkg_id : "every package", (unsigned)elements, (unsigned)cut_count,
                len, w);
    backend_log(line);

    HeapFree(GetProcessHeap(), 0, cuts);
    HeapFree(GetProcessHeap(), 0, st.c);
    if (out_len) *out_len = w;
    return out;
}

char *sh_mpkg_strip(const char *json, size_t len, size_t *out_len)
{
    return mpkg_strip_scoped(json, len, NULL, out_len);
}

/* ==================================================================== */
/* embed                                                                 */
/* ==================================================================== */

static const char MPKG_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Standard base64 with padding, into a caller-supplied buffer of at least
 * ((len + 2) / 3) * 4 + 1 bytes. */
static size_t mpkg_b64_encode(const unsigned char *p, size_t len, char *out)
{
    size_t i = 0, w = 0;
    while (i + 3 <= len) {
        unsigned v = ((unsigned)p[i] << 16) | ((unsigned)p[i + 1] << 8) | p[i + 2];
        out[w++] = MPKG_B64[(v >> 18) & 63];
        out[w++] = MPKG_B64[(v >> 12) & 63];
        out[w++] = MPKG_B64[(v >> 6) & 63];
        out[w++] = MPKG_B64[v & 63];
        i += 3;
    }
    if (len - i == 1) {
        unsigned v = (unsigned)p[i] << 16;
        out[w++] = MPKG_B64[(v >> 18) & 63];
        out[w++] = MPKG_B64[(v >> 12) & 63];
        out[w++] = '=';
        out[w++] = '=';
    } else if (len - i == 2) {
        unsigned v = ((unsigned)p[i] << 16) | ((unsigned)p[i + 1] << 8);
        out[w++] = MPKG_B64[(v >> 18) & 63];
        out[w++] = MPKG_B64[(v >> 12) & 63];
        out[w++] = MPKG_B64[(v >> 6) & 63];
        out[w++] = '=';
    }
    out[w] = '\0';
    return w;
}

void sh_mpkg_digest16(const unsigned char *payload, size_t len,
                      char out[SH_MPKG_DIGEST_CHARS + 1])
{
    mpkg_sha256 h;
    sha256_init(&h);
    sha256_update(&h, payload, len);
    sha256_hex16(&h, out);
}

/* Locate the container that is the value of member `key` directly inside container `parent`.
 * Returns its index, or -1. "Directly inside" matters: a map has more than one member called
 * "string", and only the one whose parent is the variables object is the bucket we mean. */
static int mpkg_member_container(const char *json, size_t len, const mpkg_structure *st,
                                 int parent, const char *key)
{
    size_t klen = strlen(key);
    size_t at = st->c[parent].open;
    size_t stop = st->c[parent].close;

    while (at < stop) {
        const char *q = mpkg_find(json + at, stop - at, key, klen);
        size_t koff, v;
        size_t i;
        int found = -1;
        if (!q) return -1;
        koff = (size_t)(q - json);
        at = koff + 1;
        if (koff == 0 || json[koff - 1] != '"') continue;
        if (koff + klen >= len || json[koff + klen] != '"') continue;
        v = koff + klen + 1;
        while (v < stop && mpkg_is_ws(json[v])) v++;
        if (v >= stop || json[v] != ':') continue;
        v++;
        while (v < stop && mpkg_is_ws(json[v])) v++;
        if (v >= stop || (json[v] != '{' && json[v] != '[')) continue;
        for (i = 0; i < st->count; i++) {
            if (st->c[i].open == v) { found = (int)i; break; }
        }
        if (found < 0) continue;
        /* the key itself must be a DIRECT child of `parent`, not of something nested in it */
        if (mpkg_innermost(st, koff) != parent) continue;
        return found;
    }
    return -1;
}

/* Byte span of the `index`-th element of a flat array container (numbers only -- allocCount).
 * Returns 1 with *from/*to, 0 if the array is shorter than that or holds anything nested. */
static int mpkg_flat_element(const char *json, const mpkg_structure *st, int arr,
                             unsigned index, size_t *from, size_t *to)
{
    size_t p = st->c[arr].open + 1;
    size_t stop = st->c[arr].close;
    unsigned n = 0;
    while (p < stop) {
        size_t start;
        while (p < stop && mpkg_is_ws(json[p])) p++;
        start = p;
        while (p < stop && json[p] != ',') {
            if (json[p] == '{' || json[p] == '[' || json[p] == '"') return 0;
            p++;
        }
        if (n == index) {
            size_t e = p;
            while (e > start && mpkg_is_ws(json[e - 1])) e--;
            *from = start;
            *to = e;
            return 1;
        }
        n++;
        p++;   /* past the comma */
    }
    return 0;
}

/* Count the direct elements of an array container. */
static unsigned mpkg_array_count(const char *json, const mpkg_structure *st, int arr)
{
    size_t p = st->c[arr].open + 1, stop = st->c[arr].close;
    unsigned n = 0;
    int depth = 0, in_string = 0, any = 0;
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
        if (!mpkg_is_ws(ch)) any = 1;
    }
    return any ? n + 1 : 0;
}

/* One shard variable, exactly the engine-emitted snapVarString_t the reference implementation
 * copies from a corpus map. Written compact; the engine's own writer is compact too. */
static size_t mpkg_write_shard(char *out, const char *header, const char *chunk, size_t chunk_len)
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

char *sh_mpkg_embed(const char *json, size_t len, const char *pkg_id,
                    const unsigned char *payload, size_t payload_len,
                    size_t *out_len, char *err, size_t err_cap)
{
    mpkg_structure st;
    char *base = NULL;          /* the payload-free buffer we build on top of */
    const char *src;
    size_t src_len;
    char *b64 = NULL, *out = NULL;
    size_t b64_len, shards, i, insert, w = 0, need;
    int vars, bucket, alloc;
    unsigned existing;
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    char line[224];

    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = '\0';
    if (!json || len == 0 || !pkg_id || !payload) {
        mpkg_err(err, err_cap, "embed called with nothing to embed");
        return NULL;
    }
    if (payload_len == 0 || payload_len > SH_MPKG_MAX_PAYLOAD) {
        mpkg_err(err, err_cap, "payload is %zu bytes, over the %u-byte embed budget",
                 payload_len, (unsigned)SH_MPKG_MAX_PAYLOAD);
        return NULL;
    }

    /* Re-embedding replaces rather than accumulates: strip whatever is already there first. A
     * map saved ten times must not carry ten copies of its package. */
    base = mpkg_strip_scoped(json, len, pkg_id, &src_len);
    src = base ? base : json;
    if (!base) src_len = len;

    if (!mpkg_structure_build(src, src_len, &st)) {
        mpkg_err(err, err_cap, "map JSON did not read cleanly; nothing embedded");
        goto fail;
    }
    vars = mpkg_member_container(src, src_len, &st, 0, "variables");
    if (vars < 0 || st.c[vars].kind != '{') {
        mpkg_err(err, err_cap, "map has no variables block");
        goto fail_struct;
    }
    bucket = mpkg_member_container(src, src_len, &st, vars, "string");
    if (bucket < 0 || st.c[bucket].kind != '[') {
        mpkg_err(err, err_cap, "map has no variables.string list");
        goto fail_struct;
    }
    alloc = mpkg_member_container(src, src_len, &st, vars, "allocCount");
    if (alloc < 0 || st.c[alloc].kind != '[') {
        mpkg_err(err, err_cap, "map has no variables.allocCount list");
        goto fail_struct;
    }

    b64_len = ((payload_len + 2) / 3) * 4;
    b64 = (char *)HeapAlloc(GetProcessHeap(), 0, b64_len + 1);
    if (!b64) { mpkg_err(err, err_cap, "out of memory encoding the payload"); goto fail_struct; }
    b64_len = mpkg_b64_encode(payload, payload_len, b64);

    shards = (b64_len + SH_MPKG_SHARD_CHARS - 1) / SH_MPKG_SHARD_CHARS;
    if (shards == 0) shards = 1;
    if (shards > SH_MPKG_MAX_SHARDS) {
        mpkg_err(err, err_cap, "payload needs %zu shards, over the %u cap",
                 shards, (unsigned)SH_MPKG_MAX_SHARDS);
        goto fail_b64;
    }
    sh_mpkg_digest16(payload, payload_len, digest);

    existing = mpkg_array_count(src, &st, bucket);

    /* Worst case: everything before the insert point, every shard with its wrapper and comma,
     * everything after, and room for allocCount growing by a few digits. */
    need = src_len + b64_len + shards * 256 + 64;
    out = (char *)HeapAlloc(GetProcessHeap(), 0, need + 1);
    if (!out) { mpkg_err(err, err_cap, "out of memory building the map"); goto fail_b64; }

    insert = st.c[bucket].close;       /* just before the ']' */
    memcpy(out, src, insert);
    w = insert;
    for (i = 0; i < shards; i++) {
        char header[SH_MPKG_HEADER_CAP];
        const char *chunk = b64 + i * SH_MPKG_SHARD_CHARS;
        size_t chunk_len = b64_len - i * SH_MPKG_SHARD_CHARS;
        if (chunk_len > SH_MPKG_SHARD_CHARS) chunk_len = SH_MPKG_SHARD_CHARS;
        _snprintf_s(header, sizeof header, _TRUNCATE, "%s%s.%u.%u.%s",
                    MPKG_HEADER_MAGIC, pkg_id, (unsigned)i, (unsigned)shards, digest);
        if (existing || i) out[w++] = ',';
        w += mpkg_write_shard(out + w, header, chunk, chunk_len);
    }
    memcpy(out + w, src + insert, src_len - insert);
    w += src_len - insert;
    out[w] = '\0';

    /* allocCount[STRING] must equal the list length, and the list just grew. The slot is edited
     * on the FINISHED buffer, because its offset moved with the insert. */
    {
        mpkg_structure st2;
        int vars2, alloc2;
        size_t from, to;
        if (!mpkg_structure_build(out, w, &st2)) {
            mpkg_err(err, err_cap, "the embedded map did not read back cleanly");
            goto fail_out;
        }
        vars2 = mpkg_member_container(out, w, &st2, 0, "variables");
        alloc2 = vars2 >= 0 ? mpkg_member_container(out, w, &st2, vars2, "allocCount") : -1;
        if (alloc2 < 0 || !mpkg_flat_element(out, &st2, alloc2, 4, &from, &to)) {
            HeapFree(GetProcessHeap(), 0, st2.c);
            mpkg_err(err, err_cap, "map has no variables.allocCount[4] slot to update");
            goto fail_out;
        }
        {
            char count[16];
            int n = _snprintf_s(count, sizeof count, _TRUNCATE, "%u",
                                (unsigned)(existing + shards));
            size_t tail = w - to;
            char *fin = (char *)HeapAlloc(GetProcessHeap(), 0, from + (size_t)n + tail + 1);
            if (!fin) {
                HeapFree(GetProcessHeap(), 0, st2.c);
                mpkg_err(err, err_cap, "out of memory writing the variable count");
                goto fail_out;
            }
            memcpy(fin, out, from);
            memcpy(fin + from, count, (size_t)n);
            memcpy(fin + from + n, out + to, tail);
            w = from + (size_t)n + tail;
            fin[w] = '\0';
            HeapFree(GetProcessHeap(), 0, out);
            out = fin;
        }
        HeapFree(GetProcessHeap(), 0, st2.c);
    }

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "MPKG: package '%s' EMBEDDED into the map -- %zu payload bytes as %zu shard(s), "
                "digest %s; map %zu -> %zu bytes",
                pkg_id, payload_len, shards, digest, len, w);
    backend_log(line);

    HeapFree(GetProcessHeap(), 0, b64);
    HeapFree(GetProcessHeap(), 0, st.c);
    if (base) HeapFree(GetProcessHeap(), 0, base);
    if (out_len) *out_len = w;
    return out;

fail_out:
    HeapFree(GetProcessHeap(), 0, out);
fail_b64:
    HeapFree(GetProcessHeap(), 0, b64);
fail_struct:
    HeapFree(GetProcessHeap(), 0, st.c);
fail:
    if (base) HeapFree(GetProcessHeap(), 0, base);
    return NULL;
}

/* ==================================================================== */
/* scan                                                                  */
/* ==================================================================== */

typedef struct mpkg_seen {
    unsigned char bits[SH_MPKG_MAX_SHARDS / 8];
} mpkg_seen;

static int mpkg_seen_test_set(mpkg_seen *s, unsigned idx)
{
    unsigned char m = (unsigned char)(1u << (idx & 7));
    if (s->bits[idx >> 3] & m) return 1;
    s->bits[idx >> 3] |= m;
    return 0;
}

static size_t mpkg_scan_internal(const char *json, size_t len,
                                 sh_mpkg_decl *out, size_t cap, int *overflow)
{
    mpkg_seen seen[SH_MPKG_MAX_PACKAGES];
    size_t count = 0, pos = 0, i;
    mpkg_hdr hdr;
    const char *chunk;
    size_t chunk_len;

    if (overflow) *overflow = 0;
    if (!json || !out || cap == 0) { if (overflow && json) *overflow = 1; return 0; }
    if (cap > SH_MPKG_MAX_PACKAGES) cap = SH_MPKG_MAX_PACKAGES;
    memset(seen, 0, sizeof seen);

    while (mpkg_next_shard(json, len, &pos, &hdr, &chunk, &chunk_len)) {
        sh_mpkg_decl *d = NULL;
        for (i = 0; i < count; i++)
            if (strcmp(out[i].id, hdr.id) == 0) { d = &out[i]; break; }
        if (!d) {
            if (count >= cap) { if (overflow) *overflow = 1; continue; }
            d = &out[count++];
            memset(d, 0, sizeof *d);
            strcpy_s(d->id, sizeof d->id, hdr.id);
            strcpy_s(d->digest, sizeof d->digest, hdr.digest);
            d->total = hdr.total;
            d->consistent = 1;
        }
        if (d->total != hdr.total || strcmp(d->digest, hdr.digest) != 0) {
            d->consistent = 0;   /* two versions of one package in one map */
            continue;
        }
        if (hdr.idx >= d->total) { d->consistent = 0; continue; }
        if (chunk == NULL) { d->consistent = 0; continue; }
        if (mpkg_seen_test_set(&seen[d - out], hdr.idx)) { d->consistent = 0; continue; }
        d->present++;
    }
    for (i = 0; i < count; i++)
        out[i].complete = out[i].consistent && out[i].present == out[i].total;
    return count;
}

size_t sh_mpkg_scan(const char *json, size_t len, sh_mpkg_decl *out, size_t cap)
{
    return mpkg_scan_internal(json, len, out, cap, NULL);
}

/* ==================================================================== */
/* extract                                                               */
/* ==================================================================== */

typedef struct mpkg_chunk_ref { const char *p; size_t n; unsigned filled; } mpkg_chunk_ref;

static unsigned char *mpkg_b64_decode(const mpkg_chunk_ref *chunks, unsigned total,
                                      size_t *out_len, char *err, size_t err_cap,
                                      const char *pkg_id)
{
    static signed char table[256];
    static volatile LONG table_ready = 0;
    size_t total_chars = 0, cap, produced = 0;
    unsigned char *out;
    uint32_t acc = 0;
    int acc_n = 0, pad = 0;
    unsigned t;
    size_t j;

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
    if (total_chars % 4 != 0) {
        mpkg_err(err, err_cap, "package '%s' has invalid base64 in its shards", pkg_id);
        return NULL;
    }
    cap = total_chars / 4 * 3;
    if (cap > SH_MPKG_MAX_PAYLOAD + 2) {
        mpkg_err(err, err_cap, "package '%s' payload exceeds the %u-byte budget",
                 pkg_id, (unsigned)SH_MPKG_MAX_PAYLOAD);
        return NULL;
    }
    out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, cap ? cap : 1);
    if (!out) { mpkg_err(err, err_cap, "out of memory"); return NULL; }

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
    if (produced > SH_MPKG_MAX_PAYLOAD) {
        mpkg_err(err, err_cap, "package '%s' payload exceeds the %u-byte budget",
                 pkg_id, (unsigned)SH_MPKG_MAX_PAYLOAD);
        HeapFree(GetProcessHeap(), 0, out);
        return NULL;
    }
    *out_len = produced;
    return out;
bad:
    mpkg_err(err, err_cap, "package '%s' has invalid base64 in its shards", pkg_id);
    HeapFree(GetProcessHeap(), 0, out);
    return NULL;
}

unsigned char *sh_mpkg_extract(const char *json, size_t len, const char *pkg_id,
                               size_t *out_len, char *err, size_t err_cap)
{
    mpkg_chunk_ref *chunks = NULL;
    mpkg_hdr hdr;
    const char *chunk;
    size_t chunk_len, pos = 0;
    unsigned total = 0, present = 0;
    int have_meta = 0;
    char digest[SH_MPKG_DIGEST_CHARS + 1] = "";
    unsigned char *payload = NULL;
    size_t payload_len = 0;

    mpkg_err(err, err_cap, "");
    if (!json || !pkg_id || !out_len) { mpkg_err(err, err_cap, "bad arguments"); return NULL; }
    *out_len = 0;

    while (mpkg_next_shard(json, len, &pos, &hdr, &chunk, &chunk_len)) {
        if (strcmp(hdr.id, pkg_id) != 0) continue;
        if (!have_meta) {
            total = hdr.total;
            strcpy_s(digest, sizeof digest, hdr.digest);
            have_meta = 1;
            chunks = (mpkg_chunk_ref *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                 (size_t)total * sizeof *chunks);
            if (!chunks) { mpkg_err(err, err_cap, "out of memory"); return NULL; }
        }
        if (hdr.total != total || strcmp(hdr.digest, digest) != 0) {
            mpkg_err(err, err_cap, "package '%s' has inconsistent shard headers -- "
                     "the map carries two different versions", pkg_id);
            goto fail;
        }
        if (hdr.idx >= total) {
            mpkg_err(err, err_cap, "package '%s' has shard index %u out of range (total %u)",
                     pkg_id, hdr.idx, total);
            goto fail;
        }
        if (chunk == NULL) {
            mpkg_err(err, err_cap, "package '%s' has an unreadable shard %u", pkg_id, hdr.idx);
            goto fail;
        }
        if (chunks[hdr.idx].filled) {
            mpkg_err(err, err_cap, "package '%s' has duplicate shard %u", pkg_id, hdr.idx);
            goto fail;
        }
        chunks[hdr.idx].p = chunk;
        chunks[hdr.idx].n = chunk_len;
        chunks[hdr.idx].filled = 1;
        present++;
    }
    if (!have_meta) {
        mpkg_err(err, err_cap, "package '%s' is not present in this map", pkg_id);
        return NULL;
    }
    if (present != total) {
        mpkg_err(err, err_cap, "package '%s' is incomplete: %u of %u shards",
                 pkg_id, present, total);
        goto fail;
    }
    payload = mpkg_b64_decode(chunks, total, &payload_len, err, err_cap, pkg_id);
    if (!payload) goto fail;

    {
        mpkg_sha256 s;
        char got[SH_MPKG_DIGEST_CHARS + 1];
        sha256_init(&s);
        sha256_update(&s, payload, payload_len);
        sha256_hex16(&s, got);
        if (strcmp(got, digest) != 0) {
            mpkg_err(err, err_cap, "package '%s' failed its digest: header says %s, payload is %s",
                     pkg_id, digest, got);
            HeapFree(GetProcessHeap(), 0, payload);
            goto fail;
        }
    }
    HeapFree(GetProcessHeap(), 0, chunks);
    *out_len = payload_len;
    return payload;
fail:
    if (chunks) HeapFree(GetProcessHeap(), 0, chunks);
    return NULL;
}

/* ==================================================================== */
/* zip unpack (deflate via the shared raw_deflate.c; no second inflate)  */
/* ==================================================================== */

#define MPKG_ZIP_EOCD_SIG   0x06054b50u
#define MPKG_ZIP_CEN_SIG    0x02014b50u
#define MPKG_ZIP_LOC_SIG    0x04034b50u
#define MPKG_ZIP_MAX_ENTRIES     4096u
#define MPKG_ZIP_MAX_FILE_BYTES  (64u * 1024u * 1024u)
#define MPKG_ZIP_MAX_TOTAL_BYTES (256u * 1024u * 1024u)

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

typedef struct mpkg_zentry {
    const char *name;        /* NOT NUL-terminated */
    unsigned    name_len;
    unsigned    method, csize, usize;
    const unsigned char *data;   /* compressed bytes inside the payload */
    int         is_dir;
} mpkg_zentry;

/* Locate the central directory. Returns 1 + *cd/*count, 0 on a malformed zip. */
static int mpkg_zip_open(const unsigned char *payload, size_t len,
                         const unsigned char **cd, unsigned *count,
                         char *err, size_t err_cap)
{
    size_t i, floor_ = 0;
    if (len < 22) { mpkg_err(err, err_cap, "payload too small to be a zip"); return 0; }
    if (len > 22 + 65535) floor_ = len - 22 - 65535;
    for (i = len - 22; ; i--) {
        if (rd32(payload + i) == MPKG_ZIP_EOCD_SIG) {
            unsigned n = rd16(payload + i + 10);
            uint32_t cd_size = rd32(payload + i + 12);
            uint32_t cd_off  = rd32(payload + i + 16);
            if (n > MPKG_ZIP_MAX_ENTRIES) { mpkg_err(err, err_cap, "zip has too many entries (%u)", n); return 0; }
            if (cd_off > i || cd_size > i - cd_off) { mpkg_err(err, err_cap, "zip central directory out of bounds"); return 0; }
            *cd = payload + cd_off;
            *count = n;
            return 1;
        }
        if (i == floor_) break;
    }
    mpkg_err(err, err_cap, "zip end-of-central-directory not found");
    return 0;
}

/* Read one central-directory entry at *cursor and resolve its data span
 * through the local header. Advances *cursor. */
static int mpkg_zip_entry(const unsigned char *payload, size_t len,
                          const unsigned char **cursor, mpkg_zentry *e,
                          char *err, size_t err_cap)
{
    const unsigned char *c = *cursor;
    uint32_t loc_off;
    unsigned nlen, xlen, clen;
    if ((size_t)(c - payload) + 46 > len || rd32(c) != MPKG_ZIP_CEN_SIG) {
        mpkg_err(err, err_cap, "zip central directory entry malformed");
        return 0;
    }
    e->method = rd16(c + 10);
    e->csize  = rd32(c + 20);
    e->usize  = rd32(c + 24);
    nlen = rd16(c + 28); xlen = rd16(c + 30); clen = rd16(c + 32);
    loc_off = rd32(c + 42);
    if ((size_t)(c - payload) + 46 + nlen + xlen + clen > len) {
        mpkg_err(err, err_cap, "zip central directory entry out of bounds");
        return 0;
    }
    e->name = (const char *)(c + 46);
    e->name_len = nlen;
    e->is_dir = nlen > 0 && e->name[nlen - 1] == '/';
    if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu) {
        mpkg_err(err, err_cap, "zip64 archives are not supported");
        return 0;
    }
    /* resolve the data span through the local header (its own name/extra
     * lengths differ from the central copy in general). */
    if ((size_t)loc_off + 30 > len || rd32(payload + loc_off) != MPKG_ZIP_LOC_SIG) {
        mpkg_err(err, err_cap, "zip local header out of bounds");
        return 0;
    }
    {
        unsigned lnlen = rd16(payload + loc_off + 26);
        unsigned lxlen = rd16(payload + loc_off + 28);
        size_t data_off = (size_t)loc_off + 30 + lnlen + lxlen;
        if (data_off > len || (size_t)e->csize > len - data_off) {
            mpkg_err(err, err_cap, "zip member data out of bounds");
            return 0;
        }
        e->data = payload + data_off;
    }
    *cursor = c + 46 + nlen + xlen + clen;
    return 1;
}

/* The untrusted-input rule: relative, forward-slashed, no '.'/'..'
 * segments, no drive letters, no control chars. A map is a stranger's file. */
static int mpkg_member_path_safe(const char *name, unsigned n)
{
    unsigned i, seg_start = 0;
    if (n == 0 || n >= MAX_PATH) return 0;
    if (name[0] == '/') return 0;
    for (i = 0; i <= n; i++) {
        char c = (i < n) ? name[i] : '/';   /* virtual terminator closes the last segment */
        if (i < n && (c == '\\' || c == ':' || (unsigned char)c < 0x20)) return 0;
        if (c == '/') {
            unsigned seg_len = i - seg_start;
            if (seg_len == 0) return 0;                                  /* "//" or leading '/' */
            if (seg_len == 1 && name[seg_start] == '.') return 0;
            if (seg_len == 2 && name[seg_start] == '.' && name[seg_start + 1] == '.') return 0;
            seg_start = i + 1;
        }
    }
    return 1;
}

/* Create every directory of `rel` (forward-slashed, possibly ending in the
 * file name which is NOT created) under `base`. */
static int mpkg_make_parents(const char *base, const char *rel, int whole_is_dir)
{
    char path[MAX_PATH];
    size_t base_len, i;
    if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", base, rel) < 0) return 0;
    base_len = strlen(base) + 1;
    for (i = base_len; path[i]; i++) {
        if (path[i] != '/') continue;
        path[i] = '\0';
        if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
        path[i] = '\\';
    }
    if (whole_is_dir) {
        if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    }
    return 1;
}

static int mpkg_write_file(const char *base, const char *rel,
                           const unsigned char *bytes, size_t n)
{
    char path[MAX_PATH];
    size_t i;
    HANDLE h;
    size_t total = 0;
    if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", base, rel) < 0) return 0;
    for (i = strlen(base) + 1; path[i]; i++)
        if (path[i] == '/') path[i] = '\\';
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    while (total < n) {
        DWORD chunk = (DWORD)((n - total) > 0x10000000 ? 0x10000000 : (n - total));
        DWORD wr = 0;
        if (!WriteFile(h, bytes + total, chunk, &wr, NULL) || wr == 0) break;
        total += wr;
    }
    CloseHandle(h);
    return total == n;
}

/* Count file members + verify the payload IS a package (a top-level
 * package.json) without writing anything. */
static int mpkg_zip_survey(const unsigned char *payload, size_t len,
                           unsigned *files_out, char *err, size_t err_cap)
{
    const unsigned char *cd, *cursor;
    unsigned count, i, files = 0;
    int has_marker = 0;
    unsigned long long total_bytes = 0;
    mpkg_zentry e;
    if (!mpkg_zip_open(payload, len, &cd, &count, err, err_cap)) return 0;
    cursor = cd;
    for (i = 0; i < count; i++) {
        if (!mpkg_zip_entry(payload, len, &cursor, &e, err, err_cap)) return 0;
        if (e.is_dir) {
            /* A directory entry is vetted here too, so nothing is written to
             * disk before EVERY member path has passed. */
            if (e.name_len > 1 && !mpkg_member_path_safe(e.name, e.name_len - 1)) {
                mpkg_err(err, err_cap, "unsafe member path in package: '%.*s'",
                         (int)(e.name_len > 200 ? 200 : e.name_len), e.name);
                return 0;
            }
            continue;
        }
        if (!mpkg_member_path_safe(e.name, e.name_len)) {
            mpkg_err(err, err_cap, "unsafe member path in package: '%.*s'",
                     (int)(e.name_len > 200 ? 200 : e.name_len), e.name);
            return 0;
        }
        if (e.method != 0 && e.method != 8) {
            mpkg_err(err, err_cap, "unsupported zip method %u for '%.*s'",
                     e.method, (int)e.name_len, e.name);
            return 0;
        }
        if (e.usize > MPKG_ZIP_MAX_FILE_BYTES) {
            mpkg_err(err, err_cap, "zip member '%.*s' too large", (int)e.name_len, e.name);
            return 0;
        }
        total_bytes += e.usize;
        if (total_bytes > MPKG_ZIP_MAX_TOTAL_BYTES) {
            mpkg_err(err, err_cap, "zip expands past the %u-byte cap", MPKG_ZIP_MAX_TOTAL_BYTES);
            return 0;
        }
        if (e.name_len == 12 && memcmp(e.name, "package.json", 12) == 0) has_marker = 1;
        files++;
    }
    if (!has_marker) {
        mpkg_err(err, err_cap, "payload is not a package (no top-level package.json)");
        return 0;
    }
    *files_out = files;
    return 1;
}

int sh_mpkg_unpack(const unsigned char *payload, size_t len, const char *dest_dir,
                   unsigned *files_out, char *err, size_t err_cap)
{
    const unsigned char *cd, *cursor;
    unsigned count, i, files = 0;
    mpkg_zentry e;
    char rel[MAX_PATH];

    mpkg_err(err, err_cap, "");
    if (files_out) *files_out = 0;
    if (!payload || !dest_dir || !dest_dir[0]) { mpkg_err(err, err_cap, "bad arguments"); return 0; }

    /* The whole archive is vetted BEFORE the first byte is written, so an
     * unsafe path refuses the install with nothing on disk. */
    if (!mpkg_zip_survey(payload, len, &files, err, err_cap)) return 0;

    if (!CreateDirectoryA(dest_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        mpkg_err(err, err_cap, "cannot create '%s' (err %lu)", dest_dir, GetLastError());
        return 0;
    }
    if (!mpkg_zip_open(payload, len, &cd, &count, err, err_cap)) return 0;
    cursor = cd;
    for (i = 0; i < count; i++) {
        if (!mpkg_zip_entry(payload, len, &cursor, &e, err, err_cap)) return 0;
        if (e.name_len >= sizeof rel) { mpkg_err(err, err_cap, "member path too long"); return 0; }
        memcpy(rel, e.name, e.name_len);
        rel[e.name_len] = '\0';
        if (e.is_dir) {
            rel[e.name_len - 1] = '\0';   /* drop the trailing '/' */
            if (rel[0] && !mpkg_member_path_safe(rel, e.name_len - 1)) {
                mpkg_err(err, err_cap, "unsafe member path in package: '%s'", rel);
                return 0;
            }
            if (rel[0] && !mpkg_make_parents(dest_dir, rel, 1)) {
                mpkg_err(err, err_cap, "cannot create directory '%s'", rel);
                return 0;
            }
            continue;
        }
        if (!mpkg_make_parents(dest_dir, rel, 0)) {
            mpkg_err(err, err_cap, "cannot create parents for '%s'", rel);
            return 0;
        }
        if (e.method == 0) {
            if (e.csize != e.usize) { mpkg_err(err, err_cap, "stored member size mismatch"); return 0; }
            if (!mpkg_write_file(dest_dir, rel, e.data, e.usize)) {
                mpkg_err(err, err_cap, "cannot write '%s'", rel);
                return 0;
            }
        } else {   /* method 8: raw deflate through the shared decoder */
            unsigned char *buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                                            e.usize ? e.usize : 1);
            if (!buf) { mpkg_err(err, err_cap, "out of memory"); return 0; }
            /* usize==0: write an empty file without decoding (a deflated empty
             * member still carries a 2-byte stream; its content is moot). */
            if (e.usize != 0 &&
                sh_inflate_raw(e.data, e.csize, buf, e.usize) != e.usize) {
                HeapFree(GetProcessHeap(), 0, buf);
                mpkg_err(err, err_cap, "deflate stream for '%s' is malformed", rel);
                return 0;
            }
            if (!mpkg_write_file(dest_dir, rel, buf, e.usize)) {
                HeapFree(GetProcessHeap(), 0, buf);
                mpkg_err(err, err_cap, "cannot write '%s'", rel);
                return 0;
            }
            HeapFree(GetProcessHeap(), 0, buf);
        }
    }
    if (files_out) *files_out = files;
    return 1;
}

/* ==================================================================== */
/* the boot snapshot + session state                                     */
/* ==================================================================== */

#define MPKG_SIDECAR_NAME "smpkg.digest"
#define MPKG_SESSION_MAX  32

typedef struct mpkg_boot_pkg {
    char folded[SH_PACKAGE_NAME_CAP];   /* lowercased, '/'->'-' */
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    int  has_digest;
} mpkg_boot_pkg;

typedef struct mpkg_session_entry {
    char id[SH_MPKG_ID_CAP];
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    int  outcome;   /* 1 installed, 0 declined, 2 prompt in flight */
} mpkg_session_entry;

static CRITICAL_SECTION g_mpkg_lock;
static INIT_ONCE        g_mpkg_lock_once = INIT_ONCE_STATIC_INIT;

static int           g_boot_captured = 0;
static char          g_data_root[MAX_PATH] = {0};
static mpkg_boot_pkg g_boot[SH_PACKAGES_MAX];
static size_t        g_boot_count = 0;

static mpkg_session_entry g_session[MPKG_SESSION_MAX];
static size_t             g_session_count = 0;

static int g_consent_mode = -1;   /* SH_MPKG_CONSENT_PROMPT */
static char g_last_refusal[SH_MPKG_ERR_CAP] = "";

static BOOL CALLBACK mpkg_lock_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_mpkg_lock);
    return TRUE;
}

static void mpkg_lock(void)   { InitOnceExecuteOnce(&g_mpkg_lock_once, mpkg_lock_init, NULL, NULL); EnterCriticalSection(&g_mpkg_lock); }
static void mpkg_unlock(void) { LeaveCriticalSection(&g_mpkg_lock); }

static void mpkg_fold_name(const char *name, char *out, size_t cap)
{
    size_t i;
    for (i = 0; name[i] && i < cap - 1; i++) {
        char c = name[i];
        if (c == '/' || c == '\\') c = '-';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[i] = '\0';
}

static int mpkg_read_sidecar(const char *pkg_root, char *digest_out)
{
    char path[MAX_PATH];
    char buf[SH_MPKG_DIGEST_CHARS + 1];
    HANDLE h;
    DWORD rd = 0;
    size_t i;
    if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", pkg_root, MPKG_SIDECAR_NAME) < 0)
        return 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, SH_MPKG_DIGEST_CHARS, &rd, NULL) || rd != SH_MPKG_DIGEST_CHARS) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    for (i = 0; i < SH_MPKG_DIGEST_CHARS; i++)
        if (!mpkg_is_hex(buf[i])) return 0;
    memcpy(digest_out, buf, SH_MPKG_DIGEST_CHARS);
    digest_out[SH_MPKG_DIGEST_CHARS] = '\0';
    return 1;
}

void sh_mpkg_boot_capture(const char *data_root)
{
    sh_package pkgs[SH_PACKAGES_MAX];
    size_t count = 0, i;
    char line[256];

    if (!data_root || !data_root[0]) return;
    mpkg_lock();
    if (g_boot_captured) { mpkg_unlock(); return; }   /* first capture wins: it is the BOOT state */
    strncpy_s(g_data_root, sizeof g_data_root, data_root, _TRUNCATE);
    /* An unreadable tree yields a partial list; missing packages then refuse
     * loads (never crash them), so a partial capture is safe to keep. */
    sh_packages_enumerate(data_root, pkgs, SH_PACKAGES_MAX, &count);
    g_boot_count = 0;
    for (i = 0; i < count && g_boot_count < SH_PACKAGES_MAX; i++) {
        mpkg_boot_pkg *b = &g_boot[g_boot_count++];
        mpkg_fold_name(pkgs[i].name, b->folded, sizeof b->folded);
        b->has_digest = mpkg_read_sidecar(pkgs[i].root, b->digest);
        if (!b->has_digest) b->digest[0] = '\0';
    }
    g_boot_captured = 1;
    mpkg_unlock();
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MPKG: boot snapshot captured -- %zu package(s) under %s\\overrides",
        g_boot_count, data_root);
    backend_log(line);
}

/* Name equivalence between a declared id and a boot package's folded name:
 * equal, or one ends with "-" + the other (grouping-folder prefixes fold to
 * leading "<group>-"). Generous by design -- see map_package.h. */
static int mpkg_names_match(const char *declared, const char *folded)
{
    size_t dn = strlen(declared), fn = strlen(folded);
    if (dn == 0 || fn == 0) return 0;
    if (strcmp(declared, folded) == 0) return 1;
    if (dn > fn + 1 && declared[dn - fn - 1] == '-' &&
        strcmp(declared + (dn - fn), folded) == 0) return 1;
    if (fn > dn + 1 && folded[fn - dn - 1] == '-' &&
        strcmp(folded + (fn - dn), declared) == 0) return 1;
    return 0;
}

/* Is this declared package satisfied by the BOOT snapshot? (lock held) */
/* Read the digest sidecar an install leaves beside a package. Returns 0 when the
 * package predates the sidecar or it cannot be read -- in which case its content
 * identity is simply unknown, which is different from known-and-different. */
static int mpkg_read_sidecar_digest(const char *package_root, char *out, size_t out_cap)
{
    char path[MAX_PATH];
    HANDLE handle;
    DWORD got = 0;

    if (!package_root || !out || out_cap <= SH_MPKG_DIGEST_CHARS) return 0;
    if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", package_root,
                    MPKG_SIDECAR_NAME) < 0) return 0;
    handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(handle, out, SH_MPKG_DIGEST_CHARS, &got, NULL) ||
        got != SH_MPKG_DIGEST_CHARS) {
        CloseHandle(handle);
        return 0;
    }
    CloseHandle(handle);
    out[SH_MPKG_DIGEST_CHARS] = '\0';
    return 1;
}

static int mpkg_boot_satisfies(const sh_mpkg_decl *d)
{
    size_t i;
    for (i = 0; i < g_boot_count; i++) {
        const mpkg_boot_pkg *b = &g_boot[i];
        if (b->has_digest && strcmp(b->digest, d->digest) == 0) return 1;
        if (mpkg_names_match(d->id, b->folded)) {
            if (b->has_digest && strcmp(b->digest, d->digest) != 0)
                continue;   /* same name, different version: not satisfied */
            return 1;
        }
    }
    return 0;
}

/* (lock held) */
static mpkg_session_entry *mpkg_session_find(const char *id, const char *digest)
{
    size_t i;
    for (i = 0; i < g_session_count; i++)
        if (strcmp(g_session[i].id, id) == 0 && strcmp(g_session[i].digest, digest) == 0)
            return &g_session[i];
    return NULL;
}

/* (lock held) */
static mpkg_session_entry *mpkg_session_add(const char *id, const char *digest, int outcome)
{
    mpkg_session_entry *e;
    if (g_session_count >= MPKG_SESSION_MAX) return NULL;
    e = &g_session[g_session_count++];
    strcpy_s(e->id, sizeof e->id, id);
    strcpy_s(e->digest, sizeof e->digest, digest);
    e->outcome = outcome;
    return e;
}

/* ==================================================================== */
/* install + consent                                                     */
/* ==================================================================== */

typedef struct mpkg_staged {
    char id[SH_MPKG_ID_CAP];
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    unsigned char *payload;
    size_t payload_len;
    unsigned files;
} mpkg_staged;

static void mpkg_staged_free(mpkg_staged *s)
{
    if (!s) return;
    if (s->payload) HeapFree(GetProcessHeap(), 0, s->payload);
    HeapFree(GetProcessHeap(), 0, s);
}

/* Perform the consented install. Returns 1 on success (and records it as
 * installed-this-session), 0 with `err` filled. Never touches an existing
 * folder: the user's overrides tree is not ours to overwrite. */
static int mpkg_install_staged(const mpkg_staged *s, char *err, size_t err_cap)
{
    char dest[MAX_PATH], sidecar[MAX_PATH], line[512];
    char root[MAX_PATH];
    unsigned files = 0;
    DWORD attrs;

    mpkg_lock();
    strncpy_s(root, sizeof root, g_data_root, _TRUNCATE);
    mpkg_unlock();
    if (!root[0]) { mpkg_err(err, err_cap, "no data root captured"); return 0; }
    if (_snprintf_s(dest, sizeof dest, _TRUNCATE, "%s\\overrides\\%s", root, s->id) < 0) {
        mpkg_err(err, err_cap, "destination path too long");
        return 0;
    }
    attrs = GetFileAttributesA(dest);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        /* The folder is taken. Say WHICH case this is, because the two are not
         * the same problem and the old wording ("already exists") described the
         * benign one while silently producing a dead end in the other.
         *
         * Same id, DIFFERENT digest is the real trap: the gate correctly reports
         * the map's package as missing (a same-named package with different
         * content does not satisfy it), and then the install cannot proceed
         * because the name is occupied. The map could never load and nothing
         * explained why. Installing over the top is not the answer either -- that
         * would silently replace content another map may depend on. So this is
         * reported as the version clash it is, and resolving it stays the
         * player's decision. */
        char installed[SH_MPKG_DIGEST_CHARS + 1];
        if (mpkg_read_sidecar_digest(dest, installed, sizeof installed) &&
            strcmp(installed, s->digest) != 0) {
            mpkg_err(err, err_cap,
                     "a DIFFERENT version of '%s' is already installed (has %s, this map needs "
                     "%s). Remove or rename the installed one to use this map's version",
                     s->id, installed, s->digest);
        } else {
            mpkg_err(err, err_cap, "'%s' already exists on disk; not overwriting it", dest);
        }
        return 0;
    }
    if (!sh_mpkg_unpack(s->payload, s->payload_len, dest, &files, err, err_cap))
        return 0;
    /* Record the payload digest beside the package so future gates match by
     * content, not by name. */
    if (_snprintf_s(sidecar, sizeof sidecar, _TRUNCATE, "%s\\%s", dest, MPKG_SIDECAR_NAME) >= 0) {
        HANDLE h = CreateFileA(sidecar, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr;
            WriteFile(h, s->digest, SH_MPKG_DIGEST_CHARS, &wr, NULL);
            CloseHandle(h);
        }
    }
    mpkg_lock();
    {
        mpkg_session_entry *e = mpkg_session_find(s->id, s->digest);
        if (e) e->outcome = 1;
        else mpkg_session_add(s->id, s->digest, 1);
    }
    mpkg_unlock();
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MPKG: package '%s' (digest %s) INSTALLED to %s -- %u file(s); "
        "requesting a runtime re-arm, no restart needed", s->id, s->digest, dest, files);
    backend_log(line);

    /* Registration no longer needs a relaunch: ask the decl server to re-arm. It runs on the
     * engine tick in two phases (the cut-content cvars a package needs are queued, not
     * immediate), so the identities become live a few frames from now rather than next launch. */
    sh_decl_server_request_rearm();
    return 1;
}

static void mpkg_record_decline(const char *id, const char *digest)
{
    char line[256];
    mpkg_lock();
    {
        mpkg_session_entry *e = mpkg_session_find(id, digest);
        if (e) e->outcome = 0;
        else mpkg_session_add(id, digest, 0);
    }
    mpkg_unlock();
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MPKG: user DECLINED install of package '%s' (digest %s); "
        "it will not be asked again this session", id, digest);
    backend_log(line);
}

/* The production consent prompt, on its OWN thread so the engine's main
 * thread (which is inside the refused DeserializeFromJson) is never
 * blocked. The load this rode in on is already refused; whatever the user
 * answers only affects FUTURE loads -- but 'future' now means the next load
 * in THIS session, because installing re-arms the decl server on the tick.
 *
 * This is deliberately a native Win32 prompt rather than the engine's
 * idMenuManager_Dialog: AddDialog's body text resolves in the SWF layer,
 * which is unsolved (campaign W-1/W-7), so an engine dialog today would
 * show a stock message that misleads the user about what they are
 * consenting to. An accurate ugly prompt beats a native-looking wrong one. */
/* ---- consent on the engine tick ---------------------------------------------
 *
 * The engine's own modal is raised and answered on the main thread: AddDialog
 * writes the dialog queue and the answer is read back out of it. A worker thread
 * cannot touch either. So consent runs as a small state machine driven from the
 * engine tick, and the OS message box stays as the fallback for when the engine
 * surface is unavailable -- an older build, a refused signature, or a prompt
 * needed before the engine has raised any dialog of its own.
 *
 * The GDM id is a personality, not a text source: ShowDialog takes our string
 * over whatever the id would have produced. The button set is a free parameter,
 * so a yes/no pair can be attached to any id.
 */
/* WHICH GDM ID TO BORROW, AND WHY IT MATTERS.
 *
 * The id is never seen by the player -- ShowDialog takes our text over whatever
 * the id would have produced -- so it only selects a dialog shape the shell
 * already knows how to draw. But it is NOT inert: these ids name real prompts,
 * and the shell may have an action wired to the affirmative answer.
 *
 * This was GDM_SNAPMAP_DELETE_MAP_PROMPT (0x60), chosen purely because it drew a
 * Yes/No pair. That is a prompt whose "yes" means DELETE THE SELECTED MAP. We
 * raise it from our own code rather than from the delete flow, so the shell has
 * no map staged for deletion and the answer should go nowhere -- but "should"
 * is not a basis for a button a player will actually press, and the cost of
 * being wrong is somebody's map.
 *
 * GDM_CONFIRM_VIDEO_CHANGES is the safer borrow: it is a settings confirmation,
 * its affirmative action applies pending VIDEO settings, and outside the video
 * menu there are none pending -- so an unintended "yes" applies nothing and can
 * destroy nothing. It still needs confirming against a real keypress, which is
 * the same test that settles the answer encoding. */
#define MPKG_CONSENT_GDM_ID      0x29u   /* GDM_CONFIRM_VIDEO_CHANGES */
#define MPKG_CONSENT_BUTTON_SET  6u      /* Yes / No -- identified live; sets 0 and 1 draw a
                                          * single acknowledgement, 2 draws REFRESH/CONTINUE and
                                          * 4 draws RETRY/CONTINUE, none of which is a question */

enum {
    MPKG_CONSENT_IDLE = 0,
    MPKG_CONSENT_RAISE,      /* staged; raise on the next tick */
    MPKG_CONSENT_WAITING     /* on screen; poll for the answer */
};

static volatile LONG  g_consent_state;
static mpkg_staged   *g_consent_staged;      /* owned while not IDLE */
static int            g_consent_ticket;
static volatile LONG  g_consent_waited;      /* ticks spent waiting for the surface */

/* How long to wait for the engine dialog surface before giving up. The menu
 * manager is captured from the first dialog the game raises on its own, which
 * on this build is the stay-offline notice during boot -- long before any map
 * can be loaded. So this bound is only reached if that never happened, and the
 * honest response then is to install nothing and say why. */
#define MPKG_CONSENT_WAIT_TICKS 600

static void mpkg_consent_finish(int accepted)
{
    mpkg_staged *s = g_consent_staged;

    g_consent_staged = NULL;
    g_consent_ticket = 0;
    InterlockedExchange(&g_consent_state, MPKG_CONSENT_IDLE);
    if (!s) return;

    if (accepted) {
        char err[SH_MPKG_ERR_CAP];
        if (!mpkg_install_staged(s, err, sizeof err)) {
            char line[512];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: install of package '%s' FAILED: %s", s->id, err);
            backend_log(line);
            mpkg_lock();
            {
                mpkg_session_entry *e = mpkg_session_find(s->id, s->digest);
                if (e && e->outcome == 2) e->outcome = 0;   /* failed = do not re-prompt */
            }
            mpkg_unlock();
        }
    } else {
        mpkg_record_decline(s->id, s->digest);
    }
    mpkg_staged_free(s);
}

void sh_mpkg_consent_poll(void)
{
    LONG state = InterlockedCompareExchange(&g_consent_state, 0, 0);
    char text[256];
    mpkg_staged *s;

    if (state == MPKG_CONSENT_IDLE) return;
    s = g_consent_staged;
    if (!s) { InterlockedExchange(&g_consent_state, MPKG_CONSENT_IDLE); return; }

    if (state == MPKG_CONSENT_RAISE) {
        if (!sh_engine_dialog_ready()) {
            if (InterlockedIncrement(&g_consent_waited) > MPKG_CONSENT_WAIT_TICKS) {
                backend_log("MPKG: the engine dialog surface never became available; "
                            "nothing was installed and consent was never asked");
                mpkg_consent_finish(0);
            }
            return;
        }
        /* The engine's dialog body is one 256-byte string, so this is written to
         * be read at a glance rather than to carry every field the OS box did.
         * The digest and byte count are in the log for anyone who wants them. */
        _snprintf_s(text, sizeof text, _TRUNCATE,
                    "This map brings its own mod package:  %s  (%u files, %u KB).  "
                    "It has to be installed before the map can load.  Install it now?",
                    s->id, s->files, (unsigned)((s->payload_len + 1023) / 1024));
        g_consent_ticket = sh_engine_dialog_ask(MPKG_CONSENT_GDM_ID,
                                                MPKG_CONSENT_BUTTON_SET, text);
        if (!g_consent_ticket) {
            backend_log("MPKG: the engine dialog would not raise; installing nothing, because "
                        "third-party content is never installed without an answer");
            mpkg_consent_finish(0);
            return;
        }
        InterlockedExchange(&g_consent_state, MPKG_CONSENT_WAITING);
        return;
    }

    switch (sh_engine_dialog_poll(g_consent_ticket)) {
    case SH_ENGINE_DIALOG_PENDING:
        return;
    case SH_ENGINE_DIALOG_ACCEPTED:
        mpkg_consent_finish(1);
        return;
    default:
        mpkg_consent_finish(0);
        return;
    }
}

/* Raise consent for one staged package (takes ownership of `s`).
 *
 * ONE PATH. Consent is asked through the engine's own modal and nowhere else.
 * There is deliberately no OS-message-box fallback: two ways to ask the same
 * question means two behaviours to keep correct, two things for a player to see
 * depending on state they cannot observe, and a silent downgrade whenever the
 * engine path breaks -- which is exactly how a broken engine path would go
 * unnoticed. If the engine surface cannot ask, nothing is installed and the
 * refusal says so. */
static void mpkg_request_consent(mpkg_staged *s)
{
    int mode;

    mpkg_lock();
    mode = g_consent_mode;
    mpkg_unlock();

    if (mode == 1) {          /* test seam: synchronous accept */
        char err[SH_MPKG_ERR_CAP];
        if (!mpkg_install_staged(s, err, sizeof err)) {
            char line[512];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: install of package '%s' FAILED: %s", s->id, err);
            backend_log(line);
            mpkg_lock();
            {
                mpkg_session_entry *e = mpkg_session_find(s->id, s->digest);
                if (e && e->outcome == 2) e->outcome = 0;
            }
            mpkg_unlock();
        }
        mpkg_staged_free(s);
        return;
    }
    if (mode == 0) {          /* test seam: synchronous decline */
        mpkg_record_decline(s->id, s->digest);
        mpkg_staged_free(s);
        return;
    }

    if (InterlockedCompareExchange(&g_consent_state, MPKG_CONSENT_RAISE,
                                   MPKG_CONSENT_IDLE) != MPKG_CONSENT_IDLE) {
        backend_log("MPKG: a consent dialog is already up; this one is declined rather than queued");
        mpkg_record_decline(s->id, s->digest);
        mpkg_staged_free(s);
        return;
    }
    g_consent_staged = s;
    InterlockedExchange(&g_consent_waited, 0);
    backend_log("MPKG: consent will be asked through the engine's own dialog on the next tick");
}

/* ==================================================================== */
/* THE GATE                                                              */
/* ==================================================================== */

static void mpkg_set_refusal(const char *reason)
{
    char line[SH_MPKG_ERR_CAP + 32];
    mpkg_lock();
    strncpy_s(g_last_refusal, sizeof g_last_refusal, reason, _TRUNCATE);
    mpkg_unlock();
    _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: load REFUSED -- %s", reason);
    backend_log(line);
}

int sh_mpkg_gate(const char *json, size_t len)
{
    sh_mpkg_decl decls[SH_MPKG_MAX_PACKAGES];
    size_t count, i;
    int overflow = 0;
    size_t missing_count = 0;
    const sh_mpkg_decl *first_missing = NULL;
    int first_missing_pending_restart = 0;
    char reason[SH_MPKG_ERR_CAP];

    if (!json || len == 0) return 1;

    /* The fast path: one substring sweep, no allocation. */
    if (!mpkg_find(json, len, MPKG_HEADER_MAGIC, MPKG_MAGIC_LEN)) return 1;

    count = mpkg_scan_internal(json, len, decls, SH_MPKG_MAX_PACKAGES, &overflow);
    if (count == 0 && !overflow) return 1;   /* "smpkg." was prose, not a shard header */
    if (overflow) {
        mpkg_set_refusal("map declares more packages than the gate can vet");
        return 0;
    }
    if (!g_boot_captured) {
        /* Defensive: without the boot snapshot "installed" is unknowable, and
         * guessing wrong is a process death. Refuse; a vanilla map is untouched. */
        mpkg_set_refusal("map declares packages but the boot package snapshot is missing");
        return 0;
    }

    mpkg_lock();
    for (i = 0; i < count; i++) {
        const sh_mpkg_decl *d = &decls[i];
        mpkg_session_entry *e;
        if (mpkg_boot_satisfies(d)) continue;
        e = mpkg_session_find(d->id, d->digest);
        missing_count++;
        if (!first_missing) {
            first_missing = d;
            first_missing_pending_restart = (e && e->outcome == 1);
        }
    }
    mpkg_unlock();

    if (missing_count == 0) return 1;   /* everything already installed: silent pass */

    if (first_missing_pending_restart) {
        /* Installed this session. Registration is no longer a relaunch: the decl server re-arms
         * on the engine tick, so the honest question is whether that pass has COMPLETED yet --
         * not whether the process has been restarted. Once it has, the identities are live and
         * this map is loadable in this process. */
        if (sh_decl_server_registration_succeeded()) {
            backend_log("MPKG: package installed and registered at runtime this session; "
                        "allowing the load without a restart");
            return 1;
        }
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
            "package '%s' was installed this session and is still registering; "
            "try again in a moment", first_missing->id);
        mpkg_set_refusal(reason);
        return 0;
    }

    _snprintf_s(reason, sizeof reason, _TRUNCATE,
        "map requires %zu uninstalled package(s); first: '%s' (digest %s, %u/%u shards)",
        missing_count, first_missing->id, first_missing->digest,
        first_missing->present, first_missing->total);
    mpkg_set_refusal(reason);

    /* Offer the install for the first missing package -- unless the user has
     * already answered (or is being asked) for this exact (id, digest). */
    {
        mpkg_session_entry *e;
        int should_offer = 0;
        mpkg_lock();
        e = mpkg_session_find(first_missing->id, first_missing->digest);
        if (!e) {
            if (mpkg_session_add(first_missing->id, first_missing->digest, 2))
                should_offer = 1;   /* marked in-flight */
        }
        mpkg_unlock();
        if (should_offer) {
            char err[SH_MPKG_ERR_CAP];
            size_t payload_len = 0;
            unsigned char *payload =
                sh_mpkg_extract(json, len, first_missing->id, &payload_len, err, sizeof err);
            unsigned files = 0;
            if (payload &&
                !mpkg_zip_survey(payload, payload_len, &files, err, sizeof err)) {
                HeapFree(GetProcessHeap(), 0, payload);
                payload = NULL;
            }
            if (!payload) {
                char line[SH_MPKG_ERR_CAP + 96];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "MPKG: package '%s' cannot be offered for install -- %s",
                    first_missing->id, err);
                backend_log(line);
                mpkg_lock();
                e = mpkg_session_find(first_missing->id, first_missing->digest);
                if (e && e->outcome == 2) e->outcome = 0;   /* nothing installable: don't re-ask */
                mpkg_unlock();
            } else {
                mpkg_staged *s = (mpkg_staged *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                          sizeof *s);
                if (!s) {
                    HeapFree(GetProcessHeap(), 0, payload);
                } else {
                    strcpy_s(s->id, sizeof s->id, first_missing->id);
                    strcpy_s(s->digest, sizeof s->digest, first_missing->digest);
                    s->payload = payload;
                    s->payload_len = payload_len;
                    s->files = files;
                    mpkg_request_consent(s);   /* takes ownership */
                }
            }
        }
    }
    return 0;
}

/* ==================================================================== */
/* test seams                                                            */
/* ==================================================================== */

#ifdef SH_MAP_PACKAGE_TESTING
void sh_mpkg_test_set_consent_mode(int mode)
{
    mpkg_lock();
    g_consent_mode = mode;
    mpkg_unlock();
}

void sh_mpkg_test_reset(void)
{
    mpkg_lock();
    g_boot_captured = 0;
    g_data_root[0] = '\0';
    g_boot_count = 0;
    g_session_count = 0;
    g_last_refusal[0] = '\0';
    g_consent_mode = -1;
    mpkg_unlock();
}

const char *sh_mpkg_test_last_refusal(void)
{
    return g_last_refusal;
}

int sh_mpkg_test_session_installed_count(void)
{
    int n = 0;
    size_t i;
    mpkg_lock();
    for (i = 0; i < g_session_count; i++)
        if (g_session[i].outcome == 1) n++;
    mpkg_unlock();
    return n;
}
#endif
