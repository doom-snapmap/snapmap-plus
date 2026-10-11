/* editor_delete.c -- see editor_delete.h.
 *
 * Native boundaries, located independently in each executable (no address is
 * derived from the other renderer):
 *
 *   Delete Selection action     Vulkan 0xCE3B60  OpenGL 0xCE3540
 *   Selection capabilities      Vulkan 0xCE5110  OpenGL 0xCE4AF0
 *   hover clear (decoded below) Vulkan 0x5224E0  OpenGL 0x521E20
 *   GridPropertiesBlocked       Vulkan 0x5AF830  OpenGL 0x5AEED0
 *
 * The Selection substate's tick (Vulkan 0xCE3F30, OpenGL 0xCE3910) dispatches
 * abstract action 0x5D, from the keyboard or the queue at EntityMode+0x420,
 * only while GridPropertiesBlocked(editor+0x20990, 4) is false, and only in
 * the substate's tick phase with a nonempty selection. Its first steps re-pick
 * the hovered entity, and the action adds that entity to the selection before
 * destroying it. A window request must not delete whatever the camera points
 * at, so this module does not queue 0x5D. It applies the same gates, makes the
 * selection exactly the targets, clears a non-target hover through the native
 * helper, rebuilds the native capability bits and calls the action itself.
 *
 * The action destroys the selection, clears the selected count, arms the
 * Selection substate's commit phase and rebuilds capabilities. The following
 * EntityMode updates finish the deletion natively: they wait for the wire
 * animation, commit the undo step "Delete Selection" and return to idle.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "editor_delete.h"
#ifndef SH_EDITOR_DELETE_TESTING
#include <stdio.h>
#include "engine_globals.h"
#include "backend_log.h"
#endif

/* Editor object. Field layouts are identical in both executables. */
#define ED_MAP_OFF           0x204C8  /* loaded map; null outside an editor session */
#define ED_SEL_OFF           0x204D0  /* selection object */
#define ED_RESTRICT_OFF      0x20990  /* restriction set read by GridPropertiesBlocked */
#define ED_SCREEN_OFF        0x21088  /* editor screen; null while the editor is hidden */
#define ED_DEACT_OFF         0x2120D  /* byte: a deactivate is deferred to the frame loop */
#define ED_TRANSITION_OFF    0x224DC  /* int: 5 while the editor changes state */
#define ED_MODE_OFF          0x22330  /* inline EntityMode */
#define ED_STATE_OFF         0x23618  /* int: editor state, 2 = EntityMode */

/* EntityMode. Its update runs Exit(prev) and Enter(cur) when the two differ,
 * then the current substate's update, then clears the queued action. The
 * substates are inline; GetSubstate's jump table gives idle at +0x1B0 and
 * Selection at +0x1F8 in both executables. */
#define MODE_PREV_OFF        0x1A8
#define MODE_CUR_OFF         0x1AC
#define MODE_IDLE_OFF        0x1B0
#define MODE_SELECTION_OFF   0x1F8
#define MODE_QUEUED_OFF      0x420    /* queued abstract action, -1 when empty */
#define MODE_DIRTY_OFF       0xBB8    /* byte: rebuild help on the next update */
#define SUB_PHASE_OFF        0x08     /* idle: 1 tick, others pending; Selection: 0 tick, 1 commit wait */
#define SELECTION_CAPS_OFF   0x58

#define SEL_HOVER_OFF        0x2C     /* hovered entity, -1 for none */
#define SEL_WIRE_OFF         0x48     /* hovered connection {from, to}, -1 for none */
#define SEL_IDS_OFF          0x80
#define SEL_COUNT_OFF        0x88

#define MAP_ENTS_OFF         0x6A0    /* entity slot pointers */
#define MAP_ENTCOUNT_OFF     0x6A8
#define MAP_DOORS_OFF        0x778    /* portal doors {int *ids, int count} */
#define MAP_FRAMES_OFF       0x790    /* portal frames {int *ids, int count} */
#define SLOT_DECL_OFF        0x08     /* a released slot is a shared object with no declaration */

#define EDITOR_ENTITY_MODE   2
#define EDITOR_TRANSITIONING 5
#define SUBSTATE_IDLE        1
#define SUBSTATE_SELECTION   2
#define IDLE_PHASE_TICK      1
#define SELECTION_PHASE_TICK 0
#define CAP_DELETABLE        0x02     /* every selected entity passes the native delete predicate */
#define CAP_REFERENCED       0x10     /* the action would show the referenced-entity dialog */
#define RESTRICT_DELETE      4u

