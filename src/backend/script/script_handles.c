/* script_handles.c -- see script_handles.h. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "script_handles.h"

#define INDEX_BITS    18
#define INDEX_MASK    ((1u << INDEX_BITS) - 1u)
#define GEN_MAX       ((1u << (32 - INDEX_BITS)) - 1u)
#define FIRST_CAP     1024
#define REUSE_RESERVE 4096   /* freed slots kept waiting before one is reused, so a slot's generation turns slowly */
#define GONE_FNS      4

enum { ST_FREE = 0, ST_PENDING, ST_LIVE };

typedef struct entry {
    uint32_t spawn_id;
    int16_t  mod;            /* SH_SCRIPT_HANDLE_FOREIGN for a reference no mod owns */
    uint8_t  state;
    uint16_t gen;            /* the slot's generation, 1..GEN_MAX, kept across maps */
    int32_t  prev, next;     /* the active list (insertion order), or next in the free list */
    char    *names;          /* malloc'd entityDef chain, or NULL */
} entry;

static entry   *g_e;
static int32_t  g_cap;                       /* slots allocated */
static int32_t  g_used;                      /* slots handed out this map (fresh ones lie above) */
static int32_t  g_active;                    /* entries pending or live */
static int32_t  g_head = -1, g_tail = -1;    /* the active list */
static int32_t  g_free_head = -1, g_free_tail = -1, g_nfree;
static int32_t *g_hash;                      /* index + 1, 0 = empty */
static uint32_t g_hash_size;                 /* power of two, at least twice the slots */
static SRWLOCK  g_lock = SRWLOCK_INIT;
static sh_script_resolve_fn g_resolve;
static sh_script_gone_fn g_gone[GONE_FNS];

static uint32_t hash_of(uint32_t id)
{
    id ^= id >> 16;
    id *= 0x7FEB352Du;
    id ^= id >> 15;
    return id & (g_hash_size - 1);
}

/* The entry index for a handle of this map, or -1. Lock held by the caller. */
static int index_of(sh_script_handle h)
{
    uint32_t idx;
    if (!h) return -1;
    idx = (h & INDEX_MASK) - 1u;
    if (idx >= (uint32_t)g_used) return -1;
    if (g_e[idx].state == ST_FREE || g_e[idx].gen != (h >> INDEX_BITS)) return -1;
    return (int)idx;
}

static sh_script_handle handle_of(int index)
{
    return ((uint32_t)g_e[index].gen << INDEX_BITS) | (uint32_t)(index + 1);
}

static int find_locked(uint32_t spawn_id)
{
    uint32_t s;
    if (!g_hash_size) return -1;
    s = hash_of(spawn_id);
    for (;;) {
        int32_t v = g_hash[s];
        if (!v) return -1;
        if (g_e[v - 1].spawn_id == spawn_id) return v - 1;
        s = (s + 1) & (g_hash_size - 1);
    }
}

static void hash_put(int index)
{
    uint32_t s = hash_of(g_e[index].spawn_id);
    while (g_hash[s]) s = (s + 1) & (g_hash_size - 1);
    g_hash[s] = index + 1;
}

/* Remove index from the hash, shifting the run after it back (no tombstones). */
static void hash_del(int index)
{
    uint32_t mask = g_hash_size - 1, s = hash_of(g_e[index].spawn_id), j;
    while (g_hash[s] != index + 1) s = (s + 1) & mask;
    j = s;
    for (;;) {
        uint32_t k;
        j = (j + 1) & mask;
        if (!g_hash[j]) break;
        k = hash_of(g_e[g_hash[j] - 1].spawn_id);
        if (s <= j ? (s < k && k <= j) : (s < k || k <= j)) continue;
        g_hash[s] = g_hash[j];
        s = j;
    }
    g_hash[s] = 0;
}

/* Twice the slots (and a hash twice that), up to the handle format's index
 * range. 0 when memory or the format has no more. */
static int grow_locked(void)
{
    int32_t cap = g_cap ? g_cap * 2 : FIRST_CAP, i;
    uint32_t hs = 1;
    entry *e;
    int32_t *h;
    if ((uint32_t)cap > INDEX_MASK) cap = (int32_t)INDEX_MASK;
    if (cap <= g_cap) return 0;
    while (hs < (uint32_t)cap * 2u) hs <<= 1;
    h = (int32_t *)calloc(hs, sizeof *h);
    if (!h) return 0;
    e = (entry *)realloc(g_e, (size_t)cap * sizeof *e);
    if (!e) { free(h); return 0; }
    memset(e + g_cap, 0, (size_t)(cap - g_cap) * sizeof *e);
    g_e = e;
    free(g_hash);
    g_hash = h;
    g_hash_size = hs;
    g_cap = cap;
    for (i = g_head; i >= 0; i = g_e[i].next) hash_put(i);
    return 1;
}

