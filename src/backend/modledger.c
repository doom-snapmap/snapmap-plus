/* modledger.c -- the served-bytes ledger (modledger.h). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "modledger.h"

typedef struct ml_slot {
    volatile LONG state;       /* 0 empty, 1 being claimed, 2 published */
    volatile LONG ver;         /* odd while the fields below change */
    uint32_t hash;
    const char *request;
    sh_modledger_ident id;
    void *obj;
    LONG serves;
    LONG seq;
} ml_slot;

static ml_slot *volatile g_slots;
static volatile LONG g_records, g_serves, g_overflow, g_seq;
static volatile LONG g_complete = 1;

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

static ml_slot *ml_table(int create)
{
    ml_slot *t = (ml_slot *)InterlockedCompareExchangePointer((PVOID volatile *)&g_slots, NULL, NULL);
    if (t || !create) return t;
    t = (ml_slot *)VirtualAlloc(NULL, sizeof(ml_slot) * SH_MODLEDGER_SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t) return NULL;
    if (InterlockedCompareExchangePointer((PVOID volatile *)&g_slots, t, NULL) != NULL) {
        VirtualFree(t, 0, MEM_RELEASE);
        t = (ml_slot *)InterlockedCompareExchangePointer((PVOID volatile *)&g_slots, NULL, NULL);
    }
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

/* The published slot of `request`, or NULL with the empty slot it would take. */
static ml_slot *ml_probe(ml_slot *t, const char *request, uint32_t h, ml_slot **empty)
{
    uint32_t i, n;
    if (empty) *empty = NULL;
    for (n = 0, i = h & (SH_MODLEDGER_SLOTS - 1); n < SH_MODLEDGER_SLOTS; ++n, i = (i + 1) & (SH_MODLEDGER_SLOTS - 1)) {
        ml_slot *s = &t[i];
        LONG st = InterlockedCompareExchange(&s->state, 0, 0);
        while (st == 1) { YieldProcessor(); st = InterlockedCompareExchange(&s->state, 0, 0); }
        if (st == 0) { if (empty) *empty = s; return NULL; }
        if (s->hash == h && ml_equal(s->request, request)) return s;
    }
    return NULL;
}

int sh_modledger_record(const char *request, const sh_modledger_ident *id)
{
    ml_slot *t, *s, *empty;
    uint32_t h;
    if (!request || !request[0] || !id) return 0;
    t = ml_table(1);
    if (!t) { InterlockedIncrement(&g_overflow); return 0; }
    h = ml_hash(request);
    for (;;) {
        s = ml_probe(t, request, h, &empty);
        if (s) break;
        if (!empty || (ULONG)InterlockedCompareExchange(&g_records, 0, 0) >= SH_MODLEDGER_CAP) {
            InterlockedIncrement(&g_overflow);
            return 0;
        }
        if (InterlockedCompareExchange(&empty->state, 1, 0) != 0) continue;   /* another serve took it: probe again */
        empty->hash = h;
        empty->request = request;
        InterlockedIncrement(&g_records);
        InterlockedExchange(&empty->state, 2);
        s = empty;
        break;
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
    ml_slot *t = ml_table(0), *s;
    if (!t || !request) return 0;
    s = ml_probe(t, request, ml_hash(request), NULL);
    if (!s) return 0;
    if (out) ml_copy(s, out);
    return 1;
}

int sh_modledger_bind(const char *request, void *obj)
{
    ml_slot *t = ml_table(0), *s;
    if (!t || !request) return 0;
    s = ml_probe(t, request, ml_hash(request), NULL);
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
    ml_slot *t = ml_table(0);
    uint32_t i;
    int n = 0;
    if (!t || !fn) return 0;
    for (i = 0; i < SH_MODLEDGER_SLOTS; ++i) {
        sh_modledger_rec r;
        if (InterlockedCompareExchange(&t[i].state, 0, 0) != 2) continue;
        ml_copy(&t[i], &r);
        fn(context, &r);
        n++;
    }
    return n;
}

void sh_modledger_get_stats(sh_modledger_stats *out)
{
    if (!out) return;
    out->records = (unsigned)InterlockedCompareExchange(&g_records, 0, 0);
    out->cap = SH_MODLEDGER_CAP;
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
    ml_slot *t = ml_table(0);
    if (t) memset(t, 0, sizeof(ml_slot) * SH_MODLEDGER_SLOTS);
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
