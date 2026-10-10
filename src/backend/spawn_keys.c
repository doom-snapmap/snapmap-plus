/* Package spawn-type keys at native dispatch. Three engine routines match
 * spawn-type bits: the spawn manager's entry selection (a uniform pick among
 * conductor rows sharing a bit with a request), the global modifier lookup
 * (the first modifier row meeting an AI's type) and the AI filter test. Their
 * replacements reproduce the native routines exactly for stock rows and
 * requests and apply the private-key contract of package_keys.h to rows with
 * package keys. Vulkan 0x141123830 / 0x14110faa0 / 0x141268070, OpenGL
 * 0x141115610 / 0x141101890 / 0x141259f70; the bodies match apart from call
 * and global displacements, which are decoded from the verified bytes. */
#include <windows.h>
#include <xmmintrin.h>
#include <string.h>
#include <stdlib.h>
#include "spawn_keys.h"
#include "package_keys.h"
#include "hook.h"

void backend_log(const char *message);

typedef uint32_t (*sk_type_fn)(void *conductor, void *ai);
typedef void (*sk_health_fn)(void *conductor, void *ai, float value);
typedef void *(*sk_resolve_fn)(void *owner, int handle);
typedef uint32_t (*sk_ai_type_fn)(void *filter, void *ai);

/* Native layout shared by both builds. */
#define SK_ENTRIES 0x5908        /* spawn manager: idSpawnManagerEntityInfo list */
#define SK_ENTRY_COUNT 0x5910
#define SK_ENTRY_STRIDE 0x30
#define SK_SEED 0x285be8         /* game: spawn selection random state */
#define SK_MODIFIERS 0x2210      /* conductor: global modifier rows */
#define SK_MODIFIER_COUNT 0x2218
#define SK_MODIFIER_STRIDE 12    /* healthMod, damageMod, type mask */
#define SK_DAMAGE_SCALE 0x23d6c  /* AI: outgoing damage scale */
#define SK_FILTER_MASK 0xc9c     /* AI filter: type mask */
#define SK_FILTER_CACHED 0xca4   /* AI filter: cached entity, -1 for none */
#define SK_CACHE_OWNER 0x298128  /* game: owner of cached entity handles, +0x48 */

static void **g_game;
static sk_type_fn g_type;
static sk_health_fn g_health;
static sk_resolve_fn g_resolve;
static sk_ai_type_fn g_ai_type;
static float g_low, g_high;
static void *g_select, *g_modifier, *g_filter;
static volatile LONG g_active;

/* Immutable dispatch programs, held by two publication slots. Switching
 * retains an already prepared program without allocating. Reusing a slot
 * waits for its native readers before releasing that slot's old program. */
