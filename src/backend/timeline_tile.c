/* timeline_tile.c -- see timeline_tile.h.
 *
 * Engine surface (build 13954591, snaphak-re editor-palette-surface R-001..R-003):
 *   SnapPaletteBuild   palette at editor+0x20660: entries +0x10, count +0x18,
 *                      stride 0x6f0; entry +0x000 sedef, +0x150 prototype record
 *   sedef              +0x08 decl name, +0x28 resource lifetime, +0x2c load
 *                      state, +0x1c8 resolved entityDef
 *   record             +0x38 text, +0x58 inherit, +0x60 class (pool strings)
 *   entity holder      +0x00 refcount (undo snapshots share holders),
 *                      +0x08 sedef, +0x158 record; EntityDeshare unshares it
 *   DeclRegistryAnchor / DeclTypeByName / DeclRegisterFile / DeclSourceFind /
 *   DeclFind / ResourceGenericLoad   as grid_room_native.c registers a catalog
 * Every engine read is SEH-guarded; a fault retires the module. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "timeline_tile.h"

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

int sh_tl_substate_blocks_rebind(int substate)
{
    return substate == 4 || substate == 7;
}

#ifndef SH_TIMELINE_TILE_PURE
#include "apply_engine.h"
#include "backend_log.h"
#include "patch.h"
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
#define PAL_STRIDE          0x6f0
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

static volatile LONG g_last_rebinds, g_total_rebinds;
static SRWLOCK g_status_lock = SRWLOCK_INIT;
static char g_record_class[64];
static int g_status_tile_row = -1, g_status_unknown_row = -1;

static void tl_logf(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static int rd_ptr(const void *p, void **v) { __try { *v = *(void *const *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int rd_i32(const void *p, int *v) { __try { *v = *(const int *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

static int str_eq_guarded(const char *s, const char *want)
{
    __try { return s && !strcmp(s, want); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void *clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (void *)r[i].addr;
    return NULL;
}

static const sig_result *clean_result(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return &r[i];
    return NULL;
}

/* ------------------------------------------------------------ registration */

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

/* Publish the tile's name into the snapEditorEntityDef manager and load it,
 * so the palette build that follows enumerates and admits it. The bytes are
 * served by the override provider from overrides_baked.h. Process heap: the
 * decl outlives every map. Returns 1 when the decl is ready. */
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

/* An engine Error/FatalError formats its text into last_error_msg and then
 * throws a C++ exception (0xE06D7363). Report that text only when the
 * registration changed it, so an earlier error is never blamed. */
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

static void tl_palette_build(void *palette, void *progress)
{
    /* The palette enumerates the decl manager, so register before it runs. */
    if (!InterlockedCompareExchange(&g_faulted, 0, 0)) tl_register();
    sh_modserve_palette_before_build();   /* installed mods' editor rows */
    g_build_orig(palette, progress);
    sh_modserve_palette_after_build(palette);
    g_pal_entries = NULL;   /* force a rescan and a fresh patch on the next tick */
    g_patch_failed_entries = NULL;
    sh_budget_table_palette_rebuilt();
}

/* ------------------------------------------------------------ palette rows */

static int tl_scan_palette(const unsigned char *palette)
{
    void *entries = NULL;
    int count = 0, i;
    g_tile_row = g_unknown_row = -1;
    g_tile_sedef = g_unknown_sedef = NULL;
    if (!rd_ptr(palette + PAL_ENTRIES_OFF, &entries) || !rd_i32(palette + PAL_COUNT_OFF, &count)) return 0;
    g_pal_entries = (const unsigned char *)entries;
    g_pal_count = count;
    if (!entries || count <= 0 || count > PAL_MAX) return 1;
    for (i = 0; i < count; ++i) {
        void *sedef = NULL, *name = NULL;
        const unsigned char *entry = g_pal_entries + (size_t)i * PAL_STRIDE;
        if (!rd_ptr(entry, &sedef) || !sedef || !rd_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &name)) continue;
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
    if (!rd_ptr(palette + PAL_ENTRIES_OFF, &entries) || !rd_i32(palette + PAL_COUNT_OFF, &count)) return 0;
    if ((const unsigned char *)entries != g_pal_entries || count != g_pal_count) return 0;
    if (g_tile_row >= 0 && (!rd_ptr(g_pal_entries + (size_t)g_tile_row * PAL_STRIDE, &sedef) || sedef != g_tile_sedef))
        return 0;
    if (g_unknown_row >= 0 &&
        (!rd_ptr(g_pal_entries + (size_t)g_unknown_row * PAL_STRIDE, &sedef) || sedef != g_unknown_sedef))
        return 0;
    return 1;
}

