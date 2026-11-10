/* companion_engine.c -- the engine side of owner-bound companions.
 * See companion_engine.h for the hook, addresses and offsets, and companion.h
 * for the schema. Runs on the game thread inside the spawn chain. */
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "companion.h"
#include "companion_engine.h"
#include "backend_log.h"
#include "entity.h"
#include "hook.h"
#include "modserve.h"
#include "modscene_decl.h"
#include "engine_globals.h"

enum { CS_CALLSPAWN, CS_BIND, CS_ADDTARGET, CS_SPAWNDEF, CS_ISCLIENT, CS_COUNT };

#define CALLSPAWN_STOLEN 14
static const uint8_t k_callspawn_prologue[CALLSPAWN_STOLEN] = {
    0x40, 0x53,                    /* push rbx           */
    0x48, 0x83, 0xEC, 0x40,        /* sub  rsp,0x40      */
    0x48, 0x8B, 0x01,              /* mov  rax,[rcx]     */
    0x48, 0x8B, 0xD9,              /* mov  rbx,rcx       */
    0xFF, 0x10                     /* call [rax] GetType */
};

#define ENT_DECL          0x6d0
#define ENT_PHYSICS       0x808
#define DECL_NAME         0x08
#define DECL_INHERIT      0x58
#define DECL_CLASS        0x60   /* what CreateManagedClassFromDecl constructs (decl vtable +0xB0) */
#define RES_STATE         0x2c   /* idResource: bit 0 failed (a default stands in), bit 1 unloaded */
#define VSLOT_SET_ORIGIN  0x338
#define VSLOT_SET_AXIS    0x358
#define VSLOT_UPDATE_VISUALS 0x3c0 /* physics +0x110, then BecomeActive(TH_UPDATEVISUALS) */
#define ENT_SPAWN_POSE    0x3a8   /* spawnPosition, then spawnOrientation rows at +0x3B4 */
#define GAME_CREATE_SLOT  0x448   /* the idTarget_Spawn route: create, write the pose, finish */
#define GAME_FINISH_SLOT  0x458
#define PSLOT_GET_ORIGIN  0x80
#define PSLOT_GET_AXIS    0x88
#define SPAWN_ID_NONE     0x1fffffe   /* the engine's unset spawn id, as the attachment call passes */
#define BIND_ORIENTED     1
#define INHERIT_DEPTH     8
#define NEST_DEPTH        2           /* a companion may have companions once */
#define LOG_LINES_MAX     64

typedef void  (*callspawn_fn)(void *obj);
typedef void  (*bind_fn)(void *ent, void *master, unsigned flags);
typedef void  (*add_target_fn)(void *ent, void *target);
typedef void *(*spawn_def_fn)(void *gm, void *decl, int entnum, int spawn_id, int arg5);
typedef void *(*cast_fn)(void *obj);
typedef unsigned char (*is_client_fn)(void *gm);
typedef void  (*set_vec_fn)(void *ent, const float *v);
typedef const float *(*phys_get_fn)(void *phys, int id);
typedef void *(*str_ctor_fn)(void *, const char *);
typedef void *(*type_fn)(void *, const char *);
typedef void *(*find_decl_fn)(void *, const char *, unsigned char);
typedef void *(*create_fn)(void *gm, void *decl, void *edit_args);
typedef void  (*finish_fn)(void *gm, void *ent, int entnum, unsigned long long spawn_id, int arg5, void *arg6);
typedef void  (*vfn0_fn)(void *ent);

static callspawn_fn  g_orig;
static bind_fn       g_bind;
static add_target_fn g_add_target;
static spawn_def_fn  g_spawn_def;
static cast_fn       g_is_entity;
static is_client_fn  g_is_client;
static const uint8_t *g_anchor;
static void         *g_register;
static type_fn       g_type;
static find_decl_fn  g_decl;
static create_fn     g_create;   /* idGameLocal vtable +0x448, modscene_sigs.c */
static finish_fn     g_finish;   /* +0x458: register, then CallSpawn */