struct sh_spawn_keys_program {
    volatile LONG references;
    sh_package_key_groups groups;
    uint32_t *unmodified;
    size_t unmodified_count;
    uint32_t *exact;
    size_t exact_count;
};
static sh_spawn_keys_program *g_programs[2];
static const sh_package_key_groups g_empty_groups;
static volatile LONG g_group_index, g_group_readers[2];
static const sh_package_key_groups *sk_groups_enter(LONG *held)
{
    for (;;) {
        LONG index = InterlockedCompareExchange(&g_group_index, 0, 0) & 1;
        InterlockedIncrement(g_group_readers + index);
        if ((InterlockedCompareExchange(&g_group_index, 0, 0) & 1) == index) {
            *held = index; return g_programs[index] ? &g_programs[index]->groups : &g_empty_groups;
        }
        InterlockedDecrement(g_group_readers + index);
    }
}
static void sk_groups_leave(LONG held) { InterlockedDecrement(g_group_readers + held); }
static int sk_type_order(const void *a, const void *b)
{ uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static int sk_copy_types(const uint32_t *input, size_t count, uint32_t **output, size_t *unique)
{
    if ((count && !input) || count > SIZE_MAX / sizeof(*input)) return 0;
    if (!count) return 1;
    *output = malloc(count * sizeof(**output));
    if (!*output) return 0;
    memcpy(*output, input, count * sizeof(**output));
    qsort(*output, count, sizeof(**output), sk_type_order);
    for (size_t i = 0; i < count; i++)
        if (!*unique || (*output)[*unique - 1] != (*output)[i]) (*output)[(*unique)++] = (*output)[i];
    return 1;
}
sh_spawn_keys_program *sh_spawn_keys_program_create_exact(const sh_package_key_groups *groups,
    const uint32_t *unmodified_types, size_t unmodified_count, const uint32_t *exact_types, size_t exact_count)
{
    sh_spawn_keys_program *program = calloc(1, sizeof(*program));
    if (!program) return NULL;
    program->references = 1;
    if (groups) program->groups = *groups;
    if (!sk_copy_types(unmodified_types, unmodified_count, &program->unmodified, &program->unmodified_count) ||
        !sk_copy_types(exact_types, exact_count, &program->exact, &program->exact_count)) {
        sh_spawn_keys_program_release(program); return NULL;
    }
    return program;
}
sh_spawn_keys_program *sh_spawn_keys_program_create(const sh_package_key_groups *groups,
    const uint32_t *unmodified_types, size_t count)
{ return sh_spawn_keys_program_create_exact(groups, unmodified_types, count, NULL, 0); }
void sh_spawn_keys_program_release(sh_spawn_keys_program *program)
{
    if (program && !InterlockedDecrement(&program->references)) {
        free(program->unmodified); free(program->exact); free(program);
    }
}
void sh_spawn_keys_publish_program(sh_spawn_keys_program *program)
{
    LONG next = (InterlockedCompareExchange(&g_group_index, 0, 0) & 1) ^ 1;
    if (program) InterlockedIncrement(&program->references);
    while (InterlockedCompareExchange(g_group_readers + next, 0, 0)) SwitchToThread();
    sh_spawn_keys_program *old = g_programs[next];
    g_programs[next] = program;
    InterlockedExchange(&g_group_index, next);
    sh_spawn_keys_program_release(old);
}
static int sk_unmodified(LONG held, uint32_t type)
{
    const sh_spawn_keys_program *program = g_programs[held];
    return program && program->unmodified_count && bsearch(&type, program->unmodified,
        program->unmodified_count, sizeof(type), sk_type_order) != NULL;
}
static int sk_exact(LONG held, uint32_t type)
{
    const sh_spawn_keys_program *program = g_programs[held];
    return program && program->exact_count && bsearch(&type, program->exact,
        program->exact_count, sizeof(type), sk_type_order) != NULL;
}

static uint64_t sk_select(uint8_t *manager, uint32_t request, uint64_t unused3, uint64_t unused4)
{
    int count = *(int *)(manager + SK_ENTRY_COUNT), matches = 0, pick;
    uint8_t *entries = *(uint8_t **)(manager + SK_ENTRIES), *game;
    uint32_t *seed;
    LONG held;
    const sh_package_key_groups *groups = sk_groups_enter(&held);
    uint64_t chosen = 0;
    (void)unused3; (void)unused4;
    for (int i = 0; i < count; i++)
        if (sh_package_key_request_matches(groups, request, *(uint32_t *)(entries + (size_t)i * SK_ENTRY_STRIDE))) matches++;
    if (matches <= 0) { sk_groups_leave(held); return 0; }
    /* The native pick: one draw of the game's generator, then the match at
     * that index in entry order. */
    game = (uint8_t *)*g_game;
    seed = (uint32_t *)(game + SK_SEED);
    *seed = *seed * 0x19660du + 0x3c6ef35fu;
    pick = (int)((*seed >> 10) & 0x7fffu) % matches;
    for (int i = 0; i < count && !chosen; i++) {
        uint8_t *entry = entries + (size_t)i * SK_ENTRY_STRIDE;
        if (sh_package_key_request_matches(groups, request, *(uint32_t *)entry) && pick-- == 0) chosen = (uint64_t)(uintptr_t)entry;
    }
    sk_groups_leave(held);
    return chosen;
}

/* MAXSS then MINSS with the native operand order, NaN behavior included. */
static float sk_clamp(float value)
{
    __m128 raised = _mm_max_ss(_mm_set_ss(g_low), _mm_set_ss(value));
    return _mm_cvtss_f32(_mm_min_ss(_mm_set_ss(g_high), raised));
}

static void sk_modifier(uint8_t *conductor, uint8_t *ai, uint64_t modifiers)
{
    uint32_t type = g_type(conductor, ai);
    int count = *(int *)(conductor + SK_MODIFIER_COUNT);
    const uint8_t *rows = *(const uint8_t **)(conductor + SK_MODIFIERS);
    float health, damage;
    LONG held;
    const sh_package_key_groups *groups;
    int found = -1;
    memcpy(&health, &modifiers, sizeof(health));
    memcpy(&damage, (const uint8_t *)&modifiers + sizeof(health), sizeof(damage));
    groups = sk_groups_enter(&held);
    if (sk_unmodified(held, type)) { sk_groups_leave(held); return; }
    int exact = sk_exact(held, type);
    for (int i = 0; i < count && found < 0; i++)
        if (exact ? type == *(const uint32_t *)(rows + (size_t)i * SK_MODIFIER_STRIDE + 8) :
            sh_package_key_modifier_matches(groups, type, *(const uint32_t *)(rows + (size_t)i * SK_MODIFIER_STRIDE + 8))) found = i;
    sk_groups_leave(held);
    if (found < 0) return;
    const float *row = (const float *)(rows + (size_t)found * SK_MODIFIER_STRIDE);
    g_health(conductor, ai, sk_clamp(health) * row[0]);
    *(float *)(ai + SK_DAMAGE_SCALE) = _mm_cvtss_f32(_mm_max_ss(_mm_setzero_ps(),
        _mm_set_ss(sk_clamp(damage) * row[1])));
}

static uint8_t sk_filter(uint8_t *filter, void *ai)
{
    int cached = *(int *)(filter + SK_FILTER_CACHED);
    uint32_t mask;
    if (cached == -1) mask = *(uint32_t *)(filter + SK_FILTER_MASK);
    else {
        uint8_t *game = (uint8_t *)*g_game;
        void *owner = *(void **)(*(uint8_t **)(game + SK_CACHE_OWNER) + 0x48);
        mask = g_ai_type(filter, g_resolve(owner, cached));
    }
    uint32_t type = g_ai_type(filter, ai);
    LONG held;
    const sh_package_key_groups *groups = sk_groups_enter(&held);
    uint8_t matched = (uint8_t)sh_package_key_filter_matches(groups, mask, type);
    sk_groups_leave(held);
    return matched;
}

static uint8_t *sk_clean(const sig_result *r, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (uint8_t *)r[i].addr;
    return NULL;
}
static void *sk_call(const uint8_t *site) { int32_t rel; memcpy(&rel, site + 1, 4); return (void *)(site + 5 + rel); }
static const void *sk_rip(const uint8_t *site, size_t prefix, size_t length)
{ int32_t rel; memcpy(&rel, site + prefix, 4); return site + length + rel; }

int sh_spawn_keys_install(const sig_result *r, size_t n)
{
    static const uint8_t seed[] = {0x69, 0x81, 0xe8, 0x5b, 0x28, 0x00, 0x0d, 0x66, 0x19, 0x00};
    static const uint8_t damage[] = {0xf3, 0x0f, 0x11, 0x8e, 0x6c, 0x3d, 0x02, 0x00};
    static const uint8_t load_float[] = {0xf3, 0x0f, 0x10, 0x05};
    static const uint8_t mask[] = {0x8b, 0xb9, 0x9c, 0x0c, 0x00, 0x00};
    uint8_t *select = sk_clean(r, n, "SpawnSelectEntry"), *modifier = sk_clean(r, n, "SpawnGlobalModifier");
    uint8_t *filter = sk_clean(r, n, "SnapFilterAiMatches");
    void **game_filter;
    if (g_active) return 1;
    if (!select || !modifier || !filter) {
        backend_log("SPAWN-KEYS: dispatch routines not found; package AI keys use native bits");
        return 0;
    }
    /* The instructions each replacement reproduces or decodes. */
    if (memcmp(select + 0x11a, "\x48\x8b\x0d", 3) || memcmp(select + 0x121, seed, sizeof(seed)) ||
        modifier[0x15] != 0xe8 || modifier[0x99] != 0xe8 || memcmp(modifier + 0x59, load_float, 4) ||
        memcmp(modifier + 0x7d, load_float, 4) || memcmp(modifier + 0xcd, damage, sizeof(damage)) ||
        memcmp(filter + 0x20, "\x48\x8b\x05", 3) || filter[0x32] != 0xe8 || filter[0x3d] != 0xe8 ||
        filter[0x52] != 0xe8 || sk_call(filter + 0x3d) != sk_call(filter + 0x52) || memcmp(filter + 0x46, mask, sizeof(mask))) {
        backend_log("SPAWN-KEYS: dispatch routines differ from the verified builds; package AI keys use native bits");
        return 0;
    }
    g_game = (void **)sk_rip(select + 0x11a, 3, 7);
    game_filter = (void **)sk_rip(filter + 0x20, 3, 7);
    if (game_filter != g_game) {
        backend_log("SPAWN-KEYS: dispatch routines name different game globals; package AI keys use native bits");
        return 0;
    }
    g_type = (sk_type_fn)sk_call(modifier + 0x15);
    g_health = (sk_health_fn)sk_call(modifier + 0x99);
    g_low = *(const float *)sk_rip(modifier + 0x59, 4, 8);
    g_high = *(const float *)sk_rip(modifier + 0x7d, 4, 8);
    g_resolve = (sk_resolve_fn)sk_call(filter + 0x32);
    g_ai_type = (sk_ai_type_fn)sk_call(filter + 0x3d);
    /* Whole position-independent prologues: 21, 18 and 15 bytes. */
    g_select = hook_prepare(select, (void *)sk_select, 21);
    g_modifier = hook_prepare(modifier, (void *)sk_modifier, 18);
    g_filter = hook_prepare(filter, (void *)sk_filter, 15);
    if (g_select && g_modifier && g_filter && hook_commit(g_select) == B2_PATCH_OK &&
        hook_commit(g_modifier) == B2_PATCH_OK && hook_commit(g_filter) == B2_PATCH_OK) {
        InterlockedExchange(&g_active, 1);
        backend_log("SPAWN-KEYS: private package AI keys installed (selection, modifiers, AI filter)");
        return 1;
    }
    if (g_filter && hook_unpatch(g_filter)) g_filter = NULL;
    if (g_modifier && hook_unpatch(g_modifier)) g_modifier = NULL;
    if (g_select && hook_unpatch(g_select)) g_select = NULL;
    backend_log("SPAWN-KEYS: dispatch replacements could not be installed; package AI keys use native bits");
    return 0;
}

int sh_spawn_keys_active(void) { return InterlockedCompareExchange(&g_active, 0, 0) != 0; }

int sh_spawn_keys_publish_groups(const sh_package_key_groups *library, const sh_package_key_groups *map)
{
    sh_spawn_keys_program *program = sh_spawn_keys_program_create(library, NULL, 0);
    if (!program) return 0;
    if (!sh_package_key_groups_merge(&program->groups, map)) { sh_spawn_keys_program_release(program); return 0; }
    sh_spawn_keys_publish_program(program);
    sh_spawn_keys_program_release(program);
    return 1;
}

#ifdef SH_SPAWN_KEYS_TESTING
void sh_spawn_keys_test_bind(void **game_slot, sh_spawn_keys_type_fn type, sh_spawn_keys_health_fn health,
    sh_spawn_keys_resolve_fn resolve, sh_spawn_keys_ai_type_fn ai_type, float low, float high)
{
    g_game = game_slot; g_type = type; g_health = health; g_resolve = resolve; g_ai_type = ai_type;
    g_low = low; g_high = high;
}
static LONG g_test_held = -1;
const sh_package_key_groups *sh_spawn_keys_test_hold(void)
{ LONG held; const sh_package_key_groups *groups = sk_groups_enter(&held); g_test_held = held; return groups; }
void sh_spawn_keys_test_release(void) { sk_groups_leave(g_test_held); g_test_held = -1; }
int sh_spawn_keys_test_unmodified(uint32_t type)
{
    LONG held; sk_groups_enter(&held);
    int result = sk_unmodified(held, type); sk_groups_leave(held); return result;
}
uint64_t sh_spawn_keys_test_select(void *manager, uint32_t request) { return sk_select(manager, request, 0, 0); }
int sh_spawn_keys_test_exact(uint32_t type)
{
    LONG held; sk_groups_enter(&held);
    int result = sk_exact(held, type); sk_groups_leave(held); return result;
}
void sh_spawn_keys_test_modifier(void *conductor, void *ai, uint64_t modifiers) { sk_modifier(conductor, ai, modifiers); }
int sh_spawn_keys_test_filter(void *filter, void *ai) { return sk_filter(filter, ai); }
#endif
