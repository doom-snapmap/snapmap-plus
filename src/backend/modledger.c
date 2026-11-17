/* modledger.c -- the served-bytes ledger (modledger.h). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "modledger.h"

typedef struct ml_slot {
    volatile LONG state;       /* 0 empty, 1 published (its request, hash and first serve written) */
    volatile LONG ver;         /* odd while the fields below change */
    uint32_t hash;
    const char *request;
    sh_modledger_ident id;
    void *obj;
    LONG serves;
    LONG seq;
} ml_slot;

/* The levels (modledger.h): level k has SH_MODLEDGER_SLOTS << k slots. A
 * level's pointer is published before g_nlevels counts it; only the newest
 * takes inserts, under g_insert, which also guards g_newest_used. */
static ml_slot *volatile g_levels[SH_MODLEDGER_LEVELS];
static volatile LONG g_nlevels;
static uint32_t g_newest_used;   /* records in the newest level */
static SRWLOCK g_insert = SRWLOCK_INIT;
static volatile LONG g_records, g_serves, g_overflow, g_seq;
static volatile LONG g_complete = 1;

static uint32_t ml_level_slots(int k) { return SH_MODLEDGER_SLOTS << k; }

static char ml_fold(char c)
{
    if (c == '\\') return '/';
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static uint32_t ml_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) { h ^= (unsigned char)ml_fold(*s); h *= 16777619u; }
    return h;
}

static int ml_equal(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) if (ml_fold(*a) != ml_fold(*b)) return 0;
    return !*a && !*b;
}

static int ml_count(void) { return (int)InterlockedCompareExchange(&g_nlevels, 0, 0); }

static ml_slot *ml_level(int k)
{
    return (ml_slot *)InterlockedCompareExchangePointer((PVOID volatile *)&g_levels[k], NULL, NULL);
}

/* Under g_insert: the newest level with room for one more record, allocating
 * the next one when the newest is half full; NULL when none can be had. */
