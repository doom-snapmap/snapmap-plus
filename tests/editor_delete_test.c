/* Offline model of DOOM's EntityMode delete path for editor_delete.c.
 *
 * The doubles below follow the native functions the adapter calls and the
 * frame that finishes a deletion: the Selection capability builder includes
 * the hovered entity, the Delete Selection action adds the hovered entity
 * before destroying the selection, and the next EntityMode updates run
 * Exit/Enter, the "Delete Selection" commit and the return to idle. Reading a
 * released slot's declaration is counted as the access violation it would be
 * in the game. Offsets are copied here on purpose so a layout change in the
 * adapter cannot silently change the model too.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "editor_delete.h"

#define T_ED_MAP          0x204C8
#define T_ED_SEL          0x204D0
#define T_ED_RESTRICT     0x20990
#define T_ED_SCREEN       0x21088
#define T_ED_DEACT        0x2120D
#define T_ED_TRANSITION   0x224DC
#define T_ED_MODE         0x22330
#define T_ED_STATE        0x23618
#define T_ED_SIZE         0x23700

#define T_MODE_PREV       0x1A8
#define T_MODE_CUR        0x1AC
#define T_MODE_EMPTY      0x1B0
#define T_MODE_SELECTION  0x1F8
#define T_MODE_QUEUED     0x420
#define T_MODE_DIRTY      0xBB8

#define T_SUB_PHASE       0x08
#define T_SELSUB_COUNTDOWN 0x30
#define T_SELSUB_COMMIT   0x34
#define T_SELSUB_CAPS     0x58

#define T_SEL_MAP         0x00
#define T_SEL_HOVER       0x2C
#define T_SEL_WIRE        0x48
#define T_SEL_IDS         0x80
#define T_SEL_COUNT       0x88
#define T_SEL_CAPACITY    0x8C

#define T_MAP_ENTS        0x6A0
#define T_MAP_ENTCOUNT    0x6A8
#define T_MAP_DOORS       0x778
#define T_MAP_FRAMES      0x790

#define T_SLOT_DECL       0x008
#define T_SLOT_RESTRICT   0x164
#define T_DECL_FLAGS      0x3CC
#define T_SCREEN_UIBITS   0xA28

#define T_IDLE            1
#define T_SELECTED        2
#define T_ENTITIES        16
#define T_SEL_CAP         64

static int failed;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        failed++;                                                               \
    }                                                                           \
} while (0)

/* ---------------------------------------------------------------- world -- */

static uint8_t  g_ed[T_ED_SIZE];
static uint8_t  g_sel[0x100];
static uint8_t  g_map[0x800];
static uint8_t  g_screen[0x1000];
static uint8_t  g_slots[T_ENTITIES][0x300];
static uint8_t  g_decls[T_ENTITIES][0x400];
static uint8_t  g_sentinel[0x300];          /* shared released-slot object: no declaration */
static uint8_t *g_ent_array[T_ENTITIES];
static int      g_sel_ids[T_SEL_CAP];
static int      g_doors[4], g_frames[4];

static int g_null_reads;        /* model access violations: declaration read through a released slot */
static int g_on_main = 1;
static int g_manipulating;
static int g_blocked_mask;      /* restriction bits the editor currently blocks */
static int g_override;          /* the engine's global protected-entity override */
static int g_force_referenced;  /* exercise the action's referenced branch */
static int g_fault_caps_after;  /* raise inside the Nth capability call only; 0 = never */
static int g_fault_delete;      /* raise inside the action */
static int g_action_skips_id = -1;  /* model a divergent action that leaves one target */

enum { C_CLEAR_SEL = 1, C_ADD_SEL, C_CLEAR_HOVER, C_CAPS, C_BLOCKED, C_DELETE, C_DIALOG,
       C_ACTION_CLEAR_HOVER, C_EXIT_EMPTY, C_ENTER_EMPTY, C_EXIT_SELECTION, C_ENTER_SELECTION,
       C_COMMIT, C_SET_IDLE };
static int g_log[512], g_nlog;
static int g_caps_calls;
static int g_commits;
static int g_destroyed[T_ENTITIES], g_ndestroyed;
static int g_hover_at_delete = -2, g_count_at_delete = -1, g_cur_at_delete = -1;

static void logc(int c) { if (g_nlog < (int)(sizeof g_log / sizeof g_log[0])) g_log[g_nlog++] = c; }
static int  log_index(int c) { for (int i = 0; i < g_nlog; i++) if (g_log[i] == c) return i; return -1; }
static int  log_count(int c) { int n = 0; for (int i = 0; i < g_nlog; i++) n += g_log[i] == c; return n; }

static uint8_t *mode(void)  { return g_ed + T_ED_MODE; }
static uint8_t *empty(void) { return mode() + T_MODE_EMPTY; }
static uint8_t *selsub(void){ return mode() + T_MODE_SELECTION; }
static int  *i32(uint8_t *p, size_t off) { return (int *)(p + off); }

static int  sel_count(void) { return *i32(g_sel, T_SEL_COUNT); }
static int  hover(void)     { return *i32(g_sel, T_SEL_HOVER); }
static int  cur(void)       { return *i32(mode(), T_MODE_CUR); }
static int  alive(int id)   { return g_ent_array[id] != g_sentinel; }

