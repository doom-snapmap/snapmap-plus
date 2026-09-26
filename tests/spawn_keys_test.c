/* The spawn-key replacements on synthetic engine state: native results for
 * stock rows and requests, the private-key contract for package keys. */
#include <windows.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn_keys.h"
#include "package_keys.h"

void backend_log(const char *message) { (void)message; }
int sh_decl_native_schema_enum_constants(sh_decl_native_schema *schema, const char *enum_name,
    int (*visit)(void *context, const char *name, uint32_t value), void *context)
{ (void)schema; (void)enum_name; (void)visit; (void)context; return 0; }

#define IMP 4u
#define T3 8u
#define ZOMBIE 131072u
#define K19 524288u
#define K20 1048576u

static uint8_t *g_game_state;
static void *g_game_slot;
static uint32_t g_next_type;
static void *g_health_ai;
static float g_health_value;
static uint32_t test_type(void *conductor, void *ai) { (void)conductor; (void)ai; return g_next_type; }
static void test_health(void *conductor, void *ai, float value) { (void)conductor; g_health_ai = ai; g_health_value = value; }
static void *test_resolve(void *owner, int handle) { return (uint8_t *)owner + handle; }
static uint32_t test_ai_type(void *filter, void *ai) { (void)filter; return *(uint32_t *)ai; }

static uint32_t *seed(void) { return (uint32_t *)(g_game_state + 0x285be8); }
/* The native pick: one draw, then the match at that index. */
static int native_pick(uint32_t state, int matches) { state = state * 0x19660du + 0x3c6ef35fu; return (int)((state >> 10) & 0x7fffu) % matches; }

static void test_select(void)
{
    uint8_t *manager = calloc(1, 0x5920), *entries = calloc(6, 0x30);
    const uint32_t types[6] = {IMP | ZOMBIE, IMP | ZOMBIE | K19, T3 | ZOMBIE, K20, SH_PACKAGE_KEY_MARKER | K19,
        IMP | ZOMBIE | K20};
    assert(manager && entries);
    for (int i = 0; i < 6; i++) memcpy(entries + i * 0x30, types + i, 4);
    memcpy(manager + 0x5908, &entries, sizeof(entries));
    *(int *)(manager + 0x5910) = 6;
    /* Stock requests select stock rows only, with the native draw. */
    *seed() = 12345u;
    uint64_t chosen = sh_spawn_keys_test_select(manager, IMP);
    assert(chosen == (uint64_t)(uintptr_t)entries && *seed() == 12345u * 0x19660du + 0x3c6ef35fu);
    for (uint32_t state = 1; state < 40; state++) {
        *seed() = state;
        chosen = sh_spawn_keys_test_select(manager, ZOMBIE);
        int pick = native_pick(state, 2);
        assert(chosen == (uint64_t)(uintptr_t)(entries + (pick ? 2 : 0) * 0x30));
        *seed() = state;
        chosen = sh_spawn_keys_test_select(manager, 0xffffffffu);
        assert(chosen == (uint64_t)(uintptr_t)(entries + (pick ? 2 : 0) * 0x30));
    }
    /* A package request selects its own key's rows (and stock rows its named
     * bits name), never another key; a marked key matches exactly. */
    *seed() = 7u;
    assert(sh_spawn_keys_test_select(manager, K19) == (uint64_t)(uintptr_t)(entries + 1 * 0x30));
    *seed() = 7u;
    chosen = sh_spawn_keys_test_select(manager, K20);
    assert(chosen == (uint64_t)(uintptr_t)(entries + (native_pick(7u, 2) ? 5 : 3) * 0x30));
    *seed() = 7u;
    assert(sh_spawn_keys_test_select(manager, SH_PACKAGE_KEY_MARKER | K19) == (uint64_t)(uintptr_t)(entries + 4 * 0x30));
    *seed() = 9u;
    chosen = sh_spawn_keys_test_select(manager, IMP | K19);
    assert(chosen == (uint64_t)(uintptr_t)(entries + (native_pick(9u, 2) ? 1 : 0) * 0x30));
    /* No match: no draw. */
    *seed() = 5u;
    assert(!sh_spawn_keys_test_select(manager, 1u << 25) && *seed() == 5u);
    *(int *)(manager + 0x5910) = 0;
    assert(!sh_spawn_keys_test_select(manager, IMP) && *seed() == 5u);
    free(entries); free(manager);
}

