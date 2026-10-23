/* Rebuild the native entity palette after each successful decl registration
 * pass. Registration alone does not update this catalog, which by-name
 * lookups and map validation consume. Stale entries can report "not found in palette".
 * The builder replaces its previous array.
 *
 * The decl server calls synchronously on the main thread in process-heap scope.
 * Resource-level promotion alone cannot retain the palette's array and strings.
 * Missing signatures,
 * invalid objects, vtable checks or native exceptions enter terminal REFUSED;
 * later rearm attempts do not retry this service.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include "backend_log.h"
#include "engine_globals.h"   /* the editor singleton, located from the code site that computes it */
#include "host_image.h"
#include "iface_engine.h"
#include "palette_refresh.h"
#include "package_editor.h"
#include "hook.h"

/* PR_EDITOR_PALETTE_OFF is the supported structure offset. RVAs below are
 * pinned Vulkan audit references, not lookup gates. Resolve the builder by
 * clean signature and editor_singleton through engine_globals. The OpenGL
 * singleton RVA is 0x309B588.
 */
#define PR_EDITOR_SINGLETON_RVA 0x3056748u
#define PR_EDITOR_PALETTE_OFF   0x20660u
#define PR_PALETTE_VTABLE_RVA   0x20499A0u
#define PR_BUILDER_RVA          0x54AEE0u

enum {
    PR_STATE_IDLE = 0,
    PR_STATE_PENDING,
    PR_STATE_APPLIED,
    PR_STATE_REFUSED
};

typedef void (*palette_build_fn)(void *palette, void *progress);

static volatile LONG g_state = PR_STATE_IDLE;
static const uint8_t *g_module_base;
static palette_build_fn g_builder;
typedef void (*traversal_decode_fn)(void *entity, void *data);
static traversal_decode_fn g_traversal_original;
static volatile LONG g_template_thread;
/* Anonymous universal templates are skipped, as R017 established. Setting this
 * to 1 forwards them too, which attributes a palette-build failure to the
 * guard instead of to the builder's inputs. */
static int g_forward_all = 0;

typedef void *(*palette_find_fn)(void *, const char *);
typedef void *(*palette_entry_ctor_fn)(void *, void *);
typedef unsigned char (*palette_entry_valid_fn)(void *);
typedef void (*palette_entry_dtor_fn)(void *);
typedef struct pr_native_list {
    void *data;
    int count, capacity;
    unsigned short granularity;
    unsigned char tag, kind;
    unsigned int padding;
} pr_native_list;
typedef struct pr_native_palette {
    void *vtable;
    int selected;
    unsigned int padding;
    pr_native_list bodies;
    void *end;
    pr_native_list groups, labels;
} pr_native_palette;
typedef char pr_palette_extent_check[sizeof(pr_native_palette) == 0x60 ? 1 : -1];
typedef struct pr_authoring {
    pr_native_palette expected, spare;
    void *array;
    unsigned long rebuild;
    int swapped, accepted, owns_spare;
} pr_authoring;
typedef unsigned char (*palette_resize_fn)(void *, int);
typedef void (*palette_grow_fn)(void *);
typedef void *(*palette_copy_fn)(void *, const void *);
typedef void (*palette_string_copy_fn)(void *, const void *);
typedef unsigned char (*palette_add_prepare_fn)(void *, void *);
typedef void (*palette_filter_prepare_fn)(void *, void *);
static palette_resize_fn g_palette_resize;
static palette_grow_fn g_group_grow, g_label_grow;
static palette_copy_fn g_body_copy;
static palette_string_copy_fn g_string_copy;
static palette_entry_dtor_fn g_palette_destroy;
static palette_add_prepare_fn g_add_prepare;
static palette_filter_prepare_fn g_filter_prepare, g_widget_refresh;
static void *g_mode_vtable, *g_add_vtable, *g_filter_vtable;
static unsigned long g_palette_rebuild;
/* The rows the last completed rebuild produced (sh_palette_refresh_current):
 * its row array and count, and each row's editor declaration (+0) and entity
 * definition (+0x150), recorded at that rebuild. */
static void *g_built_rows;
static int g_built_count;
static void **g_built_identity;
static unsigned long g_built_rebuild;
typedef struct pr_entry {
    struct pr_entry *next;
    char *name;
    void *decl, *body, *palette;
    size_t owners, published;
    int constructed, quarantined;
} pr_entry;
struct sh_palette_entries {
    volatile LONG refs;
    pr_entry **items;
    size_t count;
    void *palette;
    int published, quarantined;
    pr_authoring *authoring;
};
static palette_find_fn g_find_original;
static palette_entry_ctor_fn g_entry_ctor;
static palette_entry_valid_fn g_entry_valid;
static palette_entry_dtor_fn g_entry_dtor;
static SRWLOCK g_entries_lock = SRWLOCK_INIT;
static pr_entry *g_entries;
static volatile LONG g_entries_installed, g_entries_busy, g_entries_faulted;

/* R033: all five paired direct consumers use only the returned template's
 * declaration. Preserve native authority and its exact end sentinel. The
 * admission owner quiesces those consumers before final template release. */
static void *volatile g_steer;
static volatile LONG64 g_lookup_epoch;
void sh_palette_refresh_steer(sh_palette_steer_fn steer)
{
    if (InterlockedExchangePointer(&g_steer, (void *)steer) != (void *)steer)
        InterlockedIncrement64(&g_lookup_epoch);
}
static void *volatile g_rows_retired;
void sh_palette_refresh_rows_retired(void (*retired)(void)) { InterlockedExchangePointer(&g_rows_retired, (void *)retired); }
static void pr_rows_retired(void)
{
    InterlockedIncrement64(&g_lookup_epoch);
    void (*retired)(void) = (void (*)(void))g_rows_retired;
    if (retired) retired();
}
static void *pr_find_entity(void *palette, const char *name)
{
    void *result = g_find_original ? g_find_original(palette, name) : NULL;
    void *end = NULL;
    __try { if (palette) end = *(void **)((unsigned char *)palette + 0x28); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return result; }
    if (result != end && name) {
        sh_palette_steer_fn steer = (sh_palette_steer_fn)g_steer;
        return steer ? (void *)steer(palette, name, result, NULL) : result;
    }
    if (!name) return result;
    AcquireSRWLockShared(&g_entries_lock);
    for (pr_entry *entry = g_entries; entry; entry = entry->next) {
        if (entry->published && entry->palette == palette && !entry->quarantined &&
            !strcmp(entry->name, name)) { result = entry->body; break; }
    }
    ReleaseSRWLockShared(&g_entries_lock);
    return result;
}

/* Anonymous palette defaults have no traversal to add. Both images use
 * entityDef@entity+150, name@decl+8 and className@decl+60. For the universal
 * class the native branch only warns; real entity decoding must still run. */
