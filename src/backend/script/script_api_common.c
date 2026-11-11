/* script_api_common.c -- see script_api_common.h. The pure part (argument
 * checks, entity objects, events through the engine table, the animation
 * observer) builds alone with SH_SCRIPT_PURE for the tests; the engine part
 * fills the engine table on the backend. */
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script_api_common.h"
#include "script_hooks.h"

/* ------------------------------------------------------------ the engine */

static sh_script_engine g_engine;
static int g_have_engine;

void sh_script_api_set_engine(const sh_script_engine *engine)
{
    if (engine) {
        g_engine = *engine;
        g_have_engine = 1;
    } else {
        memset(&g_engine, 0, sizeof g_engine);
        g_have_engine = 0;
    }
}

const sh_script_engine *sh_script_api_engine(void)
{
    return g_have_engine ? &g_engine : NULL;
}

#define ENG(fn) (g_have_engine && g_engine.fn)

int sh_script_api_now_ms(int *ms)
{
    return ENG(game_time_ms) ? g_engine.game_time_ms(ms) : 0;
}

int sh_script_api_ground_trace(int (*trace)(const float from[3], const float to[3], uint32_t ignore_spawn_id,
                                            sh_script_trace *out),
                               const float pos[3], float out[3])
{
    float from[3], to[3];
    sh_script_trace t;
    if (!trace || !pos || !out) return 0;
    from[0] = to[0] = pos[0];
    from[1] = to[1] = pos[1];
    from[2] = pos[2] + SH_SCRIPT_GROUND_ABOVE;
    to[2] = pos[2] - SH_SCRIPT_GROUND_DEPTH;
    memset(&t, 0, sizeof t);
    if (!trace(from, to, SH_SCRIPT_SPAWN_ID_NONE, &t)) return 0;
    if (!(t.fraction < 1.0f) || t.normal[2] < SH_SCRIPT_GROUND_FLAT) return 0;
    out[0] = t.end[0];
    out[1] = t.end[1];
    out[2] = t.end[2];
    return 1;
}

void sh_script_api_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sh_script_api_entity_register();
    sh_script_api_world_register();
}

/* ------------------------------------------------------------ entities */

static void *resolve_handle(sh_script_handle h)
{
    void *ent;
    if (!h) return NULL;
    ent = sh_script_handle_entity(h);
    if (ent) return ent;
    /* Recorded but not yet delivered (this frame's spawn): the handle table
     * answers only live entries, so ask the engine for its spawn id. */
    if (sh_script_handle_known(h) && ENG(resolve)) {
        uint32_t id = sh_script_handle_spawn_id(h);
        if (id != SH_SCRIPT_SPAWN_ID_NONE) return g_engine.resolve(id);
    }
    return NULL;
}

void *sh_script_api_entity(lua_State *L, int arg, const char *call, sh_script_handle *h)
{
    uint64_t handle = sh_script_check_entity(L, arg, call);
    if (h) *h = (sh_script_handle)handle;
    if (!handle || handle > 0xFFFFFFFFull) return NULL;
    return resolve_handle((sh_script_handle)handle);
}

int sh_script_api_owns(lua_State *L, sh_script_handle h)
{
    return h && sh_script_owns_entity(L, h);
}

void *sh_script_api_owned(lua_State *L, int arg, const char *call, const char *verb, sh_script_handle *h)
{
    sh_script_handle own = 0;
    void *ent = sh_script_api_entity(L, arg, call, &own);
    if (h) *h = own;
    if (own && !sh_script_api_owns(L, own))
        sh_script_refuse(L, "\"%s\" can only %s this mod's own entities; this one is not", call, verb);
    return ent;
}

sh_script_handle sh_script_api_handle_of(void *ent)
{
    uint32_t id;
    sh_script_handle h;
    if (!ent || !ENG(spawn_id)) return 0;
    id = g_engine.spawn_id(ent);
    if (id == SH_SCRIPT_SPAWN_ID_NONE || id == SH_SCRIPT_SPAWN_ID_WORLD) return 0;
    h = sh_script_handles_find(id, NULL);
    return h ? h : sh_script_handles_ref(id);
}

void sh_script_api_push_engine_entity(lua_State *L, void *ent)
{
    sh_script_handle h = sh_script_api_handle_of(ent);
    if (h) sh_script_push_entity(L, h);
    else lua_pushnil(L);
}

int sh_script_api_kind(void *ent)
{
    if (!ent) return SH_SCRIPT_KIND_GONE;
    return ENG(kind) ? g_engine.kind(ent) : SH_SCRIPT_KIND_OTHER;
}

/* ----------------------------------------------------------- arguments */

static const char *type_words(lua_State *L, int arg)
{
    if (lua_isnoneornil(L, arg)) return "nothing";
    if (luaL_testudata(L, arg, "entity")) return "an entity";
    switch (lua_type(L, arg)) {
    case LUA_TNUMBER: return "a number";
    case LUA_TSTRING: return "text";
    case LUA_TBOOLEAN: return "true or false";
    case LUA_TTABLE: return "a table";
    case LUA_TFUNCTION: return "a function";
    default: return luaL_typename(L, arg);
    }
}

/* Field `key` (else index `index`) of the table at arg as a finite number. */
static int vec_part(lua_State *L, int arg, const char *key, int index, float *out)
{
    int ok;
    double v;
    lua_getfield(L, arg, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_rawgeti(L, arg, index);
    }
    ok = lua_type(L, -1) == LUA_TNUMBER;
    v = ok ? lua_tonumber(L, -1) : 0;
    lua_pop(L, 1);
    if (!ok || v != v || v > 1e9 || v < -1e9) return 0;
    *out = (float)v;
    return 1;
}

