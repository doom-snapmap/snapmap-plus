/* modtypeinfo.c -- see modtypeinfo.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modtypeinfo.h"

int sh_typeinfo_cache_index(const void *cache, const void *key)
{
    const unsigned char *c = (const unsigned char *)cache;
    const uintptr_t *keys;
    uintptr_t want = (uintptr_t)key;
    int n, lo = 0, hi;
    if (!c || !key) return -1;
    n = *(const int *)(c + SH_TYPEINFO_COUNT_OFF);
    if (n <= 0 || n > SH_TYPEINFO_MAX_TYPES) return -1;
    keys = (const uintptr_t *)(c + SH_TYPEINFO_KEYS_OFF);
    hi = n;
    while (lo < hi) {   /* lower bound, as the game searches */
        int mid = lo + (hi - lo) / 2;
        if (keys[mid] < want) lo = mid + 1; else hi = mid;
    }
    return (lo < n && keys[lo] == want) ? lo : -1;
}

void sh_typeinfo_view_fill(void *view, const void *key, const void *tree)
{
    unsigned char *v = (unsigned char *)view;
    if (!v) return;
    memset(v, 0, SH_TYPEINFO_KEYS_OFF + 2 * sizeof(void *));
    *(int *)(v + SH_TYPEINFO_COUNT_OFF) = 1;
    *(const void **)(v + SH_TYPEINFO_KEYS_OFF) = key;
    if (tree) memcpy(v + SH_TYPEINFO_TREES_OFF, tree, SH_TYPEINFO_TREE_SIZE);
    else memset(v + SH_TYPEINFO_TREES_OFF, 0, SH_TYPEINFO_TREE_SIZE);
}

