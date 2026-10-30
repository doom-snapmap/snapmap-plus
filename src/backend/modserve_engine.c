/* modserve_engine.c -- the engine side of modserve.h: decl registration
 * through the registry, the existence probe, the palette points, per-map
 * activation and the serving verbs of sh_mods (status, rescan, palette
 * rebuild), which modcmd.c dispatches here.
 *
 * Engine surface (build 13954591), bound as timeline_tile.c binds it:
 *   IdStrCtor/IdStrDtor (48-byte idStr), DeclRegistryAnchor (+0x10 MOV RCX,[rip]),
 *   DeclTypeByName (registry vtable [11]), DeclRegisterFile (registry vtable [7]),
 *   DeclSourceFind, DeclFind(manager, name, create), ResourceGenericLoad;
 *   idResource +0x28 lifetime, +0x2c state (bit 0 failed/default, bit 1 unloaded);
 *   sedef +0x08 name, +0x1c8 resolved entityDef; palette +0x10 entries, +0x18 count,
 *   stride 0x6f0.
 *   DeclResourceProbe: bool(self, path, out1, out2, out3, out4, quiet) at decl
 *   manager vtable +0x78, detoured over its 21-byte head (13 bytes of pushes
 *   plus LEA RBP,[RSP-0x88]; no relative operand). All seven arguments are
 *   forwarded; for a path a bundle serves the engine is asked quietly, since
 *   its own miss is fatal with quiet 0 (S1 consequence 9).
 * The registration sequence is the one lane R2 ran live (R-001): source
 * registration with a native idStr, a true probe answer, bytes through the
 * provider, DeclFind, then the generic load once for a new identity.
 *
 * Bundle requirements (the cut-content gates) are applied before every pass,
 * by me_run, through idCVar::Set(cvar, "0", force 1) (CvarSetString, VK
 * 0x140295780 / GL 0x1402959a0), the setter map_bounds.c already uses. That
 * route is immediate, so it needs no drain: Set parses the text into the int
 * at idCVar+0x30 before returning, and both blacklist matchers read exactly
 * that int on 13954591 (VK: resource matcher 0x140395150 reads the
 * g_useResourceBlackList object 0x145b13bd0 +0x30, the image matcher at
 * 0x140394fc0 reads g_useImageBlackList 0x145b13a90 +0x30; Ghidra jobs in
 * snaphak-re logs/shared-ghidra/mod-system-reinstatement/laneRQ). The retired
 * route buffered "<cvar> 0" console text, which stays queued until the
 * command buffer runs, and so needed CmdExecuteBuffer between the disarm and
 * the registration; modserve.c keeps that two-phase shape for any queued set,
 * and verifies every gate by reading +0x30 back before registering.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "modserve.h"
#include "overrides.h"
#include "signatures.h"
#include "patch.h"
#include "backend_log.h"
#include "process_heap_scope.h"
#include "engine_globals.h"
#include "commands.h"
#include "timeline_tile.h"
#include "apply_engine.h"
#include "engine_build.h"
#include "engine_cvar_read.h"

#define RES_LIFETIME_OFF    0x28
#define RES_STATE_OFF       0x2c
#define SEDEF_NAME_OFF      0x08
#define SEDEF_ENTITYDEF_OFF 0x1c8
#define PAL_ENTRIES_OFF     0x10
#define PAL_COUNT_OFF       0x18
#define PAL_STRIDE          0x6f0
#define PAL_MAX             100000
#define PROBE_STOLEN        21
#define ERROR_TEXT_MAX      200

typedef void *(*str_ctor_fn)(void *, const char *);
typedef void (*str_dtor_fn)(void *);
typedef void *(*type_fn)(void *, const char *);
typedef unsigned char (*register_fn)(void *, const void *, void *);
typedef void *(*source_fn)(void *, const char *);
typedef void *(*find_decl_fn)(void *, const char *, unsigned char);
typedef void (*generic_load_fn)(void *);
typedef unsigned char (*cvar_set_fn)(void *, const char *, unsigned char);
typedef unsigned char (*probe_fn)(void *, const char *, void *, void *, void *, void *, uint64_t);
typedef union ms_string { void *align; unsigned char bytes[48]; } ms_string;

static str_ctor_fn g_ctor;
static str_dtor_fn g_dtor;
static const unsigned char *g_anchor;
static type_fn g_type;
static register_fn g_register;
static source_fn g_source;
static find_decl_fn g_decl;
static generic_load_fn g_generic_load;
static sh_process_heap_api g_heap;
static int g_bound;
static const char *g_last_error;
static const uint8_t *g_main_thread_at;
static probe_fn g_probe_orig;
static sig_result g_probe_sig;
static int g_probe_sig_ok;
static volatile LONG g_editor_pass_done;
static DWORD g_fault_code;
static cvar_set_fn g_cvar_set;
static const void *g_cvar_slot;

extern volatile LONG g_modserve_editor_ok, g_modserve_editor_failed;   /* modserve.c */

