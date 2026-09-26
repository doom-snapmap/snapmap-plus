/* Keep SnapMap's editor type-info cache complete for entity types published
 * after it was built. idSnapTypeInfoCache (Vulkan Build 0x1405f2b00, Get
 * 0x1405f2dc0, Find 0x1405f2ff0; OpenGL 0x1405f2700, 0x1405f29c0,
 * 0x1405f2bf0) stores one serialized entity tree per editor definition that
 * names an entityDef, keyed by that entityDef's name pointer and sorted. It is
 * built once, when SnapMap first loads. An editor entity starts from its
 * parent's cached tree and reads defaults such as renderModelInfo.scale from
 * it; a parent published later (a reopened map's private placeables) misses,
 * the engine warns "parent tree not in cache" and the entity starts empty,
 * so its preview scale is zero and its model matrix cannot be inverted.
 *
 * A lookup miss for an entityDef that an editor definition names builds that
 * one tree the way Build does (a temporary entity written into a tree under
 * the process heap) and inserts it in key order; other parents keep the
 * native miss, as Build never caches them. Build itself refuses fatally once the editor
 * definition count reaches its 1500 slots, although only definitions with an
 * entityDef occupy slots; below that the native Build runs unchanged, and at
 * or above it the replacement fills the slots in native definition order and
 * leaves the rest to lookup completion. Both builds share this code apart
 * from call and global displacements, decoded from the verified bytes. */
#include <windows.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snap_type_cache.h"
#include "process_heap_scope.h"
#include "hook.h"

void backend_log(const char *message);

/* Native layout shared by both builds. */
#define STC_CAPACITY 1500          /* Build refuses at this definition count */
#define STC_KEYS 0x8               /* sorted entityDef name pointers */
#define STC_SLOTS 0x2ee8           /* cached trees, 0x5dd * 8 */
#define STC_SLOT 0x48
#define STC_DEFINITION_ENTITYDEF 0x1c8
#define STC_DECL_NAME 0x8
#define STC_PARENT_NAME 0x58       /* entity definition: its parent's name pointer */
#define STC_GAME_GET 0x228         /* game-access object: returns the game */
#define STC_CREATE 0x448           /* game: temporary entity for an entityDef */
#define STC_WRITE 0x378            /* game: write an entity into a tree builder */
#define STC_DESTROY 0x470          /* game: delete that entity */
#define STC_BUILDER 0x1240         /* native frame reserves 0x1210 bytes from +0x50 */
#define STC_ARGS 0x40
#define STC_FAILED 64

typedef void (*stc_build_fn)(void *cache);
typedef void **(*stc_get_fn)(void *cache, void **out, void *decl);
typedef void *(*stc_find_fn)(void *cache, const char **key);
typedef void *(*stc_game_fn)(void *access);
typedef void *(*stc_create_fn)(void *game, void *decl, void *args);
typedef void (*stc_write_fn)(void *game, void *entity, void *builder, unsigned char full);
typedef void (*stc_destroy_fn)(void *game, void *entity);

typedef struct stc_native {
    void **game_access;
    int (*mode)(void);
    void (*mode_init)(void);
    void (*builder_ctor)(void *builder, int zero);
    void (*builder_args)(void *args, int a, int b);
    void (*builder_reset)(void *builder);
    void (*builder_finalize)(void *builder);
    void (*builder_dtor)(void *builder);
    const int *definition_count;
    void *definition_list;
    void *(*definition_at)(void *list, int index);
    void *entity_manager;
    void *(*entity_find)(void *manager, const char *name, int create);
    void (*slot_assign)(void *slot, void *builder);
} stc_native;

static stc_native g;
static sh_process_heap_api g_heap;
static stc_build_fn g_build;
static stc_get_fn g_get;
static stc_find_fn g_find;
static void *g_build_hook, *g_get_hook, *g_find_hook;
static volatile LONG g_active;
/* The editor's own thread runs every cache routine; this guards re-entry from
 * a temporary entity's construction, not concurrency. */
static int g_busy, g_full_reported, g_heap_reported;
static const char *g_failed[STC_FAILED];
static size_t g_failed_next;

static void stc_log(const char *format, const char *name)
{
    char line[512];
    _snprintf_s(line, sizeof(line), _TRUNCATE, format, name ? name : "");
    backend_log(line);
}

