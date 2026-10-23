/* Exact delta carriers between a map's complete deliveries (map_package_share.h).
 * Planning cuts each archive at its own local records and central directory,
 * matches equal segments by SHA-256 and confirms every match byte for byte.
 * Decoding validates every field before reading a base, then verifies each base
 * and the rebuilt archive against their recorded lengths and SHA-256. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "bcrypt.lib")

#include "map_package_share.h"

#define MS_HEADER   56u  /* magic, version, archive length, SHA-256, base count */
#define MS_BASE     42u  /* key length, archive length, SHA-256; the key follows its length */
#define MS_COPY     21u  /* kind, base, offset, length */
#define MS_LITERAL  9u   /* kind, length; the bytes follow */
enum { MS_OP_COPY = 1, MS_OP_LITERAL = 2 };

#ifdef SH_MAP_PACKAGE_TESTING
static long g_ms_refusal;
static int g_ms_colliding;
void sh_mpkg_share_test_refuse_allocation(long nth) { g_ms_refusal = nth; }
long sh_mpkg_share_test_allocations_left(void) { return g_ms_refusal; }
void sh_mpkg_share_test_colliding_digests(int on) { g_ms_colliding = on; }
#define MS_REFUSED() (g_ms_refusal > 0 && !--g_ms_refusal)
#define MS_COLLIDING() g_ms_colliding
#else
#define MS_REFUSED() 0
#define MS_COLLIDING() 0
#endif
static void *ms_calloc(size_t n, size_t size) { return MS_REFUSED() ? NULL : calloc(n ? n : 1, size ? size : 1); }
static void *ms_heap(size_t n) { return MS_REFUSED() ? NULL : HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }

static int ms_error(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return 0; }

static unsigned ms_r16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned ms_r32(const unsigned char *p) { return ms_r16(p) | (ms_r16(p + 2) << 16); }
static uint64_t ms_r64(const unsigned char *p) { return ms_r32(p) | ((uint64_t)ms_r32(p + 4) << 32); }
static unsigned char *ms_w16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); return p + 2; }
static unsigned char *ms_w32(unsigned char *p, unsigned v) { ms_w16(p, v); ms_w16(p + 2, v >> 16); return p + 4; }
static unsigned char *ms_w64(unsigned char *p, uint64_t v) { ms_w32(p, (unsigned)v); ms_w32(p + 4, (unsigned)(v >> 32)); return p + 8; }

static int ms_sha256(BCRYPT_ALG_HANDLE algorithm, const unsigned char *bytes, size_t length, unsigned char out[32])
{
    BCRYPT_HASH_HANDLE hash = NULL;
    int ok = 0;
    if (BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) return 0;
    while (length) {
        ULONG step = length > 0x40000000u ? 0x40000000u : (ULONG)length;
        if (BCryptHashData(hash, (PUCHAR)bytes, step, 0) < 0) goto done;
        bytes += step; length -= step;
    }
    ok = BCryptFinishHash(hash, out, 32, 0) >= 0;
done:
    BCryptDestroyHash(hash); return ok;
}

int sh_mpkg_delta_is(const unsigned char *payload, size_t length)
{ return payload && length >= 8 && !memcmp(payload, SH_MPKG_DELTA_MAGIC, 8); }

/* ------------------------------------------------------------- segments --- */

/* The central directory's first entry and count, or 0 when the end records
 * cannot be read (no comment; ZIP64 end records included). */
static int ms_directory(const unsigned char *a, size_t n, uint64_t *entries, uint64_t *offset)
{
    const unsigned char *eocd;
    uint64_t size;
    if (n < 22) return 0;
    eocd = a + n - 22;
    if (ms_r32(eocd) != 0x06054b50u || ms_r16(eocd + 4) || ms_r16(eocd + 6) || ms_r16(eocd + 20)) return 0;
    *entries = ms_r16(eocd + 10); size = ms_r32(eocd + 12); *offset = ms_r32(eocd + 16);
    if (n >= 42 && ms_r32(eocd - 20) == 0x07064b50u) {
        uint64_t record = ms_r64(eocd - 12);
        if (record > n - 42 || n - 42 - (size_t)record < 56 || ms_r32(a + (size_t)record) != 0x06064b50u) return 0;
        *entries = ms_r64(a + (size_t)record + 32); size = ms_r64(a + (size_t)record + 40);
        *offset = ms_r64(a + (size_t)record + 48);
    }
    return *entries && *offset < n && size <= n - (size_t)*offset && *entries <= size / 46u;
}

