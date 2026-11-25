/* Coarse pages for mod-owned boards in the renderer's material pool. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "modvt_resident.h"

static unsigned page_record(uint64_t id) { return (unsigned)(id >> 30) & 31u; }
static unsigned page_level(uint64_t id) { return (unsigned)(id >> 26) & 15u; }
static int read_int(const void *p) { int v; memcpy(&v, p, sizeof v); return v; }
static unsigned read_u32(const void *p) { unsigned v; memcpy(&v, p, sizeof v); return v; }
static unsigned read_u16(const void *p) { unsigned short v; memcpy(&v, p, sizeof v); return v; }
static uint8_t *read_ptr(const void *p) { uint8_t *v; memcpy(&v, p, sizeof v); return v; }

/* A square spans 512 pages at mip zero. At mips 10 and 11 several squares share one cell. */
int sh_modvt_served_cell(uint64_t id, unsigned squares)
{
    unsigned level = page_level(id), x = (unsigned)(id >> 2) & 4095u, y = (unsigned)(id >> 14) & 4095u;
    if (level > 11 || x >= (2048u >> level) || y >= (2048u >> level)) return 0;
    if (level <= 9) return !!(squares & (1u << ((y >> (9-level))*4 + (x >> (9-level)))));
    for (unsigned i = 0; i < 16; ++i)
        if ((squares & (1u << i)) && ((i % 4) >> (level-9)) == x && ((i / 4) >> (level-9)) == y) return 1;
    return 0;
}

int sh_modvt_lock_budget(int pages, int percent)
{
    int budget;
    if (pages <= 0 || pages > 65535 || percent <= 0 || percent > 100) return 0;
    budget = pages * percent / 100;
    return budget < SH_VT_MAX_LOCK_PAGES ? budget : SH_VT_MAX_LOCK_PAGES;
}

int sh_modvt_coarse_order(const sh_modvt_cells cells[SH_MODVT_SLOTS], uint64_t *out, int capacity)
{
    int pos[SH_MODVT_SLOTS] = {0}, n = 0;
    for (int level = 11; level >= 0 && n < capacity; --level) {
        int advanced;
        do {
            advanced = 0;
            for (int k = 2; k <= SH_MODVT_LAST_USED && n < capacity; ++k) {
                if (pos[k] >= cells[k].count) continue;
                uint64_t id = cells[k].ids[pos[k]];
                if (page_level(id) != (unsigned)level) continue;
                if (page_record(id) == (unsigned)k && (id & (UINT64_C(1) << 56))) out[n++] = id;
                ++pos[k];
                advanced = 1;
            }
        } while (advanced && n < capacity);
    }
    return n;
}


int sh_modvt_page_state(const uint8_t *pool, uint64_t id)
{
    unsigned key = (unsigned)(id >> 2), coord = key & 0x0fffffffu;
    unsigned h = coord * 0x19660du + 0x3c6ef35fu;
    unsigned mask = read_u32(pool + 0x28);
    const uint8_t *heads = read_ptr(pool + 0x30), *table = read_ptr(pool + 0x38), *pages = read_ptr(pool + 8);
    int n = read_int(pool + 0xa8);
    if (!heads || !table || !pages || n <= 0 || n > 65535 || mask > 65535 || (mask & (mask + 1)) != 0) return -1;
    unsigned index = read_u16(heads + 2 * ((h ^ ((h ^ (h >> 11)) >> 11)) & mask));
    for (int i = 0; index != 65535; ++i) {
        if (index >= (unsigned)n || i >= n) return -1;
        const uint8_t *entry = table + 8 * index;
        if (read_u32(entry) == key) {
            const uint8_t *page = pages + 64 * index;
            if (read_u16(entry + 4) == 0 || read_u32(page + 8) != page_record(id)) return -1;
            return (page[0] & 2) ? 2 : 1;
        }
        index = read_u16(entry + 6);
    }
    return 0;
}

int sh_modvt_coarse_batch(const uint8_t *pool, int budget, const uint64_t *ids, int count,
                          uint32_t trusted, uint64_t *out, int capacity)
{
    int n = 0, locked = read_int(pool + 0x90);
    if (locked < 0 || locked >= budget || read_int(pool) != 0 || read_int(pool + 0x80) == 0) return 0;
    if (capacity > budget - locked) capacity = budget - locked;
    for (int i = 0; i < count && n < capacity; ++i) {
        uint64_t id = ids[i];
        unsigned k = page_record(id), level = page_level(id);
        if (k < 2 || k > SH_MODVT_LAST_USED || !(trusted & (1u << k))) continue;
        int state = sh_modvt_page_state(pool, id);
        if (state < 0 || state == 2) continue;
        if (!state && level < 11) {
            unsigned x = (unsigned)(id >> 2) & 0xfff, y = (unsigned)(id >> 14) & 0xfff;
            uint64_t parent = ((uint64_t)k << 30) | ((uint64_t)(level + 1) << 26) |
                              ((uint64_t)(y / 2) << 14) | ((uint64_t)(x / 2) << 2);
            if (sh_modvt_page_state(pool, parent) != 2) continue;
        }
        out[n++] = id;
    }
    return n;
}


