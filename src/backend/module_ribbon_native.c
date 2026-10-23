/* Package module variants in the editor's module picker.
 *
 * The picker is the blueprint add/attach mode's ribbon over the editor's
 * module palette (idSnapModulePalette at editor+0x206c0): 0x98-byte records
 * that native code copies as plain values, with category boundary pointers and
 * a visible range into them, all rebuilt by the mode's rebuild (VK 0x140ce7920
 * / GL 0x140ce7300) for sort modes 0..9. The mode's sort cycle refreshes an
 * open picker with rebuild, current record from the preview instance, and the
 * ribbon's scroll and item refresh (site VK 0x140ce65df / GL 0x140ce5fbf). A
 * published variant is a row copied from its own one-module palette
 * (sh_grid_native_variant_record).
 *
 * Rows change only inside that same sequence, run by the product from the
 * mode's tick (VK 0x140ce6150 / GL 0x140ce5b30) whenever the published set
 * changed. The tick takes the served library's variants again whenever a new
 * compilation is served (sh_package_runtime_served_serial: installs, removals,
 * a map's own packages, rollback), so a late install, removal or re-entry shows
 * while the picker is open, without toggling modes or sorting, and when it next
 * opens otherwise. The
 * entry's own second rebuild, which no widget handoff follows, never changes
 * rows. The first change gives the palette one larger native array; the
 * original array is kept for the process, and later changes move rows within
 * that array, as native sorts do, so a reader that keeps a pointer into the
 * record list never sees it freed (its readers are not all enumerated). Rows
 * of withdrawn variants leave the array; their wrappers stay owned by their
 * own palettes (grid_room_native.c). Evidence: m129
 * module-variants/NATIVE-RIBBON.md; package-architecture-v2 R-302, "The module
 * palette and the module ribbon".
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "module_ribbon_native.h"
#include "engine_globals.h"
#include "grid_room_native.h"
#include "hook.h"
#include "package_runtime.h"
#include "process_heap_scope.h"

void backend_log(const char *message);

typedef void (*mr_tick_fn)(void *mode, void *input, void *editor);
typedef void (*mr_rebuild_fn)(void *mode, void *editor, int sort);
typedef void (*mr_set_current_fn)(void *palette, void *map_record);
typedef void (*mr_ribbon_fn)(void *holder);
typedef unsigned char (*mr_resize_fn)(void *list, int size);

#define MR_RECORD   0x98u
#define MR_PALETTE  0x206c0u
#define MR_MAP      0x204c8u
#define MR_GUI      0x21088u
#define MR_RIBBON   0x9c0u
#define MR_RETAINED 16

/* The native record list (idList<0x98-byte record>), as the resize reads it. */
typedef struct mr_list {
    unsigned char *records;
    int count, capacity;
    unsigned short granularity;
    unsigned char tag, memory;
    unsigned char reserved[4];
} mr_list;

static mr_tick_fn g_tick;
static mr_rebuild_fn g_rebuild;
static mr_set_current_fn g_set_current;
static mr_ribbon_fn g_scroll, g_items;
static mr_resize_fn g_resize;
static unsigned char *g_editor;
static sh_process_heap_api g_heap;
static volatile LONG g_busy, g_faulted, g_published, g_shown;
static LONG g_seen_serial;                      /* the served compilation last taken */
static int g_seen;
static SRWLOCK g_lock = SRWLOCK_INIT;            /* the published names */
static char **g_names;
static size_t g_name_count;
static size_t g_rows_shown, g_rows_refused;     /* the last refresh */
static unsigned char *g_retained[MR_RETAINED];   /* replaced record arrays: never freed */
static int g_retained_count;

static int mr_find_rows(const void *wrapper, void *const *want, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (*(void *const *)want[i] == wrapper) return (int)i;
    return -1;
}

/* Make the palette's rows its stock rows plus one row per published variant the
 * private lookup admits. Withdrawn variants' rows leave; every other row keeps
 * its value and its relative order. */
