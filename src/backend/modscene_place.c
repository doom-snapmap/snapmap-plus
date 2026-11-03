/* modscene_place.c -- see modscene_place.h. The pure half (axis, pose and
 * record table) is above the engine half so the unit test can build it alone
 * with SH_MODSCENE_PLACE_PURE. */
#include <math.h>
#include <string.h>
#include "modscene_place.h"

void sh_modscene_place_yaw_axis(float yaw_degrees, float axis[9])
{
    const double r = (double)yaw_degrees * (3.14159265358979323846 / 180.0);
    const float c = (float)cos(r), s = (float)sin(r);
    axis[0] = c;  axis[1] = s; axis[2] = 0.0f;
    axis[3] = -s; axis[4] = c; axis[5] = 0.0f;
    axis[6] = 0.0f; axis[7] = 0.0f; axis[8] = 1.0f;
}

static int sp_finite_within(float v, float limit)
{
    return v == v && v > -limit && v < limit;
}

int sh_modscene_place_pose_ok(const float origin[3], const float angles[3])
{
    int i;
    if (!origin || !angles) return 0;
    for (i = 0; i < 3; i++)
        if (!sp_finite_within(origin[i], 1.0e6f) || !sp_finite_within(angles[i], 1.0e5f)) return 0;
    return 1;
}

void sh_modscene_place_table_clear(sh_modscene_place_table *t)
{
    if (t) memset(t, 0, sizeof *t);
}

int sh_modscene_place_remember(sh_modscene_place_table *t, const void *node, const char *player,
                               const float origin[3], const float angles[3])
{
    sh_modscene_place_record *row = NULL;
    size_t i;
    if (!t || !node || !player || !player[0] || strlen(player) >= sizeof row->player || !origin || !angles)
        return 0;
    for (i = 0; i < SH_MODSCENE_PLACE_RECORDS && !row; i++)
        if (t->rows[i].node == node) row = &t->rows[i];
    for (i = 0; i < SH_MODSCENE_PLACE_RECORDS && !row; i++)
        if (!t->rows[i].node) row = &t->rows[i];
    if (!row) {
        row = &t->rows[0];
        for (i = 1; i < SH_MODSCENE_PLACE_RECORDS; i++)
            if (t->rows[i].stamp < row->stamp) row = &t->rows[i];
    }
    memset(row, 0, sizeof *row);
    row->node = node;
    memcpy(row->player, player, strlen(player) + 1);
    memcpy(row->origin, origin, sizeof row->origin);
    memcpy(row->angles, angles, sizeof row->angles);
    row->stamp = ++t->next_stamp;
    return 1;
}

int sh_modscene_place_take(sh_modscene_place_table *t, const void *node, sh_modscene_place_record *out)
{
    size_t i;
    if (!t || !node) return 0;
    for (i = 0; i < SH_MODSCENE_PLACE_RECORDS; i++) {
        if (t->rows[i].node != node) continue;
        if (out) *out = t->rows[i];
        memset(&t->rows[i], 0, sizeof t->rows[i]);
        return 1;
    }
    return 0;
}

#ifndef SH_MODSCENE_PLACE_PURE
/* ------------------------------------------------------------------------- */
/* Engine half: build 13954591 (SH_RVA yields 0 on any other image).          */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include "modserve.h"
#include "patch.h"
#include "backend_log.h"
#include "engine_build.h"
#include "entity.h"
#include "typeinfo.h"
#include "commands.h"
#include "modscene_decl.h"
#include "modhud_scene.h"

/* Whole, position-independent prologue lengths (identical on both renderers):
 *   DoAction        mov r11,rsp; push rbp; push rdi; push r14; sub rsp,0x60;
 *                   mov eax,[rcx+0x3f8]                                  17
 *   USED output     mov [rsp+8],rbx; push rdi; sub rsp,0x20; lea rbx,[rcx+0x5200] 17
 *   OnActionRequest mov [rsp+0x18],rbp; push rsi; push r14; push r15; sub rsp,0x30 14
 *   SyncStart       mov [rsp+0x10],rbx; push rbp; push rsi; push r12; push r14;
 *                   push r15; sub rsp,0x50                                17 */
#define SP_DOACTION_STOLEN 17
#define SP_USED_STOLEN     17
#define SP_REQUEST_STOLEN  14
#define SP_SYNCSTART_STOLEN 17
/* The interaction link's sync-start helper (VK 0x1406BC7B0, GL 0x1406BBA80,
 * snaphak-re R312/R320; modscene_sigs.c) spawns the link's sync, places it at
 * the owner's physics pose, starts it and, only when it started, stores it in
 * the idEntityPtr at this + 0x8D0 (spawn ids at +0, the entity at +8). */
#define SP_LINK_SYNC_PTR    0x8D8u

/* idEntity offsets, both renderers (companion_engine.h, R312): entityDef,
 * name, physics, the targets list of 0x20-byte references ({spawn id, cached
 * spawn id, idEntity *}), and the SetOrigin / SetAxis slots. */