#define ENTITY_LIMIT         1000000u /* stale-map guard on the entity count */
#define LIST_LIMIT           65536    /* stale-object guard on selection and portal lists */

/* The hover-clear call inside Delete Selection, at +0xA0 in both executables:
 * mov rcx,r13; call AddHoveredToSelection; test al,al; je +8; mov rcx,r13;
 * call ClearHover; mov r12,[r13+0x204D0]; movsxd rbx,[r12+0x88]. */
#define DECODE_WINDOW        0x200
static const short k_clear_site[] = {
    0x49,0x8B,0xCD, 0xE8,-1,-1,-1,-1, 0x84,0xC0, 0x74,0x08, 0x49,0x8B,0xCD, 0xE8,-1,-1,-1,-1,
    0x4D,0x8B,0xA5,0xD0,0x04,0x02,0x00, 0x49,0x63,0x9C,0x24,0x88,0x00,0x00,0x00 };
#define CLEAR_SITE_CALL      15       /* offset of the second E8 */
/* ClearHover: set the hovered entity to -1, then clear screen+0xA28 bit 0x400.
 * An adjacent twin differs only in its callee, which is why this shape only
 * validates a decoded target. */
static const short k_clear_shape[] = {
    0x40,0x53, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xD9, 0x83,0xCA,0xFF, 0x48,0x8B,0x89,0xD0,0x04,0x02,0x00,
    0xE8,-1,-1,-1,-1, 0x48,0x8B,0x83,0x88,0x10,0x02,0x00,
    0x81,0xA0,0x28,0x0A,0x00,0x00,0xFF,0xFB,0xFF,0xFF, 0x48,0x83,0xC4,0x20, 0x5B, 0xC3 };

static sh_editor_delete_engine g_engine;
static int g_bound;