static void world_reset(void)
{
    memset(g_ed, 0, sizeof g_ed); memset(g_sel, 0, sizeof g_sel); memset(g_map, 0, sizeof g_map);
    memset(g_screen, 0, sizeof g_screen); memset(g_slots, 0, sizeof g_slots);
    memset(g_decls, 0, sizeof g_decls); memset(g_sentinel, 0, sizeof g_sentinel);
    for (int i = 0; i < T_ENTITIES; i++) {
        g_ent_array[i] = g_slots[i];
        *(uint8_t **)(g_slots[i] + T_SLOT_DECL) = g_decls[i];
        *(uint32_t *)(g_decls[i] + T_DECL_FLAGS) = 0x8;      /* deletable prop */
    }
    *(uint8_t **)(g_ed + T_ED_MAP) = g_map;
    *(uint8_t **)(g_ed + T_ED_SEL) = g_sel;
    *(uint8_t **)(g_ed + T_ED_SCREEN) = g_screen;
    *i32(g_ed, T_ED_STATE) = 2;
    *i32(mode(), T_MODE_CUR) = T_IDLE;
    *i32(mode(), T_MODE_PREV) = T_IDLE;
    *i32(mode(), T_MODE_QUEUED) = -1;
    *i32(empty(), T_SUB_PHASE) = 1;
    *i32(selsub(), T_SUB_PHASE) = 0;
    *(uint8_t **)(g_sel + T_SEL_MAP) = g_map;
    *i32(g_sel, T_SEL_HOVER) = -1;
    *(int64_t *)(g_sel + T_SEL_WIRE) = -1;
    *(int **)(g_sel + T_SEL_IDS) = g_sel_ids;
    *i32(g_sel, T_SEL_COUNT) = 0;
    *i32(g_sel, T_SEL_CAPACITY) = T_SEL_CAP;
    *(uint8_t ***)(g_map + T_MAP_ENTS) = g_ent_array;
    *(uint32_t *)(g_map + T_MAP_ENTCOUNT) = T_ENTITIES;
    g_doors[0] = 14; g_doors[1] = 15; g_frames[0] = -1; g_frames[1] = 13;
    *(int **)(g_map + T_MAP_DOORS) = g_doors;  *i32(g_map, T_MAP_DOORS + 8) = 2;
    *(int **)(g_map + T_MAP_FRAMES) = g_frames; *i32(g_map, T_MAP_FRAMES + 8) = 2;
    *(uint32_t *)(g_screen + T_SCREEN_UIBITS) = 0x420;
    g_null_reads = 0; g_on_main = 1; g_manipulating = 0; g_blocked_mask = 0; g_override = 0;
    g_force_referenced = 0; g_fault_caps_after = 0; g_fault_delete = 0; g_action_skips_id = -1;
    g_nlog = 0; g_caps_calls = 0; g_commits = 0; g_ndestroyed = 0;
    g_hover_at_delete = -2; g_count_at_delete = -1; g_cur_at_delete = -1;
}

/* ------------------------------------------------------- native doubles -- */

static uint32_t decl_flags(int id)
{
    uint8_t *decl = *(uint8_t **)(g_ent_array[id] + T_SLOT_DECL);
    if (!decl) { g_null_reads++; return 0; }
    return *(uint32_t *)(decl + T_DECL_FLAGS);
}

/* The native all-selected predicate: declaration flag 8, then restriction bit 4
 * unless the global override is set. */
static int deletable(int id)
{
    if (!(decl_flags(id) & 8)) return 0;
    if ((*(uint32_t *)(g_ent_array[id] + T_SLOT_RESTRICT) & 4) && !g_override) return 0;
    return 1;
}

static int sel_has(int id) { for (int i = 0; i < sel_count(); i++) if (g_sel_ids[i] == id) return 1; return 0; }

static void fake_clear_selection(void *sel)
{
    CHECK(sel == g_sel);
    logc(C_CLEAR_SEL);
    *i32(g_sel, T_SEL_COUNT) = 0;
}

static unsigned char model_add(int id)
{
    if (sel_has(id)) return 0;
    if (sel_count() >= T_SEL_CAP) return 0;
    g_sel_ids[sel_count()] = id;
    *i32(g_sel, T_SEL_COUNT) = sel_count() + 1;
    return 1;
}

static unsigned char fake_add_to_selection(void *sel, uint32_t id)
{
    CHECK(sel == g_sel);
    logc(C_ADD_SEL);
    return model_add((int)id);
}

static void model_remove(int id)
{
    int n = sel_count();
    for (int i = 0; i < n; i++)
        if (g_sel_ids[i] == id) {
            for (int j = i + 1; j < n; j++) g_sel_ids[j - 1] = g_sel_ids[j];
            *i32(g_sel, T_SEL_COUNT) = n - 1;
            return;
        }
}

static void model_clear_hover(void)
{
    *i32(g_sel, T_SEL_HOVER) = -1;
    *(uint32_t *)(g_screen + T_SCREEN_UIBITS) &= ~0x400u;
}

