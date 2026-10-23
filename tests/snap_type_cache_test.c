/* The editor type-cache replacements on synthetic engine state: lookup
 * completion inserts a missing parent's tree in key order under the process
 * heap, keeps retryable and failed parents apart, and Build runs natively
 * below the definition limit and fills the slots without failing above it. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snap_type_cache.h"

void backend_log(const char *message) { (void)message; }

#define CAPACITY 1500
#define KEYS 0x8
#define SLOTS 0x2ee8
#define SLOT 0x48
#define BUILDER_NAME 0x100
#define BUILDER_FINAL 0x108
#define BUILDER_LIFE 0x10c

/* Entity names at increasing addresses: key order is pointer order. */
static char g_names[1700][24];
static uint8_t *g_cache;

/* One synthetic entityDef per name; NULL where none exists. */
static void *g_decls[1700];
static int g_no_entity[1700];
static int g_creates, g_destroys, g_writes, g_assigns, g_dtors, g_mode_calls, g_mode_inits;
static int g_native_builds, g_native_gets, g_native_finds, g_mode_value = 1, g_mode_second = -1;
static int g_heap_available = 1;
static int g_reenter_index = -1;
static int g_expect_depth = 1;   /* completion pushes the process heap; Build runs inside the native push */

/* Process heap scope as the native stack: ids at +0x44, depth at +0xc4. */
static uint8_t g_heap_state[0x100];
static int heap_depth(void) { return *(int *)(g_heap_state + 0xc4); }
static void *heap_get(void) { return g_heap_available ? g_heap_state : NULL; }
static void heap_push(void *self, int heap)
{
    int depth = *(int *)((uint8_t *)self + 0xc4);
    *(int *)((uint8_t *)self + 0x44 + depth * 4) = heap;
    *(int *)((uint8_t *)self + 0xc4) = depth + 1;
}
static void heap_pop(void *self) { *(int *)((uint8_t *)self + 0xc4) -= 1; }

static int name_index(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < 1700; i++) if (name == g_names[i]) return i;
    return -1;
}
static uint64_t *keys(void) { return (uint64_t *)(g_cache + KEYS); }
static uint8_t *slot(int index) { return g_cache + SLOTS + (size_t)index * SLOT; }
static int count(void) { return *(int *)g_cache; }

/* Game-access object and game, each with the vtable slots the cache uses. */
static void *g_game_table[0x480 / 8], *g_access_table[0x230 / 8];
static struct { void **table; } g_game = { g_game_table }, g_access = { g_access_table };
static void *g_access_slot = &g_access;
typedef struct entity { const char *name; int alive; } entity;

static void **fake_get(void *cache, void **out, void *decl);
static void *fake_access_game(void *access) { assert(access == &g_access); return &g_game; }
static void *fake_create(void *game, void *decl, void *args)
{
    int index = name_index((const char *)decl);
    entity *e;
    assert(game == &g_game && args && index >= 0);
    g_creates++;
    if (g_reenter_index >= 0) {
        /* A temporary entity's construction reaching the cache again. */
        uint8_t parent[0x60] = {0};
        void *out = NULL;
        const char *key = g_names[g_reenter_index];
        memcpy(parent + 0x58, &key, sizeof(key));
        sh_snap_type_cache_test_get(g_cache, &out, parent);
    }
    if (g_no_entity[index]) return NULL;
    e = calloc(1, sizeof(*e));
    e->name = (const char *)decl; e->alive = 1;
    return e;
}
static void fake_write(void *game, void *ent, void *builder, unsigned char full)
{
    entity *e = ent;
    assert(game == &g_game && full == 1 && e->alive && *(int *)((uint8_t *)builder + BUILDER_LIFE) == 1);
    /* Build copies the prepared argument state into the builder's first 20 bytes. */
    for (int i = 0; i < 20; i++) assert(((uint8_t *)builder)[i] == 0x7a);
    memcpy((uint8_t *)builder + BUILDER_NAME, &e->name, sizeof(e->name));
    g_writes++;
}
static void fake_destroy(void *game, void *ent) { assert(game == &g_game); ((entity *)ent)->alive = 0; free(ent); g_destroys++; }

