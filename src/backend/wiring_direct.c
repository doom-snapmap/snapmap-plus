/* wiring_direct.c -- see wiring_direct.h. The sh_target_any "direct edge" wire hook.
 *
 * Gated on sh_target_any_is_shown() (the SAME toggle as the editor-decl visibility flip). When the wire
 * source is an output node, this forces a direct source->target edge (no input radial / no node mediation)
 * for ANY target -- INCLUDING when sh_target_any is flipped on mid-pick (a wire already stemming from an
 * output node attached to the cursor). Frees NO node. Clean-room from our own reverse-engineering; zero OG
 * SnapHak bytes.
 *
 * The editor wire tool's connect creator for an OUTPUT-NODE source is engine FUN_140cdb990 (the pick
 * processor's creator-selector 1). Stock lays a direct edge only when the target is itself an input node,
 * else it node-mediates (auto-creates an action node -- the "which input" radial the user wants gone). In
 * the reveal state we detour it to force the direct-edge outcome for ANY target, and to de-stale any
 * auto-node the stock flow left behind when the toggle was flipped mid-pick.
 *
 * De-stale is INDEX-CLEAR ONLY (write the stale slot to -1 + clear the tool's node flag) -- it must NEVER
 * delete the node: deleting frees the node body and replaces its entity-table slot with a sentinel while
 * leaving a still-live incoming edge dangling, which the editor's per-frame graph/draw pass then faults
 * dereferencing. Leaving the node a live orphan keeps every entity-table slot valid, so that fault is
 * impossible. The dragged preview edge is removed+relaid per hover by the tool's own undo (keyed on the
 * undo class we set), so setting the undo class consistently keeps the add/remove balanced (no spray).
 *
 * This never touches the tool's think-state (+0x28) or its input-capture, so input stays alive on-, mid-,
 * and off-toggle; OFF is a pure passthrough with nothing to clean up.
 *
 * PORTABILITY: cdb990 is resolved by SIGNATURE (WireConnectCreator1). The tool-struct field offsets below
 * are build-specific -- re-derive per DOOM build by decompiling FUN_140cdb990.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "wiring_direct.h"
#include "target_any.h"     /* sh_target_any_is_shown() -- the shared toggle */
#include "signatures.h"
#include "patch.h"          /* sh_install_detour_sig (reversible inline-detour layer) */
#include "backend_log.h"

#define SIG_CREATOR1     "WireConnectCreator1"   /* 0xcdb990 -- the output-node-source connect creator */

/* Whole, position-independent prologue bytes the detour installer steals. cdb990's prologue lands on an
 * instruction boundary at 15 bytes (2 reg-save MOVs + push rdi + sub rsp,0x20), >= the installer's 14-byte
 * minimum, no RIP-relative/relative-branch operand in range -- copies verbatim to the trampoline. */
#define WIRING_STOLEN    15u

/* ---- build-specific wire-tool field offsets (RE-DERIVE per DOOM build; the constants cdb990 derefs) ---- */
#define TOOL_FLAGS_C_OFF   0x0c   /* create-flag byte; |9 = the direct-edge marks (bit 0 -> cd9830 commits) */
#define TOOL_FLAGS_D_OFF   0x0d   /* flag byte; bit 0x02 = "an action node is live in slot +0x1c"          */
#define TOOL_SLOT0_OFF     0x10   /* chain slot 0 (unused by the output-node path; cleared defensively)     */
#define TOOL_SLOT1_OFF     0x14   /* chain slot 1 = the OUTPUT-NODE SOURCE (cdad70 stored it here)          */
#define TOOL_SLOT2_OFF     0x18   /* chain slot 2 = the target (consecutive with slot1 -> cdb010 pairs them)*/
#define TOOL_SLOT3_OFF     0x1c   /* chain slot 3 = the stale auto action node (mid-pick), if any           */
#define TOOL_UNDOCLASS_OFF 0x24   /* undo class; =2 so the next cd9a90 removes slot1->slot2 (no spray)       */
/* NEVER touch think (+0x28): a 0 think with the tool active swallows input. */