static sh_companion_table g_table;
static volatile LONG g_stale = 1;
static sh_modserve_stats g_built_from;
static int  g_resolved;
static const uint32_t *g_main_thread_at;   /* main_thread_id: DOOM's main thread */
static void *g_target;                      /* CallSpawn, resolved once */
static volatile LONG g_off_main_logged;
static long g_spawned, g_failed, g_owners_seen;
static int  g_log_lines;
static __declspec(thread) int g_depth;

static void cl_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    if (g_log_lines >= LOG_LINES_MAX) return;
    g_log_lines++;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void table_log(void *context, const char *line)
{
    (void)context;
    backend_log(line);
}

static void read_bundle(void *context, const char *id, const char *version, int legacy, const char *extra)
{
    (void)context; (void)version; (void)legacy;
    if (id && extra) (void)sh_companion_add_bundle(&g_table, id, extra, strlen(extra), table_log, NULL);
}

static int build_table(void)
{
    sh_modserve_get_stats(&g_built_from);
    sh_companion_table_free(&g_table);
    if (sh_modserve_each_bundle_extra(read_bundle, NULL) < 0) {
        backend_log("COMPANIONS: the bundles could not be read (out of memory); none are spawned");
        sh_companion_table_free(&g_table);
        return 0;
    }
    return g_table.count;
}

/* Only access violations are contained; an engine idException (a C++ throw)
 * must reach the engine's own handler. */
static int av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static void *read_ptr(const void *base, size_t off)
{
    void *p = NULL;
    __try { p = *(void * const *)((const uint8_t *)base + off); }
    __except (av_only(GetExceptionCode())) { p = NULL; }
    return p;
}

static unsigned char read_u8(const void *base, size_t off)
{
    unsigned char v = 0xff;
    __try { v = *((const unsigned char *)base + off); }
    __except (av_only(GetExceptionCode())) { v = 0xff; }
    return v;
}

static void *vfn(void *obj, size_t slot)
{
    void **vt = (void **)read_ptr(obj, 0);
    return vt ? read_ptr(vt, slot) : NULL;
}

static void *entitydef_manager(void)
{
    int32_t displacement;
    void *r = NULL;
    void **vt;
    if (!g_anchor || !g_type || !g_decl) return NULL;
    __try {
        if (memcmp(g_anchor + 0x10, "\x48\x8b\x0d", 3)) return NULL;
        memcpy(&displacement, g_anchor + 0x13, 4);
        r = *(void **)(g_anchor + 0x17 + displacement);
        if (!r) return NULL;
        vt = *(void ***)r;
        if (!vt || vt[7] != g_register || vt[11] != (void *)g_type) return NULL;
    } __except (av_only(GetExceptionCode())) { return NULL; }
    return g_type(r, "entityDef");
}

static const char *decl_str(void *decl, size_t off)
{
    const char *s = (const char *)read_ptr(decl, off);
    char probe = 0;
    if (!s) return NULL;
    __try { probe = s[0]; } __except (av_only(GetExceptionCode())) { return NULL; }
    return probe ? s : NULL;
}

/* The owner for `decl`: its own name, then each inherited name while it
 * stays in the mods namespace. */
static const sh_companion_owner *owner_for(void *decl, void *manager)
{
    int depth;
    const char *name = decl_str(decl, DECL_NAME);
    const sh_companion_owner *o = sh_companion_find(&g_table, name);
    if (o) return o;
    for (depth = 0; depth < INHERIT_DEPTH; depth++) {
        name = decl_str(decl, DECL_INHERIT);
        if (!name || !sh_companion_owner_namespace(name)) return NULL;
        o = sh_companion_find(&g_table, name);
        if (o) return o;
        if (!manager) return NULL;
        decl = g_decl(manager, name, 0);
        if (!decl) return NULL;
    }
    return NULL;
}

