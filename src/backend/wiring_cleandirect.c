/* Extend the revealed sh_target_any wire tool to bare entity targets.
 * Normal input/output nodes use stock wire creation. Bare targets instead add
 * a state.edit.targets reference to the source, avoiding an unroutable CSR edge.
 * Creator functions resolve by signature; recheck field offsets when porting. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "wiring_cleandirect.h"
#include "target_any.h"
#include "signatures.h"
#include "patch.h"
#include "backend_log.h"
#include "apply_engine.h"

#define SIG_CREATOR_BASE     "ConnectOutputCreator"   /* 0xcdbb40 -- base-entity-source connect creator */
#define SIG_CREATOR_OUTNODE  "WireConnectCreator1"    /* 0xcdb990 -- output-node-source connect creator */
#define WIRING_STOLEN        15u

/* Recheck these fields in the WireConnectCreator0/1 tool reads when porting. */
#define WORLD_ENTTABLE_OFF   0x204c8   /* world(param_2) -> loaded-map/entity-table object */
#define MAP_ENTARRAY_OFF     0x6a0     /* that object -> the entity-ptr array (8-byte entries) */
#define ENT_DECL_OFF         0x08      /* entity + 8 -> the resolved decl (idDeclSnapEditorEntity) */
#define DECL_FLAGS_OFF       0x3cd     /* decl -> the editor-flags byte */
#define DECL_ISINPUT_BIT     0x10      /* decl+0x3cd bit 0x10 = is-input-node (the clean-direct gate) */
#define DECL_ISOUTPUT_BIT    0x20      /* decl+0x3cd bit 0x20 = is-output-node (leave those untouched) */
#define WCD_TOOL_SOURCE_OFF  0x08      /* Source entity ID; rederive from the creator's tool reads. */

typedef void (*creator_fn)(void *tool, void *world, int idx);

static const uint8_t *g_module_base    = NULL;
static volatile LONG  g_installed      = 0;
static creator_fn     g_orig_base      = NULL;
static creator_fn     g_orig_outnode   = NULL;

/* Notify the frontend of connect edits even when entity count is unchanged,
 * so module-name labels can refresh through interface slot +0x288. */
static volatile LONG  g_connect_generation = 0;

/* Creators run every hover frame. Debounce each bare source/target pair;
 * backend reference insertion also deduplicates repeated IDs. Editor thread only. */
static int            g_last_write_source = -1;
static int            g_last_write_target = -1;

int sh_wiring_cleandirect_generation(void)
{
    return (int)g_connect_generation;
}