void sh_script_api_check_vec(lua_State *L, int arg, const char *call, const char *what, float out[3])
{
    arg = lua_absindex(L, arg);
    if (lua_type(L, arg) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" needs %s (a position with x, y and z), got %s", call, what, type_words(L, arg));
    if (!vec_part(L, arg, "x", 1, &out[0]) || !vec_part(L, arg, "y", 2, &out[1]) || !vec_part(L, arg, "z", 3, &out[2]))
        sh_script_refuse(L, "\"%s\" needs %s with x, y and z as numbers; one of them is missing or not a number",
                         call, what);
}

int sh_script_api_opt_vec(lua_State *L, int arg, const char *call, const char *what, float out[3])
{
    if (lua_isnoneornil(L, arg)) return 0;
    sh_script_api_check_vec(L, arg, call, what, out);
    return 1;
}

int sh_script_api_check_point(lua_State *L, int arg, const char *call, const char *what, float out[3])
{
    if (luaL_testudata(L, arg, "entity")) {
        void *ent = sh_script_api_entity(L, arg, call, NULL);
        return ent && ENG(origin) && g_engine.origin(ent, out);
    }
    if (lua_type(L, arg) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" needs %s (an entity, or a position with x, y and z), got %s", call, what,
                         type_words(L, arg));
    sh_script_api_check_vec(L, arg, call, what, out);
    return 1;
}

double sh_script_api_check_number(lua_State *L, int arg, const char *call, const char *what)
{
    double v;
    if (lua_type(L, arg) != LUA_TNUMBER)
        sh_script_refuse(L, "\"%s\" needs %s as a number, got %s", call, what, type_words(L, arg));
    v = lua_tonumber(L, arg);
    if (v != v || v > 1e18 || v < -1e18)
        sh_script_refuse(L, "\"%s\" needs %s as an ordinary number, not infinity or nan", call, what);
    return v;
}

int sh_script_api_opt_number(lua_State *L, int arg, const char *call, const char *what, double *out)
{
    if (lua_isnoneornil(L, arg)) return 0;
    *out = sh_script_api_check_number(L, arg, call, what);
    return 1;
}

const char *sh_script_api_check_text(lua_State *L, int arg, const char *call, const char *what, size_t max_len)
{
    size_t n;
    const char *s;
    if (lua_type(L, arg) != LUA_TSTRING)
        sh_script_refuse(L, "\"%s\" needs %s as text, got %s", call, what, type_words(L, arg));
    s = lua_tolstring(L, arg, &n);
    if (n == 0) sh_script_refuse(L, "\"%s\" needs %s, got empty text", call, what);
    if (n > max_len) sh_script_refuse(L, "\"%s\" was given %s longer than %u characters", call, what, (unsigned)max_len);
    if (strlen(s) != n) sh_script_refuse(L, "\"%s\" was given %s with a zero byte in it", call, what);
    return s;
}

void sh_script_api_push_vec(lua_State *L, const float v[3])
{
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v[0]); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v[1]); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v[2]); lua_setfield(L, -2, "z");
}

/* -------------------------------------------------------------- events */

sh_script_earg sh_script_earg_int(int v)
{
    sh_script_earg a;
    memset(&a, 0, sizeof a);
    a.type = 'i';
    a.i = v;
    return a;
}

sh_script_earg sh_script_earg_float(float v)
{
    sh_script_earg a;
    memset(&a, 0, sizeof a);
    a.type = 'f';
    a.f = v;
    return a;
}

sh_script_earg sh_script_earg_vec(const float v[3])
{
    sh_script_earg a;
    memset(&a, 0, sizeof a);
    a.type = 'v';
    memcpy(a.v, v, sizeof a.v);
    return a;
}

sh_script_earg sh_script_earg_text(char type, const char *s)
{
    sh_script_earg a;
    memset(&a, 0, sizeof a);
    a.type = type == '1' ? '1' : 's';
    a.s = s ? s : "";
    return a;
}

sh_script_earg sh_script_earg_entity(void *ent)
{
    sh_script_earg a;
    memset(&a, 0, sizeof a);
    a.type = 'e';
    a.p = ent;
    return a;
}

sh_script_earg sh_script_earg_no_anim(void)
{
    sh_script_earg a;
    memset(&a, 0, sizeof a);
    a.type = '4';
    return a;
}

int sh_script_api_event(void *ent, const char *name, const sh_script_earg *args, int n, sh_script_earg *ret)
{
    sh_script_earg r;
    int ok;
    memset(&r, 0, sizeof r);
    if (!ent || !name || n < 0 || n > SH_SCRIPT_EVENT_ARGS || !ENG(event)) {
        if (ret) *ret = r;
        return 0;
    }
    ok = g_engine.event(ent, name, args, n, &r, 0);
    if (ret) *ret = r;
    return ok;
}

int sh_script_api_post(void *ent, const char *name, const sh_script_earg *args, int n)
{
    if (!ent || !name || n < 0 || n > SH_SCRIPT_EVENT_ARGS || !ENG(event)) return 0;
    return g_engine.event(ent, name, args, n, NULL, 1);
}

/* ------------------------------------------------- animation observation */

/* One row per entity with a standing request, made by the request and
 * dropped when its entity is gone (each frame) or when the handle table
 * releases it; the rows grow with the entities that have one. */
typedef struct anim_row {
    sh_script_handle handle;
    char *dest;              /* malloc'd node names */
    char *via;
    unsigned flags;
} anim_row;

static anim_row *g_anim;
static int g_nanim, g_canim;

static anim_row *anim_find(sh_script_handle h)
{
    int i;
    if (!h) return NULL;
    for (i = 0; i < g_nanim; i++) if (g_anim[i].handle == h) return &g_anim[i];
    return NULL;
}

