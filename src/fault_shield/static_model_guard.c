/* Static render models never build from a surface table that is not a static
 * model's (static_model_guard.h). The update of a static render model
 * ("StaticModelUpdate", the idRenderModelStatic vtable +0x78 method) copies
 * its backing's surface table when it rebuilds, and reads it when its skin or
 * custom material changed. Before the original runs, the guard works out which
 * backing it will read (after the reload counter moved, the engine looks the
 * backing up again by name; the guard makes that same call first), checks that
 * table and, when it is not a static model's, points the render model at the
 * engine's own "_empty" model with the never-built marker, so the engine
 * rebuilds an empty copy. The render model draws nothing for the rest of the
 * map; nothing is freed; one line names the backing in the product's logs.
 *
 * Healthy models pay only a few loads per update: the table is checked only
 * when the engine is about to read it. The original runs outside every __try,
 * so the engine's own C++ throws still reach its Frame catch.
 *
 * Page checks cannot prove memory is the object it claims to be; the table
 * check uses two invariants of the engine's own code instead. A backing
 * surface's material is an engine object (its first word is a vtable inside
 * DOOM's image), and a backing surface never holds vertex-cache slots: the
 * render model's teardown frees the slots of every surface it copied, so a
 * slot in a backing would be freed by every render model built from it. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "static_model_guard.h"
#include "engine_layout.h"
#include "fault_record.h"
#include "hook.h"
#include "../backend/backend_log.h"

typedef uint64_t (*smu_fn)(void *self);
typedef void *(*materialize_fn)(void *list, const char *name, char make_default, char quiet);

static smu_fn          g_orig_smu;
static materialize_fn  g_materialize;
static void           *g_list;          /* the "model" resource list */
static const int      *g_counter;       /* its reload counter (list +0x60) */
static uintptr_t       g_image_lo, g_image_hi;
static uintptr_t       g_rva;
static void *volatile  g_empty;
static volatile LONG   g_fires;
static volatile LONG   g_lines;
static SRWLOCK         g_seen_lock = SRWLOCK_INIT;
static const void     *g_seen[64];
static int             g_nseen;

#define SMG_LOG_MAX 32

long sh_static_model_guard_fires(void) { return g_fires; }