static int mr_sync_rows(unsigned char *palette)
{
    mr_list *list = (mr_list *)(palette + 0x10);
    void **want = NULL;
    unsigned char *present = NULL;
    size_t n = 0, i, refused = 0;
    int kept = 0, needed, r;
    if (list->count < 0 || list->capacity < list->count || !list->records) return 0;
    if (g_name_count && !(want = calloc(g_name_count, sizeof(*want)))) return 0;
    for (i = 0; i < g_name_count; i++) {
        void *record = sh_grid_native_variant_record(g_names[i]);
        if (record && mr_find_rows(*(void **)record, want, n) < 0) want[n++] = record;
        else if (!record) refused++;
    }
    if (n && !(present = calloc(n, 1))) { free(want); return 0; }
    for (r = 0; r < list->count; r++) {
        unsigned char *row = list->records + (size_t)r * MR_RECORD;
        const void *wrapper = *(void **)row;
        if (sh_grid_native_variant_wrapper(wrapper)) {
            int at = mr_find_rows(wrapper, want, n);
            if (at < 0 || present[at]) continue; /* withdrawn, or a repeated row */
            present[at] = 1;
        }
        if (kept != r) memcpy(list->records + (size_t)kept * MR_RECORD, row, MR_RECORD);
        kept++;
    }
    needed = kept;
    for (i = 0; i < n; i++) needed += !present[i];
    if (needed > list->capacity) {
        /* One larger native array, filled before it replaces the current one;
         * memory 2 makes the resize allocate rather than reallocate. */
        mr_list fresh = {0};
        int size = needed + (needed > 512 ? needed : 512);
        fresh.granularity = list->granularity; fresh.tag = list->tag; fresh.memory = 2;
        if (g_retained_count < MR_RETAINED && g_resize(&fresh, size) && fresh.records && fresh.capacity >= needed) {
            memcpy(fresh.records, list->records, (size_t)kept * MR_RECORD);
            g_retained[g_retained_count++] = list->records;
            list->records = fresh.records; list->capacity = fresh.capacity; list->memory = fresh.memory;
        } else {
            backend_log("MODULES: module picker rows cannot grow; published variants beyond its capacity stay out");
        }
    }
    for (i = 0; i < n; i++) {
        if (present[i]) continue;
        if (kept >= list->capacity) { refused++; continue; }
        memcpy(list->records + (size_t)kept * MR_RECORD, want[i], MR_RECORD);
        present[i] = 1; kept++;
    }
    list->count = kept;
    if (*(int *)(palette + 8) < 0 || *(int *)(palette + 8) >= kept) *(int *)(palette + 8) = 0;
    for (i = 0, g_rows_shown = 0; i < n; i++) g_rows_shown += present[i];
    g_rows_refused = refused;
    free(want); free(present);
    return 1;
}

/* The native sort cycle's refresh of an open picker, with the rows changed
 * first. The rebuild then rebuilds the visible range and the categories. */
static void mr_refresh(unsigned char *mode, unsigned char *editor)
{
    unsigned char *palette = editor + MR_PALETTE, *map, *gui, *holder;
    int sort = *(int *)(mode + 0x60), instance = *(int *)(mode + 8), synced;
    LONG target = InterlockedCompareExchange(&g_published, 0, 0);
    sh_process_heap_scope heap = {0};
    char line[160];
    if (sort < 0 || sort > 9) return;
    if (!sh_process_heap_enter(&g_heap, &heap)) return;
    __try {
        AcquireSRWLockShared(&g_lock);
        synced = mr_sync_rows(palette);
        ReleaseSRWLockShared(&g_lock);
        if (!synced) __leave;
        g_rebuild(mode, editor, sort);
        map = *(unsigned char **)(editor + MR_MAP);
        if (map && instance >= 0 && instance < *(int *)(map + 0x758))
            g_set_current(palette, *(unsigned char **)(map + 0x750) + (size_t)instance * MR_RECORD);
        gui = *(unsigned char **)(editor + MR_GUI);
        holder = gui ? *(unsigned char **)(gui + MR_RIBBON) : NULL;
        if (holder) { g_scroll(holder); g_items(holder); }
        InterlockedExchange(&g_shown, target);
        snprintf(line, sizeof(line), "MODULES: module picker shows %zu published module variants (%zu not admitted)",
                 g_rows_shown, g_rows_refused);
        backend_log(line);
    } __finally {
        if (!sh_process_heap_leave(&heap)) InterlockedExchange(&g_faulted, 1);
    }
}

static void mr_tick(void *mode, void *input, void *editor)
{
    if (editor && editor == g_editor && mode && !InterlockedCompareExchange(&g_faulted, 0, 0)) {
        LONG serial = sh_package_runtime_served_serial();
        if ((!g_seen || serial != g_seen_serial) && sh_module_ribbon_publish()) { g_seen_serial = serial; g_seen = 1; }
    }
    if (editor && editor == g_editor && mode &&
        !InterlockedCompareExchange(&g_faulted, 0, 0) &&
        InterlockedCompareExchange(&g_published, 0, 0) != InterlockedCompareExchange(&g_shown, 0, 0) &&
        !InterlockedCompareExchange(&g_busy, 1, 0)) {
        __try { mr_refresh((unsigned char *)mode, (unsigned char *)editor); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&g_faulted, 1);
            backend_log("MODULES: module picker refresh faulted; picker publication disabled");
        }
        InterlockedExchange(&g_busy, 0);
    }
    g_tick(mode, input, editor);
}