static void anim_drop(int i)
{
    free(g_anim[i].dest);
    free(g_anim[i].via);
    g_anim[i] = g_anim[--g_nanim];
}

void sh_script_api_anim_reset(void)
{
    while (g_nanim) anim_drop(g_nanim - 1);
}

void sh_script_api_anim_request(sh_script_handle h, const char *dest, const char *via)
{
    anim_row *r = anim_find(h);
    char *d, *v;
    if (!h) return;
    d = _strdup(dest ? dest : "");
    v = _strdup(via ? via : "");
    if (!d || !v) { free(d); free(v); return; }
    if (!r) {
        if (g_nanim == g_canim) {
            int cap = g_canim ? g_canim * 2 : 64;
            anim_row *grown = (anim_row *)realloc(g_anim, (size_t)cap * sizeof *grown);
            if (!grown) { free(d); free(v); return; }
            g_anim = grown;
            g_canim = cap;
        }
        r = &g_anim[g_nanim++];
    } else {
        free(r->dest);
        free(r->via);
    }
    memset(r, 0, sizeof *r);
    r->handle = h;
    r->dest = d;
    r->via = v;
}

/* Node names compare without case; a request may name the state alone. */
static int node_is(const char *node, const char *want)
{
    const char *slash;
    if (!want[0] || !node[0]) return 0;
    if (!_stricmp(node, want)) return 1;
    if (strchr(want, '/')) return 0;
    slash = strrchr(node, '/');
    return slash && !_stricmp(slash + 1, want);
}

static void observe_row(anim_row *r, void *ent)
{
    char node[1024];   /* the node the engine reports (its own names) */
    if (!ent || !ENG(anim_node) || !g_engine.anim_node(ent, node, sizeof node)) return;
    if (r->via[0]) {
        if (node_is(node, r->via)) r->flags |= SH_SCRIPT_ANIM_VIA_IN;
        else if (r->flags & SH_SCRIPT_ANIM_VIA_IN) r->flags |= SH_SCRIPT_ANIM_VIA_OUT;
    }
    if (node_is(node, r->dest)) {
        r->flags |= SH_SCRIPT_ANIM_DEST;
        if (r->via[0]) r->flags |= SH_SCRIPT_ANIM_VIA_OUT;   /* the web went through the via node */
    } else if (r->flags & SH_SCRIPT_ANIM_DEST) {
        r->flags |= SH_SCRIPT_ANIM_FINISHED;
    }
}

int sh_script_api_anim_dest(sh_script_handle h, char *out, size_t cap)
{
    anim_row *r = anim_find(h);
    if (out && cap) out[0] = 0;
    if (!r || !r->dest[0]) return 0;
    strncpy_s(out, cap, r->dest, _TRUNCATE);
    return 1;
}

char *sh_script_api_anim_dest_of(sh_script_handle h)
{
    anim_row *r = anim_find(h);
    return r && r->dest[0] ? r->dest : NULL;
}

void sh_script_api_anim_observe(sh_script_handle h, void *ent)
{
    anim_row *r = anim_find(h);
    if (r) observe_row(r, ent);
}

unsigned sh_script_api_anim_flags(sh_script_handle h, void *ent, int *requested)
{
    anim_row *r = anim_find(h);
    if (requested) *requested = r != NULL;
    if (!r) return 0;
    observe_row(r, ent);
    return r->flags;
}

#define FRAME_FNS 4
static void (*g_frame_fns[FRAME_FNS])(void);

void sh_script_api_on_frame(void (*fn)(void))
{
    int i;
    for (i = 0; i < FRAME_FNS; i++) {
        if (g_frame_fns[i] == fn) return;
        if (!g_frame_fns[i]) { g_frame_fns[i] = fn; return; }
    }
}

static void frame_fns(void)
{
    int i;
    for (i = 0; i < FRAME_FNS; i++) if (g_frame_fns[i]) g_frame_fns[i]();
}

static int g_room = -1;   /* free engine entity slots this frame, -1 unread */

static void read_room(void)
{
    int n, num, used = 0;
    g_room = -1;
    if (!ENG(slots) || !ENG(slot_spawn_id)) return;
    n = g_engine.slots();
    for (num = 0; num < n; num++) if (g_engine.slot_spawn_id(num) != SH_SCRIPT_SPAWN_ID_NONE) used++;
    g_room = used < SH_SCRIPT_ENGINE_ENTITIES ? SH_SCRIPT_ENGINE_ENTITIES - used : 0;
}

void sh_script_api_check_room(lua_State *L, const char *call)
{
    if (g_room < 0) read_room();
    if (g_room >= 0 && g_room <= SH_SCRIPT_ENGINE_RESERVE)
        sh_script_refuse(L, "\"%s\" cannot add an entity now: the game holds at most %d entities at once and nearly "
                            "all are in use; remove some first", call, SH_SCRIPT_ENGINE_ENTITIES);
    if (g_room > 0) g_room--;
}

void sh_script_api_frame(void)
{
    int i;
    read_room();
    for (i = 0; i < g_nanim; i++) {
        anim_row *r = &g_anim[i];
        void *ent;
        if (!sh_script_handle_known(r->handle)) {   /* removed, or a handle of an earlier map */
            anim_drop(i--);
            continue;
        }
        if (r->flags & SH_SCRIPT_ANIM_FINISHED) continue;
        ent = resolve_handle(r->handle);
        if (ent) observe_row(r, ent);
    }
    frame_fns();
}

#ifdef SH_SCRIPT_PURE

int sh_script_api_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    (void)results; (void)count; (void)module_base;
    sh_script_api_register();
    return 1;
}