static int owner_frame(void *owner, float origin[3], float axis[9])
{
    void *phys = read_ptr(owner, ENT_PHYSICS);
    phys_get_fn get_origin = (phys_get_fn)vfn(phys, PSLOT_GET_ORIGIN);
    phys_get_fn get_axis = (phys_get_fn)vfn(phys, PSLOT_GET_AXIS);
    const float *o, *a;
    if (!get_origin || !get_axis) return 0;
    o = get_origin(phys, 0);
    a = get_axis(phys, 0);
    if (!o || !a) return 0;
    __try {
        memcpy(origin, o, 3 * sizeof(float));
        memcpy(axis, a, 9 * sizeof(float));
    } __except (av_only(GetExceptionCode())) { return 0; }
    return 1;
}

/* Spawn `decl` at a world pose the way idTarget_Spawn::SpawnSingleEntity does
 * (snaphak-re R320): create it, write spawnPosition and spawnOrientation,
 * then finish, whose CallSpawn runs the class's Spawn with that pose. The
 * class places itself, which is the only placement an idProp_Coop keeps
 * (ADD-L6 G). NULL when the route is unavailable, or when the created object
 * is not an entity (it is still finished, and returned in *loose). */

static void *spawn_posed(void *gm, void *decl, const float pose[SH_COMPANION_SPAWN_POSE_FLOATS], void **loose)
{
    uint8_t edit_args[32];
    void *obj, *ent;
    *loose = NULL;
    if (!g_create || !g_finish) return NULL;
    if (vfn(gm, GAME_CREATE_SLOT) != (void *)g_create || vfn(gm, GAME_FINISH_SLOT) != (void *)g_finish) return NULL;
    /* The default edit-state arguments (VK 0x140342C30, GL 0x140342880). */
    memset(edit_args, 0, sizeof edit_args);
    edit_args[4] = 1;
    edit_args[8] = 2;
    edit_args[12] = 0x01; edit_args[13] = 0x01;
    edit_args[17] = 0x01;
    obj = g_create(gm, decl, edit_args);
    if (!obj) return NULL;
    ent = g_is_entity(obj);
    if (ent) memcpy((uint8_t *)ent + ENT_SPAWN_POSE, pose, SH_COMPANION_SPAWN_POSE_FLOATS * sizeof(float));
    g_finish(gm, obj, -1, (unsigned long long)SPAWN_ID_NONE, -1, NULL);
    if (!ent) *loose = obj;
    return ent;
}

/* The fallback when the create and finish calls did not resolve: spawn, then
 * move. Entity SetOrigin and SetAxis write the physics only; the engine's own
 * setOrigin event follows them with UpdateVisuals, so this does too. A class
 * that stops presenting after its spawn can still keep its spawn placement. */
static void *spawn_then_move(void *gm, void *decl, const float pose[SH_COMPANION_SPAWN_POSE_FLOATS])
{
    void *child = g_spawn_def(gm, decl, -1, SPAWN_ID_NONE, 0);
    set_vec_fn set_origin, set_axis;
    vfn0_fn update_visuals;
    if (!child) return NULL;
    set_origin = (set_vec_fn)vfn(child, VSLOT_SET_ORIGIN);
    set_axis = (set_vec_fn)vfn(child, VSLOT_SET_AXIS);
    update_visuals = (vfn0_fn)vfn(child, VSLOT_UPDATE_VISUALS);
    if (set_origin) set_origin(child, pose);
    if (set_axis) set_axis(child, pose + 3);
    if (update_visuals) update_visuals(child);
    return child;
}