static void fake_clear_hover(void *editor)
{
    CHECK(editor == g_ed);
    logc(C_CLEAR_HOVER);
    model_clear_hover();
}

/* Selection capability builder: temporarily adds the hovered entity, reads the
 * hovered declaration, then evaluates the predicate over the selection. */
static void fake_caps(void *substate, void *m, void *editor)
{
    CHECK(substate == selsub() && m == mode() && editor == g_ed);
    logc(C_CAPS);
    g_caps_calls++;
    if (g_fault_caps_after && g_caps_calls == g_fault_caps_after)
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    uint8_t caps = (uint8_t)(*(uint8_t *)(selsub() + T_SELSUB_CAPS) & 0x16);
    int h = hover(), added = 0;
    if (h != -1) { added = model_add(h); (void)decl_flags(h); }
    int all = 1;
    for (int i = 0; i < sel_count(); i++) if (!deletable(g_sel_ids[i])) all = 0;
    caps = (uint8_t)((caps & 0xED) | (all ? 2 : 0));
    if (g_force_referenced) caps |= 0x10;
    if (added) model_remove(h);
    *(uint8_t *)(selsub() + T_SELSUB_CAPS) = caps;
}

static unsigned char fake_blocked(void *restrictions, uint32_t mask)
{
    CHECK(restrictions == g_ed + T_ED_RESTRICT);
    logc(C_BLOCKED);
    return (g_blocked_mask & (int)mask) ? 1 : 0;
}

static void release(int id)
{
    g_ent_array[id] = g_sentinel;
    g_destroyed[g_ndestroyed++] = id;
}

/* Delete Selection: refuse without capability bit 2, show the referenced
 * dialog for bit 0x10, add a hovered entity and clear hover, destroy every
 * selected entity, clear the count, arm the commit phase. */
static void fake_delete(void *substate, void *m, void *editor)
{
    CHECK(substate == selsub() && m == mode() && editor == g_ed);
    logc(C_DELETE);
    g_hover_at_delete = hover(); g_count_at_delete = sel_count(); g_cur_at_delete = cur();
    if (g_fault_delete) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    uint8_t caps = *(uint8_t *)(selsub() + T_SELSUB_CAPS);
    if (!(caps & 2)) return;
    if (caps & 0x10) { logc(C_DIALOG); return; }
    if (hover() != -1) { model_add(hover()); logc(C_ACTION_CLEAR_HOVER); model_clear_hover(); }
    for (int i = 0; i < sel_count(); i++)
        if (g_sel_ids[i] != g_action_skips_id) release(g_sel_ids[i]);
    *i32(g_sel, T_SEL_COUNT) = 0;
    *i32(selsub(), T_SELSUB_COUNTDOWN) = 0x1E;
    *i32(selsub(), T_SELSUB_COMMIT) = 1;
    *i32(selsub(), T_SUB_PHASE) = 1;
    fake_caps(selsub(), mode(), g_ed);
    *(uint8_t *)(mode() + T_MODE_DIRTY) = 1;
}

static int fake_on_main(void) { return g_on_main; }
static int fake_manipulation(void) { return g_manipulating; }

/* ------------------------------------------------------- native frames -- */

/* Idle capability builder (Vulkan 0xce3180 / OpenGL 0xce2b60): hovered
 * entity's declaration, or both ends of a hovered connection. */
static void model_empty_caps(void)
{
    int h = hover();
    if (h != -1) { (void)decl_flags(h); return; }
    int64_t wire = *(int64_t *)(g_sel + T_SEL_WIRE);
    int a = (int)(wire & 0xffffffff), b = (int)(wire >> 32);
    if (a != -1 && b != -1) { (void)decl_flags(a); (void)decl_flags(b); }
}

static void model_set_idle(void)
{
    logc(C_SET_IDLE);
    *i32(mode(), T_MODE_CUR) = T_IDLE;
    *(uint8_t *)(mode() + T_MODE_DIRTY) = 1;
}

/* One EntityMode update: Exit/Enter on a substate change, then the current
 * substate's update, then clear the queued action. */
static void model_frame(void)
{
    int c = cur(), p = *i32(mode(), T_MODE_PREV);
    if (p != c) {
        if (p == T_IDLE) logc(C_EXIT_EMPTY);
        if (p == T_SELECTED) logc(C_EXIT_SELECTION);
        if (c == T_IDLE) {
            logc(C_ENTER_EMPTY);
            *i32(g_sel, T_SEL_COUNT) = 0;                /* Empty Enter clears the selection */
            *i32(empty(), T_SUB_PHASE) = 1;
            model_empty_caps();
        }
        if (c == T_SELECTED) { logc(C_ENTER_SELECTION); fake_caps(selsub(), mode(), g_ed); }
        *i32(mode(), T_MODE_PREV) = c;
    }
    if (c == T_SELECTED) {
        int phase = *i32(selsub(), T_SUB_PHASE);
        if (phase == 1) {
            if (*i32(selsub(), T_SELSUB_COMMIT) == 1) { logc(C_COMMIT); g_commits++; model_set_idle(); }
            *i32(selsub(), T_SELSUB_COMMIT) = 0;
            *i32(selsub(), T_SUB_PHASE) = 0;
            fake_caps(selsub(), mode(), g_ed);
        } else if (phase == 0 && sel_count() < 1) {
            model_set_idle();
        }
    }
    *i32(mode(), T_MODE_QUEUED) = -1;
}