/* Boundaries [0, local records..., central directory, n] of a complete
 * archive's segments, strictly ascending, with *count segments. The record
 * offsets come from its central directory in order. Anything else leaves the
 * archive one segment, which still shares an identical whole archive. */
static size_t *ms_bounds(const unsigned char *a, size_t n, size_t *count)
{
    uint64_t entries = 0, offset = 0;
    size_t *bounds, used = 1, at, previous = 0;
    int readable = ms_directory(a, n, &entries, &offset) && entries <= (SIZE_MAX / sizeof(*bounds)) - 3u;
    if (!(bounds = ms_calloc(readable ? (size_t)entries + 3u : 2u, sizeof(*bounds)))) return NULL;
    bounds[0] = 0; bounds[1] = n; *count = 1;
    if (!readable) return bounds;
    at = (size_t)offset;
    for (uint64_t i = 0; i < entries; i++) {
        size_t name, extra, comment;
        uint64_t local;
        if (n - at < 46 || ms_r32(a + at) != 0x02014b50u) goto single;
        name = ms_r16(a + at + 28); extra = ms_r16(a + at + 30); comment = ms_r16(a + at + 32);
        if (name + extra + comment > n - at - 46) goto single;
        local = ms_r32(a + at + 42);
        if (local == UINT32_MAX) {
            /* ZIP64 extra: each saturated size first, then the offset. */
            const unsigned char *p = a + at + 46 + name, *end = p + extra;
            size_t skip = (ms_r32(a + at + 24) == UINT32_MAX ? 8u : 0u) + (ms_r32(a + at + 20) == UINT32_MAX ? 8u : 0u);
            int found = 0;
            while (!found && end - p >= 4) {
                unsigned tag = ms_r16(p), bytes = ms_r16(p + 2);
                if ((size_t)(end - p) - 4u < bytes) goto single;
                if (tag == 1) { if (bytes < skip + 8u) goto single; local = ms_r64(p + 4 + skip); found = 1; }
                p += 4u + bytes;
            }
            if (!found) goto single;
        }
        if (local >= offset || (i && local <= previous)) goto single;
        previous = (size_t)local;
        if (local) bounds[used++] = (size_t)local;
        at += 46 + name + extra + comment;
    }
    bounds[used++] = (size_t)offset;
    bounds[used] = n;
    *count = used; return bounds;
single:
    bounds[0] = 0; bounds[1] = n; *count = 1; return bounds;
}

/* ----------------------------------------------------------------- plan --- */

typedef struct ms_slot { unsigned char sha[32]; size_t item, offset, length; int used; } ms_slot;
typedef struct ms_index { ms_slot *slots; size_t mask; } ms_index;
typedef struct ms_op { int kind; size_t base, offset, length; } ms_op;
/* One delivery's segments, their digests and its whole archive's digest. */
typedef struct ms_split { size_t *bounds, count; unsigned char (*sha)[32]; unsigned char archive[32]; } ms_split;

static size_t ms_hash(const unsigned char sha[32], size_t length)
{ uint64_t h; memcpy(&h, sha, 8); return (size_t)(h ^ (uint64_t)length * 0x9e3779b97f4a7c15ull); }

static void ms_insert(ms_index *x, const sh_mpkg_share_item *items, const unsigned char sha[32],
    size_t item, size_t offset, size_t length)
{
    size_t at = ms_hash(sha, length) & x->mask;
    while (x->slots[at].used) {
        /* Keep the first equal segment; a later equal one adds nothing. Equal
         * digests alone never make two segments equal. */
        const ms_slot *s = &x->slots[at];
        if (s->length == length && !memcmp(s->sha, sha, 32) &&
            !memcmp(items[s->item].archive + s->offset, items[item].archive + offset, length)) return;
        at = (at + 1) & x->mask;
    }
    memcpy(x->slots[at].sha, sha, 32);
    x->slots[at].item = item; x->slots[at].offset = offset; x->slots[at].length = length; x->slots[at].used = 1;
}