#define SP_ENT_DECL        0x6D0u
#define SP_DECL_NAME       0x08u
#define SP_DECL_INHERIT    0x58u   /* a placed entity's own entityDef inherits its palette row */
#define SP_ENT_NAME        0x388u
#define SP_ENT_PHYSICS     0x808u
#define SP_ENT_TARGETS     0x3F0u
#define SP_ENT_TARGET_N    0x3F8u
#define SP_TARGET_STRIDE   0x20u
#define SP_TARGET_ENTITY   0x08u
#define SP_SPAWN_ID_NONE   0x1FFFFFE
#define SP_VSLOT_SET_ORIGIN 0x338u
#define SP_VSLOT_SET_AXIS   0x358u
#define SP_PSLOT_GET_ORIGIN 0x80u
#define SP_TARGETS_MAX     64
#define SP_PSLOT_GET_AXIS   0x88u
/* Scene spawns (snaphak-re R320). idTarget_Spawn::SpawnSingleEntity (VK
 * 0x140A7A8D0, GL 0x140A79D90) creates the entity through idGameLocal vtable
 * +0x448, writes its idEntity spawnPosition (+0x3A8) and spawnOrientation
 * (+0x3B4, idMat3 rows) before the spawn chain reads them, finishes through
 * +0x458 (register, then CallSpawn) and posts the "show" event. The two slots
 * resolve by signature (modscene_sigs.c) and are checked against the live
 * game's vtable before each spawn. */
#define SP_ENT_SPAWN_ORIGIN 0x3A8u
#define SP_ENT_SPAWN_AXIS   0x3B4u
#define SP_DECL_STATE       0x2Cu   /* idResource: bit 0 failed (a default stands in), bit 1 unloaded */
#define SP_GAME_CREATE_SLOT 0x448u
#define SP_GAME_FINISH_SLOT 0x458u
#define SP_HOMES            64      /* scene nodes whose placed pose is remembered per Play */
#define SP_SCENE_LINES      48      /* per-activation scene lines per Play */
#define SP_INHERIT_DEPTH    8

/* Engine functions (snaphak-re R313, decompiled on both renderers):
 *   idPlayer cast                       VK 0x14AF840  GL 0x14AF8D0
 *   target reference refresh            VK 0x3B8170   GL 0x3B7B60  (DoAction's own call)
 *   ProcessEventArgs(obj,ret,ev,n,...)  VK 0xB47350   GL 0xB46940
 *   idEventArg clear                    VK 0xB44640   GL 0xB43A50
 *   PostEventMS(obj,ev,delayMs,a1,a2)   VK 0xB45D00   GL 0xB452F0  (setviewpos posts teleport with it) */
#define SP_CAST_PLAYER_RVA   SH_RVA(0x14AF840u, 0x14AF8D0u)
#define SP_REF_REFRESH_RVA   SH_RVA(0x3B8170u, 0x3B7B60u)
#define SP_PROCESS_ARGS_RVA  SH_RVA(0xB47350u, 0xB46940u)
#define SP_ARG_CLEAR_RVA     SH_RVA(0xB44640u, 0xB43A50u)
#define SP_POST_EVENT_RVA    SH_RVA(0xB45D00u, 0xB452F0u)
/*   PostEventMS without arguments       VK 0xB45770   GL 0xB44D60  (xor r9d,r9d; jmp; SpawnSingleEntity's "show") */
#define SP_POST_EVENT0_RVA   SH_RVA(0xB45770u, 0xB44D60u)

/* Event registry: decl manager +0x90 -> idEventDefInterfaceLocal, +0x10
 * FindByName; a definition's compact argument format at +0x08. */
#define SP_EVMGR_VSLOT     0x90u
#define SP_EV_FIND_VSLOT   0x10u
#define SP_EV_FORMAT_OFF   0x08u

/* idEventArg: a type character, then the value from +8 ('v' an idVec3, 'a'
 * pitch, yaw, roll, 'i' an int). */
typedef struct { char type; char pad[7]; float f[4]; } sp_arg;

typedef void (*sp_doaction_fn)(void *self, void *activator);
typedef void (*sp_used_fn)(void *self);
typedef unsigned char (*sp_request_fn)(void *component, void *owner, const int *action, void *requester);
typedef void *(*sp_cast_fn)(void *obj);
typedef void (*sp_refresh_fn)(void *ref, void *other);
typedef void *(*sp_vfn0)(void *self);
typedef void *(*sp_vfn_name)(void *self, const char *name);
typedef const float *(*sp_get_vec_fn)(void *phys, int id);
typedef void (*sp_set_vec_fn)(void *ent, const float *v);
typedef void *(*sp_process_fn)(void *obj, void *ret, void *ev, int n, sp_arg *a0, sp_arg *a1, sp_arg *a2,
                               sp_arg *a3, sp_arg *a4, sp_arg *a5, sp_arg *a6, sp_arg *a7);
typedef void (*sp_clear_fn)(void *arg);
typedef void (*sp_post_fn)(void *obj, void *ev, int delay_ms, sp_arg *a1, sp_arg *a2);
typedef void (*sp_post0_fn)(void *obj, void *ev, int delay_ms);
typedef void *(*sp_create_fn)(void *gm, void *decl, void *edit_args);
typedef void (*sp_finish_fn)(void *gm, void *ent, int entnum, unsigned long long spawn_id, int arg5, void *arg6);
typedef void *(*sp_type_fn)(void *registry, const char *type);
typedef void *(*sp_find_decl_fn)(void *manager, const char *name, unsigned char make_default);
typedef const float *(*sp_get_axis_fn)(void *phys, int id);
typedef void (*sp_syncstart_fn)(void *link, void *owner, void *arg3, void *link_data, void *player);

