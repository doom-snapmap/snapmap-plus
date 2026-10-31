/* module_prefab.c -- see module_prefab.h.
 *
 * Engine surface (build 13954591, Vulkan / OpenGL, snaphak-re R-002/R-003):
 *   SnapMapCtor / SnapMapDtor     idSnapMap in place, 0x778 bytes
 *   DeserializeFromJson           native parse (the load swap is bypassed)
 *   SnapMapValidate               the validator GetLocalSavedMapEdit runs first
 *   SnapMapEditCtor / Dtor        idSnapMapEdit, 0xdd0 bytes, from an idSnapMap
 *   ModuleRedoAction -> Redo      Redo(editor) is the rel32 CALL at +0x13
 *   ModuleSelect                  the Module Mode select of one module index
 *   EditorCursorPos / ModuleCursorFloor   the Blueprint floor point under the cursor
 *   EditorBudgetUpdate            recompute the budget, limits byte editor+0x236d0
 *   GridWorldBounds               a module record's world box
 *   EditorMapToJson               (rawmap.c) the open map as JSON
 * Every engine read and call is SEH-guarded; a fault retires the module. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "module_prefab.h"
#include "module_prefab_map.h"
#include "rawmap.h"
#include "apply_engine.h"
#include "grid_room_native.h"
#include "commands.h"
#include "ui_bridge.h"
#include "backend_log.h"
#include "process_heap_scope.h"


/* editor */
#define ED_UNDO_OFF          0x10      /* -> idList<idSnapMapEdit*>: data +0, num +8, size +0xc, cursor +0x18 */
#define ED_UNDO_HDR_BYTES    0x20
#define ED_UNDO_CURSOR_OFF   0x18
#define ED_INPUT_OFF         0x20      /* input object EditorCursorPos reads */
#define ED_MAP_OFF           0x204c8   /* idSnapMapEdit * */
#define ED_SEL_OFF           0x204d0   /* selection / edit data */
#define ED_MODE_OFF          0x212b8   /* Module Mode */
#define ED_STATE_OFF         0x23618   /* 1 Blueprint, 2 Object Mode */
#define ED_LIMITS_OFF        0x236d0   /* byte: an editor budget is at or over its limit */
/* Module Mode */
#define MODE_CURRENT_OFF     0x28      /* substate running: 1 Empty, 2 Selection, 3 Edit, 4 Add */
#define MODE_SUBSTATE_OFF    0x2c      /* substate requested; the mode update enters it when it differs */
#define MODE_EDIT_FLAGS_OFF  0x870     /* the Edit substate (mode+0x810) flags byte +0x60: 1 duplicate, 4 move */
#define MODE_PENDING_OFF     0x1070    /* byte: substate change requested */
#define MODE_REFRESH_OFF     0x1071
/* selection */
#define SEL_LIST_OFF         0x68      /* int *selected module indices (sorted) */
#define SEL_COUNT_OFF        0x70
/* edit map */
#define MAP_RECORDS_OFF      0x750
#define MAP_RECORD_COUNT_OFF 0x758
#define MAP_SETTINGS_OFF     0x7f8
#define MAP_ENT_COUNT_OFF    0x6a8
#define RECORD_STRIDE        0x98
#define RECORD_ORIGIN_OFF    0x0c
#define MODULE_LOCAL_BOUNDS  0xa0
#define SNAPMAP_BYTES        0x778
#define SNAPMAP_EDIT_BYTES   0xdd0
#define ACTION_REDO_CALL_OFF 0x13
#define MODULE_CAP           1024
#define GRAB_WAIT_FRAMES     90
#define IDSTR_BYTES          0x30

typedef void  (*snapmap_ctor_fn)(void *map);
typedef void  (*snapmap_dtor_fn)(void *map);
typedef int   (*validate_fn)(void *map, void *r98, void *r88);
typedef void *(*edit_ctor_fn)(void *edit, void *settings, void *map, void *r98, void *r88);
typedef void  (*edit_dtor_fn)(void *edit);
typedef char  (*redo_fn)(void *editor);
typedef char  (*select_fn)(void *sel, int index);
typedef int  *(*cursor_fn)(void *input, int *out2);
typedef float*(*floor_fn)(void *mode, float *out3, void *editor, const int *xy);
typedef void  (*budget_fn)(void *editor);
typedef float*(*world_bounds_fn)(void *record, float *out6, const float *local6);
typedef void  (*commit_fn)(void *editor, void *idstr);
typedef void *(*idstr_ctor_fn)(void *s, const char *text);
typedef void  (*idstr_dtor_fn)(void *s);
typedef void *(*vcall_fn)(void *self);

static snapmap_ctor_fn g_map_ctor;
static snapmap_dtor_fn g_map_dtor;
static validate_fn     g_validate;
static edit_ctor_fn    g_edit_ctor;
static edit_dtor_fn    g_edit_dtor;
static redo_fn         g_redo;
static select_fn       g_select;
static cursor_fn       g_cursor;
static floor_fn        g_floor;
static budget_fn       g_budget;
static world_bounds_fn g_world_bounds;
static commit_fn       g_commit;
static idstr_ctor_fn   g_idstr_ctor;
static idstr_dtor_fn   g_idstr_dtor;
static void           *g_deser;
static sh_run_on_main_fn g_run_on_main;