static void spawn_companions(void *owner, const sh_companion_owner *o, void *gm, void *manager)
{
    float origin[3], axis[9];
    char first_class[64] = "";
    int i, made = 0, posed = 0;
    if (!owner_frame(owner, origin, axis)) {
        g_failed += o->count;
        cl_log("COMPANIONS: %s: the owner's transform could not be read; no companions", o->owner);
        return;
    }
    for (i = 0; i < o->count; i++) {
        const sh_companion_entry *e = &o->entries[i];
        float pose[SH_COMPANION_SPAWN_POSE_FLOATS];
        unsigned char state;
        void *decl = g_decl(manager, e->entitydef, 0), *child, *loose = NULL;
        if (!decl) {
            g_failed++;
            cl_log("COMPANIONS: %s: companion %d '%s' is not a known entityDef; skipped", o->owner, i, e->entitydef);
            continue;
        }
        /* SpawnEntityDef constructs the decl's parsed class. A decl whose load
         * failed (an unknown inherit, say) is a default that would spawn as a
         * bare idEntity, and an unloaded one has no class yet. */
        state = read_u8(decl, RES_STATE);
        if (state & 3) {
            g_failed++;
            cl_log("COMPANIONS: %s: companion %d '%s' %s; skipped", o->owner, i, e->entitydef,
                   (state & 1) ? "failed to load (check its inherit and parse errors)" : "is not loaded");
            continue;
        }
        sh_companion_spawn_pose(origin, axis, e, pose);
        child = spawn_posed(gm, decl, pose, &loose);
        if (child) posed++;
        else if (!loose) child = spawn_then_move(gm, decl, pose);
        if (!child) {
            g_failed++;
            cl_log("COMPANIONS: %s: companion %d '%s' did not spawn%s", o->owner, i, e->entitydef,
                   loose ? " as an entity" : "");
            continue;
        }
        if (e->bind) g_bind(child, owner, BIND_ORIENTED);
        if (e->activate) g_add_target(owner, child);
        made++;
        g_spawned++;
        if (i == 0 || !first_class[0]) {
            const char *c = decl_str(decl, DECL_CLASS);
            strncpy_s(first_class, sizeof first_class, c ? c : "?", _TRUNCATE);
        }
    }
    cl_log("COMPANIONS: %s: %d of %d companions spawned (first as %s; %d at their spawn pose, %d moved after their spawn)",
           o->owner, made, o->count, first_class[0] ? first_class : "-", posed, made - posed);
}

/* A rescan publishes a new serving table; its build stats tell it apart. */
static void refresh_if_changed(void)
{
    sh_modserve_stats now;
    sh_modserve_get_stats(&now);
    if (InterlockedCompareExchange(&g_stale, 0, 1) == 1 || now.build_ms != g_built_from.build_ms ||
        now.bundles_admitted != g_built_from.bundles_admitted || now.files != g_built_from.files)
        (void)build_table();
}

static void after_spawn(void *obj)
{
    void *ent, *decl, *gm, *manager;
    const sh_companion_owner *o;
    ent = g_is_entity(obj);
    if (!ent) return;
    decl = read_ptr(ent, ENT_DECL);
    if (!decl) return;
    /* Owners are mod names, so anything else leaves at once. */
    if (!sh_companion_owner_namespace(decl_str(decl, DECL_NAME)) &&
        !sh_companion_owner_namespace(decl_str(decl, DECL_INHERIT)))
        return;
    refresh_if_changed();
    if (!g_table.count) return;
    manager = entitydef_manager();
    o = owner_for(decl, manager);
    if (!o) return;
    g_owners_seen++;
    gm = sh_entity_gamemgr();
    if (!gm || !manager) {
        g_failed += o->count;
        cl_log("COMPANIONS: %s: the game or its entityDef list is unavailable; no companions", o->owner);
        return;
    }
    if (g_is_client(gm)) return;   /* the server's companions arrive as ordinary entities */
    spawn_companions(ent, o, gm, manager);
}

static void nested_after_spawn(void *obj)
{
    g_depth++;
    __try {
        after_spawn(obj);
    } __finally {
        g_depth--;
    }
}