/* An earlier classic segment holding exactly these bytes: equal digests are a
 * lookup only, never proof; the bytes themselves are compared. */
static const ms_slot *ms_find(const ms_index *x, const sh_mpkg_share_item *items,
    const unsigned char sha[32], const unsigned char *bytes, size_t length)
{
    size_t at = ms_hash(sha, length) & x->mask;
    while (x->slots[at].used) {
        const ms_slot *s = &x->slots[at];
        if (s->length == length && !memcmp(s->sha, sha, 32) &&
            !memcmp(items[s->item].archive + s->offset, bytes, length)) return s;
        at = (at + 1) & x->mask;
    }
    return NULL;
}

static size_t ms_shards(size_t bytes, size_t shard_chars)
{
    size_t chars = (bytes / 3u + (bytes % 3u != 0)) * 4u;
    return chars / shard_chars + (chars % shard_chars != 0);
}

static int ms_append(ms_op **ops, size_t *count, size_t *capacity, int kind, size_t base, size_t offset, size_t length)
{
    ms_op *last = *count ? &(*ops)[*count - 1] : NULL;
    if (last && last->kind == kind &&
        (kind == MS_OP_LITERAL || (last->base == base && last->offset + last->length == offset))) {
        last->length += length; return 1;
    }
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : 64u;
        ms_op *grown;
        if (next < *capacity || next > SIZE_MAX / sizeof(*grown) || MS_REFUSED() ||
            !(grown = realloc(*ops, next * sizeof(*grown)))) return 0;
        *ops = grown; *capacity = next;
    }
    (*ops)[(*count)++] = (ms_op){kind, base, offset, length};
    return 1;
}