/* A scene node's pose when the Play first activated it: its editor placement. */
typedef struct sp_home { const void *node; sh_scene_pose pose; } sp_home;

static const uint8_t *g_base;
static sp_doaction_fn g_orig_doaction;
static sp_used_fn g_orig_used;
static sp_request_fn g_orig_request;
static void *g_ev_teleport, *g_ev_view_angles;
static SRWLOCK g_lock = SRWLOCK_INIT;
static sh_modscene_place_table g_table;
static volatile LONG g_logged_place, g_logged_return, g_logged_drop, g_logged_null, g_logged_fail, g_logged_miss;
static sp_create_fn g_create;
static sp_finish_fn g_finish;
static const uint8_t *g_decl_anchor;
static void *g_decl_register;
static sp_type_fn g_decl_type;
static sp_find_decl_fn g_decl_find;
static void *g_ev_show;
static sh_scene_table g_scenes;
static sp_home g_homes[SP_HOMES];
static int g_home_count;
static volatile LONG g_scene_lines, g_logged_homes;
static sp_syncstart_fn g_orig_syncstart;
static sh_scene_pending_table g_pending;

/* Only access violations are contained; an engine idException (a C++ throw)
 * must reach the engine's own handler. */
static int sp_av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static void *sp_fn(uint32_t rva) { return (g_base && rva) ? (void *)(g_base + rva) : NULL; }

static void sp_log(volatile LONG *once, const char *fmt, ...)
{
    char line[320];
    va_list ap;
    if (once && InterlockedExchange(once, 1)) return;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
    sh_printf("%s\n", line);
}

static void *sp_read_ptr(const void *base, size_t off)
{
    void *p = NULL;
    __try { p = *(void * const *)((const uint8_t *)base + off); }
    __except (sp_av_only(GetExceptionCode())) { p = NULL; }
    return p;
}

static void *sp_vfn(void *obj, size_t slot)
{
    void *vt = obj ? sp_read_ptr(obj, 0) : NULL;
    return vt ? sp_read_ptr(vt, slot) : NULL;
}

static int sp_read_cstr(const char *src, char *dst, size_t cap)
{
    size_t i = 0;
    dst[0] = 0;
    if (!src) return 0;
    __try {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
        dst[i] = 0;
    } __except (sp_av_only(GetExceptionCode())) { dst[0] = 0; return 0; }
    return dst[0] != 0;
}

/* 1 when the entity's entityDef is a scene_play row or directly inherits one:
 * a map entity's entityDef is its own decl whose inherit names the palette row. */
static int sp_is_scene_action(void *ent)
{
    void *decl = ent ? sp_read_ptr(ent, SP_ENT_DECL) : NULL;
    char name[160], base[160];
    if (!decl) return 0;
    if (!sp_read_cstr((const char *)sp_read_ptr(decl, SP_DECL_NAME), name, sizeof name)) name[0] = 0;
    if (!sp_read_cstr((const char *)sp_read_ptr(decl, SP_DECL_INHERIT), base, sizeof base)) base[0] = 0;
    if (sh_modscene_place_is_scene_row(name) || sh_modscene_place_is_scene_row(base)) return 1;
    /* A near miss (a scene_play row this test does not recognize) is worth one line. */
    if (strstr(name, "scene_play") || strstr(base, "scene_play"))
        sp_log(&g_logged_miss, "scene-place: Activate row '%s' (inherits '%s') is not a mod scene_play row", name, base);
    return 0;
}

static const char *sp_entity_name(void *ent, char *out, size_t cap)
{
    if (!ent || !sp_read_cstr((const char *)sp_read_ptr(ent, SP_ENT_NAME), out, cap))
        _snprintf_s(out, cap, _TRUNCATE, "%s", ent ? "an unnamed entity" : "nobody");
    return out;
}

static void *sp_player_of(void *activator)
{
    sp_cast_fn cast = (sp_cast_fn)sp_fn(SP_CAST_PLAYER_RVA);
    void *p = NULL;
    if (!activator || !cast) return NULL;
    __try { p = cast(activator); } __except (sp_av_only(GetExceptionCode())) { p = NULL; }
    return p;
}

/* An event definition by name whose compact format is exactly `format`. */
static void *sp_event(const char *name, const char *format)
{
    void *declmgr = sh_typeinfo_get_declmgr(), *evmgr = NULL, *ev = NULL;
    sp_vfn0 acc = (sp_vfn0)sp_vfn(declmgr, SP_EVMGR_VSLOT);
    sp_vfn_name find;
    char have[16];
    if (!acc) return NULL;
    __try { evmgr = acc(declmgr); } __except (sp_av_only(GetExceptionCode())) { evmgr = NULL; }
    find = (sp_vfn_name)sp_vfn(evmgr, SP_EV_FIND_VSLOT);
    if (!find) return NULL;
    __try { ev = find(evmgr, name); } __except (sp_av_only(GetExceptionCode())) { ev = NULL; }
    if (!ev) return NULL;
    if (!sp_read_cstr((const char *)sp_read_ptr(ev, SP_EV_FORMAT_OFF), have, sizeof have)) have[0] = 0;
    return strcmp(have, format) ? NULL : ev;
}

