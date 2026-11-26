/* For Each AI can also visit the named actors actually placed in this map. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "direct_actor_iter.h"

#ifndef SH_DIRECT_ITER_TESTING
#include "companion_engine.h"
#include "engine_build.h"
#include "entity.h"
#include "script/script_hooks.h"
#include "script/script_handles.h"
#endif

typedef struct direct_place {
    char *name, *inherit;
    sh_direct_lifetime actor;
} direct_place;

static SRWLOCK g_lock = SRWLOCK_INIT;
static direct_place *g_places;
static size_t g_count, g_capacity;
static unsigned long long g_map;
static volatile LONG g_ready, g_pending;
static volatile LONG g_has_places;
static void (*g_original)(void *);
static void (*g_fire)(void *, unsigned, void *);
static int (*g_identity)(void *, int, sh_direct_lifetime *);
static int (*g_current)(const sh_direct_lifetime *);
static int (*g_living)(const sh_direct_lifetime *);
static int (*g_tracked)(const sh_direct_lifetime *);

int sh_direct_iter_awaiting_spawn(void) { return g_pending > 0; }

void sh_direct_iter_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_count; i++) {
        free(g_places[i].name);
        free(g_places[i].inherit);
    }
    free(g_places);
    g_places = NULL;
    g_capacity = g_count = 0;
    g_map++;
    InterlockedExchange(&g_pending, 0);
    InterlockedExchange(&g_has_places, 0);
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_direct_iter_place(const char *name, const char *inherit)
{
    direct_place row = {0}, *larger;
    int first = 0;
    if (!g_ready || !name || !*name || !inherit || !*inherit) return;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_count; i++)
        if (!_stricmp(g_places[i].name, name)) goto done;
    row.name = _strdup(name);
    row.inherit = _strdup(inherit);
    if (!row.name || !row.inherit) goto done;
    if (g_count == g_capacity) {
        size_t cap = g_capacity ? g_capacity * 2 : 8;
        if (cap > SIZE_MAX / sizeof *larger || cap < g_capacity) goto done;
        larger = realloc(g_places, cap * sizeof *larger);
        if (!larger) goto done;
        g_places = larger;
        g_capacity = cap;
    }
    first = !g_count;
    g_places[g_count++] = row;
    InterlockedIncrement(&g_pending);
    InterlockedExchange(&g_has_places, 1);
    row.name = row.inherit = NULL;
done:
    ReleaseSRWLockExclusive(&g_lock);
    free(row.name);
    free(row.inherit);
#ifndef SH_DIRECT_ITER_TESTING
    if (first) (void)sh_companions_ensure("direct actor placement");
#else
    (void)first;
#endif
}

void sh_direct_iter_after_spawn(void *entity, const char *name, const char *inherit)
{
    if (!g_pending || !name || !inherit) return;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_count; i++) {
        direct_place *p = &g_places[i];
        if (!p->actor.entity && !_stricmp(p->name, name) && !_stricmp(p->inherit, inherit)) {
            sh_direct_lifetime fresh = {0};
            if (g_identity(entity, 1, &fresh)) {
                p->actor = fresh;
                InterlockedDecrement(&g_pending);
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static int same_map(unsigned long long map)
{
    unsigned long long current;
    AcquireSRWLockShared(&g_lock);
    current = g_map;
    ReleaseSRWLockShared(&g_lock);
    return current == map;
}

static void direct_iterate(void *iterator)
{
    sh_direct_lifetime caller = {0}, *actors = NULL;
    size_t n = 0;
    unsigned long long map;
    if (!g_has_places || !g_identity(iterator, 0, &caller)) {
        g_original(iterator);
        return;
    }
    AcquireSRWLockShared(&g_lock);
    map = g_map;
    if (g_count) actors = calloc(g_count, sizeof *actors);
    if (actors) {
        for (size_t i = 0; i < g_count; i++) {
            const sh_direct_lifetime *a = &g_places[i].actor;
            if (a->entity) actors[n++] = *a;
        }
    }
    ReleaseSRWLockShared(&g_lock);

    /* A normal spawn request registers its own actor. Keep every native event first, in its original order. */
    __try {
        for (size_t i = 0; i < n; i++)
            if (!g_living(&actors[i]) || g_tracked(&actors[i]) != 0) actors[i].entity = NULL;
        g_original(iterator);
        for (size_t i = 0; i < n && same_map(map) && g_current(&caller); i++)
            if (actors[i].entity && g_living(&actors[i]) && g_tracked(&actors[i]) == 0)
                g_fire(iterator, 0x2b, actors[i].entity);
    } __finally { free(actors); }
}