static void fake_ctor(void *builder, int zero) { assert(!zero); *(int *)((uint8_t *)builder + BUILDER_LIFE) = 1; }
static void fake_args(void *args, int a, int b) { assert(!a && !b); memset(args, 0x7a, 20); }
static void fake_reset(void *builder)
{
    const char *none = NULL;
    memcpy((uint8_t *)builder + BUILDER_NAME, &none, sizeof(none));
    *(int *)((uint8_t *)builder + BUILDER_FINAL) = 0;
}
static void fake_finalize(void *builder) { *(int *)((uint8_t *)builder + BUILDER_FINAL) = 1; }
static void fake_dtor(void *builder) { assert(*(int *)((uint8_t *)builder + BUILDER_LIFE) == 1); *(int *)((uint8_t *)builder + BUILDER_LIFE) = 2; g_dtors++; }
/* A tree records its entity's name at +0 and keeps the unused slot's state
 * (+0x20 and +0x38) it was built from. */
static void fake_assign(void *s, void *builder)
{
    assert(*(int *)((uint8_t *)builder + BUILDER_FINAL) == 1);
    assert(heap_depth() == g_expect_depth && (!g_expect_depth || *(int *)(g_heap_state + 0x44) == 0));
    assert(*(uint16_t *)((uint8_t *)s + 0x20) == 1 && *(uint32_t *)((uint8_t *)s + 0x38) == 0x910000);
    memcpy(s, (uint8_t *)builder + BUILDER_NAME, 8);
    g_assigns++;
}
static void *fake_entity_find(void *manager, const char *name, int create)
{
    int index = name_index(name);
    assert(manager == &g_decls && !create);
    return index >= 0 ? g_decls[index] : NULL;
}
static int g_definition_count;
static struct { void *entity; } g_definitions[1700];
static uint8_t g_definition_objects[1700][0x1d0];
static uint8_t g_entitydef_objects[1700][0x10];
static void *fake_definition_at(void *list, int index)
{
    assert(list == g_definitions && index >= 0 && index < g_definition_count);
    return g_definition_objects[index];
}
/* g_mode_second, when set, is the second reading. */
static int fake_mode(void) { return ++g_mode_calls == 2 && g_mode_second >= 0 ? g_mode_second : g_mode_value; }
static void fake_mode_init(void) { g_mode_inits++; }

/* The native routines: a sorted search over the published keys. */
static int native_index(const char *key)
{
    int low = 0, high = count();
    while (low < high) {
        int middle = (low + high) / 2;
        if (keys()[middle] < (uint64_t)(uintptr_t)key) low = middle + 1; else high = middle;
    }
    return low < count() && keys()[low] == (uint64_t)(uintptr_t)key ? low : -1;
}
static void fake_build(void *cache) { assert(cache == g_cache); g_native_builds++; }
static void **fake_get(void *cache, void **out, void *decl)
{
    const char *key;
    int index;
    assert(cache == g_cache);
    memcpy(&key, (uint8_t *)decl + 0x58, sizeof(key));
    index = native_index(key);
    *out = index >= 0 ? slot(index) : NULL;
    g_native_gets++;
    return out;
}
static void *fake_find(void *cache, const char **key)
{
    int index;
    assert(cache == g_cache);
    index = native_index(*key);
    g_native_finds++;
    return index >= 0 ? slot(index) : NULL;
}

static void reset_cache(int built)
{
    memset(g_cache, 0, SLOTS + (size_t)CAPACITY * SLOT);
    for (int i = 0; i < CAPACITY; i++) {
        *(uint16_t *)(slot(i) + 0x20) = 1;
        *(uint32_t *)(slot(i) + 0x38) = 0x910000;
    }
    for (int i = 0; i < built; i++) {
        const char *name = g_names[i * 2];
        keys()[i] = (uint64_t)(uintptr_t)name;
        memcpy(slot(i), &name, sizeof(name));
    }
    *(int *)g_cache = built;
}

static void definitions(int total, int every);

static void bind_native(void)
{
    sh_snap_type_cache_native native = {0};
    native.game_access = &g_access_slot;
    native.mode = fake_mode; native.mode_init = fake_mode_init;
    native.builder_ctor = fake_ctor; native.builder_args = fake_args; native.builder_reset = fake_reset;
    native.builder_finalize = fake_finalize; native.builder_dtor = fake_dtor;
    native.definition_count = &g_definition_count; native.definition_list = g_definitions;
    native.definition_at = fake_definition_at; native.entity_manager = &g_decls;
    native.entity_find = fake_entity_find; native.slot_assign = fake_assign;
    native.native_build = fake_build; native.native_get = fake_get; native.native_find = fake_find;
    native.heap_get = heap_get; native.heap_push = heap_push; native.heap_pop = heap_pop;
    sh_snap_type_cache_test_bind(&native);
    g_creates = g_destroys = g_writes = g_assigns = g_dtors = g_mode_calls = g_mode_inits = 0;
    g_native_builds = g_native_gets = g_native_finds = 0;
    g_heap_available = 1; g_reenter_index = -1; g_mode_value = 1; g_mode_second = -1; g_expect_depth = 1;
    memset(g_no_entity, 0, sizeof(g_no_entity));
    for (int i = 0; i < 1700; i++) g_decls[i] = g_names[i];
    definitions(1700, 0);                  /* every name is an editor type */
}

