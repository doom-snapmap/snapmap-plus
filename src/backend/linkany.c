/* linkany.c -- see linkany.h. The sh_target_any "link any entities" toggle (option A: the clean
 * instance-filter lever). Clean-room from our own RE (DIRECT). Zero OG bytes.
 *
 * MECHANISM (option A -- force-open the target-compatibility gate, KEEP native node-mediation):
 *   The editor Add-Logic connect tool gates each candidate connection target through a LAYER-1 instance
 *   filter:  ((target_entref+0x164 & 0x20) == 0) || (DAT_14571c660 != 0)   (DIRECT, FUN_140cdbb40 @
 *   0xcdbb8c, decompiled). DAT_14571c660 (RVA 0x571c660) is a .bss global, default 0, with 41 xrefs ALL
 *   READ and NO writer anywhere in the engine (Ghidra-verified) -- so it is a single, side-effect-free
 *   LEVER: write !=0 and the OR short-circuits true, making EVERY instance a valid connection target;
 *   write 0 to restore the stock filter. Because we only open the gate and leave the creators' native
 *   node-mediation (source -> listener-node -> action-node -> target) intact, the resulting wires are
 *   BLUE/logic wires (a raw source->target edge would render GREEN -- the v1 mistake this supersedes).
 *
 *   We resolve the lever PORTABLY (no hardcoded base+RVA): sig-resolve the OUTPUT creator FUN_140cdbb40
 *   ("ConnectOutputCreator"), then RIP-decode the lever from its first `cmp dword[rip+disp],0`
 *   (`83 3D ?? ?? ?? ?? 00`) -- rip_next + disp32 = the lever slot. This mirrors sh_resolve_gamemgr
 *   (sig a prologue, decode a RIP-relative data global). Build-portable + fail-loud.
 *
 * MECHANISM (option B -- node injection, completes "link ANY"): the lever (A) opens LAYER 1 only. A
 *   separate has-nodes requirement (decl+0x448 outputs / decl+0x460 inputs != 0) gates THREE coupled
 *   clusters (picker eligibility FUN_140cf4a40, grid populate cf54e0/cf5470, creator commit cdbb40/cdb610)
 *   -- all reading the SAME four SEDEF fields -- so a node-LESS decl (only 143/1361 have input nodes) is
 *   un-connectable even with the lever open. B is the DATA analog of A's lever: while ON we ALSO give every
 *   node-less snapEditorEntityDef one synthetic OUTPUT node (a logic listener relay) + one synthetic INPUT
 *   node (a logic action relay) by writing decl+0x440/+0x448 and +0x458/+0x460; OFF restores every touched
 *   field. The relay node ptrs are captured from a LIVE node list (so they are DIRECT-registered in the
 *   editor template registry the node-create looks up). The native connect flow then runs unmodified ->
 *   the picker offers the relay -> cda140 creates it -> cdb010 wires src->node->tgt -> BLUE, saves/reloads
 *   clean. ZERO new engine sigs (reuses the GetDeclsOfType enum, exactly like sh_unhide). SEMANTIC: a wire to a TRULY node-less target is
 *   a valid blue wire but most likely INERT on that target (no node to receive) -- faithful to OG's
 *   experimental link-any; the affordance is "any entity is now a connectable endpoint."
 *
 * Off by default. FAIL-SAFE throughout: if A's sig/decode/write doesn't resolve, the command refuses
 * cleanly and pokes nothing; B's injection is best-effort (if it can't resolve the relay/registry it skips
 * and the A lever still applies) and FULLY REVERSIBLE (every mutated field is saved + restored on OFF).
 * Every memory touch is SEH-guarded -- never a crash, never a half-written state.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "linkany.h"
#include "commands.h"   /* idCmdArgs / sh_printf */
#include "signatures.h"
#include "backend_log.h"

/* The OUTPUT creator we sig-resolve solely to RIP-decode the lever from its body. */
#define CREATOR_SIG_NAME  "ConnectOutputCreator"   /* FUN_140cdbb40 (known_rva 0xcdbb40) */