#else /* the engine side ------------------------------------------------- */

#include "../backend_log.h"
#include "../entity.h"
#include "../engine_build.h"
#include "../typeinfo.h"
#include "../modscene_decl.h"

/* The first API_SIG_COUNT are required; the difficulty is optional. */
enum { API_SIG_PROCESS, API_SIG_POST, API_SIG_CLEAR, API_SIG_ENTITY, API_SIG_TRACE, API_SIG_COUNT,
       API_SIG_DIFFICULTY = API_SIG_COUNT };

/* idGameLocal */
#define GAME_TIME_SLOT      0x220u
#define GAME_DIFFICULTY_SLOT 0x670u
#define GAME_CREATE_SLOT    0x448u
#define GAME_FINISH_SLOT    0x458u
#define GAME_LOOKUP_SLOT    0x7F0u      /* +0x7F8: the same, then the idEntity cast */
#define GAME_TRACES         0xE0878u
/* idEntity */
#define ENT_NUMBER          0x08u
#define ENT_DECL            0x6D0u
#define ENT_PHYSICS         0x808u
#define ENT_SPAWN_POSE      0x3A8u      /* spawnPosition, spawnOrientation rows at +0x3B4 */
#define VSLOT_SET_ORIGIN    0x338u
#define VSLOT_SET_AXIS      0x358u
#define VSLOT_UPDATE_VISUALS 0x3C0u
#define PSLOT_GET_ORIGIN    0x80u
#define PSLOT_GET_AXIS      0x88u
#define DECL_NAME           0x08u
#define DECL_STATE          0x2Cu       /* bit 0 failed to load, bit 1 not loaded */
/* idAI2 */
#define AI_HEALTH           0x2B290u
#define HEALTH_MAX          0x14u
#define HEALTH_CUR          0x1Cu
#define HEALTH_HEAL_SLOT    0x58u
#define AI_ANIMWEB          0x225C8u
#define AI_ANIM_NODE        0x2271Cu
#define WEB_NODES           0x58u
#define WEB_SUBWEBS         0x70u
#define WEB_SUBWEB_COUNT    0x78u
#define WEB_SUBWEB_STRIDE   0x38u
#define WEB_SUBWEB_NAME     0x18u
#define WEB_STATES          0x88u
#define WEB_STATE_COUNT     0x90u
/* events */
#define EVMGR_VSLOT         0x90u
#define EV_FIND_VSLOT       0x10u
#define EV_FORMAT           0x08u
#define EV_NUMARGS          0x2Cu
/* traces */
#define TRACE_CONTENTS      0x103085u   /* the SnapMap laser hazard's mask: the world, players and demons */
#define TRACE_RESULT_SIZE   0x80u
#define TRACE_END           0x04u
#define TRACE_NORMAL        0x44u
#define TRACE_HIT_ID        0x6Cu
#define ENTITY_SLOTS        0x3FFE

typedef struct { char type; char pad[7]; uint64_t value; uint64_t extra; } eng_arg;
typedef void *(*process_fn)(void *obj, void *ret, void *def, int n, eng_arg *a0, eng_arg *a1, eng_arg *a2,
                            eng_arg *a3, eng_arg *a4, eng_arg *a5, eng_arg *a6, eng_arg *a7);
typedef char  (*post_fn)(void *obj, void *def, int ms, int n, ...);
typedef void  (*clear_fn)(void *arg);
typedef void *(*arg_entity_fn)(void *arg, void *ent);
typedef void *(*trace_fn)(void *traces, uint64_t *handle, void *result, const float *start, const float *end,
                          void *clip, const float *axis, uint32_t contents, uint32_t pass, unsigned char keep,
                          const char *name, uint32_t pass2);
typedef void *(*lookup_fn)(void *gm, const uint32_t *spawn_id);
typedef int   (*time_fn)(void *gm, int group);
typedef int   (*difficulty_fn)(void *gm);
typedef const float *(*phys_get_fn)(void *phys, int id);
typedef void  (*set_vec_fn)(void *ent, const float *v);
typedef void  (*vfn0_fn)(void *ent);
typedef unsigned char (*heal_fn)(void *health, int component, float amount, unsigned char overflow,
                                 unsigned char allow_over_max);
typedef void *(*vfn_name_fn)(void *self, const char *name);
typedef void *(*vfn0r_fn)(void *self);
typedef void *(*create_fn)(void *gm, void *decl, void *edit_args);
typedef void  (*finish_fn)(void *gm, void *ent, int entnum, unsigned long long spawn_id, int arg5, void *arg6);
typedef void *(*type_fn)(void *registry, const char *type);
typedef void *(*find_decl_fn)(void *manager, const char *name, unsigned char make_default);

static const uint8_t *g_base;
static process_fn    g_process;
static post_fn       g_post;
static clear_fn      g_clear;
static arg_entity_fn g_arg_entity;
static trace_fn      g_trace;
static void         *g_lookup;          /* idGameLocal vtable +0x7F0, checked against the live vtable */
static int           g_lookup_checked;  /* 1 verified, -1 refused */
static create_fn     g_create;
static finish_fn     g_finish;
static const uint8_t *g_decl_anchor;
static void         *g_decl_register;
static type_fn       g_decl_type;
static find_decl_fn  g_decl_find;
static difficulty_fn g_difficulty;
static uintptr_t     g_vt_ai, g_vt_player, g_vt_health;
static char          g_log_last[512];   /* the line written last */
static const float   k_identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

static void api_log(const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (!strcmp(line, g_log_last)) return;   /* the same line again says nothing new */
    strcpy_s(g_log_last, sizeof g_log_last, line);
    backend_log(line);
}