/* Index of the first entry whose key is not below `key`. */
static int tt_lower(const sh_typeinfo_table *t, const void *key)
{
    int lo = 0, hi = t->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if ((uintptr_t)t->entries[mid].key < (uintptr_t)key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

void *sh_typeinfo_table_find(const sh_typeinfo_table *t, const void *key)
{
    int i;
    if (!t || !t->entries || !key) return NULL;
    i = tt_lower(t, key);
    return (i < t->count && t->entries[i].key == key) ? t->entries[i].tree : NULL;
}

int sh_typeinfo_table_insert(sh_typeinfo_table *t, const void *key, void *tree)
{
    int i;
    if (!t || !key || !tree) return 0;
    i = t->entries ? tt_lower(t, key) : 0;
    if (i < t->count && t->entries[i].key == key) return 0;
    if (t->count == t->capacity) {
        int cap = t->capacity ? t->capacity * 2 : 64;
        sh_typeinfo_entry *e = (sh_typeinfo_entry *)realloc(t->entries, (size_t)cap * sizeof *e);
        if (!e) return 0;
        t->entries = e;
        t->capacity = cap;
    }
    memmove(t->entries + i + 1, t->entries + i, (size_t)(t->count - i) * sizeof *t->entries);
    t->entries[i].key = key;
    t->entries[i].tree = tree;
    t->count++;
    return 1;
}

static void *tt_take(sh_typeinfo_table *t, int i)
{
    void *tree = t->entries[i].tree;
    memmove(t->entries + i, t->entries + i + 1, (size_t)(t->count - i - 1) * sizeof *t->entries);
    t->count--;
    return tree;
}

void *sh_typeinfo_table_remove(sh_typeinfo_table *t, const void *key)
{
    int i;
    if (!t || !t->entries || !key) return NULL;
    i = tt_lower(t, key);
    return (i < t->count && t->entries[i].key == key) ? tt_take(t, i) : NULL;
}

int sh_typeinfo_table_remove_name(sh_typeinfo_table *t, const char *name,
                                  void (*destroy)(void *context, void *tree), void *context)
{
    int i, removed = 0;
    if (!t || !t->entries || !name || !name[0]) return 0;
    for (i = 0; i < t->count;) {
        if (!_stricmp((const char *)t->entries[i].key, name)) {
            void *tree = tt_take(t, i);
            if (destroy) destroy(context, tree);
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

void sh_typeinfo_table_free(sh_typeinfo_table *t, void (*destroy)(void *context, void *tree), void *context)
{
    int i;
    if (!t) return;
    if (destroy) for (i = 0; i < t->count; i++) destroy(context, t->entries[i].tree);
    free(t->entries);
    t->entries = NULL;
    t->count = t->capacity = 0;
}

#ifndef SH_MODTYPEINFO_PURE
#include "signatures.h"
#include "engine_globals.h"
#include "backend_log.h"
#include "patch.h"
#include "process_heap_scope.h"
#include "modserve.h"

/* The entity palette (editor+0x20660) as modserve_engine.c and timeline_tile.c
 * read it: entries at +0x10 of 0x6f0 bytes, count at +0x18; each entry starts
 * with its sedef, whose name is at +8 and whose entityDef resource at +0x1c8;
 * the cache key is that resource's name at +8. */
#define PAL_ENTRIES_OFF     0x10
#define PAL_COUNT_OFF       0x18
#define PAL_STRIDE          0x6f0
#define PAL_MAX             100000
#define SEDEF_NAME_OFF      0x08
#define SEDEF_ENTITYDEF_OFF 0x1c8
#define RESOURCE_NAME_OFF   0x08
/* idSnapEntityEdit: the pooled entityDef name Get looks up. */
#define ENTITY_KEY_OFF      0x58
/* The reflection validator's game accessor (vtable +0x240, what Build calls)
 * and the game's temporary-entity API (idGameLocal vtable). */
#define VSLOT_GAME          0x240
#define GSLOT_CREATE        0x448   /* entity *(game, decl, const state[0x14]) */
#define GSLOT_WRITE_STATE   0x378   /* void(game, entity, text, 1) */
#define GSLOT_DESTROY       0x470   /* void(game, entity) */
/* The text context Build writes a default entity into: 0x1210 bytes, its
 * 0x14-byte state header first, a memory writer at +0x1018, the text at
 * +0x1020. Allocated with headroom. */
#define TT_BYTES            0x1400
#define TT_STATE_BYTES      0x14
/* A tree's allocator fields the engine's copy constructor sets. */
#define TREE_GRAIN_OFF      0x20    /* u16 1 */
#define TREE_TAG_OFF        0x38    /* u32 0x930000 */
#define TREE_OWNED_OFF      0x40    /* u8: 1 on a tree Get hands out */
/* A tree node: five idStr (len +8, data +0x10) at 0, 0x30, 0x60, 0x90, 0xc0;
 * next sibling +0xf0, first child +0xf8, kind +0x108. */
#define NODE_NEXT_OFF       0xf0
#define NODE_CHILD_OFF      0xf8
#define NODE_KIND_OFF       0x108
#define NODE_STR_COUNT      5
#define NODE_STR_STRIDE     0x30
#define IDSTR_LEN_OFF       0x08
#define IDSTR_DATA_OFF      0x10
#define NODES_MAX           200000
#define SELF_CHECK_TYPES    3
#define GET_STOLEN          17      /* seven pushes and mov eax,0x1290; the stack probe call follows */
#define LOOKUP_STOLEN       16      /* up to the call of the search */
#define LOG_LINES_MAX       48

typedef void *(*ti_get_fn)(void *cache, void **out, void *entity);
typedef void *(*ti_lookup_fn)(void *cache, void **keyp);
typedef void *(*ti_tt_ctor_fn)(void *obj, int arg);
typedef void *(*ti_tt_state_fn)(void *state, unsigned char a, unsigned char b);
typedef unsigned char (*ti_tt_open_fn)(void *obj);
typedef unsigned char (*ti_tt_finish_fn)(void *obj);
typedef void (*ti_tt_dtor_fn)(void *obj);
typedef void (*ti_tree_text_fn)(void *tree, void *obj);
typedef void (*ti_tree_destroy_fn)(void *tree);
typedef void *(*ti_decl_find_fn)(void *ctx, const void *name, unsigned char flag);
typedef void *(*ti_game_fn)(void *validator);
typedef void *(*ti_create_fn)(void *game, void *decl, void *state);
typedef void (*ti_write_fn)(void *game, void *entity, void *obj, int flag);
typedef void (*ti_destroy_fn)(void *game, void *entity);

static unsigned char *g_cache;
static const int *g_rows;
static const uint32_t *g_main_thread_at;
static void *g_res_ctx;
static const void *g_validator_at;
static ti_tt_ctor_fn g_tt_ctor;
static ti_tt_state_fn g_tt_state;
static ti_tt_open_fn g_tt_open;
static ti_tt_finish_fn g_tt_finish;
static ti_tt_dtor_fn g_tt_dtor;
static ti_tree_text_fn g_tree_text;
static ti_tree_destroy_fn g_tree_destroy;
static ti_decl_find_fn g_decl_find;
static sh_process_heap_api g_heap;
static ti_get_fn g_get;          /* the trampolines: Get and the lookup as the game wrote them */
static ti_lookup_fn g_lookup;
static int g_installed;          /* both detours committed */
static int g_checked, g_ok;      /* the boot self-check ran; it passed (the table serves) */
static int g_depth;              /* inside the product's builder: the game's own path answers */
static sh_typeinfo_table g_table;
static long g_served, g_built, g_failed;
static int g_reported_mods, g_reported_missing;
static int g_log_lines;

static void ti_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    if (g_log_lines >= LOG_LINES_MAX) return;
    g_log_lines++;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static int ti_av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static int ti_rd_ptr(const void *at, void **out)
{
    __try { *out = *(void *const *)at; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = NULL; return 0; }
}

static int ti_on_main(void)
{
    uint32_t id = 0;
    if (!g_main_thread_at) return -1;
    __try { id = *g_main_thread_at; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!id) return -1;
    return id == GetCurrentThreadId() ? 1 : 0;
}

static int ti_cache_count(void)
{
    int n = 0;
    __try { n = *(const int *)(g_cache + SH_TYPEINFO_COUNT_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = -1; }
    return n;
}

/* A tree the way the engine's copy constructor starts one: no root, an empty
 * node allocator with the engine's grain and tag. */
static void *ti_tree_new(void)
{
    unsigned char *t = (unsigned char *)calloc(1, SH_TYPEINFO_TREE_SIZE);
    if (!t) return NULL;
    *(unsigned short *)(t + TREE_GRAIN_OFF) = 1;
    *(unsigned int *)(t + TREE_TAG_OFF) = 0x930000;
    return t;
}

static void ti_tree_free(void *context, void *tree)
{
    (void)context;
    if (!tree) return;
    __try { g_tree_destroy(tree); }
    __except (ti_av_only(GetExceptionCode())) { }
    free(tree);
}

static void *ti_game(void)
{
    void *validator = NULL, *vtbl = NULL, *fn = NULL;
    if (!g_validator_at || !ti_rd_ptr(g_validator_at, &validator) || !validator) return NULL;
    if (!ti_rd_ptr(validator, &vtbl) || !vtbl) return NULL;
    if (!ti_rd_ptr((unsigned char *)vtbl + VSLOT_GAME, &fn) || !fn) return NULL;
    return ((ti_game_fn)fn)(validator);
}

static void *ti_game_slot(void *game, size_t slot)
{
    void *vtbl = NULL, *fn = NULL;
    if (!game || !ti_rd_ptr(game, &vtbl) || !vtbl) return NULL;
    if (!ti_rd_ptr((unsigned char *)vtbl + slot, &fn)) return NULL;
    return fn;
}

/* Build the default tree of the entityDef `key` names, as idSnapTypeInfoCache::Build
 * builds each entry: create the type's default entity through the game, write
 * its state to text, destroy it, parse the text. Inside process heap 0, so the
 * tree's nodes outlive every map. `why` names the failure. */
static void *ti_build(const void *key, char *why, size_t cap)
{
    sh_process_heap_scope heap;
    unsigned char state[0x20];
    void *decl = NULL, *game = NULL, *entity = NULL, *obj = NULL, *tree = NULL;
    ti_create_fn create;
    ti_write_fn write;
    ti_destroy_fn destroy;
    int ok = 0;
    why[0] = '\0';
    if (!sh_process_heap_enter(&g_heap, &heap)) {
        _snprintf_s(why, cap, _TRUNCATE, "process heap scope unavailable");
        return NULL;
    }
    g_depth++;
    __try {
        decl = g_decl_find(g_res_ctx, key, 0);
        game = decl ? ti_game() : NULL;
        create = (ti_create_fn)ti_game_slot(game, GSLOT_CREATE);
        write = (ti_write_fn)ti_game_slot(game, GSLOT_WRITE_STATE);
        destroy = (ti_destroy_fn)ti_game_slot(game, GSLOT_DESTROY);
        if (!decl) _snprintf_s(why, cap, _TRUNCATE, "the game has no entityDef of that name");
        else if (!game || !create || !write || !destroy) _snprintf_s(why, cap, _TRUNCATE, "the game object is unavailable");
        else {
            memset(state, 0, sizeof state);
            g_tt_state(state, 0, 0);
            entity = create(game, decl, state);
            if (!entity) _snprintf_s(why, cap, _TRUNCATE, "the game did not create a default entity of that type");
            else {
                obj = calloc(1, TT_BYTES);
                tree = ti_tree_new();
                if (!obj || !tree) _snprintf_s(why, cap, _TRUNCATE, "out of memory");
                else {
                    g_tt_ctor(obj, 0);
                    memcpy(obj, state, TT_STATE_BYTES);
                    g_tt_open(obj);
                    write(game, entity, obj, 1);
                    destroy(game, entity);
                    entity = NULL;
                    g_tt_finish(obj);
                    g_tree_text(tree, obj);
                    ok = 1;
                }
                if (entity) { destroy(game, entity); entity = NULL; }
            }
        }
    } __except (ti_av_only(GetExceptionCode())) {
        _snprintf_s(why, cap, _TRUNCATE, "access violation while building it (%s)",
                    obj ? "writing or parsing the entity's state" : entity ? "creating the entity" : "finding the decl");
        ok = 0;
    }
    if (obj) {
        __try { g_tt_dtor(obj); }
        __except (ti_av_only(GetExceptionCode())) { }
        free(obj);
    }
    if (!ok && tree) { ti_tree_free(NULL, tree); tree = NULL; }
    g_depth--;
    if (!sh_process_heap_leave(&heap)) ti_log("MODS: typeinfo: the process heap scope did not balance after a build");
    return tree;
}

/* ----------------------------------------------------------- self-check */

static int ti_str_eq(const unsigned char *a, const unsigned char *b)
{
    int la = *(const int *)(a + IDSTR_LEN_OFF), lb = *(const int *)(b + IDSTR_LEN_OFF);
    const char *da = *(const char *const *)(a + IDSTR_DATA_OFF), *db = *(const char *const *)(b + IDSTR_DATA_OFF);
    if (la != lb) return 0;
    if (la <= 0) return 1;
    return da && db && !memcmp(da, db, (size_t)la);
}

/* Compare two trees by content: every node's five strings and kind, the
 * children in order, and the header fields the engine's constructor sets.
 * Pointers (nodes, the allocator's blocks) are the product's own. */
static int ti_nodes_eq(const unsigned char *a, const unsigned char *b, int *nodes, char *why, size_t cap)
{
    while (a || b) {
        const unsigned char *ca, *cb;
        int k;
        if (!a || !b) { _snprintf_s(why, cap, _TRUNCATE, "node count differs"); return 0; }
        if (++*nodes > NODES_MAX) { _snprintf_s(why, cap, _TRUNCATE, "more than %d nodes", NODES_MAX); return 0; }
        for (k = 0; k < NODE_STR_COUNT; k++)
            if (!ti_str_eq(a + k * NODE_STR_STRIDE, b + k * NODE_STR_STRIDE)) {
                const char *na = *(const char *const *)(a + IDSTR_DATA_OFF);
                _snprintf_s(why, cap, _TRUNCATE, "node '%s' string %d differs", na ? na : "", k);
                return 0;
            }
        if (*(const unsigned short *)(a + NODE_KIND_OFF) != *(const unsigned short *)(b + NODE_KIND_OFF)) {
            const char *na = *(const char *const *)(a + IDSTR_DATA_OFF);
            _snprintf_s(why, cap, _TRUNCATE, "node '%s' kind 0x%x vs 0x%x", na ? na : "",
                        *(const unsigned short *)(a + NODE_KIND_OFF), *(const unsigned short *)(b + NODE_KIND_OFF));
            return 0;
        }
        ca = *(const unsigned char *const *)(a + NODE_CHILD_OFF);
        cb = *(const unsigned char *const *)(b + NODE_CHILD_OFF);
        if (!ti_nodes_eq(ca, cb, nodes, why, cap)) return 0;
        a = *(const unsigned char *const *)(a + NODE_NEXT_OFF);
        b = *(const unsigned char *const *)(b + NODE_NEXT_OFF);
    }
    return 1;
}

static int ti_tree_eq(const void *mine, const void *game, int *nodes, char *why, size_t cap)
{
    const unsigned char *a = (const unsigned char *)mine, *b = (const unsigned char *)game;
    *nodes = 0;
    why[0] = '\0';
    __try {
        if (*(const unsigned short *)(a + TREE_GRAIN_OFF) != *(const unsigned short *)(b + TREE_GRAIN_OFF) ||
            *(const unsigned int *)(a + TREE_TAG_OFF) != *(const unsigned int *)(b + TREE_TAG_OFF) ||
            a[TREE_OWNED_OFF] != b[TREE_OWNED_OFF]) {
            _snprintf_s(why, cap, _TRUNCATE, "header differs (grain %u/%u, tag 0x%x/0x%x, owned %u/%u)",
                        *(const unsigned short *)(a + TREE_GRAIN_OFF), *(const unsigned short *)(b + TREE_GRAIN_OFF),
                        *(const unsigned int *)(a + TREE_TAG_OFF), *(const unsigned int *)(b + TREE_TAG_OFF),
                        a[TREE_OWNED_OFF], b[TREE_OWNED_OFF]);
            return 0;
        }
        return ti_nodes_eq(*(const unsigned char *const *)a, *(const unsigned char *const *)b, nodes, why, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(why, cap, _TRUNCATE, "fault while comparing");
        return 0;
    }
}

/* Once per boot, when the game's cache has entries: build stock types the
 * product's way and compare them with the game's entries. The table serves
 * only after this passes. Returns g_ok; -1 while the cache is still empty. */
static int ti_self_check(void)
{
    int n, picks[SELF_CHECK_TYPES], i, k = 0, nodes[SELF_CHECK_TYPES];
    const char *names[SELF_CHECK_TYPES];
    char why[256], detail[512];
    if (g_checked) return g_ok;
    if (!g_installed || ti_on_main() != 1) return 0;
    n = ti_cache_count();
    if (n <= 0 || n > SH_TYPEINFO_MAX_TYPES) return -1;
    g_checked = 1;
    picks[0] = 0; picks[1] = n / 2; picks[2] = n - 1;
    detail[0] = '\0';
    for (i = 0; i < SELF_CHECK_TYPES; i++) {
        const void *key = NULL;
        void *mine;
        int eq;
        if (i && picks[i] == picks[i - 1]) continue;
        if (!ti_rd_ptr(g_cache + SH_TYPEINFO_KEYS_OFF + (size_t)picks[i] * sizeof(void *), (void **)&key) || !key) continue;
        mine = ti_build(key, why, sizeof why);
        if (!mine) {
            ti_log("MODS: typeinfo: self-check: the product could not build the game's own type %s: %s; the product's "
                   "table is not used, so a mod's entity keeps drawing with a zero scale and the hidden badge",
                   (const char *)key, why);
            g_ok = 0;
            return 0;
        }
        eq = ti_tree_eq(mine, g_cache + SH_TYPEINFO_TREES_OFF + (size_t)picks[i] * SH_TYPEINFO_TREE_SIZE, &nodes[k], why, sizeof why);
        ti_tree_free(NULL, mine);
        if (!eq) {
            ti_log("MODS: typeinfo: self-check: the product's tree for %s differs from the game's (%s); the product's "
                   "table is not used, so a mod's entity keeps drawing with a zero scale and the hidden badge",
                   (const char *)key, why);
            g_ok = 0;
            return 0;
        }
        names[k] = (const char *)key;
        k++;
    }
    for (i = 0; i < k; i++) {
        size_t used = strlen(detail);
        _snprintf_s(detail + used, sizeof detail - used, _TRUNCATE, "%s%s (%d nodes)", i ? ", " : "", names[i], nodes[i]);
    }
    g_ok = k > 0;
    if (g_ok) ti_log("MODS: typeinfo: self-check passed on %s: the product builds the game's trees node for node, so a type "
                     "the game's cache lacks is served from the product's table", detail);
    else ti_log("MODS: typeinfo: self-check found no readable entry in the game's cache; the product's table is not used");
    return g_ok;
}

/* The tree for `key`, from the table or built now. NULL (with one line) when
 * it cannot be built; the game's own path then answers as before. */
static void *ti_resolve(const void *key)
{
    char why[256];
    void *tree = sh_typeinfo_table_find(&g_table, key);
    if (tree) return tree;
    if (g_depth) return NULL;
    tree = ti_build(key, why, sizeof why);
    if (!tree) {
        InterlockedIncrement(&g_failed);
        ti_log("MODS: typeinfo: no default tree for %s: %s; its entity is drawn the game's own way", (const char *)key, why);
        return NULL;
    }
    if (!sh_typeinfo_table_insert(&g_table, key, tree)) {
        ti_tree_free(NULL, tree);
        ti_log("MODS: typeinfo: the default tree of %s could not be kept (out of memory)", (const char *)key);
        return NULL;
    }
    InterlockedIncrement(&g_built);
    return tree;
}

/* A miss in the game's cache that the product answers: the key, when this
 * call is one the table may serve. */
static const void *ti_missing_key(void *cache, const void *key)
{
    if (!g_installed || cache != g_cache || !key || g_depth || ti_on_main() != 1) return NULL;
    if (sh_typeinfo_cache_index(cache, key) >= 0) return NULL;
    if (ti_self_check() != 1) return NULL;
    return key;
}

static void *ti_get_detour(void *cache, void **out, void *entity)
{
    const void *key = NULL;
    void *tree;
    if (entity && !ti_rd_ptr((const unsigned char *)entity + ENTITY_KEY_OFF, (void **)&key)) key = NULL;
    if (ti_missing_key(cache, key) && (tree = ti_resolve(key)) != NULL) {
        /* The game's own hit path runs on a cache that holds only this type. */
        unsigned char view[SH_TYPEINFO_VIEW_BYTES];
        sh_typeinfo_view_fill(view, key, tree);
        InterlockedIncrement(&g_served);
        return g_get(view, out, entity);
    }
    return g_get(cache, out, entity);
}

static void *ti_lookup_detour(void *cache, void **keyp)
{
    const void *key = NULL;
    void *tree;
    if (keyp && !ti_rd_ptr(keyp, (void **)&key)) key = NULL;
    if (ti_missing_key(cache, key) && (tree = ti_resolve(key)) != NULL) {
        InterlockedIncrement(&g_served);
        return tree;
    }
    return g_lookup(cache, keyp);
}

static const sig_result *ti_sig(const sig_result *results, size_t count, const char *name)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (results[i].name && !strcmp(results[i].name, name) && results[i].status == SIG_OK && results[i].addr)
            return &results[i];
    return NULL;
}

int sh_modtypeinfo_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    static const char *const needed[] = { "TreeTextCtor", "TreeTextDefaultState", "TreeTextOpenWriter", "TreeTextFinish",
                                          "TreeTextDtor", "GridTreeFromText", "GridTreeDestroy", "DeclFind",
                                          "SnapTypeInfoCacheGet", "SnapTypeInfoCacheLookup" };
    static const char *const globals[] = { "snap_typeinfo_cache", "snap_editor_row_count", "main_thread_id",
                                           "resource_manager_ctx", "validator_manager" };
    const sig_result *get, *lookup;
    void *get_tramp, *lookup_tramp;
    size_t i;
    const char *missing = NULL;
    for (i = 0; i < sizeof needed / sizeof needed[0] && !missing; i++)
        if (!ti_sig(results, count, needed[i])) missing = needed[i];
    for (i = 0; i < sizeof globals / sizeof globals[0] && !missing; i++)
        if (!glb_resolve(module_base, globals[i], NULL)) missing = globals[i];
    if (!missing && !sh_process_heap_bind(&g_heap, results, count, module_base)) missing = "the process heap";
    if (missing) {
        ti_log("MODS: typeinfo: %s unresolved on this build; a mod's entity placed in the editor draws with a zero scale "
               "while hovered and carries the hidden badge until a product build resolves it", missing);
        return 0;
    }
    g_cache = (unsigned char *)glb_resolve(module_base, "snap_typeinfo_cache", NULL);
    g_rows = (const int *)glb_resolve(module_base, "snap_editor_row_count", NULL);
    g_main_thread_at = (const uint32_t *)glb_resolve(module_base, "main_thread_id", NULL);
    g_res_ctx = (void *)glb_resolve(module_base, "resource_manager_ctx", NULL);
    g_validator_at = (const void *)glb_resolve(module_base, "validator_manager", NULL);
    g_tt_ctor = (ti_tt_ctor_fn)ti_sig(results, count, "TreeTextCtor")->addr;
    g_tt_state = (ti_tt_state_fn)ti_sig(results, count, "TreeTextDefaultState")->addr;
    g_tt_open = (ti_tt_open_fn)ti_sig(results, count, "TreeTextOpenWriter")->addr;
    g_tt_finish = (ti_tt_finish_fn)ti_sig(results, count, "TreeTextFinish")->addr;
    g_tt_dtor = (ti_tt_dtor_fn)ti_sig(results, count, "TreeTextDtor")->addr;
    g_tree_text = (ti_tree_text_fn)ti_sig(results, count, "GridTreeFromText")->addr;
    g_tree_destroy = (ti_tree_destroy_fn)ti_sig(results, count, "GridTreeDestroy")->addr;
    g_decl_find = (ti_decl_find_fn)ti_sig(results, count, "DeclFind")->addr;
    get = ti_sig(results, count, "SnapTypeInfoCacheGet");
    lookup = ti_sig(results, count, "SnapTypeInfoCacheLookup");
    get_tramp = sh_prepare_detour_sig(get, (void *)ti_get_detour, GET_STOLEN);
    lookup_tramp = get_tramp ? sh_prepare_detour_sig(lookup, (void *)ti_lookup_detour, LOOKUP_STOLEN) : NULL;
    if (!get_tramp || !lookup_tramp) {
        if (get_tramp) sh_uninstall_detour(get_tramp);
        ti_log("MODS: typeinfo: the %s prologue could not be relocated; a mod's entity placed in the editor draws with "
               "a zero scale while hovered and carries the hidden badge", get_tramp ? "lookup" : "Get");
        return 0;
    }
    g_get = (ti_get_fn)get_tramp;
    g_lookup = (ti_lookup_fn)lookup_tramp;
    if (sh_commit_detour(get_tramp) != B2_PATCH_OK) {
        g_get = NULL; g_lookup = NULL;
        sh_uninstall_detour(get_tramp);
        sh_uninstall_detour(lookup_tramp);
        ti_log("MODS: typeinfo: the Get detour could not be committed; a mod's entity placed in the editor draws with "
               "a zero scale while hovered and carries the hidden badge");
        return 0;
    }
    if (sh_commit_detour(lookup_tramp) != B2_PATCH_OK) {
        /* Get alone still serves placement and drawing; the refresh paths read the decl's own tree. */
        sh_uninstall_detour(lookup_tramp);
        g_lookup = NULL;
        ti_log("MODS: typeinfo: the lookup detour could not be committed; a mod's entity is drawn right, its property "
               "refresh reads its decl's own tree");
    }
    g_installed = 1;
    ti_log("MODS: typeinfo: detours installed on idSnapTypeInfoCache::Get%s; a type the game's cache lacks is served "
           "from the product's table once the boot self-check passes", g_lookup ? " and its lookup" : "");
    return 1;
}

void sh_modtypeinfo_after_palette(void *palette)
{
    void *entries = NULL;
    int count = 0, rows = 0, i, mods = 0, missing = 0, n, checked;
    if (!g_installed || !palette || ti_on_main() != 1) return;
    if (!ti_rd_ptr((unsigned char *)palette + PAL_ENTRIES_OFF, &entries)) return;
    __try { count = *(const int *)((unsigned char *)palette + PAL_COUNT_OFF); rows = *g_rows; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!entries || count <= 0 || count > PAL_MAX) return;
    n = ti_cache_count();
    for (i = 0; i < count; ++i) {
        void *sedef = NULL, *name = NULL, *resource = NULL, *key = NULL;
        const unsigned char *entry = (const unsigned char *)entries + (size_t)i * PAL_STRIDE;
        int is_mod = 0;
        if (!ti_rd_ptr(entry, &sedef) || !sedef || !ti_rd_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &name) || !name)
            continue;
        __try { is_mod = sh_modserve_is_mod_sedef((const char *)name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { is_mod = 0; }
        if (!is_mod) continue;
        mods++;
        if (!ti_rd_ptr((unsigned char *)sedef + SEDEF_ENTITYDEF_OFF, &resource) || !resource) continue;
        if (!ti_rd_ptr((unsigned char *)resource + RESOURCE_NAME_OFF, &key) || !key) continue;
        if (n > 0 && sh_typeinfo_cache_index(g_cache, key) < 0) missing++;
    }
    checked = ti_self_check();
    if (mods == g_reported_mods && missing == g_reported_missing) return;
    g_reported_mods = mods;
    g_reported_missing = missing;
    if (n <= 0) ti_log("MODS: typeinfo: %d mod rows in the palette; the game has not built its type-info cache yet", mods);
    else if (checked == 1)
        ti_log("MODS: typeinfo: %d of %d mod rows resolve through the product's table (the game's cache holds %d of at most %d "
               "types; %d rows registered)", missing, mods, n, SH_TYPEINFO_MAX_TYPES, rows);
    else
        ti_log("MODS: typeinfo: %d of %d mod rows are not in the game's cache of %d types (%d rows registered) and the "
               "product's table is not serving them", missing, mods, n, rows);
}

void sh_modtypeinfo_decl_changed(const char *type, const char *name)
{
    int removed;
    if (!g_installed || !type || !name || _stricmp(type, "entityDef") || ti_on_main() != 1) return;
    removed = sh_typeinfo_table_remove_name(&g_table, name, ti_tree_free, NULL);
    if (removed) ti_log("MODS: typeinfo: the default tree of entityDef %s is dropped with its changed bytes; rebuilt at its next use", name);
}
#endif