int sh_module_ribbon_publish(void)
{
    char **names = NULL, **old = NULL;
    size_t count = 0, old_count = 0, i;
    int same;
    char line[160];
    if (!g_tick) return 1; /* no picker publication on this build; admission still serves maps */
    if (!sh_package_runtime_module_variants(&names, &count)) return 0;
    AcquireSRWLockExclusive(&g_lock);
    same = count == g_name_count;
    for (i = 0; same && i < count; i++) same = !strcmp(names[i], g_names[i]);
    if (!same) {
        old = g_names; old_count = g_name_count;
        g_names = names; g_name_count = count; names = NULL;
        InterlockedIncrement(&g_published);
    }
    ReleaseSRWLockExclusive(&g_lock);
    sh_package_runtime_module_variants_free(old, old_count);
    sh_package_runtime_module_variants_free(names, same ? count : 0);
    if (!same) {
        snprintf(line, sizeof(line), "MODULES: %zu module variants published to the module picker", count);
        backend_log(line);
    }
    return 1;
}

char *sh_module_ribbon_status(void)
{
    char *out;
    size_t used = 0, capacity;
    AcquireSRWLockShared(&g_lock);
    capacity = 256 + g_name_count * 300;
    out = (char *)malloc(capacity);
    if (out && g_name_count) {
        int n = snprintf(out, capacity, "Module picker: %zu published module variants, %s; last refresh shows %zu, %zu not "
            "admitted; %d replaced row arrays kept.\n", g_name_count,
            InterlockedCompareExchange(&g_published, 0, 0) == InterlockedCompareExchange(&g_shown, 0, 0) ?
                "shown" : "shown at the next picker refresh", g_rows_shown, g_rows_refused, g_retained_count);
        if (n > 0) used = (size_t)n;
        for (size_t i = 0; i < g_name_count && used < capacity; i++) {
            n = snprintf(out + used, capacity - used, "  %s\n", g_names[i]);
            if (n > 0) used += (size_t)n;
        }
    } else if (out) { free(out); out = NULL; }
    ReleaseSRWLockShared(&g_lock);
    return out;
}

static void *mr_signature(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (void *)r[i].addr;
    return NULL;
}
static void *mr_call_target(const unsigned char *at)
{
    int32_t displacement;
    if (!at || at[0] != 0xE8) return NULL;
    memcpy(&displacement, at + 1, sizeof(displacement));
    return (void *)(at + 5 + displacement);
}

int sh_module_ribbon_install(const sig_result *r, size_t n, const uint8_t *base)
{
    const unsigned char *site = (const unsigned char *)mr_signature(r, n, "ModuleRibbonSortCycle");
    const unsigned char *insert = (const unsigned char *)mr_signature(r, n, "ModuleRecordInsert");
    void *tick = mr_signature(r, n, "ModuleModeAddTick");
    if (g_tick) return hook_is_installed((void *)g_tick);
    if (!site || !insert || !tick) return 0;
    /* The sort cycle's four calls are at the same offsets on both images. */
    g_rebuild = (mr_rebuild_fn)mr_call_target(site + 10);
    g_set_current = (mr_set_current_fn)mr_call_target(site + 47);
    g_scroll = (mr_ribbon_fn)mr_call_target(site + 66);
    g_items = (mr_ribbon_fn)mr_call_target(site + 85);
    /* The record list's resize is the insert's first call, at +0x74 on both. */
    g_resize = (mr_resize_fn)mr_call_target(insert + 0x74);
    g_editor = (unsigned char *)glb_resolve(base, "editor_singleton", NULL);
    if (!g_rebuild || !g_set_current || !g_scroll || !g_items || !g_resize || !g_editor ||
        !sh_process_heap_bind(&g_heap, r, n, base)) return 0;
    /* mov rax,rsp and seven pushes: 14 position-independent bytes on both. */
    g_tick = (mr_tick_fn)hook_prepare(tick, (void *)mr_tick, 14);
    if (g_tick && hook_commit((void *)g_tick) == B2_PATCH_OK) {
        backend_log("MODULES: module picker publication installed");
        return 1;
    }
    if (g_tick && hook_unpatch((void *)g_tick)) g_tick = NULL;
    return 0;
}

#ifdef SH_MODULE_RIBBON_TESTING
/* Native-call doubles for tests. */
void sh_module_ribbon_test_bind(mr_tick_fn tick, mr_rebuild_fn rebuild, mr_set_current_fn set_current,
    mr_ribbon_fn scroll, mr_ribbon_fn items, mr_resize_fn resize, unsigned char *editor, const sh_process_heap_api *heap)
{
    g_tick = tick; g_rebuild = rebuild; g_set_current = set_current; g_scroll = scroll; g_items = items;
    g_resize = resize; g_editor = editor; g_heap = *heap;
}
void sh_module_ribbon_test_tick(void *mode, void *input, void *editor) { mr_tick(mode, input, editor); }
int sh_module_ribbon_test_retained(void) { return g_retained_count; }
#endif