/* Serialize item self's delta; bases are the referenced items in first-use order. */
static unsigned char *ms_encode(const sh_mpkg_share_item *items, const ms_split *splits, size_t self,
    const ms_op *ops, size_t count, const size_t *order, size_t bases, size_t total)
{
    unsigned char *out = ms_calloc(total, 1), *p;
    size_t at = 0;
    if (!out) return NULL;
    memcpy(out, SH_MPKG_DELTA_MAGIC, 8);
    p = ms_w32(out + 8, SH_MPKG_DELTA_VERSION);
    p = ms_w64(p, items[self].length);
    memcpy(p, splits[self].archive, 32); p += 32;
    p = ms_w32(p, (unsigned)bases);
    for (size_t b = 0; b < bases; b++) {
        size_t key = strlen(items[order[b]].key);
        p = ms_w16(p, (unsigned)key); memcpy(p, items[order[b]].key, key); p += key;
        p = ms_w64(p, items[order[b]].length);
        memcpy(p, splits[order[b]].archive, 32); p += 32;
    }
    p = ms_w32(p, (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        *p++ = (unsigned char)ops[i].kind;
        if (ops[i].kind == MS_OP_COPY) {
            size_t b = 0;
            while (order[b] != ops[i].base) b++;
            p = ms_w32(p, (unsigned)b); p = ms_w64(p, ops[i].offset); p = ms_w64(p, ops[i].length);
        } else {
            p = ms_w64(p, ops[i].length);
            memcpy(p, items[self].archive + at, ops[i].length); p += ops[i].length;
        }
        at += ops[i].length;
    }
    return out;
}

void sh_mpkg_share_clear(sh_mpkg_share_item *items, size_t count)
{
    for (size_t i = 0; items && i < count; i++) { free(items[i].delta); items[i].delta = NULL; items[i].delta_length = 0; }
}

int sh_mpkg_share_plan(sh_mpkg_share_item *items, size_t count, size_t shard_chars,
    char *error, size_t capacity)
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    ms_index index = {0};
    ms_split *splits = NULL;
    ms_op *ops = NULL;
    size_t total = 0, slots = 1, op_capacity = 0;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    for (size_t i = 0; items && i < count; i++) { items[i].delta = NULL; items[i].delta_length = 0; }
    if (!items || !shard_chars) return ms_error(error, capacity, "nothing to plan");
    for (size_t i = 0; i < count; i++) {
        if (!items[i].key || !items[i].key[0] || strlen(items[i].key) >= SH_MPKG_DELTA_KEY_CAP ||
            !items[i].archive || !items[i].length) return ms_error(error, capacity, "a delivery has no key or archive");
        /* A repeated key replaces its earlier carrier: share nothing then. */
        for (size_t j = 0; j < i; j++) if (!strcmp(items[i].key, items[j].key)) return 1;
    }
    if (count < 2) return 1;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
        ms_error(error, capacity, "cannot hash package deliveries"); goto done;
    }
    if (!(splits = ms_calloc(count, sizeof(*splits)))) goto nomem;
    for (size_t i = 0; i < count; i++) {
        ms_split *s = &splits[i];
        if (!(s->bounds = ms_bounds(items[i].archive, items[i].length, &s->count)) ||
            !(s->sha = ms_calloc(s->count, sizeof(*s->sha)))) goto nomem;
        for (size_t k = 0; k < s->count; k++) {
            if (!ms_sha256(algorithm, items[i].archive + s->bounds[k], s->bounds[k + 1] - s->bounds[k], s->sha[k])) goto nohash;
            if (MS_COLLIDING()) memset(s->sha[k], 0x5a, 32); /* tests: every lookup collides */
        }
        if (!ms_sha256(algorithm, items[i].archive, items[i].length, s->archive)) goto nohash;
        if (s->count > SIZE_MAX / 4u - total) goto nomem;
        total += s->count;
    }
    while (slots < total * 2u) slots *= 2u;
    if (!(index.slots = ms_calloc(slots, sizeof(*index.slots)))) goto nomem;
    index.mask = slots - 1u;
    for (size_t i = 0; i < count; i++) {
        const ms_split *s = &splits[i];
        size_t op_count = 0, order[SH_MPKG_DELTA_MAX_BASES], bases = 0, copies = 0;
        for (size_t k = 0; i && k < s->count; k++) {
            size_t from = s->bounds[k], length = s->bounds[k + 1] - from;
            const ms_slot *found = ms_find(&index, items, s->sha[k], items[i].archive + from, length);
            if (!(found ? ms_append(&ops, &op_count, &op_capacity, MS_OP_COPY, found->item, found->offset, length) :
                    ms_append(&ops, &op_count, &op_capacity, MS_OP_LITERAL, 0, 0, length))) goto nomem;
            copies += found != NULL;
        }
        for (size_t o = 0; copies && o < op_count; o++) if (ops[o].kind == MS_OP_COPY) {
            size_t b = 0;
            while (b < bases && order[b] != ops[o].base) b++;
            if (b == bases) {
                if (bases == SH_MPKG_DELTA_MAX_BASES) { copies = 0; break; }
                order[bases++] = ops[o].base;
            }
        }
        if (copies) {
            size_t length = MS_HEADER + 4u;
            for (size_t b = 0; b < bases; b++) length += MS_BASE + strlen(items[order[b]].key);
            for (size_t o = 0; o < op_count; o++) length += ops[o].kind == MS_OP_COPY ? MS_COPY : MS_LITERAL + ops[o].length;
            /* A delta must at least halve its carrier: its readers also read its
             * bases, and older readers refuse the map, so a small overlap stays classic. */
            if (2u * ms_shards(length, shard_chars) <= ms_shards(items[i].length, shard_chars)) {
                if (!(items[i].delta = ms_encode(items, splits, i, ops, op_count, order, bases, length))) goto nomem;
                items[i].delta_length = length;
                continue;
            }
        }
        /* Only classic carriers are bases: a delta never reads another delta. */
        for (size_t k = 0; k < s->count; k++)
            ms_insert(&index, items, s->sha[k], i, s->bounds[k], s->bounds[k + 1] - s->bounds[k]);
    }
    ok = 1; goto done;