/* A slot to use: a freed one once enough are waiting, else a fresh one
 * (growing when needed), else any freed one. -1 when there is none. */
static int take_slot(void)
{
    int i = -1;
    if (g_nfree <= REUSE_RESERVE && (g_used < g_cap || grow_locked())) {
        i = g_used++;
    } else if (g_nfree) {
        i = g_free_head;
        g_free_head = g_e[i].next;
        if (g_free_head < 0) g_free_tail = -1;
        g_nfree--;
    }
    if (i >= 0) g_e[i].gen = (uint16_t)(g_e[i].gen % GEN_MAX + 1u);
    return i;
}

/* Take entry i out of use: off the active list and the hash, onto the free list. */
static void release_locked(int i)
{
    entry *e = &g_e[i];
    hash_del(i);
    if (e->prev >= 0) g_e[e->prev].next = e->next; else g_head = e->next;
    if (e->next >= 0) g_e[e->next].prev = e->prev; else g_tail = e->prev;
    free(e->names);
    e->names = NULL;
    e->state = ST_FREE;
    e->next = -1;
    e->prev = -1;
    if (g_free_tail >= 0) g_e[g_free_tail].next = i; else g_free_head = i;
    g_free_tail = i;
    g_nfree++;
    g_active--;
}

void sh_script_handles_set_resolver(sh_script_resolve_fn fn)
{
    AcquireSRWLockExclusive(&g_lock);
    g_resolve = fn;
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_script_handles_on_gone(sh_script_gone_fn fn)
{
    int i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < GONE_FNS; i++) {
        if (g_gone[i] == fn) break;
        if (!g_gone[i]) { g_gone[i] = fn; break; }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_script_handles_reset(void)
{
    int i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_used; i++) {
        free(g_e[i].names);
        g_e[i].names = NULL;
        g_e[i].state = ST_FREE;   /* the generation stays: the slot's next use turns it */
    }
    if (g_hash) memset(g_hash, 0, (size_t)g_hash_size * sizeof *g_hash);
    g_used = 0;
    g_active = 0;
    g_head = g_tail = -1;
    g_free_head = g_free_tail = -1;
    g_nfree = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

/* Add or find under the lock. A foreign record is live at once and never
 * reported; an owned one waits for its first sweep. */
static sh_script_handle add_locked(int mod, uint32_t spawn_id, const char *names, int *full)
{
    sh_script_handle h = 0;
    int i = find_locked(spawn_id);
    if (i >= 0) {
        if (g_e[i].mod == mod || mod == SH_SCRIPT_HANDLE_FOREIGN) h = handle_of(i);
    } else if ((i = take_slot()) < 0) {
        if (full) *full = 1;
    } else {
        entry *e = &g_e[i];
        e->spawn_id = spawn_id;
        e->mod = (int16_t)mod;
        e->state = mod == SH_SCRIPT_HANDLE_FOREIGN ? ST_LIVE : ST_PENDING;
        e->names = names && names[0] ? _strdup(names) : NULL;
        e->next = -1;
        e->prev = g_tail;
        if (g_tail >= 0) g_e[g_tail].next = i; else g_head = i;
        g_tail = i;
        g_active++;
        hash_put(i);
        h = handle_of(i);
    }
    return h;
}

sh_script_handle sh_script_handles_add_named(int mod, uint32_t spawn_id, const char *names, int *full)
{
    sh_script_handle h;
    if (full) *full = 0;
    if (mod < 0 || mod > 0x7FFF || spawn_id == SH_SCRIPT_SPAWN_ID_NONE || spawn_id == SH_SCRIPT_SPAWN_ID_WORLD)
        return 0;
    AcquireSRWLockExclusive(&g_lock);
    h = add_locked(mod, spawn_id, names, full);
    ReleaseSRWLockExclusive(&g_lock);
    return h;
}

sh_script_handle sh_script_handles_add(int mod, uint32_t spawn_id, int *full)
{
    return sh_script_handles_add_named(mod, spawn_id, NULL, full);
}

sh_script_handle sh_script_handles_ref(uint32_t spawn_id)
{
    sh_script_handle h;
    if (spawn_id == SH_SCRIPT_SPAWN_ID_NONE || spawn_id == SH_SCRIPT_SPAWN_ID_WORLD) return 0;
    AcquireSRWLockExclusive(&g_lock);
    h = add_locked(SH_SCRIPT_HANDLE_FOREIGN, spawn_id, NULL, NULL);
    ReleaseSRWLockExclusive(&g_lock);
    return h;
}

int sh_script_handle_names(sh_script_handle h, char *out, size_t cap)
{
    int i, n = -1;
    if (out && cap) out[0] = 0;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0) {
        const char *s = g_e[i].names ? g_e[i].names : "";
        n = (int)strlen(s);
        if (out && cap) strncpy_s(out, cap, s, _TRUNCATE);
    }
    ReleaseSRWLockShared(&g_lock);
    return n;
}

int sh_script_handle_known(sh_script_handle h)
{
    int known;
    AcquireSRWLockShared(&g_lock);
    known = index_of(h) >= 0;
    ReleaseSRWLockShared(&g_lock);
    return known;
}

sh_script_handle sh_script_handles_find(uint32_t spawn_id, int *mod)
{
    sh_script_handle h = 0;
    int i;
    if (mod) *mod = -1;
    AcquireSRWLockShared(&g_lock);
    i = find_locked(spawn_id);
    if (i >= 0) {
        h = handle_of(i);
        if (mod) *mod = g_e[i].mod;
    }
    ReleaseSRWLockShared(&g_lock);
    return h;
}

int sh_script_handle_mod(sh_script_handle h)
{
    int i, mod = -1;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0) mod = g_e[i].mod;
    ReleaseSRWLockShared(&g_lock);
    return mod;
}