static void callspawn_detour(void *obj)
{
    g_orig(obj);
    if (g_depth >= NEST_DEPTH) return;
    __try {
        nested_after_spawn(obj);
    } __except (av_only(GetExceptionCode())) {
        g_failed++;
        cl_log("COMPANIONS: a fault while adding companions was contained; that owner may be incomplete");
    }
}

static void *clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (void *)r[i].addr;
    return NULL;
}

static int resolve(const sig_result *results, size_t count, const uint8_t *module_base, void **callspawn)
{
    sig_result own[CS_COUNT];
    int i;
    int32_t rel;
    uint8_t *wrapper;
    for (i = 0; i < CS_COUNT; i++) {
        if (sig_resolve_one(module_base, &SH_COMPANION_SIGNATURES[i], &own[i]) != SIG_OK) {
            cl_log("COMPANIONS: %s did not resolve on this executable; companions are unavailable",
                   SH_COMPANION_SIGNATURES[i].name);
            return 0;
        }
    }
    *callspawn = (void *)own[CS_CALLSPAWN].addr;
    g_bind = (bind_fn)own[CS_BIND].addr;
    g_add_target = (add_target_fn)own[CS_ADDTARGET].addr;
    g_spawn_def = (spawn_def_fn)own[CS_SPAWNDEF].addr;
    g_is_client = (is_client_fn)own[CS_ISCLIENT].addr;
    wrapper = (uint8_t *)own[CS_SPAWNDEF].addr;
    memcpy(&rel, wrapper + 0x22, 4);              /* the E8 at +0x21: the idEntity cast */
    g_is_entity = (cast_fn)(wrapper + 0x26 + rel);
    g_anchor = (const uint8_t *)clean(results, count, "DeclRegistryAnchor");
    g_register = clean(results, count, "DeclRegisterFile");
    g_type = (type_fn)clean(results, count, "DeclTypeByName");
    g_decl = (find_decl_fn)clean(results, count, "DeclFind");
    if (!g_anchor || !g_register || !g_type || !g_decl) {
        backend_log("COMPANIONS: the decl registry is unresolved; companions are unavailable");
        return 0;
    }
    /* The spawn-pose route: idTarget_Spawn's create and finish calls. */
    {
        sig_result create, finish;
        g_create = NULL;
        g_finish = NULL;
        for (i = 0; SH_MODSCENE_SPAWN_SIGNATURES[i].name; i++) {
            const sig_entry *s = &SH_MODSCENE_SPAWN_SIGNATURES[i];
            if (!strcmp(s->name, "SceneCreateEntity") && sig_resolve_one(module_base, s, &create) == SIG_OK)
                g_create = (create_fn)create.addr;
            else if (!strcmp(s->name, "SceneFinishSpawn") && sig_resolve_one(module_base, s, &finish) == SIG_OK)
                g_finish = (finish_fn)finish.addr;
        }
        if (!g_create || !g_finish) {
            g_create = NULL;
            g_finish = NULL;
            backend_log("COMPANIONS: the engine's create and finish calls did not resolve; companions are moved "
                        "after their spawn, which a class that places itself at spawn ignores");
        }
    }
    return 1;
}

static int prologue_ok(void)
{
    int same = 0;
    __try { same = !memcmp(g_target, k_callspawn_prologue, CALLSPAWN_STOLEN); }
    __except (EXCEPTION_EXECUTE_HANDLER) { same = 0; }
    return same;
}

static int commit_detour(const char *phase)
{
    void *tramp = hook_prepare(g_target, (void *)callspawn_detour, CALLSPAWN_STOLEN);
    if (!tramp) {
        cl_log("COMPANIONS: trampoline preparation failed (%s)", hook_last_failure());
        return 0;
    }
    g_orig = (callspawn_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_orig = NULL;
        backend_log("COMPANIONS: the CallSpawn detour commit failed");
        return 0;
    }
    if (phase) cl_log("COMPANIONS: installed on CallSpawn at %p for %d owner(s) (%s)", g_target, g_table.count, phase);
    else cl_log("COMPANIONS: installed on CallSpawn at %p for %d owner(s)", g_target, g_table.count);
    return 1;
}