/* Only access violations are contained; an engine idException must reach
 * the engine's own handler. */
static int av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static void *read_ptr(const void *base, size_t off)
{
    void *p = NULL;
    if (!base) return NULL;
    __try { p = *(void * const *)((const uint8_t *)base + off); }
    __except (av_only(GetExceptionCode())) { p = NULL; }
    return p;
}

static void *vfn(void *obj, size_t slot)
{
    void *vt = read_ptr(obj, 0);
    return vt ? read_ptr(vt, slot) : NULL;
}

static int read_cstr(const char *src, char *out, size_t cap)
{
    size_t i = 0;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!src) return 0;
    __try {
        for (; i + 1 < cap && src[i]; i++) out[i] = src[i];
        out[i] = 0;
    } __except (av_only(GetExceptionCode())) { out[0] = 0; return 0; }
    return 1;
}

static int lookup_ok(void *gm)
{
    if (g_lookup_checked) return g_lookup_checked == 1;
    g_lookup_checked = g_lookup && vfn(gm, GAME_LOOKUP_SLOT) == g_lookup ? 1 : -1;
    if (g_lookup_checked != 1)
        api_log("Mod scripts: the game's entity lookup is not where this version of Snapmap+ expects it, so script "
                "calls on entities answer nothing.");
    return g_lookup_checked == 1;
}

static void *e_resolve(uint32_t spawn_id)
{
    void *gm = sh_entity_gamemgr();
    lookup_fn f;
    void *ent = NULL;
    if (!gm || !lookup_ok(gm)) return NULL;
    f = (lookup_fn)vfn(gm, GAME_LOOKUP_SLOT + 8);
    if (!f) return NULL;
    __try { ent = f(gm, &spawn_id); } __except (av_only(GetExceptionCode())) { ent = NULL; }
    return ent;
}

static uint32_t e_slot_spawn_id(int number)
{
    void *gm = sh_entity_gamemgr();
    uint32_t id = SH_SCRIPT_SPAWN_ID_NONE;
    if (!gm || number < 0 || number >= ENTITY_SLOTS) return id;
    __try { id = *(const uint32_t *)((const uint8_t *)gm + SH_SCRIPT_GAME_SPAWN_IDS + (size_t)number * 4); }
    __except (av_only(GetExceptionCode())) { id = SH_SCRIPT_SPAWN_ID_NONE; }
    return id;
}

static uint32_t e_spawn_id(void *ent)
{
    int num = -1;
    if (!ent) return SH_SCRIPT_SPAWN_ID_NONE;
    __try { num = *(const int *)((const uint8_t *)ent + ENT_NUMBER); }
    __except (av_only(GetExceptionCode())) { num = -1; }
    return e_slot_spawn_id(num);
}

static int e_slots(void)
{
    return ENTITY_SLOTS;
}

static int e_kind(void *ent)
{
    uintptr_t vt = (uintptr_t)read_ptr(ent, 0);
    if (!vt) return SH_SCRIPT_KIND_GONE;
    if (g_vt_ai && vt == g_vt_ai) return SH_SCRIPT_KIND_AI;
    if (g_vt_player && vt == g_vt_player) return SH_SCRIPT_KIND_PLAYER;
    return SH_SCRIPT_KIND_OTHER;
}