static sh_process_heap_api g_heap;
static int             g_heap_ok;
static volatile LONG   g_faulted;
static void * volatile g_editor;        /* last editor seen by the frame tick */

enum { MP_IDLE = 0, MP_INSTALL, MP_SELECT_WAIT, MP_GRAB_WAIT, MP_GRABBED };

/* One placement at a time. The phase moves only on the game thread, except
 * IDLE -> INSTALL, which the slot does under the lock. */
static SRWLOCK         g_lock = SRWLOCK_INIT;
static volatile LONG   g_phase = MP_IDLE;
static int             g_seq;            /* last placement accepted */
static int             g_done_seq;       /* last placement finished */
static int             g_status = 0;     /* g_done_seq's outcome */
static char            g_status_msg[192];
static char           *g_merged;         /* malloc */
static char           *g_original;       /* HeapAlloc (process heap), the map before the merge */
static int             g_first, g_added, g_has_boxes;
static void           *g_fp_cursor;      /* undo cursor at capture */
static int             g_fp_records;
static void           *g_grab_cursor;    /* undo cursor when the grab began */
static int             g_frames;

static void mp_logf(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void mp_toast(const char *text)
{
    sh_iface *iface = sh_ui_get_iface();
    if (iface && iface->vtbl && iface->vtbl->toast) iface->vtbl->toast(iface, "Module prefab", text);
}

static uintptr_t mp_clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (r[i].name && !strcmp(r[i].name, name) && (r[i].status == SIG_OK || r[i].status == SIG_OK_HOOKED))
            return r[i].addr;
    return 0;
}

/* Follow a rel32 CALL; NULL unless the byte there is E8. */
static void *mp_call_target(const unsigned char *at)
{
    int rel = 0;
    __try {
        if (!at || at[0] != 0xE8) return NULL;
        memcpy(&rel, at + 1, sizeof rel);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return (void *)(at + 5 + rel);
}

int sh_module_prefab_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    uintptr_t redo_action = mp_clean(results, count, "ModuleRedoAction");
    g_map_ctor  = (snapmap_ctor_fn)mp_clean(results, count, "SnapMapCtor");
    g_map_dtor  = (snapmap_dtor_fn)mp_clean(results, count, "SnapMapDtor");
    g_validate  = (validate_fn)mp_clean(results, count, "SnapMapValidate");
    g_edit_ctor = (edit_ctor_fn)mp_clean(results, count, "SnapMapEditCtor");
    g_edit_dtor = (edit_dtor_fn)mp_clean(results, count, "SnapMapEditDtor");
    g_select    = (select_fn)mp_clean(results, count, "ModuleSelect");
    g_cursor    = (cursor_fn)mp_clean(results, count, "EditorCursorPos");
    g_floor     = (floor_fn)mp_clean(results, count, "ModuleCursorFloor");
    g_budget    = (budget_fn)mp_clean(results, count, "EditorBudgetUpdate");
    g_world_bounds = (world_bounds_fn)mp_clean(results, count, "GridWorldBounds");
    g_commit    = (commit_fn)mp_clean(results, count, "EditorCommitEdit");
    g_idstr_ctor = (idstr_ctor_fn)mp_clean(results, count, "IdStrCtor");
    g_idstr_dtor = (idstr_dtor_fn)mp_clean(results, count, "IdStrDtor");
    g_deser     = (void *)mp_clean(results, count, "DeserializeFromJson");
    g_redo      = redo_action ? (redo_fn)mp_call_target((const unsigned char *)redo_action + ACTION_REDO_CALL_OFF) : NULL;
    sh_apply_engine_get_run_on_main(&g_run_on_main);
    /* Native undo, redo and commit run inside the editor frame's heap 0 scope;
     * the install and the fallback commit run in the same scope. */
    g_heap_ok = sh_process_heap_bind(&g_heap, results, count, module_base);
    mp_logf("MODPREFAB: map %p/%p validate %p edit %p/%p redo %p select %p cursor %p/%p budget %p bounds %p deser %p",
            (void *)g_map_ctor, (void *)g_map_dtor, (void *)g_validate, (void *)g_edit_ctor, (void *)g_edit_dtor,
            (void *)g_redo, (void *)g_select, (void *)g_cursor, (void *)g_floor, (void *)g_budget,
            (void *)g_world_bounds, g_deser);
    return g_map_ctor && g_map_dtor && g_edit_ctor && g_edit_dtor && g_redo && g_select && g_deser && g_run_on_main &&
           g_heap_ok;
}

static int mp_ready_place(void)
{
    return !InterlockedCompareExchange(&g_faulted, 0, 0) && g_map_ctor && g_map_dtor && g_edit_ctor && g_edit_dtor &&
           g_redo && g_select && g_deser && g_run_on_main && g_heap_ok && sh_rawmap_live_serialize_ready();
}

int sh_module_prefab_active(void) { return InterlockedCompareExchange(&g_phase, 0, 0) != MP_IDLE; }

/* ------------------------------------------------------------ guarded reads */