char sh_modvt_feedback_with_prefix(sh_modvt_feedback_fn consume, void *sys, const uint64_t *input, int *cursor,
                                   const uint64_t *prefix, int extra)
{
    if (extra <= 0 || input[0] > SH_VT_MAX_LOCK_PAGES || *cursor != 0) return consume(sys, input, cursor, 0, 0);
    size_t original = (size_t)input[0];
    uint64_t *local = malloc((6 + original + extra) * sizeof *local);
    if (!local) return consume(sys, input, cursor, 0, 0);
    memcpy(local, input, 6 * sizeof *local);
    local[0] += extra;
    memcpy(local + 6, prefix, extra * sizeof *local);
    memcpy(local + 6 + extra, input + 6, original * sizeof *local);
    int consumed = 0;
    char result = consume(sys, local, &consumed, 0, 0);
    *cursor = consumed > extra ? consumed - extra : 0;
    free(local);
    return result;
}

#ifndef SH_MODVT_PURE
#include <intrin.h>
#include "backend_log.h"
#include "engine_build.h"
#include "engine_cvar_read.h"
#include "engine_globals.h"
#include "hook.h"

typedef int (*cells_fn)(void *, int, unsigned, sh_modvt_pages *);
static sh_modvt_feedback_fn g_feedback;
static cells_fn g_cells;
static const void *g_frame_return, *g_cvar_slot;
static void *g_percent, *g_minimum;
static SRWLOCK g_plan_lock = SRWLOCK_INIT;
static struct {
    uint8_t *pool;
    void *readers[SH_MODVT_SLOTS];
    unsigned version;
    int budget, minimum, count;
    uint64_t ids[SH_VT_MAX_LOCK_PAGES];
} g_plan;
static ULONGLONG g_next_source_check, g_next_page_check, g_next_busy_issue;
static int g_previous_locks = -1;

static void build_plan(uint8_t *pool, void *const readers[SH_MODVT_SLOTS], unsigned version, int budget, int minimum)
{
    sh_modvt_cells cells[SH_MODVT_SLOTS] = {0};
    sh_modvt_pages *scratch = budget ? malloc(sizeof *scratch) : NULL;
    if (budget && !scratch) return;
    int ok = 1;
    for (int k = 2; k <= SH_MODVT_LAST_USED && budget && ok; ++k) {
        if (!readers[k]) continue;
        scratch->count = 0;
        __try { g_cells(sh_modvt_engine_record(k), SH_VT_MAX_LOCK_PAGES, (unsigned)minimum, scratch); }
        __except (EXCEPTION_EXECUTE_HANDLER) { scratch->count = 0; }
        if (!scratch->count || scratch->count > SH_VT_MAX_LOCK_PAGES) continue;
        unsigned squares = sh_modvt_served_squares(k);
        int n = 0;
        for (unsigned i = 0; i < scratch->count && n < budget; ++i)
            if (sh_modvt_served_cell(scratch->id[i], squares)) scratch->id[n++] = scratch->id[i];
        if (!n) continue;
        size_t bytes = (size_t)n * sizeof *scratch->id;
        uint64_t *ids = malloc(bytes);
        if (!ids) { ok = 0; break; }
        memcpy(ids, scratch->id, bytes);
        cells[k].ids = ids;
        cells[k].count = n;
    }
    if (ok) {
        AcquireSRWLockExclusive(&g_plan_lock);
        g_plan.count = sh_modvt_coarse_order(cells, g_plan.ids, budget);
        g_plan.pool = pool;
        memcpy(g_plan.readers, readers, sizeof g_plan.readers);
        g_plan.version = version;
        g_plan.minimum = minimum;
        g_plan.budget = budget;
        g_next_page_check = 0;
        g_previous_locks = -1;
        ReleaseSRWLockExclusive(&g_plan_lock);
    }
    for (int i = 2; i <= SH_MODVT_LAST_USED; ++i) free((void *)cells[i].ids);
    free(scratch);
}