/* Every page of [p, p+n) committed and readable. */
static int smg_readable(const void *p, size_t n)
{
    const uint8_t *a = (const uint8_t *)p, *end = a + (n ? n : 1);
    if (!p || end < a) return 0;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        const uint8_t *next;
        if (!VirtualQuery(a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        next = (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) return 0;
        a = next;
    }
    return 1;
}

/* NULL when `backing` holds a static model's surface table, else why not
 * (*surface: the first bad surface, -1 for the header). */
static const char *smg_table_problem(const uint8_t *backing, int *surface)
{
    const uint8_t *table;
    int count, cap, i;
    *surface = -1;
    __try {
        table = *(const uint8_t *const *)(backing + SM_SURFACES_OFF);
        count = *(const int *)(backing + SM_SURFACE_COUNT_OFF);
        cap = *(const int *)(backing + SM_SURFACE_CAP_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "the model cannot be read"; }
    if (count == 0) return NULL;
    if (count < 0 || count > SM_SURFACES_MAX) return "its surface count is out of range";
    if (cap < count) return "its surface count exceeds the table's capacity";
    if (!table) return "its surface table is missing";
    if (!smg_readable(table, (size_t)count * SM_SURFACE_STRIDE)) return "its surface table is not readable memory";
    for (i = 0; i < count; ++i) {
        const uint8_t *s = table + (size_t)i * SM_SURFACE_STRIDE;
        const uint8_t *material, *geometry;
        uintptr_t vtable;
        *surface = i;
        __try {
            material = *(const uint8_t *const *)(s + SMS_MATERIAL_OFF);
            geometry = *(const uint8_t *const *)(s + SMS_GEOMETRY_OFF);
            if (*(const uint64_t *)(s + SMS_CACHE_A_OFF) || *(const uint64_t *)(s + SMS_CACHE_B_OFF) ||
                *(const uint64_t *)(s + SMS_CACHE_C_OFF))
                return "a surface holds vertex-cache slots only a render model's own surfaces hold";
        } __except (EXCEPTION_EXECUTE_HANDLER) { return "a surface cannot be read"; }
        if (material) {
            __try { vtable = *(const uintptr_t *)material; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return "a surface's material cannot be read"; }
            if (vtable < g_image_lo || vtable >= g_image_hi) return "a surface's material is not an engine object";
        }
        if (geometry && !smg_readable(geometry, SMS_GEOMETRY_SIZE)) return "a surface's geometry is not readable memory";
    }
    *surface = -1;
    return NULL;
}

/* The engine's built-in empty static model, looked up once. */
static void *smg_empty(void)
{
    void *e = g_empty;
    if (e || !g_materialize || !g_list) return e;
    e = g_materialize(g_list, "_empty", 1, 1);
    if (e) {
        int surface;
        if (smg_table_problem((const uint8_t *)e, &surface)) return NULL;
        InterlockedCompareExchangePointer((void *volatile *)&g_empty, e, NULL);
    }
    return g_empty;
}

/* A printable copy of the resource's name, or "?" */
static void smg_name(const uint8_t *res, char *out, size_t cap)
{
    size_t i = 0;
    out[0] = '?';
    out[1] = 0;
    __try {
        const char *name = *(const char *const *)(res + RES_NAME_OFF);
        if (!name) return;
        for (; i + 1 < cap && name[i]; ++i) out[i] = (name[i] >= 0x20 && name[i] < 0x7f) ? name[i] : '?';
        out[i] = 0;
        if (!i) { out[0] = '?'; out[1] = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (!i) { out[0] = '?'; out[1] = 0; } else out[i] = 0; }
}

static void smg_report(const uint8_t *backing, const char *why, int surface)
{
    char name[160], m[480];
    unsigned level = 0, state = 0, refcounted = 0;
    int refs = 0, count = 0, i, fresh = 1;
    LONG line;
    AcquireSRWLockExclusive(&g_seen_lock);
    for (i = 0; i < g_nseen; ++i) if (g_seen[i] == backing) fresh = 0;
    if (fresh && g_nseen < (int)(sizeof g_seen / sizeof g_seen[0])) g_seen[g_nseen++] = backing;
    ReleaseSRWLockExclusive(&g_seen_lock);
    if (!fresh) return;
    line = InterlockedIncrement(&g_lines);
    if (line > SMG_LOG_MAX + 1) return;
    if (line == SMG_LOG_MAX + 1) {
        shield_fault f = { "load", -1, "static-model-guard: more than 32 models drawn empty this session -- "
                           "suppressing further lines", g_rva, 0 };
        shield_emit(&f);
        return;
    }
    smg_name(backing, name, sizeof name);
    __try {
        level = *(const unsigned *)(backing + RES_LEVEL_OFF);
        state = *(const uint8_t *)(backing + RES_STATE_OFF);
        refcounted = *(const uint8_t *)(backing + SM_REFCOUNTED_OFF);
        refs = *(const int *)(backing + SM_REFCOUNT_OFF);
        count = *(const int *)(backing + SM_SURFACE_COUNT_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    _snprintf_s(m, sizeof m, _TRUNCATE,
        "static-model-guard: model '%s' (resource %p, level %u, state 0x%02x, refcounted %u, refs %d, "
        "%d surfaces) is not a static model's surface table (surface %d: %s); a render model built from it "
        "is drawn empty instead of faulting", name, (const void *)backing, level, state, refcounted, refs,
        count, surface, why);
    {
        shield_fault f = { "load", -1, m, g_rva, (uintptr_t)backing };
        shield_emit(&f);
    }
    backend_log(m);
}

static void smg_precheck(uint8_t *self)
{
    const uint8_t *backing, *effective;
    const char *name = NULL, *why;
    int marker, counter, parm_seen, parm, surface;
    const void *skin_seen, *skin;
    void *empty;
    if (!self || !g_counter) return;
    __try {
        backing = *(const uint8_t *const *)(self + RMS_BACKING_OFF);
        marker = *(const int *)(self + RMS_MARKER_OFF);
        counter = *g_counter;
        parm_seen = *(const int *)(self + RMS_PARM_SEEN_OFF);
        parm = *(const int *)(self + RMS_PARM_OFF);
        skin_seen = *(const void *const *)(self + RMS_SKIN_SEEN_OFF);
        skin = *(const void *const *)(self + RMS_SKIN_OFF);
        if (backing && marker != -1 && marker != counter) name = *(const char *const *)(backing + RES_NAME_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!backing) return;                                   /* the engine returns at once */
    if (marker == counter && parm_seen == parm && skin_seen == skin) return;   /* it reads nothing */
    effective = backing;
    if (marker != -1 && marker != counter) {
        /* The engine looks the backing up again by name and copies the table
         * when the pointer changed. Same call, same arguments: the second one,
         * the engine's, finds what this one found. Outside every __try. */
        if (!name || !g_materialize) return;
        effective = (const uint8_t *)g_materialize(g_list, name, 1, 0);
        if (!effective) return;
    }
    why = smg_table_problem(effective, &surface);
    if (!why) return;
    empty = smg_empty();
    if (!empty || empty == effective) return;
    __try {
        *(void **)(self + RMS_BACKING_OFF) = empty;
        *(int *)(self + RMS_MARKER_OFF) = -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    InterlockedIncrement(&g_fires);
    smg_report(effective, why, surface);
}

static uint64_t sh_smu_detour(void *self)
{
    smg_precheck((uint8_t *)self);
    return g_orig_smu(self);    /* OUTSIDE every __try -- the engine's C++ throws must reach its catch */
}

/* The RIP-relative target of the disp32 at code+at whose instruction ends at code+end. */
static const uint8_t *smg_rip_target(const uint8_t *code, size_t at, size_t end)
{
    int32_t disp;
    memcpy(&disp, code + at, sizeof disp);
    return code + end + disp;
}

static void smg_log(const char *msg)
{
    shield_fault f = { "load", -1, msg, g_rva, 0 };
    shield_emit(&f);
    backend_log(msg);
}

int sh_static_model_guard_install(const uint8_t *module_base, const sig_result *results, size_t count)
{
    static const uint8_t head[] = { 0x40, 0x57, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05 };
    const uint8_t *target, *list, *counter;
    void *tramp;
    if (!module_base || !results) return 0;
    if (g_orig_smu) {
        if (hook_is_installed((void *)g_orig_smu)) return 1;
        if (!hook_unpatch((void *)g_orig_smu)) return 0;
        g_orig_smu = NULL;
    }
    target = (const uint8_t *)sig_addr_by_name(results, count, "StaticModelUpdate");
    g_materialize = (materialize_fn)sig_addr_by_name(results, count, "ResourceMaterialize");
    g_rva = target ? (uintptr_t)(target - module_base) : 0;
    if (!target || !g_materialize) {
        smg_log("static-model-guard: StaticModelUpdate or ResourceMaterialize is unresolved on this build -- "
                "NOT installed");
        return 0;
    }
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        g_image_lo = (uintptr_t)module_base;
        g_image_hi = g_image_lo + nt->OptionalHeader.SizeOfImage;
        if (memcmp(target, head, sizeof head) || target[SMU_COUNTER_CMP_OFF] != 0x3B ||
            target[SMU_COUNTER_CMP_OFF + 1] != 0x05 || target[SMU_LIST_LEA_OFF] != 0x48 ||
            target[SMU_LIST_LEA_OFF + 1] != 0x8D || target[SMU_LIST_LEA_OFF + 2] != 0x0D) target = NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { target = NULL; }
    if (!target) {
        smg_log("static-model-guard: StaticModelUpdate's head, counter compare or list address differs -- "
                "NOT installed (re-derive the offsets for this build)");
        return 0;
    }
    counter = smg_rip_target(target, SMU_COUNTER_CMP_OFF + 2, SMU_COUNTER_CMP_OFF + 6);
    list = smg_rip_target(target, SMU_LIST_LEA_OFF + 3, SMU_LIST_LEA_OFF + 7);
    if (counter != list + 0x60) {
        smg_log("static-model-guard: the counter StaticModelUpdate compares is not its list's reload counter -- "
                "NOT installed");
        return 0;
    }
    g_list = (void *)list;
    g_counter = (const int *)counter;
    tramp = hook_prepare_rip_load((void *)target, (void *)sh_smu_detour, SMU_STOLEN, SMU_COOKIE_LOAD_OFF);
    if (!tramp) {
        smg_log("static-model-guard: trampoline preparation failed -- NOT installed");
        return 0;
    }
    g_orig_smu = (smu_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_orig_smu = NULL;
        smg_log("static-model-guard: commit failed; retained callbacks require restoration");
        return 0;
    }
    smg_log("static-model-guard: armed -- static render models are drawn empty rather than built from a "
            "surface table that is not a static model's");
    return 1;
}
