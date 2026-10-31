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
 * Bundle requirements are applied before every pass, by me_run. A cvar is
 * set through idCVar::Set(cvar, text, force 1) (CvarSetString, VK
 * 0x140295780 / GL 0x1402959a0), the setter map_bounds.c already uses. Force
 * skips the engine's developer check (the product's cvar unlock lifts it for
 * the console too) and its read-only and command-line-only checks, so those
 * two are made here from the cvar's flags (idCVar+0x68: 0x8000 read-only,
 * 0x4000 command line only), and such a cvar is reported refused. That route
 * is immediate, so it needs no drain: Set assigns the value string (its data
 * pointer at idCVar+0x10) and parses it into the int at +0x30 before
 * returning. The blacklist matchers read exactly that int on 13954591 (VK:
 * resource matcher 0x140395150 reads the g_useResourceBlackList object
 * 0x145b13bd0 +0x30, the image matcher at 0x140394fc0 reads
 * g_useImageBlackList 0x145b13a90 +0x30; Ghidra jobs in snaphak-re
 * logs/shared-ghidra/mod-system-reinstatement/laneRQ; both register with
 * flags 1, a plain bool), so a read-back reports the int whenever the string
 * is an integer it disagrees with. The retired route buffered console text,
 * which stays queued until the command buffer runs; modserve.c keeps that
 * two-phase shape for any queued set, and verifies every cvar by reading it
 * back before registering. A requirement's console lines are appended with
 * BufferCommandText (the engine runs them at its next command-buffer pass),
 * on the main thread like every pass.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "modserve.h"
#include "modbundle.h"
#include "overrides.h"
#include "modstock.h"
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
#include "modvt.h"
#include "modsound.h"
#include "strids.h"
#include "editor_delete.h"

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
typedef void (*buffer_cmd_fn)(void *, const char *);
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
static const void *g_res_head;        /* decl_resource_head: the resource list-of-lists (watermark) */
static probe_fn g_probe_orig;
static sig_result g_probe_sig;
static int g_probe_sig_ok;
static volatile LONG g_editor_pass_done;
static DWORD g_fault_code;
static cvar_set_fn g_cvar_set;
static const void *g_cvar_slot;
static buffer_cmd_fn g_buffer_cmd;
static void *g_cmdsys;
static const unsigned char *g_editor;  /* editor_singleton: idSnapEditorLocal, constructed in place */

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
 * reader the builder's stock check uses), then the installed archives
 * themselves (modstock.c), which also hold campaign-only content. */