#ifdef SH_DIRECT_ITER_TESTING
void sh_direct_iter_test_bind(const sh_direct_iter_ops *ops)
{
    sh_direct_iter_reset();
    g_original = ops->original;
    g_fire = ops->fire;
    g_identity = ops->identity;
    g_current = ops->current;
    g_living = ops->living;
    g_tracked = ops->tracked;
    g_ready = 1;
}
void sh_direct_iter_test_run(void *it) { direct_iterate(it); }
#else
static void *(*g_as_ai)(void *);
static unsigned char (*g_has_director)(void *);
static void *(*g_director)(void *);

static int native_identity(void *entity, int require_ai, sh_direct_lifetime *out)
{
    const uint8_t *world = sh_entity_gamemgr();
    if (!entity || !world) return 0;
    __try {
        int slot = *(const int *)((const uint8_t *)entity + SH_SCRIPT_ENT_NUMBER);
        if (slot < 0 || slot >= SH_SCRIPT_ENT_NUMBER_WORLD ||
            *(void *const *)(world + 0x453f8 + (size_t)slot * sizeof(void *)) != entity || (require_ai && !g_as_ai(entity))) return 0;
        out->entity = entity;
        out->world = world;
        out->slot = slot;
        out->generation = *(const uint32_t *)(world + SH_SCRIPT_GAME_SPAWN_IDS + (size_t)slot * 4);
        return out->generation && out->generation != SH_SCRIPT_SPAWN_ID_NONE && out->generation != SH_SCRIPT_SPAWN_ID_WORLD;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int native_current(const sh_direct_lifetime *actor)
{
    const uint8_t *world = sh_entity_gamemgr();
    size_t slot = (size_t)actor->slot;
    if (!world || world != actor->world || slot >= SH_SCRIPT_ENT_NUMBER_WORLD) return 0;
    __try {
        return *(void *const *)(world + 0x453f8 + slot * sizeof(void *)) == actor->entity &&
               *(const uint32_t *)(world + SH_SCRIPT_GAME_SPAWN_IDS + slot * 4) == actor->generation;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int native_living(const sh_direct_lifetime *actor)
{
    if (!native_current(actor)) return 0;
    __try {
        typedef unsigned char (*state_fn)(void *);
        void **vt = *(void ***)actor->entity;
        return !((state_fn)vt[0x6c8 / 8])(actor->entity) && !((state_fn)vt[0x6d0 / 8])(actor->entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* A world without a director has no native recipients. An unreadable tracker has unknown membership. */
static int native_tracked(const sh_direct_lifetime *actor)
{
    void *world = sh_entity_gamemgr();
    if (!world) return -1;
    __try {
        const uint8_t *d, *rows;
        int count;
        if (!g_has_director(world)) return 0;
        d = g_director(world);
        if (!d) return -1;
        rows = *(const uint8_t *const *)(d + 0xd08);
        count = *(const int *)(d + 0xd10);
        if (count < 0 || count > 76 || (count && !rows)) return -1;
        for (int i = 0; i < count; i++) {
            const uint32_t *ref = (const uint32_t *)(rows + (size_t)i * 64 + 24);
            if (ref[0] == actor->generation || ref[1] == actor->generation) return 1;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static const uint8_t *callee(const uint8_t *call)
{
    int32_t delta;
    if (*call != 0xe8) return NULL;
    memcpy(&delta, call + 1, sizeof delta);
    return call + 5 + delta;
}

int sh_direct_iter_install(const sig_result *results, size_t count, const uint8_t *image)
{
    const uint8_t *iterate = NULL, *fire;
    void **slot;
    DWORD old, ignored;
    uint32_t table = SH_RVA(0x24ad6a0u, 0x24c73f0u);
    uint32_t as_ai = SH_RVA(0x14adc40u, 0x14adcd0u);
    if (g_ready) return 1;
    if (!image || !table || !as_ai) return 0;
    for (size_t i = 0; i < count; i++)
        if (results[i].status == SIG_OK && !strcmp(results[i].name, "SnapMapIterateAI")) iterate = (const uint8_t *)results[i].addr;
    if (!iterate) return 0;
    slot = (void **)(image + table + 0x8e8);
    if (*slot != iterate || memcmp(iterate + 0x94, "\xba\x2b\x00\x00\x00\x49\x8b\xce", 8)) return 0;
    fire = callee(iterate + 0x9c);
    g_has_director = (void *)callee(iterate + 0x10);
    g_director = (void *)callee(iterate + 0x2e);
    if (!fire || !g_has_director || !g_director) return 0;
    g_as_ai = (void *)(image + as_ai);
    g_fire = (void *)fire;
    g_identity = native_identity;
    g_current = native_current;
    g_living = native_living;
    g_tracked = native_tracked;
    g_original = (void *)iterate;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    void *previous = InterlockedCompareExchangePointer(slot, (void *)direct_iterate, (void *)iterate);
    VirtualProtect(slot, sizeof *slot, old, &ignored);
    if (previous != iterate) return 0;
    InterlockedExchange(&g_ready, 1);
    return 1;
}
#endif