static void me_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    memcpy(line, "MODS: ", 6);
    va_start(ap, fmt);
    _vsnprintf_s(line + 6, sizeof line - 6, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static double me_now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* 1 main thread, 0 another thread, -1 unknown. */
static int me_on_main(void)
{
    uint32_t id = 0;
    if (!g_main_thread_at) return -1;
    __try { id = *(const uint32_t *)g_main_thread_at; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!id) return -1;
    return id == GetCurrentThreadId() ? 1 : 0;
}

static void me_error_text(char *out, size_t cap)
{
    size_t n;
    out[0] = 0;
    if (!g_last_error) return;
    __try { strncpy_s(out, cap, g_last_error, _TRUNCATE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
    n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
}

static int me_fault_filter(DWORD code) { g_fault_code = code; return EXCEPTION_EXECUTE_HANDLER; }

static void me_log_fault(const char *what, const char *type, const char *name, const char *before)
{
    char after[ERROR_TEXT_MAX];
    me_error_text(after, sizeof after);
    me_log("%s %s '%s' FAULTED (exception 0x%08lx)%s%s", what, type, name, (unsigned long)g_fault_code,
           (after[0] && strcmp(after, before)) ? "; engine error: " : "",
           (after[0] && strcmp(after, before)) ? after : "");
}

/* ------------------------------------------------------------------ registry */

static void *me_registry(void)
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

static void *me_manager(const char *type)
{
    void *r = me_registry();
    return r ? g_type(r, type) : NULL;
}

static int op_source_registered(void *context, const char *type, const char *name)
{
    char before[ERROR_TEXT_MAX];
    int found = -1;
    (void)context;
    me_error_text(before, sizeof before);
    __try {
        void *m = me_manager(type);
        found = m ? (g_source(m, name) != NULL) : -1;
    } __except (me_fault_filter(GetExceptionCode())) { me_log_fault("source lookup", type, name, before); found = -1; }
    return found;
}

static int op_register_file(void *context, const char *type, const char *name, const char *reg_path)
{
    char before[ERROR_TEXT_MAX];
    int ok = 0;
    (void)context;
    me_error_text(before, sizeof before);
    __try {
        void *r = me_registry(), *m = r ? g_type(r, type) : NULL;
        if (m) {
            ms_string s;
            unsigned char registered;
            g_ctor(&s, reg_path);
            registered = g_register(r, &s, NULL);
            g_dtor(&s);
            ok = registered && g_source(m, name) != NULL;
        }
    } __except (me_fault_filter(GetExceptionCode())) { me_log_fault("registration", type, name, before); ok = 0; }
    return ok;
}

static int op_load(void *context, const char *type, const char *name, int persist,
                   unsigned *out_state, unsigned *out_lifetime)
{
    char before[ERROR_TEXT_MAX];
    int ok = 0;
    (void)context;
    *out_state = *out_lifetime = 0;
    me_error_text(before, sizeof before);
    __try {
        void *m = me_manager(type), *decl = NULL;
        if (m) {
            decl = g_decl(m, name, 0);
            if (!decl) decl = g_decl(m, name, 1);
        }
        if (decl) {
            unsigned char *d = (unsigned char *)decl;
            unsigned char state = d[RES_STATE_OFF];
            unsigned lifetime = *(unsigned *)(d + RES_LIFETIME_OFF);
            if (!(state & 1)) {
                /* A new identity with no consumers takes the native source load
                 * once, at the persistent level, as timeline_tile.c does. */
                *(unsigned *)(d + RES_LIFETIME_OFF) = 4;
                d[RES_STATE_OFF] = (unsigned char)(state & ~2);
                g_generic_load(decl);
                state = d[RES_STATE_OFF];
            }
            /* Editor rows stay through map purges; everything else returns to
             * the engine's own level (1), which the next map unload purges. */
            *(unsigned *)(d + RES_LIFETIME_OFF) = persist ? 4u : lifetime;
            *out_state = state;
            *out_lifetime = *(unsigned *)(d + RES_LIFETIME_OFF);
            ok = (state & 3) == 0;
            if (ok && !_stricmp(type, "snapEditorEntityDef") && !*(void **)(d + SEDEF_ENTITYDEF_OFF)) {
                me_log("sedef '%s' resolved no entityDef (sedef+0x1c8 is NULL); palette admission would reject it", name);
                ok = 0;
            }
        }
    } __except (me_fault_filter(GetExceptionCode())) { me_log_fault("load", type, name, before); ok = 0; }
    return ok;
}

/* The player's game archives, read through the engine provider (the same
 * reader the builder's stock check uses). */
static int op_stock_exists(void *context, const char *request)
{
    unsigned char *body = NULL;
    size_t len = 0;
    (void)context;
    __try {
        body = sh_overrides_read_engine_resource(request, &len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (!body) return 0;
    HeapFree(GetProcessHeap(), 0, body);
    return 1;
}

static const sh_modserve_engine_ops g_ops = { NULL, op_source_registered, op_register_file, op_load, op_stock_exists };

/* ------------------------------------------------------------------ gates */

static void *me_gate_cvar(const char *name)
{
    return sh_engine_cvar_find(g_cvar_slot, SH_IDCVAR_NAME_FOR_LOOKUP, name);
}

static int gate_read(void *context, const char *cvar, int *value)
{
    (void)context;
    return sh_engine_cvar_read_int(me_gate_cvar(cvar), value);
}

/* Immediate: the value is in idCVar+0x30 when Set returns. */
static int gate_set(void *context, const char *cvar, const char *value)
{
    void *c = me_gate_cvar(cvar);
    unsigned char ok = 0;
    (void)context;
    if (!c || !g_cvar_set) return 0;
    __try { ok = g_cvar_set(c, value, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok ? SH_MODSERVE_GATE_SET : 0;
}

static const sh_modserve_gate_ops g_gate_ops = { NULL, gate_read, gate_set, NULL };

/* Run a plan inside process heap 0: decl objects outlive every map. The
 * bundles' requirements are applied and read back first, outside the scope
 * (as map_bounds.c sets its cvars), and gate-blocked steps are marked. */
static int me_run(sh_modserve_plan *plan, const char *phase)
{
    sh_process_heap_scope heap = { 0 };
    sh_modserve_gates gates;
    int ready = -1;
    if (!g_bound) { me_log("%s: decl registry unavailable on this build", phase); return -1; }
    if (plan->count) {
        sh_modserve_requirements_apply(g_cvar_set && g_cvar_slot ? &g_gate_ops : NULL, phase, &gates);
        sh_modserve_plan_block_unmet(plan, &gates);
    }
    if (!sh_process_heap_enter(&g_heap, &heap)) { me_log("%s: process heap scope unavailable; nothing registered", phase); return -1; }
    __try { ready = sh_modserve_plan_run(plan, &g_ops, phase); }
    __finally {
        if (!sh_process_heap_leave(&heap)) { me_log("%s: process heap scope did not balance", phase); ready = -1; }
    }
    return ready;
}

void sh_modserve_play_begin(void)
{
    int on_main;
    if (!sh_modserve_requirements()) return;
    on_main = me_on_main();
    if (on_main != 1) {
        me_log("play: requirements not re-checked (%s)", on_main ? "the main thread is unknown" : "not on the main thread");
        return;
    }
    sh_modserve_requirements_check(g_cvar_set && g_cvar_slot ? &g_gate_ops : NULL, "play", NULL);
}

/* ------------------------------------------------------------------ probe */

static unsigned char me_probe_hook(void *self, const char *path, void *a, void *b, void *c, void *d, uint64_t quiet)
{
    int ours = 0;
    unsigned char r;
    if (path && sh_modserve_active()) ours = sh_modserve_probe_claims(path);
    r = g_probe_orig(self, path, a, b, c, d, ours ? ((quiet & ~(uint64_t)0xff) | 1u) : quiet);
    return r ? r : (unsigned char)(ours ? 1 : 0);
}

/* The probe is detoured only once the index holds a decl, so an empty mods
 * folder leaves the engine's probe untouched. */
static void me_ensure_probe(void)
{
    void *tramp;
    sh_modserve_stats st;
    if (g_probe_orig || !g_probe_sig_ok) return;
    sh_modserve_get_stats(&st);
    if (!st.files) return;
    tramp = sh_prepare_detour_sig(&g_probe_sig, (void *)me_probe_hook, PROBE_STOLEN);
    if (!tramp) { me_log("existence probe detour refused; new decl identities cannot load"); g_probe_sig_ok = 0; return; }
    g_probe_orig = (probe_fn)tramp;
    if (sh_commit_detour(tramp) != B2_PATCH_OK) {
        if (sh_uninstall_detour(tramp)) g_probe_orig = NULL;
        g_probe_sig_ok = 0;
        me_log("existence probe detour commit failed; new decl identities cannot load");
        return;
    }
    me_log("existence probe hook installed (rva 0x%X, %d-byte window)", g_probe_sig.rva, PROBE_STOLEN);
}

/* ------------------------------------------------------------------ install */

static void *me_clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (void *)r[i].addr;
    return NULL;
}

int sh_modserve_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    size_t i;
    g_ctor = (str_ctor_fn)me_clean(results, count, "IdStrCtor");
    g_dtor = (str_dtor_fn)me_clean(results, count, "IdStrDtor");
    g_anchor = (const unsigned char *)me_clean(results, count, "DeclRegistryAnchor");
    g_type = (type_fn)me_clean(results, count, "DeclTypeByName");
    g_register = (register_fn)me_clean(results, count, "DeclRegisterFile");
    g_source = (source_fn)me_clean(results, count, "DeclSourceFind");
    g_decl = (find_decl_fn)me_clean(results, count, "DeclFind");
    g_generic_load = (generic_load_fn)me_clean(results, count, "ResourceGenericLoad");
    /* The requirement gates: optional, so a build without them still
     * registers every bundle that needs none. */
    g_cvar_set = (cvar_set_fn)me_clean(results, count, "CvarSetString");
    g_cvar_slot = (const void *)glb_resolve(module_base, "cvar_system_slot", NULL);
    g_last_error = (const char *)glb_resolve(module_base, "last_error_msg", NULL);
    g_main_thread_at = (const uint8_t *)glb_resolve(module_base, "main_thread_id", NULL);
    g_bound = g_ctor && g_dtor && g_anchor && g_type && g_register && g_source && g_decl && g_generic_load &&
              sh_process_heap_bind(&g_heap, results, count, module_base) && g_main_thread_at;
    for (i = 0; i < count; ++i)
        if (results[i].name && !strcmp(results[i].name, "DeclResourceProbe") && results[i].status == SIG_OK) {
            g_probe_sig = results[i];
            g_probe_sig_ok = 1;
        }
    if (!g_bound) me_log("decl registration unavailable on this build (registry, heap scope or main thread unresolved)");
    if (!g_probe_sig_ok) me_log("existence probe unavailable (DeclResourceProbe is not a clean match)");
    if (!g_cvar_set || !g_cvar_slot)
        me_log("bundle requirements unavailable (%s unresolved); bundles that need a blacklist gate are blocked",
               !g_cvar_set ? "CvarSetString" : "the cvar system");
    if (g_bound) me_ensure_probe();
    return g_bound;
}

/* ------------------------------------------------------------------ palette */

void sh_modserve_palette_before_build(void)
{
    sh_modserve_plan plan;
    double t0;
    int ready, i, rows_ok = 0, rows_failed = 0, first = !InterlockedExchange(&g_editor_pass_done, 1);
    if (!g_bound || !sh_modserve_active()) return;
    me_ensure_probe();
    t0 = me_now_ms();
    if (sh_modserve_plan_editor(&plan) < 0) { me_log("editor rows: plan refused (out of memory)"); return; }
    if (!plan.count) { sh_modserve_plan_free(&plan); if (first) me_log("editor rows registered in 0.0 ms (0 rows)"); return; }
    ready = me_run(&plan, "editor");
    for (i = 0; i < plan.count; ++i) {
        if (plan.steps[i].rank != 3) continue;
        if (plan.steps[i].ready == 1) rows_ok++;
        else rows_failed++;
    }
    InterlockedExchange(&g_modserve_editor_ok, rows_ok);
    InterlockedExchange(&g_modserve_editor_failed, rows_failed);
    me_log("editor rows registered in %.1f ms (%d rows; %d of %d steps ready, %d blocked)", me_now_ms() - t0,
           rows_ok, ready < 0 ? 0 : ready, plan.count, plan.blocked);
    sh_modserve_plan_free(&plan);
}

static int me_rd_ptr(const void *p, void **v) { __try { *v = *(void *const *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int me_rd_i32(const void *p, int *v) { __try { *v = *(const int *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

static int me_count_mod_rows(void *palette, int *rows)
{
    void *entries = NULL;
    int count = 0, i, mods = 0;
    *rows = -1;
    if (!palette || !me_rd_ptr((unsigned char *)palette + PAL_ENTRIES_OFF, &entries) ||
        !me_rd_i32((unsigned char *)palette + PAL_COUNT_OFF, &count)) return -1;
    *rows = count;
    if (!entries || count <= 0 || count > PAL_MAX) return 0;
    for (i = 0; i < count; ++i) {
        void *sedef = NULL, *name = NULL;
        const unsigned char *entry = (const unsigned char *)entries + (size_t)i * PAL_STRIDE;
        int is_mod = 0;
        if (!me_rd_ptr(entry, &sedef) || !sedef || !me_rd_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &name) || !name)
            continue;
        __try { is_mod = sh_modserve_is_mod_sedef((const char *)name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { is_mod = 0; }
        mods += is_mod;
    }
    return mods;
}

void sh_modserve_palette_after_build(void *palette)
{
    int rows, mods;
    if (!sh_modserve_active()) return;
    mods = me_count_mod_rows(palette, &rows);
    me_log("palette built: %d rows, %d from installed bundles", rows, mods);
}

int modserve_palette_rebuild_requested(void)
{
    int before = -1, after = -1, r, main = me_on_main();
    double t0;
    if (main != 1) {
        me_log("palette rebuild refused: %s", main ? "the main thread is unknown" : "not on the main thread");
        return 0;
    }
    /* The module palette (editor+0x206c0, FUN_141213220) is built against the
     * entity palette and is NOT re-run here: whether it keeps entity-row
     * pointers across this rebuild is an open trace (lane R3 section 3.6). */
    t0 = me_now_ms();
    r = sh_timeline_tile_rebuild_palette(&before, &after);
    switch (r) {
    case SH_TL_REBUILD_OK:
        me_log("palette rebuilt in %.1f ms: %d rows before, %d after", me_now_ms() - t0, before, after);
        return 1;
    case SH_TL_REBUILD_BUSY:
        me_log("palette rebuild refused: the editor holds an entity or a wire (%d rows)", before);
        return 0;
    case SH_TL_REBUILD_HEAP:
        me_log("palette rebuild refused: process heap 0 could not be entered (%d rows)", before);
        return 0;
    case SH_TL_REBUILD_DISABLED:
        me_log("palette rebuild refused: disabled after an unbalanced heap scope earlier this session");
        return 0;
    case SH_TL_REBUILD_FAULTED:
        me_log("palette rebuild FAULTED inside the builder: %d rows before, %d after", before, after);
        return 0;
    case SH_TL_REBUILD_HEAP_LEAVE:
        me_log("palette rebuild ran but the heap scope did not balance; rebuilds are disabled (%d -> %d rows)", before, after);
        return 0;
    default:
        me_log("palette rebuild unavailable: no editor seen yet or the palette build hook is not installed");
        return 0;
    }
}

/* ------------------------------------------------------------------ per-map */

static const char *me_rebuild_reason(int r)
{
    switch (r) {
    case SH_TL_REBUILD_BUSY: return "the editor holds an entity or a wire";
    case SH_TL_REBUILD_HEAP: return "process heap 0 could not be entered";
    case SH_TL_REBUILD_DISABLED: return "disabled after an unbalanced heap scope earlier this session";
    case SH_TL_REBUILD_FAULTED: return "the builder FAULTED";
    case SH_TL_REBUILD_HEAP_LEAVE: return "the heap scope did not balance; rebuilds are now disabled";
    default: return "no palette build hook";
    }
}

static int pal_ready(void *context)
{
    int rows = -1;
    (void)context;
    return me_count_mod_rows(sh_timeline_tile_palette(), &rows) >= 0 && rows > 0;
}

/* Linear over the rows (about 1,300 on 13954591): the array is sorted by the
 * synthesized record's inherit name, not by sedef name. */
static int pal_has(void *context, const char *sedef_name)
{
    unsigned char *palette = (unsigned char *)sh_timeline_tile_palette();
    void *entries = NULL;
    int count = 0, i, found = 0;
    (void)context;
    if (!palette || !sedef_name || !me_rd_ptr(palette + PAL_ENTRIES_OFF, &entries) ||
        !me_rd_i32(palette + PAL_COUNT_OFF, &count) || !entries || count <= 0 || count > PAL_MAX) return 0;
    for (i = 0; i < count && !found; ++i) {
        void *sedef = NULL, *name = NULL;
        const unsigned char *entry = (const unsigned char *)entries + (size_t)i * PAL_STRIDE;
        if (!me_rd_ptr(entry, &sedef) || !sedef || !me_rd_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &name) || !name)
            continue;
        __try { found = !_stricmp((const char *)name, sedef_name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { found = 0; }
    }
    return found;
}

static int pal_rebuild(void *context, int *before, int *after)
{
    (void)context;
    return sh_timeline_tile_rebuild_palette(before, after);
}

/* Inside the load gate, after activation and before the map parses: rows the
 * built palette lacks are registered by the rebuild's editor pass, so the
 * validator's palette lookup finds them. One call per activation. */
static void me_sync_rows(const char *const *bundle_hashes, int n)
{
    static const sh_modserve_palette_ops ops = { NULL, pal_ready, pal_has, pal_rebuild };
    sh_modserve_rows_sync res;
    double t0 = me_now_ms();
    int r = sh_modserve_map_rows_sync(bundle_hashes, n, &ops, &res);
    if (r < 0) { me_log("map rows: check refused (out of memory)"); return; }
    if (!r) return;
    if (res.rebuild == SH_TL_REBUILD_OK)
        me_log("map rows: %d of %d editor row(s) missing from the palette; %d registered, %d now in the palette; "
               "palette rebuilt in %.1f ms (%d -> %d rows)", res.missing, res.rows, res.registered, res.admitted,
               me_now_ms() - t0, res.rows_before, res.rows_after);
    else
        me_log("map rows: %d of %d editor row(s) missing from the palette; rebuild refused after %.1f ms: %s "
               "(%d rows); the map's placed rows will not validate", res.missing, res.rows, me_now_ms() - t0,
               me_rebuild_reason(res.rebuild), res.rows_before);
}

int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    sh_modserve_plan plan;
    int unknown = 0, ready, main = me_on_main();
    double t0;
    /* A map without manifest rows: nothing to do, nothing logged. */
    if (n <= 0 || !bundle_hashes) return 0;
    if (main != 1) { me_log("activation refused: not on the main thread"); return -1; }
    if (!g_bound) { me_log("activation refused: decl registry unavailable"); return -1; }
    me_ensure_probe();
    t0 = me_now_ms();
    if (sh_modserve_plan_bundles(bundle_hashes, n, &plan, &unknown) < 0) { me_log("activation refused: out of memory"); return -1; }
    ready = plan.count ? me_run(&plan, "map") : 0;
    me_log("activated %d bundles (%d steps, %d ready, %d blocked, %d not installed) in %.1f ms", n - unknown,
           plan.count, ready < 0 ? 0 : ready, plan.blocked, unknown, me_now_ms() - t0);
    sh_modserve_plan_free(&plan);
    if (ready >= 0) me_sync_rows(bundle_hashes, n);
    return ready;
}

/* ------------------------------------------------------------------ console */

static void me_print_bundle(void *context, const char *id, const char *version, const char *hash,
                            int admitted, const char *cause, int files, int legacy)
{
    (void)context;
    sh_printf("  %s%s@%s  %.12s  %s%s%s  %d files\n", legacy ? "legacy " : "", id, version, hash,
              admitted ? "active" : "SKIPPED", admitted ? "" : ": ", admitted ? "" : cause, files);
}

static int me_rebuild_main(void *context) { (void)context; return modserve_palette_rebuild_requested(); }

int sh_modserve_command(void *args, const char *verb)
{
    idCmdArgs *a = (idCmdArgs *)args;
    const char *arg = cmd_argv(a, 2);
    if (!verb || !_stricmp(verb, "status")) {
        sh_modserve_stats st;
        sh_modserve_get_stats(&st);
        sh_printf("sh_mods: %d of %d bundles active, %d files (%d paths), %d shadows, %d conflicts (%d composed); index %.1f ms\n",
                  st.bundles_admitted, st.bundles_seen, st.files, st.keys, st.shadows, st.conflicts,
                  st.conflicts_composed, st.build_ms);
        sh_printf("sh_mods: served %lu opens (%lu cache hits, %lu read failures), %lu probe answers; "
                  "editor rows %d ready, %d failed; registry %s, probe %s\n",
                  st.opens_served, st.cache_hits, st.open_failures, st.probe_claims, st.editor_rows_registered,
                  st.editor_rows_failed, g_bound ? "bound" : "unavailable", g_probe_orig ? "hooked" : "not hooked");
        sh_modserve_each_bundle(me_print_bundle, NULL);
        return 1;
    }
    if (!_stricmp(verb, "rescan")) {
        int n = sh_modserve_index_rescan();
        me_ensure_probe();
        sh_printf("sh_mods: rescan %s (%d bundles active); new editor rows need 'sh_mods palette rebuild'\n",
                  n < 0 ? "refused" : "done", n < 0 ? 0 : n);
        return 1;
    }
    if (!_stricmp(verb, "palette")) {
        sh_run_on_main_fn run = NULL;
        int result = 0;
        if (!arg || _stricmp(arg, "rebuild")) { sh_printf("usage: sh_mods palette rebuild\n"); return 1; }
        if (me_on_main() == 1) result = modserve_palette_rebuild_requested();
        else {
            sh_apply_engine_get_run_on_main(&run);
            if (!run || run(NULL, me_rebuild_main, NULL, 3000, &result) != 1) {
                sh_printf("sh_mods: palette rebuild did not start on the main thread\n");
                return 1;
            }
        }
        sh_printf("sh_mods: palette rebuild %s (see the MODS: line in the log)\n", result ? "done" : "refused");
        return 1;
    }
    return 0;
}