static void **lookup(int parent, void **out)
{
    uint8_t decl[0x60] = {0};
    const char *key = parent >= 0 ? g_names[parent] : NULL;
    memcpy(decl + 0x58, &key, sizeof(key));
    return sh_snap_type_cache_test_get(g_cache, out, decl);
}

/* A missing parent is built and inserted at its key position; neighbours
 * keep their trees, and the native lookup that follows finds the new one. */
static void test_completion(void)
{
    void *out = NULL;
    bind_native(); reset_cache(4);                /* names 0, 2, 4, 6 */
    assert(lookup(3, &out) == &out);
    assert(count() == 5 && g_creates == 1 && g_writes == 1 && g_destroys == 1 && g_assigns == 1 && g_dtors == 1);
    assert(g_native_gets == 1 && out == slot(2));
    for (int i = 0, expected[] = {0, 2, 3, 4, 6}; i < 5; i++) {
        const char *tree;
        memcpy(&tree, slot(i), sizeof(tree));
        assert(keys()[i] == (uint64_t)(uintptr_t)g_names[expected[i]] && tree == g_names[expected[i]]);
        assert(*(uint16_t *)(slot(i) + 0x20) == 1 && *(uint32_t *)(slot(i) + 0x38) == 0x910000);
    }
    assert(heap_depth() == 0);
    /* Before every key and after every key. */
    assert(lookup(7, &out) && count() == 6 && keys()[5] == (uint64_t)(uintptr_t)g_names[7] && out == slot(5));
    /* A present key builds nothing. */
    {
        const char *key = g_names[2];
        void *found = sh_snap_type_cache_test_find(g_cache, &key);
        assert(found == slot(1) && g_creates == 2 && g_native_finds == 1 && count() == 6);
    }
}

/* A parent that is not published yet stays retryable; one whose definition
 * yields no entity is not attempted again. */
static void test_failures(void)
{
    void *out = (void *)1;
    bind_native(); reset_cache(2);                /* names 0, 2 */
    g_decls[5] = NULL;
    assert(lookup(5, &out) && out == NULL && count() == 2 && g_creates == 0);
    g_decls[5] = g_names[5];
    assert(lookup(5, &out) && out == slot(2) && count() == 3 && g_creates == 1);
    g_no_entity[9] = 1;
    assert(lookup(9, &out) && out == NULL && count() == 3 && g_creates == 2 && g_dtors == 3);
    assert(lookup(9, &out) && out == NULL && g_creates == 2);
    /* No parent at all, or no built cache: the native lookup alone. */
    assert(lookup(-1, &out) && g_creates == 2);
    reset_cache(0);
    assert(lookup(11, &out) && out == NULL && g_creates == 2 && count() == 0);
    assert(heap_depth() == 0);
}

/* Without the process heap nothing is built; it is retried once available. */
static void test_heap(void)
{
    void *out = NULL;
    bind_native(); reset_cache(2);
    g_heap_available = 0;
    assert(lookup(1, &out) && out == NULL && count() == 2 && g_creates == 0);
    g_heap_available = 1;
    assert(lookup(1, &out) && out == slot(1) && count() == 3 && g_creates == 1 && heap_depth() == 0);
}

/* A full cache keeps native behaviour. */
static void test_full(void)
{
    void *out = (void *)1;
    bind_native(); reset_cache(CAPACITY);
    assert(lookup(1, &out) && out == NULL && count() == CAPACITY && g_creates == 0);
}

/* Construction reaching the cache again does not recurse. */
static void test_reentry(void)
{
    void *out = NULL;
    bind_native(); reset_cache(2);
    g_reenter_index = 3;
    assert(lookup(1, &out) && out == slot(1) && count() == 3 && g_creates == 1 && g_native_gets == 2);
    assert(native_index(g_names[3]) < 0);
}

static void definitions(int total, int every)
{
    g_definition_count = total;
    for (int i = 0; i < total; i++) {
        void *entity = NULL;
        if (!every || i % every) {
            const char *name = g_names[1699 - i];
            entity = g_entitydef_objects[i];
            memcpy(g_entitydef_objects[i] + 8, &name, sizeof(name));
        }
        memcpy(g_definition_objects[i] + 0x1c8, &entity, sizeof(entity));
    }
}