static ml_slot *ml_insert_level(int *k_out)
{
    int n = ml_count();
    ml_slot *t;
    if (n && g_newest_used < ml_level_slots(n - 1) / 2) { *k_out = n - 1; return ml_level(n - 1); }
    if (n == SH_MODLEDGER_LEVELS) return NULL;
    t = (ml_slot *)VirtualAlloc(NULL, sizeof(ml_slot) * (size_t)ml_level_slots(n), MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (!t) return NULL;
    InterlockedExchangePointer((PVOID volatile *)&g_levels[n], t);
    InterlockedExchange(&g_nlevels, n + 1);
    g_newest_used = 0;
    *k_out = n;
    return t;
}

static int ml_hexdigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The first 16 hex digits; 0 when fewer are present. */
static uint64_t ml_hex64(const char *s)
{
    uint64_t v = 0;
    int i;
    if (!s) return 0;
    for (i = 0; i < 16; ++i) {
        int d = ml_hexdigit(s[i]);
        if (d < 0) return 0;
        v = (v << 4) | (uint64_t)d;
    }
    return v;
}

int sh_modledger_ident_of(const char *bundle_hash, const char *file_hash, const unsigned char *bytes, size_t length,
                          sh_modledger_ident *out)
{
    uint64_t sha;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->bundle = ml_hex64(bundle_hash);
    out->length = (uint64_t)length;
    sha = ml_hex64(file_hash);
    if (sha) { out->kind = SH_MODLEDGER_ID_SHA; out->file = sha; return 1; }
    if (bytes || !length) {
        uint64_t h = 1469598103934665603ull;
        size_t i;
        for (i = 0; i < length; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
        out->kind = SH_MODLEDGER_ID_FNV;
        out->file = h;
        return 1;
    }
    return 0;
}

int sh_modledger_same(const sh_modledger_ident *a, const sh_modledger_ident *b)
{
    /* A recorded SHA-256 names the bytes whatever size was reported (a stock
     * pointer's indexed size is 0); bytes hashed here also compare lengths. */
    return a && b && a->kind != SH_MODLEDGER_ID_NONE && a->kind == b->kind && a->file == b->file &&
           (a->kind == SH_MODLEDGER_ID_SHA || a->length == b->length);
}

static void ml_write_begin(ml_slot *s)
{
    for (;;) {
        LONG v = InterlockedCompareExchange(&s->ver, 0, 0);
        if (!(v & 1) && InterlockedCompareExchange(&s->ver, v + 1, v) == v) return;
        YieldProcessor();
    }
}

static void ml_write_end(ml_slot *s) { InterlockedIncrement(&s->ver); }

/* The published slot of `request` in level `k`, or NULL with the empty slot it
 * would take. A level is never more than half full, so a probe ends at an
 * empty slot. */
static ml_slot *ml_probe_level(int k, const char *request, uint32_t h, ml_slot **empty)
{
    ml_slot *t = ml_level(k);
    uint32_t mask = ml_level_slots(k) - 1, i, n;
    if (empty) *empty = NULL;
    for (n = 0, i = h & mask; n <= mask; ++n, i = (i + 1) & mask) {
        ml_slot *s = &t[i];
        if (!InterlockedCompareExchange(&s->state, 0, 0)) { if (empty) *empty = s; return NULL; }
        if (s->hash == h && ml_equal(s->request, request)) return s;
    }
    return NULL;
}

/* The published slot of `request` in any level, or NULL. Lock-free. */
static ml_slot *ml_probe(const char *request, uint32_t h)
{
    int k, n = ml_count();
    for (k = 0; k < n; ++k) {
        ml_slot *s = ml_probe_level(k, request, h, NULL);
        if (s) return s;
    }
    return NULL;
}

/* The first serve of `request`: under the writer lock, probe every level again
 * (another thread may have just published it) and otherwise publish it, with
 * this serve written, in the newest level. 1 published, 0 found, -1 no room. */
static int ml_insert(const char *request, uint32_t h, const sh_modledger_ident *id, ml_slot **found)
{
    ml_slot *s = NULL;
    int k, r = 0;
    AcquireSRWLockExclusive(&g_insert);
    *found = ml_probe(request, h);
    if (!*found) {
        if (ml_insert_level(&k)) (void)ml_probe_level(k, request, h, &s);
        if (!s) r = -1;
        else {
            s->hash = h;
            s->request = request;
            s->id = *id;
            s->serves = 1;
            s->seq = InterlockedIncrement(&g_seq);
            g_newest_used++;
            InterlockedIncrement(&g_records);
            InterlockedExchange(&s->state, 1);
            r = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_insert);
    return r;
}

int sh_modledger_record(const char *request, const sh_modledger_ident *id)
{
    ml_slot *s;
    uint32_t h;
    if (!request || !request[0] || !id) return 0;
    h = ml_hash(request);
    s = ml_probe(request, h);
    if (!s) {
        int r = ml_insert(request, h, id, &s);
        if (r < 0) { InterlockedIncrement(&g_overflow); return 0; }
        if (r > 0) { InterlockedIncrement(&g_serves); return 1; }
    }
    ml_write_begin(s);
    s->id = *id;
    s->serves++;
    s->seq = InterlockedIncrement(&g_seq);
    ml_write_end(s);
    InterlockedIncrement(&g_serves);
    return 1;
}

static void ml_copy(ml_slot *s, sh_modledger_rec *out)
{
    for (;;) {
        LONG v1 = InterlockedCompareExchange(&s->ver, 0, 0), v2;
        if (v1 & 1) { YieldProcessor(); continue; }
        out->request = s->request;
        out->id = s->id;
        out->obj = s->obj;
        out->serves = s->serves;
        out->seq = (unsigned long)s->seq;
        MemoryBarrier();
        v2 = InterlockedCompareExchange(&s->ver, 0, 0);
        if (v1 == v2) return;
    }
}

int sh_modledger_find(const char *request, sh_modledger_rec *out)
{
    ml_slot *s;
    if (!request) return 0;
    s = ml_probe(request, ml_hash(request));
    if (!s) return 0;
    if (out) ml_copy(s, out);
    return 1;
}

int sh_modledger_bind(const char *request, void *obj)
{
    ml_slot *s;
    if (!request) return 0;
    s = ml_probe(request, ml_hash(request));
    if (!s) return 0;
    ml_write_begin(s);
    s->obj = obj;
    ml_write_end(s);
    return 1;
}

void sh_modledger_set_complete(int complete) { InterlockedExchange(&g_complete, complete ? 1 : 0); }

int sh_modledger_complete(void)
{
    return InterlockedCompareExchange(&g_complete, 0, 0) && !InterlockedCompareExchange(&g_overflow, 0, 0);
}

int sh_modledger_each(sh_modledger_fn fn, void *context)
{
    uint32_t i, slots;
    int n = 0, k, levels = ml_count();
    if (!fn) return 0;
    for (k = 0; k < levels; ++k) {
        ml_slot *t = ml_level(k);
        for (i = 0, slots = ml_level_slots(k); i < slots; ++i) {
            sh_modledger_rec r;
            if (!InterlockedCompareExchange(&t[i].state, 0, 0)) continue;
            ml_copy(&t[i], &r);
            fn(context, &r);
            n++;
        }
    }
    return n;
}

void sh_modledger_get_stats(sh_modledger_stats *out)
{
    int k, levels = ml_count();
    if (!out) return;
    out->records = (unsigned)InterlockedCompareExchange(&g_records, 0, 0);
    for (out->cap = 0, k = 0; k < levels; ++k) out->cap += ml_level_slots(k) / 2;
    out->serves = (unsigned long)InterlockedCompareExchange(&g_serves, 0, 0);
    out->overflow = (unsigned long)InterlockedCompareExchange(&g_overflow, 0, 0);
    out->complete = sh_modledger_complete();
}

int sh_modledger_decide(const sh_modledger_rec *rec, const sh_modledger_ident *now, int stock, int complete)
{
    if (stock) return SH_MODLEDGER_REFRESH;
    if (!rec) return complete ? SH_MODLEDGER_GAME : SH_MODLEDGER_REFRESH;
    return sh_modledger_same(&rec->id, now) ? SH_MODLEDGER_KEEP : SH_MODLEDGER_REFRESH;
}

const char *sh_modledger_decision_name(int decision)
{
    switch (decision) {
    case SH_MODLEDGER_KEEP: return "keep";
    case SH_MODLEDGER_REFRESH: return "refresh";
    case SH_MODLEDGER_GAME: return "game";
    default: return "?";
    }
}

void sh_modledger_reset(void)
{
    int k, levels = ml_count();
    for (k = 0; k < levels; ++k) {
        VirtualFree(ml_level(k), 0, MEM_RELEASE);
        g_levels[k] = NULL;
    }
    g_nlevels = 0;
    g_newest_used = 0;
    g_records = g_serves = g_overflow = g_seq = 0;
    g_complete = 1;
}

/* ------------------------------------------------------------ pass tally */

sh_modledger_tally_type *sh_modledger_tally_add(sh_modledger_tally *t, const char *type, const char *name)
{
    sh_modledger_tally_type *row = NULL;
    size_t at;
    int i;
    if (!t) return NULL;
    if (!type) type = "?";
    for (i = 0; i < t->ntypes; ++i)
        if (!strcmp(t->types[i].type, type)) { row = &t->types[i]; break; }
    if (!row) {
        if (t->ntypes == SH_MODLEDGER_TALLY_TYPES) row = &t->types[SH_MODLEDGER_TALLY_TYPES - 1];
        else {
            row = &t->types[t->ntypes++];
            strncpy_s(row->type, sizeof row->type, type, _TRUNCATE);
        }
    }
    row->count++;
    at = strlen(row->names);
    if (name && at < sizeof row->names - 4)
        _snprintf_s(row->names + at, sizeof row->names - at, _TRUNCATE, "%s%s", at ? ", " : "", name);
    return row;
}

int sh_modledger_tally_truncated(const sh_modledger_tally_type *row)
{
    return row && strlen(row->names) >= sizeof row->names - 5;
}

/* --------------------------------------------------------- name folding */

static char ml_name_fold(char c)
{
    if (c == '-') return '_';
    if (c == 92) return '/';
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int sh_modledger_name_contains(const char *text, const char *needle)
{
    size_t n, i, j, len;
    if (!needle || !needle[0]) return 1;
    if (!text) return 0;
    n = strlen(needle);
    len = strlen(text);
    for (i = 0; i + n <= len; ++i) {
        for (j = 0; j < n && ml_name_fold(text[i + j]) == ml_name_fold(needle[j]); ++j) {}
        if (j == n) return 1;
    }
    return 0;
}