static uint64_t *stc_keys(void *cache) { return (uint64_t *)((uint8_t *)cache + STC_KEYS); }
static uint8_t *stc_slot(void *cache, int index) { return (uint8_t *)cache + STC_SLOTS + (size_t)index * STC_SLOT; }
static int stc_key_order(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void *stc_game(void)
{
    void *access = g.game_access ? *g.game_access : NULL;
    return access ? ((stc_game_fn)(*(void ***)access)[STC_GAME_GET / 8])(access) : NULL;
}

/* One native tree for the named entityDef, in the builder Build shares
 * across entries: a temporary entity written into it, then that entity
 * deleted. 1 when the slot now holds the tree, 0 when no such entityDef
 * exists (yet), -1 when the entity could not be created. */
static int stc_fill(void *game, void *builder, void *args, const char *name, void *slot)
{
    void **table = *(void ***)game, *decl, *entity;
    decl = name ? g.entity_find(g.entity_manager, name, 0) : NULL;
    if (!decl) return 0;
    entity = ((stc_create_fn)table[STC_CREATE / 8])(game, decl, args);
    if (!entity) return -1;
    g.builder_reset(builder);
    ((stc_write_fn)table[STC_WRITE / 8])(game, entity, builder, 1);
    ((stc_destroy_fn)table[STC_DESTROY / 8])(game, entity);
    g.builder_finalize(builder);
    g.slot_assign(slot, builder);
    return 1;
}

/* The builder and argument state Build prepares before its loop. */
static int stc_builder_open(void **builder, void **args)
{
    *builder = _aligned_malloc(STC_BUILDER, 16);
    *args = _aligned_malloc(STC_ARGS, 16);
    if (!*builder || !*args) {
        _aligned_free(*builder); _aligned_free(*args); *builder = *args = NULL;
        return 0;
    }
    memset(*builder, 0, STC_BUILDER);
    memset(*args, 0, STC_ARGS);
    g.builder_ctor(*builder, 0);
    g.builder_args(*args, 0, 0);
    memcpy(*builder, *args, 20);
    return 1;
}
static void stc_builder_close(void *builder, void *args)
{
    if (builder) g.builder_dtor(builder);
    _aligned_free(builder);
    _aligned_free(args);
}

static int stc_failed(const char *key)
{
    for (size_t i = 0; i < STC_FAILED; i++) if (g_failed[i] == key) return 1;
    return 0;
}

/* Build caches exactly the entityDefs editor definitions name; any other
 * parent (a stock decal's base, for one) keeps the native miss. A parent
 * found to be no editor type is not rescanned until definitions change. */
static struct { const char *key; int definitions; } g_other[STC_FAILED];
static size_t g_other_next;
static int stc_editor_type(const char *key)
{
    int total = g.definition_count ? *g.definition_count : 0;
    for (size_t i = 0; i < STC_FAILED; i++)
        if (g_other[i].key == key && g_other[i].definitions == total) return 0;
    for (int i = 0; i < total; i++) {
        uint8_t *definition = (uint8_t *)g.definition_at(g.definition_list, i);
        uint8_t *entity = definition ? *(uint8_t **)(definition + STC_DEFINITION_ENTITYDEF) : NULL;
        if (entity && *(const char *const *)(entity + STC_DECL_NAME) == key) return 1;
    }
    g_other[g_other_next % STC_FAILED].key = key;
    g_other[g_other_next++ % STC_FAILED].definitions = total;
    return 0;
}

/* Build the missing parent tree and insert it at its sorted position; the
 * native lookup that follows then finds it. Keys and trees move together;
 * a tree owns only node storage outside its slot, so slots relocate. */
static void stc_complete(void *cache, const char *key)
{
    sh_process_heap_scope scope;
    uint8_t built[STC_SLOT];
    void *game, *builder = NULL, *args = NULL;
    uint64_t *keys;
    int count, low = 0, high, ok = 0;
    if (!cache || !key || g_busy) return;
    count = *(const int *)cache;
    if (count <= 0 || count > STC_CAPACITY) return;
    keys = stc_keys(cache);
    for (high = count; low < high;) {
        int middle = low + (high - low) / 2;
        if (keys[middle] < (uint64_t)(uintptr_t)key) low = middle + 1; else high = middle;
    }
    if (low < count && keys[low] == (uint64_t)(uintptr_t)key) return;
    if (stc_failed(key)) return;
    __try { if (!stc_editor_type(key)) return; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (count == STC_CAPACITY) {
        if (!g_full_reported) stc_log("SNAP-TYPES: the editor type cache is full; %s keeps native editor defaults", key);
        g_full_reported = 1;
        return;
    }
    g_busy = 1;
    if (!sh_process_heap_enter(&g_heap, &scope)) {
        if (!g_heap_reported) stc_log("SNAP-TYPES: process heap scope unavailable; %s keeps native editor defaults", key);
        g_heap_reported = 1;
        g_busy = 0;
        return;
    }
    __try {
        game = stc_game();
        memcpy(built, stc_slot(cache, count), STC_SLOT);
        if (game && stc_builder_open(&builder, &args)) {
            ok = stc_fill(game, builder, args, key, built);
            stc_builder_close(builder, args);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = -1;
        stc_log("SNAP-TYPES: native fault while caching editor defaults for %s", key);
    }
    if (!sh_process_heap_leave(&scope))
        stc_log("SNAP-TYPES: process heap scope was not restored after caching %s", key);
    if (ok > 0) {
        memmove(keys + low + 1, keys + low, (size_t)(count - low) * sizeof(*keys));
        memmove(stc_slot(cache, low + 1), stc_slot(cache, low), (size_t)(count - low) * STC_SLOT);
        keys[low] = (uint64_t)(uintptr_t)key;
        memcpy(stc_slot(cache, low), built, STC_SLOT);
        *(int *)cache = count + 1;
        stc_log("SNAP-TYPES: cached editor defaults for %s", key);
    } else if (ok < 0) {
        /* A definition that exists but yields no entity keeps failing; one
         * that does not exist yet may still be published, so it is retried. */
        g_failed[g_failed_next++ % STC_FAILED] = key;
        stc_log("SNAP-TYPES: %s yields no native entity; it keeps native editor defaults", key);
    }
    g_busy = 0;
}

/* Build at or above the native limit: the entityDef-bearing definitions in
 * native order up to the slot count, sorted, each tree built as Build does.
 * A definition without an entity is skipped rather than fatal. */
static void stc_build_all(void *cache)
{
    uint64_t *keys = stc_keys(cache);
    int total = *g.definition_count, count = 0, extra = 0, built = 0, failed = 0;
    void *game = stc_game(), *builder = NULL, *args = NULL;
    char line[256];
    if (!game) {
        backend_log("SNAP-TYPES: no game for the editor type cache; lookups keep native editor defaults");
        return;
    }
    if (g.mode() == 1 || g.mode() == 0) g.mode_init();
    for (int i = 0; i < total; i++) {
        uint8_t *definition = (uint8_t *)g.definition_at(g.definition_list, i);
        uint8_t *entity = definition ? *(uint8_t **)(definition + STC_DEFINITION_ENTITYDEF) : NULL;
        if (!entity) continue;
        if (count == STC_CAPACITY) { extra++; continue; }
        keys[count++] = *(const uint64_t *)(entity + STC_DECL_NAME);
    }
    qsort(keys, (size_t)count, sizeof(*keys), stc_key_order);
    if (!stc_builder_open(&builder, &args)) {
        backend_log("SNAP-TYPES: editor type cache allocation failed; lookups keep native editor defaults");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (stc_fill(game, builder, args, (const char *)(uintptr_t)keys[i], stc_slot(cache, built)) > 0)
            keys[built++] = keys[i];
        else failed++;
    }
    stc_builder_close(builder, args);
    *(int *)cache = built;
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "SNAP-TYPES: %d editor definitions reach the native limit; cached %d entity types, %d without an entity, %d beyond the slots",
        total, built, failed, extra);
    backend_log(line);
}

static void stc_build(void *cache)
{
    if (!cache || *(const int *)cache || !g.definition_count || *g.definition_count < STC_CAPACITY) {
        g_build(cache);
        return;
    }
    stc_build_all(cache);
}

static void **stc_get(void *cache, void **out, void *decl)
{
    const char *key = NULL;
    if (decl) __try { key = *(const char *const *)((const uint8_t *)decl + STC_PARENT_NAME); }
        __except (EXCEPTION_EXECUTE_HANDLER) { key = NULL; }
    stc_complete(cache, key);
    return g_get(cache, out, decl);
}

static void *stc_find(void *cache, const char **key)
{
    const char *name = NULL;
    if (key) __try { name = *key; } __except (EXCEPTION_EXECUTE_HANDLER) { name = NULL; }
    stc_complete(cache, name);
    return g_find(cache, key);
}

static uint8_t *stc_clean(const sig_result *r, size_t n, const char *name, const uint8_t *base)
{
    for (size_t i = 0; i < n; i++)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK && r[i].addr &&
            r[i].addr == (uintptr_t)base + r[i].rva) return (uint8_t *)r[i].addr;
    return NULL;
}
static void *stc_call(const uint8_t *site) { int32_t rel; memcpy(&rel, site + 1, 4); return (void *)(site + 5 + rel); }
static void *stc_rip(const uint8_t *site, size_t prefix, size_t length)
{ int32_t rel; memcpy(&rel, site + prefix, 4); return (void *)(site + length + rel); }
static int stc_bytes(const uint8_t *code, size_t offset, const char *expected, size_t length)
{ return !memcmp(code + offset, expected, length); }

int sh_snap_type_cache_install(const sig_result *r, size_t n, const uint8_t *base)
{
    uint8_t *build = stc_clean(r, n, "SnapTypeCacheBuild", base), *get = stc_clean(r, n, "SnapTypeCacheGet", base);
    uint8_t *find = stc_clean(r, n, "SnapTypeCacheFind", base);
    if (InterlockedCompareExchange(&g_active, 0, 0)) return 1;
    if (!build || !get || !find || !sh_process_heap_bind(&g_heap, r, n, base)) {
        backend_log("SNAP-TYPES: editor type cache routines not found; runtime entity types keep native editor defaults");
        return 0;
    }
    /* Every instruction the replacements reproduce, decode or hook. */
    if (!stc_bytes(build, 0x00, "\x40\x56\x57\x41\x56\xb8\x70\x12\x00\x00\xe8", 11) ||
        !stc_bytes(build, 0x0f, "\x48\x2b\xe0", 3) || !stc_bytes(build, 0x40, "\x83\x39\x00\x0f\x85", 5) ||
        !stc_bytes(build, 0x49, "\x48\x8b\x0d", 3) ||
        !stc_bytes(build, 0x50, "\x48\x8b\x01\xff\x90\x28\x02\x00\x00", 9) ||
        build[0x6e] != 0xe8 || !stc_bytes(build, 0x73, "\x83\xf8\x01", 3) || build[0x78] != 0xe8 ||
        build[0x81] != 0xe8 || stc_call(build + 0x6e) != stc_call(build + 0x78) ||
        !stc_bytes(build, 0x86, "\x33\xd2\x48\x8d\x4c\x24\x50\xe8", 8) ||
        !stc_bytes(build, 0x93, "\x45\x33\xc0\x33\xd2\x48\x8d\x4c\x24\x20\xe8", 11) ||
        !stc_bytes(build, 0xa2, "\x0f\x10\x44\x24\x20\x0f\x29\x44\x24\x50\x8b\x44\x24\x30\x89\x44\x24\x60", 18) ||
        !stc_bytes(build, 0xb4, "\x8b\x2d", 2) || !stc_bytes(build, 0xba, "\x81\xfd\xdc\x05\x00\x00", 6) ||
        !stc_bytes(build, 0xe0, "\x8b\xd7\x48\x8d\x0d", 5) || build[0xe9] != 0xe8 ||
        !stc_bytes(build, 0xee, "\x48\x8b\x90\xc8\x01\x00\x00", 7) || !stc_bytes(build, 0x101, "\x48\x8b\x42\x08", 4) ||
        !stc_bytes(build, 0x164, "\x45\x33\xc0\x48\x8d\x0d", 6) || build[0x16e] != 0xe8 ||
        !stc_bytes(build, 0x173, "\x4c\x8b\x0e\x4c\x8d\x44\x24\x20\x48\x8b\xd0\x48\x8b\xce\x41\xff\x91\x48\x04\x00\x00", 21) ||
        !stc_bytes(build, 0x190, "\x48\x8d\x4c\x24\x50\xe8", 6) ||
        !stc_bytes(build, 0x19a, "\x4c\x8b\x16\x41\xb1\x01\x4c\x8d\x44\x24\x50\x48\x8b\xd5\x48\x8b\xce\x41\xff\x92\x78\x03\x00\x00", 24) ||
        !stc_bytes(build, 0x1b2, "\x4c\x8b\x06\x48\x8b\xd5\x48\x8b\xce\x41\xff\x90\x70\x04\x00\x00", 16) ||
        !stc_bytes(build, 0x1c2, "\x48\x8d\x4c\x24\x50\xe8", 6) ||
        !stc_bytes(build, 0x1cf, "\x48\x8d\x0c\xc5\xdd\x05\x00\x00", 8) ||
        !stc_bytes(build, 0x1de, "\x48\x8d\x54\x24\x50\xe8", 6) || !stc_bytes(build, 0x20a, "\x48\x8d\x4c\x24\x50\xe8", 6) ||
        !stc_bytes(get, 0x00, "\x40\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\xb8\x90\x12\x00\x00\xe8", 18) ||
        !stc_bytes(get, 0x55, "\x49\x8b\x58\x58", 4) ||
        !stc_bytes(find, 0x00, "\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20\x48\x8b\xfa\x48\x8b\xd9\xe8", 17) ||
        !stc_bytes(find, 0x3b, "\x48\x8d\x80\xdd\x05\x00\x00", 7)) {
        backend_log("SNAP-TYPES: editor type cache routines differ from the verified builds; runtime entity types keep native editor defaults");
        return 0;
    }
    g.game_access = (void **)stc_rip(build + 0x49, 3, 7);
    g.mode = (int (*)(void))stc_call(build + 0x6e);
    g.mode_init = (void (*)(void))stc_call(build + 0x81);
    g.builder_ctor = (void (*)(void *, int))stc_call(build + 0x8d);
    g.builder_args = (void (*)(void *, int, int))stc_call(build + 0x9d);
    g.definition_count = (const int *)stc_rip(build + 0xb4, 2, 6);
    g.definition_list = stc_rip(build + 0xe2, 3, 7);
    g.definition_at = (void *(*)(void *, int))stc_call(build + 0xe9);
    g.entity_manager = stc_rip(build + 0x167, 3, 7);
    g.entity_find = (void *(*)(void *, const char *, int))stc_call(build + 0x16e);
    g.builder_reset = (void (*)(void *))stc_call(build + 0x195);
    g.builder_finalize = (void (*)(void *))stc_call(build + 0x1c7);
    g.slot_assign = (void (*)(void *, void *))stc_call(build + 0x1e3);
    g.builder_dtor = (void (*)(void *))stc_call(build + 0x20f);
    /* Whole position-independent prologues: Build keeps its stack probe call. */
    g_build_hook = hook_prepare_relative_call(build, (void *)stc_build, 18, 10);
    g_get_hook = hook_prepare(get, (void *)stc_get, 17);
    g_find_hook = hook_prepare(find, (void *)stc_find, 16);
    g_build = (stc_build_fn)g_build_hook;
    g_get = (stc_get_fn)g_get_hook;
    g_find = (stc_find_fn)g_find_hook;
    if (g_build_hook && g_get_hook && g_find_hook && hook_commit(g_build_hook) == B2_PATCH_OK &&
        hook_commit(g_get_hook) == B2_PATCH_OK && hook_commit(g_find_hook) == B2_PATCH_OK) {
        InterlockedExchange(&g_active, 1);
        backend_log("SNAP-TYPES: editor type cache completion installed (build, lookup, find)");
        return 1;
    }
    if (g_find_hook && hook_unpatch(g_find_hook)) g_find_hook = NULL;
    if (g_get_hook && hook_unpatch(g_get_hook)) g_get_hook = NULL;
    if (g_build_hook && hook_unpatch(g_build_hook)) g_build_hook = NULL;
    backend_log("SNAP-TYPES: editor type cache replacements could not be installed; runtime entity types keep native editor defaults");
    return 0;
}

int sh_snap_type_cache_active(void) { return InterlockedCompareExchange(&g_active, 0, 0) != 0; }

#ifdef SH_SNAP_TYPE_CACHE_TESTING
void sh_snap_type_cache_test_bind(const sh_snap_type_cache_native *n)
{
    g.game_access = n->game_access; g.mode = n->mode; g.mode_init = n->mode_init;
    g.builder_ctor = n->builder_ctor; g.builder_args = n->builder_args; g.builder_reset = n->builder_reset;
    g.builder_finalize = n->builder_finalize; g.builder_dtor = n->builder_dtor;
    g.definition_count = n->definition_count; g.definition_list = n->definition_list;
    g.definition_at = n->definition_at; g.entity_manager = n->entity_manager; g.entity_find = n->entity_find;
    g.slot_assign = n->slot_assign;
    g_build = n->native_build; g_get = n->native_get; g_find = n->native_find;
    g_heap.get = n->heap_get; g_heap.push = n->heap_push; g_heap.pop = n->heap_pop;
    memset((void *)g_failed, 0, sizeof(g_failed));
    memset(g_other, 0, sizeof(g_other));
    g_failed_next = g_other_next = 0; g_busy = g_full_reported = g_heap_reported = 0;
}
void sh_snap_type_cache_test_build(void *cache) { stc_build(cache); }
void **sh_snap_type_cache_test_get(void *cache, void **out, void *decl) { return stc_get(cache, out, decl); }
void *sh_snap_type_cache_test_find(void *cache, const char **key) { return stc_find(cache, key); }
#endif