/* Bounded scan window (bytes from the creator's entry) for the lever's `cmp dword[rip+disp],0`. The
 * lever read sits at +0x4c on the pinned build; the OTHER `cmp dword[rip],0` in this fn (a different
 * global, DAT_14571b6f8) is far out at +0x255 -- capping the scan at 0x100 EXCLUDES it, so the FIRST
 * hit is unambiguously the instance-filter lever even if the exact offset shifts on a recompile. */
#define LEVER_SCAN_START  0x20u
#define LEVER_SCAN_END    0x100u

static const uint8_t *g_module_base = NULL;
static volatile LONG  g_installed   = 0;     /* one-shot latch for sh_linkany_install */
static volatile LONG  g_on          = 0;     /* the toggle state (0 = off by default) */
static volatile LONG *g_lever       = NULL;  /* the resolved lever slot (NULL = not yet resolved) */

/* ---- option B: node injection for node-less endpoints (the "link ANY" completion) ----------------
 * The SEDEF (idDeclSnapEditorEntity) node-list layout + the enum are the same sh_unhide/sh_listres use. */
#define SEDEF_TYPE_NAME    "idDeclSnapEditorEntity"
#define LIST_ARRAY_OFF     0x20u    /* decl-manager node -> decl-ptr array (sh_unhide LIST_ARRAY_OFF) */
#define LIST_COUNT_OFF     0x28u    /* decl-manager node -> decl count */
#define DECL_NAME_OFF      0x08u    /* decl -> name char* (VERIFIED: sh_commands LISTRES_NAME_OFF) */
#define DECL_OUT_PTR_OFF   0x440u   /* SEDEF -> output node-ptr array (cf54e0 [rcx+0x440]) */
#define DECL_OUT_CNT_OFF   0x448u   /* SEDEF -> output node count   (cf4a40/cf54e0 [+0x448]) */
#define DECL_IN_PTR_OFF    0x458u   /* SEDEF -> input node-ptr array (cf5470 [rdx+0x458]) */
#define DECL_IN_CNT_OFF    0x460u   /* SEDEF -> input node count    (cf4a40/cf5470 [+0x460]) */
#define SEDEF_COUNT_CAP    (1u << 20)   /* implausible-count bail (sh_unhide parity) */
#define NODELIST_SCAN_CAP  64u          /* max node entries scanned per decl when hunting a relay */
#define MAX_INJECTED       4096         /* >= 1361 decls x 2 sides; static restore table */

typedef void *(*get_decls_fn)(const char *type_name);

/* The two synthetic 1-element node arrays ALL node-less decls are pointed at while ON. Each holds one
 * relay node ptr captured from a live node list (so it is registered where cda140's lookup expects). */
static void *g_out_arr[1] = { NULL };   /* a logic-listener relay (the synthetic OUTPUT node) */
static void *g_in_arr[1]  = { NULL };   /* a logic-action relay   (the synthetic INPUT node)  */

/* Restore table: every (decl,side) field we overwrote + its original ptr (its count was 0 by selection). */
typedef struct inj_rec { void *decl; uint8_t is_input; void *orig_ptr; } inj_rec;
static inj_rec g_injected[MAX_INJECTED];
static int     g_injected_n = 0;