static int rd_i32(const void *p, int *v) { __try { *v = *(const int *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int rd_ptr(const void *p, void **v) { __try { *v = *(void *const *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int rd_u8(const void *p, unsigned char *v) { __try { *v = *(const unsigned char *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

static unsigned char *mp_editor(void)
{
    unsigned char *ed = (unsigned char *)g_editor;
    void *map = NULL;
    if (!ed || !rd_ptr(ed + ED_MAP_OFF, &map) || !map) return NULL;
    return ed;
}

/* Blueprint, in Empty or Selection (a selection is replaced by the placed modules), and no
 * substate change pending. */
static int mp_blueprint_idle(unsigned char *ed, int *state_out)
{
    int state = 0, sub = 0, cur = 0;
    unsigned char pending = 1;
    if (!rd_i32(ed + ED_STATE_OFF, &state)) return 0;
    if (state_out) *state_out = state;
    if (state != 1) return 0;
    if (!rd_i32(ed + ED_MODE_OFF + MODE_SUBSTATE_OFF, &sub) || !rd_i32(ed + ED_MODE_OFF + MODE_CURRENT_OFF, &cur) ||
        !rd_u8(ed + ED_MODE_OFF + MODE_PENDING_OFF, &pending)) return 0;
    return (sub == 1 || sub == 2) && cur == sub && !pending;
}

static void *mp_undo_cursor(unsigned char *ed)
{
    void *list = NULL, *c = NULL;
    if (rd_ptr(ed + ED_UNDO_OFF, &list) && list) rd_ptr((unsigned char *)list + ED_UNDO_CURSOR_OFF, &c);
    return c;
}

static int mp_record_count(unsigned char *ed)
{
    void *map = NULL;
    int n = -1;
    if (rd_ptr(ed + ED_MAP_OFF, &map) && map) rd_i32((unsigned char *)map + MAP_RECORD_COUNT_OFF, &n);
    return n;
}

/* ------------------------------------------------------------ the install */

/* Build an idSnapMapEdit from json and assign it into the open map through
 * Redo with a stand-in undo list. Game thread. 1 on success. */
static int mp_install_scoped(unsigned char *ed, const char *json, char *why, int why_cap);
/* mp_install_scoped inside the process heap scope native undo uses, then the
 * budget recompute; *limits = the editor reports a budget at or over its limit. */
static int mp_install(unsigned char *ed, const char *json, char *why, int why_cap, unsigned char *limits)
{
    sh_process_heap_scope scope;
    int ok;
    if (limits) *limits = 0;
    if (!sh_process_heap_enter(&g_heap, &scope)) {
        _snprintf_s(why, why_cap, _TRUNCATE, "the engine heap scope could not be entered");
        return 0;
    }
    ok = mp_install_scoped(ed, json, why, why_cap);
    if (ok && g_budget) {
        __try { g_budget(ed); if (limits) *limits = *(unsigned char *)(ed + ED_LIMITS_OFF); }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_faulted, 1); }
    }
    if (!sh_process_heap_leave(&scope)) {
        InterlockedExchange(&g_faulted, 1);
        backend_log("MODPREFAB: the heap scope did not restore; module prefabs are off for this session");
    }
    return ok;
}
static int mp_install_scoped(unsigned char *ed, const char *json, char *why, int why_cap)
{
    void *map = NULL, *r98 = NULL, *r88 = NULL;
    unsigned char *sm = NULL, *edit = NULL, *undo = NULL;
    unsigned char saved[ED_UNDO_HDR_BYTES];
    void *entries[2];
    int built_map = 0, built_edit = 0, swapped = 0, ok = 0, rc = 0;
    char stepped = 0;
    sm = (unsigned char *)_aligned_malloc(SNAPMAP_BYTES + 0x100, 16);
    edit = (unsigned char *)_aligned_malloc(SNAPMAP_EDIT_BYTES + 0x100, 16);
    if (!sm || !edit) { _snprintf_s(why, why_cap, _TRUNCATE, "out of memory"); goto out; }
    memset(sm, 0, SNAPMAP_BYTES + 0x100);
    memset(edit, 0, SNAPMAP_EDIT_BYTES + 0x100);
    __try {
        map = *(void **)(ed + ED_MAP_OFF);
        undo = *(unsigned char **)(ed + ED_UNDO_OFF);
        if (!map || !undo) { _snprintf_s(why, why_cap, _TRUNCATE, "no map is open"); __leave; }
        g_map_ctor(sm); built_map = 1;
        rc = sh_rawmap_deserialize_plain(g_deser, json, sm);
        if (!rc) { _snprintf_s(why, why_cap, _TRUNCATE, "the engine could not read the merged map"); __leave; }
        r98 = ((vcall_fn)(*(void ***)ed)[0x98 / 8])(ed);
        r88 = ((vcall_fn)(*(void ***)ed)[0x88 / 8])(ed);
        if (g_validate) {
            rc = g_validate(sm, r98, r88);
            if (rc != 0) { _snprintf_s(why, why_cap, _TRUNCATE, "the engine refused the merged map (validator %d)", rc); __leave; }
        }
        g_edit_ctor(edit, (unsigned char *)map + MAP_SETTINGS_OFF, sm, r98, r88); built_edit = 1;
        /* Redo steps the cursor from entries[0] to entries[1] and assigns it. */
        memcpy(saved, undo, sizeof saved);
        entries[0] = edit; entries[1] = edit;
        *(void ***)undo = entries;
        *(int *)(undo + 8) = 2;
        *(int *)(undo + 0xc) = 2;
        *(void ***)(undo + ED_UNDO_CURSOR_OFF) = &entries[0];
        swapped = 1;
        __try { stepped = g_redo(ed); }
        __finally { memcpy(undo, saved, sizeof saved); swapped = 0; }
        if (!stepped) { _snprintf_s(why, why_cap, _TRUNCATE, "the editor did not take the map"); __leave; }
        *(unsigned char *)(ed + ED_MODE_OFF + MODE_PENDING_OFF) = 1;
        *(unsigned char *)(ed + ED_MODE_OFF + MODE_REFRESH_OFF) = 1;
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (swapped && undo) memcpy(undo, saved, sizeof saved);
        InterlockedExchange(&g_faulted, 1);
        _snprintf_s(why, why_cap, _TRUNCATE, "the install faulted; module prefabs are off for this session");
        ok = 0;
    }
    __try {
        if (built_edit) g_edit_dtor(edit);
        if (built_map) g_map_dtor(sm);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted, 1);
        ok = 0;
    }
out:
    if (sm) _aligned_free(sm);
    if (edit) _aligned_free(edit);
    return ok;
}

static void mp_finish(int status, const char *msg)
{
    AcquireSRWLockExclusive(&g_lock);
    g_status = status;
    strncpy_s(g_status_msg, sizeof g_status_msg, msg ? msg : "", _TRUNCATE);
    g_done_seq = g_seq;
    ReleaseSRWLockExclusive(&g_lock);
    mp_logf("MODPREFAB: placement %d -> %d (%s)", g_seq, status, msg ? msg : "");
}

static void mp_release_buffers(void)
{
    free(g_merged); g_merged = NULL;
    if (g_original) { HeapFree(GetProcessHeap(), 0, g_original); g_original = NULL; }
}

/* The selection list holds exactly the placed modules? */
static int mp_selection_is_placed(unsigned char *ed, int *count_out)
{
    int i, n = 0, ok = 1;
    __try {
        unsigned char *sel = *(unsigned char **)(ed + ED_SEL_OFF);
        int *list = sel ? *(int **)(sel + SEL_LIST_OFF) : NULL;
        n = sel ? *(int *)(sel + SEL_COUNT_OFF) : 0;
        if (n != g_added || !list) ok = 0;
        for (i = 0; ok && i < n; i++) if (list[i] != g_first + i) ok = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (count_out) *count_out = n;
    return ok;
}

/* Select the placed modules through the native Module Mode select. One select
 * gathers the modules connected to it, so only indices not yet in the list are
 * selected (a second select of a selected module would toggle it off). When
 * the gather also took modules of the open map, the list is narrowed in place
 * to the placed ones (its capacity already holds more). */
static int mp_select_placed(unsigned char *ed)
{
    int i, j, n = 0;
    __try {
        unsigned char *sel = *(unsigned char **)(ed + ED_SEL_OFF);
        if (!sel) return 0;
        for (i = 0; i < g_added; i++) {
            int idx = g_first + i, have = 0;
            int *list = *(int **)(sel + SEL_LIST_OFF);
            n = *(int *)(sel + SEL_COUNT_OFF);
            for (j = 0; list && j < n; j++) if (list[j] == idx) { have = 1; break; }
            if (!have) g_select(sel, idx);
        }
        if (!mp_selection_is_placed(ed, &n)) {
            int *list = *(int **)(sel + SEL_LIST_OFF);
            int have = 0;
            for (j = 0; list && j < n; j++) if (list[j] >= g_first && list[j] < g_first + g_added) have++;
            if (list && have == g_added && n >= g_added) {
                for (i = 0; i < g_added; i++) list[i] = g_first + i;
                *(int *)(sel + SEL_COUNT_OFF) = g_added;
                mp_logf("MODPREFAB: the select also took %d module(s) of the open map; narrowed to the placed ones", n - g_added);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted, 1);
        return 0;
    }
    return mp_selection_is_placed(ed, &n);
}

/* The select grows the selection list with the engine allocator, so it runs in the
 * process heap scope native selects use: a list allocated on the map heap is
 * released by Play's teardown and the next selection clear corrupts the heap. */
static int mp_select_placed_scoped(unsigned char *ed)
{
    sh_process_heap_scope scope;
    int ok;
    if (!sh_process_heap_enter(&g_heap, &scope)) return 0;
    ok = mp_select_placed(ed);
    if (!sh_process_heap_leave(&scope)) { InterlockedExchange(&g_faulted, 1); ok = 0; }
    return ok;
}

/* Ask Module Mode for a substate change, as its own request helpers do. */
static void mp_request_substate(unsigned char *ed, int sub)
{
    unsigned char *mode = ed + ED_MODE_OFF;
    __try {
        if (sub == 3) {
            /* the 0x3b Move request: clear the duplicate bit, bit 3 = came from Selection */
            mode[MODE_EDIT_FLAGS_OFF] = (unsigned char)((mode[MODE_EDIT_FLAGS_OFF] & ~0x09) |
                                        (*(int *)(mode + MODE_SUBSTATE_OFF) == 2 ? 8 : 0));
        }
        *(int *)(mode + MODE_SUBSTATE_OFF) = sub;
        mode[MODE_PENDING_OFF] = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_faulted, 1); }
}

static void mp_commit(unsigned char *ed, const char *label)
{
    unsigned char s[IDSTR_BYTES + 16];
    sh_process_heap_scope scope;
    if (!g_commit || !g_idstr_ctor || !g_idstr_dtor) return;
    if (!sh_process_heap_enter(&g_heap, &scope)) return;
    __try {
        memset(s, 0, sizeof s);
        g_idstr_ctor(s, label);
        g_commit(ed, s);
        g_idstr_dtor(s);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_faulted, 1); }
    if (!sh_process_heap_leave(&scope)) InterlockedExchange(&g_faulted, 1);
}

/* The placement could not be grabbed: keep it, as one undo step, where it landed. */
static void mp_keep_unmoved(unsigned char *ed, const char *why)
{
    mp_commit(ed, "Place Module Prefab");
    mp_finish(1, why);
    InterlockedExchange(&g_phase, MP_IDLE);
}

void sh_module_prefab_tick(void *editor)
{
    unsigned char *ed = (unsigned char *)editor;
    LONG phase;
    char why[192] = "";
    int state = 0, current = 0;
    if (ed) g_editor = ed;
    phase = InterlockedCompareExchange(&g_phase, 0, 0);
    if (phase == MP_IDLE || !ed) return;
    if (InterlockedCompareExchange(&g_faulted, 0, 0)) {
        mp_release_buffers();
        mp_finish(0, "module prefabs are off for this session");
        InterlockedExchange(&g_phase, MP_IDLE);
        return;
    }
    rd_i32(ed + ED_STATE_OFF, &state);
    rd_i32(ed + ED_MODE_OFF + MODE_CURRENT_OFF, &current);
    if (phase == MP_INSTALL) {
        unsigned char limits = 0;
        if (!mp_blueprint_idle(ed, NULL)) {
            mp_release_buffers(); mp_finish(0, "Blueprint mode was busy; nothing was placed");
            InterlockedExchange(&g_phase, MP_IDLE); return;
        }
        if (mp_undo_cursor(ed) != g_fp_cursor || mp_record_count(ed) != g_fp_records) {
            mp_release_buffers(); mp_finish(0, "the map changed before the placement; try again");
            InterlockedExchange(&g_phase, MP_IDLE); return;
        }
        if (!mp_install(ed, g_merged, why, (int)sizeof why, &limits)) {
            mp_release_buffers(); mp_finish(0, why);
            InterlockedExchange(&g_phase, MP_IDLE); return;
        }
        if (mp_record_count(ed) != g_fp_records + g_added)
            mp_logf("MODPREFAB: record count %d after install, expected %d", mp_record_count(ed), g_fp_records + g_added);
        if (limits) {
            unsigned char again = 0;
            int back = g_original && mp_install(ed, g_original, why, (int)sizeof why, &again);
            mp_release_buffers();
            mp_finish(-5, back ? "the prefab would put the map over the editor's limits"
                               : "over the editor's limits, and the map could not be put back");
            mp_toast("This prefab would put the map over the editor's limits.");
            InterlockedExchange(&g_phase, MP_IDLE); return;
        }
        mp_release_buffers();
        g_grab_cursor = mp_undo_cursor(ed);
        g_frames = 0;
        if (!mp_select_placed_scoped(ed)) { mp_keep_unmoved(ed, "placed; select and move it with the Blueprint tools"); return; }
        /* a native select leaves Empty for Selection; the Move grab is asked for from there */
        mp_request_substate(ed, 2);
        mp_logf("MODPREFAB: %d placed module(s) selected (first %d)", g_added, g_first);
        InterlockedExchange(&g_phase, MP_SELECT_WAIT);
        return;
    }
    g_frames++;
    if (phase == MP_SELECT_WAIT) {
        if (state == 1 && current == 2) {
            if (!mp_selection_is_placed(ed, NULL) && !mp_select_placed_scoped(ed)) {
                mp_keep_unmoved(ed, "placed; the selection changed before the grab");
                return;
            }
            mp_request_substate(ed, 3);
            g_frames = 0;
            InterlockedExchange(&g_phase, MP_GRAB_WAIT);
            return;
        }
        if (g_frames > GRAB_WAIT_FRAMES || state != 1) mp_keep_unmoved(ed, "placed; the Selection mode did not open");
        return;
    }
    if (phase == MP_GRAB_WAIT) {
        if (state == 1 && current == 3) {
            /* Edit has entered as a Move. Mark it a duplicate: Space commits one
             * "Duplicate Modules" step, Esc deletes the placed group. */
            __try {
                unsigned char *f = ed + ED_MODE_OFF + MODE_EDIT_FLAGS_OFF;
                *f = (unsigned char)((*f | 1) & ~4);
            } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_faulted, 1); }
            mp_logf("MODPREFAB: grab live after %d frame(s); Space places, Esc cancels", g_frames);
            mp_finish(1, "placed and grabbed; move it, then Space to place or Esc to cancel");
            InterlockedExchange(&g_phase, MP_GRABBED);
            return;
        }
        if (g_frames > GRAB_WAIT_FRAMES || state != 1) mp_keep_unmoved(ed, "placed; the move grab did not start, so it stays where it landed");
        return;
    }
    if (phase == MP_GRABBED) {
        if (state == 1 && current == 3) return;     /* still holding it */
        if (mp_undo_cursor(ed) != g_grab_cursor) {
            mp_logf("MODPREFAB: placement accepted (%d module(s)); navigation update requested", g_added);
            sh_apply_engine_nav_request_update();
        } else {
            mp_logf("MODPREFAB: placement cancelled; the placed modules were removed");
        }
        InterlockedExchange(&g_phase, MP_IDLE);
    }
}

/* ------------------------------------------------------------ capture (game thread) */

/* The module guard's palette query (module_prefab_map.h): grid_room_native.c
 * answers it on the engine thread. */
static int mp_module_known(void *ctx, const char *name)
{
    (void)ctx;
    return sh_grid_native_module_known(name);
}

typedef struct mp_capture {
    int    op;
    int    result;            /* 1 ok or a refusal code */
    const sh_mp_summary *modules;   /* PLACE / CHECK: the prefab's modules, checked on the engine thread */
    sh_mp_module_check check;
    int    checked;           /* sh_mp_check_modules' result: missing count, -1 not checked */
    char  *json;              /* HeapAlloc */
    size_t len;
    int    sel[MODULE_CAP];
    int    nsel;
    float  bounds[MODULE_CAP][6];
    int    have_bounds;
    float  anchor[3];
    float  floor[3];
    int    have_floor;
    void  *undo_cursor;
    int    records;
} mp_capture;

/* Check the prefab's modules on the engine thread. A refusal names them on the
 * console (one line). 1 when placement may go on. */
static int mp_capture_check(mp_capture *c)
{
    c->checked = sh_mp_check_modules(c->modules, mp_module_known, NULL, &c->check);
    if (c->checked > 0) {
        char line[1200];
        sh_mp_missing_console_line(&c->check, line, sizeof line);
        sh_printf("%s\n", line);
        mp_logf("MODPREFAB: placement refused: %.280s", line);
        return 0;
    }
    return 1;
}

static int mp_capture_main(void *ctx)
{
    mp_capture *c = (mp_capture *)ctx;
    unsigned char *ed = mp_editor();
    void *map = NULL, *sel = NULL;
    int state = 0, i, n = 0;
    c->result = -1;
    if (!ed) return 0;
    if (c->op == SH_MODULE_PREFAB_CHECK) {
        c->checked = sh_mp_check_modules(c->modules, mp_module_known, NULL, &c->check);
        c->result = 1;
        return 1;
    }
    if (!mp_blueprint_idle(ed, &state)) {
        int sub = 0;
        unsigned char pending = 1;
        if (state != 1) { c->result = (c->op == SH_MODULE_PREFAB_SAVE) ? -2 : -4; return 0; }
        c->result = (c->op == SH_MODULE_PREFAB_SAVE) ? -1 : 0;
        /* Save also reads a selection made in the Selection substate. */
        rd_i32(ed + ED_MODE_OFF + MODE_SUBSTATE_OFF, &sub);
        rd_u8(ed + ED_MODE_OFF + MODE_PENDING_OFF, &pending);
        if (c->op != SH_MODULE_PREFAB_SAVE || sub != 2 || pending) return 0;
    }
    __try {
        map = *(void **)(ed + ED_MAP_OFF);
        sel = *(void **)(ed + ED_SEL_OFF);
        c->records = *(int *)((unsigned char *)map + MAP_RECORD_COUNT_OFF);
        c->undo_cursor = mp_undo_cursor(ed);
        if (c->op == SH_MODULE_PREFAB_SAVE) {
            int *list = sel ? *(int **)((unsigned char *)sel + SEL_LIST_OFF) : NULL;
            unsigned char *records = *(unsigned char **)((unsigned char *)map + MAP_RECORDS_OFF);
            n = sel ? *(int *)((unsigned char *)sel + SEL_COUNT_OFF) : 0;
            if (n <= 0 || !list) { c->result = -3; return 0; }
            if (n > MODULE_CAP) n = MODULE_CAP;
            c->have_bounds = g_world_bounds != NULL;
            for (i = 0; i < n; i++) {
                int idx = list[i];
                unsigned char *rec, *wrapper, *module;
                if (idx < 0 || idx >= c->records) { c->result = -1; return 0; }
                c->sel[c->nsel++] = idx;
                rec = records + (size_t)idx * RECORD_STRIDE;
                if (i == 0) memcpy(c->anchor, rec + RECORD_ORIGIN_OFF, sizeof c->anchor);
                if (c->have_bounds) {
                    wrapper = *(unsigned char **)rec;
                    module = wrapper ? *(unsigned char **)wrapper : NULL;
                    if (!module) c->have_bounds = 0;
                    else g_world_bounds(rec, c->bounds[i], (const float *)(module + MODULE_LOCAL_BOUNDS));
                }
            }
        } else if (c->modules && !mp_capture_check(c)) {
            c->result = -6;
            return 0;
        } else if (g_cursor && g_floor) {
            int xy[2] = { 0, 0 };
            float out[3] = { 0, 0, 0 };
            g_cursor(ed + ED_INPUT_OFF, xy);
            g_floor(ed + ED_MODE_OFF, out, ed, xy);
            if (out[0] == out[0] && out[1] == out[1] && out[2] == out[2] &&
                fabsf(out[0]) < 1e6f && fabsf(out[1]) < 1e6f && fabsf(out[2]) < 1e6f) {
                memcpy(c->floor, out, sizeof out);
                c->have_floor = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted, 1);
        c->result = -1;
        return 0;
    }
    if (!sh_rawmap_serialize_live_copy(map, &c->json, &c->len)) { c->result = -1; return 0; }
    c->result = 1;
    return 1;
}

static int mp_capture_run(mp_capture *c)
{
    int r = 0;
    if (!g_run_on_main) return 0;
    /* The Prefabs tab's check waits less: it runs on every selection. */
    if (g_run_on_main(NULL, mp_capture_main, c, c->op == SH_MODULE_PREFAB_CHECK ? 1000 : 3000, &r) != 1) return 0;
    return 1;
}

static int mp_write_file(const char *path, const char *data, size_t len)
{
    FILE *f = NULL;
    size_t put;
    if (!path || !*path || fopen_s(&f, path, "wb") != 0 || !f) return 0;
    put = fwrite(data, 1, len, f);
    if (fclose(f) != 0) return 0;
    return put == len;
}

static void mp_out(sh_module_prefab_call *call, const char *text)
{
    if (call->out && call->out_cap > 0) strncpy_s(call->out, (size_t)call->out_cap, text ? text : "", _TRUNCATE);
}

static int mp_save(sh_module_prefab_call *call)
{
    mp_capture *c = (mp_capture *)calloc(1, sizeof *c);
    char err[256] = "";
    char *frag = NULL, *side = NULL;
    size_t flen = 0;
    sh_mp_summary sum;
    float (*rel)[6] = NULL;
    int i, k, result = -1;
    memset(&sum, 0, sizeof sum);
    if (!c) return -1;
    c->op = SH_MODULE_PREFAB_SAVE;
    if (!mp_capture_run(c)) { mp_out(call, "the game did not answer"); goto done; }
    if (c->result != 1) {
        result = c->result;
        mp_out(call, result == -2 ? "switch to Blueprint mode" : result == -3 ? "select one or more modules" : "the open map could not be read");
        goto done;
    }
    if (!sh_mp_cut(c->json, c->len, c->sel, c->nsel, &frag, &flen, &sum, err, (int)sizeof err)) {
        mp_logf("MODPREFAB: save refused: %s", err);
        mp_out(call, err);
        goto done;
    }
    if (c->have_bounds) {
        rel = (float (*)[6])calloc((size_t)c->nsel, sizeof *rel);
        if (rel) for (i = 0; i < c->nsel; i++) for (k = 0; k < 6; k++) rel[i][k] = c->bounds[i][k] - c->anchor[k % 3];
    }
    side = sh_mp_sidecar_json(&sum, rel ? (const float (*)[6])rel : NULL, NULL);
    if (!side || !mp_write_file(call->path, frag, flen) || !mp_write_file(call->meta_path, side, strlen(side))) {
        mp_out(call, "the prefab file could not be written");
        goto done;
    }
    mp_logf("MODPREFAB: saved %d module(s), %d entities, %d door slot(s), %d variable(s); dropped %d wire(s), %d ref(s) -> %s",
            sum.module_count, sum.entity_count, sum.door_count, sum.variable_count, sum.dropped_wires, sum.dropped_refs,
            call->path);
    mp_out(call, side);
    result = 1;
done:
    sh_mp_summary_free(&sum);
    free(frag); free(side); free(rel);
    if (c->json) HeapFree(GetProcessHeap(), 0, c->json);
    free(c);
    return result;
}

static int mp_place(sh_module_prefab_call *call)
{
    mp_capture *c;
    char err[256] = "";
    char *merged = NULL;
    size_t mlen = 0;
    float at[3];
    int first = 0, added = 0, boxes = 0;
    sh_mp_summary sum;
    if (!call->text || !sh_mp_is_module_file(call->text, call->text_len)) { mp_out(call, "not a module prefab"); return -1; }
    if (!sh_mp_inspect(call->text, call->text_len, &sum, err, (int)sizeof err)) { mp_out(call, err); return -1; }
    boxes = sum.has_nav_boxes;
    if (InterlockedCompareExchange(&g_phase, 0, 0) != MP_IDLE) { sh_mp_summary_free(&sum); mp_out(call, "a placement is still in progress"); return 0; }
    c = (mp_capture *)calloc(1, sizeof *c);
    if (!c) { sh_mp_summary_free(&sum); return 0; }
    c->op = SH_MODULE_PREFAB_PLACE;
    c->modules = &sum;
    if (!mp_capture_run(c)) { sh_mp_summary_free(&sum); mp_out(call, "the game did not answer"); free(c); return 0; }
    sh_mp_summary_free(&sum);
    c->modules = NULL;
    if (c->result == -6) {
        char toast[600];
        sh_mp_missing_toast(&c->check, toast, sizeof toast);
        mp_toast(toast);
        mp_out(call, toast);
        if (c->json) HeapFree(GetProcessHeap(), 0, c->json);
        free(c);
        return -6;
    }
    if (c->result != 1) {
        int r = c->result == -4 ? -4 : 0;
        mp_out(call, r == -4 ? "switch to Blueprint mode" : "finish the Blueprint action first");
        if (c->json) HeapFree(GetProcessHeap(), 0, c->json);
        free(c);
        return r;
    }
    /* the anchor at the floor point under the cursor, on the 128 grid */
    if (c->have_floor) {
        at[0] = 128.0f * floorf(c->floor[0] / 128.0f + 0.5f);
        at[1] = 128.0f * floorf(c->floor[1] / 128.0f + 0.5f);
        at[2] = c->floor[2];
    } else { at[0] = at[1] = at[2] = 0.0f; }
    if (!sh_mp_merge(c->json, c->len, call->text, call->text_len, at, &merged, &mlen, &first, &added, err, (int)sizeof err)) {
        mp_logf("MODPREFAB: merge refused: %s", err);
        mp_out(call, err);
        HeapFree(GetProcessHeap(), 0, c->json);
        free(c);
        return -1;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_phase != MP_IDLE) {
        ReleaseSRWLockExclusive(&g_lock);
        free(merged); HeapFree(GetProcessHeap(), 0, c->json); free(c);
        mp_out(call, "a placement is still in progress");
        return 0;
    }
    g_merged = merged;
    g_original = c->json;
    g_first = first; g_added = added; g_has_boxes = boxes;
    g_fp_cursor = c->undo_cursor; g_fp_records = c->records;
    call->seq = ++g_seq;
    InterlockedExchange(&g_phase, MP_INSTALL);
    ReleaseSRWLockExclusive(&g_lock);
    mp_logf("MODPREFAB: placement %d staged: %d module(s) at (%.0f %.0f %.0f)%s, first index %d, %zu bytes",
            call->seq, added, at[0], at[1], at[2], c->have_floor ? "" : " [no cursor point]", first, mlen);
    free(c);
    mp_out(call, "staged");
    return 2;
}

/* The Prefabs tab's pre-Load check: the same module check, nothing placed. */
static int mp_check(sh_module_prefab_call *call)
{
    mp_capture *c;
    sh_mp_summary sum;
    char err[256] = "", *json;
    int r = -1;
    mp_out(call, "{\"checked\":false}");
    if (!g_run_on_main || !call->text || !sh_mp_is_module_file(call->text, call->text_len)) return -1;
    if (!sh_mp_inspect(call->text, call->text_len, &sum, err, (int)sizeof err)) return -1;
    c = (mp_capture *)calloc(1, sizeof *c);
    if (!c) { sh_mp_summary_free(&sum); return -1; }
    c->op = SH_MODULE_PREFAB_CHECK;
    c->modules = &sum;
    c->checked = -1;
    if (mp_capture_run(c) && c->result == 1) r = c->checked;
    sh_mp_summary_free(&sum);
    json = sh_mp_check_json(r >= 0 ? &c->check : NULL);
    if (json) { mp_out(call, json); free(json); }
    free(c);
    return r;
}

int sh_module_prefab_slot(struct sh_iface *self, sh_module_prefab_call *call)
{
    (void)self;
    if (!call) return 0;
    switch (call->op) {
    case SH_MODULE_PREFAB_STATE: {
        unsigned char *ed = mp_editor();
        int state = 0;
        call->blueprint = call->entity_mode = 0;
        if (!ed || !rd_i32(ed + ED_STATE_OFF, &state)) return 0;
        call->blueprint = state == 1;
        call->entity_mode = state == 2;
        return 1;
    }
    case SH_MODULE_PREFAB_SAVE:
        if (InterlockedCompareExchange(&g_faulted, 0, 0) || !sh_rawmap_live_serialize_ready()) {
            mp_out(call, "module prefabs are unavailable on this build");
            return -1;
        }
        return mp_save(call);
    case SH_MODULE_PREFAB_PLACE:
        if (!mp_ready_place()) { mp_out(call, "module prefab placement is unavailable on this build"); return 0; }
        return mp_place(call);
    case SH_MODULE_PREFAB_CHECK:
        return mp_check(call);
    case SH_MODULE_PREFAB_STATUS: {
        int r;
        AcquireSRWLockShared(&g_lock);
        if (call->seq > g_done_seq) { r = 2; mp_out(call, "pending"); }
        else { r = g_status; mp_out(call, g_status_msg); }
        ReleaseSRWLockShared(&g_lock);
        return r;
    }
    default:
        return 0;
    }
}
