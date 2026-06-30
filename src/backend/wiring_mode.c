/* wiring_mode.c -- see wiring_mode.h. The sh_target_any interactive "link any entities" wire mode.
 * Clean-room from our own reverse-engineering (DIRECT). Zero OG SnapHak bytes.
 *
 * The editor wire tool's central pick processor (FUN_140cdad70 @ 0xcdad70) classifies every picked
 * entity and drives the stock node-mediated connect flow. We inline-detour it and gate a new behavior on
 * g_wire_mode:
 *
 *   OFF -> trampoline to the original (the stock wire tool is untouched; no uninstall needed for OFF).
 *   ON  -> a two-pick direct-edge state machine:
 *           pick #1  : remember the source table index (g_src), reset the tool, swallow the pick.
 *           pick #2  : lay a direct connection g_src -> picked via the editor's own connection primitive
 *                      FUN_1405a70d0(ET+0x5e0, src, tgt), mark the map dirty, reset the tool, swallow.
 *           index -1 : a deselect / internal re-pick -- ignored (not treated as a pick).
 *
 * The pick processor's ABI (DIRECT, from our decompile):
 *   void FUN_140cdad70(tool[rcx], passthrough[rdx], WORLD[r8], pickedIndex[r9d uint]).
 *   WORLD is the editor world; ET = *(WORLD+0x204c8); the op/undo object = *(WORLD+0x204d0).
 * The tool-reset helper FUN_140cdb3e0(tool) clears the four pick slots + the picker-result fields, so we
 * call it after each handled pick to keep the tool clean (which also restores it on OFF).
 * The connection primitive FUN_1405a70d0(csr=ET+0x5e0, src[edx], tgt[r8d]) -> char(1 on success)
 * directly mutates the live forward/reverse adjacency -- the same routine the stock tool uses internally.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wiring_mode.h"
#include "commands.h"     /* idCmdArgs / sh_printf */
#include "signatures.h"
#include "patch.h"        /* sh_install_detour_sig (reuses hook.c's reversible inline-detour installer) */
#include "backend_log.h"

/* ---- engine-function signature names (in the shipped signature DB) ------------------------------- */
#define SIG_PICK_PROCESSOR   "PickProcessor"          /* FUN_140cdad70 -- the detour target */
#define SIG_TOOL_RESET       "ToolReset"              /* FUN_140cdb3e0 -- per-pick tool reset */
#define SIG_OUTPUT_CREATOR   "ConnectOutputCreator"   /* FUN_140cdbb40 -- the connect-primitive decode anchor */

/* Whole, position-independent prologue bytes the detour installer steals (3 reg-save MOVs = 15 bytes).
 * 15 >= the installer's 14-byte minimum, lands on an instruction boundary, and carries no RIP-relative
 * or relative-branch operand -- so it copies verbatim to the trampoline. */
#define WIRING_STOLEN        15u

/* ---- build-specific editor-struct offsets (RE-DERIVE per DOOM build) -----------------------------
 * All four are read straight out of the pick processor's own decompile (the values it dereferences), so
 * the re-derive recipe is: decompile the pick processor on the new build and read the constants it uses. */
#define WORLD_MAP_OBJ_OFF    0x204c8   /* WORLD -> ET (loaded-map / connection container) */
#define WORLD_OP_OBJ_OFF     0x204d0   /* WORLD -> op/undo object (carries the dirty flag) */
#define ET_FWD_CSR_OFF       0x5e0     /* ET -> forward adjacency base (the connection primitive's arg0) */
#define OP_DIRTY_OFF         0x18      /* op object -> map-dirty flag (set to 1 so a live edit persists) */

/* The connection primitive's distinctive 23-byte prologue (the bytes up to -- but not including -- the
 * inner call it forwards to). A byte-identical sibling edge-REMOVE routine shares this prologue and
 * differs ONLY in that inner call's target, so the primitive cannot be isolated by its own bytes; we
 * instead find the single call to a function with THIS prologue inside the output creator (which lays
 * exactly one such edge). RE-DERIVE: re-extract the prologue from the connection primitive's decompile. */