static int sp_events_ready(void)
{
    if (!g_ev_teleport) g_ev_teleport = sp_event("teleport", "va");          /* teleport(idVec3 origin, idAngles angles) */
    if (!g_ev_view_angles) g_ev_view_angles = sp_event("getViewAngles", "b"); /* vector getViewAngles(bool includeWeaponKick) */
    return g_ev_teleport && g_ev_view_angles;
}

/* The player's feet origin (physics GetOrigin) and view angles (getViewAngles). */
static int sp_player_pose(void *player, float origin[3], float angles[3])
{
    void *phys = sp_read_ptr(player, SP_ENT_PHYSICS);
    sp_get_vec_fn get_origin = (sp_get_vec_fn)sp_vfn(phys, SP_PSLOT_GET_ORIGIN);
    sp_process_fn process = (sp_process_fn)sp_fn(SP_PROCESS_ARGS_RVA);
    sp_clear_fn clear = (sp_clear_fn)sp_fn(SP_ARG_CLEAR_RVA);
    sp_arg a[8], ret;
    int ok = 0;
    if (!get_origin || !process || !clear || !sp_events_ready()) return 0;
    memset(a, 0, sizeof a);
    memset(&ret, 0, sizeof ret);
    a[0].type = 'i';   /* includeWeaponKick = 0; the engine's argument copy drops a 'b' value */
    __try {
        const float *o = get_origin(phys, 0);
        if (o) {
            memcpy(origin, o, 3 * sizeof(float));
            process(player, &ret, g_ev_view_angles, 1, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]);
            if (ret.type == 'v') { memcpy(angles, ret.f, 3 * sizeof(float)); ok = 1; }
            clear(&ret);
        }
    } __except (sp_av_only(GetExceptionCode())) { ok = 0; }
    return ok && sh_modscene_place_pose_ok(origin, angles);
}

/* The entity a target reference names, refreshed the way DoAction refreshes it. */
static void *sp_target(uint8_t *ref)
{
    sp_refresh_fn refresh = (sp_refresh_fn)sp_fn(SP_REF_REFRESH_RVA);
    void *ent = NULL;
    __try {
        int *id = (int *)ref;
        void **cached = (void **)(ref + SP_TARGET_ENTITY);
        if (id[0] != id[1]) { if (refresh) refresh(ref, ref + 4); }
        else if (id[0] != SP_SPAWN_ID_NONE && !*cached && sh_entity_gamemgr()) { if (refresh) refresh(ref, ref); }
        ent = *cached;
    } __except (sp_av_only(GetExceptionCode())) { ent = NULL; }
    return ent;
}

/* ---- scenes blocks (modscene_decl.h) ------------------------------------ */

static void sp_scene_log(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    if (InterlockedIncrement(&g_scene_lines) > SP_SCENE_LINES) return;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
    sh_printf("%s\n", line);
}

static unsigned char sp_read_u8(const void *base, size_t off)
{
    unsigned char v = 0xff;
    __try { v = *((const unsigned char *)base + off); }
    __except (sp_av_only(GetExceptionCode())) { v = 0xff; }
    return v;
}

/* The entityDef type manager, from the decl registry the shared results name. */
static void *sp_entitydef_manager(void)
{
    int32_t displacement;
    void *r = NULL;
    void **vt;
    if (!g_decl_anchor || !g_decl_type || !g_decl_find) return NULL;
    __try {
        if (memcmp(g_decl_anchor + 0x10, "\x48\x8b\x0d", 3)) return NULL;
        memcpy(&displacement, g_decl_anchor + 0x13, 4);
        r = *(void **)(g_decl_anchor + 0x17 + displacement);
        if (!r) return NULL;
        vt = *(void ***)r;
        if (!vt || vt[7] != g_decl_register || vt[11] != (void *)g_decl_type) return NULL;
    } __except (sp_av_only(GetExceptionCode())) { return NULL; }
    return g_decl_type(r, "entityDef");
}

static void *sp_find_entitydef(void *manager, const char *name)
{
    void *decl = NULL;
    if (!manager || !name) return NULL;
    __try { decl = g_decl_find(manager, name, 0); } __except (sp_av_only(GetExceptionCode())) { decl = NULL; }
    return decl;
}

/* The scenes entry for a node: its own entityDef, then each inherit while it
 * stays in the mods namespace (a placed node's entityDef inherits its palette row). */
static const sh_scene_entry *sp_scene_of(void *node)
{
    void *decl = node ? sp_read_ptr(node, SP_ENT_DECL) : NULL, *manager = NULL;
    char name[SH_SCENE_NAME_CAP];
    const sh_scene_entry *e;
    int depth;
    if (!decl || !g_scenes.count) return NULL;
    if (sp_read_cstr((const char *)sp_read_ptr(decl, SP_DECL_NAME), name, sizeof name) &&
        (e = sh_scene_find(&g_scenes, name)) != NULL)
        return e;
    for (depth = 0; depth < SP_INHERIT_DEPTH; depth++) {
        if (!sp_read_cstr((const char *)sp_read_ptr(decl, SP_DECL_INHERIT), name, sizeof name)) return NULL;
        if (!sh_scene_owner_namespace(name)) return NULL;
        if ((e = sh_scene_find(&g_scenes, name)) != NULL) return e;
        if (!manager) manager = sp_entitydef_manager();
        decl = sp_find_entitydef(manager, name);
        if (!decl) return NULL;
    }
    return NULL;
}