static void test_modifier(void)
{
    uint8_t *conductor = calloc(1, 0x2220), *ai = calloc(1, 0x23d70);
    float rows[4][3] = {{2.0f, 3.0f, 0.0f}, {5.0f, 6.0f, 0.0f}, {7.0f, 8.0f, 0.0f}, {9.0f, 0.5f, 0.0f}};
    const uint32_t masks[4] = {T3, K19, IMP, SH_PACKAGE_KEY_MARKER | K20};
    uint8_t *packed = calloc(4, 12);
    uint64_t modifiers;
    float health = 20.0f, damage = 0.0f;
    assert(conductor && ai && packed);
    for (int i = 0; i < 4; i++) { memcpy(packed + i * 12, rows[i], 8); memcpy(packed + i * 12 + 8, masks + i, 4); }
    memcpy(conductor + 0x2210, &packed, sizeof(packed));
    *(int *)(conductor + 0x2218) = 4;
    memcpy(&modifiers, &health, 4); memcpy((uint8_t *)&modifiers + 4, &damage, 4);
    /* A variant of the stock imp with its own key gets its own row. */
    g_next_type = IMP | ZOMBIE | K19;
    sh_spawn_keys_test_modifier(conductor, ai, modifiers);
    assert(g_health_ai == ai && g_health_value == 10.0f * 5.0f && *(float *)(ai + 0x23d6c) == 0.01f * 6.0f);
    /* The stock imp gets the stock row, never a package's. */
    g_next_type = IMP | ZOMBIE;
    sh_spawn_keys_test_modifier(conductor, ai, modifiers);
    assert(g_health_value == 10.0f * 7.0f && *(float *)(ai + 0x23d6c) == 0.01f * 8.0f);
    /* A variant whose key has no row falls back to its named bits. */
    g_next_type = IMP | K20;
    sh_spawn_keys_test_modifier(conductor, ai, modifiers);
    assert(g_health_value == 10.0f * 7.0f);
    g_next_type = SH_PACKAGE_KEY_MARKER | K20;
    health = 0.5f; damage = 2.0f;
    memcpy(&modifiers, &health, 4); memcpy((uint8_t *)&modifiers + 4, &damage, 4);
    sh_spawn_keys_test_modifier(conductor, ai, modifiers);
    assert(g_health_value == 0.5f * 9.0f && *(float *)(ai + 0x23d6c) == 2.0f * 0.5f);
    /* The native clamp keeps a NaN modifier (MAXSS/MINSS operand order) and
     * floors outgoing damage at zero. */
    health = NAN; damage = 3.0f;
    memcpy(&modifiers, &health, 4); memcpy((uint8_t *)&modifiers + 4, &damage, 4);
    rows[3][1] = -2.0f; memcpy(packed + 3 * 12 + 4, &rows[3][1], 4);
    g_next_type = SH_PACKAGE_KEY_MARKER | K20;
    sh_spawn_keys_test_modifier(conductor, ai, modifiers);
    assert(isnan(g_health_value) && *(float *)(ai + 0x23d6c) == 0.0f);
    /* No matching row: nothing applied. */
    g_health_ai = NULL; *(float *)(ai + 0x23d6c) = -1.0f;
    g_next_type = K20;
    sh_spawn_keys_test_modifier(conductor, ai, modifiers);
    assert(!g_health_ai && *(float *)(ai + 0x23d6c) == -1.0f);
    free(packed); free(ai); free(conductor);
}