static void tl_set_status_class(const char *cls)
{
    AcquireSRWLockExclusive(&g_status_lock);
    __try { strncpy_s(g_record_class, sizeof g_record_class, cls ? cls : "", _TRUNCATE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_record_class[0] = 0; }
    g_status_tile_row = g_tile_row;
    g_status_unknown_row = g_unknown_row;
    ReleaseSRWLockExclusive(&g_status_lock);
}

/* The palette build gives the tile's prototype record the entityDef's own
 * class (idEntity). Give it idTarget_Timeline; previews and placements copy
 * the record, text included. */
static void tl_patch_record(void)
{
    void *record = NULL, *cls = NULL, *text = NULL;
    if (g_tile_row < 0) { tl_set_status_class(""); return; }
    if (!rd_ptr(g_pal_entries + (size_t)g_tile_row * PAL_STRIDE + ENTRY_RECORD_OFF, &record) || !record) return;
    if (!rd_ptr((unsigned char *)record + RECORD_CLASS_OFF, &cls)) return;
    if (str_eq_guarded((const char *)cls, SH_TL_CLASS)) { tl_set_status_class((const char *)cls); return; }
    if (g_patch_failed_entries == g_pal_entries) return;
    if (!sh_apply_engine_commit_record(record, sh_tl_record_json)) {
        g_patch_failed_entries = g_pal_entries;
        tl_set_status_class("");
        if (!g_patch_fail_logged) { g_patch_fail_logged = 1; backend_log("TL tile: prototype record patch refused"); }
        return;
    }
    (void)rd_ptr((unsigned char *)record + RECORD_CLASS_OFF, &cls);
    (void)rd_ptr((unsigned char *)record + RECORD_TEXT_OFF, &text);
    tl_set_status_class(str_eq_guarded((const char *)cls, SH_TL_CLASS) ? SH_TL_CLASS : "");
    if (!g_patch_logged) {
        int has_line = 0;
        __try { has_line = text && strstr((const char *)text, SH_TL_CLASS) != NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { has_line = 0; }
        g_patch_logged = 1;
        tl_logf("TL tile: prototype record set to %s (row %d, class line in text: %s)",
                SH_TL_CLASS, g_tile_row, has_line ? "yes" : "NO");
    }
}

/* ------------------------------------------------------------ rebind */

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
    int substate = 0, rebinds = 0;
    if (!g_tile_sedef || !g_unknown_sedef) return;
    if (!rd_ptr(editor + ED_MAP_OFF, &map) || !map) { g_ent_seen = 0; return; }
    if (!rd_ptr((unsigned char *)map + MAP_ENTITIES_OFF, &array_ptr) ||
        !rd_i32((unsigned char *)map + MAP_ENTITY_COUNT_OFF, (int *)&count) || !array_ptr || count > ENTITY_MAX)
        return;
    array = (void **)array_ptr;
    hash = tl_hash_entities(array, count);
    if (g_ent_seen && array_ptr == g_ent_array && count == g_ent_count && hash == g_ent_hash) return;
    if (rd_i32(editor + ED_ENTITY_MODE_OFF + MODE_SUBSTATE_OFF, &substate) && sh_tl_substate_blocks_rebind(substate))
        return;   /* retried once the held entity or wire is released */
    for (i = SH_TL_FIRST_ENTITY; i < count; ++i) {
        void *holder = array[i], *sedef = NULL, *record = NULL, *inherit = NULL, *cls = NULL;
        const void *want;
        int refs = 0;
        if (!holder || !rd_ptr((unsigned char *)holder + HOLDER_SEDEF_OFF, &sedef) || !sedef ||
            !rd_ptr((unsigned char *)holder + HOLDER_RECORD_OFF, &record) || !record ||
            !rd_ptr((unsigned char *)record + RECORD_INHERIT_OFF, &inherit) ||
            !rd_ptr((unsigned char *)record + RECORD_CLASS_OFF, &cls))
            continue;
        __try { want = sh_tl_wanted_sedef((const char *)inherit, (const char *)cls, sedef,
                                          g_tile_sedef, g_unknown_sedef); }
        __except (EXCEPTION_EXECUTE_HANDLER) { want = NULL; }
        if (!want || want == sedef) continue;
        /* Undo snapshots share holders; write a private copy. Without the
         * helper only an unshared holder is rebound. */
        if (!sh_apply_engine_deshare_bound() && (!rd_i32(holder, &refs) || refs != 1)) continue;
        if (!sh_apply_engine_deshare(&array[i]) || !(holder = array[i])) continue;
        *(const void **)((unsigned char *)holder + HOLDER_SEDEF_OFF) = want;
        ++rebinds;
        if (InterlockedIncrement(&g_total_rebinds) <= REBIND_LOG_MAX)
            tl_logf("TL tile: entity %u rebound to %s", i,
                    want == g_tile_sedef ? SH_TL_TILE_SEDEF : SH_TL_UNKNOWN_SEDEF);
    }
    /* Desharing replaces holders; hash the list as it now stands. */
    g_ent_array = array_ptr;
    g_ent_count = count;
    g_ent_hash = tl_hash_entities(array, count);
    g_ent_seen = 1;
    InterlockedExchange(&g_last_rebinds, rebinds);
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

/* ------------------------------------------------------------ install + slot */

int sh_timeline_tile_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    const sig_result *build = clean_result(results, count, "SnapPaletteBuild");
    void *tramp;
    if (g_build_orig) return 1;
    g_ctor = (str_ctor_fn)clean(results, count, "IdStrCtor");
    g_dtor = (str_dtor_fn)clean(results, count, "IdStrDtor");
    g_anchor = (const unsigned char *)clean(results, count, "DeclRegistryAnchor");
    g_type = (type_fn)clean(results, count, "DeclTypeByName");
    g_register = (register_fn)clean(results, count, "DeclRegisterFile");
    g_source = (source_fn)clean(results, count, "DeclSourceFind");
    g_decl = (find_decl_fn)clean(results, count, "DeclFind");
    g_generic_load = (generic_load_fn)clean(results, count, "ResourceGenericLoad");
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
    return 1;
}

/* A runtime rebuild of the live editor palette, shaped like the engine's own
 * callers of the builder (snaphak-re mod-system-reinstatement lane R3,
 * section 3): process heap 0 is entered and verified, so the new array, rows
 * and strings survive the Play transition that frees the ambient map heap;
 * the detour then runs pending registrations inside that scope, the builder
 * with a NULL progress object (guarded in the builder), and the cache resets
 * after it. The module palette (editor+0x206c0) is not rebuilt; whether it
 * keeps entity-row pointers is an open trace. Main thread only, from a point
 * where no native frame is iterating the palette (the caller checks the
 * thread). */
int sh_timeline_tile_rebuild_palette(int *rows_before, int *rows_after)
{
    unsigned char *ed = (unsigned char *)g_editor;
    sh_process_heap_scope heap = {0};
    int substate = 0, result = SH_TL_REBUILD_OK;
    if (rows_before) *rows_before = -1;
    if (rows_after) *rows_after = -1;
    if (!ed || !g_build_orig || !g_heap_ok) return SH_TL_REBUILD_UNAVAILABLE;
    if (InterlockedCompareExchange(&g_rebuild_disabled, 0, 0)) return SH_TL_REBUILD_DISABLED;
    if (rows_before) (void)rd_i32(ed + ED_PALETTE_OFF + PAL_COUNT_OFF, rows_before);
    if (rd_i32(ed + ED_ENTITY_MODE_OFF + MODE_SUBSTATE_OFF, &substate) && sh_tl_substate_blocks_rebind(substate))
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
    if (rows_after) (void)rd_i32(ed + ED_PALETTE_OFF + PAL_COUNT_OFF, rows_after);
    return result;
}

int sh_timeline_tile_slot(struct sh_iface *self, sh_timeline_tile_call *call)
{
    unsigned char *ed = (unsigned char *)g_editor;
    void *map = NULL;
    (void)self;
    if (!call || call->op != SH_TIMELINE_TILE_STATUS) return 0;
    call->tile_row = call->unknown_row = -1;
    call->last_rebinds = (int)InterlockedCompareExchange(&g_last_rebinds, 0, 0);
    call->total_rebinds = (int)InterlockedCompareExchange(&g_total_rebinds, 0, 0);
    call->registered = InterlockedCompareExchange(&g_registered, 0, 0) == 1;
    call->record_class[0] = 0;
    if (!ed || !rd_ptr(ed + ED_MAP_OFF, &map) || !map) return 0;
    AcquireSRWLockShared(&g_status_lock);
    call->tile_row = g_status_tile_row;
    call->unknown_row = g_status_unknown_row;
    strncpy_s(call->record_class, sizeof call->record_class, g_record_class, _TRUNCATE);
    ReleaseSRWLockShared(&g_status_lock);
    return 1;
}
#endif