/* An entity's physics origin and the yaw of its axis. */
static int sp_entity_pose(void *ent, sh_scene_pose *out)
{
    void *phys = sp_read_ptr(ent, SP_ENT_PHYSICS);
    sp_get_vec_fn get_origin = (sp_get_vec_fn)sp_vfn(phys, SP_PSLOT_GET_ORIGIN);
    sp_get_axis_fn get_axis = (sp_get_axis_fn)sp_vfn(phys, SP_PSLOT_GET_AXIS);
    float o[3], ax[9], zero[3] = { 0.0f, 0.0f, 0.0f };
    int ok = 0;
    if (!get_origin || !get_axis) return 0;
    __try {
        const float *po = get_origin(phys, 0), *pa = get_axis(phys, 0);
        if (po && pa) {
            memcpy(o, po, sizeof o);
            memcpy(ax, pa, sizeof ax);
            ok = 1;
        }
    } __except (sp_av_only(GetExceptionCode())) { ok = 0; }
    if (!ok || !sh_modscene_place_pose_ok(o, zero)) return 0;
    out->x = o[0];
    out->y = o[1];
    out->z = o[2];
    out->yaw = (float)(atan2((double)ax[1], (double)ax[0]) * (180.0 / 3.14159265358979323846));
    return out->yaw == out->yaw;
}

/* The node's pose when this Play first activated it (its editor placement),
 * recorded now when it is new. A node the backend moved keeps its first pose. */