static void test_filter(void)
{
    uint8_t *filter = calloc(1, 0xcb0), *owner = calloc(1, 0x100), *table = calloc(1, 0x50);
    uint32_t stock = IMP | ZOMBIE, variant = IMP | ZOMBIE | K19, other = IMP | K20, custom = K19;
    assert(filter && owner && table);
    memcpy(table + 0x48, &owner, sizeof(owner));
    memcpy(g_game_state + 0x298128, &table, sizeof(table));
    *(int *)(filter + 0xca4) = -1;
    *(uint32_t *)(filter + 0xc9c) = IMP;
    assert(sh_spawn_keys_test_filter(filter, &stock) && sh_spawn_keys_test_filter(filter, &variant));
    *(uint32_t *)(filter + 0xc9c) = K19;
    assert(!sh_spawn_keys_test_filter(filter, &stock) && sh_spawn_keys_test_filter(filter, &variant) &&
        !sh_spawn_keys_test_filter(filter, &other) && sh_spawn_keys_test_filter(filter, &custom));
    *(uint32_t *)(filter + 0xc9c) = 0xbffdffffu;
    assert(sh_spawn_keys_test_filter(filter, &stock) && sh_spawn_keys_test_filter(filter, &variant));
    /* Same type as a cached entity. */
    memcpy(owner + 0x10, &variant, 4);
    *(int *)(filter + 0xca4) = 0x10;
    assert(sh_spawn_keys_test_filter(filter, &stock) && sh_spawn_keys_test_filter(filter, &other) &&
        sh_spawn_keys_test_filter(filter, &custom));
    memcpy(owner + 0x10, &custom, 4);
    assert(sh_spawn_keys_test_filter(filter, &variant) && !sh_spawn_keys_test_filter(filter, &stock));
    free(table); free(owner); free(filter);
}

/* Published group codes: a group request selects among its members only,
 * never the row holding their OR; a group modifier row reaches its members;
 * a group filter classifies them. Two tables disagreeing on one code are not
 * published, and later publications replace earlier ones. */