nohash:
    ms_error(error, capacity, "cannot hash package deliveries"); goto done;
nomem:
    ms_error(error, capacity, "out of memory sharing package deliveries");
done:
    if (!ok) sh_mpkg_share_clear(items, count);
    for (size_t i = 0; splits && i < count; i++) { free(splits[i].bounds); free(splits[i].sha); }
    free(splits); free(index.slots); free(ops);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

/* --------------------------------------------------------------- decode --- */

typedef struct ms_reader { const unsigned char *p; size_t left; } ms_reader;
static int ms_take(ms_reader *r, size_t n, const unsigned char **at)
{ if (r->left < n) return 0; *at = r->p; r->p += n; r->left -= n; return 1; }

typedef struct ms_base {
    char key[SH_MPKG_DELTA_KEY_CAP];
    uint64_t length;
    unsigned char sha[32];
    unsigned char *bytes;
    int read;
} ms_base;

static int ms_key_valid(const unsigned char *key, size_t length)
{
    if (!length || length >= SH_MPKG_DELTA_KEY_CAP) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

unsigned char *sh_mpkg_delta_decode(const unsigned char *payload, size_t length, const char *self,
    sh_mpkg_delta_resolve resolve, void *context, size_t max_archive, size_t *out_length,
    char *error, size_t capacity)
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    ms_reader r = {payload, length};
    const unsigned char *at, *ops_start;
    ms_base *bases = NULL;
    unsigned char *out = NULL, sha[32], target_sha[32];
    uint64_t target, produced = 0;
    unsigned version, base_count = 0, op_count;
    const char *reason = "shared package carrier is truncated";
    int ok = 0;
    if (out_length) *out_length = 0;
    if (error && capacity) error[0] = 0;
    if (!payload || !resolve || !out_length || !self) { ms_error(error, capacity, "bad arguments"); return NULL; }
    if (!sh_mpkg_delta_is(payload, length) || !ms_take(&r, MS_HEADER, &at)) goto refused;
    version = ms_r32(at + 8);
    if (version != SH_MPKG_DELTA_VERSION) {
        if (error && capacity) snprintf(error, capacity,
            "shared package carrier format %u is newer than this Snapmap+ reads", version);
        return NULL;
    }
    target = ms_r64(at + 12); memcpy(target_sha, at + 20, 32); base_count = ms_r32(at + 52);
    if (!target || target > max_archive || target > SIZE_MAX) { reason = "shared package carrier has an invalid archive length"; goto refused; }
    if (!base_count || base_count > SH_MPKG_DELTA_MAX_BASES) { reason = "shared package carrier has an invalid base count"; goto refused; }
    if (!(bases = ms_calloc(base_count, sizeof(*bases)))) { reason = "out of memory reading a shared package carrier"; goto refused; }
    for (unsigned b = 0; b < base_count; b++) {
        size_t key;
        if (!ms_take(&r, 2, &at)) goto refused;
        key = ms_r16(at);
        if (!ms_take(&r, key, &at)) goto refused;
        if (!ms_key_valid(at, key)) { reason = "shared package carrier names an invalid base"; goto refused; }
        memcpy(bases[b].key, at, key); bases[b].key[key] = 0;
        if (!strcmp(bases[b].key, self)) { reason = "shared package carrier names itself as a base"; goto refused; }
        for (unsigned c = 0; c < b; c++)
            if (!strcmp(bases[c].key, bases[b].key)) { reason = "shared package carrier repeats a base"; goto refused; }
        if (!ms_take(&r, MS_BASE - 2u, &at)) goto refused;
        bases[b].length = ms_r64(at); memcpy(bases[b].sha, at + 8, 32);
        if (!bases[b].length || bases[b].length > max_archive) { reason = "shared package carrier has an invalid base length"; goto refused; }
    }
    if (!ms_take(&r, 4, &at)) goto refused;
    op_count = ms_r32(at);
    if (!op_count || op_count > r.left / (MS_LITERAL + 1u)) { reason = "shared package carrier has an invalid operation count"; goto refused; }
    /* Prove every operation and the exact total before reading any base. */
    ops_start = r.p;
    for (unsigned o = 0; o < op_count; o++) {
        uint64_t count;
        int kind;
        if (!ms_take(&r, 1, &at)) goto refused;
        kind = *at;
        if (kind == MS_OP_COPY) {
            unsigned b; uint64_t offset;
            if (!ms_take(&r, MS_COPY - 1u, &at)) goto refused;
            b = ms_r32(at); offset = ms_r64(at + 4); count = ms_r64(at + 12);
            if (b >= base_count || !count || offset > bases[b].length || count > bases[b].length - offset) {
                reason = "shared package carrier reads outside its base"; goto refused;
            }
            bases[b].read = 1;
        } else if (kind == MS_OP_LITERAL) {
            if (!ms_take(&r, MS_LITERAL - 1u, &at)) goto refused;
            count = ms_r64(at);
            if (!count) { reason = "shared package carrier has an empty operation"; goto refused; }
            if (count > r.left || !ms_take(&r, (size_t)count, &at)) goto refused;
        } else { reason = "shared package carrier has an unknown operation"; goto refused; }
        if (count > target - produced) { reason = "shared package carrier overruns its archive"; goto refused; }
        produced += count;
    }
    if (produced != target) { reason = "shared package carrier does not cover its archive"; goto refused; }
    if (r.left) { reason = "shared package carrier has trailing bytes"; goto refused; }
    for (unsigned b = 0; b < base_count; b++)
        if (!bases[b].read) { reason = "shared package carrier names an unused base"; goto refused; }
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
        reason = "cannot verify a shared package carrier"; goto refused;
    }
    for (unsigned b = 0; b < base_count; b++) {
        size_t got = 0;
        char detail[256] = "";
        bases[b].bytes = resolve(context, bases[b].key, &got, detail, sizeof(detail));
        if (!bases[b].bytes) {
            if (error && capacity) snprintf(error, capacity, "shared package carrier's base '%s' is unavailable: %s",
                bases[b].key, detail[0] ? detail : "missing");
            goto done;
        }
        if (sh_mpkg_delta_is(bases[b].bytes, got)) { reason = "shared package carrier's base is itself shared"; goto refused; }
        if (got != bases[b].length || !ms_sha256(algorithm, bases[b].bytes, got, sha) || memcmp(sha, bases[b].sha, 32)) {
            if (error && capacity) snprintf(error, capacity,
                "shared package carrier's base '%s' is not its recorded archive", bases[b].key);
            goto done;
        }
    }
    if (!(out = ms_heap((size_t)target))) { reason = "out of memory rebuilding a shared package"; goto refused; }
    {
        size_t written = 0;
        r.p = ops_start; r.left = length - (size_t)(ops_start - payload);
        for (unsigned o = 0; o < op_count; o++) {
            const unsigned char *field;
            size_t count;
            ms_take(&r, 1, &at);
            if (*at == MS_OP_COPY) {
                ms_take(&r, MS_COPY - 1u, &field);
                count = (size_t)ms_r64(field + 12);
                memcpy(out + written, bases[ms_r32(field)].bytes + (size_t)ms_r64(field + 4), count);
            } else {
                ms_take(&r, MS_LITERAL - 1u, &field);
                count = (size_t)ms_r64(field);
                ms_take(&r, count, &field);
                memcpy(out + written, field, count);
            }
            written += count;
        }
    }
    if (!ms_sha256(algorithm, out, (size_t)target, sha) || memcmp(sha, target_sha, 32)) {
        reason = "shared package carrier does not rebuild its recorded archive"; goto refused;
    }
    ok = 1; goto done;
refused:
    ms_error(error, capacity, reason);
done:
    for (unsigned b = 0; bases && b < base_count; b++) if (bases[b].bytes) HeapFree(GetProcessHeap(), 0, bases[b].bytes);
    free(bases);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok && out) { HeapFree(GetProcessHeap(), 0, out); out = NULL; }
    if (out) *out_length = (size_t)target;
    return out;
}
