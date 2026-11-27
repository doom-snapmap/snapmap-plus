/* timeline_tile.c -- see timeline_tile.h. Build 13954591: palette at editor+0x20660 (entries +0x10, count +0x18,
 * stride 0x6f0; entry +0 sedef, +0x150 prototype record); sedef +0x08 decl name, +0x28 lifetime, +0x2c load
 * state, +0x1c8 entityDef; record +0x38 text, +0x58 inherit, +0x60 class; entity holder +0 refcount (undo
 * snapshots share holders; EntityDeshare unshares), +0x08 sedef, +0x158 record. Decl registry calls are the
 * ones grid_room_native.c uses. Every engine read is SEH-guarded; a fault retires the module. */
#include "safe_read.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "timeline_tile.h"
#include "direct_actor.h"

const char sh_tl_record_json[] =
    "{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"},"
    "\"displayName\":\"\","
    "\"entityDef\":{\"className\":\"" SH_TL_CLASS "\",\"inherit\":\"" SH_TL_INHERIT "\",\"name\":\"\","
    "\"state\":{\"edit\":{}},\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":26},"
    "\"isMegaModel\":false,\"layerMask\":1,\"pinned\":false,\"restrictionMask\":0,\"uniqueId\":0,"
    "\"~type\":\"idSnapEntity\"}";

const void *sh_tl_wanted_sedef(const char *inherit, const char *cls, const void *current,
                               const void *tile, const void *unknown)
{
    if (!inherit || strcmp(inherit, SH_TL_INHERIT) || !tile || !unknown) return NULL;
    if (current != tile && current != unknown) return NULL;
    return (cls && !strcmp(cls, SH_TL_CLASS)) ? tile : unknown;
}

const void *sh_tl_stock_row_first(const char *current_row, const void *current, const void *stock_row)
{
    if (!current_row || strncmp(current_row, "mods/", 5) || !stock_row || stock_row == current) return NULL;
    return stock_row;
}

int sh_tl_picker_offers(int placeable, const char *row)
{
    const char *hash;
    if (!placeable || !row || _strnicmp(row, SH_DIRECT_ACTOR_ID, sizeof(SH_DIRECT_ACTOR_ID) - 1)) return placeable;
    hash = row + sizeof(SH_DIRECT_ACTOR_ID) - 1;
    return strspn(hash, "0123456789abcdefABCDEF") != 64 || hash[64] != 0;
}

int sh_tl_substate_blocks_rebind(int substate)
{
    return substate == 4 || substate == 7;
}

static __declspec(thread) uint32_t g_category_tag;

static uint32_t tl_category_tag(const unsigned char *row)
{
    const unsigned char *sedef = *(const unsigned char *const *)row;
    return *(const uint32_t *)(sedef + 0x94);
}

static unsigned char tl_category_matches(const unsigned char *row)
{
    return tl_category_tag(row) == g_category_tag;
}

static unsigned char *tl_group_tag(unsigned char *first, unsigned char *end, uint32_t tag, sh_tl_partition partition)
{
    static const unsigned char keep = 1;
    const sh_tl_predicate predicate = { &keep, tl_category_matches };
    unsigned char *row, *boundary;
    uint32_t saved;
    for (boundary = first; boundary < end && tl_category_tag(boundary) == tag; boundary += SH_TL_PALETTE_STRIDE) {}
    for (row = boundary; row < end; row += SH_TL_PALETTE_STRIDE)
        if (tl_category_tag(row) == tag) {
            if (!partition) return NULL;
            saved = g_category_tag;
            g_category_tag = tag;
            __try { boundary = partition(boundary, end, &predicate); }
            __finally { g_category_tag = saved; }
            break;
        }
    return boundary;
}

unsigned char *sh_tl_category_end(unsigned char *first, unsigned char *end, sh_tl_partition partition)
{
    unsigned char *row, *boundary;
    uint32_t tag;
    if (first == end) return end;
    tag = tl_category_tag(first);
    for (row = first + SH_TL_PALETTE_STRIDE; row < end; row += SH_TL_PALETTE_STRIDE) {
        uint32_t next = tl_category_tag(row);
        if (next < tag) tag = next;
    }
    boundary = tl_group_tag(first, end, tag, partition);
    return boundary ? boundary : end;
}

unsigned char *sh_tl_first_custom(unsigned char *first, unsigned char *end, sh_tl_partition partition)
{
    /* DOOM's string ID for #str_sh_category_1 is identical in every language. */
    return tl_group_tag(first, end, 0x6c261296u, partition);
}

#include <windows.h>
#include <string.h>
#define PALETTE_SORT_STOLEN 16    /* sub rsp,0x48; movzx eax,[rsp+0x28]; lea rdx,[rip+comparator] */
#define PALETTE_SORT_LEA_OFF 9    /* the lea the trampoline relocates */
#define PALETTE_SORT_CALL_OFF SH_TIMELINE_SORT_CALL_OFF  /* the E8 to the sort inside SnapPaletteSortCall */

