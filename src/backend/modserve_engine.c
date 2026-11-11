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
#include "modhud_scene.h"
#include "modscene_place.h"
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
#include "modledger.h"
#include "modtypeinfo.h"
#include "companion_engine.h"
#include "script/script_hooks.h"   /* mod scripts' map gates */

#define RES_LIFETIME_OFF    0x28
#define RES_STATE_OFF       0x2c
#define SEDEF_NAME_OFF      0x08
#define SEDEF_ENTITYDEF_OFF 0x1c8
#define PAL_ENTRIES_OFF     0x10
#define PAL_COUNT_OFF       0x18
#define PAL_STRIDE          0x6f0
#define PAL_MAX             100000
#define PROBE_STOLEN        21
#define LOOKUP_STOLEN       19
#define LOAD_STOLEN         20
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
typedef void *(*lookup_fn)(void *, const char *, uint64_t);
typedef void *(*materialize_fn)(void *, const char *, char, char);
typedef union ms_string { void *align; unsigned char bytes[48]; } ms_string;

static str_ctor_fn g_ctor;
static str_dtor_fn g_dtor;
static const unsigned char *g_anchor;
static type_fn g_type;
static register_fn g_register;
static source_fn g_source;
static find_decl_fn g_decl;
static generic_load_fn g_generic_load;
static materialize_fn g_materialize;   /* ResourceMaterialize: find-or-load in one resource list */
static sh_process_heap_api g_heap;
static int g_bound;
static const char *g_last_error;
static const uint8_t *g_main_thread_at;
static const void *g_res_head;        /* decl_resource_head: the resource list-of-lists (watermark) */
static probe_fn g_probe_orig;
static sig_result g_probe_sig;
static int g_probe_sig_ok;
static lookup_fn g_lookup_orig;       /* ResourceLookup, detoured while the index holds a decl */
static sig_result g_lookup_sig;
static int g_lookup_sig_ok;
static generic_load_fn g_load_orig;   /* ResourceGenericLoad, detoured with the lookup */
static sig_result g_load_sig;
static int g_load_sig_ok;
static int g_attrib_checked;
static volatile LONG g_editor_pass_done;
static DWORD g_fault_code;
static cvar_set_fn g_cvar_set;
static const void *g_cvar_slot;
static buffer_cmd_fn g_buffer_cmd;
static void *g_cmdsys;
static const unsigned char *g_editor;  /* editor_singleton: idSnapEditorLocal, constructed in place */
static void *g_res_lock;               /* resource_lock: the static struct whose first qword is the CRITICAL_SECTION */
static __declspec(thread) int g_peek;  /* a product read of served bytes: not a serve to the engine */

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

/* The engine's resource lock (report ADD-G3): every list add, remove and find
 * takes it, and list add swap-removes and grows the entry arrays, so every
 * walk of a list here holds it. Recursive on one thread. Its accessor builds
 * the CRITICAL_SECTION on first use, so a NULL pointer means no list has been
 * touched yet and nothing can race the walk. */
static CRITICAL_SECTION *me_lock(void)
{
    CRITICAL_SECTION *cs = NULL;
    if (!g_res_lock) return NULL;
    __try { cs = *(CRITICAL_SECTION *const *)g_res_lock; } __except (EXCEPTION_EXECUTE_HANDLER) { cs = NULL; }
    if (cs) EnterCriticalSection(cs);
    return cs;
}

static void me_unlock(CRITICAL_SECTION *cs) { if (cs) LeaveCriticalSection(cs); }

/* Raw resource names: case-insensitive, and the id's '-' and '_' are one
 * (a decl names md6/mods/<id>/..., the file may spell the id either way). */
static char me_fold(char c)
{
    if (c == '-') return '_';
    if (c == '\\') return '/';
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int me_name_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (; *a && *b; ++a, ++b) if (me_fold(*a) != me_fold(*b)) return 0;
    return !*a && !*b;
}