#define EDGE_PROLOGUE_LEN    23
static const uint8_t EDGE_PROLOGUE[EDGE_PROLOGUE_LEN] = {
    0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10, 0x57, 0x48,0x83,0xEC,0x20,
    0x41,0x8B,0xF8, 0x48,0x8B,0xD9, 0x8B,0xF2
};
#define EDGE_ANCHOR_SCAN     0x140u    /* bytes of the output-creator body to scan for the edge call */
#define IN_IMAGE_SPAN        0x10000000u  /* a decoded target must land inside the mapped DOOM image */

/* ---- engine-function typedefs (resolved by signature) ------------------------------------------- */
typedef void (*pick_fn) (void *tool, void *passthrough, void *world, unsigned picked_index);
typedef void (*reset_fn)(void *tool);
typedef char (*edge_fn) (void *csr, int src, int tgt);

/* ---- module state ------------------------------------------------------------------------------- */
static const uint8_t *g_module_base = NULL;
static volatile LONG  g_installed   = 0;     /* one-shot install latch */
static volatile LONG  g_wire_mode   = 0;     /* the toggle (0 = off by default) */
static int            g_src         = -1;    /* the remembered first-pick source index (-1 = none) */

static pick_fn  g_orig_pick = NULL;          /* trampoline to the original pick processor (NULL = not installed) */
static reset_fn g_reset     = NULL;          /* the tool-reset helper */
static edge_fn  g_edge      = NULL;          /* the connection primitive */
static void    *g_last_tool = NULL;          /* the live tool object (captured at the first pick) */

/* ---- helpers ------------------------------------------------------------------------------------ */

/* Resolve a named signature off the cached module base (mirrors the other feature modules' resolvers). */
static int wm_resolve_sig(const char *name, sig_result *out)
{
    if (g_module_base == NULL || name == NULL) return 0;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) != 0) continue;
        sig_resolve_one(g_module_base, &BACKEND_ENGINE_SIGNATURES[i], out);
        return 1;
    }
    return 0;
}

/* SEH-guarded byte compare (a wild decoded pointer must never fault the editor). */
static int wm_bytes_eq(const uint8_t *p, const uint8_t *q, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) if (p[i] != q[i]) return 0; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Resolve the connection primitive the version-portable way: sig the output creator, then scan its body
 * for the single `call rel32` whose decoded target carries the connection primitive's prologue. Returns
 * the primitive address, or NULL if zero / more than one distinct such target is found (refuse cleanly).
 * SEH-guarded; a bad scan yields NULL, never a fault. */