/* The page follows a window delete with an empty selection. Integrated
 * product writes honor the native-pending guard; this models that contract. */
static void product_clear_selection(int honor_guard)
{
    if (honor_guard && sh_editor_delete_native_pending()) return;
    *i32(g_sel, T_SEL_COUNT) = 0;
    if (cur() == T_IDLE || cur() == T_SELECTED) *i32(mode(), T_MODE_CUR) = T_IDLE;
}

/* Today's window path: the single-entity leaf for each ID, the idle write when
 * the selection is empty, then the page's clear. */
static void legacy_window_delete(const int *ids, int n)
{
    for (int i = 0; i < n; i++) if (alive(ids[i])) release(ids[i]);
    if (sel_count() <= 0 && cur() == T_SELECTED) *i32(mode(), T_MODE_CUR) = T_IDLE;
    product_clear_selection(0);
}

static void bind_model(void)
{
    sh_editor_delete_engine e;
    memset(&e, 0, sizeof e);
    e.editor = g_ed;
    e.clear_selection = fake_clear_selection;
    e.add_to_selection = fake_add_to_selection;
    e.clear_hover = fake_clear_hover;
    e.refresh_capabilities = fake_caps;
    e.blocked = fake_blocked;
    e.delete_selection = fake_delete;
    e.on_main_thread = fake_on_main;
    e.manipulation_in_progress = fake_manipulation;
    CHECK(sh_editor_delete_bind(&e) == 1);
}

static void select_natively(const int *ids, int n, int h)
{
    *i32(g_sel, T_SEL_COUNT) = 0;
    for (int i = 0; i < n; i++) model_add(ids[i]);
    *i32(g_sel, T_SEL_HOVER) = h;
    *i32(mode(), T_MODE_CUR) = n ? T_SELECTED : T_IDLE;
    *i32(mode(), T_MODE_PREV) = *i32(mode(), T_MODE_CUR);
}

static int run(const int *ids, int n, sh_editor_delete_result *r)
{
    memset(r, 0x5A, sizeof *r);
    g_nlog = 0;
    return sh_editor_delete_batch(ids, n, r);
}

/* ----------------------------------------------------------------- tests -- */

/* The model reproduces issue #111's consumer for today's window path: the
 * page's clear moves Selection to idle, and Empty Enter reads the deleted
 * hovered entity. This is the failure the adapter must remove. */
static void legacy_leaf_loop_reaches_stale_hover_consumer(void)
{
    world_reset();
    int ids[] = { 5 };
    select_natively(ids, 1, 5);
    legacy_window_delete(ids, 1);
    CHECK(hover() == 5 && !alive(5));
    model_frame();
    CHECK(g_null_reads == 1);
    CHECK(g_commits == 0);
}