static int op_stock_exists(void *context, const char *request)
{
    unsigned char *body = NULL;
    size_t len = 0;
    (void)context;
    __try {
        body = sh_overrides_read_engine_resource(request, &len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return sh_modstock_exists(request);
    }
    if (!body) return sh_modstock_exists(request) == 1 ? 1 : 0;
    HeapFree(GetProcessHeap(), 0, body);
    return 1;
}

static void op_sources_published(void *context);
static const char *g_reparse_phase = "plan";   /* the running pass, for the early-decl lines */
static const sh_modserve_engine_ops g_ops = { NULL, op_source_registered, op_register_file, op_load, op_stock_exists,
                                              op_sources_published };

/* ------------------------------------------------------------------ gates */

static void *me_gate_cvar(const char *name)
{
    return sh_engine_cvar_find(g_cvar_slot, SH_IDCVAR_NAME_FOR_LOOKUP, name);
}

#define ME_CVAR_VALUE_STR  0x10u      /* idCVar value-string data pointer */
#define ME_CVAR_READ_ONLY  0x8000u    /* flags bit 15 */
#define ME_CVAR_INIT_ONLY  0x4000u    /* flags bit 14: command line only */

static int me_plain_int(const char *s)
{
    if (*s == '-') ++s;
    if (!*s) return 0;
    for (; *s; ++s) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* The cvar's value text; the int the engine parsed from it when the text is
 * an integer they disagree on (the int is what bool and int readers use). */
static int gate_read(void *context, const char *cvar, char *value, size_t cap)
{
    const unsigned char *c = (const unsigned char *)me_gate_cvar(cvar);
    int n = 0, ok = 0;
    (void)context;
    if (!c || !value || cap < 16) return 0;
    __try {
        const char *s = *(const char *const *)(c + ME_CVAR_VALUE_STR);
        size_t i = 0;
        if (s) {
            for (; i + 1 < cap && s[i]; ++i) value[i] = s[i];
            value[i] = 0;
            ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!sh_engine_cvar_read_int(c, &n)) return 0;
    if (!ok || (me_plain_int(value) && atoi(value) != n)) _snprintf_s(value, cap, _TRUNCATE, "%d", n);
    return 1;
}

/* Immediate: the value is in the cvar when Set returns. */
static int gate_set(void *context, const char *cvar, const char *value)
{
    unsigned char *c = (unsigned char *)me_gate_cvar(cvar);
    unsigned flags = 0;
    unsigned char ok = 0;
    (void)context;
    if (!c || !g_cvar_set) return 0;
    __try { flags = *(const unsigned *)(c + SH_IDCVAR_FLAGS); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (flags & ME_CVAR_READ_ONLY) return SH_MODSERVE_GATE_READ_ONLY;
    if (flags & ME_CVAR_INIT_ONLY) return SH_MODSERVE_GATE_INIT_ONLY;
    __try { ok = g_cvar_set(c, value, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok ? SH_MODSERVE_GATE_SET : 0;
}

/* One console line, for the engine's next command-buffer pass. */
static int gate_command(void *context, const char *line)
{
    char text[SH_MB_REQ_LINE_MAX + 2];
    (void)context;
    if (!g_buffer_cmd || !g_cmdsys || !line) return 0;
    _snprintf_s(text, sizeof text, _TRUNCATE, "%s\n", line);
    __try { g_buffer_cmd(g_cmdsys, text); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

static const sh_modserve_gate_ops g_gate_ops = { NULL, gate_read, gate_set, NULL, gate_command };
/* The cvar system unresolved: no cvar is set, the lines still run. */
static const sh_modserve_gate_ops g_lines_ops = { NULL, NULL, NULL, NULL, gate_command };

static const sh_modserve_gate_ops *me_gate_ops(void)
{
    return g_cvar_set && g_cvar_slot ? &g_gate_ops : &g_lines_ops;
}

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
        sh_modserve_requirements_apply(me_gate_ops(), phase, &gates);
        sh_modserve_plan_block_unmet(plan, &gates);
    }
    if (!sh_process_heap_enter(&g_heap, &heap)) { me_log("%s: process heap scope unavailable; nothing registered", phase); return -1; }
    g_reparse_phase = phase;
    __try { ready = sh_modserve_plan_run(plan, &g_ops, phase); }
    __finally {
        if (!sh_process_heap_leave(&heap)) { me_log("%s: process heap scope did not balance", phase); ready = -1; }
    }
    return ready;
}

void sh_modserve_play_begin(void)
{
    int on_main;
    on_main = me_on_main();
    /* An uninstalled mod the map being played still places comes back first;
     * the game-side refresh waits for the return to the editor. */
    if (on_main == 1) (void)sh_modserve_restore(NULL, 0, NULL, 0, SH_MODSERVE_RESTORE_PLAY);
    if (!sh_modserve_requirements_held()) return;
    if (on_main != 1) {
        me_log("play: requirements not re-checked (%s)", on_main ? "the main thread is unknown" : "not on the main thread");
        return;
    }
    sh_modserve_requirements_check(me_gate_ops(), "play", NULL);
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

/* Bundle strings (strids.c) follow the published index. A table published
 * on the main thread (sh_mods rescan) refreshes them at once; one published
 * elsewhere (an install job, the Mods tab) asks the main thread through the
 * apply call slot, and when the slot is busy or the frame does not run the
 * request stays pending for the next editor or map pass below. */
static int me_strings_main(void *context)
{
    (void)context;
    return sh_strids_refresh_pending() != 0;
}

static void me_index_changed(void)
{
    sh_run_on_main_fn run = NULL;
    int result = 0;
    sh_strids_request_refresh();
    if (me_on_main() == 1) { (void)sh_strids_refresh_pending(); return; }
    sh_apply_engine_get_run_on_main(&run);
    if (run) (void)run(NULL, me_strings_main, NULL, 1500, &result);
}

int sh_modserve_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    size_t i;
    /* The boot index was built before this point, and the string injector
     * (installed after it) read that table in its first pass. */
    sh_modserve_set_index_listener(me_index_changed);
    g_ctor = (str_ctor_fn)me_clean(results, count, "IdStrCtor");
    g_dtor = (str_dtor_fn)me_clean(results, count, "IdStrDtor");
    g_anchor = (const unsigned char *)me_clean(results, count, "DeclRegistryAnchor");
    g_type = (type_fn)me_clean(results, count, "DeclTypeByName");
    g_register = (register_fn)me_clean(results, count, "DeclRegisterFile");
    g_source = (source_fn)me_clean(results, count, "DeclSourceFind");
    g_decl = (find_decl_fn)me_clean(results, count, "DeclFind");
    g_generic_load = (generic_load_fn)me_clean(results, count, "ResourceGenericLoad");
    /* Requirements: optional, so a build without them still registers every
     * bundle that needs none. */
    g_cvar_set = (cvar_set_fn)me_clean(results, count, "CvarSetString");
    g_buffer_cmd = (buffer_cmd_fn)me_clean(results, count, "BufferCommandText");
    g_cmdsys = sh_resolve_cmdsys(results, count, module_base);
    g_cvar_slot = (const void *)glb_resolve(module_base, "cvar_system_slot", NULL);
    g_last_error = (const char *)glb_resolve(module_base, "last_error_msg", NULL);
    g_main_thread_at = (const uint8_t *)glb_resolve(module_base, "main_thread_id", NULL);
    g_res_head = (const void *)glb_resolve(module_base, "decl_resource_head", NULL);
    g_editor = (const unsigned char *)glb_resolve(module_base, "editor_singleton", NULL);
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
        me_log("bundle cvar requirements unavailable (%s unresolved); bundles that set a cvar are blocked",
               !g_cvar_set ? "CvarSetString" : "the cvar system");
    if (!g_buffer_cmd || !g_cmdsys)
        me_log("bundle requirement commands unavailable (%s unresolved); their lines are not run",
               !g_buffer_cmd ? "BufferCommandText" : "the command system");
    if (g_bound) me_ensure_probe();
    return g_bound;
}

/* ------------------------------------------------------------------ early decls */

/* Shadows the engine parsed from its own bytes before the provider existed
 * (modserve.h, sh_modserve_each_unopened_shadow). Each one the engine holds
 * parsed is loaded again through ResourceGenericLoad with its state bits
 * cleared, the sequence op_load uses for a new identity: the load reads the
 * source through the provider, which now serves the bundle's bytes. One the
 * engine has not parsed yet is left for the provider. Main thread, inside
 * process heap 0 (a re-parsed decl lives for the process). */
#define ME_REPARSE_TYPES 12
typedef struct me_reparse_type { const char *type; int count; char names[160]; } me_reparse_type;
typedef struct me_reparse {
    me_reparse_type types[ME_REPARSE_TYPES];
    int ntypes, reparsed, through_provider, failed, faulted, pending;
} me_reparse;

static me_reparse_type *me_reparse_slot(me_reparse *r, const char *type)
{
    int i;
    for (i = 0; i < r->ntypes; ++i) if (!strcmp(r->types[i].type, type)) return &r->types[i];
    if (r->ntypes == ME_REPARSE_TYPES) return &r->types[ME_REPARSE_TYPES - 1];
    r->types[r->ntypes].type = type;
    return &r->types[r->ntypes++];
}

static int me_reparse_one(void *context, const char *type, const char *name, const char *request, const char *bundle_id)
{
    me_reparse *r = (me_reparse *)context;
    char before[ERROR_TEXT_MAX];
    int result = 0;
    (void)bundle_id;
    me_error_text(before, sizeof before);
    __try {
        void *m = me_manager(type), *decl = m ? g_decl(m, name, 0) : NULL;
        unsigned char *d = (unsigned char *)decl, state;
        if (!d) return 0;
        state = d[RES_STATE_OFF];
        if (state & 2) { r->pending++; return 0; }   /* not parsed yet: the provider will serve it */
        {
            me_reparse_type *t = me_reparse_slot(r, type);
            size_t at = strlen(t->names);
            d[RES_STATE_OFF] = (unsigned char)(state & ~3);
            g_generic_load(decl);
            state = d[RES_STATE_OFF];
            r->reparsed++;
            t->count++;
            if (at < sizeof t->names - 4)
                _snprintf_s(t->names + at, sizeof t->names - at, _TRUNCATE, "%s%s", at ? ", " : "", name);
            if (sh_modserve_open_count(request) > 0) r->through_provider++;
            if (state & 1) {
                r->failed++;
                me_log("reparse: %s '%s' (%s) failed to parse the bundle's bytes (state 0x%02x)", type, name,
                       bundle_id, state);
            }
        }
        result = 1;
    } __except (me_fault_filter(GetExceptionCode())) {
        me_log_fault("reparse", type, name, before);
        r->faulted++;
        result = 1;   /* not offered again */
    }
    return result;
}

/* Inside process heap 0: from me_run's scope, or entered here. */
static void me_reparse_early(const char *phase, int in_heap)
{
    me_reparse r;
    sh_process_heap_scope heap = { 0 };
    double t0 = me_now_ms();
    int offered = 0, i;
    if (!g_bound || !sh_modserve_active() || me_on_main() != 1) return;
    memset(&r, 0, sizeof r);
    if (!in_heap && !sh_process_heap_enter(&g_heap, &heap)) {
        me_log("%s: early decls not re-parsed (process heap scope unavailable)", phase);
        return;
    }
    __try { offered = sh_modserve_each_unopened_shadow(me_reparse_one, &r); }
    __finally {
        if (!in_heap && !sh_process_heap_leave(&heap)) me_log("%s: early-decl re-parse heap scope did not balance", phase);
    }
    if (!r.reparsed && !r.faulted) return;
    for (i = 0; i < r.ntypes; ++i)
        me_log("reparsed %d %s decl(s) the engine parsed before the serving index (%s%s)", r.types[i].count,
               r.types[i].type, r.types[i].names, strlen(r.types[i].names) >= sizeof r.types[i].names - 5 ? ", ..." : "");
    me_log("%s: early decls: %d re-parsed in %.1f ms (%d read through the provider, %d failed, %d faulted); "
           "%d of %d unserved shadow(s) not parsed yet", phase, r.reparsed, me_now_ms() - t0, r.through_provider,
           r.failed, r.faulted, r.pending, offered);
}

/* Models and images a gated bundle serves that the engine loaded before the
 * gates were live (modserve.h, sh_modserve_each_ungated_raw): each one the
 * resource lists hold is loaded again in place, which resolves its materials
 * and pixels with the blacklists off. Matched by name within the list of its
 * class (idMD6Model, idImage), since a material and an image can share one. */
#define ME_RAW_MAX 4096
typedef struct me_raw_name { const char *name; int image; int hit; } me_raw_name;
typedef struct me_raw {
    me_raw_name *names; int n, cap;
    int reloaded, models, images, failed, faulted;
    char first[700];
} me_raw;

static int me_raw_collect(void *context, const char *type, const char *name, const char *request, const char *bundle_id)
{
    me_raw *r = (me_raw *)context;
    (void)request; (void)bundle_id;
    if (r->n < r->cap) {
        r->names[r->n].name = name;
        r->names[r->n].image = !_stricmp(type, "image");
        r->names[r->n].hit = 0;
        r->n++;
    }
    return 0;
}

static int me_raw_settle(void *context, const char *type, const char *name, const char *request, const char *bundle_id)
{
    (void)context; (void)type; (void)name; (void)request; (void)bundle_id;
    return 1;
}

static void me_raw_one(void *context, const char *list_class, void *resource, const char *name)
{
    me_raw *r = (me_raw *)context;
    int i, image;
    if (!list_class) return;
    if (!_stricmp(list_class, "idImage")) image = 1;
    else if (!_stricmp(list_class, "idMD6Model")) image = 0;
    else return;
    for (i = 0; i < r->n; ++i) {
        unsigned char *d;
        if (r->names[i].hit || r->names[i].image != image || _stricmp(r->names[i].name, name)) continue;
        r->names[i].hit = 1;
        d = (unsigned char *)resource;
        __try {
            if (d[RES_STATE_OFF] & 2) return;   /* not loaded: its next load sees the live gates */
            d[RES_STATE_OFF] = (unsigned char)(d[RES_STATE_OFF] & ~3);
            g_generic_load(resource);
            if (d[RES_STATE_OFF] & 1) r->failed++;
            r->reloaded++;
            if (image) r->images++; else r->models++;
            if (strlen(r->first) + strlen(name) + 3 < sizeof r->first)
                _snprintf_s(r->first + strlen(r->first), sizeof r->first - strlen(r->first), _TRUNCATE, "%s%s",
                            r->first[0] ? ", " : "", name);
        } __except (me_fault_filter(GetExceptionCode())) {
            r->faulted++;
            me_log("reload: %s '%s' FAULTED (exception 0x%08lx)", list_class, name, (unsigned long)g_fault_code);
        }
        return;
    }
}

static void me_reload_ungated(const char *phase, int in_heap)
{
    me_raw r;
    sh_process_heap_scope heap = { 0 };
    double t0 = me_now_ms();
    if (!g_bound || !g_res_head || !sh_modserve_active() || me_on_main() != 1) return;
    memset(&r, 0, sizeof r);
    r.cap = ME_RAW_MAX;
    r.names = (me_raw_name *)calloc((size_t)r.cap, sizeof *r.names);
    if (!r.names) return;
    (void)sh_modserve_each_ungated_raw(me_raw_collect, &r);
    if (!r.n) { free(r.names); return; }
    if (!in_heap && !sh_process_heap_enter(&g_heap, &heap)) {
        me_log("%s: gated models and images not reloaded (process heap scope unavailable)", phase);
        free(r.names);
        return;
    }
    __try { (void)sh_modserve_resource_each(g_res_head, me_raw_one, &r); }
    __finally {
        if (!in_heap && !sh_process_heap_leave(&heap)) me_log("%s: gated reload heap scope did not balance", phase);
    }
    (void)sh_modserve_each_ungated_raw(me_raw_settle, NULL);
    me_log("%s: reloaded %d model(s) and %d image(s) loaded before their bundles' requirements were live in %.1f ms "
           "(%d failed, %d faulted; %d of %d not held)%s%s%s", phase, r.models, r.images, me_now_ms() - t0, r.failed,
           r.faulted, r.n - r.reloaded - r.faulted, r.n, r.first[0] ? ": " : "", r.first,
           r.reloaded > 0 && strlen(r.first) + 40 >= sizeof r.first ? ", ..." : "");
    free(r.names);
}

/* ------------------------------------------------------------------ refresh */

/* What a rescan changed under the engine (modserve.h, sh_modserve_each_refresh):
 * each changed decl the engine holds parsed is re-parsed with the early-decl
 * primitive (me_reparse_one), a gone shadow decl the same way (the provider
 * no longer answers it, so the engine reads its own bytes), and each changed
 * model and image it holds loaded is reloaded in place with the gated-reload
 * primitive (me_raw_one). Main thread, inside process heap 0; never during
 * Play (sh_modserve_refresh_after_rescan defers to the next pass). */
#define ED_ACTIVE_OFF       0x09      /* editor_set_active's byte: clear during SnapMap Play */

/* `sh_mods rescan` prints every refresh line to the console as well. */
static int g_refresh_echo;
static void me_rlog(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    memcpy(line, "MODS: ", 6);
    va_start(ap, fmt);
    _vsnprintf_s(line + 6, sizeof line - 6, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
    if (g_refresh_echo) sh_printf("%s\n", line);
}
#define ED_DEACT_REASON_OFF 0x2366c   /* 3: deactivated for Play */

/* 1 during SnapMap Play, 0 otherwise, -1 unknown. */
static int me_in_play(void)
{
    unsigned char active = 1;
    int reason = 0;
    if (!g_editor) return -1;
    __try {
        active = g_editor[ED_ACTIVE_OFF];
        reason = *(const int *)(g_editor + ED_DEACT_REASON_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return !active && reason == 3;
}

typedef struct me_refresh {
    me_reparse decls;
    me_raw raw;
    int stage;            /* 0 decls and collect, 1 settle the collected models and images */
    int not_held, gated;
} me_refresh;

static int me_refresh_visit(void *context, const sh_modserve_refresh_item *it)
{
    me_refresh *c = (me_refresh *)context;
    if (!it->gates_live) { if (!c->stage) c->gated++; return 0; }
    if (it->kind == SH_MODSERVE_RF_DECL) {
        int before = c->decls.reparsed + c->decls.faulted;
        (void)me_reparse_one(&c->decls, it->type, it->name, it->request, it->bundle_id);
        if (c->decls.reparsed + c->decls.faulted == before) c->not_held++;   /* its next parse reads the current bytes */
        return 1;
    }
    if (c->stage) return 1;
    (void)me_raw_collect(&c->raw, it->type, it->name, it->request, it->bundle_id);
    return 0;
}

static void me_refresh_apply(const char *phase, int in_heap, sh_modserve_refresh *out)
{
    me_refresh c;
    sh_process_heap_scope heap = { 0 };
    double t0 = me_now_ms(), t1;
    int i;
    if (!g_bound || me_on_main() != 1) return;
    (void)sh_modserve_refresh_expand();
    memset(&c, 0, sizeof c);
    c.raw.cap = ME_RAW_MAX;
    c.raw.names = (me_raw_name *)calloc((size_t)c.raw.cap, sizeof *c.raw.names);
    if (!c.raw.names) return;
    if (!in_heap && !sh_process_heap_enter(&g_heap, &heap)) {
        me_rlog("refresh: changes not applied at the %s (process heap scope unavailable)", phase);
        free(c.raw.names);
        return;
    }
    __try {
        if (sh_modserve_each_refresh(me_refresh_visit, &c) && c.raw.n && g_res_head)
            (void)sh_modserve_resource_each(g_res_head, me_raw_one, &c.raw);
    } __finally {
        if (!in_heap && !sh_process_heap_leave(&heap)) me_rlog("refresh: heap scope did not balance at the %s", phase);
    }
    t1 = me_now_ms();
    c.stage = 1;
    if (c.raw.n && g_res_head) (void)sh_modserve_each_refresh(me_refresh_visit, &c);
    if (c.decls.reparsed || c.decls.faulted || c.not_held) {
        for (i = 0; i < c.decls.ntypes; ++i)
            me_rlog("refresh: re-parsed %d %s decl(s) whose served bytes or parent changed (%s%s)", c.decls.types[i].count,
                   c.decls.types[i].type, c.decls.types[i].names,
                   strlen(c.decls.types[i].names) >= sizeof c.decls.types[i].names - 5 ? ", ..." : "");
        me_rlog("refresh: decls: %d re-parsed at the %s in %.1f ms (%d read through the provider, %d failed, %d faulted; "
               "%d not held, read at their next parse)", c.decls.reparsed, phase, t1 - t0, c.decls.through_provider,
               c.decls.failed, c.decls.faulted, c.not_held);
    }
    if (c.raw.n)
        me_rlog("refresh: models and images: reloaded %d model(s) and %d image(s) at the %s (%d failed, %d faulted; "
               "%d of %d not held, read at their next load)%s%s%s", c.raw.models, c.raw.images, phase, c.raw.failed,
               c.raw.faulted, c.raw.n - c.raw.reloaded - c.raw.faulted, c.raw.n, c.raw.first[0] ? ": " : "",
               c.raw.first, c.raw.reloaded > 0 && strlen(c.raw.first) + 40 >= sizeof c.raw.first ? ", ..." : "");
    if (!g_res_head && c.raw.n)
        me_rlog("refresh: %d model(s) and image(s) not reloaded (decl_resource_head unresolved)", c.raw.n);
    if (c.gated)
        me_rlog("refresh: %d change(s) wait for their bundles' requirements, applied at the next editor pass or map load",
               c.gated);
    if (out) {
        out->reparsed += c.decls.reparsed;
        out->reloaded += c.raw.reloaded;
        out->failed += c.decls.failed + c.decls.faulted + c.raw.failed + c.raw.faulted;
    }
    free(c.raw.names);
}

static void me_refresh_note(sh_modserve_refresh *r, const char *fmt, ...)
{
    size_t at = strlen(r->note);
    va_list ap;
    if (at + 3 >= sizeof r->note) return;
    if (at) { memcpy(r->note + at, "; ", 3); at += 2; }
    va_start(ap, fmt);
    _vsnprintf_s(r->note + at, sizeof r->note - at, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* A deferred refresh waits for the editor: a watcher thread hands it to the
 * main-thread call slot, which drains in editor frames only, once Play has
 * ended (the next registration pass applies it too, whichever comes first).
 * Changes still waiting for their bundles' gates stay for the next pass. */
static volatile LONG g_refresh_watch;

/* The editor side of a refresh, main thread, not in Play: new gameplay decls
 * of mods this session registered are registered now (as a map's activation
 * would), and when a rescan added, changed or removed editor rows the live
 * palette is rebuilt (the 'sh_mods palette rebuild' path, whose editor pass
 * registers new rows first). Without a palette yet the first editor entry
 * builds it with every row. */
static volatile LONG g_palette_wanted;

static void me_refresh_editor(const char *phase, sh_modserve_refresh *r)
{
    sh_modserve_plan plan;
    int ready;
    if (sh_modserve_plan_refresh(&plan) > 0) {
        double t0 = me_now_ms();
        ready = me_run(&plan, "refresh");
        r->registered += ready > 0 ? ready : 0;
        me_rlog("refresh: registered %d of %d new decl(s) of mods in use at the %s in %.1f ms (%d blocked)",
                ready < 0 ? 0 : ready, plan.count, phase, me_now_ms() - t0, plan.blocked);
    }
    sh_modserve_plan_free(&plan);
    if (!InterlockedCompareExchange(&g_palette_wanted, 0, 0)) return;
    if (!sh_timeline_tile_palette()) {
        InterlockedExchange(&g_palette_wanted, 0);
        me_rlog("refresh: editor rows changed; no editor palette yet, the first editor entry builds it with them");
        return;
    }
    if (modserve_palette_rebuild_requested()) {
        InterlockedExchange(&g_palette_wanted, 0);
        r->palette = 1;
        me_rlog("refresh: editor rows changed; palette rebuilt at the %s (see the palette line above)", phase);
    } else {
        r->palette = -1;
        me_rlog("refresh: editor rows changed; palette rebuild refused at the %s (see the line above); "
                "'sh_mods palette rebuild' retries it", phase);
    }
}

static int me_refresh_main(void *context)
{
    sh_modserve_refresh r;
    sh_modserve_diff d;
    (void)context;
    if (me_in_play() == 1 || me_on_main() != 1) return 0;
    sh_modserve_last_diff(&d);
    memset(&r, 0, sizeof r);
    if (d.pending) me_refresh_apply("return to the editor", 0, &r);
    me_refresh_editor("return to the editor", &r);
    sh_modserve_last_diff(&d);
    me_rlog("refresh: deferred changes applied at the return to the editor: %d re-parsed, %d reloaded, %d registered, "
            "palette %s, %d failed, %d still pending", r.reparsed, r.reloaded, r.registered,
            r.palette > 0 ? "rebuilt" : r.palette < 0 ? "refused" : "unchanged", r.failed, d.pending);
    /* A refused palette rebuild is tried again on a later pass. */
    return r.palette < 0 ? 0 : 1;
}

static DWORD WINAPI me_refresh_watcher(LPVOID unused)
{
    sh_run_on_main_fn run = NULL;
    int i, tries, result = 0;
    (void)unused;
    sh_apply_engine_get_run_on_main(&run);
    /* Up to an hour; any registration pass in between applies the same marks.
     * A palette rebuild the editor refuses (an entity or a wire held, or a
     * native edit such as the Mods tab's Delete Selection still committing)
     * is tried again once the editor is free, at most 40 times. */
    for (i = 0, tries = 0; run && i < 7200 && tries < 40; ++i) {
        sh_modserve_diff d;
        Sleep(500);
        sh_modserve_last_diff(&d);
        if (!d.pending && !InterlockedCompareExchange(&g_palette_wanted, 0, 0)) break;
        if (me_in_play() == 1 || sh_editor_delete_native_pending()) continue;
        tries++;
        if (run(NULL, me_refresh_main, NULL, 1000, &result) == 1 && result == 1) break;
    }
    InterlockedExchange(&g_refresh_watch, 0);
    return 0;
}

static void me_refresh_watch(void)
{
    HANDLE h;
    if (InterlockedCompareExchange(&g_refresh_watch, 1, 0)) return;
    h = CreateThread(NULL, 0, me_refresh_watcher, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_refresh_watch, 0);
}

int sh_modserve_refresh_after_rescan(sh_modserve_refresh *out)
{
    sh_modserve_refresh r;
    sh_modserve_diff d;
    int play, main = me_on_main(), rows, later;
    const char *why = NULL;
    memset(&r, 0, sizeof r);
    if (main == 1) (void)sh_modserve_refresh_expand();
    /* Requirements follow the new table at once: a mod uninstalled or dropped
     * has its cvars restored and its deactivate lines run, a new one is set.
     * During Play, or off the main thread, the next pass does it. */
    if (main == 1 && me_in_play() != 1 && sh_modserve_requirements_held()) {
        sh_modserve_gates g;
        (void)sh_modserve_requirements_apply(me_gate_ops(), "rescan", &g);
    }
    sh_modserve_last_diff(&d);
    rows = d.added_rows + d.changed_rows + d.gone_rows;
    later = d.added_decls - d.added_rows - d.added_active;   /* new names of mods no map has used yet */
    if (rows) InterlockedExchange(&g_palette_wanted, 1);
    if (!d.changed && !d.gone && !d.added_decls && !d.pending) {
        strncpy_s(r.note, sizeof r.note, "no served file changed", _TRUNCATE);
        me_rlog("refresh: no served file changed");
        if (out) *out = r;
        return 0;
    }
    play = me_in_play();
    if (!g_bound) why = "decl registration is unavailable on this build";
    else if (main != 1) why = "not on the main thread";
    else if (play == 1) why = "Play is running";
    if (!why) {
        if (d.pending) me_refresh_apply("rescan", 0, &r);
        me_refresh_editor("rescan", &r);
        sh_modserve_last_diff(&d);
    }
    r.pending = d.pending + (why ? d.added_active : 0) + (InterlockedCompareExchange(&g_palette_wanted, 0, 0) && why ? 1 : 0);
    r.needs_map_reload = d.changed_map + (r.palette < 0 ? rows : 0);
    r.needs_restart = d.changed_restart + d.gone_names;
    r.state = why && r.pending ? 2 : 1;
    if (!why && r.palette < 0 && g_bound) {
        /* The editor refused the palette rebuild for now: retried when it is free. */
        me_refresh_watch();
        me_refresh_note(&r, "the editor palette is rebuilt when the editor is free");
    }
    if (why && r.pending) {
        if (play == 1 && g_bound) me_refresh_watch();
        me_rlog("refresh: deferred: %d change(s) wait for the return to the editor or the next map load (%s)", r.pending,
                why);
        me_refresh_note(&r, "%d change(s) apply on returning to the editor or at the next map load (%s)", r.pending, why);
    } else {
        me_refresh_note(&r, "%d decl(s) re-parsed, %d model(s) and image(s) reloaded%s", r.reparsed, r.reloaded,
                        r.failed ? " (some failed; see the log)" : "");
        if (r.registered) me_refresh_note(&r, "%d new decl(s) registered", r.registered);
        if (r.palette > 0) me_refresh_note(&r, "editor palette rebuilt (%d row change(s))", rows);
        if (d.pending) me_refresh_note(&r, "%d wait for the bundles' requirements (next editor pass)", d.pending);
    }
    if (later)
        me_rlog("refresh: %d new decl name(s) of mods no map has used this session register when a map that uses the "
                "mod loads", later);
    if (r.needs_map_reload) {
        me_rlog("refresh: needs a map reload: %d changed movie(s)%s", d.changed_map,
                r.palette < 0 ? ", and the editor rows until the palette is rebuilt" : "");
        me_refresh_note(&r, "%d change(s) need a map reload%s", r.needs_map_reload,
                        r.palette < 0 ? " ('sh_mods palette rebuild' applies the editor rows)" : "");
    }
    if (r.needs_restart) {
        me_rlog("refresh: needs a restart: %d changed file(s) nothing reloads in this process%s%s, %d removed decl name(s) "
                "stay registered", d.changed_restart, d.restart_first[0] ? ": " : "", d.restart_first, d.gone_names);
        me_refresh_note(&r, "%d change(s) need a restart", r.needs_restart);
    }
    me_rlog("refresh: summary: %d changed, %d new, %d gone, %d inheriting decl(s) follow their parents; %d "
            "re-parsed, %d reloaded, %d registered, palette %s, %d need a map reload, %d need a restart, %d pending",
            d.changed, d.added, d.gone, d.dependents, r.reparsed, r.reloaded, r.registered,
            r.palette > 0 ? "rebuilt" : r.palette < 0 ? "refused" : rows ? "deferred" : "unchanged",
            r.needs_map_reload, r.needs_restart, r.pending);
    if (out) *out = r;
    return r.state;
}

/* The plan's sources are registered; its loads have not run (me_run). A
 * rescan's pending changes are applied first, so a file whose bytes changed
 * is re-parsed as a change, not as an early decl. */
static void op_sources_published(void *context)
{
    (void)context;
    me_refresh_apply(g_reparse_phase, 1, NULL);
    me_reparse_early(g_reparse_phase, 1);
    me_reload_ungated(g_reparse_phase, 1);
}

/* ------------------------------------------------------------------ palette */

/* Keep the decls an editor row points at for the session (modserve.h,
 * sh_modserve_editor_refs): an object the engine created at level 1 after
 * boot is raised to 4, as everything a stock row names was at the boot
 * promotion. The row's parse normally made the object; one it did not is
 * created by name first. Only the level changes: contents load on demand. */
typedef struct me_keep { int kept, permanent, created, missing, loaded; } me_keep;

static void me_keep_ref(void *context, const char *type, const char *name, const char *row)
{
    me_keep *k = (me_keep *)context;
    char before[ERROR_TEXT_MAX];
    me_error_text(before, sizeof before);
    __try {
        void *m = me_manager(type), *decl = m ? g_decl(m, name, 0) : NULL;
        /* Not created by the row's parse yet: create it now, so it is born
         * before the first purge and kept like the others. */
        if (!decl && m) { decl = g_decl(m, name, 1); if (decl) k->created++; }
        if (!decl) { k->missing++; return; }
        /* Created by reference and not parsed yet: parse it now, inside the
         * pass's watermark, so the resources it creates are raised with it. */
        if ((((unsigned char *)decl)[RES_STATE_OFF] & 3) == 2) {
            ((unsigned char *)decl)[RES_STATE_OFF] &= (unsigned char)~2;
            g_generic_load(decl);
            k->loaded++;
        }
        if (*(unsigned *)((unsigned char *)decl + RES_LIFETIME_OFF) == 1) {
            *(unsigned *)((unsigned char *)decl + RES_LIFETIME_OFF) = 4;
            k->kept++;
            me_log("editor: %s '%s' kept for the session (editor row '%s' points at it)", type, name, row);
        } else k->permanent++;
    } __except (me_fault_filter(GetExceptionCode())) { me_log_fault("keep", type, name, before); }
}

void sh_modserve_palette_before_build(void)
{
    sh_modserve_plan plan;
    sh_modserve_watermark mark = { NULL, 0, 0 };
    double t0;
    int ready, i, rows_ok = 0, rows_failed = 0, first = !InterlockedExchange(&g_editor_pass_done, 1);
    /* Mod virtual-texture families first: a material binds to its family only
     * when the family's record exists when the material is parsed. */
    sh_modvt_before_pass("editor");
    /* Mod sound banks: submitted once the sound system is up (modsound.h). */
    sh_modsound_before_pass("editor");
    /* Bundle strings of an index published off the main thread. */
    (void)sh_strids_refresh_pending();
    if (!g_bound || !sh_modserve_active()) return;
    me_ensure_probe();
    t0 = me_now_ms();
    if (sh_modserve_plan_editor(&plan) < 0) { me_log("editor rows: plan refused (out of memory)"); return; }
    if (!plan.count) {
        sh_modserve_plan_free(&plan);
        me_refresh_apply("editor", 0, NULL);
        me_reparse_early("editor", 0);
        me_reload_ungated("editor", 0);
        if (first) me_log("editor rows registered in 0.0 ms (0 rows)");
        return;
    }
    /* Everything the pass creates from here on is the rows' closure. */
    if (!g_res_head || !sh_modserve_watermark_take(g_res_head, &mark))
        me_log("editor: resource watermark unavailable (%s); only the decls rows name are kept",
               g_res_head ? "the resource lists could not be walked" : "decl_resource_head unresolved");
    ready = me_run(&plan, "editor");
    {
        me_keep keep = { 0, 0, 0, 0, 0 };
        sh_process_heap_scope heap = { 0 };
        int refs = 0;
        /* Inside process heap 0, as registration: an object created here outlives every map. */
        if (sh_process_heap_enter(&g_heap, &heap)) {
            __try { refs = sh_modserve_editor_refs(&plan, me_keep_ref, &keep); }
            __finally { if (!sh_process_heap_leave(&heap)) me_log("editor: process heap scope did not balance"); }
        }
        if (refs && (keep.kept || keep.missing))
            me_log("editor: %d decl(s) editor rows point at kept for the session (%d created here, %d parsed here), %d already permanent, %d not found",
                   keep.kept, keep.created, keep.loaded, keep.permanent, keep.missing);
    }
    if (mark.ok) {
        sh_modserve_raised raised;
        if (sh_modserve_watermark_raise(g_res_head, &mark, &raised) && raised.created)
            me_log("editor: %u resource(s) the rows' closure created kept for the session (%u new, %u lists): %s",
                   raised.raised, raised.created, raised.lists, raised.summary);
        else if (!raised.lists) me_log("editor: resource lists could not be walked after the pass; closure not raised");
    }
    sh_modserve_watermark_free(&mark);
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
    sh_modvt_before_pass("map");
    sh_modsound_before_pass("map");
    (void)sh_strids_refresh_pending();
    me_ensure_probe();
    t0 = me_now_ms();
    if (sh_modserve_plan_bundles(bundle_hashes, n, &plan, &unknown) < 0) { me_log("activation refused: out of memory"); return -1; }
    /* A bundle the map names that is not installed may be an uninstalled mod
     * still in removed-mods: reinstated, the index rebuilt, planned again. */
    if (unknown > 0 && sh_modserve_restore(bundle_hashes, n, NULL, 0, SH_MODSERVE_RESTORE_LOAD) > 0) {
        sh_modserve_plan_free(&plan);
        unknown = 0;
        if (sh_modserve_plan_bundles(bundle_hashes, n, &plan, &unknown) < 0) { me_log("activation refused: out of memory"); return -1; }
    }
    if (plan.count) ready = me_run(&plan, "map");
    else { me_refresh_apply("map", 0, NULL); me_reparse_early("map", 0); me_reload_ungated("map", 0); ready = 0; }
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

static void me_print_line(void *context, const char *line) { (void)context; sh_printf("  %s\n", line); }

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
        (void)sh_modserve_requirements_report(me_print_line, NULL);
        {
            sh_modserve_diff d;
            sh_modserve_last_diff(&d);
            if (d.pending)
                sh_printf("sh_mods: refresh: %d change(s) pending%s; they apply on returning to the editor or at the next "
                          "map load ('sh_mods refresh' in the editor applies them now)\n", d.pending,
                          me_in_play() == 1 ? " (Play is running)" : "");
        }
        sh_modvt_print_status();
        sh_modsound_print_status();
        return 1;
    }
    if (!_stricmp(verb, "rescan") || !_stricmp(verb, "refresh")) {
        sh_modserve_refresh r;
        int n = sh_modserve_index_rescan();
        me_ensure_probe();
        sh_printf("sh_mods: rescan %s (%d bundles active); new editor rows need 'sh_mods palette rebuild'\n",
                  n < 0 ? "refused" : "done", n < 0 ? 0 : n);
        if (n >= 0) {
            g_refresh_echo = 1;
            (void)sh_modserve_refresh_after_rescan(&r);
            g_refresh_echo = 0;
            sh_printf("sh_mods: refresh: %s\n", r.note);
        }
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