void sh_modvt_resident_frame(void)
{
    if (!g_cells || !g_feedback || (!sh_modvt_owned_count() && !g_plan.count)) return;
    unsigned version = sh_modvt_source_version();
    ULONGLONG now = GetTickCount64();
    if (version == g_plan.version && now < g_next_source_check) return;
    g_next_source_check = now + 1000;
    void *readers[SH_MODVT_SLOTS] = {0};
    uint8_t *pool = NULL;
    int budget = 0, minimum = 12;
    if (sh_modvt_owned_count()) {
        if (!g_percent) g_percent = sh_engine_cvar_find(g_cvar_slot, SH_IDCVAR_NAME_FOR_LOOKUP, "vt_maxLockedPagesPercent");
        if (!g_minimum) g_minimum = sh_engine_cvar_find(g_cvar_slot, SH_IDCVAR_NAME_FOR_LOOKUP, "vt_minLockedVmtrLOD");
        uint8_t *stock = sh_modvt_engine_record(1);
        int percent = 0, floor = 12;
        if (stock && sh_engine_cvar_read_int(g_percent, &percent) && sh_engine_cvar_read_int(g_minimum, &floor)) {
            __try {
                pool = read_ptr(stock + SH_VT_REC_CACHE);
                if (pool && read_int(pool) == 0 && floor >= 0 && floor < 12) {
                    minimum = floor;
                    budget = sh_modvt_lock_budget(read_int(pool + 0xa8), percent);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { pool = NULL; budget = 0; }
        }
        if (budget) for (int k = 2; k <= SH_MODVT_LAST_USED; ++k) readers[k] = sh_modvt_owned_reader(k, pool);
    }
    if (g_plan.version != version || pool != g_plan.pool || budget != g_plan.budget || minimum != g_plan.minimum ||
        memcmp(readers, g_plan.readers, sizeof readers))
        build_plan(pool, readers, version, budget, minimum);
}

/* The normal frame consumes this private list once. The async transcode jobs retain engine-owned page data.
 * Unserved requests die with the call; after a restart admission is recomputed from the current pool. */
static int coarse_prefix(void *sys, int has_demand, uint64_t *out)
{
    int n = 0;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_plan_lock);
    __try {
        uint8_t *stock = sh_modvt_engine_record(1), *pool = g_plan.pool;
        if (sys == sh_modvt_engine_sys() && g_plan.count && stock && pool && read_ptr(stock + SH_VT_REC_CACHE) == pool &&
            g_plan.version == sh_modvt_source_version()) {
            int locks = read_int(pool + 0x90);
            if ((!has_demand || now >= g_next_busy_issue) && (now >= g_next_page_check || locks != g_previous_locks)) {
                uint32_t trusted = 0;
                for (int k = 2; k <= SH_MODVT_LAST_USED; ++k)
                    if (g_plan.readers[k] && sh_modvt_owned_reader(k, pool) == g_plan.readers[k]) trusted |= 1u << k;
                n = sh_modvt_coarse_batch(pool, g_plan.budget, g_plan.ids, g_plan.count, trusted, out, has_demand ? 1 : 4);
                if (n && has_demand) g_next_busy_issue = now + 50;
                g_previous_locks = locks;
                g_next_page_check = now + (n ? (has_demand ? 50 : 0) : 500);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    ReleaseSRWLockExclusive(&g_plan_lock);
    return n;
}

static char frame_feedback(void *sys, const uint64_t *input, int *cursor, char flush, char lock_all)
{
    uint64_t prefix[4];
    if (_ReturnAddress() != g_frame_return || flush || lock_all || !input || !cursor || *cursor ||
        !sh_modvt_owned_count() || input[0] > SH_VT_MAX_LOCK_PAGES)
        return g_feedback(sys, input, cursor, flush, lock_all);
    int extra = coarse_prefix(sys, input[0] != 0, prefix);
    if (!extra) return g_feedback(sys, input, cursor, flush, lock_all);
    return sh_modvt_feedback_with_prefix(g_feedback, sys, input, cursor, prefix, extra);
}

void sh_modvt_resident_install(const sig_result *site, const sig_result *provider, const uint8_t *base)
{
    static const uint8_t saves[] = {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24};
    if (!site || !provider || site->status != SIG_OK || provider->status != SIG_OK) return;
    const uint8_t *call = (const uint8_t *)site->addr + 28;
    if (*call != 0xe8) return;
    const uint8_t *consumer = call + 5 + read_int(call + 1);
    if (memcmp(consumer, saves, sizeof saves)) return;
    g_cvar_slot = (const void *)glb_resolve(base, "cvar_system_slot", NULL);
    if (!g_cvar_slot) return;
    g_cells = (cells_fn)provider->addr;
    g_frame_return = call + 5;
    g_feedback = (sh_modvt_feedback_fn)hook_prepare((void *)consumer, (void *)frame_feedback, 21);
    if (!g_feedback || hook_commit((void *)g_feedback) != B2_PATCH_OK) {
        if (g_feedback && hook_unpatch((void *)g_feedback)) g_feedback = NULL;
        g_cells = NULL;
        backend_log("VT: coarse residency unavailable; boards continue to stream normally");
    }
}
#endif