static int pr_empty_universal_template(const void *entity)
{
    const unsigned char *decl;
    const char *name, *class_name;
    static const char universal[] = "idInfo_UniversalTraversal";
    if ((DWORD)InterlockedCompareExchange(&g_template_thread, 0, 0) != GetCurrentThreadId()) return 0;
    __try {
        if (!entity || !(decl = *(const unsigned char *const *)((const unsigned char *)entity + 0x150))) return 0;
        name = *(const char *const *)(decl + 8);
        class_name = *(const char *const *)(decl + 0x60);
        return name && !name[0] && class_name && !strncmp(class_name, universal, sizeof(universal));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static volatile LONG g_template_matches, g_template_forwarded;
static int sh_palette_refresh_forward_all(void)
{ return g_forward_all; }
static void pr_decode_traversal(void *entity, void *data)
{
    if (pr_empty_universal_template(entity)) {
        InterlockedIncrement(&g_template_matches);
        if (!sh_palette_refresh_forward_all()) return;
    }
    InterlockedIncrement(&g_template_forwarded);
    if (g_traversal_original) g_traversal_original(entity, data);
}

#ifdef SH_PALETTE_REFRESH_TESTING
static volatile LONG g_test_call_count;
#endif

static const sig_result *pr_result(const sig_result *results, size_t count,
                                   const char *name)
{
    size_t i;
    if (!results || !name) return NULL;
    for (i = 0; i < count; i++)
        if (results[i].name && strcmp(results[i].name, name) == 0)
            return &results[i];
    return NULL;
}

/* 1 = non-null, 0 = null, -1 = the object was not readable. */
static int pr_read_ptr(const void *address, void **out)
{
    if (!address || !out) return -1;
    __try {
        *out = *(void *const *)address;
        return *out ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

/* Check that the palette vtable lies in a readable, non-writable, non-
 * executable host section. This is a plausibility check; unreadable PE
 * headers refuse.
 */
static int pr_address_in_readonly_section(const uint8_t *module_base, const void *address)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        const IMAGE_NT_HEADERS64 *nt;
        const IMAGE_SECTION_HEADER *sec;
        uintptr_t rva;
        unsigned int i;
        if (!module_base || !address || (uintptr_t)address < (uintptr_t)module_base) return 0;
        rva = (uintptr_t)address - (uintptr_t)module_base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        sec = (const IMAGE_SECTION_HEADER *)IMAGE_FIRST_SECTION(nt);
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            uintptr_t start = (uintptr_t)sec->VirtualAddress;
            uintptr_t span  = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            if (rva < start || rva >= start + span) continue;
            return (sec->Characteristics & IMAGE_SCN_MEM_READ) != 0 &&
                   (sec->Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) == 0;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void pr_refuse(const char *reason)
{
    InterlockedExchange(&g_state, PR_STATE_REFUSED);
    backend_log(reason ? reason : "palette-refresh REFUSED");
}

static const unsigned char *pr_relative(const unsigned char *at, const char *prefix, size_t size)
{
    int displacement;
    if (!at || memcmp(at, prefix, size)) return NULL;
    memcpy(&displacement, at + size, sizeof(displacement));
    return at + size + sizeof(displacement) + displacement;
}

static void pr_authoring_install(const sig_result *results, size_t count, const uint8_t *base)
{
    static const char *names[] = {"SnapPaletteArrayResize", "SnapPaletteDestroy",
        "EntityBodyCopyAssignAnchor", "IdStrAssignFromStr", "SnapPaletteAddPrepare",
        "SnapPaletteFpsPrepare", "SnapEntityModeCtor"};
    const unsigned char *at[7], *category, *add_ctor, *filter_ctor;
    for (size_t i = 0; i < 7; i++) {
        const sig_result *r = pr_result(results, count, names[i]);
        if (!r || r->status != SIG_OK || !r->addr || r->addr != (uintptr_t)base + r->rva) return;
        at[i] = (const unsigned char *)r->addr;
    }
    __try {
        /* These call/LEA paths and adjacent field instructions are paired
         * native layout contracts. Resolve targets, never use pinned RVAs. */
        category = pr_relative((const unsigned char *)g_builder + 0x1c2, "\xe8", 1);
        if (!category || memcmp((const unsigned char *)g_builder + 0x1bf, "\x49\x8b\xcf", 3) ||
            memcmp(at[6] + 0x77, "\x48\x8d\x8f\x58\x02\x00\x00", 7) ||
            memcmp(at[6] + 0xcd, "\x48\x8d\x8f\xf0\x03\x00\x00", 7)) return;
        add_ctor = pr_relative(at[6] + 0x7e, "\xe8", 1);
        filter_ctor = pr_relative(at[6] + 0xd4, "\xe8", 1);
        g_mode_vtable = (void *)pr_relative(at[6] + 0x1b, "\x48\x8d\x05", 3);
        g_add_vtable = (void *)pr_relative(add_ctor, "\x48\x8d\x05", 3);
        g_filter_vtable = (void *)pr_relative(filter_ctor, "\x48\x8d\x05", 3);
        g_group_grow = (palette_grow_fn)pr_relative(category + 0xb8, "\xe8", 1);
        g_label_grow = (palette_grow_fn)pr_relative(category + 0x146, "\xe8", 1);
        g_widget_refresh = (palette_filter_prepare_fn)pr_relative(at[5] + 0x71, "\xe9", 1);
        if (!g_mode_vtable || !g_add_vtable || !g_filter_vtable || !g_group_grow ||
            !g_label_grow || !g_widget_refresh) return;
        /* The package-editor adapter already owns this verified entry hook;
         * calling its entry preserves normal body-copy provenance behavior. */
        g_body_copy = (palette_copy_fn)(at[2] - 914);
        g_string_copy = (palette_string_copy_fn)at[3];
        g_palette_destroy = (palette_entry_dtor_fn)at[1];
        g_add_prepare = (palette_add_prepare_fn)at[4];
        g_filter_prepare = (palette_filter_prepare_fn)at[5];
        g_palette_resize = (palette_resize_fn)at[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_palette_resize = NULL; }
}

static void pr_entries_install(const sig_result *results, size_t count, const uint8_t *base)
{
    static const char *names[] = {
        "SnapPaletteFindEntity", "SnapPaletteEntryConstruct", "SnapPaletteEntryValid", "EntityBodyDtor"
    };
    uintptr_t addresses[4] = {0};
    for (size_t i = 0; i < 4; ++i) {
        const sig_result *value = pr_result(results, count, names[i]);
        if (!value || value->status != SIG_OK || !value->addr ||
            value->addr != (uintptr_t)base + value->rva) {
            backend_log("palette additions unavailable: clean native template helpers required");
            return;
        }
        addresses[i] = value->addr;
    }
    g_entry_ctor = (palette_entry_ctor_fn)addresses[1];
    g_entry_valid = (palette_entry_valid_fn)addresses[2];
    g_entry_dtor = (palette_entry_dtor_fn)addresses[3];
    /* Sixteen complete position-independent bytes in both paired images. */
    g_find_original = (palette_find_fn)hook_prepare((void *)addresses[0], pr_find_entity, 16);
    if (!g_find_original) return;
    if (hook_commit((void *)g_find_original) != B2_PATCH_OK) {
        if (hook_unpatch((void *)g_find_original)) g_find_original = NULL;
        backend_log("palette additions unavailable: named lookup hook commit failed");
        return;
    }
    InterlockedExchange(&g_entries_installed, 1);
}

/* ---- Product lookup rows in native palette pickers --------------------------
 * A native picker resets the palette's searchable end to the whole body list
 * and narrows it with keep filters. Name lookups (map validation and repair,
 * module conversion, flat entity construction) instead reset the end and
 * re-sort by entityDef name before searching. The add tool keeps placeable
 * rows, its path-node mode keeps path nodes and the filter picker keeps
 * filters, all through one stable partition (VK 1405477b0, GL 140546fd0)
 * that keeps a row when closure.keep(row) equals *closure.value (R-257).
 * The placeable bit is no internal-row marker for a placeable served row:
 * prefab creation refuses a selection unless each row is placeable, a filter
 * or a logic input/output ("entities in selection can't be duplicated"), and
 * decals read it as artist placement. Rows the product generates for lookup
 * (R-255 variant rows) therefore keep every flag of the row they inherit; after
 * each keep partition they also leave the picker prefix, as unkept rows do.
 * Lookup, load, validation and duplication still read the whole body list. */
typedef char (*pr_row_predicate)(const void *entry);
typedef struct pr_partition_closure {
    const char *value;
    pr_row_predicate keep;
    unsigned char tag;
} pr_partition_closure;
typedef unsigned char *(*palette_partition_fn)(unsigned char *first, unsigned char *last,
                                               pr_partition_closure *closure);
static palette_partition_fn g_partition_original;
static SRWLOCK g_internal_lock = SRWLOCK_INIT;
static char **g_internal_rows;   /* sorted, owned */
static size_t g_internal_count;
static volatile LONG g_internal_unlisted;
static int pr_name_order(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }
void sh_palette_refresh_internal_rows(const char *const *names, size_t count)
{
    char **copy = count ? calloc(count, sizeof(*copy)) : NULL, **old;
    size_t used = 0, old_count;
    for (size_t i = 0; copy && names && i < count; i++) {
        if (!names[i] || !*names[i]) continue;
        if (!(copy[used] = _strdup(names[i]))) break;
        used++;
    }
    if (count && (!copy || !names || used != count)) {
        /* A partial set would list some lookup rows; publish none and say so. */
        for (size_t i = 0; copy && i < used; i++) free(copy[i]);
        free(copy); copy = NULL; used = 0;
        backend_log("palette pickers: lookup row set unavailable; generated rows stay listed");
    }
    if (used > 1) qsort(copy, used, sizeof(*copy), pr_name_order);
    AcquireSRWLockExclusive(&g_internal_lock);
    old = g_internal_rows; old_count = g_internal_count;
    g_internal_rows = copy; g_internal_count = used;
    ReleaseSRWLockExclusive(&g_internal_lock);
    for (size_t i = 0; i < old_count; i++) free(old[i]);
    free(old);
}
/* Caller holds g_internal_lock shared. 1 keeps the row in the picker. */
static char pr_row_listed(const void *entry)
{
    __try {
        const unsigned char *decl = entry ? *(const unsigned char *const *)entry : NULL;
        const char *name = decl ? *(const char *const *)(decl + 8) : NULL;
        if (!name || !*name) return 1;
        return bsearch(&name, g_internal_rows, g_internal_count, sizeof(*g_internal_rows),
                       pr_name_order) ? 0 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}
/* Every keep partition's result becomes the picker's searchable end. Its
 * second pass is the closure partition with the product predicate. */
typedef unsigned char *(*palette_keep_fn)(unsigned char *first, unsigned char *last, void *predicate);
#define PR_KEEP_INSTANCES 10
static palette_keep_fn g_keep_original[PR_KEEP_INSTANCES];
static unsigned char *pr_keep_after(unsigned char *first, unsigned char *end, unsigned char *last)
{
    static const char listed = 1;
    if (!g_partition_original || !end || end <= first || end > last || (size_t)(end - first) % 0x6f0) return end;
    AcquireSRWLockShared(&g_internal_lock);
    if (g_internal_count) {
        pr_partition_closure own = {&listed, pr_row_listed, 0};
        unsigned char *kept = g_partition_original(first, end, &own);
        if (kept && kept >= first && kept <= end && !((size_t)(kept - first) % 0x6f0)) {
            if (kept != end) InterlockedIncrement(&g_internal_unlisted);
            end = kept;
        }
    }
    ReleaseSRWLockShared(&g_internal_lock);
    return end;
}
#define PR_KEEP_DETOUR(n) \
    static unsigned char *pr_keep_##n(unsigned char *first, unsigned char *last, void *predicate) \
    { return pr_keep_after(first, g_keep_original[n](first, last, predicate), last); }
PR_KEEP_DETOUR(0) PR_KEEP_DETOUR(1) PR_KEEP_DETOUR(2) PR_KEEP_DETOUR(3) PR_KEEP_DETOUR(4)
PR_KEEP_DETOUR(5) PR_KEEP_DETOUR(6) PR_KEEP_DETOUR(7) PR_KEEP_DETOUR(8) PR_KEEP_DETOUR(9)
static const palette_keep_fn g_keep_detours[PR_KEEP_INSTANCES] = {pr_keep_0, pr_keep_1, pr_keep_2,
    pr_keep_3, pr_keep_4, pr_keep_5, pr_keep_6, pr_keep_7, pr_keep_8, pr_keep_9};
static void pr_keep_release(size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (g_keep_original[i] && hook_unpatch((void *)g_keep_original[i])) g_keep_original[i] = NULL;
    g_partition_original = NULL;
}
static void pr_pickers_install(const sig_result *results, size_t count, const uint8_t *base)
{
    /* Keep-wrapper template through its predicate LEA opcode, then the two
     * partition prologues: identical in the paired images. A (25 bytes) spills
     * its by-value predicate; B (21 bytes) takes a closure pointer. Both are
     * whole position-independent instructions: pushes, SUB RSP and the MOV of
     * the -2 unwind-state slot. */
    static const unsigned char keep[] = {0x88, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xEC, 0x40,
        0x48, 0x8B, 0x51, 0x28, 0x48, 0x8D, 0x44, 0x24, 0x58, 0x48, 0x89, 0x44, 0x24, 0x20,
        0x4C, 0x8D, 0x44, 0x24, 0x20, 0x48, 0x8D, 0x05};
    static const unsigned char shape_a[] = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41,
        0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x40, 0xFE, 0xFF, 0xFF, 0xFF};
    static const unsigned char shape_b[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x40, 0xFE, 0xFF, 0xFF, 0xFF};
    const sig_result *r = pr_result(results, count, "SnapPaletteFpsPrepare");
    const unsigned char *fps, *wrapper = NULL, *closure_keep = NULL, *found[PR_KEEP_INSTANCES] = {0};
    size_t stolen[PR_KEEP_INSTANCES] = {0}, instances = 0, sites = 0, closure_index = PR_KEEP_INSTANCES, i;
    if (!r || r->status != SIG_OK || !r->addr || r->addr != (uintptr_t)base + r->rva) {
        backend_log("palette pickers: lookup rows stay listed; filter preparation requires a clean SIG_OK resolve");
        return;
    }
    fps = (const unsigned char *)r->addr;
    __try {
        /* The filter picker calls its keep-filter wrapper with value 1; the
         * wrapper calls the closure partition and stores the new end at
         * palette+0x28. */
        if (!memcmp(fps + 0x24, "\xb2\x01\x48\x8b\xcf", 5)) wrapper = pr_relative(fps + 0x29, "\xe8", 1);
        if (wrapper && !memcmp(wrapper, keep, sizeof(keep)) && !memcmp(wrapper + 0x3d, "\x48\x89\x43\x28", 4))
            closure_keep = pr_relative(wrapper + 0x38, "\xe8", 1);
        /* All palette keep wrappers (category, weapons, logic input/output,
         * path node, filter, placeable, tutorial and the selector filters) form
         * one paired cluster from 0x120 before the filter keep to 0x1c0 after
         * it. Each stores a partition result as the searchable end: a CALL,
         * then MOV [RBX|RSI+0x28],RAX, directly or after MOV RBX,[RSP+0x38]. */
        for (const unsigned char *at = closure_keep ? wrapper - 0x120 : NULL;
             at && at + 14 <= wrapper + 0x1c0; at++) {
            const unsigned char *after = at + 5, *target;
            size_t size;
            if (*at != 0xE8) continue;
            if (!memcmp(after, "\x48\x8b\x5c\x24\x38", 5)) after += 5;
            if (memcmp(after, "\x48\x89\x43\x28", 4) && memcmp(after, "\x48\x89\x46\x28", 4)) continue;
            target = pr_relative(at, "\xe8", 1);
            size = !memcmp(target, shape_a, sizeof(shape_a)) ? sizeof(shape_a) :
                   !memcmp(target, shape_b, sizeof(shape_b)) ? sizeof(shape_b) : 0;
            if (!size) { instances = SIZE_MAX; break; }
            sites++;
            for (i = 0; i < instances && found[i] != target; i++) {}
            if (i < instances) continue;
            if (instances == PR_KEEP_INSTANCES) { instances = SIZE_MAX; break; }
            found[instances] = target; stolen[instances++] = size;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { instances = SIZE_MAX; }
    for (i = 0; instances == PR_KEEP_INSTANCES && i < instances; i++)
        if (found[i] == closure_keep && stolen[i] == sizeof(shape_b)) closure_index = i;
    if (instances != PR_KEEP_INSTANCES || sites != 12 || closure_index == PR_KEEP_INSTANCES) {
        backend_log("palette pickers: lookup rows stay listed; keep partition layout differs");
        return;
    }
    for (i = 0; i < instances; i++)
        if (!(g_keep_original[i] = (palette_keep_fn)hook_prepare((void *)found[i], (void *)g_keep_detours[i], stolen[i]))) break;
    if (i < instances) {
        pr_keep_release(i);
        backend_log("palette pickers: lookup rows stay listed; hook preparation failed");
        return;
    }
    /* Publish every trampoline, and the second pass, before any commit. */
    g_partition_original = (palette_partition_fn)g_keep_original[closure_index];
    for (i = 0; i < instances; i++)
        if (hook_commit((void *)g_keep_original[i]) != B2_PATCH_OK) break;
    if (i < instances) {
        pr_keep_release(instances);
        backend_log("palette pickers: lookup rows stay listed; hook commit failed");
        return;
    }
    backend_log("palette pickers: generated lookup rows leave all 10 keep-filtered picker partitions");
}

int sh_palette_refresh_install(const sig_result *results, size_t count,
                               const uint8_t *module_base)
{
    const sig_result *builder, *traversal;

    if (InterlockedCompareExchange(&g_state, PR_STATE_IDLE, PR_STATE_IDLE) != PR_STATE_IDLE ||
        g_builder != NULL || g_traversal_original != NULL)
        return 0;
    builder = pr_result(results, count, "SnapPaletteBuild");
    /* This call has a vtable/data-layout contract, so a hook-tolerant resolve
     * is not acceptable even though it is callable for ordinary leaf calls. */
    if (!builder || builder->status != SIG_OK || !builder->addr || !module_base) {
        pr_refuse("palette-refresh REFUSED: SnapPaletteBuild requires a clean SIG_OK resolve");
        return 0;
    }

    /* Require the resolved address and RVA to describe the same host module. */
    if (builder->addr != (uintptr_t)module_base + builder->rva) {
        pr_refuse("palette-refresh REFUSED: SnapPaletteBuild resolve is not self-consistent with the host image base");
        return 0;
    }

    traversal = pr_result(results, count, "SnapEntityTraversalDecode");
    if (!traversal || traversal->status != SIG_OK || !traversal->addr ||
        traversal->addr != (uintptr_t)module_base + traversal->rva) {
        pr_refuse("palette-refresh REFUSED: clean traversal decoder unavailable");
        return 0;
    }
    /* Eighteen whole position-independent bytes, ending after SUB RSP,0xd0. */
    g_traversal_original = (traversal_decode_fn)hook_prepare((void *)traversal->addr,
                                                           pr_decode_traversal, 18);
    if (!g_traversal_original) {
        pr_refuse("palette-refresh REFUSED: traversal template hook preparation failed");
        return 0;
    }
    if (hook_commit((void *)g_traversal_original) != B2_PATCH_OK) {
        if (hook_unpatch((void *)g_traversal_original)) g_traversal_original = NULL;
        pr_refuse("palette-refresh REFUSED: traversal template hook commit failed");
        return 0;
    }
    g_module_base = module_base;
    g_builder = (palette_build_fn)builder->addr;
    pr_entries_install(results, count, module_base);
    pr_authoring_install(results, count, module_base);
    pr_pickers_install(results, count, module_base);
    backend_log("palette-refresh installed: waiting for complete native decl registration");
    return 1;
}

static void pr_entries_error(char *error, size_t capacity, const char *message)
{ if (error && capacity) snprintf(error, capacity, "%s", message); }

int sh_palette_entries_ready(void)
{
    return InterlockedCompareExchange(&g_entries_installed, 0, 0) &&
        !InterlockedCompareExchange(&g_entries_faulted, 0, 0) &&
        InterlockedCompareExchange(&g_state, 0, 0) != PR_STATE_PENDING;
}

static int pr_entries_admitted(sh_palette_entries_admitted_fn admitted, void *context)
{
    __try { return admitted && admitted(context); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void *pr_entries_palette(void)
{
    const uint8_t *editor = sh_iface_engine_editor_base();
    uintptr_t expected = glb_resolve(g_module_base, "editor_singleton", NULL);
    void *vtable = NULL;
    if (!editor || !expected || (uintptr_t)editor != expected ||
        pr_read_ptr(editor + PR_EDITOR_PALETTE_OFF, &vtable) != 1 ||
        !pr_address_in_readonly_section(g_module_base, vtable)) return NULL;
    return (void *)(editor + PR_EDITOR_PALETTE_OFF);
}

/* Native construction copies the entityDef resource's +8 name into an
 * independently allocated editor defsub. The palette finder reads +0x58
 * from that defsub; callers here hold the original resource instead. */
static const char *pr_entries_decl_name(const void *decl)
{
    const uint8_t *entity = decl ? *(const uint8_t **)((const uint8_t *)decl + 0x1c8) : NULL;
    return entity ? *(const char **)(entity + 8) : NULL;
}

static int pr_entries_identity(const sh_palette_entry_source *source)
{
    __try {
        const char *name = source ? pr_entries_decl_name(source->snap_editor_entity) : NULL;
        return source && source->entity_def_name && source->entity_def_name[0] && name &&
            !strcmp(name, source->entity_def_name);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int pr_entries_native_matches(void *palette, const char *name, void *decl)
{
    __try {
        void *row = g_find_original(palette, name);
        return row == *(void **)((uint8_t *)palette + 0x28) || (row && *(void **)row == decl);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int pr_span(const void *data, size_t size)
{
    uintptr_t at = (uintptr_t)data, end;
    if (!size) return 1;
    if (!at || size > UINTPTR_MAX - at) return 0;
    end = at + size;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)at, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !info.Protect ||
            info.RegionSize > UINTPTR_MAX - (uintptr_t)info.BaseAddress) return 0;
        uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next <= at) return 0;
        at = next;
    }
    return 1;
}
static int pr_list_valid(const pr_native_list *list, size_t stride)
{
    return list->count >= 0 && list->capacity >= list->count &&
        (list->kind == 0 || list->kind == 3 || list->kind == 4) &&
        (!!list->data == !!list->capacity) && (size_t)list->capacity <= SIZE_MAX / stride &&
        pr_span(list->data, (size_t)list->capacity * stride);
}
static int pr_palette_valid(const pr_native_palette *p)
{
    uintptr_t first, end;
    if (!pr_span(p, sizeof(*p)) || !pr_list_valid(&p->bodies, 0x6f0) ||
        !pr_list_valid(&p->groups, sizeof(void *)) || !pr_list_valid(&p->labels, 0x30)) return 0;
    first = (uintptr_t)p->bodies.data; end = (uintptr_t)p->end;
    if (end < first || end - first > (size_t)p->bodies.count * 0x6f0 ||
        (end - first) % 0x6f0 || p->selected < 0 ||
        (end != first && (size_t)p->selected >= (end - first) / 0x6f0)) return 0;
    if (!p->groups.count) return !p->labels.count;
    if (p->groups.count != p->labels.count + 1) return 0;
    void **groups = p->groups.data;
    /* Native reset expands end to all bodies before categories are rebuilt.
     * The existing categories may still cover only a searchable prefix. */
    if ((uintptr_t)groups[0] != first) return 0;
    for (int i = 1; i < p->groups.count; i++)
        if ((uintptr_t)groups[i] < (uintptr_t)groups[i-1] || (uintptr_t)groups[i] > end ||
            ((uintptr_t)groups[i] - first) % 0x6f0) return 0;
    for (int i = 0; i < p->labels.count; i++) {
        const unsigned char *s = (const unsigned char *)p->labels.data + (size_t)i * 0x30;
        int length = *(const int *)(s + 8);
        if (length < 0 || !pr_span(*(const void *const *)(s + 0x10), (size_t)length + 1)) return 0;
    }
    return 1;
}
/* The live editor palette embedded in the editor singleton. Its storage lasts
 * for the process and native rebuilds replace only its rows, so callers read it
 * afresh for each use instead of keeping a pointer seen elsewhere; NULL while
 * the editor or its palette layout is not intact. */
void *sh_palette_refresh_editor_palette(void)
{
    void *palette = pr_entries_palette();
    return palette && pr_palette_valid(palette) ? palette : NULL;
}
/* Record the rows a completed rebuild produced; an unreadable palette records none. */
static void pr_built_record(void)
{
    const pr_native_palette *p = sh_palette_refresh_editor_palette();
    void **identity = NULL;
    free(g_built_identity); g_built_identity = NULL; g_built_rows = NULL; g_built_count = 0;
    if (!p || p->bodies.count <= 0 || (size_t)p->bodies.count > SIZE_MAX / (2 * sizeof(void *)) ||
        !(identity = malloc((size_t)p->bodies.count * 2 * sizeof(void *)))) return;
    __try {
        for (int i = 0; i < p->bodies.count; i++) {
            const unsigned char *body = (const unsigned char *)p->bodies.data + (size_t)i * 0x6f0;
            identity[2 * i] = *(void *const *)body;
            identity[2 * i + 1] = *(void *const *)(body + 0x150);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(identity); return; }
    g_built_identity = identity; g_built_rows = p->bodies.data; g_built_count = p->bodies.count;
    g_built_rebuild = g_palette_rebuild;
}
int sh_palette_refresh_current(void)
{
    const pr_native_palette *p;
    int same = 0;
    if (InterlockedCompareExchange(&g_state, 0, 0) != PR_STATE_APPLIED ||
        InterlockedCompareExchange(&g_entries_busy, 0, 0) || !g_built_identity ||
        g_built_rebuild != g_palette_rebuild || !(p = sh_palette_refresh_editor_palette()) ||
        p->bodies.data != g_built_rows || p->bodies.count != g_built_count) return 0;
    __try {
        same = 1;
        for (int i = 0; i < p->bodies.count && same; i++) {
            const unsigned char *body = (const unsigned char *)p->bodies.data + (size_t)i * 0x6f0;
            same = *(void *const *)body == g_built_identity[2 * i] &&
                *(void *const *)(body + 0x150) == g_built_identity[2 * i + 1];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { same = 0; }
    return same;
}
struct sh_palette_lookup_snapshot {
    char *name;
    void *palette, *body, *declaration;
    pr_native_palette header;
    LONG64 epoch;
};
static int pr_lookup_allowed(void)
{
    sh_palette_steer_fn steer = (sh_palette_steer_fn)g_steer;
    return (!steer || (steer == sh_package_editor_row_steer && !sh_package_editor_row_steering_active())) &&
        sh_palette_entries_ready() && !InterlockedCompareExchange(&g_entries_busy, 0, 0) &&
        InterlockedCompareExchange(&g_state, 0, 0) != PR_STATE_PENDING;
}
/* The paired native comparator uses entityDef+58, with an absent definition
 * yielding the empty key. Every byte read is checked; a bad row is unknown. */
static const char *pr_lookup_key(const unsigned char *body)
{
    const unsigned char *definition;
    const char *key;
    if (!pr_span(body, 0x158)) return NULL;
    definition = *(const unsigned char *const *)(body + 0x150);
    if (!definition) return "";
    if (!pr_span(definition, 0x60)) return NULL;
    key = *(const char *const *)(definition + 0x58);
    if (!key) return NULL;
    for (size_t n = 0;; n++) {
        if (n == SIZE_MAX || (uintptr_t)key > UINTPTR_MAX - n || !pr_span(key + n, 1)) return NULL;
        if (!key[n]) return key;
    }
}
/* Caller holds entries_lock. No stored extension node is dereferenced later. */
static int pr_lookup_select(void *palette, const char *name, void **body, void **decl, pr_native_palette *header)
{
    pr_native_palette *p = palette;
    *body = *decl = NULL;
    if (!pr_lookup_allowed() || palette != sh_palette_refresh_editor_palette()) return -1;
    *header = *p;
    size_t low = 0, high = ((uintptr_t)p->end - (uintptr_t)p->bodies.data) / 0x6f0;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const char *key = pr_lookup_key((unsigned char *)p->bodies.data + mid * 0x6f0);
        if (!key) return -1;
        if (strcmp(key, name) < 0) low = mid + 1; else high = mid;
    }
    if (low < ((uintptr_t)p->end - (uintptr_t)p->bodies.data) / 0x6f0) {
        unsigned char *found = (unsigned char *)p->bodies.data + low * 0x6f0;
        const char *key = pr_lookup_key(found);
        if (!key) return -1;
        if (!strcmp(key, name)) { *body = found; *decl = *(void **)found; return *decl ? 1 : -1; }
    }
    for (pr_entry *entry = g_entries; entry; entry = entry->next)
        if (entry->published && entry->constructed && !entry->quarantined && entry->palette == palette &&
            !strcmp(entry->name, name)) {
            if (!entry->body || *(void **)entry->body != entry->decl || !entry->decl) return -1;
            *body = entry->body; *decl = entry->decl; return 1;
        }
    return 0;
}
int sh_palette_lookup_capture(void *palette, const char *name, sh_palette_lookup_snapshot **out,
    void **declaration, char *error, size_t capacity)
{
    if (declaration) *declaration = NULL;
    if (!out || *out || !declaration || !name) return -1;
    sh_palette_lookup_snapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot || !(snapshot->name = _strdup(name))) { sh_palette_lookup_release(snapshot); return -1; }
    snapshot->palette = palette; int result = -1;
    AcquireSRWLockShared(&g_entries_lock);
    __try {
        snapshot->epoch = InterlockedCompareExchange64(&g_lookup_epoch, 0, 0);
        result = pr_lookup_select(palette, name, &snapshot->body, &snapshot->declaration, &snapshot->header);
        if (result >= 0 && (!pr_lookup_allowed() ||
            snapshot->epoch != InterlockedCompareExchange64(&g_lookup_epoch, 0, 0) ||
            memcmp(palette, &snapshot->header, sizeof(snapshot->header)))) result = -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { result = -1; }
    ReleaseSRWLockShared(&g_entries_lock);
    if (result < 0) {
        pr_entries_error(error, capacity, "the current native palette selection is unavailable or changing");
        sh_palette_lookup_release(snapshot); return -1;
    }
    *declaration = snapshot->declaration; *out = snapshot; return result;
}
int sh_palette_lookup_current(const sh_palette_lookup_snapshot *snapshot)
{
    int current = 0;
    if (!snapshot) return 0;
    AcquireSRWLockShared(&g_entries_lock);
    __try {
        void *body, *decl; pr_native_palette header;
        if (snapshot->epoch == InterlockedCompareExchange64(&g_lookup_epoch, 0, 0) &&
            pr_lookup_select(snapshot->palette, snapshot->name, &body, &decl, &header) >= 0 &&
            body == snapshot->body && decl == snapshot->declaration &&
            !memcmp(&header, &snapshot->header, sizeof(header)) && pr_lookup_allowed() &&
            snapshot->epoch == InterlockedCompareExchange64(&g_lookup_epoch, 0, 0)) current = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { current = 0; }
    ReleaseSRWLockShared(&g_entries_lock); return current;
}
void sh_palette_lookup_release(sh_palette_lookup_snapshot *snapshot)
{
    if (snapshot) { free(snapshot->name); free(snapshot); }
}
static int pr_authoring_fault(sh_palette_entries *batch, char *error, size_t capacity)
{
    batch->quarantined = 1; InterlockedExchange(&g_entries_faulted, 1);
    pr_entries_error(error, capacity, "native authoring palette failed; retain the complete source owner");
    return -1;
}
static int pr_reserve(pr_native_list *list, int count, palette_grow_fn grow)
{
    while (list->capacity < count) {
        int before = list->capacity;
        grow(list);
        if (list->capacity <= before || !list->data) return 0;
    }
    return 1;
}
static int pr_active_add(unsigned char *editor)
{
    unsigned char *mode = editor + 0x22330;
    int current = *(int *)(mode + 0x1a8);
    if (*(int *)(editor + 0x23618) != 2 || *(void **)mode != g_mode_vtable ||
        current != *(int *)(mode + 0x1ac)) return 0;
    if (current == 3 && *(void **)(mode + 0x258) == g_add_vtable) return 3;
    if (current == 9 && *(void **)(mode + 0x3f0) == g_filter_vtable) return 9;
    return 0;
}
static void pr_authoring_refresh(void *palette, int restore)
{
    unsigned char *editor = (unsigned char *)palette - PR_EDITOR_PALETTE_OFF;
    int active = pr_active_add(editor);
    if (!active) return;
    if (restore) {
        unsigned char *ui = *(unsigned char **)(editor + 0x21088);
        if (ui && *(void **)(ui + 0x9c0)) g_widget_refresh(*(void **)(ui + 0x9c0), palette);
    } else if (active == 3) {
        g_add_prepare(editor + 0x22330 + 0x258, editor);
    } else g_filter_prepare(editor + 0x22330 + 0x3f0, editor);
}

int sh_palette_entries_prepare_authoring(sh_palette_entries *batch,
    sh_palette_entries_admitted_fn admitted, void *context, char *error, size_t capacity)
{
    int result = 0, missing = 0;
    pr_authoring *a = NULL;
    unsigned char *include = NULL;
    if (error && capacity) *error = 0;
    if (!batch || batch->quarantined) return batch ? -1 : 0;
    if (batch->published || batch->authoring || !g_palette_resize || !sh_palette_entries_ready() ||
        !pr_entries_admitted(admitted, context) || InterlockedCompareExchange(&g_entries_busy, 1, 0)) return 0;
    __try {
        pr_native_palette *current = batch->palette;
        if (current != pr_entries_palette() || !pr_palette_valid(current)) goto done;
        include = calloc(batch->count ? batch->count : 1, 1); if (!include) goto done;
        for (size_t i = 0; i < batch->count; i++) {
            int found = 0;
            for (int j = 0; j < current->bodies.count; j++) {
                void *decl = *(void **)((unsigned char *)current->bodies.data + (size_t)j * 0x6f0);
                if (decl == batch->items[i]->decl) { found = 1; break; }
                if (decl) {
                    const char *name = pr_entries_decl_name(decl);
                    if (name && !strcmp(name, batch->items[i]->name)) goto done;
                }
            }
            if (!found) { if (missing == INT_MAX) goto done; include[i] = 1; missing++; }
        }
        if (!missing) { result = 1; goto done; }
        if (current->bodies.count > INT_MAX - missing) goto done;
        a = calloc(1, sizeof(*a)); if (!a) goto done;
        batch->authoring = a; a->expected = *current;
        a->spare.vtable = current->vtable; a->spare.selected = current->selected;
        a->spare.bodies.tag = a->spare.groups.tag = a->spare.labels.tag = 5;
        a->owns_spare = 1;
        int total = current->bodies.count + missing;
        if (!g_palette_resize(&a->spare.bodies, total) || a->spare.bodies.capacity < total ||
            !a->spare.bodies.data) goto done;
        a->spare.bodies.count = total;
        for (int i = 0; i < current->bodies.count; i++) {
            void *destination = (unsigned char *)a->spare.bodies.data + (size_t)i * 0x6f0;
            if (g_body_copy(destination, (unsigned char *)current->bodies.data + (size_t)i * 0x6f0) != destination) {
                result = pr_authoring_fault(batch, error, capacity); goto done;
            }
        }
        int at = current->bodies.count;
        for (size_t i = 0; i < batch->count; i++) if (include[i]) {
            void *destination = (unsigned char *)a->spare.bodies.data + (size_t)at++ * 0x6f0;
            if (g_body_copy(destination, batch->items[i]->body) != destination) {
                result = pr_authoring_fault(batch, error, capacity); goto done;
            }
        }
        a->spare.end = (unsigned char *)a->spare.bodies.data +
            ((uintptr_t)current->end - (uintptr_t)current->bodies.data);
        if (!pr_reserve(&a->spare.groups, current->groups.count, g_group_grow) ||
            !pr_reserve(&a->spare.labels, current->labels.count, g_label_grow)) goto done;
        a->spare.groups.count = current->groups.count;
        for (int i = 0; i < current->groups.count; i++)
            ((void **)a->spare.groups.data)[i] = (unsigned char *)a->spare.bodies.data +
                ((uintptr_t)((void **)current->groups.data)[i] - (uintptr_t)current->bodies.data);
        a->spare.labels.count = current->labels.count;
        for (int i = 0; i < current->labels.count; i++)
            g_string_copy((unsigned char *)a->spare.labels.data + (size_t)i * 0x30,
                (unsigned char *)current->labels.data + (size_t)i * 0x30);
        if (!pr_palette_valid(&a->spare)) goto done;
        a->array = a->spare.bodies.data; result = 1;
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) { result = pr_authoring_fault(batch, error, capacity); }
    if (!result && a) {
        __try { g_palette_destroy(&a->spare); free(a); batch->authoring = NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { result = pr_authoring_fault(batch, error, capacity); }
    }
    free(include); InterlockedExchange(&g_entries_busy, 0);
    if (!result) pr_entries_error(error, capacity, "native authoring palette layout or allocation is unavailable");
    return result;
}

static int pr_authoring_publish(sh_palette_entries *batch, char *error, size_t capacity)
{
    pr_authoring *a = batch->authoring;
    if (!a || a->swapped) return 1;
    __try {
        pr_native_palette *current = batch->palette;
        if (!pr_palette_valid(current) || memcmp(current, &a->expected, sizeof(*current)) ||
            !pr_palette_valid(&a->spare)) return 0;
        pr_native_palette previous = *current;
        *current = a->spare; a->spare = previous;
        a->swapped = 1; a->rebuild = g_palette_rebuild;
        pr_authoring_refresh(current, 0);
        if (!pr_palette_valid(current)) return pr_authoring_fault(batch, error, capacity);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return pr_authoring_fault(batch, error, capacity); }
}
void sh_palette_entries_accept(sh_palette_entries *batch)
{ if (batch && batch->published && batch->authoring && batch->authoring->swapped) batch->authoring->accepted = 1; }

static int pr_authoring_end(sh_palette_entries *batch, char *error, size_t capacity)
{
    pr_authoring *a = batch->authoring;
    if (!a) return 1;
    __try {
        pr_native_palette *current = batch->palette;
        if (a->swapped && a->accepted) {
            if (g_palette_rebuild == a->rebuild || g_state != PR_STATE_APPLIED) {
                pr_entries_error(error, capacity, "authoring palette still owns native readers; await complete palette replacement");
                return 0;
            }
        } else if (a->swapped) {
            if (batch->palette != pr_entries_palette() || !pr_palette_valid(current) ||
                current->bodies.data != a->array) return 0;
            pr_native_palette replaced = *current;
            *current = a->spare; a->spare = replaced; a->swapped = 0;
            pr_authoring_refresh(current, 1);
        }
        if (a->owns_spare) { g_palette_destroy(&a->spare); a->owns_spare = 0; }
        free(a); batch->authoring = NULL; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return pr_authoring_fault(batch, error, capacity); }
}

/* Caller owns the mutation gate and an admitted quiescent boundary. Destroy
 * before unlink/free; a failed destructor keeps the incomplete native object
 * and its source-owning batch quarantined, with no second destructor attempt. */
static int pr_entries_dispose(sh_palette_entries *batch, char *error, size_t capacity)
{
    if (batch->quarantined) return -1;
    int retired = pr_authoring_end(batch, error, capacity);
    if (retired != 1) return retired;
    if (batch->published) {
        AcquireSRWLockExclusive(&g_entries_lock);
        for (size_t i = 0; i < batch->count; ++i)
            if (batch->items[i]) --batch->items[i]->published;
        batch->published = 0;
        ReleaseSRWLockExclusive(&g_entries_lock);
    }
    for (size_t i = 0; i < batch->count; ++i) {
        pr_entry *entry = batch->items[i];
        if (!entry) continue;
        if (entry->owners > 1) { --entry->owners; batch->items[i] = NULL; continue; }
        if (entry->constructed) {
            int finished = 0;
            __try { g_entry_dtor(entry->body); finished = 1; }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
            if (!finished) {
                entry->quarantined = batch->quarantined = 1;
                InterlockedExchange(&g_entries_faulted, 1);
                pr_entries_error(error, capacity, "native palette template destruction failed; retain source owner");
                return -1;
            }
        }
        AcquireSRWLockExclusive(&g_entries_lock);
        pr_entry **at = &g_entries;
        while (*at && *at != entry) at = &(*at)->next;
        if (*at) *at = entry->next;
        ReleaseSRWLockExclusive(&g_entries_lock);
        free(entry->body); free(entry->name); free(entry); batch->items[i] = NULL;
    }
    free(batch->items); free(batch);
    return 1;
}

int sh_palette_entries_prepare(const sh_palette_entry_source *sources, size_t count,
    sh_palette_entries_admitted_fn admitted, void *context, sh_palette_entries **out,
    char *error, size_t capacity)
{
    sh_palette_entries *batch = NULL;
    int result = 0;
    if (error && capacity) *error = 0;
    if (!out || *out || (count && !sources) || count > SIZE_MAX / sizeof(pr_entry *) ||
        !sh_palette_entries_ready() || !pr_entries_admitted(admitted, context) ||
        InterlockedCompareExchange(&g_entries_busy, 1, 0)) {
        pr_entries_error(error, capacity, "palette template preparation awaits an admitted boundary"); return 0;
    }
    if (!(batch = calloc(1, sizeof(*batch)))) goto done;
    batch->refs = 1;
    if (!(batch->palette = pr_entries_palette()) ||
        (count && !(batch->items = calloc(count, sizeof(*batch->items))))) goto done;
    for (size_t i = 0; i < count; ++i) {
        const sh_palette_entry_source *source = sources + i;
        pr_entry *entry = NULL;
        int duplicate = 0, valid = 0, completed = 0;
        if (!pr_entries_identity(source) ||
            !pr_entries_native_matches(batch->palette, source->entity_def_name, source->snap_editor_entity)) {
            pr_entries_error(error, capacity, "palette entry does not match its exact native name/declaration"); goto done;
        }
        for (entry = g_entries; entry; entry = entry->next) {
            if (entry->palette == batch->palette && !strcmp(entry->name, source->entity_def_name)) break;
        }
        if (entry) {
            if (entry->decl != source->snap_editor_entity || entry->quarantined || entry->owners == SIZE_MAX) {
                pr_entries_error(error, capacity, "palette exact name already belongs to a different declaration"); goto done;
            }
            for (size_t j = 0; j < batch->count; ++j) if (batch->items[j] == entry) duplicate = 1;
            if (!duplicate) { ++entry->owners; batch->items[batch->count++] = entry; }
            continue;
        }
        __try { valid = g_entry_valid(source->snap_editor_entity) != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            batch->quarantined = 1; InterlockedExchange(&g_entries_faulted, 1);
            result = -1; goto done;
        }
        if (!valid) { pr_entries_error(error, capacity, "native palette declaration validation refused the entry"); goto done; }
        if (!(entry = calloc(1, sizeof(*entry)))) goto done;
        entry->name = _strdup(source->entity_def_name);
        entry->body = calloc(1, 0x6f0);
        if (!entry->name || !entry->body) { free(entry->body); free(entry->name); free(entry); goto done; }
        entry->decl = source->snap_editor_entity; entry->palette = batch->palette; entry->owners = 1;
        AcquireSRWLockExclusive(&g_entries_lock);
        entry->next = g_entries; g_entries = entry;
        ReleaseSRWLockExclusive(&g_entries_lock);
        batch->items[batch->count++] = entry;
        if (InterlockedCompareExchange(&g_template_thread, (LONG)GetCurrentThreadId(), 0)) {
            pr_entries_error(error, capacity, "native palette template construction is already active"); goto done;
        }
        __try {
            __try { completed = g_entry_ctor(entry->body, entry->decl) == entry->body; }
            __finally { InterlockedExchange(&g_template_thread, 0); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { completed = 0; }
        if (!completed) {
            entry->quarantined = batch->quarantined = 1;
            InterlockedExchange(&g_entries_faulted, 1);
            pr_entries_error(error, capacity, "native palette template construction failed; retain source owner");
            result = -1; goto done;
        }
        entry->constructed = 1;
    }
    result = 1;
done:
    if (!result && batch && pr_entries_dispose(batch, error, capacity) < 0) result = -1;
    else if (!result) batch = NULL;
    if (result) *out = batch;
    if (result != 1 && error && capacity && !*error) pr_entries_error(error, capacity,
        result < 0 ? "native palette preparation failed; retain source owner" : "palette template preparation failed");
    InterlockedExchange(&g_entries_busy, 0);
    return result;
}

int sh_palette_entries_publish(sh_palette_entries *batch,
    sh_palette_entries_admitted_fn admitted, void *context, char *error, size_t capacity)
{
    int result = 0;
    if (error && capacity) *error = 0;
    if (!batch || batch->quarantined) return batch ? -1 : 0;
    if (!sh_palette_entries_ready() || !pr_entries_admitted(admitted, context) ||
        InterlockedCompareExchange(&g_entries_busy, 1, 0)) return 0;
    if (batch->palette != pr_entries_palette()) goto done;
    for (size_t i = 0; i < batch->count; ++i) {
        pr_entry *entry = batch->items[i];
        if (!entry || entry->quarantined || entry->published == SIZE_MAX ||
            !pr_entries_native_matches(batch->palette, entry->name, entry->decl)) goto done;
    }
    result = pr_authoring_publish(batch, error, capacity);
    if (result != 1) goto done;
    AcquireSRWLockExclusive(&g_entries_lock);
    if (!batch->published) {
        for (size_t i = 0; i < batch->count; ++i) ++batch->items[i]->published;
        batch->published = 1;
    }
    ReleaseSRWLockExclusive(&g_entries_lock);
    result = 1;
done:
    pr_rows_retired(); /* published or partly failed: no stored row pointer survives */
    InterlockedExchange(&g_entries_busy, 0);
    if (!result && error && capacity && !*error)
        pr_entries_error(error, capacity, "palette template publication changed native identity");
    return result;
}

int sh_palette_entries_retain(sh_palette_entries *batch)
{
    LONG refs;
    if (!batch) return 0;
    do {
        refs = InterlockedCompareExchange(&batch->refs, 0, 0);
        if (refs <= 0 || refs == LONG_MAX) return 0;
    } while (InterlockedCompareExchange(&batch->refs, refs + 1, refs) != refs);
    return 1;
}

int sh_palette_entries_end(sh_palette_entries **owned,
    sh_palette_entries_admitted_fn admitted, void *context, char *error, size_t capacity)
{
    sh_palette_entries *batch;
    int result;
    if (error && capacity) *error = 0;
    if (!owned || !(batch = *owned)) return 1;
    if (batch->quarantined) {
        pr_entries_error(error, capacity, "native palette batch is quarantined; retain source owner"); return -1;
    }
    if (!pr_entries_admitted(admitted, context) || InterlockedCompareExchange(&g_entries_busy, 1, 0)) return 0;
    if (InterlockedDecrement(&batch->refs)) result = 1;
    else {
        result = pr_entries_dispose(batch, error, capacity);
        if (result != 1) InterlockedExchange(&batch->refs, 1);
    }
    pr_rows_retired();
    if (result == 1) *owned = NULL;
    InterlockedExchange(&g_entries_busy, 0);
    return result;
}

/* Claim the service for one rebuild. A pass may start from IDLE (the first
 * registration of the process) or from APPLIED (every later one). REFUSED and
 * PENDING are not claimable: the first is terminal, the second means a rebuild
 * is already in flight. */
static int pr_claim(void)
{
    if (InterlockedCompareExchange(&g_entries_busy, 0, 0)) return 0;
    if (InterlockedCompareExchange(&g_state, PR_STATE_PENDING, PR_STATE_IDLE) == PR_STATE_IDLE)
        return 1;
    return InterlockedCompareExchange(&g_state, PR_STATE_PENDING, PR_STATE_APPLIED) ==
           PR_STATE_APPLIED;
}

int sh_palette_refresh_after_decl_registration(void)
{
    const uint8_t *editor;
    void *palette_vtable = NULL;
    int vtable_status;

    if (!pr_claim())
        return 0;
    backend_log("palette-refresh ARMED: complete native decl registration succeeded");

    if (!g_module_base || !g_builder) {
        pr_refuse("palette-refresh REFUSED: clean builder/module dependency unavailable");
        return 0;
    }
    /* Use the runtime-resolved editor global; refuse if it is unavailable. */
    {
        uintptr_t expect = glb_resolve(g_module_base, "editor_singleton", NULL);
        if (!expect) {
            pr_refuse("palette-refresh REFUSED: the editor singleton could not be located on this build");
            return 0;
        }
        editor = sh_iface_engine_editor_base();
        if (!editor || (uintptr_t)editor != expect) {
            pr_refuse("palette-refresh REFUSED: editor singleton identity validation failed");
            return 0;
        }
    }

    /* Before calling the builder, require a readable palette object and a
     * non-null vtable in a read-only host section.
     */
    vtable_status = pr_read_ptr(editor + PR_EDITOR_PALETTE_OFF, &palette_vtable);
    if (vtable_status < 0 || vtable_status == 0 ||
        !pr_address_in_readonly_section(g_module_base, palette_vtable)) {
        pr_refuse("palette-refresh REFUSED: editor palette object/vtable validation failed");
        return 0;
    }

    /* Keep PENDING through the native call so reentry cannot replace its array.
     * Retire stored row pointers first: a build that faults part way may
     * already have freed rows. */
    pr_rows_retired();
    /* The template counters describe this rebuild alone: a count that kept
     * growing across rebuilds read as a palette that grew (R-381). */
    InterlockedExchange(&g_template_matches, 0);
    InterlockedExchange(&g_template_forwarded, 0);
    __try {
#ifdef SH_PALETTE_REFRESH_TESTING
        InterlockedIncrement(&g_test_call_count);
#endif
        InterlockedExchange(&g_template_thread, (LONG)GetCurrentThreadId());
        __try {
            g_builder((void *)(editor + PR_EDITOR_PALETTE_OFF), NULL);
        } __finally {
            InterlockedExchange(&g_template_thread, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pr_refuse("palette-refresh REFUSED: SnapPaletteBuild raised an exception");
        return 0;
    }
    if (InterlockedCompareExchange(&g_state, PR_STATE_APPLIED, PR_STATE_PENDING) != PR_STATE_PENDING)
        return 0;
    ++g_palette_rebuild;
    pr_built_record();
    {
        char line[192];
        snprintf(line, sizeof(line),
            "palette-refresh FIRED: native SnapPaletteBuild rebuilt the entity palette (rebuild %ld; this rebuild "
            "matched %ld anonymous universal template(s), decoded %ld; forward_all=%d)",
            (long)g_palette_rebuild, InterlockedCompareExchange(&g_template_matches, 0, 0),
            InterlockedCompareExchange(&g_template_forwarded, 0, 0), g_forward_all);
        backend_log(line);
    }
    pr_rows_retired();
    return 1;
}

#ifdef SH_PALETTE_REFRESH_TESTING
void sh_palette_refresh_test_reset(void)
{
    InterlockedIncrement64(&g_lookup_epoch);
    sh_palette_refresh_steer(NULL);
    InterlockedExchange(&g_state, PR_STATE_IDLE);
    InterlockedExchange(&g_test_call_count, 0);
    g_module_base = NULL;
    g_builder = NULL;
    g_traversal_original = NULL;
    InterlockedExchange(&g_template_thread, 0);
    g_find_original = NULL; g_entry_ctor = NULL; g_entry_valid = NULL; g_entry_dtor = NULL;
    g_palette_resize = NULL; g_group_grow = NULL; g_label_grow = NULL;
    g_body_copy = NULL; g_string_copy = NULL; g_palette_destroy = NULL;
    g_add_prepare = NULL; g_filter_prepare = NULL; g_widget_refresh = NULL;
    g_mode_vtable = g_add_vtable = g_filter_vtable = NULL; g_palette_rebuild = 0;
    free(g_built_identity); g_built_identity = NULL; g_built_rows = NULL; g_built_count = 0; g_built_rebuild = 0;
    g_partition_original = NULL;
    memset(g_keep_original, 0, sizeof(g_keep_original));
    sh_palette_refresh_internal_rows(NULL, 0);
    InterlockedExchange(&g_internal_unlisted, 0);
    InterlockedExchange(&g_entries_installed, 0);
    InterlockedExchange(&g_entries_busy, 0);
    InterlockedExchange(&g_entries_faulted, 0);
}

void sh_palette_refresh_test_bind(const uint8_t *module_base, void *builder)
{
    g_module_base = module_base;
    g_builder = (palette_build_fn)builder;
}

int sh_palette_refresh_test_state(void)
{
    return (int)InterlockedCompareExchange(&g_state, 0, 0);
}

int sh_palette_refresh_test_call_count(void)
{
    return (int)InterlockedCompareExchange(&g_test_call_count, 0, 0);
}

void sh_palette_refresh_test_decoder(void *decoder)
{
    g_traversal_original = (traversal_decode_fn)decoder;
}

void sh_palette_refresh_test_decode(void *entity, void *data)
{
    pr_decode_traversal(entity, data);
}

void sh_palette_pickers_test_bind(void *partition)
{ g_partition_original = (palette_partition_fn)partition; g_keep_original[0] = (palette_keep_fn)partition; }
void *sh_palette_pickers_test_partition(void *first, void *last, void *closure)
{ return g_keep_detours[0]((unsigned char *)first, (unsigned char *)last, closure); }
void sh_palette_pickers_test_bind_keep(int n, void *original)
{ if (n > 0 && n < PR_KEEP_INSTANCES) g_keep_original[n] = (palette_keep_fn)original; }
void *sh_palette_pickers_test_keep(int n, void *first, void *last, void *predicate)
{ return n >= 0 && n < PR_KEEP_INSTANCES ? g_keep_detours[n]((unsigned char *)first, (unsigned char *)last, predicate) : NULL; }
long sh_palette_pickers_test_unlisted(void)
{ return InterlockedCompareExchange(&g_internal_unlisted, 0, 0); }
void sh_palette_authoring_test_bind(const sh_palette_authoring_test_api *api)
{
    g_palette_resize = (palette_resize_fn)api->resize;
    g_group_grow = (palette_grow_fn)api->grow_groups; g_label_grow = (palette_grow_fn)api->grow_labels;
    g_body_copy = (palette_copy_fn)api->copy_body; g_string_copy = (palette_string_copy_fn)api->copy_string;
    g_palette_destroy = (palette_entry_dtor_fn)api->destroy;
    g_add_prepare = (palette_add_prepare_fn)api->prepare_add;
    g_filter_prepare = (palette_filter_prepare_fn)api->prepare_filter;
    g_widget_refresh = (palette_filter_prepare_fn)api->refresh_widget;
    g_mode_vtable = api->mode_vtable; g_add_vtable = api->add_vtable; g_filter_vtable = api->filter_vtable;
}

void sh_palette_entries_test_bind(void *find, void *construct, void *valid, void *destroy)
{
    g_find_original = (palette_find_fn)find; g_entry_ctor = (palette_entry_ctor_fn)construct;
    g_entry_valid = (palette_entry_valid_fn)valid; g_entry_dtor = (palette_entry_dtor_fn)destroy;
    InterlockedExchange(&g_entries_installed, find && construct && valid && destroy);
}
void *sh_palette_entries_test_find(void *palette, const char *name)
{ return pr_find_entity(palette, name); }
size_t sh_palette_entries_test_count(void)
{
    size_t count = 0;
    for (pr_entry *entry = g_entries; entry; entry = entry->next) ++count;
    return count;
}
/* Test fixtures use fake native objects with no child allocations. Never
 * expose this discard route in production: quarantined native storage is owned. */
void sh_palette_entries_test_discard_quarantine(sh_palette_entries **batch)
{
    if (!batch || !*batch) return;
    for (size_t i = 0; i < (*batch)->count; ++i) {
        pr_entry *entry = (*batch)->items[i];
        if (entry) { entry->quarantined = 0; entry->constructed = 0; }
    }
    (*batch)->quarantined = 0;
    (void)pr_entries_dispose(*batch, NULL, 0); *batch = NULL;
}
#endif