static void dispatch_releases_targets_and_leaves_commit_to_native_frames(void)
{
    world_reset(); bind_model();
    int ids[] = { 3, 4, 5 };
    sh_editor_delete_result r;
    CHECK(run(ids, 3, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(r.status == SH_EDITOR_DELETE_DISPATCHED && r.requested == 3 && r.targets == 3);
    CHECK(r.released == 3 && r.rolled_back == 0 && r.first_id == -1);
    CHECK(!alive(3) && !alive(4) && !alive(5) && alive(2) && alive(6));
    CHECK(sel_count() == 0 && hover() == -1);
    CHECK(*i32(selsub(), T_SUB_PHASE) == 1 && *i32(selsub(), T_SELSUB_COMMIT) == 1);
    CHECK(cur() == T_SELECTED);
    CHECK(g_commits == 0);                         /* the commit belongs to the native frame */
    CHECK(sh_editor_delete_native_pending() == 1);
    model_frame();
    CHECK(g_commits == 1 && cur() == T_IDLE);
    model_frame();
    CHECK(log_count(C_ENTER_EMPTY) == 1);
    CHECK(g_null_reads == 0);
    CHECK(sh_editor_delete_native_pending() == 0);
}

/* A hovered non-target must leave hover before capabilities are computed and
 * before the action, or the native action deletes it too. */
static void non_target_hover_is_cleared_before_capabilities_and_action(void)
{
    world_reset(); bind_model();
    *i32(g_sel, T_SEL_HOVER) = 9;
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(r.hover_cleared == 1);
    CHECK(alive(9) && !alive(3));
    int is = log_index(C_CLEAR_SEL), ia = log_index(C_ADD_SEL), ih = log_index(C_CLEAR_HOVER);
    int ic = log_index(C_CAPS), id = log_index(C_DELETE);
    CHECK(is >= 0 && ia > is && ic > ia && id > ic);
    CHECK(ih >= 0 && ih < ic);
    CHECK(g_hover_at_delete == -1 && g_count_at_delete == 1 && g_cur_at_delete == T_SELECTED);
    CHECK(log_count(C_ACTION_CLEAR_HOVER) == 0);
}

/* A protected hovered non-target must not veto the window request. */
static void protected_non_target_hover_does_not_refuse(void)
{
    world_reset(); bind_model();
    *(uint32_t *)(g_decls[9] + T_DECL_FLAGS) = 0;
    *i32(g_sel, T_SEL_HOVER) = 9;
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(alive(9) && !alive(3));
}

/* When the hovered entity is a target the native action clears hover itself. */
static void hovered_target_is_cleared_by_the_native_action(void)
{
    world_reset(); bind_model();
    *i32(g_sel, T_SEL_HOVER) = 4;
    int ids[] = { 3, 4 };
    sh_editor_delete_result r;
    CHECK(run(ids, 2, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(r.hover_cleared == 0);
    CHECK(log_count(C_CLEAR_HOVER) == 0 && log_count(C_ACTION_CLEAR_HOVER) == 1);
    CHECK(hover() == -1 && !alive(3) && !alive(4));
}

/* Issue #111's sequence through the adapter: a window selection, a window
 * delete with the cursor on the target, then the page's clear. */
static void window_select_delete_clear_has_no_stale_reads(void)
{
    world_reset(); bind_model();
    int ids[] = { 6 };
    select_natively(ids, 1, 6);
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    product_clear_selection(1);
    model_frame();
    model_frame();
    CHECK(g_null_reads == 0);
    CHECK(g_commits == 1);
    CHECK(cur() == T_IDLE && hover() == -1 && sel_count() == 0);
}

/* Without the integration guard the page's clear strands the pending commit:
 * Selection never runs its commit phase. This documents the requirement. */
static void unguarded_product_clear_strands_the_native_commit(void)
{
    world_reset(); bind_model();
    int ids[] = { 6 };
    select_natively(ids, 1, 6);
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    product_clear_selection(0);
    model_frame();
    model_frame();
    CHECK(g_commits == 0);
    CHECK(*i32(selsub(), T_SUB_PHASE) == 1);
    CHECK(sh_editor_delete_native_pending() == 1);
}

static void protected_target_refuses_and_restores_selection_and_substate(void)
{
    world_reset(); bind_model();
    int prior[] = { 8, 2 };
    select_natively(prior, 2, -1);
    *(uint32_t *)(g_slots[4] + T_SLOT_RESTRICT) = 4;      /* protected, no override */
    int ids[] = { 3, 4 };
    sh_editor_delete_result r;
    CHECK(run(ids, 2, &r) == SH_EDITOR_DELETE_PROTECTED);
    CHECK(r.rolled_back == 1 && r.released == 0);
    CHECK(log_count(C_DELETE) == 0);
    CHECK(alive(3) && alive(4));
    CHECK(sel_count() == 2 && g_sel_ids[0] == 8 && g_sel_ids[1] == 2);
    CHECK(cur() == T_SELECTED && *i32(mode(), T_MODE_PREV) == T_SELECTED);
    CHECK(*i32(selsub(), T_SUB_PHASE) == 0);
    /* Capabilities were rebuilt for the restored selection, not left for the targets. */
    CHECK(log_count(C_CAPS) == 2);
    CHECK((*(uint8_t *)(selsub() + T_SELSUB_CAPS) & 2) != 0);
    CHECK(g_null_reads == 0);
}

static void protected_target_from_idle_restores_idle(void)
{
    world_reset(); bind_model();
    *(uint32_t *)(g_decls[4] + T_DECL_FLAGS) = 0;          /* not a deletable declaration */
    int ids[] = { 4 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_PROTECTED);
    CHECK(r.rolled_back == 1 && alive(4));
    CHECK(cur() == T_IDLE && *i32(mode(), T_MODE_PREV) == T_IDLE && sel_count() == 0);
    model_frame();
    CHECK(log_count(C_ENTER_SELECTION) == 0 && g_commits == 0);
}

static void global_override_lets_native_capabilities_accept_protected_entity(void)
{
    world_reset(); bind_model();
    *(uint32_t *)(g_slots[4] + T_SLOT_RESTRICT) = 4;
    g_override = 1;
    int ids[] = { 4 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(!alive(4));
}

static void restriction_refuses_before_any_engine_mutation(void)
{
    world_reset(); bind_model();
    g_blocked_mask = 4;
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_RESTRICTED);
    CHECK(log_count(C_BLOCKED) == 1 && g_nlog == 1);
    CHECK(alive(3) && cur() == T_IDLE && r.rolled_back == 0);
}

static void other_restriction_bits_do_not_refuse(void)
{
    world_reset(); bind_model();
    g_blocked_mask = 0x800 | 0x100 | 2;
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
}

static void expect_busy(int busy)
{
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_BUSY);
    CHECK(r.busy == busy);
    CHECK(g_nlog == 0);
    CHECK(alive(3));
}

static void native_owners_refuse_before_any_engine_call(void)
{
    world_reset(); bind_model();
    *i32(selsub(), T_SUB_PHASE) = 1;                       /* a delete still waiting to commit */
    expect_busy(SH_EDITOR_DELETE_BUSY_PHASE);

    world_reset(); bind_model();
    *i32(empty(), T_SUB_PHASE) = 3;                         /* idle hover-delete commit pending */
    expect_busy(SH_EDITOR_DELETE_BUSY_PHASE);

    world_reset(); bind_model();
    *i32(mode(), T_MODE_CUR) = 3; *i32(mode(), T_MODE_PREV) = 3;   /* a manipulation substate */
    expect_busy(SH_EDITOR_DELETE_BUSY_SUBSTATE);

    world_reset(); bind_model();
    *i32(mode(), T_MODE_PREV) = 3;                          /* leaving manipulation not yet exited */
    expect_busy(SH_EDITOR_DELETE_BUSY_TRANSITION);

    world_reset(); bind_model();
    *i32(mode(), T_MODE_QUEUED) = 0x5D;
    expect_busy(SH_EDITOR_DELETE_BUSY_QUEUED);

    world_reset(); bind_model();
    g_manipulating = 1;
    expect_busy(SH_EDITOR_DELETE_BUSY_MANIPULATION);
}

static void expect_status(int status)
{
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == status);
    CHECK(g_nlog == 0 && alive(3));
}

static void editor_readiness_refuses_before_any_engine_call(void)
{
    world_reset(); bind_model(); *(uint8_t **)(g_ed + T_ED_MAP) = NULL;    expect_status(SH_EDITOR_DELETE_NOT_READY);
    world_reset(); bind_model(); *(uint8_t **)(g_ed + T_ED_SEL) = NULL;    expect_status(SH_EDITOR_DELETE_NOT_READY);
    world_reset(); bind_model(); *(uint8_t **)(g_ed + T_ED_SCREEN) = NULL; expect_status(SH_EDITOR_DELETE_NOT_READY);
    world_reset(); bind_model(); *i32(g_ed, T_ED_TRANSITION) = 5;          expect_status(SH_EDITOR_DELETE_NOT_READY);
    world_reset(); bind_model(); g_ed[T_ED_DEACT] = 1;                     expect_status(SH_EDITOR_DELETE_NOT_READY);
    world_reset(); bind_model(); *i32(g_ed, T_ED_STATE) = 1;               expect_status(SH_EDITOR_DELETE_NOT_ENTITY);
    world_reset(); bind_model(); g_on_main = 0;                            expect_status(SH_EDITOR_DELETE_NOT_MAIN);
    world_reset(); bind_model(); g_on_main = -1;                           expect_status(SH_EDITOR_DELETE_NOT_MAIN);
    world_reset(); sh_editor_delete_bind(NULL);                            expect_status(SH_EDITOR_DELETE_UNAVAILABLE);
}

static void invalid_targets_refuse_before_any_engine_call(void)
{
    sh_editor_delete_result r;
    world_reset(); bind_model();
    CHECK(run(NULL, 1, &r) == SH_EDITOR_DELETE_INVALID && g_nlog == 0);
    int one[] = { 3 };
    CHECK(run(one, 0, &r) == SH_EDITOR_DELETE_INVALID && g_nlog == 0);
    CHECK(run(one, SH_EDITOR_DELETE_MAX + 1, &r) == SH_EDITOR_DELETE_INVALID && g_nlog == 0);
    int range[] = { 3, T_ENTITIES };
    CHECK(run(range, 2, &r) == SH_EDITOR_DELETE_INVALID && r.first_id == T_ENTITIES && g_nlog == 0);
    int neg[] = { -1 };
    CHECK(run(neg, 1, &r) == SH_EDITOR_DELETE_INVALID && r.first_id == -1 && g_nlog == 0);
    g_ent_array[7] = g_sentinel;                           /* already released */
    int dead[] = { 3, 7 };
    CHECK(run(dead, 2, &r) == SH_EDITOR_DELETE_INVALID && r.first_id == 7 && g_nlog == 0);
    *(uint8_t **)(g_slots[8] + T_SLOT_DECL) = NULL;         /* live slot without a declaration */
    int nodecl[] = { 8 };
    CHECK(run(nodecl, 1, &r) == SH_EDITOR_DELETE_INVALID && r.first_id == 8 && g_nlog == 0);
    CHECK(alive(3));
}

static void duplicate_ids_are_deleted_once(void)
{
    world_reset(); bind_model();
    int ids[] = { 7, 7, 8, 7 };
    sh_editor_delete_result r;
    CHECK(run(ids, 4, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(r.requested == 4 && r.targets == 2 && r.released == 2);
    CHECK(g_ndestroyed == 2 && g_count_at_delete == 2);
}

static void portal_entities_refuse_before_any_engine_call(void)
{
    sh_editor_delete_result r;
    world_reset(); bind_model();
    int door[] = { 3, 15 };
    CHECK(run(door, 2, &r) == SH_EDITOR_DELETE_PORTAL && r.first_id == 15 && g_nlog == 0);
    int frame[] = { 13 };
    CHECK(run(frame, 1, &r) == SH_EDITOR_DELETE_PORTAL && r.first_id == 13 && g_nlog == 0);
    CHECK(alive(3) && alive(13) && alive(15));
}

static void connection_hover_touching_a_target_is_cleared(void)
{
    world_reset(); bind_model();
    *(int64_t *)(g_sel + T_SEL_WIRE) = ((int64_t)9 << 32) | 3;   /* wire 3 -> 9 hovered */
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(r.wire_cleared == 1);
    CHECK(*(int64_t *)(g_sel + T_SEL_WIRE) == -1);
    model_frame(); model_frame();
    CHECK(g_null_reads == 0 && g_commits == 1);
}

static void unrelated_connection_hover_is_kept(void)
{
    world_reset(); bind_model();
    int64_t wire = ((int64_t)9 << 32) | 10;
    *(int64_t *)(g_sel + T_SEL_WIRE) = wire;
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(r.wire_cleared == 0 && *(int64_t *)(g_sel + T_SEL_WIRE) == wire);
}

static void prior_non_target_selection_is_counted_and_kept_alive(void)
{
    world_reset(); bind_model();
    int prior[] = { 2, 3 };
    select_natively(prior, 2, -1);
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(r.deselected == 1 && alive(2) && !alive(3));
    CHECK(g_count_at_delete == 1);
}

/* A selection left holding a released ID (today's leaf path can do this) is
 * never handed back to the capability builder, which would read its missing
 * declaration. */
static void released_prior_selection_is_not_restored(void)
{
    world_reset(); bind_model();
    int prior[] = { 2, 7 };
    select_natively(prior, 2, -1);
    g_ent_array[7] = g_sentinel;
    *(uint32_t *)(g_decls[4] + T_DECL_FLAGS) = 0;
    int bad[] = { 4 };
    sh_editor_delete_result r;
    CHECK(run(bad, 1, &r) == SH_EDITOR_DELETE_PROTECTED);
    CHECK(r.rolled_back == 1 && sel_count() == 1 && g_sel_ids[0] == 2);
    CHECK(g_null_reads == 0);

    world_reset(); bind_model();
    select_natively(prior, 2, -1);
    g_ent_array[7] = g_sentinel;
    int ids[] = { 3 };
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(r.deselected == 2 && g_null_reads == 0);
}

/* A product idle request that the native update has not applied yet is
 * replaced; a pending entry into Selection is kept. Both reach the commit. */
static void pending_selection_or_idle_request_still_commits(void)
{
    world_reset(); bind_model();
    int prior[] = { 2 };
    select_natively(prior, 1, -1);
    *i32(mode(), T_MODE_CUR) = T_IDLE;                       /* prev stays Selection */
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    model_frame(); model_frame();
    CHECK(g_commits == 1 && cur() == T_IDLE && g_null_reads == 0);
    CHECK(log_count(C_ENTER_SELECTION) == 0);

    world_reset(); bind_model();
    *i32(mode(), T_MODE_CUR) = T_SELECTED;                   /* prev stays idle */
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_DISPATCHED);
    model_frame(); model_frame();
    CHECK(g_commits == 1 && cur() == T_IDLE && g_null_reads == 0);
    CHECK(log_count(C_ENTER_SELECTION) == 1);
}

static void referenced_refusal_rolls_back(void)
{
    world_reset(); bind_model();
    g_force_referenced = 1;
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_REFERENCED);
    CHECK(r.rolled_back == 1 && alive(3) && log_count(C_DELETE) == 0);
    CHECK(cur() == T_IDLE && sel_count() == 0);
}

static void capability_fault_rolls_back(void)
{
    world_reset(); bind_model();
    int prior[] = { 2 };
    select_natively(prior, 1, -1);
    g_fault_caps_after = 1;
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_FAULT);
    CHECK(r.rolled_back == 1 && alive(3) && log_count(C_DELETE) == 0);
    CHECK(sel_count() == 1 && g_sel_ids[0] == 2 && cur() == T_SELECTED);
}

static void action_fault_reports_without_rollback(void)
{
    world_reset(); bind_model();
    g_fault_delete = 1;
    int ids[] = { 3 };
    sh_editor_delete_result r;
    CHECK(run(ids, 1, &r) == SH_EDITOR_DELETE_FAULT);
    CHECK(r.rolled_back == 0);
}

static void divergent_action_is_reported_incomplete(void)
{
    world_reset(); bind_model();
    g_action_skips_id = 4;
    int ids[] = { 3, 4 };
    sh_editor_delete_result r;
    CHECK(run(ids, 2, &r) == SH_EDITOR_DELETE_INCOMPLETE);
    CHECK(r.released == 1 && r.targets == 2);
}

static void null_result_pointer_is_allowed(void)
{
    world_reset(); bind_model();
    int ids[] = { 3 };
    g_nlog = 0;
    CHECK(sh_editor_delete_batch(ids, 1, NULL) == SH_EDITOR_DELETE_DISPATCHED);
    CHECK(!alive(3));
}

static void native_pending_tracks_native_owners(void)
{
    world_reset(); bind_model();
    CHECK(sh_editor_delete_native_pending() == 0);
    *i32(mode(), T_MODE_QUEUED) = 0x60;
    CHECK(sh_editor_delete_native_pending() == 1);
    world_reset();
    *i32(empty(), T_SUB_PHASE) = 4;
    CHECK(sh_editor_delete_native_pending() == 1);
    world_reset();
    *i32(selsub(), T_SUB_PHASE) = 1;
    CHECK(sh_editor_delete_native_pending() == 1);
    world_reset();
    *i32(g_ed, T_ED_STATE) = 1;                             /* not EntityMode: nothing to protect */
    *i32(selsub(), T_SUB_PHASE) = 1;
    CHECK(sh_editor_delete_native_pending() == 0);
    sh_editor_delete_bind(NULL);
    CHECK(sh_editor_delete_native_pending() == 0);
}

/* ------------------------------------------------------- decode helper -- */

static const uint8_t k_site[] = {
    0x49,0x8B,0xCD, 0xE8,0,0,0,0, 0x84,0xC0, 0x74,0x08, 0x49,0x8B,0xCD, 0xE8,0,0,0,0,
    0x4D,0x8B,0xA5,0xD0,0x04,0x02,0x00, 0x49,0x63,0x9C,0x24,0x88,0x00,0x00,0x00 };
static const uint8_t k_shape[] = {
    0x40,0x53, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xD9, 0x83,0xCA,0xFF, 0x48,0x8B,0x89,0xD0,0x04,0x02,0x00,
    0xE8,0x11,0x22,0x33,0x44, 0x48,0x8B,0x83,0x88,0x10,0x02,0x00,
    0x81,0xA0,0x28,0x0A,0x00,0x00,0xFF,0xFB,0xFF,0xFF, 0x48,0x83,0xC4,0x20, 0x5B, 0xC3 };

static uint8_t g_image[0x3000];

static void place_site(size_t action, size_t at, size_t callee)
{
    memcpy(g_image + action + at, k_site, sizeof k_site);
    int32_t rel = (int32_t)((intptr_t)callee - (intptr_t)(action + at + 20));
    memcpy(g_image + action + at + 16, &rel, 4);
}

static void decode_finds_the_action_callee(void)
{
    memset(g_image, 0xCC, sizeof g_image);
    memcpy(g_image + 0x2000, k_shape, sizeof k_shape);
    place_site(0x100, 0xA0, 0x2000);
    CHECK(sh_editor_delete_decode_clear_hover(g_image + 0x100, g_image, sizeof g_image) ==
          (uintptr_t)(g_image + 0x2000));
}

static void decode_rejects_wrong_shape_extra_site_and_out_of_image(void)
{
    memset(g_image, 0xCC, sizeof g_image);
    memcpy(g_image + 0x2000, k_shape, sizeof k_shape);
    g_image[0x2000 + 38] = 0xF7;                            /* clears a different UI bit */
    place_site(0x100, 0xA0, 0x2000);
    CHECK(sh_editor_delete_decode_clear_hover(g_image + 0x100, g_image, sizeof g_image) == 0);

    memset(g_image, 0xCC, sizeof g_image);
    memcpy(g_image + 0x2000, k_shape, sizeof k_shape);
    place_site(0x100, 0xA0, 0x2000);
    place_site(0x100, 0x140, 0x2000);
    CHECK(sh_editor_delete_decode_clear_hover(g_image + 0x100, g_image, sizeof g_image) == 0);

    memset(g_image, 0xCC, sizeof g_image);
    place_site(0x100, 0xA0, 0x2FF0);                        /* callee runs past the image */
    CHECK(sh_editor_delete_decode_clear_hover(g_image + 0x100, g_image, sizeof g_image) == 0);

    memset(g_image, 0xCC, sizeof g_image);
    CHECK(sh_editor_delete_decode_clear_hover(g_image + 0x100, g_image, sizeof g_image) == 0);
    CHECK(sh_editor_delete_decode_clear_hover(NULL, g_image, sizeof g_image) == 0);
}

int main(void)
{
    legacy_leaf_loop_reaches_stale_hover_consumer();
    dispatch_releases_targets_and_leaves_commit_to_native_frames();
    non_target_hover_is_cleared_before_capabilities_and_action();
    protected_non_target_hover_does_not_refuse();
    hovered_target_is_cleared_by_the_native_action();
    window_select_delete_clear_has_no_stale_reads();
    unguarded_product_clear_strands_the_native_commit();
    protected_target_refuses_and_restores_selection_and_substate();
    protected_target_from_idle_restores_idle();
    global_override_lets_native_capabilities_accept_protected_entity();
    restriction_refuses_before_any_engine_mutation();
    other_restriction_bits_do_not_refuse();
    native_owners_refuse_before_any_engine_call();
    editor_readiness_refuses_before_any_engine_call();
    invalid_targets_refuse_before_any_engine_call();
    duplicate_ids_are_deleted_once();
    portal_entities_refuse_before_any_engine_call();
    connection_hover_touching_a_target_is_cleared();
    unrelated_connection_hover_is_kept();
    prior_non_target_selection_is_counted_and_kept_alive();
    released_prior_selection_is_not_restored();
    pending_selection_or_idle_request_still_commits();
    referenced_refusal_rolls_back();
    capability_fault_rolls_back();
    action_fault_reports_without_rollback();
    divergent_action_is_reported_incomplete();
    null_result_pointer_is_allowed();
    native_pending_tracks_native_owners();
    decode_finds_the_action_callee();
    decode_rejects_wrong_shape_extra_site_and_out_of_image();
    if (failed) { fprintf(stderr, "editor_delete_test: %d check(s) failed\n", failed); return 1; }
    printf("editor_delete_test: all checks passed\n");
    return 0;
}