static void test_groups(void)
{
    const uint32_t a = 270008320u, b = 271056896u, c = 272105472u, g = SH_PACKAGE_KEY_MARKER | (1u << 22);
    sh_package_key_groups library, map, other;
    sh_package_key_set ab = {{0}}, bc = {{0}};
    uint8_t *manager = calloc(1, 0x5920), *entries = calloc(4, 0x30);
    const uint32_t types[4] = {IMP, a, b, c};
    memset(&library, 0, sizeof(library)); memset(&map, 0, sizeof(map)); memset(&other, 0, sizeof(other));
    assert(manager && entries && sh_package_key_set_add(&ab, a) && sh_package_key_set_add(&ab, b));
    assert(sh_package_key_set_add(&bc, b) && sh_package_key_set_add(&bc, c));
    assert(sh_package_key_groups_define(&library, g, &ab) && sh_package_key_groups_define(&other, g, &bc));
    for (int i = 0; i < 4; i++) memcpy(entries + i * 0x30, types + i, 4);
    memcpy(manager + 0x5908, &entries, sizeof(entries));
    *(int *)(manager + 0x5910) = 4;
    /* Unpublished, g names itself: nothing answers it. */
    assert(sh_spawn_keys_publish_groups(NULL, NULL));
    *seed() = 3u;
    assert(!sh_spawn_keys_test_select(manager, g) && *seed() == 3u);
    assert(sh_spawn_keys_publish_groups(&library, &map));
    for (uint32_t state = 1; state < 40; state++) {
        *seed() = state;
        uint64_t chosen = sh_spawn_keys_test_select(manager, g);
        assert(chosen == (uint64_t)(uintptr_t)(entries + (native_pick(state, 2) ? 2 : 1) * 0x30));
    }
    *seed() = 3u;
    assert(sh_spawn_keys_test_select(manager, c) == (uint64_t)(uintptr_t)(entries + 3 * 0x30));
    /* The map's table may not redefine a library code. */
    assert(!sh_spawn_keys_publish_groups(&library, &other));
    *seed() = 3u;
    assert(sh_spawn_keys_test_select(manager, g) != (uint64_t)(uintptr_t)(entries + 3 * 0x30));
    /* A group modifier row applies to both members, not to the OR's owner. */
    uint8_t *conductor = calloc(1, 0x2220), *ai = calloc(1, 0x23d70), *packed = calloc(2, 12);
    float rows[2][3] = {{3.0f, 1.0f, 0.0f}, {4.0f, 1.0f, 0.0f}};
    const uint32_t masks[2] = {g, IMP};
    uint64_t modifiers;
    float health = 10.0f, damage = 1.0f;
    assert(conductor && ai && packed);
    for (int i = 0; i < 2; i++) { memcpy(packed + i * 12, rows[i], 8); memcpy(packed + i * 12 + 8, masks + i, 4); }
    memcpy(conductor + 0x2210, &packed, sizeof(packed));
    *(int *)(conductor + 0x2218) = 2;
    memcpy(&modifiers, &health, 4); memcpy((uint8_t *)&modifiers + 4, &damage, 4);
    g_next_type = IMP | a; g_health_ai = NULL;
    sh_spawn_keys_test_modifier(conductor, ai, modifiers);
    assert(g_health_ai == ai && g_health_value == 30.0f);
    g_next_type = b;
    sh_spawn_keys_test_modifier(conductor, ai, modifiers);
    assert(g_health_value == 30.0f);
    g_next_type = IMP | c; g_health_ai = NULL;
    sh_spawn_keys_test_modifier(conductor, ai, modifiers);
    assert(g_health_ai == ai && g_health_value == 40.0f);
    /* A group filter classifies its members only. */
    uint8_t *filter = calloc(1, 0xcb0);
    uint32_t first = a, second = IMP | b, third = c;
    assert(filter);
    *(int *)(filter + 0xca4) = -1;
    *(uint32_t *)(filter + 0xc9c) = g;
    assert(sh_spawn_keys_test_filter(filter, &first) && sh_spawn_keys_test_filter(filter, &second) &&
        !sh_spawn_keys_test_filter(filter, &third));
    /* A map provider's own group (no library table) is published too. */
    assert(sh_spawn_keys_publish_groups(NULL, &library));
    assert(sh_spawn_keys_test_filter(filter, &first) && !sh_spawn_keys_test_filter(filter, &third));
    *seed() = 3u;
    assert(sh_spawn_keys_test_select(manager, g) != (uint64_t)(uintptr_t)(entries + 3 * 0x30) &&
        sh_spawn_keys_test_select(manager, g));
    /* Retiring the tables leaves g naming itself again. */
    assert(sh_spawn_keys_publish_groups(NULL, NULL));
    assert(!sh_spawn_keys_test_filter(filter, &first));
    free(filter); free(packed); free(ai); free(conductor); free(entries); free(manager);
}

/* Publication is copied into whichever buffer no replacement reads and then
 * switched: a replacement holding a table (from entry to return, on any
 * thread) keeps reading exactly that table while publications continue, and
 * a publication that would reuse its buffer waits until it returns. */