/* SEH-guarded primitives (decl objects are heap RW; no VirtualProtect needed -- same as sh_unhide). */
static int ir_ptr(const void *src, void **out)
{ __try { *out = *(void *const *)src; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int ir_u32(const void *src, uint32_t *out)
{ __try { *out = *(const uint32_t *)src; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int iw_ptr(void *dst, void *val)
{ __try { *(void **)dst = val; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int iw_u32(void *dst, uint32_t val)
{ __try { *(uint32_t *)dst = val; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static const char *ir_name(const void *decl)
{ __try { return *(const char *const *)((const uint8_t *)decl + DECL_NAME_OFF); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; } }

/* Resolve a named sig off the cached module base (mirrors sh_algo/sh_linkany v1's resolver). */
static int linkany_resolve_sig(const char *name, sig_result *out)
{
    if (g_module_base == NULL || name == NULL) return 0;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) != 0) continue;
        sig_resolve_one(g_module_base, &BACKEND_ENGINE_SIGNATURES[i], out);
        return 1;
    }
    return 0;
}

/* RIP-decode the instance-filter lever from the resolved creator body. Scans [START,END) for the first
 * `83 3D <disp32> 00` (cmp dword ptr[rip+disp32], 0) and returns rip_next + disp32. NULL on no-find /
 * fault / an implausible (out-of-image) target. SEH-guarded -- a bad scan never faults the editor. */
static volatile LONG *decode_lever(const uint8_t *creator)
{
    __try {
        for (uint32_t off = LEVER_SCAN_START; off + 7u <= LEVER_SCAN_END; off++) {
            if (creator[off] != 0x83 || creator[off + 1] != 0x3D) continue;   /* cmp dword[rip+d],imm8 */
            if (creator[off + 6] != 0x00) continue;                           /* ... imm8 must be 0 */
            int32_t disp;
            memcpy(&disp, creator + off + 2, 4);
            const uint8_t *rip_next = creator + off + 7;
            const uint8_t *slot     = rip_next + (intptr_t)disp;
            /* sanity: a .bss global lives inside the mapped DOOM image, not in our own / wild memory */
            if (slot < g_module_base || slot > g_module_base + 0x10000000u) return NULL;
            return (volatile LONG *)slot;
        }
        return NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* Resolve + cache the lever slot once (idempotent). Returns the slot or NULL (caller refuses on NULL). */
static volatile LONG *ensure_lever(void)
{
    if (g_lever != NULL) return g_lever;
    {
        sig_result r;
        if (!linkany_resolve_sig(CREATOR_SIG_NAME, &r) || r.status != SIG_OK) {
            sh_printf("sh_target_any: connect-tool sig unresolved (status=%d) -- cannot toggle.\n",
                      (int)r.status);
            return NULL;
        }
        g_lever = decode_lever((const uint8_t *)r.addr);
        if (g_lever == NULL)
            sh_printf("sh_target_any: instance-filter lever decode FAILED (stale layout?) -- cannot toggle.\n");
        return g_lever;
    }
}

/* Write `val` into the lever (VirtualProtect RW -> store -> readback-verify -> restore protection).
 * Returns 1 iff the readback confirms the write. SEH-guarded; on any fault writes nothing partial. */
static int lever_write(volatile LONG *slot, LONG val)
{
    DWORD oldp = 0, tmp = 0;
    int   ok   = 0;
    if (!VirtualProtect((void *)slot, sizeof(LONG), PAGE_READWRITE, &oldp)) return 0;
    __try {
        *slot = val;
        ok = (*slot == val);   /* readback verify -- the real guard a mis-decode can't survive */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    VirtualProtect((void *)slot, sizeof(LONG), oldp, &tmp);   /* restore the page's prior protection */
    return ok;
}

/* B-injection: give every node-LESS SEDEF one synthetic output + input node so the native connect flow
 * treats it as connectable (-> a BLUE node-mediated wire). Best-effort: returns the # of fields injected
 * (0 = skipped, the A lever still applies). Reversible via linkany_restore_nodes (g_injected table). */
static int linkany_inject_nodes(void)
{
    sig_result r;
    if (!linkany_resolve_sig("GetDeclsOfType", &r) || r.status != SIG_OK) {
        sh_printf("sh_target_any: GetDeclsOfType unresolved -- node-injection skipped (lever still on).\n");
        return 0;
    }
    void *list = ((get_decls_fn)r.addr)(SEDEF_TYPE_NAME);   /* main-thread (Cbuf) call, like sh_unhide */
    void *array = NULL; uint32_t count = 0;
    if (list == NULL ||
        !ir_ptr((const uint8_t *)list + LIST_ARRAY_OFF, &array) ||
        !ir_u32((const uint8_t *)list + LIST_COUNT_OFF, &count)) {
        sh_printf("sh_target_any: SEDEF registry unreadable -- node-injection skipped.\n");
        return 0;
    }
    if (array == NULL || count == 0) {
        sh_printf("sh_target_any: SEDEF registry empty (editor not up?) -- node-injection skipped.\n");
        return 0;
    }
    if (count > SEDEF_COUNT_CAP) {
        sh_printf("sh_target_any: SEDEF count implausible -- node-injection skipped.\n");
        return 0;
    }

    /* Pass 1: capture a registered logic-relay node ptr for each side. Prefer the named relays
     * (logic_fired listener / logic/fire action); fall back to the FIRST node seen in any list (also
     * registered, also yields a blue wire). Captured FROM a live node list => cda140's registry lookup hits. */
    void *relay_out = NULL, *relay_in = NULL, *fb_out = NULL, *fb_in = NULL;
    for (uint32_t i = 0; i < count && (relay_out == NULL || relay_in == NULL); i++) {
        void *decl = NULL;
        if (!ir_ptr((const uint8_t *)array + (size_t)i * 8, &decl) || decl == NULL) continue;
        uint32_t oc = 0, ic = 0; void *op = NULL, *ip = NULL;
        ir_u32((const uint8_t *)decl + DECL_OUT_CNT_OFF, &oc); ir_ptr((const uint8_t *)decl + DECL_OUT_PTR_OFF, &op);
        ir_u32((const uint8_t *)decl + DECL_IN_CNT_OFF, &ic);  ir_ptr((const uint8_t *)decl + DECL_IN_PTR_OFF, &ip);
        if (relay_out == NULL && oc != 0 && op != NULL) {
            uint32_t lim = oc < NODELIST_SCAN_CAP ? oc : NODELIST_SCAN_CAP;
            for (uint32_t j = 0; j < lim; j++) {
                void *nd = NULL;
                if (!ir_ptr((const uint8_t *)op + (size_t)j * 8, &nd) || nd == NULL) continue;
                if (fb_out == NULL) fb_out = nd;
                { const char *nm = ir_name(nd); if (nm != NULL && strstr(nm, "logic_fired") != NULL) { relay_out = nd; break; } }
            }
        }
        if (relay_in == NULL && ic != 0 && ip != NULL) {
            uint32_t lim = ic < NODELIST_SCAN_CAP ? ic : NODELIST_SCAN_CAP;
            for (uint32_t j = 0; j < lim; j++) {
                void *nd = NULL;
                if (!ir_ptr((const uint8_t *)ip + (size_t)j * 8, &nd) || nd == NULL) continue;
                if (fb_in == NULL) fb_in = nd;
                { const char *nm = ir_name(nd); if (nm != NULL && strstr(nm, "logic/fire") != NULL) { relay_in = nd; break; } }
            }
        }
    }
    if (relay_out == NULL) relay_out = fb_out;
    if (relay_in  == NULL) relay_in  = fb_in;
    if (relay_out == NULL || relay_in == NULL) {
        sh_printf("sh_target_any: no registered logic relay node found -- node-injection skipped (lever still on).\n");
        return 0;
    }
    g_out_arr[0] = relay_out; g_in_arr[0] = relay_in;

    /* Pass 2: inject into every node-less side + record the original ptr for restore. */
    g_injected_n = 0;
    for (uint32_t i = 0; i < count; i++) {
        void *decl = NULL;
        if (!ir_ptr((const uint8_t *)array + (size_t)i * 8, &decl) || decl == NULL) continue;
        uint32_t oc = 0, ic = 0;
        if (ir_u32((const uint8_t *)decl + DECL_OUT_CNT_OFF, &oc) && oc == 0 && g_injected_n < MAX_INJECTED) {
            void *op = NULL; ir_ptr((const uint8_t *)decl + DECL_OUT_PTR_OFF, &op);
            if (iw_ptr((uint8_t *)decl + DECL_OUT_PTR_OFF, g_out_arr) && iw_u32((uint8_t *)decl + DECL_OUT_CNT_OFF, 1)) {
                g_injected[g_injected_n].decl = decl; g_injected[g_injected_n].is_input = 0;
                g_injected[g_injected_n].orig_ptr = op; g_injected_n++;
            }
        }
        if (ir_u32((const uint8_t *)decl + DECL_IN_CNT_OFF, &ic) && ic == 0 && g_injected_n < MAX_INJECTED) {
            void *ip = NULL; ir_ptr((const uint8_t *)decl + DECL_IN_PTR_OFF, &ip);
            if (iw_ptr((uint8_t *)decl + DECL_IN_PTR_OFF, g_in_arr) && iw_u32((uint8_t *)decl + DECL_IN_CNT_OFF, 1)) {
                g_injected[g_injected_n].decl = decl; g_injected[g_injected_n].is_input = 1;
                g_injected[g_injected_n].orig_ptr = ip; g_injected_n++;
            }
        }
    }
    {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: sh_target_any node-injection ON (%d fields on node-less decls of %u)",
                    g_injected_n, count);
        backend_log(line);
    }
    return g_injected_n;
}

/* Restore every field linkany_inject_nodes overwrote (count back to 0, ptr back to its original). */
static void linkany_restore_nodes(void)
{
    int n = g_injected_n;
    for (int i = 0; i < n; i++) {
        void *decl = g_injected[i].decl;
        if (g_injected[i].is_input) {
            iw_ptr((uint8_t *)decl + DECL_IN_PTR_OFF, g_injected[i].orig_ptr);
            iw_u32((uint8_t *)decl + DECL_IN_CNT_OFF, 0);
        } else {
            iw_ptr((uint8_t *)decl + DECL_OUT_PTR_OFF, g_injected[i].orig_ptr);
            iw_u32((uint8_t *)decl + DECL_OUT_CNT_OFF, 0);
        }
    }
    g_injected_n = 0;
    g_out_arr[0] = g_in_arr[0] = NULL;
    if (n > 0) {
        char line[96];
        _snprintf_s(line, sizeof line, _TRUNCATE, "B2: sh_target_any node-injection OFF (restored %d fields)", n);
        backend_log(line);
    }
}

void h_link_any(struct idCmdArgs *a)
{
    (void)a;
    volatile LONG *lever = ensure_lever();
    if (lever == NULL) return;   /* ensure_lever already explained the refusal */

    if (!g_on) {
        if (!lever_write(lever, 1)) {
            sh_printf("sh_target_any: lever write/readback FAILED -- mode NOT enabled.\n");
            return;
        }
        g_on = 1;
        int injected = linkany_inject_nodes();   /* B: node injection (best-effort; A lever already on) */
        sh_printf("sh_target_any: LINK-ANY mode ON -- the Add-Logic tool now accepts ANY entity as a "
                  "connection target (native node-mediated, BLUE/logic wires). %d node-less decl fields "
                  "given a logic relay so even node-less entities are connectable (a wire to a truly "
                  "node-less target is valid but likely inert on it). Run sh_target_any again to turn off.\n",
                  injected);
        backend_log("B2: sh_target_any link-any ON (instance-filter lever = 1)");
    } else {
        linkany_restore_nodes();   /* B: restore the decls BEFORE clearing the lever */
        if (!lever_write(lever, 0)) {
            sh_printf("sh_target_any: lever write/readback FAILED -- node-injection restored, lever may still be ON.\n");
            return;
        }
        g_on = 0;
        sh_printf("sh_target_any: LINK-ANY mode OFF -- normal connection-target filtering restored "
                  "(instance filter + node-injection reverted).\n");
        backend_log("B2: sh_target_any link-any OFF (instance-filter lever = 0)");
    }
}

void sh_linkany_install(const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return;   /* one-shot */
    g_module_base = module_base;   /* h_link_any resolves the creator sig + decodes the lever at FIRE */
    backend_log("B2: sh_target_any link-any ready (off by default; instance-filter lever resolves at FIRE)");
}