uint32_t sh_script_handle_spawn_id(sh_script_handle h)
{
    int i;
    uint32_t id = SH_SCRIPT_SPAWN_ID_NONE;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0) id = g_e[i].spawn_id;
    ReleaseSRWLockShared(&g_lock);
    return id;
}

void *sh_script_handle_entity(sh_script_handle h)
{
    int i;
    void *e = NULL;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0 && g_e[i].state == ST_LIVE && g_resolve) e = g_resolve(g_e[i].spawn_id);
    ReleaseSRWLockShared(&g_lock);
    return e;
}

int sh_script_handle_alive(sh_script_handle h)
{
    int i, alive = 0;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0) alive = g_e[i].state == ST_LIVE;
    ReleaseSRWLockShared(&g_lock);
    return alive;
}

int sh_script_handles_sweep(sh_script_handle_change *out, int cap)
{
    int i, next, n = 0, k, f;
    sh_script_gone_fn fns[GONE_FNS];
    if (!out || cap <= 0) return 0;
    AcquireSRWLockExclusive(&g_lock);
    memcpy(fns, g_gone, sizeof fns);
    for (i = g_head; i >= 0 && n < cap; i = next) {
        entry *e = &g_e[i];
        int present = g_resolve && g_resolve(e->spawn_id) != NULL;
        next = e->next;
        if (present) {
            if (e->state != ST_PENDING) continue;
            e->state = ST_LIVE;
            out[n].kind = SH_SCRIPT_HANDLE_SPAWNED;
        } else {
            out[n].kind = e->mod >= 0 && e->state == ST_LIVE ? SH_SCRIPT_HANDLE_REMOVED : SH_SCRIPT_HANDLE_GONE;
        }
        out[n].mod = e->mod;
        out[n].handle = handle_of(i);
        out[n].spawn_id = e->spawn_id;
        if (out[n].kind != SH_SCRIPT_HANDLE_SPAWNED) release_locked(i);
        n++;
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (k = 0; k < n; k++)
        if (out[k].kind != SH_SCRIPT_HANDLE_SPAWNED)
            for (f = 0; f < GONE_FNS && fns[f]; f++) fns[f](out[k].handle, out[k].spawn_id);
    return n;
}

int sh_script_handles_live(int mod)
{
    int i, n = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = g_head; i >= 0; i = g_e[i].next)
        if (g_e[i].state == ST_LIVE && g_e[i].mod >= 0 && (mod < 0 || g_e[i].mod == mod)) n++;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

int sh_script_handles_count(void)
{
    int n;
    AcquireSRWLockShared(&g_lock);
    n = g_active;
    ReleaseSRWLockShared(&g_lock);
    return n;
}