/* Guarded engine memory access. */
static int rd_ptr(const void *p, void **out)
{
    __try { *out = *(void *const *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rd_s32(const void *p, int *out)
{
    __try { *out = *(const int *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rd_u8(const void *p, uint8_t *out)
{
    __try { *out = *(const uint8_t *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rd_s64(const void *p, int64_t *out)
{
    __try { *out = *(const int64_t *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int wr_s32(void *p, int v)
{
    __try { *(int *)p = v; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int wr_u8(void *p, uint8_t v)
{
    __try { *(uint8_t *)p = v; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int wr_s64(void *p, int64_t v)
{
    __try { *(int64_t *)p = v; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Guarded native calls; 0 means the call raised. */
static int call_clear_selection(void *sel)
{
    __try { g_engine.clear_selection(sel); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int call_add(void *sel, int id)
{
    __try { (void)g_engine.add_to_selection(sel, (uint32_t)id); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int call_clear_hover(void *ed)
{
    __try { g_engine.clear_hover(ed); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int call_caps(void *sub, void *mode, void *ed)
{
    __try { g_engine.refresh_capabilities(sub, mode, ed); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int call_blocked(void *restrictions, uint32_t mask, int *blocked)
{
    __try { *blocked = g_engine.blocked(restrictions, mask) != 0; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int call_delete(void *sub, void *mode, void *ed)
{
    __try { g_engine.delete_selection(sub, mode, ed); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int sh_editor_delete_bind(const sh_editor_delete_engine *engine)
{
    memset(&g_engine, 0, sizeof g_engine);
    g_bound = 0;
    if (!engine || !engine->editor || !engine->clear_selection || !engine->add_to_selection ||
        !engine->clear_hover || !engine->refresh_capabilities || !engine->blocked ||
        !engine->delete_selection)
        return 0;
    g_engine = *engine;
    g_bound = 1;
    return 1;
}

/* A slot holds a live entity when it and its declaration pointer are nonnull. */
static int entity_alive(uint8_t *ents, uint32_t count, int id)
{
    void *slot = NULL, *decl = NULL;
    if (id < 0 || (uint32_t)id >= count) return 0;
    if (!rd_ptr(ents + (size_t)id * 8, &slot) || !slot) return 0;
    if (!rd_ptr((uint8_t *)slot + SLOT_DECL_OFF, &decl)) return 0;
    return decl != NULL;
}

static int in_bits(const uint8_t *bits, uint32_t count, int id)
{
    return id >= 0 && (uint32_t)id < count && (bits[(uint32_t)id >> 3] & (1u << (id & 7))) != 0;
}

/* Return 1 when id appears in a portal list {int *ids, int count}. Unreadable
 * lists count as owning, which refuses rather than risks a dangling door. */
static int portal_owned(uint8_t *map, size_t list_off, int id)
{
    void *arr = NULL;
    int n = 0;
    if (!rd_ptr(map + list_off, &arr) || !rd_s32(map + list_off + 8, &n)) return 1;
    if (n < 0 || n > LIST_LIMIT) return 1;
    if (n > 0 && !arr) return 1;
    for (int i = 0; i < n; i++) {
        int v = 0;
        if (!rd_s32((uint8_t *)arr + (size_t)i * 4, &v)) return 1;
        if (v == id) return 1;
    }
    return 0;
}

/* Everything the dispatch decided, so a refusal can restore it. */
typedef struct ed_ctx {
    uint8_t *ed, *map, *sel, *mode, *selection_sub;
    uint8_t *ents;
    uint32_t ent_count;
    int cur;
    int *prior;
    int prior_count;
    int changed_substate;
    int64_t wire;
    int wire_cleared;
} ed_ctx;

/* Restore the selection (live entries only, in their order), the requested
 * substate and the hovered connection, then rebuild the capability bits. A
 * cleared hover stays cleared; the next native pick recomputes it. */
static int rollback(ed_ctx *c)
{
    int ok = call_clear_selection(c->sel);
    for (int i = 0; ok && i < c->prior_count; i++)
        if (entity_alive(c->ents, c->ent_count, c->prior[i]) && !call_add(c->sel, c->prior[i])) ok = 0;
    if (c->changed_substate && !wr_s32(c->mode + MODE_CUR_OFF, c->cur)) ok = 0;
    if (c->wire_cleared && !wr_s64(c->sel + SEL_WIRE_OFF, c->wire)) ok = 0;
    if (!call_caps(c->selection_sub, c->mode, c->ed)) ok = 0;
    return ok;
}

static int batch(const int *ids, int count, sh_editor_delete_result *r)
{
    ed_ctx c;
    memset(&c, 0, sizeof c);
    if (!g_bound) return SH_EDITOR_DELETE_UNAVAILABLE;
    if (g_engine.on_main_thread && g_engine.on_main_thread() != 1) return SH_EDITOR_DELETE_NOT_MAIN;
    if (!ids || count <= 0 || count > SH_EDITOR_DELETE_MAX) return SH_EDITOR_DELETE_INVALID;

    c.ed = g_engine.editor;
    void *map = NULL, *sel = NULL, *screen = NULL;
    uint8_t deact = 0;
    int transition = 0, state = 0;
    if (!rd_ptr(c.ed + ED_MAP_OFF, &map) || !map ||
        !rd_ptr(c.ed + ED_SEL_OFF, &sel) || !sel ||
        !rd_ptr(c.ed + ED_SCREEN_OFF, &screen) || !screen ||
        !rd_u8(c.ed + ED_DEACT_OFF, &deact) || deact ||
        !rd_s32(c.ed + ED_TRANSITION_OFF, &transition) || transition == EDITOR_TRANSITIONING ||
        !rd_s32(c.ed + ED_STATE_OFF, &state))
        return SH_EDITOR_DELETE_NOT_READY;
    if (state != EDITOR_ENTITY_MODE) return SH_EDITOR_DELETE_NOT_ENTITY;
    c.map = (uint8_t *)map;
    c.sel = (uint8_t *)sel;
    c.mode = c.ed + ED_MODE_OFF;
    c.selection_sub = c.mode + MODE_SELECTION_OFF;

    /* The native tick only dispatches from idle or Selection, in its tick
     * phase, with no other action queued for this frame. */
    int prev = 0, idle_phase = 0, selection_phase = 0, queued = 0;
    if (!rd_s32(c.mode + MODE_CUR_OFF, &c.cur) || !rd_s32(c.mode + MODE_PREV_OFF, &prev) ||
        !rd_s32(c.mode + MODE_IDLE_OFF + SUB_PHASE_OFF, &idle_phase) ||
        !rd_s32(c.selection_sub + SUB_PHASE_OFF, &selection_phase) ||
        !rd_s32(c.mode + MODE_QUEUED_OFF, &queued))
        return SH_EDITOR_DELETE_NOT_READY;
    r->busy = SH_EDITOR_DELETE_BUSY_SUBSTATE;
    if (c.cur != SUBSTATE_IDLE && c.cur != SUBSTATE_SELECTION) return SH_EDITOR_DELETE_BUSY;
    r->busy = SH_EDITOR_DELETE_BUSY_TRANSITION;
    if (prev != SUBSTATE_IDLE && prev != SUBSTATE_SELECTION) return SH_EDITOR_DELETE_BUSY;
    r->busy = SH_EDITOR_DELETE_BUSY_PHASE;
    if (selection_phase != SELECTION_PHASE_TICK) return SH_EDITOR_DELETE_BUSY;
    if ((c.cur == SUBSTATE_IDLE || prev == SUBSTATE_IDLE) && idle_phase != IDLE_PHASE_TICK)
        return SH_EDITOR_DELETE_BUSY;
    r->busy = SH_EDITOR_DELETE_BUSY_QUEUED;
    if (queued != -1) return SH_EDITOR_DELETE_BUSY;
    r->busy = SH_EDITOR_DELETE_BUSY_MANIPULATION;
    if (g_engine.manipulation_in_progress && g_engine.manipulation_in_progress())
        return SH_EDITOR_DELETE_BUSY;
    r->busy = SH_EDITOR_DELETE_BUSY_NONE;

    void *ents = NULL;
    uint32_t ent_count = 0;
    if (!rd_ptr(c.map + MAP_ENTS_OFF, &ents) || !ents ||
        !rd_s32(c.map + MAP_ENTCOUNT_OFF, (int *)&ent_count) || ent_count > ENTITY_LIMIT)
        return SH_EDITOR_DELETE_NOT_READY;
    c.ents = (uint8_t *)ents;
    c.ent_count = ent_count;

    int status = SH_EDITOR_DELETE_FAULT;
    uint8_t *bits = (uint8_t *)calloc((size_t)ent_count / 8 + 1, 1);
    int *targets = (int *)malloc(sizeof(int) * (size_t)count);
    if (!bits || !targets) goto done;
    int ntargets = 0;
    for (int i = 0; i < count; i++) {
        int id = ids[i];
        if (!entity_alive(c.ents, c.ent_count, id)) {
            r->first_id = id;
            status = SH_EDITOR_DELETE_INVALID;
            goto done;
        }
        if (in_bits(bits, c.ent_count, id)) continue;
        bits[(uint32_t)id >> 3] |= (uint8_t)(1u << (id & 7));
        targets[ntargets++] = id;
    }
    r->targets = ntargets;

    /* Slot release does not edit the portal door/frame lists, and module
     * collision dereferences every listed entity. A window request must never
     * leave such a reference behind, whatever the entity's declaration flags
     * say, so portal entities are refused before anything changes. */
    for (int i = 0; i < ntargets; i++)
        if (portal_owned(c.map, MAP_DOORS_OFF, targets[i]) ||
            portal_owned(c.map, MAP_FRAMES_OFF, targets[i])) {
            r->first_id = targets[i];
            status = SH_EDITOR_DELETE_PORTAL;
            goto done;
        }

    int blocked = 0;
    if (!call_blocked(c.ed + ED_RESTRICT_OFF, RESTRICT_DELETE, &blocked)) goto done;
    if (blocked) { status = SH_EDITOR_DELETE_RESTRICTED; goto done; }

    /* Snapshot what the request will change. */
    int hover = -1;
    void *prior_ids = NULL;
    if (!rd_s32(c.sel + SEL_HOVER_OFF, &hover) || !rd_s64(c.sel + SEL_WIRE_OFF, &c.wire) ||
        !rd_s32(c.sel + SEL_COUNT_OFF, &c.prior_count) || c.prior_count < 0 ||
        c.prior_count > LIST_LIMIT || !rd_ptr(c.sel + SEL_IDS_OFF, &prior_ids) ||
        (c.prior_count > 0 && !prior_ids)) {
        status = SH_EDITOR_DELETE_NOT_READY;
        goto done;
    }
    if (c.prior_count > 0) {
        c.prior = (int *)malloc(sizeof(int) * (size_t)c.prior_count);
        if (!c.prior) goto done;
        for (int i = 0; i < c.prior_count; i++)
            if (!rd_s32((uint8_t *)prior_ids + (size_t)i * 4, &c.prior[i])) {
                status = SH_EDITOR_DELETE_NOT_READY;
                goto done;
            }
    }

    /* From here every refusal restores the snapshot. */
    int ok = call_clear_selection(c.sel);
    for (int i = 0; ok && i < ntargets; i++) ok = call_add(c.sel, targets[i]);
    if (ok) {
        int n = -1;
        void *now = NULL;
        ok = rd_s32(c.sel + SEL_COUNT_OFF, &n) && n == ntargets && rd_ptr(c.sel + SEL_IDS_OFF, &now) && now;
        for (int i = 0; ok && i < n; i++) {
            int v = -1;
            ok = rd_s32((uint8_t *)now + (size_t)i * 4, &v) && in_bits(bits, c.ent_count, v);
        }
    }
    /* The capability builder and the action both fold the hovered entity into
     * the selection. A non-target hover leaves first; a hovered target is the
     * action's to clear. */
    if (ok && hover != -1 && !in_bits(bits, c.ent_count, hover)) {
        ok = call_clear_hover(c.ed);
        r->hover_cleared = ok;
    }
    /* Entering Selection natively requires hovering an entity, which drops a
     * hovered connection. Idle's Enter later reads both connection ends, so a
     * connection touching a target is dropped here with the store the native
     * setter performs (it is a plain compare-and-store in both executables). */
    if (ok && c.wire != -1 &&
        (in_bits(bits, c.ent_count, (int)(c.wire & 0xffffffff)) || in_bits(bits, c.ent_count, (int)(c.wire >> 32)))) {
        ok = wr_s64(c.sel + SEL_WIRE_OFF, -1);
        c.wire_cleared = ok;
        r->wire_cleared = ok;
    }
    /* Selection must be the current substate so its update runs the commit
     * phase. The native substate setters write the index and the dirty byte;
     * EntityMode's update performs Exit/Enter. */
    if (ok && c.cur == SUBSTATE_IDLE) {
        ok = wr_s32(c.mode + MODE_CUR_OFF, SUBSTATE_SELECTION) && wr_u8(c.mode + MODE_DIRTY_OFF, 1);
        c.changed_substate = 1;
    }
    if (ok) ok = call_caps(c.selection_sub, c.mode, c.ed);
    uint8_t caps = 0;
    if (ok) ok = rd_u8(c.selection_sub + SELECTION_CAPS_OFF, &caps);
    if (!ok) {
        r->rolled_back = rollback(&c);
        goto done;
    }
    if (!(caps & CAP_DELETABLE) || (caps & CAP_REFERENCED)) {
        status = (caps & CAP_DELETABLE) ? SH_EDITOR_DELETE_REFERENCED : SH_EDITOR_DELETE_PROTECTED;
        r->rolled_back = rollback(&c);
        if (!r->rolled_back) status = SH_EDITOR_DELETE_FAULT;
        goto done;
    }

    if (!call_delete(c.selection_sub, c.mode, c.ed)) goto done;

    for (int i = 0; i < ntargets; i++)
        if (!entity_alive(c.ents, c.ent_count, targets[i])) r->released++;
    for (int i = 0; i < c.prior_count; i++)
        if (!in_bits(bits, c.ent_count, c.prior[i])) r->deselected++;
    status = r->released == ntargets ? SH_EDITOR_DELETE_DISPATCHED : SH_EDITOR_DELETE_INCOMPLETE;

done:
    free(c.prior);
    free(targets);
    free(bits);
    return status;
}

int sh_editor_delete_batch(const int *ids, int count, sh_editor_delete_result *out)
{
    sh_editor_delete_result r;
    memset(&r, 0, sizeof r);
    r.first_id = -1;
    r.requested = count;
    r.status = batch(ids, count, &r);
    if (r.status != SH_EDITOR_DELETE_BUSY) r.busy = SH_EDITOR_DELETE_BUSY_NONE;
    if (out) *out = r;
    return r.status;
}

int sh_editor_delete_native_pending(void)
{
    void *map = NULL;
    int state = 0, cur = 0, prev = 0, idle_phase = 0, selection_phase = 0, queued = 0;
    if (!g_bound) return 0;
    uint8_t *ed = g_engine.editor, *mode = ed + ED_MODE_OFF;
    if (!rd_ptr(ed + ED_MAP_OFF, &map) || !map) return 0;
    if (!rd_s32(ed + ED_STATE_OFF, &state) || state != EDITOR_ENTITY_MODE) return 0;
    if (!rd_s32(mode + MODE_CUR_OFF, &cur) || !rd_s32(mode + MODE_PREV_OFF, &prev) ||
        !rd_s32(mode + MODE_IDLE_OFF + SUB_PHASE_OFF, &idle_phase) ||
        !rd_s32(mode + MODE_SELECTION_OFF + SUB_PHASE_OFF, &selection_phase) ||
        !rd_s32(mode + MODE_QUEUED_OFF, &queued))
        return 1;
    if (selection_phase != SELECTION_PHASE_TICK) return 1;
    if ((cur == SUBSTATE_IDLE || prev == SUBSTATE_IDLE) && idle_phase != IDLE_PHASE_TICK) return 1;
    return queued != -1;
}

static int shape_at(const uint8_t *p, const short *shape, size_t n)
{
    __try {
        for (size_t i = 0; i < n; i++)
            if (shape[i] >= 0 && p[i] != (uint8_t)shape[i]) return 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uintptr_t sh_editor_delete_decode_clear_hover(const uint8_t *action, const uint8_t *image,
                                              size_t image_size)
{
    const size_t site_len = sizeof k_clear_site / sizeof k_clear_site[0];
    const size_t shape_len = sizeof k_clear_shape / sizeof k_clear_shape[0];
    if (!action || !image || image_size < shape_len || action < image ||
        (size_t)(action - image) >= image_size)
        return 0;
    size_t avail = image_size - (size_t)(action - image);
    size_t window = avail < DECODE_WINDOW + site_len ? avail : DECODE_WINDOW + site_len;
    uintptr_t callee = 0;
    int sites = 0;
    for (size_t off = 0; off + site_len <= window; off++) {
        if (!shape_at(action + off, k_clear_site, site_len)) continue;
        int32_t rel = 0;
        memcpy(&rel, action + off + CLEAR_SITE_CALL + 1, sizeof rel);
        callee = (uintptr_t)(action + off + CLEAR_SITE_CALL + 5) + (intptr_t)rel;
        sites++;
    }
    if (sites != 1) return 0;
    if (callee < (uintptr_t)image || callee - (uintptr_t)image > image_size - shape_len) return 0;
    if (!shape_at((const uint8_t *)callee, k_clear_shape, shape_len)) return 0;
    return callee;
}

#ifndef SH_EDITOR_DELETE_TESTING
static const uint8_t *g_main_thread_at;

static int on_main_thread(void)
{
    int id = 0;
    if (!g_main_thread_at || !rd_s32(g_main_thread_at, &id) || id == 0) return -1;
    return (DWORD)id == GetCurrentThreadId() ? 1 : 0;
}

static size_t image_size(const uint8_t *base)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int sh_editor_delete_install(const sig_result *results, size_t n, const uint8_t *module_base,
                             const uint8_t *editor, int (*manipulation_in_progress)(void))
{
    sh_editor_delete_engine e;
    char line[256];
    memset(&e, 0, sizeof e);
    uintptr_t action = sig_addr_by_name(results, n, "SelectionDeleteAction");
    size_t span = module_base ? image_size(module_base) : 0;
    uintptr_t clear = span ? sh_editor_delete_decode_clear_hover((const uint8_t *)action, module_base, span) : 0;
    e.editor = (uint8_t *)editor;
    e.clear_selection = (void (*)(void *))sig_addr_by_name(results, n, "ClearSelection");
    e.add_to_selection = (unsigned char (*)(void *, uint32_t))sig_addr_by_name(results, n, "AddToSelection");
    e.clear_hover = (void (*)(void *))clear;
    e.refresh_capabilities = (void (*)(void *, void *, void *))sig_addr_by_name(results, n, "SelectionCapabilities");
    e.blocked = (unsigned char (*)(void *, uint32_t))sig_addr_by_name(results, n, "GridPropertiesBlocked");
    e.delete_selection = (void (*)(void *, void *, void *))action;
    e.manipulation_in_progress = manipulation_in_progress;
    if (module_base) g_main_thread_at = (const uint8_t *)glb_resolve(module_base, "main_thread_id", NULL);
    e.on_main_thread = on_main_thread;
    int bound = g_main_thread_at && sh_editor_delete_bind(&e);
    if (!bound) sh_editor_delete_bind(NULL);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "editor delete: %s (action=%p caps=%p clear_hover=%p blocked=%p add=%p clear=%p main=%p)",
        bound ? "native Delete Selection bound" : "unavailable; window deletion refuses",
        (void *)action, (void *)e.refresh_capabilities, (void *)clear, (void *)e.blocked,
        (void *)e.add_to_selection, (void *)e.clear_selection, (void *)g_main_thread_at);
    backend_log(line);
    return bound;
}
#else
int sh_editor_delete_install(const sig_result *results, size_t n, const uint8_t *module_base,
                             const uint8_t *editor, int (*manipulation_in_progress)(void))
{
    (void)results; (void)n; (void)module_base; (void)editor; (void)manipulation_in_progress;
    return 0;
}
#endif