typedef void (*creator_fn)(void *tool, void *world, int idx);

static const uint8_t   *g_module_base   = NULL;
static volatile LONG    g_installed     = 0;
static creator_fn       g_orig_creator1 = NULL;   /* trampoline to stock cdb990 */

/* Resolve a named signature off the cached module base. */
static int wd_resolve_sig(const char *name, sig_result *out)
{
    if (g_module_base == NULL || name == NULL) return 0;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) != 0) continue;
        sig_resolve_one(g_module_base, &BACKEND_ENGINE_SIGNATURES[i], out);
        return 1;
    }
    return 0;
}

/* The detour on cdb990. ABI: void(tool, world, idx) maps (rcx, rdx, r8d). */
static void connect_creator1_detour(void *tool, void *world, int idx)
{
    (void)world;
    /* OFF (not in the reveal state): transparent passthrough -- the stock tool is byte-clean, input untouched. */
    if (!sh_target_any_is_shown() || g_orig_creator1 == NULL) {
        if (g_orig_creator1) g_orig_creator1(tool, world, idx);
        return;
    }

    __try {
        uint8_t *t = (uint8_t *)tool;
        int src = *(int *)(t + TOOL_SLOT1_OFF);          /* the output-node source (cdad70 put it here) */

        /* idx == -1 (deselect/internal) OR idx == src (the source pick itself) -> lay NO edge, create NO node.
         * Returning WITHOUT the original suppresses stock's spurious source-pick action-node create and
         * leaves the tool as the pick processor set it (slot1=src, think=2 -- a valid handled state). */
        if (idx == -1 || idx == src)
            return;

        /* DE-STALE (the mid-pick-toggle guard): if the toggle was flipped on mid-pick, the stock flow may
         * already have auto-created an action node into slot3 (+0x1c, +0xd&2). Left in place, the trailing
         * cdb010 would chain tgt->node (a spray). INDEX-clear the target-side slots + forget the node flag so
         * cdb010 lays ONLY src->tgt. NEVER cda210/cda2b0 -- they FREE the node + sentinel its entity-table
         * slot, which is the per-frame crash. The node is left a live orphan (benign). No-op in the clean flow
         * (slot +0x1c is already -1 there). */
        *(int *)(t + TOOL_SLOT0_OFF)        = -1;
        *(int *)(t + TOOL_SLOT3_OFF)        = -1;
        *(uint8_t *)(t + TOOL_FLAGS_D_OFF) &= (uint8_t)~2;

        /* FORCE THE DIRECT EDGE: record the target into slot2 (consecutive with slot1=src) + the direct-edge
         * flags. The caller's trailing cdb010 then lays slot1(src) -> slot2(tgt) for ANY target. +0x24=2 so
         * the next hover's cd9a90 (case 2) removes exactly this edge -> add/remove cancel -> no spray. */
        *(int *)(t + TOOL_SLOT2_OFF)        = idx;
        *(uint8_t *)(t + TOOL_FLAGS_C_OFF) |= (uint8_t)9;
        *(int *)(t + TOOL_UNDOCLASS_OFF)    = 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void sh_wiring_direct_install(const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return;   /* one-shot */
    g_module_base = module_base;

    sig_result rc1;
    if (!wd_resolve_sig(SIG_CREATOR1, &rc1) || rc1.status != SIG_OK) {
        backend_log("B2: sh_target_any direct-edge NOT armed (WireConnectCreator1 sig unresolved)");
        return;
    }
    void *tramp = sh_install_detour_sig(&rc1, (void *)connect_creator1_detour, WIRING_STOLEN);
    if (tramp == NULL) {
        backend_log("B2: sh_target_any direct-edge NOT armed (cdb990 detour install failed)");
        return;
    }
    g_orig_creator1 = (creator_fn)tramp;
    backend_log("B2: sh_target_any direct-edge ready (off until sh_target_any reveal; cdb990 detoured)");
}