static sh_scene_pose sp_home_of(const void *node, sh_scene_pose now)
{
    sh_scene_pose home = now;
    int i, found = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_home_count; i++)
        if (g_homes[i].node == node) { home = g_homes[i].pose; found = 1; break; }
    if (!found) {
        if (g_home_count < SP_HOMES) {
            g_homes[g_home_count].node = node;
            g_homes[g_home_count].pose = now;
            g_home_count++;
        } else {
            found = -1;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (found < 0)
        sp_log(&g_logged_homes, "scene-place: more than %d scene nodes this Play; a moved node's next scene starts where it was moved", SP_HOMES);
    return home;
}

static void sp_move(void *node, sh_scene_pose at, int *done)
{
    sp_set_vec_fn set_origin = (sp_set_vec_fn)sp_vfn(node, SP_VSLOT_SET_ORIGIN);
    sp_set_vec_fn set_axis = (sp_set_vec_fn)sp_vfn(node, SP_VSLOT_SET_AXIS);
    float origin[3], axis[9];
    *done = 0;
    if (!set_origin || !set_axis) return;
    origin[0] = at.x;
    origin[1] = at.y;
    origin[2] = at.z;
    sh_modscene_place_yaw_axis(at.yaw, axis);
    __try { set_origin(node, origin); set_axis(node, axis); *done = 1; }
    __except (sp_av_only(GetExceptionCode())) { *done = 0; }
}

/* Spawn one entityDef at a pose the way idTarget_Spawn does. Returns NULL and
 * a reason when it cannot. */
static const char *sp_spawn_scene_entity(const char *entitydef, sh_scene_pose at)
{
    void *gm = sh_entity_gamemgr(), *manager, *decl, *ent = NULL;
    sp_post0_fn post0 = (sp_post0_fn)sp_fn(SP_POST_EVENT0_RVA);
    uint8_t edit_args[32];
    float axis[9];
    int ok = 0;
    unsigned char state;
    if (!g_create || !g_finish) return "the engine's spawn calls did not resolve on this executable";
    if (!gm) return "the game is not running";
    if (sp_vfn(gm, SP_GAME_CREATE_SLOT) != (void *)g_create || sp_vfn(gm, SP_GAME_FINISH_SLOT) != (void *)g_finish)
        return "the game's spawn slots are not the resolved functions";
    manager = sp_entitydef_manager();
    decl = sp_find_entitydef(manager, entitydef);
    if (!decl) return "it is not a known entityDef";
    state = sp_read_u8(decl, SP_DECL_STATE);
    if (state & 3) return (state & 1) ? "its entityDef failed to load" : "its entityDef is not loaded";
    /* The default edit-state arguments (FUN_140342C30 on Vulkan). */
    memset(edit_args, 0, sizeof edit_args);
    edit_args[4] = 1;
    edit_args[8] = 2;
    edit_args[12] = 0x01; edit_args[13] = 0x01;
    edit_args[17] = 0x01;
    sh_modscene_place_yaw_axis(at.yaw, axis);
    __try {
        ent = g_create(gm, decl, edit_args);
        if (ent) {
            float *origin = (float *)((uint8_t *)ent + SP_ENT_SPAWN_ORIGIN);
            origin[0] = at.x;
            origin[1] = at.y;
            origin[2] = at.z;
            memcpy((uint8_t *)ent + SP_ENT_SPAWN_AXIS, axis, sizeof axis);
            g_finish(gm, ent, -1, (unsigned long long)SP_SPAWN_ID_NONE, -1, NULL);
            ok = 1;
        }
    } __except (sp_av_only(GetExceptionCode())) { ok = 0; }
    if (!ok) return ent ? "its spawn faulted" : "the engine did not create it";
    if (!g_ev_show) g_ev_show = sp_event("show", "");
    if (g_ev_show && post0) {
        __try { post0(ent, g_ev_show, 0); } __except (sp_av_only(GetExceptionCode())) { }
    }
    return NULL;
}

/* Move every target of a scene_play row to where its scene plays, spawn what
 * its scenes entry lists, and remember the player when the player returns. */
static void sp_place_targets(void *self, void *player)
{
    float origin[3], angles[3];
    char who[64], node_name[64];
    uint8_t *list = (uint8_t *)sp_read_ptr(self, SP_ENT_TARGETS);
    int count = 0, i, moved = 0;
    sh_scene_pose me;
    if (!sp_read_cstr((const char *)sp_read_ptr(player, SP_ENT_NAME), who, sizeof who)) {
        sp_log(&g_logged_fail, "scene-place: the activating player has no name; the scene plays at the node");
        return;
    }
    if (!sp_player_pose(player, origin, angles)) {
        sp_log(&g_logged_fail, "scene-place: %s's position or view angles could not be read; the scene plays at the node", who);
        return;
    }
    __try { count = *(const int *)((const uint8_t *)self + SP_ENT_TARGET_N); }
    __except (sp_av_only(GetExceptionCode())) { count = 0; }
    if (!list || count <= 0) return;
    if (count > SP_TARGETS_MAX) count = SP_TARGETS_MAX;
    me.x = origin[0];
    me.y = origin[1];
    me.z = origin[2];
    me.yaw = angles[1];
    for (i = 0; i < count; i++) {
        void *node = sp_target(list + (size_t)i * SP_TARGET_STRIDE);
        const sh_scene_entry *e;
        sh_scene_pose now, home, play;
        int done = 0, reaimed = 0, k;
        if (!node) continue;
        e = sp_scene_of(node);
        if (e && !sp_entity_pose(node, &now)) {
            sp_scene_log("scene-place: %s's placement could not be read; it plays at the player as before", e->node);
            e = NULL;
        }
        home = e ? sp_home_of(node, now) : me;
        play = sh_scene_play_frame(e, home, me, &reaimed);
        sp_move(node, play, &done);
        if (!done) continue;
        moved++;
        if (!e || e->return_player) {
            AcquireSRWLockExclusive(&g_lock);
            (void)sh_modscene_place_remember(&g_table, node, who, origin, angles);
            ReleaseSRWLockExclusive(&g_lock);
        }
        if (!e) {
            sp_log(&g_logged_place, "scene-place: a scene node plays at %s (%s at %.1f %.1f %.1f, yaw %.1f)",
                   who, sp_entity_name(node, node_name, sizeof node_name), origin[0], origin[1], origin[2], angles[1]);
            continue;
        }
        if (e->at_node)
            sp_scene_log("scene-place: %s plays at its node (scene at %.1f %.1f %.1f, yaw %.1f)",
                         e->node, play.x, play.y, play.z, play.yaw);
        else
            sp_scene_log("scene-place: %s plays at %s (%.1f %.1f %.1f, view yaw %.1f; scene at %.1f %.1f %.1f, yaw %.1f%s; anchor %.1f %.1f %.1f, yaw %.1f%s)",
                         e->node, who, me.x, me.y, me.z, me.yaw, play.x, play.y, play.z, play.yaw,
                         reaimed ? ", turned toward the anchor" : "", home.x, home.y, home.z, home.yaw,
                         e->return_player ? "" : "; no return");
        if (e->spawn_count) {
            /* Spawned when the node's sync actually starts (sp_syncstart), at
             * the frame the node marks now. */
            sh_scene_spawn at[SH_SCENE_MAX_SPAWNS];
            sh_scene_pose frame = sh_scene_node_frame(e, home);
            int pending;
            for (k = 0; k < e->spawn_count; k++) {
                at[k] = e->spawns[k];
                at[k].at = sh_scene_compose(frame, e->spawns[k].at);
            }
            AcquireSRWLockExclusive(&g_lock);
            pending = sh_scene_pending_set(&g_pending, node, e->node, at, e->spawn_count);
            ReleaseSRWLockExclusive(&g_lock);
            if (!pending) sp_scene_log("scene-place: %s's spawns could not be queued", e->node);
        }
    }
    if (!moved) sp_log(&g_logged_fail, "scene-place: no scene node could be moved; the scene plays at the node");
}

static void sp_doaction(void *self, void *activator)
{
    int decision = SH_MODSCENE_PLACE_PASS;
    void *player = NULL;
    if (sp_is_scene_action(self)) {
        int in_play = sh_modserve_engine_in_play();
        if (in_play == 1) player = sp_player_of(activator);
        decision = sh_modscene_place_decide(in_play, 1, player != NULL);
    }
    if (decision == SH_MODSCENE_PLACE_DROP) {
        char who[64];
        sp_log(&g_logged_drop, "scene-place: dropped an Activate from %s: only a player can play a scene node",
               sp_entity_name(activator, who, sizeof who));
        return;
    }
    if (decision == SH_MODSCENE_PLACE_PLACE) sp_place_targets(self, player);
    g_orig_doaction(self, activator);
}

static void *sp_link_sync(void *link)
{
    return link ? sp_read_ptr(link, SP_LINK_SYNC_PTR) : NULL;
}

/* The interaction link's sync start. A scene node with queued spawns gets them
 * in the same call, right after the engine started its sync, so a demon and
 * the player's track begin on one tick; a sync that did not start (the node
 * was busy, disabled or used up) spawns nothing. */
static void sp_syncstart(void *link, void *owner, void *arg3, void *link_data, void *player)
{
    sh_scene_pending queued;
    void *before = sp_link_sync(link), *after;
    int have, k;
    g_orig_syncstart(link, owner, arg3, link_data, player);
    if (!owner || !g_pending.count) return;
    AcquireSRWLockExclusive(&g_lock);
    have = sh_scene_pending_take(&g_pending, owner, &queued);
    ReleaseSRWLockExclusive(&g_lock);
    if (!have) return;
    after = sp_link_sync(link);
    if (!after || after == before) {
        sp_scene_log("scene-place: %s's sync did not start; nothing was spawned", queued.node_name);
        return;
    }
    if (sh_modserve_engine_in_play() != 1) return;
    for (k = 0; k < queued.count; k++) {
        const sh_scene_spawn *s = &queued.spawns[k];
        const char *why = sp_spawn_scene_entity(s->entitydef, s->at);
        if (why)
            sp_scene_log("scene-place: %s could not spawn %s: %s", queued.node_name, s->entitydef, why);
        else
            sp_scene_log("scene-place: %s spawned %s at %.1f %.1f %.1f, yaw %.1f with its sync", queued.node_name,
                         s->entitydef, s->at.x, s->at.y, s->at.z, s->at.yaw);
    }
}

static void sp_return(const sh_modscene_place_record *rec)
{
    void *player = sh_entity_find(rec->player);
    sp_post_fn post = (sp_post_fn)sp_fn(SP_POST_EVENT_RVA);
    sp_arg v, a;
    int ok = 0;
    if (!player || sp_player_of(player) != player || !post || !sp_events_ready()) {
        sp_log(&g_logged_fail, "scene-place: %s is gone; nobody to return after the scene", rec->player);
        return;
    }
    memset(&v, 0, sizeof v);
    memset(&a, 0, sizeof a);
    v.type = 'v';
    memcpy(v.f, rec->origin, sizeof rec->origin);
    a.type = 'a';
    a.f[0] = rec->angles[0];
    a.f[1] = rec->angles[1];
    a.f[2] = 0.0f;
    __try { post(player, g_ev_teleport, 0, &v, &a); ok = 1; }
    __except (sp_av_only(GetExceptionCode())) { ok = 0; }
    if (ok)
        sp_log(&g_logged_return, "scene-place: %s returned after the scene (%.1f %.1f %.1f, yaw %.1f)",
               rec->player, rec->origin[0], rec->origin[1], rec->origin[2], rec->angles[1]);
    else
        sp_log(&g_logged_fail, "scene-place: returning %s faulted; the player stays where the scene left them", rec->player);
}

static void sp_used(void *self)
{
    sh_modscene_place_record rec;
    int have;
    AcquireSRWLockExclusive(&g_lock);
    have = sh_modscene_place_take(&g_table, self, &rec);
    ReleaseSRWLockExclusive(&g_lock);
    if (sh_modserve_engine_in_play() == 1) {
        const sh_scene_entry *e = have ? NULL : sp_scene_of(self);
        if (have) sp_return(&rec);
        else if (e) sp_scene_log("scene-place: %s finished its scene; the player stays", e->node);
        /* A scene node's scene is over: a HUD its scene hid and never showed comes back
         * (its show row can sit on a frame the sync no longer plays). */
        if ((have || e) && sh_modhud_scene_restore())
            sp_scene_log("scene-place: the HUD was shown again when the scene finished");
    }
    g_orig_used(self);
}

static unsigned char sp_on_action_request(void *component, void *owner, const int *action, void *requester)
{
    if (!requester) {
        char who[64];
        int number = -1;
        __try { number = action ? *action : -1; } __except (sp_av_only(GetExceptionCode())) { number = -1; }
        sp_log(&g_logged_null, "scene-place: refused interaction action %d for %s: it has no requester", number,
               sp_entity_name(owner, who, sizeof who));
        return 0;
    }
    return g_orig_request(component, owner, action, requester);
}

static const sig_result *sp_sig(const sig_result *results, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (results[i].name && !strcmp(results[i].name, name)) return &results[i];
    return NULL;
}

/* Prepare and commit one detour, publishing its trampoline first. */
static void *sp_detour(const sig_result *r, void *detour, size_t stolen, void **orig)
{
    void *tramp = r ? sh_prepare_detour_sig(r, detour, stolen) : NULL;
    if (!tramp) return NULL;
    *orig = tramp;
    if (sh_commit_detour(tramp) != B2_PATCH_OK) {
        if (sh_uninstall_detour(tramp)) *orig = NULL;
        return NULL;
    }
    return tramp;
}

static void sp_guard_install(const sig_result *results, size_t n)
{
    if (sp_detour(sp_sig(results, n, "InteractionOnActionRequest"), (void *)sp_on_action_request,
                  SP_REQUEST_STOLEN, (void **)&g_orig_request))
        backend_log("scene-place: interaction requests without a requester are refused");
    else
        backend_log("scene-place: the empty-requester guard is not available on this build");
}

static const sig_result *sp_clean(const sig_result *results, size_t n, const char *name)
{
    const sig_result *r = sp_sig(results, n, name);
    return (r && r->status == SIG_OK) ? r : NULL;
}

/* The scene spawns: the engine's create and finish calls (private signatures)
 * and the decl registry the shared results name. Without them a scenes
 * block's spawns are skipped with a line; placement is unaffected. */
static void sp_spawns_install(const sig_result *results, size_t n)
{
    sig_result own[3];
    const sig_result *anchor = sp_clean(results, n, "DeclRegistryAnchor"), *reg = sp_clean(results, n, "DeclRegisterFile");
    const sig_result *type = sp_clean(results, n, "DeclTypeByName"), *find = sp_clean(results, n, "DeclFind");
    int i;
    for (i = 0; i < 3; i++)
        if (!g_base || sig_resolve_one(g_base, &SH_MODSCENE_SPAWN_SIGNATURES[i], &own[i]) != SIG_OK) {
            backend_log("scene-place: scene spawns are unavailable on this executable (the engine spawn calls did not resolve)");
            return;
        }
    if (!anchor || !reg || !type || !find || !sp_fn(SP_POST_EVENT0_RVA)) {
        backend_log("scene-place: scene spawns are unavailable on this executable (the decl registry is unresolved)");
        return;
    }
    g_create = (sp_create_fn)own[0].addr;
    g_finish = (sp_finish_fn)own[1].addr;
    g_decl_anchor = (const uint8_t *)anchor->addr;
    g_decl_register = (void *)reg->addr;
    g_decl_type = (sp_type_fn)type->addr;
    g_decl_find = (sp_find_decl_fn)find->addr;
    if (!sp_detour(&own[2], (void *)sp_syncstart, SP_SYNCSTART_STOLEN, (void **)&g_orig_syncstart)) {
        g_create = NULL;
        g_finish = NULL;
        backend_log("scene-place: scene spawns are unavailable on this executable (the sync start could not be detoured)");
        return;
    }
    backend_log("scene-place: scene spawns available: a mod's scenes block can spawn at its node when its sync starts");
}

int sh_modscene_place_install(const sig_result *results, size_t n, const unsigned char *module_base)
{
    void *used;
    g_base = module_base;
    sp_guard_install(results, n);
    sp_spawns_install(results, n);
    if (!sp_fn(SP_CAST_PLAYER_RVA) || !sp_fn(SP_REF_REFRESH_RVA) || !sp_fn(SP_PROCESS_ARGS_RVA) ||
        !sp_fn(SP_ARG_CLEAR_RVA) || !sp_fn(SP_POST_EVENT_RVA)) {
        backend_log("scene-place SKIPPED -- scene nodes play at the node on this executable");
        return 0;
    }
    used = sp_detour(sp_sig(results, n, "InteractableUsedOutput"), (void *)sp_used, SP_USED_STOLEN,
                     (void **)&g_orig_used);
    if (!used) {
        backend_log("scene-place SKIPPED -- the interactable USED output could not be detoured");
        return 0;
    }
    if (!sp_detour(sp_sig(results, n, "SceneInteractionDoAction"), (void *)sp_doaction, SP_DOACTION_STOLEN,
                   (void **)&g_orig_doaction)) {
        /* Without the DoAction half nothing is ever recorded; take the USED half back out. */
        if (sh_uninstall_detour(used)) g_orig_used = NULL;
        backend_log("scene-place SKIPPED -- the interaction DoAction could not be detoured");
        return 0;
    }
    backend_log("scene-place installed: mod scene nodes play at the activating player");
    return 1;
}

static void sp_scene_refusal(void *context, const char *line)
{
    (void)context;
    backend_log(line);
}

static void sp_read_scenes(void *context, const char *id, const char *version, int legacy, const char *extra)
{
    (void)context; (void)version; (void)legacy;
    if (id && extra) (void)sh_scene_add_bundle(&g_scenes, id, extra, strlen(extra), sp_scene_refusal, NULL);
}

void sh_modscene_place_play_begin(void)
{
    int declared;
    AcquireSRWLockExclusive(&g_lock);
    sh_modscene_place_table_clear(&g_table);
    memset(g_homes, 0, sizeof g_homes);
    g_home_count = 0;
    sh_scene_pending_clear(&g_pending);
    ReleaseSRWLockExclusive(&g_lock);
    /* The scenes blocks of the admitted bundles, read again for every Play. */
    sh_scene_table_free(&g_scenes);
    if (sh_modserve_each_bundle_extra(sp_read_scenes, NULL) < 0) sh_scene_table_free(&g_scenes);
    declared = g_scenes.count;
    if (declared)
        backend_log(declared == 1 ? "scene-place: 1 mod scene node declares where its scene plays"
                                  : "scene-place: mod scene nodes declare where their scenes play");
    InterlockedExchange(&g_scene_lines, 0);
    InterlockedExchange(&g_logged_homes, 0);
    InterlockedExchange(&g_logged_place, 0);
    InterlockedExchange(&g_logged_return, 0);
    InterlockedExchange(&g_logged_drop, 0);
    InterlockedExchange(&g_logged_null, 0);
    InterlockedExchange(&g_logged_fail, 0);
    InterlockedExchange(&g_logged_miss, 0);
}
#endif
