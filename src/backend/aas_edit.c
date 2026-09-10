/* Editable AAS payload storage. Header, settings and lump records remain in
 * their original big-endian form, preserving every untouched byte. Per-lump
 * allocations make appends local; serialization concatenates them.
 *
 * Parsing checks the file layout and count relationships. Cross-lump indices
 * and BSP integrity belong to sh_navmesh_validate_aas, which must validate
 * serialized output before the engine receives it.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "aas_edit.h"
#include "navmesh.h"

/* Lump order and record sizes mirror NAV_LUMPS in navmesh.c. These tables
 * must agree; both modules are independently linked by tests.
 */
typedef struct aas_lump_def { const char *name; unsigned record; } aas_lump_def;

static const aas_lump_def AAS_LUMPS[SH_AAS_L__COUNT] = {
    { "planes",                  16 },
    { "vertices",                12 },
    { "edges",                   12 },
    { "edgeIndex",                4 },
    { "reachabilities",          40 },
    { "areas",                   44 },
    { "nodes",                   16 },
    { "portals",                 12 },
    { "portalIndex",              4 },
    { "clusters",                16 },
    { "obstaclePVS",              1 },
    { "reachNames",             132 },
    { "traversalAnimNames",     128 },
    { "dependencyNames",        128 },
    { "interactionEntityNames", 128 },
    { "cover",                   56 },
    { "areaCoverIndex",           4 },
    { "touchingCoverIndex",       4 },
    { "traversalPoints",         60 },
    { "hintNodes",               24 },
    { "trees",                   24 },
    { "areaBounds",              12 }
};

/* Settings: u32 type, three length-prefixed strings, then 39 big-endian
 * words. Shipped strings each occupy 64 bytes: 4 + 3*(4+64) + 39*4 = 364.
 * Words 0..5 hold agent mins/maxs; word 12 is maxStepHeight.
 *
 * The word base is derived from the string lengths. If they leave an invalid
 * word span, the reader uses the shipped offset, 208.
 */
#define AAS_SET_WORDS_OFF   208u
#define AAS_SET_WORD_COUNT  39u
#define AAS_SET_W_BBOX      0u      /* words 0..2 mins, 3..5 maxs */
#define AAS_SET_W_MAXSTEP   12u

typedef struct aas_lump {
    unsigned char *bytes;   /* `count` records, big-endian, verbatim */
    unsigned       count;
    unsigned       cap;     /* records the allocation holds */
} aas_lump;

struct sh_aas {
    unsigned char header[SH_AAS_HEADER_BYTES];
    unsigned char settings[SH_AAS_SETTINGS_BYTES];
    aas_lump      lump[SH_AAS_L__COUNT];
};