/* A parent no editor definition names keeps the native miss, as Build never
 * caches it, until a definition that names it is published. */
static void test_non_editor(void)
{
    void *out = (void *)1;
    bind_native(); reset_cache(2);
    definitions(1690, 0);                  /* names 0..9 are no editor type */
    assert(lookup(5, &out) && out == NULL && count() == 2 && g_creates == 0);
    assert(lookup(5, &out) && out == NULL && g_creates == 0);
    definitions(1700, 0);
    assert(lookup(5, &out) && out == slot(2) && count() == 3 && g_creates == 1);
}

/* Below the limit, or once built, the native Build runs unchanged. */
static void test_build_native(void)
{
    bind_native(); reset_cache(0); g_expect_depth = 0;
    definitions(CAPACITY - 1, 0);
    sh_snap_type_cache_test_build(g_cache);
    assert(g_native_builds == 1 && g_creates == 0 && count() == 0);
    reset_cache(3);
    definitions(CAPACITY + 10, 0);
    sh_snap_type_cache_test_build(g_cache);
    assert(g_native_builds == 2 && g_creates == 0 && count() == 3);
}

/* At the limit the slots are filled in native definition order, sorted,
 * definitions without an entityDef skipped and failed entities dropped. */
static void test_build_replacement(void)
{
    int expected = 0;
    bind_native(); reset_cache(0); g_expect_depth = 0;
    definitions(1700, 50);                 /* every fiftieth has no entityDef */
    g_no_entity[1699 - 1] = 1;             /* definition 1 yields no entity */
    g_decls[1699 - 2] = NULL;              /* definition 2 names no published entityDef */
    for (int i = 0; i < 1700; i++) if (i % 50) expected++;
    assert(expected > CAPACITY);
    sh_snap_type_cache_test_build(g_cache);
    assert(g_native_builds == 0 && g_mode_calls == 1 && g_mode_inits == 1);
    assert(count() == CAPACITY - 2 && g_creates == CAPACITY - 1 && g_assigns == CAPACITY - 2 && g_dtors == 1);
    for (int i = 1; i < count(); i++) assert(keys()[i - 1] < keys()[i]);
    for (int i = 0; i < count(); i++) {
        const char *tree;
        memcpy(&tree, slot(i), sizeof(tree));
        assert(keys()[i] == (uint64_t)(uintptr_t)tree);
    }
    assert(native_index(g_names[1699 - 1]) < 0 && native_index(g_names[1699 - 2]) < 0 &&
           native_index(g_names[1699 - 3]) >= 0);
    /* Taken in native order: the definitions beyond the slots are absent. */
    assert(native_index(g_names[0]) < 0 && native_index(g_names[1699 - 1501]) >= 0 && native_index(g_names[1699 - 1531]) < 0);
    /* The mode rule: the initializer runs for mode 1 or a second reading of 0. */
    bind_native(); reset_cache(0); definitions(1600, 0); g_mode_value = 2; g_expect_depth = 0;
    sh_snap_type_cache_test_build(g_cache);
    assert(g_mode_calls == 2 && g_mode_inits == 0);
    bind_native(); reset_cache(0); definitions(1600, 0); g_mode_value = 2; g_mode_second = 0; g_expect_depth = 0;
    sh_snap_type_cache_test_build(g_cache);
    assert(g_mode_calls == 2 && g_mode_inits == 1);
    /* Exactly at the limit the replacement runs; every slot is filled. */
    bind_native(); reset_cache(0); definitions(CAPACITY, 0); g_expect_depth = 0;
    sh_snap_type_cache_test_build(g_cache);
    assert(g_native_builds == 0 && count() == CAPACITY);
}

int main(void)
{
    g_cache = calloc(1, SLOTS + (size_t)CAPACITY * SLOT);
    assert(g_cache);
    for (int i = 0; i < 1700; i++) snprintf(g_names[i], sizeof(g_names[i]), "entity/%04d", i);
    g_access_table[0x228 / 8] = (void *)fake_access_game;
    g_game_table[0x448 / 8] = (void *)fake_create;
    g_game_table[0x378 / 8] = (void *)fake_write;
    g_game_table[0x470 / 8] = (void *)fake_destroy;
    test_completion();
    test_failures();
    test_heap();
    test_full();
    test_reentry();
    test_non_editor();
    test_build_native();
    test_build_replacement();
    free(g_cache);
    assert(!sh_snap_type_cache_active());
    puts("snap_type_cache_test: PASS");
    return 0;
}