/* The sort reached through the E8 at call_site, when its first 16 bytes are
 * the wrapper's (the comparator's displacement differs per renderer). */
const unsigned char *sh_timeline_tile_sort_target(const unsigned char *call_site)
{
    static const unsigned char prologue[PALETTE_SORT_STOLEN] =
        { 0x48, 0x83, 0xEC, 0x48, 0x0F, 0xB6, 0x44, 0x24, 0x28, 0x48, 0x8D, 0x15, 0, 0, 0, 0 };
    const unsigned char *target;
    int32_t rel;
    __try {
        if (call_site[0] != 0xE8) return NULL;
        memcpy(&rel, call_site + 1, 4);
        target = call_site + 5 + rel;
        if (memcmp(target, prologue, 12)) return NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return target;
}

/* The builder calls admission with the editor declaration and constructs a palette row only when it returns true. */
const unsigned char *sh_timeline_tile_admit_target(const unsigned char *build)
{
    static const unsigned char prologue[] = {0x48,0x83,0xec,0x28,0x48,0x83,0xb9,0xc8,0x01,0,0,0,0x48,0x8b,0xd1};
    static const unsigned char before[] = {0x48,0x8b,0xe8,0x48,0x8b,0xc8,0xe8};
    static const unsigned char after[] = {0x84,0xc0,0x74,0x56,0x48,0x8b,0xd5};
    int32_t rel;
    if (!build) return NULL;
    __try {
        const unsigned char *call = build + 0xd6, *target;
        if (memcmp(call - 6, before, sizeof before) || memcmp(call + 5, after, sizeof after)) return NULL;
        memcpy(&rel, call + 1, sizeof rel);
        target = call + 5 + rel;
        return memcmp(target, prologue, sizeof prologue) ? NULL : target;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

#ifndef SH_TIMELINE_TILE_PURE
#include "apply_engine.h"
#include "backend_log.h"
#include "patch.h"
#include "hook.h"
#include "process_heap_scope.h"
#include "engine_globals.h"
#include "budget_table.h"
#include "modserve.h"

#define ED_PALETTE_OFF      0x20660
#define ED_MAP_OFF          0x204c8
#define ED_ENTITY_MODE_OFF  0x22330   /* inline EntityMode object */
#define MODE_SUBSTATE_OFF   0x1ac
#define PAL_ENTRIES_OFF     0x10
#define PAL_COUNT_OFF       0x18
#define PAL_STRIDE          SH_TL_PALETTE_STRIDE
#define PAL_MAX             100000
#define ENTRY_RECORD_OFF    0x150
#define SEDEF_NAME_OFF      0x08
#define SEDEF_LIFETIME_OFF  0x28
#define SEDEF_STATE_OFF     0x2c
#define SEDEF_ENTITYDEF_OFF 0x1c8
#define RECORD_TEXT_OFF     0x38
#define RECORD_INHERIT_OFF  0x58
#define RECORD_CLASS_OFF    0x60
#define MAP_ENTITIES_OFF    0x6a0
#define MAP_ENTITY_COUNT_OFF 0x6a8
#define HOLDER_SEDEF_OFF    0x08
#define HOLDER_RECORD_OFF   0x158
#define ENTITY_MAX          1000000u
#define PALETTE_BUILD_STOLEN 18       /* mov rax,rsp; five pushes; sub rsp,0x770 */
#define REBIND_LOG_MAX      4

typedef void *(*str_ctor_fn)(void *, const char *);
typedef void (*str_dtor_fn)(void *);
typedef void *(*type_fn)(void *, const char *);
typedef unsigned char (*register_fn)(void *, const void *, void *);
typedef void *(*source_fn)(void *, const char *);
typedef void *(*find_decl_fn)(void *, const char *, unsigned char);
typedef void (*generic_load_fn)(void *);
typedef void (*palette_build_fn)(void *, void *);
typedef union tl_string { void *align; unsigned char bytes[48]; } tl_string;

static str_ctor_fn g_ctor;
static str_dtor_fn g_dtor;
static const unsigned char *g_anchor;
static type_fn g_type;
static register_fn g_register;
static source_fn g_source;
static find_decl_fn g_decl;
static generic_load_fn g_generic_load;
static palette_build_fn g_build_orig;
static sh_process_heap_api g_heap;
static int g_heap_ok;
static const char *g_last_error;      /* engine's last Error/FatalError text, 0x800 bytes */
#define ERROR_TEXT_MAX 200

static volatile LONG g_faulted;
static volatile LONG g_rebuild_disabled;  /* an unbalanced heap scope ends runtime rebuilds */
static volatile LONG g_registered;    /* 1 registered, -1 refused for this process */
static int g_register_tries;
static void * volatile g_editor;      /* last editor seen by the frame tick */
static void * volatile g_built_palette; /* editor+0x20660 of the last completed build (its one call site) */

/* Palette cache, revalidated each frame by entry pointer, count and sedef. */
static const unsigned char *g_pal_entries;
static int g_pal_count = -1;
static int g_tile_row = -1, g_unknown_row = -1;
static const void *g_tile_sedef, *g_unknown_sedef;
static int g_patch_logged, g_patch_fail_logged;
static const unsigned char *g_patch_failed_entries;   /* no retry until the palette changes */

/* Entity list gate: array pointer, count and a hash of the holder pointers. */
static const void *g_ent_array;
static uint32_t g_ent_count;
static uint64_t g_ent_hash;
static int g_ent_seen;

static volatile LONG g_total_rebinds;

static void tl_logf(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}


static int str_eq_guarded(const char *s, const char *want)
{
    __try { return s && !strcmp(s, want); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}



static void *tl_registry(void)
{
    int32_t displacement;
    void *r;
    void **vt;
    if (!g_anchor || memcmp(g_anchor + 0x10, "\x48\x8b\x0d", 3)) return NULL;
    memcpy(&displacement, g_anchor + 0x13, 4);
    r = *(void **)(g_anchor + 0x17 + displacement);
    if (!r) return NULL;
    vt = *(void ***)r;
    if (!vt || vt[7] != (void *)g_register || vt[11] != (void *)g_type) return NULL;
    return r;
}

/* Publish the tile's name into the snapEditorEntityDef manager and load it (bytes from overrides_baked.h via
 * the override provider) so the next palette build admits it. Process heap, as the decl outlives every map; returns 1 when ready. */
static int tl_register_body(void)
{
    void *r, *manager, *decl;
    tl_string source;
    unsigned char state;
    void *entity_def = NULL;
    r = tl_registry();
    if (!r) { backend_log("TL tile: declaration registry unavailable"); return 0; }
    manager = g_type(r, "snapEditorEntityDef");
    if (!manager) { backend_log("TL tile: snapEditorEntityDef type unavailable"); return 0; }
    if (!g_source(manager, SH_TL_TILE_SEDEF)) {
        unsigned char registered;
        g_ctor(&source, SH_TL_TILE_PATH);
        registered = g_register(r, &source, NULL);
        g_dtor(&source);
        if (!registered || !g_source(manager, SH_TL_TILE_SEDEF)) {
            backend_log("TL tile: source registration failed");
            return 0;
        }
    }
    decl = g_decl(manager, SH_TL_TILE_SEDEF, 0);
    if (!decl) decl = g_decl(manager, SH_TL_TILE_SEDEF, 1);
    if (!decl) { backend_log("TL tile: lookup failed"); return 0; }
    state = *((unsigned char *)decl + SEDEF_STATE_OFF);
    entity_def = *(void **)((unsigned char *)decl + SEDEF_ENTITYDEF_OFF);
    /* Create-if-missing can leave an empty default object; nothing else
     * refers to this identity yet, so it takes the native source load. */
    if (!(state & 1) && !entity_def) {
        *(unsigned int *)((unsigned char *)decl + SEDEF_LIFETIME_OFF) = 4;
        *((unsigned char *)decl + SEDEF_STATE_OFF) = (unsigned char)(state & ~2);
        g_generic_load(decl);
        state = *((unsigned char *)decl + SEDEF_STATE_OFF);
        entity_def = *(void **)((unsigned char *)decl + SEDEF_ENTITYDEF_OFF);
    }
    if ((state & 3) || !entity_def) {
        tl_logf("TL tile: decl not ready (state 0x%02x, entityDef %p)", state, entity_def);
        return 0;
    }
    /* Keep the decl through the map-transition purge, as for runtime catalogs. */
    *(unsigned int *)((unsigned char *)decl + SEDEF_LIFETIME_OFF) = 4;
    tl_logf("TL tile: registered %s (sedef %p, entityDef %p)", SH_TL_TILE_SEDEF, decl, entity_def);
    return 1;
}

static DWORD g_fault_code;

static int tl_fault_filter(DWORD code)
{
    g_fault_code = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void tl_error_text(char *out, size_t cap)
{
    out[0] = 0;
    if (!g_last_error) return;
    __try { strncpy_s(out, cap, g_last_error, _TRUNCATE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}

/* An engine Error/FatalError formats its text into last_error_msg, then throws a C++ exception (0xE06D7363).
 * Report that text only when the registration changed it, so a stale error is never blamed. */
static void tl_log_fault(const char *before)
{
    char after[ERROR_TEXT_MAX];
    size_t n;
    tl_error_text(after, sizeof after);
    n = strlen(after);
    while (n && (after[n - 1] == '\n' || after[n - 1] == '\r')) after[--n] = 0;
    if (after[0] && strcmp(after, before))
        tl_logf("TL tile: registration faulted (exception 0x%08lx): engine error: %s",
                (unsigned long)g_fault_code, after);
    else
        tl_logf("TL tile: registration faulted (exception 0x%08lx); no new engine error text",
                (unsigned long)g_fault_code);
}

static void tl_register(void)
{
    sh_process_heap_scope heap = {0};
    char before[ERROR_TEXT_MAX];
    int ok = 0;
    if (InterlockedCompareExchange(&g_registered, 0, 0) != 0) return;
    if (!g_heap_ok || !g_anchor || !g_type || !g_register || !g_source || !g_decl || !g_generic_load ||
        !g_ctor || !g_dtor) return;
    if (!sh_process_heap_enter(&g_heap, &heap)) { backend_log("TL tile: heap scope unavailable"); return; }
    tl_error_text(before, sizeof before);
    __try {
        __try { ok = tl_register_body(); }
        __except (tl_fault_filter(GetExceptionCode())) { ok = 0; tl_log_fault(before); }
    } __finally {
        if (!sh_process_heap_leave(&heap)) ok = 0;
    }
    if (ok) InterlockedExchange(&g_registered, 1);
    else if (++g_register_tries >= 3) {
        InterlockedExchange(&g_registered, -1);
        backend_log("TL tile: registration refused; the Timeline tile is unavailable");
    }
}

/* The engine sorts palette rows in place from four places; the Play build sorts twice, the second time in the
 * map heap. A sort reallocates a row's string in the current heap when the incoming value is longer, so a row
 * growing in the map-heap sort is freed on return to the editor and the next Play's sort reads it (DOOM+0x339c40
 * reads clipModelName in released memory). Every sort here runs on process heap 0, as the builder does. */
typedef void (*palette_sort_fn)(void *);
static palette_sort_fn g_sort_orig;
typedef unsigned char *(*tl_category_fn)(unsigned char *first, unsigned char *end, const void *key);
static tl_category_fn g_category_orig;
static sh_tl_partition g_category_partition;

static unsigned char *tl_partition_category(unsigned char *first, unsigned char *end, const sh_tl_predicate *predicate)
{
    sh_process_heap_scope heap = {0};
    unsigned char *boundary;
    if (!g_heap_ok || !sh_process_heap_enter(&g_heap, &heap)) return NULL;
    __try { boundary = g_category_partition(first, end, predicate); }
    __finally { sh_process_heap_leave(&heap); }
    return boundary;
}

static unsigned char *tl_category_bound(unsigned char *first, unsigned char *end, const void *key)
{
    static __declspec(thread) unsigned char *expected_next, *range_end;
    unsigned char *bound = NULL;
    /* The native heading loop advances to our last boundary. Move Custom once per visible range. */
    if (first != expected_next || end != range_end)
        bound = sh_tl_first_custom(first, end, tl_partition_category);
    if (!bound || bound == first)
        bound = sh_tl_category_end(first, g_category_orig(first, end, key), tl_partition_category);
    expected_next = bound == end ? NULL : bound;
    range_end = end;
    return bound;
}
static volatile LONG g_sorts, g_sorts_unscoped;

static void tl_palette_sort(void *palette)
{
    sh_process_heap_scope heap = {0};
    int scoped = g_heap_ok && sh_process_heap_enter(&g_heap, &heap);
    InterlockedIncrement(&g_sorts);
    if (!scoped && InterlockedIncrement(&g_sorts_unscoped) == 1)
        backend_log("TL tile: a palette sort ran outside process heap 0 (the scope could not be entered); "
                    "rows it grows do not survive the next map transition");
    __try { g_sort_orig(palette); }
    __finally {
        if (scoped && !sh_process_heap_leave(&heap))
            backend_log("TL tile: the palette sort's heap scope did not balance");
    }
}

void sh_timeline_tile_sorts(int *sorts, int *unscoped, int *guarded)
{
    if (sorts) *sorts = (int)InterlockedCompareExchange(&g_sorts, 0, 0);
    if (unscoped) *unscoped = (int)InterlockedCompareExchange(&g_sorts_unscoped, 0, 0);
    if (guarded) *guarded = g_sort_orig != NULL;
}

static unsigned char (*g_actor_admit)(void *);
static unsigned char (*g_picker_offers)(void *);
static __declspec(thread) int g_building_palette;

int sh_timeline_actor_filter_ready(void) { return g_actor_admit && g_picker_offers; }

static unsigned char tl_picker_offers(void *entry)
{
    unsigned char placeable = g_picker_offers(entry);
    if (placeable) {
        __try {
            const unsigned char *decl = *(const unsigned char *const *)entry;
            const char *row = *(const char *const *)(decl + SEDEF_NAME_OFF);
            return (unsigned char)sh_tl_picker_offers(placeable, row);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return placeable;
}

static unsigned char tl_actor_admit(void *declaration)
{
    if (g_building_palette && declaration) {
        const char *row = NULL;
        __try { row = *(const char *const *)((unsigned char *)declaration + SEDEF_NAME_OFF);
            if (row && !_strnicmp(row, SH_DIRECT_ACTOR_ID, sizeof(SH_DIRECT_ACTOR_ID)-1) &&
                !sh_modserve_actor_enabled(row)) return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return g_actor_admit(declaration);
}

static void tl_palette_build(void *palette, void *progress)
{
    /* The palette enumerates the decl manager, so register before it runs. */
    if (!InterlockedCompareExchange(&g_faulted, 0, 0)) tl_register();
    sh_modserve_palette_before_build();   /* installed mods' editor rows */
    ++g_building_palette;
    __try { g_build_orig(palette, progress); }
    __finally { --g_building_palette; }
    g_built_palette = palette;
    sh_modserve_palette_after_build(palette);
    g_pal_entries = NULL;   /* force a rescan and a fresh patch on the next tick */
    g_patch_failed_entries = NULL;
    sh_budget_table_palette_rebuilt();
}

/* The mod rows that share their entityDef with a stock row, and that row
 * (sh_tl_stock_row_first). The palette sorts by entityDef, so they are
 * neighbours. */
#define SHARED_MAX 64
static const void *g_shared_mod[SHARED_MAX], *g_shared_stock[SHARED_MAX];
static int g_nshared;

static const char *tl_row_key(const unsigned char *entry, const char **row)
{
    void *sedef = NULL, *name = NULL, *record = NULL, *key = NULL;
    *row = NULL;
    if (!sh_read_ptr(entry, &sedef) || !sedef || !sh_read_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &name)) return NULL;
    *row = (const char *)name;
    if (!sh_read_ptr(entry + ENTRY_RECORD_OFF, &record) || !record || !sh_read_ptr((unsigned char *)record + RECORD_INHERIT_OFF, &key))
        return NULL;
    return (const char *)key;
}

static void tl_scan_shared(int count)
{
    int i = 0;
    g_nshared = 0;
    while (i < count) {
        const char *row = NULL, *key = tl_row_key(g_pal_entries + (size_t)i * PAL_STRIDE, &row), *other;
        const void *stock = NULL;
        int j = i + 1, k;
        while (j < count) {
            other = tl_row_key(g_pal_entries + (size_t)j * PAL_STRIDE, &row);
            if (!key || !other || strcmp(key, other)) break;
            ++j;
        }
        for (k = i; j - i > 1 && k < j && !stock; ++k)
            if (tl_row_key(g_pal_entries + (size_t)k * PAL_STRIDE, &row) && row && strncmp(row, "mods/", 5))
                (void)sh_read_ptr(g_pal_entries + (size_t)k * PAL_STRIDE, (void **)&stock);
        for (k = i; stock && k < j; ++k) {
            const void *sedef = NULL;
            if (!tl_row_key(g_pal_entries + (size_t)k * PAL_STRIDE, &row) || !row ||
                !sh_tl_stock_row_first(row, NULL, stock) || g_nshared >= SHARED_MAX)
                continue;
            (void)sh_read_ptr(g_pal_entries + (size_t)k * PAL_STRIDE, (void **)&sedef);
            g_shared_mod[g_nshared] = sedef;
            g_shared_stock[g_nshared++] = stock;
            if (g_nshared <= REBIND_LOG_MAX)
                tl_logf("palette: the mod row %s shares the entityDef %s with a stock row; stock entities stay the "
                        "stock row's", row, key);
        }
        i = j;
    }
}

static int tl_scan_palette(const unsigned char *palette)
{
    void *entries = NULL;
    int count = 0, i;
    g_tile_row = g_unknown_row = -1;
    g_tile_sedef = g_unknown_sedef = NULL;
    g_nshared = 0;
    if (!sh_read_ptr(palette + PAL_ENTRIES_OFF, &entries) || !sh_read_i32(palette + PAL_COUNT_OFF, &count)) return 0;
    g_pal_entries = (const unsigned char *)entries;
    g_pal_count = count;
    if (!entries || count <= 0 || count > PAL_MAX) return 1;
    __try { tl_scan_shared(count); } __except (EXCEPTION_EXECUTE_HANDLER) { g_nshared = 0; }
    for (i = 0; i < count; ++i) {
        void *sedef = NULL, *name = NULL;
        const unsigned char *entry = g_pal_entries + (size_t)i * PAL_STRIDE;
        if (!sh_read_ptr(entry, &sedef) || !sedef || !sh_read_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &name)) continue;
        if (g_tile_row < 0 && str_eq_guarded((const char *)name, SH_TL_TILE_SEDEF)) {
            g_tile_row = i; g_tile_sedef = sedef;
        } else if (g_unknown_row < 0 && str_eq_guarded((const char *)name, SH_TL_UNKNOWN_SEDEF)) {
            g_unknown_row = i; g_unknown_sedef = sedef;
        }
        if (g_tile_row >= 0 && g_unknown_row >= 0) break;
    }
    return 1;
}

/* Cheap per-frame check: the cached rows still hold their sedefs. */
static int tl_palette_current(const unsigned char *palette)
{
    void *entries = NULL, *sedef = NULL;
    int count = 0;
    if (!sh_read_ptr(palette + PAL_ENTRIES_OFF, &entries) || !sh_read_i32(palette + PAL_COUNT_OFF, &count)) return 0;
    if ((const unsigned char *)entries != g_pal_entries || count != g_pal_count) return 0;
    if (g_tile_row >= 0 && (!sh_read_ptr(g_pal_entries + (size_t)g_tile_row * PAL_STRIDE, &sedef) || sedef != g_tile_sedef))
        return 0;
    if (g_unknown_row >= 0 &&
        (!sh_read_ptr(g_pal_entries + (size_t)g_unknown_row * PAL_STRIDE, &sedef) || sedef != g_unknown_sedef))
        return 0;
    return 1;
}


/* The palette build gives the tile's prototype record the entityDef's own
 * class (idEntity). Give it idTarget_Timeline; previews and placements copy
 * the record, text included. */
static void tl_patch_record(void)
{
    void *record = NULL, *cls = NULL, *text = NULL;
    if (g_tile_row < 0) return;
    if (!sh_read_ptr(g_pal_entries + (size_t)g_tile_row * PAL_STRIDE + ENTRY_RECORD_OFF, &record) || !record) return;
    if (!sh_read_ptr((unsigned char *)record + RECORD_CLASS_OFF, &cls)) return;
    if (str_eq_guarded((const char *)cls, SH_TL_CLASS)) return;
    if (g_patch_failed_entries == g_pal_entries) return;
    if (!sh_apply_engine_commit_record(record, sh_tl_record_json)) {
        g_patch_failed_entries = g_pal_entries;
        if (!g_patch_fail_logged) { g_patch_fail_logged = 1; backend_log("TL tile: prototype record patch refused"); }
        return;
    }
    (void)sh_read_ptr((unsigned char *)record + RECORD_CLASS_OFF, &cls);
    (void)sh_read_ptr((unsigned char *)record + RECORD_TEXT_OFF, &text);
    if (!g_patch_logged) {
        int has_line = 0;
        __try { has_line = text && strstr((const char *)text, SH_TL_CLASS) != NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { has_line = 0; }
        g_patch_logged = 1;
        tl_logf("TL tile: prototype record set to %s (row %d, class line in text: %s)",
                SH_TL_CLASS, g_tile_row, has_line ? "yes" : "NO");
    }
}

static uint64_t tl_hash_entities(void **array, uint32_t count)
{
    uint64_t h = 1469598103934665603ULL;
    uint32_t i;
    for (i = 0; i < count; ++i) {
        h ^= (uint64_t)(uintptr_t)array[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void tl_rebind(unsigned char *editor)
{
    void *map = NULL, *array_ptr = NULL;
    void **array;
    uint32_t count = 0, i;
    uint64_t hash;
    int substate = 0;
    if ((!g_tile_sedef || !g_unknown_sedef) && !g_nshared) return;
    if (!sh_read_ptr(editor + ED_MAP_OFF, &map) || !map) { g_ent_seen = 0; return; }
    if (!sh_read_ptr((unsigned char *)map + MAP_ENTITIES_OFF, &array_ptr) ||
        !sh_read_i32((unsigned char *)map + MAP_ENTITY_COUNT_OFF, (int *)&count) || !array_ptr || count > ENTITY_MAX)
        return;
    array = (void **)array_ptr;
    hash = tl_hash_entities(array, count);
    if (g_ent_seen && array_ptr == g_ent_array && count == g_ent_count && hash == g_ent_hash) return;
    if (sh_read_i32(editor + ED_ENTITY_MODE_OFF + MODE_SUBSTATE_OFF, &substate) && sh_tl_substate_blocks_rebind(substate))
        return;   /* retried once the held entity or wire is released */
    for (i = SH_TL_FIRST_ENTITY; i < count; ++i) {
        void *holder = array[i], *sedef = NULL, *record = NULL, *inherit = NULL, *cls = NULL;
        const void *want;
        int refs = 0, k;
        if (!holder || !sh_read_ptr((unsigned char *)holder + HOLDER_SEDEF_OFF, &sedef) || !sedef ||
            !sh_read_ptr((unsigned char *)holder + HOLDER_RECORD_OFF, &record) || !record ||
            !sh_read_ptr((unsigned char *)record + RECORD_INHERIT_OFF, &inherit) ||
            !sh_read_ptr((unsigned char *)record + RECORD_CLASS_OFF, &cls))
            continue;
        __try { want = sh_tl_wanted_sedef((const char *)inherit, (const char *)cls, sedef,
                                          g_tile_sedef, g_unknown_sedef); }
        __except (EXCEPTION_EXECUTE_HANDLER) { want = NULL; }
        for (k = 0; !want && k < g_nshared; ++k) if (g_shared_mod[k] == sedef) want = g_shared_stock[k];
        if (!want || want == sedef) continue;
        /* Undo snapshots share holders; write a private copy. Without the
         * helper only an unshared holder is rebound. */
        if (!sh_apply_engine_deshare_bound() && (!sh_read_i32(holder, &refs) || refs != 1)) continue;
        if (!sh_apply_engine_deshare(&array[i]) || !(holder = array[i])) continue;
        *(const void **)((unsigned char *)holder + HOLDER_SEDEF_OFF) = want;
        if (InterlockedIncrement(&g_total_rebinds) <= REBIND_LOG_MAX)
            tl_logf("TL tile: entity %u rebound to %s", i,
                    want == g_tile_sedef ? SH_TL_TILE_SEDEF : want == g_unknown_sedef ? SH_TL_UNKNOWN_SEDEF
                                                                                       : "its stock row");
    }
    /* Desharing replaces holders, so hash the list after it. */
    g_ent_array = array_ptr;
    g_ent_count = count;
    g_ent_hash = tl_hash_entities(array, count);
    g_ent_seen = 1;
}

void sh_timeline_tile_tick(void *editor)
{
    unsigned char *ed = (unsigned char *)editor, *palette;
    if (!ed || InterlockedCompareExchange(&g_faulted, 0, 0)) return;
    g_editor = ed;
    palette = ed + ED_PALETTE_OFF;
    __try {
        if (!tl_palette_current(palette)) {
            if (!tl_scan_palette(palette)) return;
            g_ent_seen = 0;   /* the rows moved: re-check every entity */
        }
        tl_patch_record();
        tl_rebind(ed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted, 1);
        backend_log("TL tile: faulted; disabled for this session");
    }
}

int sh_timeline_tile_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    const sig_result *build = sig_ok(results, count, "SnapPaletteBuild");
    void *tramp;
    if (g_build_orig) return 1;
    g_ctor = (str_ctor_fn)sig_pointer(results, count, "IdStrCtor");
    g_dtor = (str_dtor_fn)sig_pointer(results, count, "IdStrDtor");
    g_anchor = (const unsigned char *)sig_pointer(results, count, "DeclRegistryAnchor");
    g_type = (type_fn)sig_pointer(results, count, "DeclTypeByName");
    g_register = (register_fn)sig_pointer(results, count, "DeclRegisterFile");
    g_source = (source_fn)sig_pointer(results, count, "DeclSourceFind");
    g_decl = (find_decl_fn)sig_pointer(results, count, "DeclFind");
    g_generic_load = (generic_load_fn)sig_pointer(results, count, "ResourceGenericLoad");
    g_heap_ok = sh_process_heap_bind(&g_heap, results, count, module_base);
    g_last_error = (const char *)glb_resolve(module_base, "last_error_msg", NULL);
    if (!build || !g_ctor || !g_dtor || !g_anchor || !g_type || !g_register || !g_source || !g_decl ||
        !g_generic_load || !g_heap_ok) {
        backend_log("TL tile: unavailable; palette build or decl registry signatures refused");
        return 0;
    }
    tramp = sh_prepare_detour_sig(build, (void *)tl_palette_build, PALETTE_BUILD_STOLEN);
    if (!tramp) { backend_log("TL tile: palette build detour refused"); return 0; }
    g_build_orig = (palette_build_fn)tramp;
    if (sh_commit_detour(tramp) != B2_PATCH_OK) {
        if (sh_uninstall_detour(tramp)) g_build_orig = NULL;
        backend_log("TL tile: palette build detour commit failed");
        return 0;
    }
    backend_log("TL tile: palette build hook installed");
    {
        const sig_result *site = sig_ok(results, count, "SnapPaletteCategoryBound");
        g_category_partition = (sh_tl_partition)sig_pointer(results, count, "SnapPaletteStablePartition");
        void *st = site && g_category_partition ? sh_prepare_detour_sig(site, (void *)tl_category_bound, 14) : NULL;
        g_category_orig = (tl_category_fn)st;
        if (!st || sh_commit_detour(st) != B2_PATCH_OK) {
            if (st && sh_uninstall_detour(st)) g_category_orig = NULL;
            backend_log("Toybox: native category grouping unavailable");
        }
    }
    {
        const unsigned char *admit = sh_timeline_tile_admit_target((const unsigned char *)build->addr);
        void *hook = admit ? hook_prepare((void *)admit, (void *)tl_actor_admit, 15) : NULL;
        if (hook) {
            g_actor_admit = (unsigned char (*)(void *))hook;
            if (hook_commit(hook) != B2_PATCH_OK) {
                (void)hook_unpatch(hook);
                g_actor_admit = NULL;
            }
        }
        if (!g_actor_admit) backend_log("direct actor: native palette admission check unavailable");
    }
    {
        const sig_result *site = sig_ok(results, count, "SnapPalettePickerPlaceable");
        void *hook = site ? sh_prepare_detour_sig(site, (void *)tl_picker_offers, 14) : NULL;
        if (hook) {
            g_picker_offers = (unsigned char (*)(void *))hook;
            if (sh_commit_detour(hook) != B2_PATCH_OK) {
                (void)sh_uninstall_detour(hook);
                g_picker_offers = NULL;
            }
        }
        if (!g_picker_offers) backend_log("direct actor: Add Object filtering unavailable");
    }
    {
        /* The sort guard (tl_palette_sort). Without it only the released-string fix is lost; the build hook above
         * stands either way. */
        const sig_result *site = sig_ok(results, count, "SnapPaletteSortCall");
        const unsigned char *target = site ? sh_timeline_tile_sort_target((const unsigned char *)site->addr + PALETTE_SORT_CALL_OFF) : NULL;
        void *st = target ? hook_prepare_rip_lea((void *)target, (void *)tl_palette_sort, PALETTE_SORT_STOLEN,
                                                 PALETTE_SORT_LEA_OFF)
                          : NULL;
        if (!st) {
            backend_log(target ? "TL tile: palette sort detour refused; palette sorts stay on the engine's heap"
                        : site ? "TL tile: the palette sort's prologue is not the known one; palette sorts stay on the engine's heap"
                               : "TL tile: palette sort signature refused; palette sorts stay on the engine's heap");
        } else if (sh_commit_detour(st) != B2_PATCH_OK) {
            (void)sh_uninstall_detour(st);
            backend_log("TL tile: palette sort detour commit failed; palette sorts stay on the engine's heap");
        } else {
            g_sort_orig = (palette_sort_fn)st;
            backend_log("TL tile: palette sort hook installed (every sort runs on process heap 0)");
        }
    }
    return 1;
}

/* Rebuild the live editor palette like the engine's builder callers, inside verified process heap 0 so it
 * survives the Play transition that frees the map heap; pending registrations run in that scope (NULL progress
 * object), then the cache resets. The module palette (editor+0x206c0) is untouched; main thread only. */
void *sh_timeline_tile_palette(void)
{
    return g_built_palette;
}

int sh_timeline_tile_rebuild_palette(int *rows_before, int *rows_after)
{
    /* A map opened from the browser can parse before the editor's first frame
     * tick; the builder's only call site passes editor+0x20660, so the last
     * built palette names the editor too. */
    unsigned char *ed = g_editor ? (unsigned char *)g_editor
                      : g_built_palette ? (unsigned char *)g_built_palette - ED_PALETTE_OFF : NULL;
    sh_process_heap_scope heap = {0};
    int substate = 0, result = SH_TL_REBUILD_OK;
    if (rows_before) *rows_before = -1;
    if (rows_after) *rows_after = -1;
    if (!ed || !g_build_orig || !g_heap_ok) return SH_TL_REBUILD_UNAVAILABLE;
    if (InterlockedCompareExchange(&g_rebuild_disabled, 0, 0)) return SH_TL_REBUILD_DISABLED;
    if (rows_before) (void)sh_read_i32(ed + ED_PALETTE_OFF + PAL_COUNT_OFF, rows_before);
    if (sh_read_i32(ed + ED_ENTITY_MODE_OFF + MODE_SUBSTATE_OFF, &substate) && sh_tl_substate_blocks_rebind(substate))
        return SH_TL_REBUILD_BUSY;
    if (!sh_process_heap_enter(&g_heap, &heap)) return SH_TL_REBUILD_HEAP;
    __try {
        __try { tl_palette_build(ed + ED_PALETTE_OFF, NULL); }
        __except (EXCEPTION_EXECUTE_HANDLER) { result = SH_TL_REBUILD_FAULTED; }
    } __finally {
        if (!sh_process_heap_leave(&heap)) {
            InterlockedExchange(&g_rebuild_disabled, 1);
            result = SH_TL_REBUILD_HEAP_LEAVE;
        }
    }
    if (rows_after) (void)sh_read_i32(ed + ED_PALETTE_OFF + PAL_COUNT_OFF, rows_after);
    return result;
}

#endif