static void aas_verr(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || cap == 0) return;
    va_start(ap, fmt);
    _vsnprintf_s(err, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static int aas_lump_ok(int lump) { return lump >= 0 && lump < SH_AAS_L__COUNT; }

/* Area indices are u16 in areaBounds, reachabilities and BSP leaves. */
static unsigned aas_lump_cap(int lump)
{
    return (lump == SH_AAS_L_AREAS) ? (unsigned)SH_AAS_MAX_AREAS
                                    : (unsigned)SH_AAS_MAX_RECORDS;
}

/* ==================================================================== */
/* big-endian field helpers                                              */
/* ==================================================================== */

uint32_t sh_aas_get_u32(const unsigned char *rec, unsigned off)
{
    const unsigned char *p = rec + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint16_t sh_aas_get_u16(const unsigned char *rec, unsigned off)
{
    const unsigned char *p = rec + off;
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}

int32_t sh_aas_get_i32(const unsigned char *rec, unsigned off)
{
    return (int32_t)sh_aas_get_u32(rec, off);
}

int16_t sh_aas_get_i16(const unsigned char *rec, unsigned off)
{
    return (int16_t)sh_aas_get_u16(rec, off);
}

/* Through the integer, not a cast of the pointer: the record is not aligned to
 * anything, and the on-disk order is big-endian whatever the host is. */
float sh_aas_get_f32(const unsigned char *rec, unsigned off)
{
    uint32_t bits = sh_aas_get_u32(rec, off);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

void sh_aas_put_u32(unsigned char *rec, unsigned off, uint32_t v)
{
    unsigned char *p = rec + off;
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

void sh_aas_put_u16(unsigned char *rec, unsigned off, uint16_t v)
{
    unsigned char *p = rec + off;
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

void sh_aas_put_i32(unsigned char *rec, unsigned off, int32_t v)
{
    sh_aas_put_u32(rec, off, (uint32_t)v);
}

void sh_aas_put_i16(unsigned char *rec, unsigned off, int16_t v)
{
    sh_aas_put_u16(rec, off, (uint16_t)v);
}

void sh_aas_put_f32(unsigned char *rec, unsigned off, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    sh_aas_put_u32(rec, off, bits);
}

/* ==================================================================== */
/* lifetime                                                              */
/* ==================================================================== */

void sh_aas_free(sh_aas *a)
{
    int i;
    if (!a) return;
    for (i = 0; i < SH_AAS_L__COUNT; i++)
        if (a->lump[i].bytes) HeapFree(GetProcessHeap(), 0, a->lump[i].bytes);
    HeapFree(GetProcessHeap(), 0, a);
}

sh_aas *sh_aas_parse(const unsigned char *bytes, size_t len, char *err, size_t err_cap)
{
    unsigned counts[SH_AAS_L__COUNT];
    size_t   starts[SH_AAS_L__COUNT];
    size_t   off = SH_AAS_PREAMBLE_BYTES;
    sh_aas  *a;
    int      i;

    if (err && err_cap) err[0] = '\0';
    if (!bytes) {
        aas_verr(err, err_cap, "no payload");
        return NULL;
    }
    if (len > SH_SMNAV_MAX_PAYLOAD) {
        aas_verr(err, err_cap, "the payload is %zu bytes, over the %u-byte budget",
                 len, (unsigned)SH_SMNAV_MAX_PAYLOAD);
        return NULL;
    }
    if (len < SH_AAS_PREAMBLE_BYTES + (size_t)SH_AAS_LUMPS * 4) {
        aas_verr(err, err_cap, "the payload is %zu bytes, too small to be an AAS file", len);
        return NULL;
    }
    if (memcmp(bytes, SH_AAS_MAGIC, 4) != 0) {
        aas_verr(err, err_cap, "the payload does not start with the AAS magic");
        return NULL;
    }
    if (bytes[4] != SH_AAS_MAJOR || bytes[5] != SH_AAS_MINOR) {
        aas_verr(err, err_cap, "the AAS version is %u.%u, not %u.%u",
                 (unsigned)bytes[4], (unsigned)bytes[5],
                 (unsigned)SH_AAS_MAJOR, (unsigned)SH_AAS_MINOR);
        return NULL;
    }

    /* Walk all 22 count-prefixed lumps and require an exact end-of-file match. */
    for (i = 0; i < SH_AAS_L__COUNT; i++) {
        uint64_t bytes_needed;
        if (len - off < 4) {
            aas_verr(err, err_cap, "lump %s: no room for its count", AAS_LUMPS[i].name);
            return NULL;
        }
        counts[i] = sh_aas_get_u32(bytes + off, 0);
        off += 4;
        if (counts[i] > SH_AAS_MAX_RECORDS) {
            aas_verr(err, err_cap, "lump %s: a count of %u is beyond any real file",
                     AAS_LUMPS[i].name, counts[i]);
            return NULL;
        }
        bytes_needed = (uint64_t)counts[i] * AAS_LUMPS[i].record;
        if (bytes_needed > (uint64_t)(len - off)) {
            aas_verr(err, err_cap, "lump %s: %u records overrun the payload",
                     AAS_LUMPS[i].name, counts[i]);
            return NULL;
        }
        starts[i] = off;
        off += (size_t)bytes_needed;
    }
    if (off != len) {
        aas_verr(err, err_cap, "%zu trailing bytes after the last lump", len - off);
        return NULL;
    }

    /* Enforce the validator's file-level count relationships before allocating. */
    if (counts[SH_AAS_L_AREAS] > SH_AAS_MAX_AREAS) {
        aas_verr(err, err_cap, "%u areas, over the %u a u16 index can reach",
                 counts[SH_AAS_L_AREAS], (unsigned)SH_AAS_MAX_AREAS);
        return NULL;
    }
    if (counts[SH_AAS_L_CLUSTERS] > 0xFFFFu) {
        aas_verr(err, err_cap, "%u clusters, over the %u a u16 index can reach",
                 counts[SH_AAS_L_CLUSTERS], 0xFFFFu);
        return NULL;
    }
    if (counts[SH_AAS_L_AREABOUNDS] != counts[SH_AAS_L_AREAS]) {
        aas_verr(err, err_cap, "areaBounds holds %u records for %u areas",
                 counts[SH_AAS_L_AREABOUNDS], counts[SH_AAS_L_AREAS]);
        return NULL;
    }

    a = (sh_aas *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *a);
    if (!a) {
        aas_verr(err, err_cap, "out of memory parsing the AAS payload");
        return NULL;
    }
    memcpy(a->header, bytes, SH_AAS_HEADER_BYTES);
    memcpy(a->settings, bytes + SH_AAS_HEADER_BYTES, SH_AAS_SETTINGS_BYTES);

    /* Start at the parsed count; later appends grow capacity geometrically. */
    for (i = 0; i < SH_AAS_L__COUNT; i++) {
        size_t n = (size_t)counts[i] * AAS_LUMPS[i].record;
        a->lump[i].count = counts[i];
        a->lump[i].cap = counts[i];
        if (n == 0) continue;
        a->lump[i].bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, n);
        if (!a->lump[i].bytes) {
            aas_verr(err, err_cap, "out of memory reading lump %s", AAS_LUMPS[i].name);
            sh_aas_free(a);
            return NULL;
        }
        memcpy(a->lump[i].bytes, bytes + starts[i], n);
    }
    return a;
}

unsigned char *sh_aas_write(const sh_aas *a, size_t *out_len)
{
    unsigned char *out;
    size_t total = SH_AAS_PREAMBLE_BYTES, off;
    int i;

    if (out_len) *out_len = 0;
    if (!a) return NULL;

    for (i = 0; i < SH_AAS_L__COUNT; i++)
        total += 4 + (size_t)a->lump[i].count * AAS_LUMPS[i].record;

    /* Keep serialized output within the same payload limit as the shard reader. */
    if (total > SH_SMNAV_MAX_PAYLOAD) return NULL;

    out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, total);
    if (!out) return NULL;

    memcpy(out, a->header, SH_AAS_HEADER_BYTES);
    memcpy(out + SH_AAS_HEADER_BYTES, a->settings, SH_AAS_SETTINGS_BYTES);
    off = SH_AAS_PREAMBLE_BYTES;
    for (i = 0; i < SH_AAS_L__COUNT; i++) {
        size_t n = (size_t)a->lump[i].count * AAS_LUMPS[i].record;
        sh_aas_put_u32(out + off, 0, a->lump[i].count);
        off += 4;
        if (n) memcpy(out + off, a->lump[i].bytes, n);
        off += n;
    }
    if (out_len) *out_len = total;
    return out;
}

/* ==================================================================== */
/* generic lump access                                                   */
/* ==================================================================== */

unsigned sh_aas_count(const sh_aas *a, int lump)
{
    if (!a || !aas_lump_ok(lump)) return 0;
    return a->lump[lump].count;
}

unsigned sh_aas_record_size(int lump)
{
    if (!aas_lump_ok(lump)) return 0;
    return AAS_LUMPS[lump].record;
}

unsigned char *sh_aas_rec(sh_aas *a, int lump, unsigned i)
{
    if (!a || !aas_lump_ok(lump)) return NULL;
    if (i >= a->lump[lump].count) return NULL;
    return a->lump[lump].bytes + (size_t)i * AAS_LUMPS[lump].record;
}

const unsigned char *sh_aas_rec_const(const sh_aas *a, int lump, unsigned i)
{
    return sh_aas_rec((sh_aas *)a, lump, i);
}

/* Grow geometrically, with a minimum allocation of 16 records. */
static int aas_reserve(aas_lump *L, int lump, unsigned need)
{
    unsigned limit = aas_lump_cap(lump);
    unsigned cap = L->cap;
    unsigned char *p;
    size_t bytes;

    if (need <= cap) return 1;
    if (cap < 16) cap = 16;
    while (cap < need) cap *= 2;      /* need <= limit <= SH_AAS_MAX_RECORDS */
    if (cap > limit) cap = limit;

    bytes = (size_t)cap * AAS_LUMPS[lump].record;
    p = L->bytes
        ? (unsigned char *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, L->bytes, bytes)
        : (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (!p) return 0;
    L->bytes = p;
    L->cap = cap;
    return 1;
}

int sh_aas_truncate(sh_aas *a, int lump, unsigned count)
{
    if (!a || !aas_lump_ok(lump) || count > a->lump[lump].count) return 0;
    a->lump[lump].count = count;
    return 1;
}

int sh_aas_append(sh_aas *a, int lump, unsigned n, unsigned *out_first)
{
    aas_lump *L;
    uint64_t want;

    if (!a || !aas_lump_ok(lump)) return 0;
    L = &a->lump[lump];
    want = (uint64_t)L->count + n;
    if (want > (uint64_t)aas_lump_cap(lump)) return 0;
    if (out_first) *out_first = L->count;
    if (n == 0) return 1;
    if (!aas_reserve(L, lump, (unsigned)want)) return 0;
    memset(L->bytes + (size_t)L->count * AAS_LUMPS[lump].record, 0,
           (size_t)n * AAS_LUMPS[lump].record);
    L->count = (unsigned)want;
    return 1;
}

/* ==================================================================== */
/* the settings block                                                    */
/* ==================================================================== */

/* Where the 39 words begin: past the u32 type and the three length-prefixed
 * strings. See the layout note at the top of this file for the fallback. */
static unsigned aas_words_off(const sh_aas *a)
{
    unsigned off = 4;
    int i;
    for (i = 0; i < 3; i++) {
        uint32_t n;
        if (off + 4 > SH_AAS_SETTINGS_BYTES) return AAS_SET_WORDS_OFF;
        n = sh_aas_get_u32(a->settings, off);
        off += 4;
        if (n > (uint32_t)(SH_AAS_SETTINGS_BYTES - off)) return AAS_SET_WORDS_OFF;
        off += (unsigned)n;
    }
    if (off + AAS_SET_WORD_COUNT * 4 != SH_AAS_SETTINGS_BYTES) return AAS_SET_WORDS_OFF;
    return off;
}

float sh_aas_setting_f32(const sh_aas *a, unsigned off)
{
    if (!a || off > SH_AAS_SETTINGS_BYTES - 4) return 0.0f;
    return sh_aas_get_f32(a->settings, off);
}

void sh_aas_set_setting_f32(sh_aas *a, unsigned off, float v)
{
    if (!a || off > SH_AAS_SETTINGS_BYTES - 4) return;
    sh_aas_put_f32(a->settings, off, v);
}

void sh_aas_agent_bounds(const sh_aas *a, float out_mins[3], float out_maxs[3])
{
    unsigned base;
    int i;

    if (out_mins) out_mins[0] = out_mins[1] = out_mins[2] = 0.0f;
    if (out_maxs) out_maxs[0] = out_maxs[1] = out_maxs[2] = 0.0f;
    if (!a) return;

    base = aas_words_off(a) + AAS_SET_W_BBOX * 4;
    for (i = 0; i < 3; i++) {
        if (out_mins) out_mins[i] = sh_aas_setting_f32(a, base + (unsigned)i * 4);
        if (out_maxs) out_maxs[i] = sh_aas_setting_f32(a, base + (unsigned)(3 + i) * 4);
    }
}

float sh_aas_max_step_height(const sh_aas *a)
{
    if (!a) return 0.0f;
    return sh_aas_setting_f32(a, aas_words_off(a) + AAS_SET_W_MAXSTEP * 4);
}
