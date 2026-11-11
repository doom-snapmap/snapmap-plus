/* script_handles.c -- see script_handles.h. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "script_handles.h"

#define HASH_SLOTS   (SH_SCRIPT_HANDLES_MAX * 2)   /* power of two, half full at most */
#define EPOCH_SHIFT  24
#define INDEX_MASK   0x00FFFFFFu

enum { ST_FREE = 0, ST_PENDING, ST_LIVE, ST_DEAD };

typedef struct entry {
    uint32_t spawn_id;
    int16_t  mod;            /* SH_SCRIPT_HANDLE_FOREIGN for a reference no mod owns */
    uint8_t  state;
    char    *names;          /* malloc'd entityDef chain, or NULL */
} entry;

static entry    g_entries[SH_SCRIPT_HANDLES_MAX];
static int32_t  g_hash[HASH_SLOTS];          /* index + 1, 0 = empty */
static volatile LONG g_count;                 /* entries used this map */
static volatile LONG g_epoch = 1;
static SRWLOCK  g_lock = SRWLOCK_INIT;
static sh_script_resolve_fn g_resolve;

static uint32_t hash_of(uint32_t id)
{
    id ^= id >> 16;
    id *= 0x7FEB352Du;
    id ^= id >> 15;
    return id & (HASH_SLOTS - 1);
}

/* The entry index for a handle of this map, or -1. Lock held by the caller. */
static int index_of(sh_script_handle h)
{
    uint32_t idx;
    if (!h || (h >> EPOCH_SHIFT) != (uint32_t)g_epoch) return -1;
    idx = (h & INDEX_MASK) - 1;
    return idx < (uint32_t)g_count ? (int)idx : -1;
}

static sh_script_handle handle_of(int index)
{
    return ((uint32_t)g_epoch << EPOCH_SHIFT) | (uint32_t)(index + 1);
}

static int find_locked(uint32_t spawn_id)
{
    uint32_t s = hash_of(spawn_id);
    for (;;) {
        int32_t v = g_hash[s];
        if (!v) return -1;
        if (g_entries[v - 1].spawn_id == spawn_id) return v - 1;
        s = (s + 1) & (HASH_SLOTS - 1);
    }
}

void sh_script_handles_set_resolver(sh_script_resolve_fn fn)
{
    AcquireSRWLockExclusive(&g_lock);
    g_resolve = fn;
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_script_handles_reset(void)
{
    LONG e;
    int i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < (int)g_count; i++) free(g_entries[i].names);
    memset(g_entries, 0, sizeof g_entries);
    memset(g_hash, 0, sizeof g_hash);
    g_count = 0;
    e = g_epoch + 1;
    if (e > 0xFF) e = 1;
    g_epoch = e;
    ReleaseSRWLockExclusive(&g_lock);
}

/* Add or find under the lock. A foreign record is live at once and never
 * reported; an owned one waits for its first sweep. */
static sh_script_handle add_locked(int mod, uint32_t spawn_id, const char *names, int *full)
{
    sh_script_handle h = 0;
    int i = find_locked(spawn_id);
    if (i >= 0) {
        if (g_entries[i].mod == mod || mod == SH_SCRIPT_HANDLE_FOREIGN) h = handle_of(i);
    } else if (g_count >= SH_SCRIPT_HANDLES_MAX) {
        if (full) *full = 1;
    } else {
        uint32_t s = hash_of(spawn_id);
        i = (int)g_count;
        g_entries[i].spawn_id = spawn_id;
        g_entries[i].mod = (int16_t)mod;
        g_entries[i].state = mod == SH_SCRIPT_HANDLE_FOREIGN ? ST_LIVE : ST_PENDING;
        g_entries[i].names = names && names[0] ? _strdup(names) : NULL;
        while (g_hash[s]) s = (s + 1) & (HASH_SLOTS - 1);
        g_hash[s] = i + 1;
        g_count = i + 1;
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
        const char *s = g_entries[i].names ? g_entries[i].names : "";
        n = (int)strlen(s);
        if (out && cap) strncpy_s(out, cap, s, _TRUNCATE);
    }
    ReleaseSRWLockShared(&g_lock);
    return n;
}

int sh_script_handle_known(sh_script_handle h)
{
    int i, known = 0;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0) known = g_entries[i].state == ST_PENDING || g_entries[i].state == ST_LIVE;
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
        if (mod) *mod = g_entries[i].mod;
    }
    ReleaseSRWLockShared(&g_lock);
    return h;
}

int sh_script_handle_mod(sh_script_handle h)
{
    int i, mod = -1;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0) mod = g_entries[i].mod;
    ReleaseSRWLockShared(&g_lock);
    return mod;
}

uint32_t sh_script_handle_spawn_id(sh_script_handle h)
{
    int i;
    uint32_t id = SH_SCRIPT_SPAWN_ID_NONE;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0) id = g_entries[i].spawn_id;
    ReleaseSRWLockShared(&g_lock);
    return id;
}

void *sh_script_handle_entity(sh_script_handle h)
{
    int i;
    void *e = NULL;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0 && g_entries[i].state == ST_LIVE && g_resolve) e = g_resolve(g_entries[i].spawn_id);
    ReleaseSRWLockShared(&g_lock);
    return e;
}

int sh_script_handle_alive(sh_script_handle h)
{
    int i, alive = 0;
    AcquireSRWLockShared(&g_lock);
    i = index_of(h);
    if (i >= 0) alive = g_entries[i].state == ST_LIVE;
    ReleaseSRWLockShared(&g_lock);
    return alive;
}

int sh_script_handles_sweep(sh_script_handle_change *out, int cap)
{
    int i, n = 0;
    if (!out || cap <= 0) return 0;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < (int)g_count && n < cap; i++) {
        entry *e = &g_entries[i];
        int present;
        if (e->state != ST_PENDING && e->state != ST_LIVE) continue;
        if (e->mod < 0) continue;               /* a foreign reference: resolved at each use, never reported */
        present = g_resolve && g_resolve(e->spawn_id) != NULL;
        if (e->state == ST_PENDING) {
            if (!present) { e->state = ST_DEAD; continue; }
            e->state = ST_LIVE;
            out[n].kind = SH_SCRIPT_HANDLE_SPAWNED;
        } else {
            if (present) continue;
            e->state = ST_DEAD;
            out[n].kind = SH_SCRIPT_HANDLE_REMOVED;
        }
        out[n].mod = e->mod;
        out[n].handle = handle_of(i);
        n++;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}

int sh_script_handles_live(int mod)
{
    int i, n = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < (int)g_count; i++)
        if (g_entries[i].state == ST_LIVE && g_entries[i].mod >= 0 && (mod < 0 || g_entries[i].mod == mod)) n++;
    ReleaseSRWLockShared(&g_lock);
    return n;
}