static void *wm_resolve_edge(void)
{
    sig_result r;
    if (!wm_resolve_sig(SIG_OUTPUT_CREATOR, &r) || r.status != SIG_OK) {
        sh_printf("sh_target_any: connect-tool anchor sig unresolved (status=%d) -- wire-any unavailable.\n",
                  (int)r.status);
        return NULL;
    }
    const uint8_t *body = (const uint8_t *)r.addr;
    const uint8_t *lo   = g_module_base;
    const uint8_t *hi   = g_module_base + IN_IMAGE_SPAN;
    void *found    = NULL;
    int   distinct = 0;
    __try {
        for (uint32_t off = 0; off + 5u <= EDGE_ANCHOR_SCAN; off++) {
            if (body[off] != 0xE8) continue;                 /* call rel32 */
            int32_t rel;
            memcpy(&rel, body + off + 1, 4);
            const uint8_t *tgt = body + off + 5 + (intptr_t)rel;
            if (tgt < lo || tgt > hi) continue;              /* out-of-image -> not it */
            if (!wm_bytes_eq(tgt, EDGE_PROLOGUE, EDGE_PROLOGUE_LEN)) continue;
            if (found != NULL && found != (void *)tgt) { distinct = 2; break; }  /* ambiguous */
            if (found == NULL) { found = (void *)tgt; distinct = 1; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    if (distinct != 1) {
        sh_printf("sh_target_any: connection-primitive decode %s -- wire-any unavailable.\n",
                  distinct == 0 ? "found no match" : "was ambiguous");
        return NULL;
    }
    return found;
}

/* SEH-guarded tool reset (clears the pick slots + picker-result fields). */
static void wm_reset_tool(void *tool)
{
    if (!g_reset || !tool) return;
    __try { g_reset(tool); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* SEH-guarded direct connection src -> tgt, then mark the map dirty so the edit persists. */
static void wm_fire_edge(void *world, int src, int tgt)
{
    if (!g_edge || !world) return;
    __try {
        void *et = *(void **)((const uint8_t *)world + WORLD_MAP_OBJ_OFF);
        if (et == NULL) return;
        g_edge((uint8_t *)et + ET_FWD_CSR_OFF, src, tgt);
        void *op = *(void **)((const uint8_t *)world + WORLD_OP_OBJ_OFF);
        if (op != NULL) *(int *)((uint8_t *)op + OP_DIRTY_OFF) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* ---- the detour: the flag-gated pick processor --------------------------------------------------- */
/* MS x64 ABI maps (rcx,rdx,r8,r9) to these four parameters, matching the original's declaration exactly. */
static void pick_processor_detour(void *tool, void *passthrough, void *world, unsigned picked_index)
{
    /* OFF: transparent passthrough to the stock wire tool (the whole point of the flag-gate -- OFF needs
     * no uninstall and leaves the original behavior completely intact). */
    if (!g_wire_mode || g_orig_pick == NULL) {
        if (g_orig_pick) g_orig_pick(tool, passthrough, world, picked_index);
        return;
    }

    g_last_tool = tool;   /* remember the live tool so OFF can reset any half-done pick */

    /* deselect / internal re-pick (index -1) -- not a real pick, ignore. */
    if ((int)picked_index < 0) return;

    if (g_src < 0) {
        /* first pick: remember the source, keep the tool clean, swallow the pick (skip the stock picker). */
        g_src = (int)picked_index;
        wm_reset_tool(tool);
        return;
    }

    /* second pick: lay the direct connection source -> picked, mark dirty, reset, swallow. */
    wm_fire_edge(world, g_src, (int)picked_index);
    g_src = -1;
    wm_reset_tool(tool);
}

/* ---- install + toggle --------------------------------------------------------------------------- */

void sh_wiring_mode_install(const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return;   /* one-shot */
    g_module_base = module_base;

    /* Resolve ALL three engine deps FIRST; install the detour only if every one resolves (so we never
     * leave a detour live with a missing primitive -- no half-state, no crash). */
    sig_result rp, rr;
    if (!wm_resolve_sig(SIG_PICK_PROCESSOR, &rp) || rp.status != SIG_OK) {
        backend_log("B2: sh_target_any wire-any NOT armed (pick-processor sig unresolved)");
        return;
    }
    if (!wm_resolve_sig(SIG_TOOL_RESET, &rr) || rr.status != SIG_OK) {
        backend_log("B2: sh_target_any wire-any NOT armed (tool-reset sig unresolved)");
        return;
    }
    void *edge = wm_resolve_edge();   /* anchor-and-decode the connection primitive */
    if (edge == NULL) {
        backend_log("B2: sh_target_any wire-any NOT armed (connection primitive unresolved)");
        return;
    }
    g_reset = (reset_fn)rr.addr;
    g_edge  = (edge_fn)edge;

    /* Install the (off-by-default) detour LAST, once every dep is in hand. */
    void *tramp = sh_install_detour_sig(&rp, (void *)pick_processor_detour, WIRING_STOLEN);
    if (tramp == NULL) {
        g_reset = NULL;
        g_edge  = NULL;
        backend_log("B2: sh_target_any wire-any NOT armed (pick-processor detour install failed)");
        return;
    }
    g_orig_pick = (pick_fn)tramp;
    backend_log("B2: sh_target_any wire-any ready (off by default; pick-processor detour installed)");
}

void h_wiring_mode(struct idCmdArgs *a)
{
    (void)a;
    if (g_orig_pick == NULL) {
        sh_printf("sh_target_any: wire-any unavailable -- the editor connect-tool functions did not "
                  "resolve on this build.\n");
        return;
    }
    if (!g_wire_mode) {
        g_src = -1;
        g_wire_mode = 1;
        sh_printf("sh_target_any: WIRE-ANY mode ON -- pick a source entity then a target with the wire "
                  "tool to lay a direct connection between ANY two entities (even node-less targets like a "
                  "timeline). Repeat as needed; run sh_target_any again to turn off.\n");
        backend_log("B2: sh_target_any wire-any ON");
    } else {
        g_wire_mode = 0;
        g_src = -1;
        wm_reset_tool(g_last_tool);   /* clear any half-done pick; harmless if no pick happened */
        sh_printf("sh_target_any: WIRE-ANY mode OFF -- the normal wire tool is restored.\n");
        backend_log("B2: sh_target_any wire-any OFF");
    }
}