static int phys_read(void *ent, size_t slot, float *out, size_t n)
{
    void *phys = read_ptr(ent, ENT_PHYSICS);
    phys_get_fn get = (phys_get_fn)vfn(phys, slot);
    int ok = 0;
    if (!get) return 0;
    __try {
        const float *v = get(phys, 0);
        if (v) { memcpy(out, v, n * sizeof(float)); ok = 1; }
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_origin(void *ent, float out[3])
{
    return phys_read(ent, PSLOT_GET_ORIGIN, out, 3);
}

static int e_axis(void *ent, float out[9])
{
    return phys_read(ent, PSLOT_GET_AXIS, out, 9);
}

/* The pair the engine's setOrigin event makes: the entity's SetOrigin /
 * SetAxis, then UpdateVisuals. */
static int e_place(void *ent, const float *origin, const float *axis)
{
    set_vec_fn set_origin = (set_vec_fn)vfn(ent, VSLOT_SET_ORIGIN);
    set_vec_fn set_axis = (set_vec_fn)vfn(ent, VSLOT_SET_AXIS);
    vfn0_fn update = (vfn0_fn)vfn(ent, VSLOT_UPDATE_VISUALS);
    int ok = 0;
    if (!set_origin || !set_axis || !update) return 0;
    __try {
        if (origin) set_origin(ent, origin);
        if (axis) set_axis(ent, axis);
        update(ent);
        ok = 1;
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_def_name(void *ent, char *out, size_t cap)
{
    void *decl = read_ptr(ent, ENT_DECL);
    return decl && read_cstr((const char *)read_ptr(decl, DECL_NAME), out, cap) && out[0];
}

static void *evmgr(void)
{
    void *declmgr = sh_typeinfo_get_declmgr();
    vfn0r_fn acc = (vfn0r_fn)vfn(declmgr, EVMGR_VSLOT);
    void *m = NULL;
    if (!acc) return NULL;
    __try { m = acc(declmgr); } __except (av_only(GetExceptionCode())) { m = NULL; }
    return m;
}

/* The event definition named `name` with exactly n arguments whose types the
 * caller's match (a 'b' takes an 'i'). */
static void *find_event(const char *name, const sh_script_earg *args, int n)
{
    void *m = evmgr(), *def = NULL;
    vfn_name_fn find = (vfn_name_fn)vfn(m, EV_FIND_VSLOT);
    char format[16];
    int numargs = -1, i;
    if (!find) return NULL;
    __try { def = find(m, name); } __except (av_only(GetExceptionCode())) { def = NULL; }
    if (!def) return NULL;
    if (!read_cstr((const char *)read_ptr(def, EV_FORMAT), format, sizeof format)) return NULL;
    __try { numargs = *(const int *)((const uint8_t *)def + EV_NUMARGS); }
    __except (av_only(GetExceptionCode())) { numargs = -1; }
    if (numargs != n || (int)strlen(format) != n) {
        api_log("Mod scripts: the game's event %s does not take the arguments Snapmap+ sends (%d, format '%s').",
                name, numargs, format);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        char want = format[i], have = args[i].type;
        if (want == have || (want == 'b' && have == 'i')) continue;
        api_log("Mod scripts: the game's event %s takes '%c' as argument %d, not '%c'.", name, want, i + 1, have);
        return NULL;
    }
    return def;
}

/* Build engine arguments; 0 when an entity argument could not be built. */
static int build_args(const sh_script_earg *args, int n, eng_arg *a)
{
    int i;
    memset(a, 0, sizeof(eng_arg) * SH_SCRIPT_EVENT_ARGS);
    for (i = 0; i < n; i++) {
        switch (args[i].type) {
        case 'i': a[i].type = 'i'; a[i].value = (uint64_t)(uint32_t)args[i].i; break;
        case 'f': { uint32_t bits; memcpy(&bits, &args[i].f, 4); a[i].type = 'f'; a[i].value = bits; break; }
        case 'v': case 'a': a[i].type = args[i].type; memcpy(&a[i].value, args[i].v, 3 * sizeof(float)); break;
        case 's': case '1': case '2': a[i].type = args[i].type; a[i].value = (uint64_t)(uintptr_t)args[i].s; break;
        case 'd': a[i].type = 'd'; a[i].value = (uint64_t)(uintptr_t)args[i].p; break;
        case '4': a[i].type = '4'; a[i].value = 0; break;
        case 'e': {
            int ok = 0;
            if (!args[i].p) return 0;
            __try { g_arg_entity(&a[i], args[i].p); ok = 1; } __except (av_only(GetExceptionCode())) { ok = 0; }
            if (!ok) return 0;
            break;
        }
        default: return 0;
        }
    }
    return 1;
}

static int e_event(void *ent, const char *name, const sh_script_earg *args, int n, sh_script_earg *ret, int post)
{
    eng_arg a[SH_SCRIPT_EVENT_ARGS];
    uint8_t r[0x18];
    void *def;
    int ok = 0;
    if (!g_process || !g_post || !g_clear || !g_arg_entity) return 0;
    def = find_event(name, args, n);
    if (!def || !build_args(args, n, a)) return 0;
    if (post) {
        __try {
            switch (n) {   /* PostEventArgs reads its arguments as varargs */
            case 0: ok = g_post(ent, def, 0, 0) != 0; break;
            case 1: ok = g_post(ent, def, 0, 1, &a[0]) != 0; break;
            case 2: ok = g_post(ent, def, 0, 2, &a[0], &a[1]) != 0; break;
            case 3: ok = g_post(ent, def, 0, 3, &a[0], &a[1], &a[2]) != 0; break;
            case 4: ok = g_post(ent, def, 0, 4, &a[0], &a[1], &a[2], &a[3]) != 0; break;
            case 5: ok = g_post(ent, def, 0, 5, &a[0], &a[1], &a[2], &a[3], &a[4]) != 0; break;
            case 6: ok = g_post(ent, def, 0, 6, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 0; break;
            case 7: ok = g_post(ent, def, 0, 7, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]) != 0; break;
            default: ok = g_post(ent, def, 0, 8, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]) != 0; break;
            }
        } __except (av_only(GetExceptionCode())) { ok = 0; }
    } else {
        memset(r, 0, sizeof r);
        __try {
            g_process(ent, r, def, n, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]);
            ok = 1;
        } __except (av_only(GetExceptionCode())) { ok = 0; }
        if (ret) {
            memset(ret, 0, sizeof *ret);
            if (ok) {
                ret->type = (char)r[0];
                memcpy(&ret->i, r + 8, 4);
                memcpy(&ret->f, r + 8, 4);
                memcpy(ret->v, r + 8, 3 * sizeof(float));
                memcpy(&ret->p, r + 8, sizeof ret->p);   /* a 'd' decl; an 'e' return is not a pointer */
            }
        }
        __try { g_clear(r); } __except (av_only(GetExceptionCode())) { }
    }
    return ok;
}

static void *ai_health_object(void *ai)
{
    uint8_t *h = (uint8_t *)ai + AI_HEALTH;
    return g_vt_health && (uintptr_t)read_ptr(h, 0) == g_vt_health ? h : NULL;
}

static int e_ai_health(void *ai, float *cur, float *max)
{
    uint8_t *h = (uint8_t *)ai_health_object(ai);
    int ok = 0;
    if (!h) return 0;
    __try {
        *cur = *(const float *)(h + HEALTH_CUR);
        *max = *(const float *)(h + HEALTH_MAX);
        ok = *cur == *cur && *max == *max;
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_ai_heal(void *ai, float amount)
{
    void *h = ai_health_object(ai);
    heal_fn heal = (heal_fn)vfn(h, HEALTH_HEAL_SLOT);
    int ok = 0;
    if (!heal || !(amount >= 0)) return 0;
    __try { heal(h, 0, amount, 0, 0); ok = 1; } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_ai_set_health(void *ai, float value)
{
    uint8_t *h = (uint8_t *)ai_health_object(ai);
    int ok = 0;
    if (!h || !(value >= 1.0f)) return 0;
    __try {
        if (value < *(float *)(h + HEALTH_CUR)) *(float *)(h + HEALTH_CUR) = value;
        ok = 1;
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_anim_node(void *ai, char *out, size_t cap)
{
    void *web = read_ptr(ai, AI_ANIMWEB);
    char sub[96], state[96];
    int ok = 0;
    if (!web || !out || cap < 2) return 0;
    out[0] = 0;
    __try {
        short index = *(const short *)((const uint8_t *)ai + AI_ANIM_NODE);
        const uint8_t *node, *subwebs, *states;
        short si, ti;
        if (index < 0) return 0;
        node = *(const uint8_t * const *)(*(const uint8_t * const *)((const uint8_t *)web + WEB_NODES) + (size_t)index * 8);
        if (!node) return 0;
        si = *(const short *)(node + 2);
        ti = *(const short *)(node + 4);
        subwebs = *(const uint8_t * const *)((const uint8_t *)web + WEB_SUBWEBS);
        states = *(const uint8_t * const *)((const uint8_t *)web + WEB_STATES);
        sub[0] = state[0] = 0;
        if (si >= 0 && si < *(const int *)((const uint8_t *)web + WEB_SUBWEB_COUNT))
            read_cstr(*(const char * const *)(subwebs + (size_t)si * WEB_SUBWEB_STRIDE + WEB_SUBWEB_NAME), sub, sizeof sub);
        if (ti >= 0 && ti < *(const int *)((const uint8_t *)web + WEB_STATE_COUNT))
            read_cstr(*(const char * const *)(states + (size_t)ti * 8), state, sizeof state);
        ok = state[0] != 0;
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    if (!ok) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s", sub, sub[0] ? "/" : "", state);
    return 1;
}

static void *entitydef_manager(void)
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
    } __except (av_only(GetExceptionCode())) { return NULL; }
    return g_decl_type(r, "entityDef");
}

static void yaw_axis(float yaw, float axis[9])
{
    double r = (double)yaw * (3.14159265358979323846 / 180.0);
    float c = (float)cos(r), s = (float)sin(r);
    axis[0] = c;  axis[1] = s; axis[2] = 0;
    axis[3] = -s; axis[4] = c; axis[5] = 0;
    axis[6] = 0;  axis[7] = 0; axis[8] = 1;
}

/* Spawn an entityDef at a pose the way idTarget_Spawn::SpawnSingleEntity
 * does (the companions and scene spawns): create, write spawnPosition and
 * spawnOrientation, finish (register, then CallSpawn, whose spawn subscriber
 * records it for its mod), then "show". */
static void *e_spawn(const char *def, const float origin[3], float yaw, const char **why)
{
    void *gm = sh_entity_gamemgr(), *manager, *decl = NULL, *obj = NULL;
    uint8_t edit_args[32], state = 0xff;
    float pose[12];
    int ok = 0;
    *why = NULL;
    if (!g_create || !g_finish) { *why = "the game's spawn calls were not found in this executable"; return NULL; }
    if (!gm) { *why = "no map is running"; return NULL; }
    if (vfn(gm, GAME_CREATE_SLOT) != (void *)g_create || vfn(gm, GAME_FINISH_SLOT) != (void *)g_finish) {
        *why = "the game's spawn calls are not where this version of Snapmap+ expects them";
        return NULL;
    }
    manager = entitydef_manager();
    if (manager) { __try { decl = g_decl_find(manager, def, 0); } __except (av_only(GetExceptionCode())) { decl = NULL; } }
    if (!decl) { *why = "it is not an entityDef of this mod's build"; return NULL; }
    __try { state = *((const uint8_t *)decl + DECL_STATE); } __except (av_only(GetExceptionCode())) { state = 0xff; }
    if (state & 3) { *why = (state & 1) ? "its entityDef failed to load" : "its entityDef is not loaded"; return NULL; }
    memset(edit_args, 0, sizeof edit_args);   /* the default edit-state arguments (VK 0x140342C30) */
    edit_args[4] = 1;
    edit_args[8] = 2;
    edit_args[12] = 0x01; edit_args[13] = 0x01;
    edit_args[17] = 0x01;
    memcpy(pose, origin, 3 * sizeof(float));
    yaw_axis(yaw, pose + 3);
    __try {
        obj = g_create(gm, decl, edit_args);
        if (obj) {
            memcpy((uint8_t *)obj + ENT_SPAWN_POSE, pose, sizeof pose);
            g_finish(gm, obj, -1, (unsigned long long)SH_SCRIPT_SPAWN_ID_NONE, -1, NULL);
            ok = 1;
        }
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    if (!ok) { *why = obj ? "the game failed while spawning it" : "the game did not create it"; return NULL; }
    (void)e_event(obj, "show", NULL, 0, NULL, 1);
    return obj;
}

static int e_trace(const float from[3], const float to[3], uint32_t ignore, sh_script_trace *out)
{
    void *gm = sh_entity_gamemgr();
    uint8_t result[TRACE_RESULT_SIZE];
    uint64_t handle = 0;
    int ok = 0;
    if (!gm || !g_trace) return 0;
    memset(result, 0, sizeof result);
    __try {
        g_trace((uint8_t *)gm + GAME_TRACES, &handle, result, from, to, NULL, k_identity, TRACE_CONTENTS, ignore, 0,
                "script trace", SH_SCRIPT_SPAWN_ID_NONE);
        ok = 1;
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    if (!ok) return 0;
    memcpy(&out->fraction, result, sizeof(float));
    memcpy(out->end, result + TRACE_END, sizeof out->end);
    memcpy(out->normal, result + TRACE_NORMAL, sizeof out->normal);
    memcpy(&out->hit_spawn_id, result + TRACE_HIT_ID, sizeof out->hit_spawn_id);
    return out->fraction == out->fraction;
}

static int e_game_time_ms(int *ms)
{
    void *gm = sh_entity_gamemgr();
    time_fn f = (time_fn)vfn(gm, GAME_TIME_SLOT);
    int ok = 0;
    if (!f) return 0;
    __try { *ms = f(gm, 1); ok = 1; } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_ground(const float pos[3], float out[3])
{
    return sh_script_api_ground_trace(e_trace, pos, out);
}

/* The game's own difficulty (idGameLocal +0x670), only when the live vtable
 * holds the function the signature found. */
static int e_difficulty(int *level)
{
    void *gm = sh_entity_gamemgr();
    int ok = 0, v = 0;
    if (!gm || !g_difficulty || vfn(gm, GAME_DIFFICULTY_SLOT) != (void *)g_difficulty) return 0;
    __try { v = g_difficulty(gm); ok = 1; } __except (av_only(GetExceptionCode())) { ok = 0; }
    if (!ok || v < 0 || v > 4) return 0;
    *level = v;
    return 1;
}

static void *clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; r && i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (void *)r[i].addr;
    return NULL;
}

int sh_script_api_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    static const sh_script_engine engine = {
        e_resolve, e_spawn_id, e_slots, e_slot_spawn_id, e_kind, e_origin, e_axis, e_place, e_def_name, e_event,
        e_ai_health, e_ai_heal, e_ai_set_health, e_anim_node, e_spawn, e_trace, e_game_time_ms, e_ground,
        e_difficulty
    };
    sig_result own[API_SIG_COUNT + 1], lookup;
    int i;
    if (!module_base) return 0;
    g_base = module_base;
    for (i = 0; i < API_SIG_COUNT; i++) {
        if (sig_resolve_one(module_base, &SH_SCRIPT_API_SIGNATURES[i], &own[i]) != SIG_OK) {
            api_log("Mod scripts: the game function %s was not found in this executable, so the script calls on "
                    "entities and the world are unavailable.", SH_SCRIPT_API_SIGNATURES[i].name);
            return 0;
        }
    }
    if (sig_resolve_one(module_base, &SH_SCRIPT_SIGNATURES[SH_SCRIPT_SIG_SPAWN_ID_LOOKUP], &lookup) != SIG_OK) {
        api_log("Mod scripts: the game's entity lookup was not found in this executable, so the script calls on "
                "entities and the world are unavailable.");
        return 0;
    }
    g_vt_ai = SH_RVA(0x2035B30u, 0x204F900u) ? (uintptr_t)module_base + SH_RVA(0x2035B30u, 0x204F900u) : 0;
    g_vt_player = SH_RVA(0x22A1298u, 0x22BAFC8u) ? (uintptr_t)module_base + SH_RVA(0x22A1298u, 0x22BAFC8u) : 0;
    g_vt_health = SH_RVA(0x2035820u, 0x204F5F0u) ? (uintptr_t)module_base + SH_RVA(0x2035820u, 0x204F5F0u) : 0;
    if (!g_vt_ai || !g_vt_player || !g_vt_health) {
        api_log("Mod scripts: this executable is not the game build Snapmap+ supports, so the script calls on "
                "entities and the world are unavailable.");
        return 0;
    }
    g_process = (process_fn)own[API_SIG_PROCESS].addr;
    g_post = (post_fn)own[API_SIG_POST].addr;
    g_clear = (clear_fn)own[API_SIG_CLEAR].addr;
    g_arg_entity = (arg_entity_fn)own[API_SIG_ENTITY].addr;
    g_trace = (trace_fn)own[API_SIG_TRACE].addr;
    if (sig_resolve_one(module_base, &SH_SCRIPT_API_SIGNATURES[API_SIG_DIFFICULTY], &own[API_SIG_DIFFICULTY]) == SIG_OK)
        g_difficulty = (difficulty_fn)own[API_SIG_DIFFICULTY].addr;
    else
        api_log("Mod scripts: the game's difficulty was not found in this executable, so difficulty() answers nil.");
    g_lookup = (void *)lookup.addr;
    /* spawning: the idTarget_Spawn create and finish calls and the decl registry */
    g_decl_anchor = (const uint8_t *)clean(results, count, "DeclRegistryAnchor");
    g_decl_register = clean(results, count, "DeclRegisterFile");
    g_decl_type = (type_fn)clean(results, count, "DeclTypeByName");
    g_decl_find = (find_decl_fn)clean(results, count, "DeclFind");
    for (i = 0; SH_MODSCENE_SPAWN_SIGNATURES[i].name; i++) {
        const sig_entry *s = &SH_MODSCENE_SPAWN_SIGNATURES[i];
        sig_result r;
        if (!strcmp(s->name, "SceneCreateEntity") && sig_resolve_one(module_base, s, &r) == SIG_OK)
            g_create = (create_fn)r.addr;
        else if (!strcmp(s->name, "SceneFinishSpawn") && sig_resolve_one(module_base, s, &r) == SIG_OK)
            g_finish = (finish_fn)r.addr;
    }
    if (!g_create || !g_finish || !g_decl_anchor || !g_decl_register || !g_decl_type || !g_decl_find) {
        g_create = NULL;
        g_finish = NULL;
        api_log("Mod scripts: the game's spawn calls were not found in this executable, so spawn() answers nil.");
    }
    sh_script_api_set_engine(&engine);
    sh_script_api_register();
    backend_log("SCRIPTS: entity, animation, world and time calls resolved");
    return 1;
}

#endif /* SH_SCRIPT_PURE */