/* `name` inside `body` under the same folding. */
static int me_body_names(const unsigned char *body, size_t len, const char *name)
{
    size_t n = strlen(name), i, j;
    if (!n || len < n) return 0;
    for (i = 0; i + n <= len; ++i) {
        for (j = 0; j < n && me_fold((char)body[i + j]) == me_fold(name[j]); ++j) {}
        if (j == n) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ ledger */

/* The provider's serves (modledger.h). A product read of served bytes
 * (validation, the dependents search) sets g_peek and is not recorded. */
static void me_serve_listener(const char *request, const char *bundle_hash, const char *file_hash,
                              const unsigned char *bytes, size_t length)
{
    sh_modledger_ident id;
    if (g_peek) return;
    if (sh_modledger_ident_of(bundle_hash, file_hash, bytes, length, &id)) (void)sh_modledger_record(request, &id);
}

/* What the open map serves for `request` now. 1 with its identity. */
static int me_now_ident(const char *request, sh_modledger_ident *out)
{
    const char *bundle = NULL, *file = NULL;
    const unsigned char *body = NULL;
    size_t body_len = 0;
    uint64_t size = 0;
    if (!sh_modserve_serve_identity(request, &bundle, &file, &body, &body_len, &size)) return 0;
    if (!file && !body) return 0;
    return sh_modledger_ident_of(bundle, file, body, body ? body_len : (size_t)size, out);
}

/* The ledger's decision for a pending refresh item (sh_modledger_decide). */
static int me_ledger_decision(const sh_modserve_refresh_item *it)
{
    sh_modledger_rec rec;
    sh_modledger_ident now;
    int has = sh_modledger_find(it->request, &rec), have_now = !it->gone && me_now_ident(it->request, &now);
    return sh_modledger_decide(has ? &rec : NULL, have_now ? &now : NULL, it->shadow || it->gone,
                               sh_modledger_complete());
}

/* The served bytes of `request` for a product check, not recorded as a serve. */
static unsigned char *me_peek(const char *request, size_t *len)
{
    unsigned char *bytes = NULL;
    int r;
    *len = 0;
    g_peek = 1;
    r = sh_modserve_open(request, &bytes, len);
    g_peek = 0;
    return r > 0 ? bytes : NULL;
}

/* A .bmd6anim the engine's Load accepts (VK 0x1415c60e0 / GL 0x1415c72d0):
 * the first dword read big-endian is 0x2602414d (magic 0x2602, version
 * 0x414d), two header dwords, then the skeleton name (int32 length, text).
 * A load that fails frees the held anim's data (ResourceReconstruct), so the
 * bytes are checked before the load is asked for. */
#define ME_ANIM_MAGIC 0x2602414du
static int me_anim_bytes_ok(const unsigned char *b, size_t len, char *why, size_t cap)
{
    uint32_t magic, n;
    if (!b || len < 16) { _snprintf_s(why, cap, _TRUNCATE, "%zu bytes, shorter than the header", len); return 0; }
    magic = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    if (magic != ME_ANIM_MAGIC) {
        _snprintf_s(why, cap, _TRUNCATE, "magic and version 0x%08x, not 0x%08x", magic, ME_ANIM_MAGIC);
        return 0;
    }
    memcpy(&n, b + 12, 4);
    if (!n || n > 512 || 16u + n > len) {
        _snprintf_s(why, cap, _TRUNCATE, "skeleton name length %u does not fit %zu bytes", n, len);
        return 0;
    }
    return 1;
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

/* The engine error text a load left, when it names the decl's own file: the
 * parse failed. An animWeb's parse catches its own idLexerException (VK
 * FUN_1416e2c10 / GL FUN_1416e3590), so DeclRead returns success and the
 * state byte shows nothing; the reporter copies the text into last_error_msg
 * before the throw on both renderers. */
static char g_refusal[ERROR_TEXT_MAX];

static int me_refused(const char *before, const char *type, const char *name)
{
    char after[ERROR_TEXT_MAX];
    me_error_text(after, sizeof after);
    if (!after[0] || !strcmp(after, before) || !sh_modserve_error_names_decl(after, type, name)) return 0;
    strcpy_s(g_refusal, sizeof g_refusal, after);
    return 1;
}

static void op_refusal(void *context, char *out, size_t cap)
{
    (void)context;
    if (out && cap) strncpy_s(out, cap, g_refusal, _TRUNCATE);
}

static int op_load(void *context, const char *type, const char *name, int persist,
                   unsigned *out_state, unsigned *out_lifetime)
{
    char before[ERROR_TEXT_MAX];
    int ok = 0;
    (void)context;
    *out_state = *out_lifetime = 0;
    g_refusal[0] = 0;
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
            /* DeclFind loads a name the probe admits on demand: a parse it
             * already refused is not run a second time. */
            int refused = me_refused(before, type, name);
            /* A permanent decl: parsed here it would bind to decls this map
             * pass created at level 1 and keep them after the purge (the menu
             * collectibles' unlockModels). out_* carry the levels before. */
            if (!refused && sh_modserve_load_left_to_engine(lifetime, state, persist)) {
                *out_state = state;
                *out_lifetime = lifetime;
                return SH_MODSERVE_LOAD_LEFT;
            }
            if (!refused && !(state & 1)) {
                /* A new identity with no consumers takes the native source load
                 * once, at the persistent level, as timeline_tile.c does. */
                *(unsigned *)(d + RES_LIFETIME_OFF) = 4;
                sh_modserve_kept_add(d);
                d[RES_STATE_OFF] = (unsigned char)(state & ~2);
                g_generic_load(decl);
                state = d[RES_STATE_OFF];
                refused = me_refused(before, type, name);
            }
            /* Editor rows stay through map purges; everything else returns to
             * the engine's own level (1), which the next map unload purges. */
            *(unsigned *)(d + RES_LIFETIME_OFF) = persist ? 4u : lifetime;
            *out_state = state;
            *out_lifetime = *(unsigned *)(d + RES_LIFETIME_OFF);
            ok = refused ? SH_MODSERVE_LOAD_REFUSED : (state & 3) == 0;
            if (ok == 1 && !_stricmp(type, "snapEditorEntityDef") && !*(void **)(d + SEDEF_ENTITYDEF_OFF)) {
                me_log("sedef '%s' resolved no entityDef (sedef+0x1c8 is NULL); palette admission would reject it", name);
                ok = 0;
            }
        }
    } __except (me_fault_filter(GetExceptionCode())) { me_log_fault("load", type, name, before); ok = 0; }
    return ok;
}

/* The lookup an animWeb's parse makes for each model it names: DeclFind
 * without makeDefault, which answers from the resource lists, then the
 * visibility probe (a name the probe admits is created and loaded, as the
 * parse would have done). Inside the pass's process heap scope. */
static int op_decl_found(void *context, const char *type, const char *name)
{
    int found = -1;
    (void)context;
    __try {
        void *m = me_manager(type);
        if (m) found = g_decl(m, name, 0) != NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { found = -1; }
    return found;
}

/* Whether the engine holds a resource of this name now, by a walk of the
 * type's own resource list (DeclTypeByName answers the list object: its
 * entries at +0x20, count at +0x28, each resource's owning list at +0x10 and
 * name at +0x08). Unlike DeclFind it creates nothing and loads nothing, so
 * asking does not change what the registration it guards would do. */
#define ME_LIST_ENTRIES 0x20
#define ME_LIST_COUNT   0x28
#define ME_RES_NAME     0x08
#define ME_RES_LIST     0x10
static int op_held(void *context, const char *type, const char *name, unsigned *out_state, unsigned *out_lifetime)
{
    int held = -1;
    const unsigned char *m = NULL;
    CRITICAL_SECTION *cs;
    (void)context;
    *out_state = *out_lifetime = 0;
    __try { m = (const unsigned char *)me_manager(type); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!m) return -1;
    cs = me_lock();
    __try {
        {
            unsigned char *const *entries = *(unsigned char *const *const *)(m + ME_LIST_ENTRIES);
            int count = *(const int *)(m + ME_LIST_COUNT), i;
            if (count < 0 || count > (1 << 22) || (count && !entries)) count = -1;
            held = count < 0 ? -1 : 0;
            for (i = 0; i < count; ++i) {
                const unsigned char *r = entries[i];
                const char *n;
                if (!r || *(const unsigned char *const *)(r + ME_RES_LIST) != m) continue;
                n = *(const char *const *)(r + ME_RES_NAME);
                if (n && !strcmp(n, name)) {
                    *out_state = r[RES_STATE_OFF];
                    *out_lifetime = *(const unsigned *)(r + RES_LIFETIME_OFF);
                    held = 1;
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { held = -1; }
    me_unlock(cs);
    return held;
}

/* Every resource of every list (decl_resource_head; list +0x08 type, +0x10
 * class, +0x18 next, +0x20 entries, +0x28 count), copied under the resource
 * lock; the caller works on the copy after it is released. */
#define ME_LIST_TYPE  0x08
#define ME_LIST_CLASS 0x10
#define ME_LIST_NEXT  0x18
typedef struct me_held { const char *type, *cls, *name; void *obj; unsigned lifetime; unsigned char state; } me_held;
typedef struct me_held_set { me_held *v; int n, cap, lists, ok; } me_held_set;

static void me_held_free(me_held_set *s) { free(s->v); memset(s, 0, sizeof *s); }

static int me_held_collect(me_held_set *s, int level4_only)
{
    CRITICAL_SECTION *cs;
    memset(s, 0, sizeof *s);
    if (!g_res_head) return 0;
    cs = me_lock();
    __try {
        const unsigned char *l = *(const unsigned char *const *)g_res_head;
        for (; l && s->lists < 4096; l = *(const unsigned char *const *)(l + ME_LIST_NEXT), s->lists++) {
            unsigned char *const *entries = *(unsigned char *const *const *)(l + ME_LIST_ENTRIES);
            int count = *(const int *)(l + ME_LIST_COUNT), i;
            const char *type = *(const char *const *)(l + ME_LIST_TYPE), *cls = *(const char *const *)(l + ME_LIST_CLASS);
            if (count < 0 || count > (1 << 22) || (count && !entries)) break;
            for (i = 0; i < count; ++i) {
                const unsigned char *r = entries[i];
                unsigned lifetime;
                if (!r) continue;
                lifetime = *(const unsigned *)(r + RES_LIFETIME_OFF);
                if (level4_only && lifetime != 4) continue;
                if (s->n == s->cap) {
                    int cap = s->cap ? s->cap * 2 : 4096;
                    me_held *v = (me_held *)realloc(s->v, (size_t)cap * sizeof *v);
                    if (!v) break;
                    s->v = v;
                    s->cap = cap;
                }
                s->v[s->n].type = type;
                s->v[s->n].cls = cls;
                s->v[s->n].name = *(const char *const *)(r + ME_RES_NAME);
                s->v[s->n].obj = (void *)r;
                s->v[s->n].lifetime = lifetime;
                s->v[s->n].state = r[RES_STATE_OFF];
                s->n++;
            }
        }
        s->ok = !l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { s->ok = 0; }
    me_unlock(cs);
    return s->ok;
}

static int me_name_cmp(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char x = me_fold(*a), y = me_fold(*b);
        if (x != y) return (unsigned char)x < (unsigned char)y ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}

static int me_held_cmp(const void *pa, const void *pb)
{
    const me_held *a = (const me_held *)pa, *b = (const me_held *)pb;
    int c = _stricmp(a->type ? a->type : "", b->type ? b->type : "");
    return c ? c : me_name_cmp(a->name ? a->name : "", b->name ? b->name : "");
}

/* Sorted by type and folded name, for me_held_find. */
static void me_held_sort(me_held_set *s)
{
    if (s->n > 1) qsort(s->v, (size_t)s->n, sizeof *s->v, me_held_cmp);
}

/* The held resource of a type list and name (folded) in a sorted set, or NULL. */
static const me_held *me_held_find(const me_held_set *s, const char *type, const char *name)
{
    int lo = 0, hi = s->n - 1;
    me_held key;
    key.type = type;
    key.name = name;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2, c = me_held_cmp(&key, &s->v[mid]);
        if (!c) return &s->v[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return NULL;
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
                                              op_sources_published, op_decl_found, op_refusal, op_held };

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

/* The editor closure's watermark (modserve.h, SH_MODSERVE_PART_*). A boot
 * editor pass creates everything its rows lead the engine to create inside
 * one watermark and keeps it; work for those rows done later (a refresh after
 * an in-session install, a map pass registering a decl a row needs) takes one
 * of its own, so it is kept the same way. Nested work inside a watermark
 * (the editor pass, a kept plan part) is covered by the outer one: depth
 * counts them, and g_refresh_part limits the refresh run inside a kept plan
 * part to the closure (the editor pass refreshes everything inside its own). */
static int g_mark_depth;
static int g_refresh_part = SH_MODSERVE_PART_ALL;

typedef struct me_closure { sh_modserve_watermark mark; int counted; } me_closure;

/* What the pass looks up (sh_modserve_lookups_*): recorded from the outermost
 * watermark's start to its end by the ResourceLookup detour, so a kept decl
 * never binds to an object that outlives it by less (report F8b). */
static void me_lookups_open(void)
{
    if (g_lookup_orig) sh_modserve_lookups_open();
}

static void me_lookups_close(const char *phase, const char *what)
{
    sh_modserve_lookups lk;
    if (!sh_modserve_lookups_recording()) return;
    if (!sh_modserve_lookups_close(g_res_head, &lk)) {
        me_log("%s: resource lists could not be walked after the %s; what it looked up was not kept", phase, what);
        return;
    }
    if (lk.overflow)
        me_log("%s: the %s looked up more than the product records; the rest were not kept", phase, what);
    if (lk.raised)
        me_log("%s: %u resource(s) the %s looked up kept for the session with it (%u looked up, %u live): %s%s", phase,
               lk.raised, what, lk.looked_up, lk.live, lk.first, strlen(lk.first) + 40 >= sizeof lk.first ? ", ..." : "");
}

static void me_closure_begin(me_closure *c)
{
    memset(c, 0, sizeof *c);
    if (!g_res_head || !sh_modserve_watermark_take(g_res_head, &c->mark))
        me_log("editor closure: resource watermark unavailable (%s); what the editor rows' decls create now is not kept",
               g_res_head ? "the resource lists could not be walked" : "decl_resource_head unresolved");
    if (!g_mark_depth++) me_lookups_open();
    c->counted = 1;
}

static void me_closure_end(me_closure *c, const char *phase, const char *what);
static int me_in_play(void);

/* The render models kept decls hold, loaded here while their files serve
 * (sh_modserve_held_models): each one not held yet is created (DeclFind with
 * create, as op_load) and loaded; a default box or a model a map heap holds is
 * loaded again in place. All inside process heap 0 and a keeper of its own
 * (what the load creates, its materials and images, lives as long as the
 * model, ADD-G6), then held at level 4. A load that still ends in a default
 * stays at the engine's level and is named. */
typedef struct me_held_load {
    int loaded, reloaded, failed, faulted, skipped;
    char first[400], bad[300];
} me_held_load;

static void me_held_note(char *buf, size_t cap, const char *name)
{
    size_t at = strlen(buf);
    if (at + strlen(name) + 3 < cap) _snprintf_s(buf + at, cap - at, _TRUNCATE, "%s%s", at ? ", " : "", name);
}

static int me_held_one(void *context, const char *name, void *resource)
{
    me_held_load *r = (me_held_load *)context;
    char before[ERROR_TEXT_MAX];
    me_error_text(before, sizeof before);
    __try {
        unsigned char *d = (unsigned char *)resource;
        if (!d) {
            /* Not held: the engine's own find-or-load (the model manager's
             * FindModel runs it on this list), quiet, with a default made on
             * failure as the spawn would. */
            void *list = sh_modserve_resource_list(g_res_head, SH_MODSERVE_HELD_MODEL_TYPE);
            if (!list || !g_materialize) { r->skipped++; return 1; }
            d = (unsigned char *)g_materialize(list, name, 1, 1);
            if (!d) { r->failed++; me_held_note(r->bad, sizeof r->bad, name); return 1; }
        } else {
            /* Loaded again in place: the failed, unloaded and default bits
             * cleared first, as op_load does for a new identity. */
            d[RES_STATE_OFF] = (unsigned char)(d[RES_STATE_OFF] & ~7);
            g_generic_load(d);
        }
        if (d[RES_STATE_OFF] & 5) {
            /* Still the default: not kept, so the next load can read it whole. */
            *(unsigned *)(d + RES_LIFETIME_OFF) = 1;
            r->failed++;
            me_held_note(r->bad, sizeof r->bad, name);
            return 1;
        }
        *(unsigned *)(d + RES_LIFETIME_OFF) = 4;
        sh_modserve_kept_add(d);
        if (resource) r->reloaded++; else r->loaded++;
        me_held_note(r->first, sizeof r->first, name);
    } __except (me_fault_filter(GetExceptionCode())) {
        me_log_fault("held model load", SH_MODSERVE_HELD_MODEL_TYPE, name, before);
        r->faulted++;
    }
    return 1;
}

static void me_hold_models(const char *phase)
{
    static int busy;
    me_held_load r;
    sh_modserve_holds h;
    sh_process_heap_scope heap = { 0 };
    me_closure keeper;
    int n = -1, own;
    if (busy || !g_bound || !g_res_head || !g_generic_load || !sh_modserve_active() || me_on_main() != 1 ||
        me_in_play() == 1)
        return;
    busy = 1;
    memset(&r, 0, sizeof r);
    if (!sh_process_heap_enter(&g_heap, &heap)) {
        me_log("%s: models kept decls hold not loaded (process heap scope unavailable)", phase);
        busy = 0;
        return;
    }
    own = sh_modserve_inplace_keeper(g_mark_depth, 1);
    if (own) me_closure_begin(&keeper);
    __try { n = sh_modserve_held_models(g_res_head, me_held_one, &r, &h); }
    __finally {
        if (own) me_closure_end(&keeper, phase, "held model load");
        if (!sh_process_heap_leave(&heap)) me_log("%s: held model load heap scope did not balance", phase);
    }
    if (n < 0) me_log("%s: resource lists could not be walked; models kept decls hold were not loaded", phase);
    else if (r.skipped)
        me_log("%s: %d model(s) kept decls hold not loaded: the resource find-or-load is unavailable on this build; "
               "they are loaded per map", phase, r.skipped);
    if (n > r.skipped)
        me_log("%s: %d model(s) kept decls hold loaded for the session while their bundles serve them (%d new, %d "
               "loaded again: a default or a map heap's; %d still a default, %d faulted)%s%s%s%s%s", phase,
               r.loaded + r.reloaded, r.loaded, r.reloaded, r.failed, r.faulted, r.first[0] ? ": " : "", r.first,
               strlen(r.first) + 40 >= sizeof r.first ? ", ..." : "", r.bad[0] ? "; still a default: " : "", r.bad);
    busy = 0;
}

/* Keep what the kept decls hold (sh_modserve_holds_raise): after every
 * serving pass, and before the Play level load purges level 1. */
static void me_keep_holds(const char *phase)
{
    sh_modserve_holds h;
    if (!g_res_head) return;
    me_hold_models(phase);
    if (!sh_modserve_holds_raise(g_res_head, &h)) {
        me_log("%s: resource lists could not be walked; what kept decls hold was not checked", phase);
        return;
    }
    if (h.raised)
        me_log("%s: %u resource(s) kept decls hold kept for the session with them (%u kept decl(s), %u name(s) they "
               "need): %s%s", phase, h.raised, h.roots, h.names, h.first,
               strlen(h.first) + 40 >= sizeof h.first ? ", ..." : "");
}

/* Keep what kept objects loaded or looked up (sh_modserve_attrib_close):
 * after every keep by pass or by name, and before the Play level load's
 * purge, which `forget` marks. */
static void me_keep_attrib(const char *phase, int forget)
{
    sh_modserve_attrib a;
    if (!g_res_head || !g_load_orig) return;
    if (!sh_modserve_attrib_close(g_res_head, forget, &a)) {
        me_log("%s: resource lists could not be walked; what kept objects load was not checked", phase);
        return;
    }
    if (!g_attrib_checked) {
        g_attrib_checked = 1;
        if (!a.pairs)
            me_log("%s: the resource load hook saw no load inside another since it was installed; what kept objects "
                   "load is kept only by name", phase);
        else me_log("%s: resource load attribution live (%u load(s) and lookup(s) inside another recorded, %u kept)",
                    phase, a.pairs, a.kept);
    }
    if (a.overflow)
        me_log("%s: more loads were recorded than the product keeps track of; some of what kept objects loaded may "
               "not be kept", phase);
    {
        char left[300];
        unsigned n = sh_modserve_left_to_map(left, sizeof left);
        if (n)
            me_log("%s: %u keep(s) left at level 1 because the map's heap holds what their load allocated (the next "
                   "load recreates them): %s%s", phase, n, left, strlen(left) + 40 >= sizeof left ? ", ..." : "");
    }
    if (a.raised || a.raised_at_load)
        me_log("%s: %u resource(s) kept objects loaded or looked up kept for the session with them (%u as they "
               "loaded; %u followed from %u recorded pair(s), %u kept)%s%s%s", phase, a.raised + a.raised_at_load,
               a.raised_at_load, a.followed, a.pairs, a.kept, a.first[0] ? ": " : "", a.first,
               strlen(a.first) + 40 >= sizeof a.first ? ", ..." : "");
}

static void me_closure_end(me_closure *c, const char *phase, const char *what)
{
    if (c->counted) g_mark_depth--;
    if (c->mark.ok) {
        sh_modserve_raised raised;
        if (sh_modserve_watermark_raise(g_res_head, &c->mark, &raised) && raised.created)
            me_log("%s: %u resource(s) the %s created kept for the session, as at boot (%u new, %u lists): %s", phase,
                   raised.raised, what, raised.created, raised.lists, raised.summary);
        else if (!raised.lists) me_log("%s: resource lists could not be walked after the %s; not kept", phase, what);
    }
    sh_modserve_watermark_free(&c->mark);
    if (!g_mark_depth) {
        me_lookups_close(phase, what);
        me_keep_holds(phase);
        me_keep_attrib(phase, 0);
    }
}

static void me_reparse_early(const char *phase, int in_heap);
static void me_reload_ungated(const char *phase, int in_heap);

static int me_run_plan(sh_modserve_plan *plan, const char *phase)
{
    sh_process_heap_scope heap = { 0 };
    int ready = -1;
    if (!sh_process_heap_enter(&g_heap, &heap)) { me_log("%s: process heap scope unavailable; nothing registered", phase); return -1; }
    g_reparse_phase = phase;
    __try { ready = sh_modserve_plan_run(plan, &g_ops, phase); }
    __finally {
        if (!sh_process_heap_leave(&heap)) { me_log("%s: process heap scope did not balance", phase); ready = -1; }
    }
    return ready;
}

/* Run a plan inside process heap 0: decl objects outlive every map. The
 * bundles' requirements are applied and read back first, outside the scope
 * (as map_bounds.c sets its cvars), and gate-blocked steps are marked.
 * Outside a watermark, the steps an editor row needs that are not registered
 * yet run first inside one (sh_modserve_plan_split): a map pass or a refresh
 * plan that registers them keeps what they create, as the boot editor pass
 * does; the rest of the plan runs after, at the engine's own level. */
static int me_run(sh_modserve_plan *plan, const char *phase)
{
    sh_modserve_gates gates;
    if (!g_bound) { me_log("%s: decl registry unavailable on this build", phase); return -1; }
    if (plan->count) {
        sh_modserve_requirements_apply(me_gate_ops(), phase, &gates);
        sh_modserve_plan_block_unmet(plan, &gates);
    }
    if (!g_mark_depth && plan->count) {
        sh_modserve_plan kept, rest;
        int n = sh_modserve_plan_split(plan, &kept, &rest);
        if (n > 0) {
            me_closure c;
            int ra, rb = 0, part = g_refresh_part;
            me_closure_begin(&c);
            g_refresh_part = SH_MODSERVE_PART_EDITOR;
            ra = me_run_plan(&kept, phase);
            g_refresh_part = part;
            me_closure_end(&c, phase, "decls editor rows need");
            sh_modserve_plan_merge_ready(plan, &kept);
            if (rest.count) {
                (void)sh_modserve_plan_block_failed_needs(&rest);
                rb = me_run_plan(&rest, phase);
                sh_modserve_plan_merge_ready(plan, &rest);
            } else {
                /* The early shadows and gated reloads a pass's sources hook
                 * runs belong to no row: outside the watermark. */
                me_reparse_early(phase, 0);
                me_reload_ungated(phase, 0);
            }
            sh_modserve_plan_free(&kept);
            sh_modserve_plan_free(&rest);
            return ra < 0 || rb < 0 ? -1 : ra + rb;
        }
    }
    return me_run_plan(plan, phase);
}

static int me_in_play(void);

/* 1 during SnapMap Play, 0 otherwise, -1 unknown. */
int sh_modserve_engine_in_play(void)
{
    return me_in_play();
}

int sh_modserve_engine_set_cvar(const char *cvar, const char *value)
{
    return gate_set(NULL, cvar, value) == SH_MODSERVE_GATE_SET;
}

void sh_modserve_play_begin(void)
{
    int on_main;
    sh_modhud_scene_play_begin();
    sh_modscene_place_play_begin();
    on_main = me_on_main();
    /* Companions first: Play precedes every spawn burst of the session
     * (report ADD-V1) and does not parse the map again, so the map gate alone
     * misses a mod installed since the last map load. */
    if (on_main == 1) (void)sh_companions_ensure("play");
    /* Map scope: entities placed since the load or the last save may list a
     * bundle; its shadows and requirements come in before the level loads. */
    if (on_main == 1 && sh_modserve_scope_source(SH_MODSERVE_AT_PLAY) > 0) (void)sh_modserve_scope_apply("play");
    /* Mod scripts: the mods this Play declares (the listing just set) are
     * prepared before its spawns, and the companions' pass arms the spawn
     * chain for them when they need it. */
    if (on_main == 1) {
        sh_script_hooks_gate(SH_SCRIPT_GATE_PLAY);
        if (sh_script_hooks_open()) (void)sh_companions_ensure("play scripts");
    }
    /* What the editor's spawns created for a kept decl since the last pass
     * is still level 1: kept before the level load's purge frees it. */
    if (on_main == 1 && g_bound && sh_modserve_active()) me_keep_holds("play");
    /* What kept objects loaded since then is kept the same way; the pairs
     * are forgotten, since the purge frees the level-1 parents they name. */
    if (on_main == 1 && g_bound) me_keep_attrib("play", 1);
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

/* Every resource the engine's lookup returns on the main thread while a pass
 * is open is recorded (sh_modserve_lookups_note); one it returns inside a load
 * is attributed to that load (sh_modserve_attrib_note) and kept at once when
 * the load is a kept object's. */
static void *me_lookup_hook(void *list, const char *name, uint64_t quiet)
{
    void *r = g_lookup_orig(list, name, quiet);
    if (r && (sh_modserve_lookups_recording() || sh_modserve_attrib_depth()) && me_on_main() == 1) {
        if (sh_modserve_lookups_recording()) sh_modserve_lookups_note(r);
        sh_modserve_attrib_found(r);
    }
    return r;
}

/* The engine's load nesting on the main thread: ResourceMaterialize creates a
 * resource it does not find and loads it here, and that load materializes
 * what the resource uses, so a load opened inside another is that one's
 * (report F9: the chainsaw gore md6Def loads its mesh, whose load creates its
 * material). The pair is recorded; a kept parent keeps the child, raised
 * when its own load returns (the load's failure path reconstructs the object
 * at level 1). The depth is restored however the load leaves. */
/* The heap a load on the main thread allocates from with heap -1: the top of
 * idMemLocal's 32-entry stack (ids at +0x44, depth at +0xC4), else 0, as
 * the allocator decides (VK 0x140328530). -1 when it cannot be read. */
static int me_heap_now(void)
{
    const unsigned char *self;
    int depth, id = 0;
    if (!g_heap.get) return -1;
    __try {
        self = (const unsigned char *)g_heap.get();
        if (!self) return -1;
        depth = *(const int *)(self + 0xc4);
        if (depth >= 1 && depth <= 32) id = *(const int *)(self + 0x44 + (depth - 1) * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return id;
}

static void me_load_hook(void *res)
{
    int depth, kept = 0;
    if (me_on_main() != 1) { g_load_orig(res); return; }
    depth = sh_modserve_attrib_enter(res, me_heap_now(), &kept);
    __try { g_load_orig(res); }
    __finally { sh_modserve_attrib_leave(depth); }
    if (kept) (void)sh_modserve_attrib_keep(res);
}

static void me_ensure_load(void)
{
    void *tramp;
    sh_modserve_stats st;
    if (g_load_orig || !g_load_sig_ok) return;
    sh_modserve_get_stats(&st);
    if (!st.files) return;
    tramp = sh_prepare_detour_sig(&g_load_sig, (void *)me_load_hook, LOAD_STOLEN);
    if (!tramp) {
        me_log("resource load detour refused; what kept objects load is kept only by name");
        g_load_sig_ok = 0;
        return;
    }
    g_load_orig = (generic_load_fn)tramp;
    if (sh_commit_detour(tramp) != B2_PATCH_OK) {
        if (sh_uninstall_detour(tramp)) g_load_orig = NULL;
        g_load_sig_ok = 0;
        me_log("resource load detour commit failed; what kept objects load is kept only by name");
        return;
    }
    me_log("resource load hook installed (rva 0x%X, %d-byte window)", g_load_sig.rva, LOAD_STOLEN);
}

static void me_ensure_lookup(void)
{
    void *tramp;
    sh_modserve_stats st;
    if (g_lookup_orig || !g_lookup_sig_ok) return;
    sh_modserve_get_stats(&st);
    if (!st.files) return;
    tramp = sh_prepare_detour_sig(&g_lookup_sig, (void *)me_lookup_hook, LOOKUP_STOLEN);
    if (!tramp) {
        me_log("resource lookup detour refused; what a mod pass looks up is kept only by name");
        g_lookup_sig_ok = 0;
        return;
    }
    g_lookup_orig = (lookup_fn)tramp;
    if (sh_commit_detour(tramp) != B2_PATCH_OK) {
        if (sh_uninstall_detour(tramp)) g_lookup_orig = NULL;
        g_lookup_sig_ok = 0;
        me_log("resource lookup detour commit failed; what a mod pass looks up is kept only by name");
        return;
    }
    me_log("resource lookup hook installed (rva 0x%X, %d-byte window)", g_lookup_sig.rva, LOOKUP_STOLEN);
}

/* The probe is detoured only once the index holds a decl, so an empty mods
 * folder leaves the engine's probe untouched; the lookup and load likewise. */
static void me_ensure_probe(void)
{
    void *tramp;
    sh_modserve_stats st;
    me_ensure_lookup();
    me_ensure_load();
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
    sh_companions_refresh();   /* the companion table follows the index at the next gate or owner spawn */
    sh_strids_request_refresh();
    if (me_on_main() == 1) { (void)sh_strids_refresh_pending(); return; }
    sh_apply_engine_get_run_on_main(&run);
    if (run) (void)run(NULL, me_strings_main, NULL, 1500, &result);
}

static void me_console_line(const char *line) { sh_printf("%s\n", line); }

int sh_modserve_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    size_t i;
    sh_modserve_set_console(me_console_line);
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
    /* Optional: without it the held models are loaded only when the engine has created them. */
    g_materialize = (materialize_fn)me_clean(results, count, "ResourceMaterialize");
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
    /* The resource lock: every list walk here and in modserve.c holds it. */
    g_res_lock = (void *)glb_resolve(module_base, "resource_lock", NULL);
    sh_modserve_set_resource_lock(g_res_lock);
    if (!g_res_lock) me_log("resource lock unresolved; the resource lists are walked without it");
    /* The editor's type-info cache: the default trees of the rows a mod
     * registers after the game built it come from the product (modtypeinfo.c). */
    sh_modtypeinfo_install(results, count, module_base);
    /* The served-bytes ledger: complete only when nothing was served before this point. */
    {
        sh_modserve_stats st;
        sh_modserve_get_stats(&st);
        sh_modledger_set_complete(st.opens_served == 0);
        sh_modserve_set_serve_listener(me_serve_listener);
        me_log("served-bytes ledger bound (%s)", st.opens_served ? "the provider served files before it; a held name "
               "with no record is refreshed rather than trusted" : "every serve of the session is recorded");
    }
    g_bound = g_ctor && g_dtor && g_anchor && g_type && g_register && g_source && g_decl && g_generic_load &&
              sh_process_heap_bind(&g_heap, results, count, module_base) && g_main_thread_at;
    for (i = 0; i < count; ++i)
        if (results[i].name && !strcmp(results[i].name, "DeclResourceProbe") && results[i].status == SIG_OK) {
            g_probe_sig = results[i];
            g_probe_sig_ok = 1;
        }
    for (i = 0; i < count; ++i)
        if (results[i].name && !strcmp(results[i].name, "ResourceLookup") && results[i].status == SIG_OK) {
            g_lookup_sig = results[i];
            g_lookup_sig_ok = 1;
        }
    for (i = 0; i < count; ++i)
        if (results[i].name && !strcmp(results[i].name, "ResourceGenericLoad") && results[i].status == SIG_OK) {
            g_load_sig = results[i];
            g_load_sig_ok = 1;
        }
    if (!g_bound) me_log("decl registration unavailable on this build (registry, heap scope or main thread unresolved)");
    if (!g_probe_sig_ok) me_log("existence probe unavailable (DeclResourceProbe is not a clean match)");
    if (!g_lookup_sig_ok)
        me_log("resource lookup unavailable (ResourceLookup is not a clean match); what a mod pass looks up is kept "
               "only by name");
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
typedef struct me_reparse {
    sh_modledger_tally tally;   /* per type, for the log (copies the type: dependents pass a stack buffer) */
    int reparsed, through_provider, failed, faulted, pending, skipped, restored;
    int surfaces;               /* surfaces offered and left alone (sh_modserve_is_surface) */
} me_reparse;

/* Every in-place load keeps the resource's lifetime: ResourceGenericLoad's
 * failure path reconstructs the object (ResourceReconstruct, VK 0x1416c67f0),
 * and idResource's constructor (VK 0x1416c5580) writes level 1 or 2, so a
 * permanent decl that failed a re-parse would be destroyed by the next map
 * unload while everything permanent that points at it (the menu's
 * collectibles, an editor row) keeps the pointer. */
static void me_load_in_place(unsigned char *d, const char *what, const char *type, const char *name)
{
    unsigned was = 0, left;
    d[RES_STATE_OFF] = (unsigned char)(d[RES_STATE_OFF] & ~3);
    left = sh_modserve_load_keep_lifetime(d, g_generic_load, &was);
    if (left != was)
        me_log("%s: %s '%s' kept at lifetime %u (the load left %u)", what, type, name, was, left);
}

static int me_reparse_one(void *context, const char *type, const char *name, const char *request, const char *bundle_id)
{
    me_reparse *r = (me_reparse *)context;
    char before[ERROR_TEXT_MAX];
    int result = 0;
    /* A surface is the game's own decl under its own name: never re-parsed
     * by any pass (ADD-L11); the game parses it from these same bytes. */
    if (request && sh_modserve_is_surface(request)) { r->surfaces++; return 1; }
    me_error_text(before, sizeof before);
    __try {
        void *m = me_manager(type), *decl = m ? g_decl(m, name, 0) : NULL;
        unsigned char *d = (unsigned char *)decl, state;
        char model[200], why[ERROR_TEXT_MAX + 80];
        if (!d) return 0;
        state = d[RES_STATE_OFF];
        if (state & 2) { r->pending++; return 0; }   /* not parsed yet: the provider will serve it */
        /* A permanent decl served the game's own bytes: a re-parse would only
         * rebind it to decls a pass may have created at level 1, which the
         * next map unload frees under it (the menu collectibles' unlockModels). */
        if (*(unsigned *)(d + RES_LIFETIME_OFF) == 4 && sh_modserve_is_restore(request) == 1) {
            me_log("reparse: %s '%s' (%s) left as the game parsed it: permanent, and its bundle serves the game's own "
                   "bytes", type, name, bundle_id);
            return 1;
        }
        /* An animWeb naming a model the engine cannot find would come out of
         * its parse empty (its nodes freed first, nothing rebuilt): the
         * game's own web stays. */
        if (!_stricmp(type, "animWeb") && sh_modserve_animweb_check(request, &g_ops, model, sizeof model) == 1) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "it names model '%s', which SnapMap cannot load; the game's own "
                        "animWeb is kept", model);
            (void)sh_modserve_withdraw(request, type, name, why);
            r->skipped++;
            return 1;
        }
        {
            int refused;
            me_load_in_place(d, "reparse", type, name);
            state = d[RES_STATE_OFF];
            refused = me_refused(before, type, name);
            r->reparsed++;
            (void)sh_modledger_tally_add(&r->tally, type, name);
            if (sh_modserve_open_count(request) > 0) r->through_provider++;
            if ((state & 1) || refused) {
                r->failed++;
                me_log("reparse: %s '%s' (%s) failed to parse the bundle's bytes (state 0x%02x)%s%s", type, name,
                       bundle_id, state, refused ? ": " : "", refused ? g_refusal : "");
                /* The bundle's bytes stop being served and the engine parses
                 * its own again, so the decl is whole. */
                _snprintf_s(why, sizeof why, _TRUNCATE, "the game refused its bytes%s%s; the game's own decl is restored",
                            refused ? ": " : "", refused ? g_refusal : "");
                if (sh_modserve_withdraw(request, type, name, why)) {
                    me_load_in_place(d, "restore", type, name);
                    r->restored++;
                }
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
    if (!r.reparsed && !r.faulted && !r.skipped) return;
    for (i = 0; i < r.tally.ntypes; ++i)
        me_log("reparsed %d %s decl(s) the engine parsed before the serving index (%s%s)", r.tally.types[i].count,
               r.tally.types[i].type, r.tally.types[i].names,
               sh_modledger_tally_truncated(&r.tally.types[i]) ? ", ..." : "");
    me_log("%s: early decls: %d re-parsed in %.1f ms (%d read through the provider, %d failed, %d restored, %d faulted, "
           "%d withdrawn unparsed); %d of %d unserved shadow(s) not parsed yet", phase, r.reparsed, me_now_ms() - t0,
           r.through_provider, r.failed, r.restored, r.faulted, r.skipped, r.pending, offered);
}

/* Models and images a gated bundle serves that the engine loaded before the
 * gates were live (modserve.h, sh_modserve_each_ungated_raw): each one the
 * resource lists hold is loaded again in place, which resolves its materials
 * and pixels with the blacklists off. Matched by name within the list of its
 * class (idMD6Model, idImage), since a material and an image can share one. */
#define ME_RAW_MAX 4096
#define ME_RAW_MODEL 0
#define ME_RAW_IMAGE 1
#define ME_RAW_ANIM  2
typedef struct me_raw_name { const char *name, *request; int kind, hit, done; } me_raw_name;
typedef struct me_raw {
    me_raw_name *names; int n, cap;
    int reloaded, models, images, anims, failed, faulted;
    int refused;          /* anims whose served bytes the engine's Load would refuse: the held one is kept */
    int anims_level1;     /* anims held below level 4: read from the current bytes at their next load */
    char first[700];
} me_raw;

static int me_raw_kind(const char *type)
{
    if (type && !_stricmp(type, "image")) return ME_RAW_IMAGE;
    if (type && !_stricmp(type, "anim")) return ME_RAW_ANIM;
    return ME_RAW_MODEL;
}

static int me_raw_collect(void *context, const char *type, const char *name, const char *request, const char *bundle_id)
{
    me_raw *r = (me_raw *)context;
    (void)bundle_id;
    /* A surface model or image is collected like any other: the gated reload
     * is the one pass that loads the game's own bytes again, inside its
     * keeper, so a boot-held stock mesh gets its blacklisted materials
     * (ADD-L13). */
    if (r->n < r->cap) {
        r->names[r->n].name = name;
        r->names[r->n].request = request;
        r->names[r->n].kind = me_raw_kind(type);
        r->names[r->n].hit = 0;
        r->names[r->n].done = 0;
        r->n++;
    }
    return 0;
}

static int me_raw_settle(void *context, const char *type, const char *name, const char *request, const char *bundle_id)
{
    (void)context; (void)type; (void)name; (void)request; (void)bundle_id;
    return 1;
}

static void me_raw_note(me_raw *r, const char *name)
{
    if (strlen(r->first) + strlen(name) + 3 < sizeof r->first)
        _snprintf_s(r->first + strlen(r->first), sizeof r->first - strlen(r->first), _TRUNCATE, "%s%s",
                    r->first[0] ? ", " : "", name);
}

/* An anim is reloaded only when it is held for the session (level 4: stock,
 * or raised with the editor closure); a level-1 anim is freed at every map
 * transition and read again from the current bytes. Its served bytes are
 * checked first: a Load that fails frees the data the held anim points at. */
static int me_anim_ready(me_raw *r, unsigned char *d, const me_raw_name *n)
{
    char why[160];
    unsigned char *bytes;
    size_t len = 0;
    int ok;
    if (*(const unsigned *)(d + RES_LIFETIME_OFF) != 4) { r->anims_level1++; return 0; }
    bytes = n->request ? me_peek(n->request, &len) : NULL;
    if (!bytes) _snprintf_s(why, sizeof why, _TRUNCATE, "its bundle's bytes could not be read");
    ok = bytes && me_anim_bytes_ok(bytes, len, why, sizeof why);
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    if (!ok) {
        r->refused++;
        me_log("reload: anim '%s' kept as the engine holds it: the served %s is not one its Load accepts (%s)", n->name,
               n->request ? n->request : "file", why);
    }
    return ok;
}

static void me_raw_one(void *context, const char *list_class, void *resource, const char *name)
{
    me_raw *r = (me_raw *)context;
    int i, kind;
    if (!list_class) return;
    if (!_stricmp(list_class, "idImage")) kind = ME_RAW_IMAGE;
    else if (!_stricmp(list_class, "idMD6Model")) kind = ME_RAW_MODEL;
    else if (!_stricmp(list_class, "idMD6Anim")) kind = ME_RAW_ANIM;
    else return;
    for (i = 0; i < r->n; ++i) {
        unsigned char *d;
        if (r->names[i].hit || r->names[i].kind != kind || !me_name_eq(r->names[i].name, name)) continue;
        r->names[i].hit = 1;
        d = (unsigned char *)resource;
        __try {
            if (d[RES_STATE_OFF] & 2) return;   /* not loaded: its next load sees the live gates and bytes */
            if (kind == ME_RAW_ANIM && !me_anim_ready(r, d, &r->names[i])) return;
            me_load_in_place(d, "reload", list_class, name);
            if (d[RES_STATE_OFF] & 1) r->failed++;
            r->reloaded++;
            r->names[i].done = 1;
            if (r->names[i].request) (void)sh_modledger_bind(r->names[i].request, d);
            if (kind == ME_RAW_IMAGE) r->images++;
            else if (kind == ME_RAW_ANIM) r->anims++;
            else r->models++;
            me_raw_note(r, name);
        } __except (me_fault_filter(GetExceptionCode())) {
            r->faulted++;
            me_log("reload: %s '%s' FAULTED (exception 0x%08lx)", list_class, name, (unsigned long)g_fault_code);
        }
        return;
    }
}

/* After anims reloaded in place: the md6Def and animWeb decls the open map
 * serves that name one are parsed again, so whatever they built from the old
 * data is rebuilt from the new (which objects cache an anim's data pointer
 * is not established, report ADD-G1). Inside process heap 0, main thread. */
#define ME_DEP_SEEN 256
typedef struct me_dep {
    const me_raw *raw;
    me_reparse *decls;
    int found;
    const char *seen[ME_DEP_SEEN];
    int nseen;
} me_dep;

static void me_dep_visit(void *context, const char *request, uint64_t size, const char *bundle_id,
                         const char *bundle_hash)
{
    me_dep *c = (me_dep *)context;
    unsigned char *body;
    size_t len = 0;
    int i;
    (void)size; (void)bundle_hash;
    if (strncmp(request, "generated/decls/md6def/", 23) && strncmp(request, "generated/decls/animweb/", 24)) return;
    if (sh_modserve_is_surface(request)) return;
    for (i = 0; i < c->nseen; ++i) if (!strcmp(c->seen[i], request)) return;
    body = me_peek(request, &len);
    if (!body) return;
    for (i = 0; i < c->raw->n; ++i) {
        char type[64], name[SH_MB_REQUEST_MAX + 1];
        if (c->raw->names[i].kind != ME_RAW_ANIM || !c->raw->names[i].done ||
            !me_body_names(body, len, c->raw->names[i].name))
            continue;
        if (sh_mb_logical_name(request, type, sizeof type, name, sizeof name)) {
            if (c->nseen < ME_DEP_SEEN) c->seen[c->nseen++] = request;
            (void)me_reparse_one(c->decls, type, name, request, bundle_id);
            c->found++;
        }
        break;
    }
    HeapFree(GetProcessHeap(), 0, body);
}

static int me_anim_dependents(const me_raw *raw, me_reparse *decls)
{
    me_dep c;
    if (!raw->anims) return 0;
    memset(&c, 0, sizeof c);
    c.raw = raw;
    c.decls = decls;
    sh_modserve_each_served(me_dep_visit, &c);
    return c.found;
}

static void me_reload_ungated(const char *phase, int in_heap)
{
    me_raw r;
    sh_process_heap_scope heap = { 0 };
    me_closure keeper;
    int own;
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
    /* A reload in place of a held model resolves its materials and images
     * again: whatever that creates must live as long as the held model, or
     * the next purge frees images a permanent material still points at (the
     * cold-launch crash, ADD-G6). Outside an editor pass's watermark this
     * reload opens its own keeper (sh_modserve_inplace_keeper). */
    own = sh_modserve_inplace_keeper(g_mark_depth, r.n);
    if (own) me_closure_begin(&keeper);
    __try { (void)sh_modserve_resource_each(g_res_head, me_raw_one, &r); }
    __finally {
        if (own) me_closure_end(&keeper, phase, "gated reload");
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

/* A map pass (modserve_activate_bundles): the engine's purge has run, the
 * departed map's entities are gone and the next map's are not spawned, so a
 * held anim may be reloaded in place (report ADD-G1). Anims marked at any
 * other point wait for it. */
static int g_anim_pass;
/* Anims the last refresh outside a map pass left waiting: the map pass says
 * when it has settled them, so the count does not read as stuck. */
static int g_anims_waited;

typedef struct me_refresh {
    me_reparse decls;
    me_raw raw;
    int stage;            /* 0 decls and collect, 1 settle the collected models, images and anims */
    int not_held, gated;
    int kept;             /* held with the bytes served now (the ledger): nothing to reload */
    int game;             /* never served by a mod this session (the ledger): not refreshed */
    int anims_waiting;    /* anims marked outside a map pass */
    int dependents;       /* md6Def and animWeb decls re-parsed after their anims reloaded */
} me_refresh;

static int me_refresh_visit(void *context, const sh_modserve_refresh_item *it)
{
    me_refresh *c = (me_refresh *)context;
    if (!it->gates_live) { if (!c->stage) c->gated++; return 0; }
    if (it->kind == SH_MODSERVE_RF_ANIM && !g_anim_pass) { if (!c->stage) c->anims_waiting++; return 0; }
    if (it->kind == SH_MODSERVE_RF_DECL) {
        int before = c->decls.reparsed + c->decls.faulted;
        sh_modtypeinfo_decl_changed(it->type, it->name);   /* its default tree follows the new bytes */
        (void)me_reparse_one(&c->decls, it->type, it->name, it->request, it->bundle_id);
        if (c->decls.reparsed + c->decls.faulted == before) c->not_held++;   /* its next parse reads the current bytes */
        return 1;
    }
    if (c->stage) return 1;
    /* Raw resources: the ledger says whether the bytes the engine was given
     * under this name differ from the bytes served now. (Decls keep the
     * marks alone: a child is marked for its parent's change, not its own.) */
    switch (me_ledger_decision(it)) {
    case SH_MODLEDGER_KEEP: c->kept++; return 1;
    case SH_MODLEDGER_GAME: c->game++; return 1;
    default: break;
    }
    (void)me_raw_collect(&c->raw, it->type, it->name, it->request, it->bundle_id);
    return 0;
}

static int me_refresh_count(void *context, const sh_modserve_refresh_item *it)
{
    (void)it;
    (*(int *)context)++;
    return 0;   /* counted only: nothing is settled */
}

static void me_refresh_apply_part(const char *phase, int in_heap, sh_modserve_refresh *out, int part)
{
    me_refresh c;
    sh_process_heap_scope heap = { 0 };
    me_closure keeper;
    double t0 = me_now_ms(), t1;
    int i, items = 0, own;
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
    /* A re-parse or reload in place of a held resource binds it to what it
     * resolves now; outside a watermark (the rest of a refresh, a scope
     * change) that is kept by a keeper of its own, as inside the editor's
     * pass (ADD-G6). Only when there is something to apply. */
    (void)sh_modserve_each_refresh_part(me_refresh_count, &items, part);
    own = sh_modserve_inplace_keeper(g_mark_depth, items);
    if (own) me_closure_begin(&keeper);
    __try {
        if (sh_modserve_each_refresh_part(me_refresh_visit, &c, part) && c.raw.n && g_res_head) {
            (void)sh_modserve_resource_each(g_res_head, me_raw_one, &c.raw);
            c.dependents = me_anim_dependents(&c.raw, &c.decls);
        }
    } __finally {
        if (own) me_closure_end(&keeper, phase, "refresh");
        if (!in_heap && !sh_process_heap_leave(&heap)) me_rlog("refresh: heap scope did not balance at the %s", phase);
    }
    t1 = me_now_ms();
    c.stage = 1;
    if (c.raw.n && g_res_head) (void)sh_modserve_each_refresh_part(me_refresh_visit, &c, part);
    if (c.decls.reparsed || c.decls.faulted || c.not_held) {
        for (i = 0; i < c.decls.tally.ntypes; ++i)
            me_rlog("refresh: re-parsed %d %s decl(s) whose served bytes or parent changed (%s%s)",
                   c.decls.tally.types[i].count, c.decls.tally.types[i].type, c.decls.tally.types[i].names,
                   sh_modledger_tally_truncated(&c.decls.tally.types[i]) ? ", ..." : "");
        me_rlog("refresh: decls: %d re-parsed at the %s in %.1f ms (%d read through the provider, %d failed, %d faulted; "
               "%d not held, read at their next parse%s)", c.decls.reparsed, phase, t1 - t0, c.decls.through_provider,
               c.decls.failed, c.decls.faulted, c.not_held, c.dependents ? "; includes the md6Def and animWeb decls "
               "naming a reloaded anim" : "");
    }
    if (c.raw.n)
        me_rlog("refresh: models, images and anims: reloaded %d model(s), %d image(s) and %d anim(s) at the %s (%d failed, "
               "%d faulted, %d anim(s) refused before loading, %d anim(s) below level 4 read at their next load; %d of %d "
               "not held, read at their next load)%s%s%s", c.raw.models, c.raw.images, c.raw.anims, phase, c.raw.failed,
               c.raw.faulted, c.raw.refused, c.raw.anims_level1, c.raw.n - c.raw.reloaded - c.raw.faulted, c.raw.n,
               c.raw.first[0] ? ": " : "", c.raw.first,
               c.raw.reloaded > 0 && strlen(c.raw.first) + 40 >= sizeof c.raw.first ? ", ..." : "");
    if (c.dependents)
        me_rlog("refresh: %d md6Def and animWeb decl(s) naming a reloaded anim re-parsed at the %s", c.dependents, phase);
    if (c.kept || c.game)
        me_rlog("refresh: ledger: %d model(s), image(s) and anim(s) already hold the bytes served now, %d were never "
               "given a mod's bytes; not reloaded", c.kept, c.game);
    if (c.anims_waiting) {
        g_anims_waited = c.anims_waiting;
        if (!strcmp(phase, "map load"))
            me_rlog("refresh: %d anim(s) wait for this load's map pass, where held anims are reloaded in place",
                    c.anims_waiting);
        else
            me_rlog("refresh: %d anim(s) wait for the next map load, where held anims are reloaded in place", c.anims_waiting);
    }
    if (!g_res_head && c.raw.n)
        me_rlog("refresh: %d model(s), image(s) and anim(s) not reloaded (decl_resource_head unresolved)", c.raw.n);
    if (c.gated)
        me_rlog("refresh: %d change(s) wait for their bundles' requirements, applied at the next editor pass or map load",
               c.gated);
    if (out) {
        out->reparsed += c.decls.reparsed;
        out->reloaded += c.raw.reloaded;
        out->failed += c.decls.failed + c.decls.faulted + c.raw.failed + c.raw.faulted + c.raw.refused;
    }
    free(c.raw.names);
}

/* Apply a refresh. Outside a watermark, the editor closure's changes (rows
 * and what they need, modserve.h) are applied first inside one, so what their
 * re-parse and reload create is kept for the session as at boot, then the
 * rest at the engine's own level. Inside a watermark: the part it covers. */
static void me_refresh_apply(const char *phase, int in_heap, sh_modserve_refresh *out)
{
    if (g_mark_depth) { me_refresh_apply_part(phase, in_heap, out, g_refresh_part); return; }
    if (g_bound && me_on_main() == 1 && sh_modserve_refresh_editor_pending() > 0) {
        me_closure c;
        me_closure_begin(&c);
        me_refresh_apply_part(phase, in_heap, out, SH_MODSERVE_PART_EDITOR);
        me_closure_end(&c, phase, "editor rows' refresh");
        me_refresh_apply_part(phase, in_heap, out, SH_MODSERVE_PART_REST);
        return;
    }
    me_refresh_apply_part(phase, in_heap, out, SH_MODSERVE_PART_ALL);
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

static void me_keep_rows(const char *phase);

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
    /* The rows' references before anything else creates them: the editor
     * redraws a placed entity of a changed row from its new references, and
     * an object the editor creates for it would be map-scoped. */
    me_keep_rows(phase);
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
    (void)sh_companions_ensure("return to the editor");
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
    if (main == 1) (void)sh_companions_ensure("rescan");
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
    if (!d.changed && !d.gone && !d.added_decls && !d.pending && !d.changed_scripts) {
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
    r.needs_map_reload = d.changed_map + d.changed_anims + (r.palette < 0 ? rows : 0);
    /* A removed decl name stays registered (the engine cannot unregister a
     * name) but nothing serves it: said as such, not as a needed restart. */
    r.needs_restart = d.changed_restart;
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
        me_rlog("refresh: needs a map reload: %d changed movie(s), %d changed anim(s) (reloaded in place at the next map "
                "load)%s", d.changed_map, d.changed_anims,
                r.palette < 0 ? ", and the editor rows until the palette is rebuilt" : "");
        me_refresh_note(&r, "%d change(s) need a map reload%s", r.needs_map_reload,
                        r.palette < 0 ? " ('sh_mods palette rebuild' applies the editor rows)" : "");
    }
    if (r.needs_restart) {
        me_rlog("refresh: needs a restart: %d changed file(s) nothing reloads in this process%s%s", d.changed_restart,
                d.restart_first[0] ? ": " : "", d.restart_first);
        me_refresh_note(&r, "%d change(s) need a restart", r.needs_restart);
    }
    if (d.changed_scripts) {
        /* A Play reads its scripts when it starts (script_source.c): a change
         * during Play runs at the next one, with no restart. */
        me_rlog("refresh: %d changed mod script file(s) run from the next Play", d.changed_scripts);
        me_refresh_note(&r, "%d script change(s) run from the next Play", d.changed_scripts);
    }
    if (d.gone_names) {
        me_rlog("refresh: %d removed decl name(s) stay registered until the game restarts; nothing serves them",
                d.gone_names);
        me_refresh_note(&r, "%d removed name(s) stay registered until the game restarts; nothing serves them",
                        d.gone_names);
    }
    me_rlog("refresh: summary: %d changed, %d new, %d gone, %d inheriting decl(s) follow their parents; %d "
            "re-parsed, %d reloaded, %d registered, palette %s, %d need a map reload, %d need a restart, %d removed "
            "name(s) stay registered until the game restarts, %d pending",
            d.changed, d.added, d.gone, d.dependents, r.reparsed, r.reloaded, r.registered,
            r.palette > 0 ? "rebuilt" : r.palette < 0 ? "refused" : rows ? "deferred" : "unchanged",
            r.needs_map_reload, r.needs_restart, d.gone_names, r.pending);
    if (out) *out = r;
    return r.state;
}

/* A map-scope change (modserve.h): requirements first, so a bundle whose
 * shadows come in has its gates live before its models reload, then what the
 * listing marked. */
int sh_modserve_scope_apply(const char *phase)
{
    sh_modserve_refresh r;
    sh_modserve_diff d;
    sh_modserve_gates g;
    int held;
    if (!phase) phase = "map";
    if (me_on_main() != 1) {
        me_log("scope: %s: not applied here (not on the main thread); the next registration pass applies it", phase);
        return -1;
    }
    memset(&r, 0, sizeof r);
    memset(&g, 0, sizeof g);
    held = sh_modserve_requirements_held();
    if (held) (void)sh_modserve_requirements_apply(me_gate_ops(), phase, &g);
    /* The stock banks the declared bundles' borrowed sound events need: a mod
     * added to the open map (Use in this map) has its sounds at the next Play
     * without a map load, as the map pass gives a loaded map (modsound.h). */
    if (g_bound) {
        int n = sh_modserve_scope_hashes(NULL, 0);
        if (n > 0) {
            char (*h)[SH_MODSERVE_HASH_HEX + 1] = (char (*)[SH_MODSERVE_HASH_HEX + 1])calloc((size_t)n, sizeof *h);
            const char **list = (const char **)calloc((size_t)n, sizeof *list);
            if (h && list) {
                int i;
                n = sh_modserve_scope_hashes(h, n);
                for (i = 0; i < n; ++i) list[i] = h[i];
                (void)sh_modsound_stock_before_map(list, n);
            }
            free(list);
            free(h);
        }
    }
    sh_modserve_last_diff(&d);
    if (!d.pending && !held) return 0;
    if (g_bound && d.pending) {
        me_refresh_apply(phase, 0, &r);
        me_reload_ungated(phase, 0);
    }
    sh_modserve_last_diff(&d);
    me_log("scope: %s: %d decl(s) re-parsed, %d model(s) and image(s) reloaded, %d requirement cvar(s) changed, "
           "%d command(s) run, %d still pending%s", phase, r.reparsed, r.reloaded, g.changed, g.commands, d.pending,
           g_bound ? "" : " (decl registration is unavailable on this build)");
    return 1;
}

int sh_modserve_scope_activate(const char *phase)
{
    char (*h)[SH_MODSERVE_HASH_HEX + 1] = NULL;
    const char **list = NULL;
    sh_modserve_plan plan;
    int n, i, unknown = 0;
    if (!phase) phase = "map";
    if (me_on_main() != 1 || !g_bound || me_in_play() == 1 || !sh_modserve_scope_map_open())
        return sh_modserve_scope_apply(phase);
    n = sh_modserve_scope_hashes(NULL, 0);
    if (n > 0) {
        h = (char (*)[SH_MODSERVE_HASH_HEX + 1])calloc((size_t)n, sizeof *h);
        list = (const char **)calloc((size_t)n, sizeof *list);
    }
    if (n > 0 && h && list) {
        n = sh_modserve_scope_hashes(h, n);
        for (i = 0; i < n; ++i) list[i] = h[i];
        if (sh_modserve_plan_bundles(list, n, &plan, &unknown) >= 0) {
            if (plan.count) {
                double t0 = me_now_ms();
                /* As a map load runs it: requirements, the sources, the
                 * pending re-parses (op_sources_published), then the loads. */
                int ready = me_run(&plan, phase);
                me_log("scope: %s: the map's %d declared bundle(s) activated (%d steps, %d ready, %d blocked) in %.1f ms",
                       phase, n - unknown, plan.count, ready < 0 ? 0 : ready, plan.blocked, me_now_ms() - t0);
            }
            sh_modserve_plan_free(&plan);
        }
    } else if (n > 0) me_log("scope: %s: activation skipped (out of memory)", phase);
    free(list);
    free(h);
    return sh_modserve_scope_apply(phase);
}

/* The plan's sources are registered; its loads have not run (me_run). A
 * rescan's pending changes are applied first, so a file whose bytes changed
 * is re-parsed as a change, not as an early decl. */
static void op_sources_published(void *context)
{
    (void)context;
    me_refresh_apply(g_reparse_phase, 1, NULL);
    /* Inside a kept plan part (me_run) only the editor closure is applied;
     * the early shadows and gated reloads run with the rest of the plan. */
    if (g_mark_depth && g_refresh_part == SH_MODSERVE_PART_EDITOR) return;
    me_reparse_early(g_reparse_phase, 1);
    me_reload_ungated(g_reparse_phase, 1);
}

/* ------------------------------------------------------------------ palette */

/* Keep the decls an editor row points at for the session (modserve.h,
 * sh_modserve_editor_refs): an object the engine created at level 1 after
 * boot is raised to 4, as everything a stock row names was at the boot
 * promotion. The row's parse normally made the object; one it did not is
 * created by name first. Only the level changes: contents load on demand. */
typedef struct me_keep {
    int kept, permanent, created, missing, loaded;
    int older;                            /* kept from level 1 although it predates the watermark */
    const sh_modserve_watermark *mark;    /* the pass's watermark, or NULL */
} me_keep;

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
            /* Created before the watermark (by an open map's own load, say):
             * what its earlier parse created is not new to the watermark and
             * stays at the engine's level. A row names an md6Def by name only
             * (the editor finds it at entity Init, VK FUN_14123b340 / GL
             * FUN_14123a7e0, without creating it), so such an md6Def is left
             * whole at level 1: kept alone it would outlive its MD6 model, and
             * the next Play built an animator on the freed model (R315 live,
             * VK DOOM+0x173fb46). An animWeb or material the row holds by
             * pointer is kept, as before. */
            int older = k->mark && sh_modserve_watermark_has(k->mark, decl);
            if (older) k->older++;
            if (older && !_stricmp(type, "md6Def")) {
                me_log("editor: %s '%s' left at level 1 (editor row '%s' names it): it was created before this pass, "
                       "and keeping it without the resources its parse created would outlive them", type, name, row);
                return;
            }
            *(unsigned *)((unsigned char *)decl + RES_LIFETIME_OFF) = 4;
            sh_modserve_kept_add(decl);
            k->kept++;
            if (older)
                me_log("editor: %s '%s' kept for the session (editor row '%s' points at it); it was created at level 1 "
                       "before this pass, so what its parse created is kept by the names its file needs", type, name, row);
            else me_log("editor: %s '%s' kept for the session (editor row '%s' points at it)", type, name, row);
        } else k->permanent++;
    } __except (me_fault_filter(GetExceptionCode())) { me_log_fault("keep", type, name, before); }
}

/* The keeper of an editor pass for the rows this session registered, inside
 * a watermark of its own (a refresh whose rows changed, before the palette
 * rebuild, which may be refused and retried later). */
static void me_keep_rows(const char *phase)
{
    sh_modserve_plan plan;
    me_closure c;
    me_keep keep = { 0, 0, 0, 0, 0, 0, NULL };
    sh_process_heap_scope heap = { 0 };
    int refs = 0;
    if (!g_bound || me_on_main() != 1 || g_mark_depth) return;
    if (sh_modserve_plan_editor(&plan) <= 0) { sh_modserve_plan_free(&plan); return; }
    sh_modserve_plan_mark_registered(&plan);
    me_closure_begin(&c);
    keep.mark = c.mark.ok ? &c.mark : NULL;
    if (sh_process_heap_enter(&g_heap, &heap)) {
        __try { refs = sh_modserve_editor_refs(&plan, me_keep_ref, &keep); }
        __finally { if (!sh_process_heap_leave(&heap)) me_log("%s: process heap scope did not balance", phase); }
    }
    me_closure_end(&c, phase, "editor rows' references");
    if (refs && (keep.kept || keep.missing))
        me_rlog("refresh: %d decl(s) editor rows point at kept for the session at the %s (%d created here, %d parsed "
                "here, %d older), %d already permanent, %d not found", keep.kept, phase, keep.created, keep.loaded,
                keep.older, keep.permanent, keep.missing);
    sh_modserve_plan_free(&plan);
}

static void me_ledger_sweep(const char *phase);

/* The editor rows' registration an editor entry runs (the palette's pass)
 * and a hub launch's gate runs before its level load
 * (sh_modserve_launch_preload): every row the scope serves and the closure
 * of decls it needs, inside a watermark and process heap 0, kept for the
 * session. `*todo` receives the steps no earlier pass had settled. Returns
 * the plan's step count, or -1. */
static int me_editor_rows(const char *phase, int first, int *todo)
{
    sh_modserve_plan plan;
    sh_modserve_watermark mark = { NULL, 0, 0 };
    double t0;
    int ready, i, rows_ok = 0, rows_failed = 0, count, editor = !strcmp(phase, "editor");
    if (todo) *todo = 0;
    t0 = me_now_ms();
    if (sh_modserve_plan_editor(&plan) < 0) { me_log("%s rows: plan refused (out of memory)", phase); return -1; }
    if (!plan.count) {
        sh_modserve_plan_free(&plan);
        me_refresh_apply(phase, 0, NULL);
        me_reparse_early(phase, 0);
        me_reload_ungated(phase, 0);
        if (first) me_log("%s%seditor rows registered in 0.0 ms (0 rows)", editor ? "" : phase, editor ? "" : ": ");
        return 0;
    }
    if (todo) *todo = sh_modserve_plan_unvisited(&plan);
    /* Everything the pass creates from here on is the rows' closure. */
    if (!g_res_head || !sh_modserve_watermark_take(g_res_head, &mark))
        me_log("%s: resource watermark unavailable (%s); only the decls rows name are kept", phase,
               g_res_head ? "the resource lists could not be walked" : "decl_resource_head unresolved");
    /* The pass's watermark covers its registration, the refresh its sources
     * publish, and the keeper (me_run neither splits nor nests another). */
    {
        int part = g_refresh_part;
        if (!g_mark_depth++) me_lookups_open();
        g_refresh_part = SH_MODSERVE_PART_ALL;
        ready = me_run(&plan, phase);
        g_refresh_part = part;
    }
    {
        me_keep keep = { 0, 0, 0, 0, 0, 0, NULL };
        sh_process_heap_scope heap = { 0 };
        int refs = 0;
        keep.mark = mark.ok ? &mark : NULL;
        /* Inside process heap 0, as registration: an object created here outlives every map. */
        if (sh_process_heap_enter(&g_heap, &heap)) {
            __try { refs = sh_modserve_editor_refs(&plan, me_keep_ref, &keep); }
            __finally { if (!sh_process_heap_leave(&heap)) me_log("%s: process heap scope did not balance", phase); }
        }
        if (refs && (keep.kept || keep.missing))
            me_log("%s: %d decl(s) editor rows point at kept for the session (%d created here, %d parsed here, %d older), %d already permanent, %d not found",
                   phase, keep.kept, keep.created, keep.loaded, keep.older, keep.permanent, keep.missing);
    }
    g_mark_depth--;
    if (mark.ok) {
        sh_modserve_raised raised;
        if (sh_modserve_watermark_raise(g_res_head, &mark, &raised) && raised.created)
            me_log("%s: %u resource(s) the rows' closure created kept for the session (%u new, %u lists): %s",
                   phase, raised.raised, raised.created, raised.lists, raised.summary);
        else if (!raised.lists) me_log("%s: resource lists could not be walked after the pass; closure not raised", phase);
    }
    sh_modserve_watermark_free(&mark);
    if (!g_mark_depth) me_lookups_close(phase, "rows' closure");
    me_keep_holds(phase);
    if (!g_mark_depth) me_keep_attrib(phase, 0);
    for (i = 0; i < plan.count; ++i) {
        if (plan.steps[i].rank != SH_MODSERVE_RANK_SEDEF) continue;
        if (plan.steps[i].ready == 1) rows_ok++;
        else rows_failed++;
    }
    InterlockedExchange(&g_modserve_editor_ok, rows_ok);
    InterlockedExchange(&g_modserve_editor_failed, rows_failed);
    me_log("%s%seditor rows registered in %.1f ms (%d rows; %d of %d steps ready, %d blocked; %d new to this pass)",
           editor ? "" : phase, editor ? "" : ": ", me_now_ms() - t0, rows_ok, ready < 0 ? 0 : ready, plan.count,
           plan.blocked, todo ? *todo : 0);
    count = plan.count;
    sh_modserve_plan_free(&plan);
    return count;
}

/* The passes' common start: mod virtual-texture families first (a material
 * binds to its family only when the family's record exists when the
 * material is parsed), mod sound banks once the sound system is up
 * (modsound.h), bundle strings of an index published off the main thread. */
static void me_pass_prelude(const char *phase)
{
    sh_modvt_before_pass(phase);
    sh_modsound_before_pass(phase);
    (void)sh_strids_scope_check();   /* string edits follow the open map's declared mods */
    (void)sh_strids_refresh_pending();
}

void sh_modserve_palette_before_build(void)
{
    int first = !InterlockedExchange(&g_editor_pass_done, 1), todo = 0;
    me_pass_prelude("editor");
    if (!g_bound || !sh_modserve_active()) return;
    me_ensure_probe();
    (void)sh_companions_ensure("editor");
    /* A palette pass is the editor's: the PLAY mode a hub launch set at its
     * launch boundary (modpublished.c), which nothing ends, must not refuse
     * undeclared rows' sources here (the editor's tick resets it too, but only
     * once the editor shows a map). */
    if (sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_PLAY && me_in_play() != 1 && me_on_main() == 1 &&
        sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) > 0)
        (void)sh_modserve_scope_apply("editor");
    (void)me_editor_rows("editor", first, &todo);
}

int sh_modserve_launch_preload(const char *why)
{
    const char *phase = "launch";
    char (*h)[SH_MODSERVE_HASH_HEX + 1] = NULL;
    const char **list = NULL;
    double t0 = me_now_ms();
    int n, i, rows_todo = 0, map_todo = 0, map_steps = 0, unknown = 0;
    if (!why || !why[0]) why = "hub launch";
    if (!g_bound || !sh_modserve_active()) return 0;
    if (me_on_main() != 1) {
        me_log("%s: %s: decls not parsed at the gate (not on the main thread); the level load parses them", phase, why);
        return -1;
    }
    if (me_in_play() == 1) return 0;
    me_pass_prelude(phase);
    me_ensure_probe();
    (void)sh_companions_ensure(phase);
    /* The hub's own mode, as the palette pass resets it: a PLAY mode left
     * over from an earlier launch would refuse undeclared rows' sources, and
     * the level load's palette pass would then register them after all. The
     * launch sets PLAY right after this pass (sh_modui_scope_launch). */
    if (sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_PLAY && sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) > 0)
        (void)sh_modserve_scope_apply(phase);
    /* 1. The editor rows and their closure, as an editor entry registers
     * them; otherwise the level load's palette pass is the first to. */
    InterlockedExchange(&g_editor_pass_done, 1);
    (void)me_editor_rows(phase, 0, &rows_todo);
    /* 2. The launched map's declared builds (the listing its parse made):
     * normally its parse's map pass settled all of it this frame. */
    n = sh_modserve_scope_hashes(NULL, 0);
    if (n > 0) {
        h = (char (*)[SH_MODSERVE_HASH_HEX + 1])calloc((size_t)n, sizeof *h);
        list = (const char **)calloc((size_t)n, sizeof *list);
    }
    if (n > 0 && h && list) {
        sh_modserve_plan plan;
        n = sh_modserve_scope_hashes(h, n);
        for (i = 0; i < n; ++i) list[i] = h[i];
        (void)sh_modsound_stock_before_map(list, n);
        if (sh_modserve_plan_bundles(list, n, &plan, &unknown) >= 0) {
            map_steps = plan.count;
            map_todo = plan.count ? sh_modserve_plan_unvisited(&plan) : 0;
            if (map_todo) {
                g_anim_pass = 1;
                (void)me_run(&plan, phase);
                me_ledger_sweep(phase);
                g_anim_pass = 0;
            }
            sh_modserve_plan_free(&plan);
        }
    } else if (n > 0) me_log("%s: %s: the declared builds were not checked (out of memory)", phase, why);
    free(list);
    free(h);
    me_log("%s: %s: decls parsed at the gate, before the level load, in %.1f ms: %d editor-row step(s) and %d of the "
           "map's %d declared step(s) were new (%d declared build(s), %d not installed); the level load's passes find "
           "them parsed", phase, why, me_now_ms() - t0, rows_todo, map_todo, map_steps, n > 0 ? n : 0, unknown);
    return rows_todo + map_todo;
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

/* 'sh_mods palette check': where each palette row's owned strings live. The
 * Play build sorts the palette in place and copies every row through the
 * row copy constructor, so a row string outside the process heap is read
 * after the Play transition freed it (the review's A-D3 fault: a row's
 * clipModelName, entry+0x228, in a released map heap). */
#define PAL_STR_FIELDS 6
#define PAL_CHECK_LIST 16
static const unsigned me_pal_str_off[PAL_STR_FIELDS] = { 0x168, 0x1a0, 0x1f8, 0x228, 0x258, 0x350 };
static const char *const me_pal_str_name[PAL_STR_FIELDS] =
    { "display name", "model", "+0x1f8", "clipModelName", "clipModel type", "door lock message" };

static int me_rd_u32(const void *p, unsigned *v)
{
    __try { *v = *(const unsigned *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* 0 none, 1 process, 2 persist, 3 map, 4 not committed (released), 5 no
 * recognisable allocator header. */
static int me_ptr_class(const void *data, char *why, size_t cap)
{
    MEMORY_BASIC_INFORMATION mbi;
    const char *h;
    why[0] = 0;
    if (!data) return 0;
    memset(&mbi, 0, sizeof mbi);
    if (!VirtualQuery(data, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) {
        _snprintf_s(why, cap, _TRUNCATE, "not committed (state 0x%lx)", (unsigned long)mbi.State);
        return 4;
    }
    h = sh_apply_engine_block_heap_name(data);
    strncpy_s(why, cap, h, _TRUNCATE);
    if (!strncmp(h, "MAP", 3)) return 3;
    if (!strncmp(h, "PERSIST", 7)) return 2;
    if (!strncmp(h, "PROCESS", 7)) return 1;
    return 5;
}

static void me_palette_check(void)
{
    static const char *const cls[] = { "none", "process heap", "persistent heap", "MAP heap", "released memory", "unknown header" };
    unsigned char *palette = (unsigned char *)sh_timeline_tile_palette();
    void *entries = NULL;
    int count = 0, i, f, c, listed = 0, rows_bad = 0, rec_bad = 0, sorts = 0, unscoped = 0, guarded = 0;
    unsigned inline_n = 0, by_class[6] = { 0 };
    char why[96], line[400];
    if (!palette || !me_rd_ptr(palette + PAL_ENTRIES_OFF, &entries) || !me_rd_i32(palette + PAL_COUNT_OFF, &count) ||
        !entries || count <= 0 || count > PAL_MAX) {
        sh_printf("sh_mods palette check: no editor palette yet\n");
        return;
    }
    {
        int ac = me_ptr_class(entries, why, sizeof why);
        _snprintf_s(line, sizeof line, _TRUNCATE, "palette check: %d rows; the array at %p is in %s (%s)", count, entries,
                    cls[ac], why);
        sh_printf("sh_mods %s\n", line);
        me_log("%s", line);
    }
    for (i = 0; i < count; ++i) {
        const unsigned char *entry = (const unsigned char *)entries + (size_t)i * PAL_STRIDE;
        void *sedef = NULL, *name = NULL, *record = NULL;
        const char *sedef_name = "?";
        int row_bad = 0;
        if (me_rd_ptr(entry, &sedef) && sedef && me_rd_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &name) && name)
            sedef_name = (const char *)name;
        for (f = 0; f < PAL_STR_FIELDS; ++f) {
            const unsigned char *s = entry + me_pal_str_off[f];
            void *data = NULL;
            unsigned len = 0;
            if (!me_rd_ptr(s + 0x10, &data) || !me_rd_u32(s + 8, &len)) continue;
            if (!data || data == (const void *)(s + 0x1c)) { ++inline_n; continue; }
            c = me_ptr_class(data, why, sizeof why);
            ++by_class[c];
            if (c >= 3) {
                row_bad = 1;
                if (listed < PAL_CHECK_LIST) {
                    ++listed;
                    __try {
                        _snprintf_s(line, sizeof line, _TRUNCATE, "palette check: row %d %s: %s (+0x%x) data %p, %u chars: %s", i,
                                    sedef_name, me_pal_str_name[f], me_pal_str_off[f], data, len, why);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        _snprintf_s(line, sizeof line, _TRUNCATE, "palette check: row %d: %s (+0x%x) data %p: %s", i,
                                    me_pal_str_name[f], me_pal_str_off[f], data, why);
                    }
                    sh_printf("sh_mods %s\n", line);
                    me_log("%s", line);
                }
            }
        }
        /* The record is an interior pointer (no header of its own): only a
         * released or map-heap block start counts. */
        if (me_rd_ptr(entry + 0x150, &record) && record && (c = me_ptr_class(record, why, sizeof why)) >= 3 && c <= 4) {
            ++rec_bad;
            row_bad = 1;
            if (listed < PAL_CHECK_LIST) {
                ++listed;
                __try {
                    _snprintf_s(line, sizeof line, _TRUNCATE, "palette check: row %d %s: record (+0x150) %p: %s", i, sedef_name,
                                record, why);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    _snprintf_s(line, sizeof line, _TRUNCATE, "palette check: row %d: record (+0x150) %p: %s", i, record, why);
                }
                sh_printf("sh_mods %s\n", line);
                me_log("%s", line);
            }
        }
        rows_bad += row_bad;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "palette check: strings: %u inline, %u process heap, %u persistent heap, %u MAP heap, %u released, %u unknown; "
                "%d row(s) with a string or record outside the process heap (%d records)%s",
                inline_n, by_class[1], by_class[2], by_class[3], by_class[4], by_class[5], rows_bad, rec_bad,
                rows_bad ? "; the next Play build would read them after the map heap is freed" : "");
    sh_printf("sh_mods %s\n", line);
    me_log("%s", line);
    sh_timeline_tile_sorts(&sorts, &unscoped, &guarded);
    _snprintf_s(line, sizeof line, _TRUNCATE, "palette check: sort guard %s; %d sort(s) this session, %d outside process heap 0",
                guarded ? "installed" : "NOT installed", sorts, unscoped);
    sh_printf("sh_mods %s\n", line);
    me_log("%s", line);
}

void sh_modserve_palette_after_build(void *palette)
{
    int rows, mods;
    if (!sh_modserve_active()) return;
    mods = me_count_mod_rows(palette, &rows);
    me_log("palette built: %d rows, %d from installed bundles", rows, mods);
    /* A mod row the game's type-info cache lacks is served its default tree
     * by the product; one line says how many rows that is. */
    if (mods > 0) sh_modtypeinfo_after_palette(palette);
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

/* The ledger's backstop at a map pass (modledger.h): a name the provider
 * served this session whose bytes differ from what the open map serves now,
 * that the engine holds and that the scope marks did not settle, is refreshed
 * in place by its type's mechanism (decls re-parsed; models, images and anims
 * reloaded); a held skeleton or other raw type has no in-place route and is
 * reported. Stock paths (shadows) and paths the map does not serve are left
 * to the marks. Main thread, inside process heap 0. */
#define ME_SWEEP_MAX 512
typedef struct me_sweep_item { const char *request; } me_sweep_item;
typedef struct me_sweep { me_sweep_item v[ME_SWEEP_MAX]; int n, over; } me_sweep;

static void me_sweep_collect(void *context, const sh_modledger_rec *rec)
{
    me_sweep *s = (me_sweep *)context;
    sh_modledger_ident now;
    sh_modserve_hit hit;
    if (!rec->request || sh_modserve_is_surface(rec->request)) return;   /* the game's own bytes: never refreshed */
    if (!me_now_ident(rec->request, &now) || sh_modledger_same(&rec->id, &now)) return;
    if (!sh_modserve_lookup(rec->request, &hit) || hit.is_shadow) return;
    if (s->n < ME_SWEEP_MAX) s->v[s->n++].request = rec->request;
    else s->over++;
}

static void me_ledger_sweep(const char *phase)
{
    me_sweep *s;
    me_held_set held;
    me_reparse decls;
    me_raw raw;
    sh_process_heap_scope heap = { 0 };
    int i, restart = 0;
    char restart_first[300] = "";
    if (!g_bound || me_on_main() != 1 || !g_res_head) return;
    s = (me_sweep *)calloc(1, sizeof *s);
    if (!s) return;
    (void)sh_modledger_each(me_sweep_collect, s);
    if (!s->n) { free(s); return; }
    if (!me_held_collect(&held, 0)) { me_log("%s: ledger: resource lists could not be walked", phase); me_held_free(&held); free(s); return; }
    me_held_sort(&held);
    memset(&decls, 0, sizeof decls);
    memset(&raw, 0, sizeof raw);
    raw.cap = ME_SWEEP_MAX;
    raw.names = (me_raw_name *)calloc((size_t)raw.cap, sizeof *raw.names);
    if (!raw.names || !sh_process_heap_enter(&g_heap, &heap)) {
        me_log("%s: ledger: %d held name(s) with other bytes not refreshed (%s)", phase, s->n,
               raw.names ? "process heap scope unavailable" : "out of memory");
        free(raw.names); me_held_free(&held); free(s);
        return;
    }
    __try {
        for (i = 0; i < s->n; ++i) {
            char type[64], name[SH_MB_REQUEST_MAX + 1];
            const me_held *h;
            sh_modserve_hit hit;
            const char *rq = s->v[i].request;
            if (!sh_mb_logical_name(rq, type, sizeof type, name, sizeof name)) continue;
            h = me_held_find(&held, type, name);
            if (!h || (h->state & 2)) continue;   /* not held loaded: its next load reads the current bytes */
            if (!strncmp(rq, "generated/decls/", 16)) {
                if (sh_modserve_lookup(rq, &hit)) (void)me_reparse_one(&decls, type, name, rq, hit.bundle_id);
                continue;
            }
            if (!_stricmp(type, "baseModel") || !_stricmp(type, "image") || (!_stricmp(type, "anim") && g_anim_pass)) {
                raw.names[raw.n].name = h->name;
                raw.names[raw.n].request = rq;
                raw.names[raw.n].kind = me_raw_kind(type);
                raw.n++;
                me_raw_one(&raw, h->cls, h->obj, h->name);
                continue;
            }
            restart++;
            if (strlen(restart_first) + strlen(name) + 16 < sizeof restart_first)
                _snprintf_s(restart_first + strlen(restart_first), sizeof restart_first - strlen(restart_first),
                            _TRUNCATE, "%s%s %s", restart_first[0] ? ", " : "", type, name);
        }
        if (raw.anims) (void)me_anim_dependents(&raw, &decls);
    } __finally {
        if (!sh_process_heap_leave(&heap)) me_log("%s: ledger: heap scope did not balance", phase);
    }
    me_log("%s: ledger: %d held name(s) carried other bytes than the open map serves: %d decl(s) re-parsed, %d model(s), "
           "%d image(s) and %d anim(s) reloaded (%d failed, %d refused)%s%s%s", phase, s->n + s->over,
           decls.reparsed, raw.models, raw.images, raw.anims, decls.failed + raw.failed + raw.faulted, raw.refused,
           restart ? "; needs a restart (no in-place route): " : "", restart_first, s->over ? "; more not checked" : "");
    free(raw.names);
    me_held_free(&held);
    free(s);
}

int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    sh_modserve_plan plan;
    int unknown = 0, ready, main = me_on_main();
    double t0;
    /* A new map: the last one's mod scripts end (they start at its Play). */
    if (main == 1) sh_script_hooks_gate(SH_SCRIPT_GATE_MAP_LOAD);
    /* Companions follow the index at every map load, a map with no rows included. */
    if (main == 1) (void)sh_companions_ensure("map");
    /* A map without manifest rows: nothing to do, nothing logged. */
    if (n <= 0 || !bundle_hashes) return 0;
    if (main != 1) { me_log("activation refused: not on the main thread"); return -1; }
    if (!g_bound) { me_log("activation refused: decl registry unavailable"); return -1; }
    sh_modvt_before_pass("map");
    sh_modsound_before_pass("map");
    /* Stock banks the declared bundles' borrowed sound events need (modsound.h). */
    (void)sh_modsound_stock_before_map(bundle_hashes, n);
    (void)sh_strids_scope_check();
    (void)sh_strids_refresh_pending();
    me_ensure_probe();
    t0 = me_now_ms();
    if (sh_modserve_plan_bundles(bundle_hashes, n, &plan, &unknown) < 0) { me_log("activation refused: out of memory"); return -1; }
    /* The map pass: held anims may be reloaded in place (g_anim_pass). */
    g_anim_pass = me_in_play() != 1;
    if (plan.count) ready = me_run(&plan, "map");
    else { me_refresh_apply("map", 0, NULL); me_reparse_early("map", 0); me_reload_ungated("map", 0); ready = 0; }
    me_ledger_sweep("map");
    g_anim_pass = 0;
    if (g_anims_waited) {
        int left = sh_modserve_refresh_pending_anims();
        if (!left) {
            me_log("map: the %d anim(s) that waited for a map load are settled (reloaded in place or already held); "
                   "none wait now", g_anims_waited);
            g_anims_waited = 0;
        } else if (left != g_anims_waited) {
            me_log("map: %d of the %d anim(s) that waited are settled; %d still wait", g_anims_waited - left,
                   g_anims_waited, left);
            g_anims_waited = left;
        }
    }
    me_log("activated %d bundles (%d steps, %d ready, %d blocked, %d not installed) in %.1f ms", n - unknown,
           plan.count, ready < 0 ? 0 : ready, plan.blocked, unknown, me_now_ms() - t0);
    sh_modserve_plan_free(&plan);
    if (ready >= 0) me_sync_rows(bundle_hashes, n);
    return ready;
}

/* ------------------------------------------------------------------ console */

static void me_print_bundle(void *context, const char *id, const char *hash, int admitted, const char *cause,
                            int files, int place)
{
    (void)context;
    sh_printf("  %s%s  %.12s  %s%s%s  %d files\n", place ? "in a map's place: " : "", id, hash,
              admitted ? "active" : "SKIPPED", admitted ? "" : ": ", admitted ? "" : cause, files);
}

static void me_print_line(void *context, const char *line) { (void)context; sh_printf("  %s\n", line); }

static int me_rebuild_main(void *context) { (void)context; return modserve_palette_rebuild_requested(); }

/* `sh_mods held [filter]`: the census of level-4 resources whose names the
 * product served this session, by type (report ADD-G1's open item: whether
 * a mod skeleton ever reaches level 4), then the ledger itself. */
#define ME_HELD_TYPES 24
#define ME_HELD_LINES 200
typedef struct me_census_type { char type[40]; int count; char names[240]; } me_census_type;
typedef struct me_census {
    const me_held_set *held;
    const char *filter;
    me_census_type types[ME_HELD_TYPES];
    int ntypes, records, held4, held1, lines, skipped;
} me_census;

static me_census_type *me_census_slot(me_census *c, const char *type)
{
    int i;
    for (i = 0; i < c->ntypes; ++i) if (!_stricmp(c->types[i].type, type)) return &c->types[i];
    if (c->ntypes == ME_HELD_TYPES) return &c->types[ME_HELD_TYPES - 1];
    strncpy_s(c->types[c->ntypes].type, sizeof c->types[c->ntypes].type, type, _TRUNCATE);
    return &c->types[c->ntypes++];
}

static void me_census_visit(void *context, const sh_modledger_rec *rec)
{
    me_census *c = (me_census *)context;
    char type[64], name[SH_MB_REQUEST_MAX + 1];
    const me_held *h = NULL;
    c->records++;
    if (rec->request && sh_mb_logical_name(rec->request, type, sizeof type, name, sizeof name))
        h = me_held_find(c->held, type, name);
    if (h) {
        (void)sh_modledger_bind(rec->request, h->obj);
        if (h->lifetime == 4) {
            me_census_type *t = me_census_slot(c, type);
            size_t at = strlen(t->names);
            c->held4++;
            t->count++;
            if (at + strlen(name) + 3 < sizeof t->names)
                _snprintf_s(t->names + at, sizeof t->names - at, _TRUNCATE, "%s%s", at ? ", " : "", name);
        } else c->held1++;
    }
    if (c->filter && c->filter[0] && !sh_modledger_name_contains(rec->request, c->filter)) return;
    if (c->lines >= ME_HELD_LINES) { c->skipped++; return; }
    c->lines++;
    sh_printf("  %s  bundle %016llx  %s %016llx  %llu bytes  served %ld  %s%s\n", rec->request ? rec->request : "?",
              (unsigned long long)rec->id.bundle, rec->id.kind == SH_MODLEDGER_ID_SHA ? "sha256" : "fnv64",
              (unsigned long long)rec->id.file, (unsigned long long)rec->id.length, rec->serves,
              !h ? "not held" : h->lifetime == 4 ? "held at level 4" : "held at level 1",
              h && (h->state & 2) ? " (not loaded)" : h && (h->state & 1) ? " (failed or default)" : "");
}

static void me_print_held(const char *filter)
{
    me_held_set held;
    me_census c;
    sh_modledger_stats st;
    int i;
    sh_modledger_get_stats(&st);
    if (!me_held_collect(&held, 0)) {
        sh_printf("sh_mods held: the resource lists could not be walked%s\n",
                  g_res_head ? "" : " (decl_resource_head unresolved)");
        me_held_free(&held);
        return;
    }
    me_held_sort(&held);
    memset(&c, 0, sizeof c);
    c.held = &held;
    c.filter = filter;
    sh_printf("sh_mods held: ledger: %u of %u request path(s) recorded, %lu serve(s), %s%s; %d resource(s) in %d list(s)%s\n",
              st.records, st.cap, st.serves, st.complete ? "complete" : "incomplete",
              st.overflow ? " (table full)" : !st.complete ? " (the provider served before the ledger was bound)" : "",
              held.n, held.lists, g_res_lock ? "" : " (walked without the resource lock)");
    sh_printf("sh_mods held: ledger records%s%s:\n", filter && filter[0] ? " matching " : "", filter && filter[0] ? filter : "");
    (void)sh_modledger_each(me_census_visit, &c);
    if (c.skipped) sh_printf("  ... %d more\n", c.skipped);
    sh_printf("sh_mods held: level 4 names the product served: %d (%d more held at level 1); by type:%s\n", c.held4,
              c.held1, c.ntypes ? "" : " none");
    for (i = 0; i < c.ntypes; ++i)
        sh_printf("  %s %d: %s%s\n", c.types[i].type, c.types[i].count, c.types[i].names,
                  strlen(c.types[i].names) + 8 >= sizeof c.types[i].names ? ", ..." : "");
    {
        int skel = 0;
        for (i = 0; i < c.ntypes; ++i) if (!_stricmp(c.types[i].type, "skeleton")) skel = c.types[i].count;
        sh_printf("sh_mods held: skeletons a mod serves held at level 4: %d (%s)\n", skel,
                  skel ? "a changed one keeps its bytes until a restart" : "none; a mod skeleton is read again at each map load");
    }
    me_held_free(&held);
}

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
        (void)sh_modserve_scope_report(me_print_line, NULL);
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
    if (!_stricmp(verb, "held")) {
        me_print_held(arg);
        return 1;
    }
    if (!_stricmp(verb, "rescan") || !_stricmp(verb, "refresh")) {
        sh_modserve_refresh r;
        int n = sh_modserve_index_rescan();
        me_ensure_probe();
        if (me_on_main() == 1) (void)sh_companions_ensure("rescan");
        sh_printf("sh_mods: rescan %s (%d bundles active)\n",
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
        if (arg && !_stricmp(arg, "check")) { me_palette_check(); return 1; }
        if (!arg || _stricmp(arg, "rebuild")) { sh_printf("usage: sh_mods palette rebuild | check\n"); return 1; }
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