int sh_companions_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    if (g_orig && hook_is_installed((void *)g_orig)) return 1;
    if (!g_resolved) {
        if (!module_base || !resolve(results, count, module_base, &g_target)) return 0;
        /* For the runtime arm: only DOOM's main thread runs CallSpawn (ADD-G2, ADD-V1). */
        g_main_thread_at = (const uint32_t *)glb_resolve(module_base, "main_thread_id", NULL);
        g_resolved = 1;
    }
    InterlockedExchange(&g_stale, 0);
    if (!build_table()) {
        backend_log("COMPANIONS: no admitted bundle declares companions; the spawn chain is untouched until one "
                    "does (it is armed then at the next map load or Play, on the main thread)");
        return 0;
    }
    if (!prologue_ok()) {
        backend_log("COMPANIONS: CallSpawn's prologue is not the expected one (another detour?); not installed");
        return 0;
    }
    return commit_detour(NULL);
}

static int on_main_thread(void)
{
    uint32_t id = 0;
    if (!g_main_thread_at) return -1;
    __try { id = *g_main_thread_at; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!id) return -1;
    return id == GetCurrentThreadId() ? 1 : 0;
}

/* A new serving index since the table was built: the index listener's mark,
 * or other build stats (the test the live detour makes at each owner spawn). */
static int index_changed(void)
{
    sh_modserve_stats now;
    if (InterlockedCompareExchange(&g_stale, 0, 0)) return 1;
    sh_modserve_get_stats(&now);
    return now.build_ms != g_built_from.build_ms || now.bundles_admitted != g_built_from.bundles_admitted ||
           now.files != g_built_from.files;
}

/* Runtime arming (companion_engine.h). Every check comes before the table is
 * read again, so a session without companions costs a stats compare per gate. */
int sh_companions_ensure(const char *phase)
{
    sh_companion_arm a;
    int d;
    const char *at = phase ? phase : "gate";
    memset(&a, 0, sizeof a);
    a.resolved = g_resolved;
    a.installed = g_orig && hook_is_installed((void *)g_orig);
    a.on_main = a.resolved && !a.installed ? on_main_thread() : 0;
    a.index_changed = a.resolved && !a.installed && a.on_main == 1 ? index_changed() : 0;
    a.owners = -1;
    a.prologue_ok = -1;
    d = sh_companion_arm_decide(&a);
    if (d == SH_COMPANION_ARM_BUILD) {
        InterlockedExchange(&g_stale, 0);
        a.owners = build_table();
        a.prologue_ok = a.owners > 0 ? prologue_ok() : -1;
        d = sh_companion_arm_decide(&a);
    }
    switch (d) {
    case SH_COMPANION_ARM_LIVE: return 1;
    case SH_COMPANION_ARM_COMMIT: return commit_detour(at);
    case SH_COMPANION_ARM_OFF_MAIN:
        if (!InterlockedExchange(&g_off_main_logged, 1))
            cl_log("COMPANIONS: not armed at the %s: %s; a main-thread gate arms it", at,
                   a.on_main ? "the main thread is unknown" : "not on DOOM's main thread");
        return 0;
    case SH_COMPANION_ARM_FOREIGN:
        cl_log("COMPANIONS: CallSpawn's prologue is not the expected one (another detour?); not installed at the %s", at);
        return 0;
    case SH_COMPANION_ARM_NO_OWNER:
        cl_log("COMPANIONS: new serving index at the %s; no admitted bundle declares companions", at);
        return 0;
    default:
        return 0;
    }
}

void sh_companions_refresh(void)
{
    InterlockedExchange(&g_stale, 1);
}