typedef struct publisher { const sh_package_key_groups *library; volatile LONG done; } publisher;
static DWORD WINAPI publish_thread(LPVOID context)
{
    publisher *job = context;
    assert(sh_spawn_keys_publish_groups(job->library, NULL));
    InterlockedExchange(&job->done, 1);
    return 0;
}
typedef struct reader { uint8_t *filter; volatile LONG *stop; uint32_t type; LONG calls, misses; } reader;
static DWORD WINAPI reader_thread(LPVOID context)
{
    reader *job = context;
    while (!InterlockedCompareExchange(job->stop, 0, 0)) {
        if (!sh_spawn_keys_test_filter(job->filter, &job->type)) job->misses++;
        job->calls++;
    }
    return 0;
}
static void test_publication(void)
{
    const uint32_t a = 270008320u, b = 271056896u, c = 272105472u, g = SH_PACKAGE_KEY_MARKER | (1u << 22);
    const uint32_t pattern = (g & SH_PACKAGE_KEY_SINGLE) >> 19;
    sh_package_key_groups *first = calloc(1, sizeof(*first)), *second = calloc(1, sizeof(*second));
    sh_package_key_groups *third = calloc(1, sizeof(*third));
    sh_package_key_set ab = {{0}}, ac = {{0}}, bc = {{0}};
    uint8_t *filter = calloc(1, 0xcb0);
    uint32_t type_a = a, type_b = b;
    assert(first && second && third && filter);
    assert(sh_package_key_set_add(&ab, a) && sh_package_key_set_add(&ab, b));
    assert(sh_package_key_set_add(&ac, a) && sh_package_key_set_add(&ac, c));
    assert(sh_package_key_set_add(&bc, b) && sh_package_key_set_add(&bc, c));
    assert(sh_package_key_groups_define(first, g, &ab) && sh_package_key_groups_define(second, g, &ac) &&
        sh_package_key_groups_define(third, g, &bc));
    *(int *)(filter + 0xca4) = -1;
    *(uint32_t *)(filter + 0xc9c) = g;
    assert(sh_spawn_keys_publish_groups(first, NULL));
    const sh_package_key_groups *held = sh_spawn_keys_test_hold();
    assert(held->defined[pattern] && sh_package_key_set_equal(held->members + pattern, &ab));
    /* The next publication writes the other buffer and takes effect. */
    assert(sh_spawn_keys_publish_groups(second, NULL));
    assert(sh_spawn_keys_test_filter(filter, &type_a) && !sh_spawn_keys_test_filter(filter, &type_b));
    /* The one after it would reuse the held buffer: it waits. */
    publisher job = {third, 0};
    HANDLE thread = CreateThread(NULL, 0, publish_thread, &job, 0, NULL);
    assert(thread);
    Sleep(250);
    assert(!InterlockedCompareExchange(&job.done, 0, 0));
    assert(held->defined[pattern] && sh_package_key_set_equal(held->members + pattern, &ab));
    assert(sh_spawn_keys_test_filter(filter, &type_a));
    sh_spawn_keys_test_release();
    assert(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && InterlockedCompareExchange(&job.done, 0, 0));
    CloseHandle(thread);
    assert(!sh_spawn_keys_test_filter(filter, &type_a) && sh_spawn_keys_test_filter(filter, &type_b));
    /* Readers on four threads against back-to-back publications: a is a
     * member of g in both tables, so a reader never sees it missing. */
    volatile LONG stop = 0;
    reader readers[4];
    HANDLE threads[4];
    assert(sh_spawn_keys_publish_groups(first, NULL));
    for (int i = 0; i < 4; i++) {
        readers[i] = (reader){filter, &stop, a, 0, 0};
        assert((threads[i] = CreateThread(NULL, 0, reader_thread, readers + i, 0, NULL)) != NULL);
    }
    for (int i = 0; i < 20000; i++) assert(sh_spawn_keys_publish_groups(i & 1 ? first : second, NULL));
    InterlockedExchange(&stop, 1);
    assert(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (int i = 0; i < 4; i++) {
        CloseHandle(threads[i]);
        assert(readers[i].calls > 0 && !readers[i].misses);
    }
    assert(sh_spawn_keys_publish_groups(NULL, NULL));
    free(filter); free(third); free(second); free(first);
}

int main(void)
{
    g_game_state = calloc(1, 0x2a0000);
    assert(g_game_state);
    g_game_slot = g_game_state;
    sh_spawn_keys_test_bind(&g_game_slot, test_type, test_health, test_resolve, test_ai_type, 0.01f, 10.0f);
    test_select();
    test_modifier();
    test_filter();
    test_groups();
    test_publication();
    free(g_game_state);
    assert(!sh_spawn_keys_active());
    puts("spawn_keys_test: PASS");
    return 0;
}