/* Resolve the hovered target's decl-flags byte pointer from (world, idx). Returns the byte* or NULL. */
static uint8_t *wcd_target_flags(void *world, int idx)
{
    if (!world || idx < 0) return NULL;
    __try {
        void *mapobj = *(void *const *)((const uint8_t *)world + WORLD_ENTTABLE_OFF);
        if (!mapobj) return NULL;
        void *array = *(void *const *)((const uint8_t *)mapobj + MAP_ENTARRAY_OFF);
        if (!array) return NULL;
        void *ent = ((void *const *)array)[idx];
        if (!ent) return NULL;
        void *decl = *(void *const *)((const uint8_t *)ent + ENT_DECL_OFF);
        if (!decl) return NULL;
        return (uint8_t *)decl + DECL_FLAGS_OFF;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* Route bare targets through reference insertion and nodes through stock wiring. */
static void wcd_run(creator_fn stock, void *tool, void *world, int idx)
{
    if (!sh_target_any_is_shown() || stock == NULL) {
        if (stock) stock(tool, world, idx);
        return;
    }

    /* A target with neither input nor output flags cannot accept a CSR edge.
     * Use its native activate reference instead of leaving a dangling wire. */
    uint8_t *flagp = (idx >= 0) ? wcd_target_flags(world, idx) : NULL;
    int bare = 0;
    if (flagp) {
        __try {
            uint8_t f = *flagp;
            if ((f & DECL_ISOUTPUT_BIT) == 0 && (f & DECL_ISINPUT_BIT) == 0) bare = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { bare = 0; }
    }

    if (bare && idx >= 0) {
        /* Suppress stock creation and queue one reference per distinct hover pair. */
        int source = -1;
        __try { source = *(const int *)((const uint8_t *)tool + WCD_TOOL_SOURCE_OFF); }
        __except (EXCEPTION_EXECUTE_HANDLER) { source = -1; }
        if (source >= 0 && (source != g_last_write_source || idx != g_last_write_target)) {
            g_last_write_source = source;
            g_last_write_target = idx;
            ae_schedule_target_write(source, idx);        /* Queue serialization and apply on the engine command drain. */
            InterlockedIncrement(&g_connect_generation);
        }
        return;
    }

    /* Reset debounce when leaving a bare target, then resume stock wiring. */
    g_last_write_source = -1;
    g_last_write_target = -1;
    stock(tool, world, idx);
    if (idx >= 0) InterlockedIncrement(&g_connect_generation);
}

static void connect_creator_base_detour(void *tool, void *world, int idx)
{
    wcd_run(g_orig_base, tool, world, idx);
}

static void connect_creator_outnode_detour(void *tool, void *world, int idx)
{
    wcd_run(g_orig_outnode, tool, world, idx);
}

static int wcd_resolve_sig(const char *name, sig_result *out)
{
    if (g_module_base == NULL || name == NULL) return 0;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) != 0) continue;
        sig_resolve_one(g_module_base, &BACKEND_ENGINE_SIGNATURES[i], out);
        return 1;
    }
    return 0;
}

void sh_wiring_cleandirect_install(const uint8_t *module_base)
{
    if (g_installed && sh_detour_is_installed((void *)g_orig_base) &&
        sh_detour_is_installed((void *)g_orig_outnode)) return;
    InterlockedExchange(&g_installed, 0);
    if (g_orig_outnode) {
        if (!sh_uninstall_detour((void *)g_orig_outnode)) return;
        g_orig_outnode = NULL;
    }
    if (g_orig_base) {
        if (!sh_uninstall_detour((void *)g_orig_base)) return;
        g_orig_base = NULL;
    }
    g_module_base = module_base;

    sig_result rb, ro;
    if (!wcd_resolve_sig(SIG_CREATOR_BASE, &rb) || rb.status != SIG_OK) {
        backend_log("B2: sh_target_any clean-direct NOT armed (base creator sig unresolved)");
        return;
    }
    if (!wcd_resolve_sig(SIG_CREATOR_OUTNODE, &ro) || ro.status != SIG_OK) {
        backend_log("B2: sh_target_any clean-direct NOT armed (output-node creator sig unresolved)");
        return;
    }

    void *tb = sh_prepare_detour_sig(&rb, (void *)connect_creator_base_detour, WIRING_STOLEN);
    if (tb == NULL) {
        backend_log("B2: sh_target_any clean-direct NOT armed (base creator detour install failed)");
        return;
    }
    g_orig_base = (creator_fn)tb;
    void *to = sh_prepare_detour_sig(&ro, (void *)connect_creator_outnode_detour, WIRING_STOLEN);
    g_orig_outnode = (creator_fn)to;
    if (!to || sh_commit_detour(tb) != B2_PATCH_OK || sh_commit_detour(to) != B2_PATCH_OK) {
        if (to && sh_uninstall_detour(to)) g_orig_outnode = NULL;
        if (sh_uninstall_detour(tb)) g_orig_base = NULL;
        backend_log("B2: sh_target_any clean-direct commit failed; retained callbacks require restoration");
        return;
    }
    InterlockedExchange(&g_installed, 1);
    backend_log("B2: sh_target_any clean-direct ready (off until reveal; cdbb40 + cdb990 detoured)");
}
