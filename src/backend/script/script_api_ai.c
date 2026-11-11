/* script_api_ai.c -- the AI calls of mod scripts (script_api_ai.h).
 *
 * The Lua side checks every argument and the caller's ownership first, then
 * makes its engine calls through the seam (sh_script_ai_engine), and never
 * raises after an engine call. The engine side (below SH_SCRIPT_PURE) fills
 * the seam on build 13954591, both renderers.
 *
 * Engine paths (VK / GL, evidence in snaphak-re logs/shared-ghidra/SC5):
 *   commands      the catalogue events, run with ProcessEventArgs
 *                 (VK 0x140B47350 / GL 0x140B46940) on the demon;
 *   park          IsRunningScript (VK 0x14042FBC0 / GL 0x14042EE60) detoured;
 *   enemy         idEntityPtr at ai+0x25050, resolved by VK 0x1403935E0 /
 *                 GL 0x140392E10;
 *   awareness     GetAwareness(ai+0x29A88, entity) VK 0x1404A9E30 /
 *                 GL 0x1404A85B0, the AIAWARE_ word of the entity's record;
 *   sight         the isEntityVisible event (the perception's own sight
 *                 state, which the engine refreshes from its traces);
 *   move status   *(*(ai+0x23198)+0xD20), as Shared_MoveDone (VK
 *                 0x1403D7DF0) and Shared_MoveFailedAny (>= 5) read it;
 *   path          the move FSM's path query Shared_TargetDistanceFilter
 *                 calls: VK 0x140515300 / GL 0x140514030 (moveFSM, query,
 *                 goal, navTolerance, out reachable point, out length,
 *                 flag) -> 0 or 1 found, 2 still searching, above 2 none;
 *   trigger       the weapon's trigger word, as Shared_TriggerPulled reads
 *                 it (weapon +0x8F8 + 4 * [+0x8D4], 0 = pulled). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "script_api_ai.h"
#include "script_runtime.h"
#include "../../third_party/lua/lauxlib.h"

#define AI_PI 3.14159265358979323846

const sh_ai_event_def SH_AI_EVENTS[SH_AI_EV_COUNT] = {
    { "ssaction_MoveToPoint",        "vifbv" },
    { "ssaction_Idle",               "ii" },
    { "ssaction_SetWalkState",       "i" },
    { "ssaction_SetPlayerFocus",     "ibb" },
    { "ssaction_ChangeAnimState",    "i1i" },
    { "ssaction_ChangeAnimStateVia", "i11i" },
    { "ssaction_ForceAnimState",     "i1ii" },
    { "setFocus",                    "ebbi" },
    { "clearAimFocus",               "" },
    { "clearLookFocus",              "" },
    { "setEnemy",                    "eb" },
    { "setPlayerEnemy",              "i" },
    { "isEntityVisible",             "e" },
    { "hasAwarenessOfEnemy",         "" },
    { "pullTriggerRight",            "" },
    { "releaseTriggerRight",         "" },
    { "equipInventoryItemByIndex",   "i" },
    { "setLaserState",               "bs" },
    { "ae_startLaserSweep",          "4s" },
    { "ae_setOverrideFocusTagName",  "4is" },
    { "ae_clearOverrideFocusTag",    "4i" },
    { "setScriptAbort",              "bb" },
    { "setScriptAbortFlags",         "bb" },
    { "ae_setLaserState",            "4bs" },
};

/* ------------------------------------------------------------------ pure */

int sh_ai_args_fit(const char *format, const sh_ai_arg *args, int n)
{
    int i;
    if (!format || n != (int)strlen(format) || n > 8 || (n && !args)) return 0;
    for (i = 0; i < n; i++) {
        char f = format[i], t = args[i].type;
        if (f != t) return 0;
        if ((f == 's' || f == '1') && !args[i].s) return 0;
        if (f == 'e' && !args[i].ent) return 0;
        if (f == '4' && args[i].ent) return 0;
        if (f != 'v' && f != 'i' && f != 'f' && f != 'b' && f != 's' && f != '1' && f != 'e' && f != '4') return 0;
    }
    return 1;
}

int sh_ai_event_format_ok(int ev, const char *registry_format)
{
    if (ev < 0 || ev >= SH_AI_EV_COUNT || !registry_format) return 0;
    return strcmp(SH_AI_EVENTS[ev].format, registry_format) == 0;
}

void sh_ai_move_issue(sh_ai_move *m, uint32_t frame)
{
    m->state = SH_AI_MOVE_RUNNING;
    m->seen = 0;
    m->issued = frame;
}

int sh_ai_move_step(sh_ai_move *m, int status, uint32_t frame)
{
    if (m->state != SH_AI_MOVE_RUNNING) return m->state;
    if (status >= 5) { m->state = SH_AI_MOVE_FAILED; return m->state; }
    if (status > 0) { m->seen = 1; return m->state; }
    if (status == 0) {
        if (m->seen || frame - m->issued >= SH_AI_MOVE_SETTLE) m->state = SH_AI_MOVE_ARRIVED;
    }
    return m->state;
}

const char *sh_ai_awareness_name(int level)
{
    switch (level) {
    case SH_AI_AWARE_UNAWARE: return "unaware";
    case SH_AI_AWARE_LOST: return "lost";
    case SH_AI_AWARE_SUSPECTED: return "suspected";
    case SH_AI_AWARE_CONFIRMED: return "confirmed";
    default: return NULL;
    }
}

float sh_ai_angle_to(const float from[3], float yaw, const float to[3])
{
    double dx = to[0] - from[0], dy = to[1] - from[1], a;
    if (dx == 0 && dy == 0) return 0.0f;
    a = atan2(dy, dx) * 180.0 / AI_PI - yaw;
    while (a > 180.0) a -= 360.0;
    while (a <= -180.0) a += 360.0;
    return (float)a;
}

void sh_ai_ring(const float center[3], const float toward[3], float r, int count, float (*out)[3])
{
    double base = atan2(toward[1] - center[1], toward[0] - center[0]);
    int i;
    for (i = 0; i < count; i++) {
        double a = base + 2.0 * AI_PI * i / count;
        out[i][0] = (float)(center[0] + r * cos(a));
        out[i][1] = (float)(center[1] + r * sin(a));
        out[i][2] = center[2];
    }
}

float sh_ai_position_score(float dist_to_target, float ideal, float path_len, float path_weight)
{
    return fabsf(dist_to_target - ideal) + path_len * path_weight;
}

/* ------------------------------------------------------------- per demon */

#define AI_RECORDS_MAX 1024
#define AI_FIND_MAX    16
#define AI_FOLLOW_MIN  32.0f     /* move_to_entity re-aims when the target moved this far */

typedef struct ai_rec {
    sh_script_handle handle;
    int        aborts_sent;      /* setScriptAbort 0 0 and setScriptAbortFlags 0 0 posted */
    int        walk;             /* walk state last set, 0 none */
    sh_ai_move move;
    int        has_dest;
    float      dest[3];
    float      tolerance;
    int        facing;           /* a face order is standing: 1 an entity, 2 a position */
    sh_script_handle face_ent;
    float      face_pos[3];
} ai_rec;

#define AI_TURN_DONE_DEG 10.0f   /* turn_done: facing within this many degrees of the face target */

static ai_rec g_recs[AI_RECORDS_MAX];
static const sh_script_ai_engine *g_engine;

void sh_script_ai_set_engine(const sh_script_ai_engine *engine) { g_engine = engine; }

void sh_script_ai_reset(void) { memset(g_recs, 0, sizeof g_recs); }

/* The record of `h` (made when missing); a record whose handle no longer
 * belongs to this map is reused. NULL when the table is full. */
static ai_rec *rec_of(sh_script_handle h)
{
    int i, free_slot = -1;
    for (i = 0; i < AI_RECORDS_MAX; i++) {
        if (g_recs[i].handle == h) return &g_recs[i];
        if (free_slot < 0 && (!g_recs[i].handle || !sh_script_handle_known(g_recs[i].handle))) free_slot = i;
    }
    if (free_slot < 0) return NULL;
    memset(&g_recs[free_slot], 0, sizeof g_recs[free_slot]);
    g_recs[free_slot].handle = h;
    return &g_recs[free_slot];
}

static int rec_index(const ai_rec *r) { return (int)(r - g_recs); }

/* ------------------------------------------------------------- arguments */

static sh_ai_arg arg_i(int v) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = 'i'; a.i = v; return a; }
static sh_ai_arg arg_b(int v) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = 'b'; a.i = v ? 1 : 0; return a; }
static sh_ai_arg arg_f(float v) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = 'f'; a.f = v; return a; }
static sh_ai_arg arg_s(char t, const char *s) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = t; a.s = s; return a; }
static sh_ai_arg arg_e(void *e) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = 'e'; a.ent = e; return a; }
static sh_ai_arg arg_anim(void) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = '4'; return a; }
static sh_ai_arg arg_v(const float v[3])
{
    sh_ai_arg a;
    memset(&a, 0, sizeof a);
    a.type = 'v';
    a.v[0] = v[0]; a.v[1] = v[1]; a.v[2] = v[2];
    return a;
}

/* Run a catalogue event; 1 when the engine ran it. */
static int run(void *ent, int ev, const sh_ai_arg *args, int n, double *ret)
{
    if (!sh_ai_args_fit(SH_AI_EVENTS[ev].format, args, n)) return 0;
    return g_engine->event(ent, ev, args, n, ret);
}

/* A position at `idx`: a table {x=, y=, z=} or {x, y, z}. Raises a plain
 * error naming `call` otherwise. */
static void check_vec(lua_State *L, int idx, const char *call, float out[3])
{
    static const char *keys[3] = { "x", "y", "z" };
    int k;
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" needs a position (a table with x, y and z), got %s", call,
                         lua_isnoneornil(L, idx) ? "nothing" : luaL_typename(L, idx));
    for (k = 0; k < 3; k++) {
        int isnum = 0;
        lua_Number v;
        lua_getfield(L, idx, keys[k]);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, idx, k + 1); }
        v = lua_tonumberx(L, -1, &isnum);
        lua_pop(L, 1);
        if (!isnum || v != v || v > 1e7 || v < -1e7)
            sh_script_refuse(L, "\"%s\" needs a position with a number for %s", call, keys[k]);
        out[k] = (float)v;
    }
}

static void push_vec(lua_State *L, const float v[3])
{
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v[0]); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v[1]); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v[2]); lua_setfield(L, -2, "z");
}

static int is_entity(lua_State *L, int idx)
{
    return lua_type(L, idx) == LUA_TUSERDATA && luaL_testudata(L, idx, "entity") != NULL;
}

/* The demon a call acts on: argument 1 must be an entity of this mod that
 * is an idAI2. Returns NULL when the entity was removed (the call then
 * answers nil). Raises a plain error otherwise. */
static void *self_ai(lua_State *L, const char *call, sh_script_handle *h_out)
{
    uint64_t h = sh_script_check_entity(L, 1, call);
    void *ent;
    if (!g_engine)
        sh_script_refuse(L, "\"%s\" is not available in this session (Snapmap+ could not find the game's AI "
                         "functions; see the backend log)", call);
    if (!h) return NULL;
    if (!sh_script_owns_entity(L, h))
        sh_script_refuse(L, "\"%s\" works only on this mod's own demons", call);
    ent = g_engine->entity((sh_script_handle)h);
    if (!ent) return NULL;
    if (!g_engine->is_ai(ent))
        sh_script_refuse(L, "\"%s\" works only on a demon, and this entity is not one", call);
    if (h_out) *h_out = (sh_script_handle)h;
    return ent;
}

/* An entity argument at `idx` that may be any entity (a player, another
 * demon). NULL when it was removed. */
static void *other_entity(lua_State *L, int idx, const char *call)
{
    uint64_t h = sh_script_check_entity(L, idx, call);
    return h ? g_engine->entity((sh_script_handle)h) : NULL;
}

/* A target at `idx`: an entity (its origin) or a position. Returns 1 with
 * the point, 0 when the entity was removed. *ent_out gets the entity. */
static int target_point(lua_State *L, int idx, const char *call, float out[3], void **ent_out)
{
    if (ent_out) *ent_out = NULL;
    if (is_entity(L, idx)) {
        void *e = other_entity(L, idx, call);
        if (!e || !g_engine->origin(e, out)) return 0;
        if (ent_out) *ent_out = e;
        return 1;
    }
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" needs an entity or a position, got %s", call,
                         lua_isnoneornil(L, idx) ? "nothing" : luaL_typename(L, idx));
    check_vec(L, idx, call, out);
    return 1;
}

/* Check that `idx` holds an entity or a position, without the engine. */
static void check_target(lua_State *L, int idx, const char *call)
{
    float v[3];
    if (is_entity(L, idx)) { sh_script_check_entity(L, idx, call); return; }
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" needs an entity or a position, got %s", call,
                         lua_isnoneornil(L, idx) ? "nothing" : luaL_typename(L, idx));
    check_vec(L, idx, call, v);
}

static int opt_bool_field(lua_State *L, int idx, const char *key)
{
    int v;
    lua_getfield(L, idx, key);
    v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

static float opt_num_field(lua_State *L, int idx, const char *key, float dflt, const char *call, float lo, float hi)
{
    float v = dflt;
    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1)) {
        int isnum = 0;
        lua_Number n = lua_tonumberx(L, -1, &isnum);
        if (!isnum || n != n || n < lo || n > hi)
            sh_script_refuse(L, "\"%s\": %s must be a number from %g to %g", call, key, (double)lo, (double)hi);
        v = (float)n;
    }
    lua_pop(L, 1);
    return v;
}

static float opt_tolerance(lua_State *L, int idx, const char *call)
{
    lua_Number t;
    if (lua_isnoneornil(L, idx)) return 0.0f;
    if (lua_type(L, idx) != LUA_TNUMBER)
        sh_script_refuse(L, "\"%s\" needs a number of inches as its tolerance, got %s", call, luaL_typename(L, idx));
    t = lua_tonumber(L, idx);
    if (t != t || t < 0 || t > 4096)
        sh_script_refuse(L, "\"%s\" needs a tolerance from 0 to 4096 inches", call);
    return (float)t;
}

/* The flags table of move_to and move_to_entity. */
typedef struct move_flags { int run, aim; } move_flags;
static move_flags check_flags(lua_State *L, int idx, const char *call)
{
    move_flags f = { 0, 0 };
    if (lua_isnoneornil(L, idx)) return f;
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" takes its options as a table, as { run = true }, got %s", call, luaL_typename(L, idx));
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        if (!k || (strcmp(k, "run") && strcmp(k, "strafe") && strcmp(k, "aim_while_moving")))
            sh_script_refuse(L, "\"%s\" knows the options run, strafe and aim_while_moving, not \"%s\"", call,
                             k ? k : "?");
        lua_pop(L, 1);
    }
    f.run = opt_bool_field(L, idx, "run");
    f.aim = opt_bool_field(L, idx, "strafe") || opt_bool_field(L, idx, "aim_while_moving");
    return f;
}

/* ----------------------------------------------------------- the park */

/* Park the demon for script control: the engine side marks it, and the
 * abort flags are cleared once. Returns its record, or NULL when the
 * engine refused to park it (the call then does nothing). */
static ai_rec *park(void *ai, sh_script_handle h)
{
    ai_rec *r = rec_of(h);
    if (!r) return NULL;
    if (!g_engine->park(ai)) return NULL;
    if (!r->aborts_sent) {
        sh_ai_arg a[2];
        a[0] = arg_b(0); a[1] = arg_b(0);
        if (run(ai, SH_AI_EV_SET_SCRIPT_ABORT, a, 2, NULL) & run(ai, SH_AI_EV_SET_SCRIPT_ABORT_FLAGS, a, 2, NULL))
            r->aborts_sent = 1;
    }
    return r;
}

static void set_walk(void *ai, ai_rec *r, int state)
{
    sh_ai_arg a = arg_i(state);
    if (r->walk == state) return;
    if (run(ai, SH_AI_EV_SET_WALK_STATE, &a, 1, NULL)) r->walk = state;
}

/* Aim at the current enemy while moving (2016 locomotion strafes when the
 * aim focus is off the travel direction). */
static void aim_enemy(void *ai)
{
    void *enemy = g_engine->enemy(ai);
    sh_ai_arg a[4];
    if (!enemy) return;
    a[0] = arg_e(enemy); a[1] = arg_b(1); a[2] = arg_b(1); a[3] = arg_i(SH_AI_AIMPOINT_CENTER);
    run(ai, SH_AI_EV_SET_FOCUS, a, 4, NULL);
}

static int issue_move(void *ai, ai_rec *r, const float dest[3], float tol, move_flags f)
{
    sh_ai_arg a[5];
    float zero[3] = { 0, 0, 0 };
    set_walk(ai, r, f.run ? SH_AI_WALKSTATE_RUNNING : SH_AI_WALKSTATE_WALKING);
    if (f.aim) aim_enemy(ai);
    a[0] = arg_v(dest); a[1] = arg_i(SH_AI_ARRIVAL_STOP); a[2] = arg_f(tol > 0 ? tol : 0.1f);
    a[3] = arg_b(0); a[4] = arg_v(zero);
    if (!run(ai, SH_AI_EV_MOVE_TO_POINT, a, 5, NULL)) return 0;
    sh_ai_move_issue(&r->move, g_engine->frame());
    r->has_dest = 1;
    memcpy(r->dest, dest, sizeof r->dest);
    r->tolerance = tol;
    return 1;
}

/* ---------------------------------------------------------- the calls */

/* ai:move_to(pos, [tolerance], [flags]) -> true when the order was given */
static int l_move_to(lua_State *L)
{
    sh_script_handle h = 0;
    float dest[3], tol;
    move_flags f;
    void *ai;
    ai_rec *r;
    check_vec(L, 2, "move_to", dest);
    tol = opt_tolerance(L, 3, "move_to");
    f = check_flags(L, 4, "move_to");
    ai = self_ai(L, "move_to", &h);
    if (!ai || !(r = park(ai, h))) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, issue_move(ai, r, dest, tol, f));
    return 1;
}

/* ai:move_to_entity(e, [tolerance], [flags]): a move to the entity's
 * position, given again whenever the entity has moved AI_FOLLOW_MIN inches
 * (or the tolerance, when larger) from where the last order pointed, or the
 * last move ended. The engine's own move-to-entity overshoots its target. */
static int l_move_to_entity(lua_State *L)
{
    sh_script_handle h = 0;
    float to[3], tol;
    move_flags f;
    void *ai, *target;
    ai_rec *r;
    double dx, dy, dz, gap;
    sh_script_check_entity(L, 2, "move_to_entity");
    tol = opt_tolerance(L, 3, "move_to_entity");
    f = check_flags(L, 4, "move_to_entity");
    ai = self_ai(L, "move_to_entity", &h);
    if (!ai) { lua_pushboolean(L, 0); return 1; }
    target = other_entity(L, 2, "move_to_entity");
    if (!target || !g_engine->origin(target, to) || !(r = park(ai, h))) { lua_pushboolean(L, 0); return 1; }
    if (r->has_dest && r->move.state == SH_AI_MOVE_RUNNING) {
        dx = to[0] - r->dest[0]; dy = to[1] - r->dest[1]; dz = to[2] - r->dest[2];
        gap = tol > AI_FOLLOW_MIN ? tol : AI_FOLLOW_MIN;
        if (dx * dx + dy * dy + dz * dz < gap * gap) { lua_pushboolean(L, 1); return 1; }
    }
    lua_pushboolean(L, issue_move(ai, r, to, tol, f));
    return 1;
}

/* ai:stop() */
static int l_stop(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai = self_ai(L, "stop", &h);
    ai_rec *r;
    sh_ai_arg a[2];
    if (!ai || !(r = park(ai, h))) return 0;
    a[0] = arg_i(0); a[1] = arg_i(SH_AI_IDLETURN_COMBAT);
    if (run(ai, SH_AI_EV_IDLE, a, 2, NULL)) {
        r->move.state = SH_AI_MOVE_NONE;
        r->has_dest = 0;
    }
    return 0;
}

/* ai:move_done() -> done, failed */
static int l_move_done(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai = self_ai(L, "move_done", &h);
    ai_rec *r;
    int st, status;
    if (!ai || !(r = rec_of(h))) { lua_pushnil(L); return 1; }
    if (r->move.state == SH_AI_MOVE_NONE) { lua_pushboolean(L, 1); lua_pushboolean(L, 0); return 2; }
    status = g_engine->move_status(ai);
    st = sh_ai_move_step(&r->move, status < 0 ? 0 : status, g_engine->frame());
    if (st == SH_AI_MOVE_RUNNING && r->tolerance > 0 && r->has_dest) {
        float at[3];
        if (g_engine->origin(ai, at)) {
            double dx = at[0] - r->dest[0], dy = at[1] - r->dest[1];
            if (dx * dx + dy * dy <= (double)r->tolerance * r->tolerance) st = r->move.state = SH_AI_MOVE_ARRIVED;
        }
    }
    lua_pushboolean(L, st != SH_AI_MOVE_RUNNING);
    lua_pushboolean(L, st == SH_AI_MOVE_FAILED);
    return 2;
}

/* ai:face(target): an entity (keep facing it) or a position (turn once). */
static int l_face(lua_State *L)
{
    sh_script_handle h = 0;
    float p[3];
    void *ai, *ent = NULL;
    ai_rec *r;
    int is_ent = is_entity(L, 2);
    if (!is_ent) check_vec(L, 2, "face", p);
    else sh_script_check_entity(L, 2, "face");
    ai = self_ai(L, "face", &h);
    if (!ai) return 0;
    if (is_ent && !(ent = other_entity(L, 2, "face"))) return 0;
    if (!(r = park(ai, h))) return 0;
    if (ent) {
        sh_ai_arg a[4];
        a[0] = arg_e(ent); a[1] = arg_b(0); a[2] = arg_b(1); a[3] = arg_i(SH_AI_AIMPOINT_CENTER);
        if (run(ai, SH_AI_EV_SET_FOCUS, a, 4, NULL)) {
            r->facing = 1;
            r->face_ent = g_engine->handle_of(ent);
        }
    } else {
        float at[3], dir[3];
        double len;
        sh_ai_arg a[5];
        if (!g_engine->origin(ai, at)) return 0;
        dir[0] = p[0] - at[0]; dir[1] = p[1] - at[1]; dir[2] = 0;
        len = sqrt((double)dir[0] * dir[0] + (double)dir[1] * dir[1]);
        if (len < 1e-3) return 0;
        dir[0] = (float)(dir[0] / len); dir[1] = (float)(dir[1] / len);
        a[0] = arg_v(at); a[1] = arg_i(SH_AI_ARRIVAL_STOP); a[2] = arg_f(0.1f); a[3] = arg_b(1); a[4] = arg_v(dir);
        if (run(ai, SH_AI_EV_MOVE_TO_POINT, a, 5, NULL)) {
            sh_ai_move_issue(&r->move, g_engine->frame());
            r->has_dest = 1;
            memcpy(r->dest, at, sizeof at);
            r->tolerance = 0;
            r->facing = 2;
            memcpy(r->face_pos, p, sizeof p);
        }
    }
    return 0;
}

/* ai:turn_done() -> the demon faces its last face() target within
 * AI_TURN_DONE_DEG degrees (true when no face order stands; nil while the
 * target entity is gone). Made of the facing and the target's position:
 * 2016's turn has no completion flag a script can read. */
static int l_turn_done(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai = self_ai(L, "turn_done", &h);
    ai_rec *r;
    float at[3], to[3], yaw;
    if (!ai || !(r = rec_of(h))) { lua_pushnil(L); return 1; }
    if (!r->facing) { lua_pushboolean(L, 1); return 1; }
    if (r->facing == 1) {
        void *t = g_engine->entity(r->face_ent);
        if (!t || !g_engine->origin(t, to)) { lua_pushnil(L); return 1; }
    } else {
        memcpy(to, r->face_pos, sizeof to);
    }
    if (!g_engine->origin(ai, at) || !g_engine->yaw(ai, &yaw)) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, fabsf(sh_ai_angle_to(at, yaw, to)) <= AI_TURN_DONE_DEG);
    return 1;
}

/* ai:aim_at(e) aims the weapon at an entity; ai:aim_at(nil) stops aiming. */
static int l_aim_at(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai, *ent = NULL;
    if (!lua_isnoneornil(L, 2)) sh_script_check_entity(L, 2, "aim_at");
    ai = self_ai(L, "aim_at", &h);
    if (!ai) return 0;
    if (!lua_isnoneornil(L, 2) && !(ent = other_entity(L, 2, "aim_at"))) return 0;
    if (!park(ai, h)) return 0;
    if (ent) {
        sh_ai_arg a[4];
        a[0] = arg_e(ent); a[1] = arg_b(1); a[2] = arg_b(1); a[3] = arg_i(SH_AI_AIMPOINT_CENTER);
        run(ai, SH_AI_EV_SET_FOCUS, a, 4, NULL);
    } else {
        run(ai, SH_AI_EV_CLEAR_AIM_FOCUS, NULL, 0, NULL);
    }
    return 0;
}

/* ai:set_enemy([e]): e, or with no argument the closest player. */
static int l_set_enemy(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai, *ent = NULL;
    sh_ai_arg a[2];
    if (!lua_isnoneornil(L, 2)) sh_script_check_entity(L, 2, "set_enemy");
    ai = self_ai(L, "set_enemy", &h);
    if (!ai) return 0;
    if (!lua_isnoneornil(L, 2) && !(ent = other_entity(L, 2, "set_enemy"))) return 0;
    if (!park(ai, h)) return 0;
    if (ent) {
        a[0] = arg_e(ent); a[1] = arg_b(1);
        run(ai, SH_AI_EV_SET_ENEMY, a, 2, NULL);
    } else {
        a[0] = arg_i(SH_AI_PLAYER_CLOSEST);
        run(ai, SH_AI_EV_SET_PLAYER_ENEMY, a, 1, NULL);
    }
    return 0;
}

/* ai:enemy() -> entity or nil */
static int l_enemy(lua_State *L)
{
    void *ai = self_ai(L, "enemy", NULL), *e;
    if (!ai || !(e = g_engine->enemy(ai))) { lua_pushnil(L); return 1; }
    sh_script_push_entity(L, g_engine->handle_of(e));
    return 1;
}

/* ai:can_see(e) -> the demon's perception sees e now */
static int l_can_see(lua_State *L)
{
    void *ai, *e;
    double v = 0;
    sh_script_check_entity(L, 2, "can_see");
    ai = self_ai(L, "can_see", NULL);
    if (!ai) { lua_pushnil(L); return 1; }
    e = other_entity(L, 2, "can_see");
    if (!e) { lua_pushboolean(L, 0); return 1; }
    {
        sh_ai_arg a = arg_e(e);
        if (!run(ai, SH_AI_EV_IS_ENTITY_VISIBLE, &a, 1, &v)) { lua_pushnil(L); return 1; }
    }
    lua_pushboolean(L, v != 0);
    return 1;
}

/* ai:angle_to(target) -> degrees from the demon's facing, -180..180, + to its left */
static int l_angle_to(lua_State *L)
{
    float at[3], to[3], yaw;
    void *ai;
    check_target(L, 2, "angle_to");
    ai = self_ai(L, "angle_to", NULL);
    if (!ai) { lua_pushnil(L); return 1; }
    if (!target_point(L, 2, "angle_to", to, NULL) || !g_engine->origin(ai, at) || !g_engine->yaw(ai, &yaw)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, sh_ai_angle_to(at, yaw, to));
    return 1;
}

/* ai:aware() -> the demon is aware of its enemy */
static int l_aware(lua_State *L)
{
    void *ai = self_ai(L, "aware", NULL);
    double v = 0;
    if (!ai || !run(ai, SH_AI_EV_HAS_AWARENESS_OF_ENEMY, NULL, 0, &v)) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, v != 0);
    return 1;
}

/* ai:awareness([e]) -> "unaware" | "lost" | "suspected" | "confirmed" of e
 * (its enemy when e is omitted); nil when there is no one to be aware of. */
static int l_awareness(lua_State *L)
{
    void *ai, *e;
    const char *name;
    if (!lua_isnoneornil(L, 2)) sh_script_check_entity(L, 2, "awareness");
    ai = self_ai(L, "awareness", NULL);
    if (!ai) { lua_pushnil(L); return 1; }
    e = lua_isnoneornil(L, 2) ? g_engine->enemy(ai) : other_entity(L, 2, "awareness");
    if (!e || !(name = sh_ai_awareness_name(g_engine->awareness(ai, e)))) { lua_pushnil(L); return 1; }
    lua_pushstring(L, name);
    return 1;
}

/* The path query for `goal` under key slot `n` of the demon's record. */
static int path_of(void *ai, sh_script_handle h, const float goal[3], int n, float *len)
{
    ai_rec *r = rec_of(h);
    int key;
    if (!r) return -1;
    key = 0x53500000 | (rec_index(r) << 5) | (n & 31);
    return g_engine->path(ai, goal, key, len);
}

/* ai:path_length(pos) -> inches along the navigation; nil, "searching"
 * while the engine is still finding the path (ask again next think); nil,
 * "no path" when it cannot get there. */
static int l_path_length(lua_State *L)
{
    sh_script_handle h = 0;
    float goal[3], len = 0;
    void *ai;
    int st;
    check_target(L, 2, "path_length");
    ai = self_ai(L, "path_length", &h);
    if (!ai) { lua_pushnil(L); return 1; }
    if (!target_point(L, 2, "path_length", goal, NULL)) { lua_pushnil(L); lua_pushstring(L, "no path"); return 2; }
    st = path_of(ai, h, goal, 0, &len);
    if (st == 0) { lua_pushnumber(L, len); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, st == 1 ? "searching" : st == 2 ? "no path" : "unavailable");
    return 2;
}

/* ai:can_reach(pos) -> true, false, or nil while the engine is searching */
static int l_can_reach(lua_State *L)
{
    sh_script_handle h = 0;
    float goal[3], len = 0;
    void *ai;
    int st;
    check_target(L, 2, "can_reach");
    ai = self_ai(L, "can_reach", &h);
    if (!ai) { lua_pushnil(L); return 1; }
    if (!target_point(L, 2, "can_reach", goal, NULL)) { lua_pushboolean(L, 0); return 1; }
    st = path_of(ai, h, goal, 0, &len);
    if (st == 1) lua_pushnil(L);
    else lua_pushboolean(L, st == 0);
    return 1;
}

/* ai:find_position(parms) -> pos, or nil, "searching" | "none".
 * Made of the navigation path query: `count` points on the circle of
 * radius `ideal` around the target (its enemy when parms.target is
 * omitted), each asked for a path; the reachable one with the lowest
 * score |distance to target - ideal| + path length * path_weight wins,
 * skipping those whose path is longer than max_path. parms: target, min,
 * max, ideal, count (2..16), path_weight, max_path. */
static int l_find_position(lua_State *L)
{
    sh_script_handle h = 0;
    float center[3], at[3], ring[AI_FIND_MAX][3], best[3];
    float mn, mx, ideal, weight, max_path, best_score = 0;
    int count, i, have = 0, searching = 0, has_target;
    void *ai, *target = NULL;
    if (!lua_isnoneornil(L, 2) && lua_type(L, 2) != LUA_TTABLE)
        sh_script_refuse(L, "\"find_position\" takes a table of settings, as { ideal = 400 }, got %s", luaL_typename(L, 2));
    if (lua_isnoneornil(L, 2)) { lua_settop(L, 1); lua_newtable(L); }
    mn = opt_num_field(L, 2, "min", 0, "find_position", 0, 65536);
    mx = opt_num_field(L, 2, "max", 2048, "find_position", 0, 65536);
    if (mx < mn) sh_script_refuse(L, "\"find_position\": max must not be less than min");
    ideal = opt_num_field(L, 2, "ideal", (mn + mx) / 2, "find_position", mn, mx);
    count = (int)opt_num_field(L, 2, "count", 8, "find_position", 2, AI_FIND_MAX);
    weight = opt_num_field(L, 2, "path_weight", 0.5f, "find_position", 0, 100);
    max_path = opt_num_field(L, 2, "max_path", 0, "find_position", 0, 1e6f);
    lua_getfield(L, 2, "target");
    has_target = !lua_isnil(L, -1);
    if (has_target && !is_entity(L, -1) && lua_type(L, -1) != LUA_TTABLE)
        sh_script_refuse(L, "\"find_position\": target must be an entity or a position");
    if (has_target && lua_type(L, -1) == LUA_TTABLE) check_vec(L, lua_gettop(L), "find_position", center);
    ai = self_ai(L, "find_position", &h);
    if (!ai) { lua_pushnil(L); return 1; }
    if (has_target && is_entity(L, -1)) {
        if (!target_point(L, lua_gettop(L), "find_position", center, &target)) { lua_pushnil(L); lua_pushstring(L, "none"); return 2; }
    } else if (!has_target) {
        target = g_engine->enemy(ai);
        if (!target || !g_engine->origin(target, center)) { lua_pushnil(L); lua_pushstring(L, "none"); return 2; }
    }
    if (!g_engine->origin(ai, at)) { lua_pushnil(L); return 1; }
    sh_ai_ring(center, at, ideal, count, ring);
    for (i = 0; i < count; i++) {
        float len = 0, d, score;
        int st = path_of(ai, h, ring[i], 1 + i, &len);
        if (st == 1) { searching = 1; continue; }
        if (st != 0 || (max_path > 0 && len > max_path)) continue;
        d = ideal;   /* every candidate is on the ideal circle */
        score = sh_ai_position_score(d, ideal, len, weight);
        if (!have || score < best_score) { have = 1; best_score = score; memcpy(best, ring[i], sizeof best); }
    }
    if (have) { push_vec(L, best); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, searching ? "searching" : "none");
    return 2;
}

/* ai:trigger(on) pulls (true) or releases (false) the weapon's trigger. */
static int l_trigger(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai;
    int on;
    luaL_checkany(L, 2);
    on = lua_toboolean(L, 2);
    ai = self_ai(L, "trigger", &h);
    if (!ai || !park(ai, h)) return 0;
    run(ai, on ? SH_AI_EV_PULL_TRIGGER : SH_AI_EV_RELEASE_TRIGGER, NULL, 0, NULL);
    return 0;
}

/* ai:trigger_pulled() -> the weapon's trigger is held (a burst running) */
static int l_trigger_pulled(lua_State *L)
{
    void *ai = self_ai(L, "trigger_pulled", NULL);
    int st;
    if (!ai) { lua_pushnil(L); return 1; }
    st = g_engine->trigger_state(ai);
    lua_pushboolean(L, st == 1);
    return 1;
}

/* ai:equip(index) equips item `index` (0 first) of the demon's inventory. */
static int l_equip(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai;
    lua_Integer idx;
    sh_ai_arg a;
    if (!lua_isinteger(L, 2) || (idx = lua_tointeger(L, 2)) < 0 || idx > 63)
        sh_script_refuse(L, "\"equip\" needs an inventory index from 0 to 63");
    ai = self_ai(L, "equip", &h);
    if (!ai || !park(ai, h)) return 0;
    a = arg_i((int)idx);
    run(ai, SH_AI_EV_EQUIP_ITEM, &a, 1, NULL);
    return 0;
}

static const char *check_tag(lua_State *L, int idx, const char *call, const char *what)
{
    size_t n = 0;
    const char *s = lua_type(L, idx) == LUA_TSTRING ? lua_tolstring(L, idx, &n) : NULL;
    size_t i;
    if (!s || !n || n > 63) sh_script_refuse(L, "\"%s\" needs %s (a short name)", call, what);
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '_' || s[i] == '/' || s[i] == '.'))
            sh_script_refuse(L, "\"%s\": \"%s\" is not a usable name (letters, digits, _ / and . only)", call, s);
    return s;
}

/* ai:laser(tag, on) asks the laser targeter's lasers at `tag` to show or hide. */
static int l_laser(lua_State *L)
{
    sh_script_handle h = 0;
    const char *tag = check_tag(L, 2, "laser", "a laser tag");
    int on;
    void *ai;
    sh_ai_arg a[2];
    luaL_checkany(L, 3);
    on = lua_toboolean(L, 3);
    ai = self_ai(L, "laser", &h);
    if (!ai || !park(ai, h)) return 0;
    a[0] = arg_b(on); a[1] = arg_s('s', tag);
    run(ai, SH_AI_EV_SET_LASER_STATE, a, 2, NULL);
    return 0;
}

/* ai:laser_state(tag, on): the frame-command form of the same request
 * (ae_setLaserState), as the attack clips send it. */
static int l_laser_state(lua_State *L)
{
    sh_script_handle h = 0;
    const char *tag = check_tag(L, 2, "laser_state", "a laser tag");
    int on;
    void *ai;
    sh_ai_arg a[3];
    luaL_checkany(L, 3);
    on = lua_toboolean(L, 3);
    ai = self_ai(L, "laser_state", &h);
    if (!ai || !park(ai, h)) return 0;
    a[0] = arg_anim(); a[1] = arg_b(on); a[2] = arg_s('s', tag);
    run(ai, SH_AI_EV_AE_SET_LASER_STATE, a, 3, NULL);
    return 0;
}

/* ai:laser_sweep(name) starts the named sweep of the demon's laser targeter decl. */
static int l_laser_sweep(lua_State *L)
{
    sh_script_handle h = 0;
    const char *name = check_tag(L, 2, "laser_sweep", "a sweep name");
    void *ai = self_ai(L, "laser_sweep", &h);
    sh_ai_arg a[2];
    if (!ai || !park(ai, h)) return 0;
    a[0] = arg_anim(); a[1] = arg_s('s', name);
    run(ai, SH_AI_EV_START_LASER_SWEEP, a, 2, NULL);
    return 0;
}

/* ai:focus_tag(tag) aims from the named tag; ai:focus_tag(nil) goes back to the default. */
static int l_focus_tag(lua_State *L)
{
    sh_script_handle h = 0;
    const char *tag = lua_isnoneornil(L, 2) ? NULL : check_tag(L, 2, "focus_tag", "a tag name");
    void *ai = self_ai(L, "focus_tag", &h);
    sh_ai_arg a[3];
    if (!ai || !park(ai, h)) return 0;
    a[0] = arg_anim(); a[1] = arg_i(SH_AI_FOCI_AIM);
    if (tag) { a[2] = arg_s('s', tag); run(ai, SH_AI_EV_SET_FOCUS_TAG, a, 3, NULL); }
    else run(ai, SH_AI_EV_CLEAR_FOCUS_TAG, a, 2, NULL);
    return 0;
}

int sh_script_ai_anim(void *ai, sh_script_handle h, const char *dest, const char *via, int force, int blend_ms)
{
    sh_ai_arg a[4];
    if (!g_engine || !ai || !dest || !park(ai, h)) return 0;
    a[0] = arg_i(SH_AI_ANIMWEB_BODY);
    a[1] = arg_s('1', dest);
    if (force) {
        a[2] = arg_i(blend_ms);
        a[3] = arg_i(SH_AI_ANIMWAIT_DONT_WAIT);
        return run(ai, SH_AI_EV_FORCE_ANIM_STATE, a, 4, NULL);
    }
    if (via) {
        a[2] = arg_s('1', via);
        a[3] = arg_i(SH_AI_ANIMWAIT_DONT_WAIT);
        return run(ai, SH_AI_EV_CHANGE_ANIM_STATE_VIA, a, 4, NULL);
    }
    a[2] = arg_i(SH_AI_ANIMWAIT_DONT_WAIT);
    return run(ai, SH_AI_EV_CHANGE_ANIM_STATE, a, 3, NULL);
}

static const struct { const char *name; lua_CFunction fn; } k_calls[] = {
    { "move_to", l_move_to },           { "move_to_entity", l_move_to_entity },
    { "stop", l_stop },                 { "move_done", l_move_done },
    { "face", l_face },                 { "aim_at", l_aim_at },
    { "set_enemy", l_set_enemy },       { "enemy", l_enemy },
    { "can_see", l_can_see },
    { "angle_to", l_angle_to },         { "aware", l_aware },
    { "awareness", l_awareness },       { "path_length", l_path_length },
    { "can_reach", l_can_reach },       { "find_position", l_find_position },
    { "trigger", l_trigger },           { "trigger_pulled", l_trigger_pulled },
    { "equip", l_equip },               { "laser", l_laser },
    { "laser_sweep", l_laser_sweep },   { "focus_tag", l_focus_tag },
    { "turn_done", l_turn_done },       { "laser_state", l_laser_state },
};

int sh_script_ai_register(void)
{
    static int done;
    int i, n = 0;
    if (done) return 0;
    done = 1;
    for (i = 0; i < (int)(sizeof k_calls / sizeof k_calls[0]); i++)
        n += sh_script_api_add(SH_SCRIPT_API_METHOD, k_calls[i].name, k_calls[i].fn);
    return n;
}

#ifndef SH_SCRIPT_PURE
/* ======================================================= the engine side */
#include <windows.h>
#include "../backend_log.h"
#include "../engine_build.h"
#include "../entity.h"
#include "../hook.h"
#include "../typeinfo.h"
#include "script_hooks.h"
#include "script_api_common.h"

/* idAI2's idTypeInfo (the address its GetType returns: VK vtable
 * 0x142035B30 slot 0 = 0x140391970, GL 0x14204F900 slot 0 = 0x140391240). */
#define AI_IDAI2_TYPEINFO_RVA     SH_RVA(0x5CF7A90u, 0x45FD110u)

typedef void *(*ai_vfn0)(void *self);
typedef void *(*ai_vfn_int)(void *self, int i);
typedef uint64_t (*ai_irs_fn)(void *ai);
typedef int (*ai_awareness_fn)(void *perception, void *entity);
typedef void *(*ai_ptr_get_fn)(void *entity_ptr);
typedef void *(*ai_path_key_fn)(void *ai, int index, const char *name);
typedef int (*ai_path_query_fn)(void *move_fsm, void *query, const float *goal, float nav_tolerance,
                                float *out_reachable, float *out_length, uint8_t flag);

static uint8_t          *g_base;
static ai_irs_fn         g_irs_target;
static ai_irs_fn         g_irs_orig;
static ai_awareness_fn   g_get_awareness;
static ai_ptr_get_fn     g_ptr_get;
static ai_path_key_fn    g_path_key;
static ai_path_query_fn  g_path_query;
static const uint64_t   *g_path_global;
static const uint8_t    *g_ai2_type;
static int               g_detour_failed;

static int av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static void ai_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void *vfn(void *self, size_t slot)
{
    __try {
        const uint8_t *vt;
        if (!self) return NULL;
        vt = *(const uint8_t * const *)self;
        return vt ? *(void * const *)(vt + slot) : NULL;
    } __except (av_only(GetExceptionCode())) { return NULL; }
}

/* A catalogue event through the shared event helper (script_api_common.h),
 * which finds it by name and checks its arguments against the game's. */
static int e_event(void *ent, int ev, const sh_ai_arg *args, int n, double *ret)
{
    sh_script_earg a[SH_SCRIPT_EVENT_ARGS], r;
    int i;
    if (ev < 0 || ev >= SH_AI_EV_COUNT || n > SH_SCRIPT_EVENT_ARGS || !ent) return 0;
    for (i = 0; i < n; i++) {
        const sh_ai_arg *s = &args[i];
        switch (s->type) {
        case 'i': case 'b': a[i] = sh_script_earg_int(s->i); break;   /* the engine copies a bool as 'i' */
        case 'f': a[i] = sh_script_earg_float(s->f); break;
        case 'v': a[i] = sh_script_earg_vec(s->v); break;
        case 's': case '1': a[i] = sh_script_earg_text(s->type, s->s); break;
        case '4': a[i] = sh_script_earg_no_anim(); break;
        case 'e': a[i] = sh_script_earg_entity(s->ent); break;
        default: return 0;
        }
    }
    if (!sh_script_api_event(ent, SH_AI_EVENTS[ev].name, a, n, ret ? &r : NULL)) return 0;
    if (ret) {
        if (r.type == 'f') *ret = r.f;
        else if (r.type == 'b') *ret = (r.i & 0xff) != 0;
        else *ret = r.i;
    }
    return 1;
}

static void *e_entity(sh_script_handle h) { return sh_script_handle_entity(h); }

static uint32_t spawn_id_of(void *entity)
{
    uint8_t *gm = (uint8_t *)sh_entity_gamemgr();
    uint32_t id = SH_SCRIPT_SPAWN_ID_NONE;
    if (!gm || !entity) return id;
    __try {
        int num = *(const int *)((const uint8_t *)entity + SH_SCRIPT_ENT_NUMBER);
        if (num >= 0 && num < SH_SCRIPT_ENT_NUMBER_WORLD)
            id = *(const uint32_t *)(gm + SH_SCRIPT_GAME_SPAWN_IDS + (size_t)num * 4);
    } __except (av_only(GetExceptionCode())) { id = SH_SCRIPT_SPAWN_ID_NONE; }
    return id;
}

static sh_script_handle e_handle_of(void *entity)
{
    uint32_t id = spawn_id_of(entity);
    sh_script_handle h;
    if (id == SH_SCRIPT_SPAWN_ID_NONE) return 0;
    h = sh_script_handles_find(id, NULL);
    return h ? h : sh_script_handles_ref(id);
}

static int e_is_ai(void *entity)
{
    ai_vfn0 get_type = (ai_vfn0)vfn(entity, 0);
    int ok = 0;
    if (!get_type || !g_ai2_type) return 0;
    __try {
        const uint8_t *t = (const uint8_t *)get_type(entity);
        int num = *(const int *)(t + SH_AI_TYPE_NUM);
        int lo = *(const int *)(g_ai2_type + SH_AI_TYPE_NUM), hi = *(const int *)(g_ai2_type + SH_AI_TYPE_NUM + 4);
        ok = lo <= hi && num >= lo && num <= hi;
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_origin(void *entity, float out[3])
{
    int ok = 0;
    __try {
        void *phys = *(void **)((uint8_t *)entity + SH_AI_OFF_PHYSICS);
        ai_vfn_int f = (ai_vfn_int)vfn(phys, SH_AI_VSLOT_ORIGIN);
        const float *o = f ? (const float *)f(phys, 0) : NULL;
        if (o) { out[0] = o[0]; out[1] = o[1]; out[2] = o[2]; ok = 1; }
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_yaw(void *entity, float *deg)
{
    int ok = 0;
    __try {
        void *phys = *(void **)((uint8_t *)entity + SH_AI_OFF_PHYSICS);
        ai_vfn_int f = (ai_vfn_int)vfn(phys, SH_AI_VSLOT_AXIS);
        const float *m = f ? (const float *)f(phys, 0) : NULL;   /* idMat3: row 0 is forward */
        if (m) { *deg = (float)(atan2(m[1], m[0]) * 180.0 / AI_PI); ok = 1; }
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static void *e_enemy(void *ai)
{
    void *e = NULL;
    if (!g_ptr_get) return NULL;
    __try { e = g_ptr_get((uint8_t *)ai + SH_AI_OFF_ENEMY); }
    __except (av_only(GetExceptionCode())) { e = NULL; }
    return e;
}

static int e_awareness(void *ai, void *target)
{
    int v = -1;
    if (!g_get_awareness || !target) return -1;
    __try { v = g_get_awareness((uint8_t *)ai + SH_AI_OFF_PERCEPTION, target); }
    __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_move_status(void *ai)
{
    int v = -1;
    __try {
        const uint8_t *fsm = *(const uint8_t * const *)((const uint8_t *)ai + SH_AI_OFF_MOVE_FSM);
        if (fsm) v = *(const int *)(fsm + SH_AI_OFF_MOVE_STATUS);
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_trigger_state(void *ai)
{
    int v = -1;
    __try {
        const uint8_t *holder = *(const uint8_t * const *)((const uint8_t *)ai + SH_AI_OFF_WEAPON_HOLDER);
        const uint8_t *w = holder ? *(const uint8_t * const *)(holder + 8) : NULL;
        if (!w) {
            ai_vfn0 f = (ai_vfn0)vfn(ai, SH_AI_VSLOT_WEAPON);
            w = f ? (const uint8_t *)f(ai) : NULL;
        }
        if (w) {
            int slot = *(const int *)(w + SH_AI_OFF_WEAPON_SLOT);
            if (slot >= 0 && slot < 4) v = *(const int *)(w + SH_AI_OFF_WEAPON_TRIGGER + 4 * slot) == 0;
        }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_path(void *ai, const float goal[3], int key, float *len)
{
    struct { uint64_t global; uint64_t name; int key; int pad; } q;
    float reach[3] = { 0, 0, 0 }, length = 0;
    int st = -1;
    if (!g_path_query || !g_path_key || !g_path_global) return -1;
    __try {
        void *fsm = *(void **)((uint8_t *)ai + SH_AI_OFF_MOVE_FSM);
        const char *name = *(const char * const *)((uint8_t *)ai + SH_AI_OFF_PATH_NAME);
        const uint64_t *slot = (const uint64_t *)g_path_key(ai, 0, name);
        if (fsm && slot) {
            q.global = *g_path_global;
            q.name = *slot;
            q.key = key;
            q.pad = 0;
            st = g_path_query(fsm, &q, goal, 0.0f, reach, &length, 0);
        }
    } __except (av_only(GetExceptionCode())) { st = -1; }
    if (st == 0 || st == 1) { *len = length; return 0; }
    if (st == 2) return 1;
    return st < 0 ? -1 : 2;
}

static uint32_t e_frame(void) { return sh_script_hooks_frame(); }

/* ---------------------------------------------------------------- park */

#define AI_PARKED_MAX 512
typedef struct { void *volatile ai; volatile LONG spawn_id; } ai_parked;
static ai_parked g_parked[AI_PARKED_MAX];

/* IsRunningScript(ai): true for a parked demon whose spawn id still matches
 * (the same entity, not a new one in its memory), else the engine's answer.
 * FSM worker threads; reads only. */
static uint64_t irs_detour(void *ai)
{
    int i;
    for (i = 0; i < AI_PARKED_MAX; i++) {
        if (g_parked[i].ai == ai && ai) {
            uint32_t id = spawn_id_of(ai);
            if (id != SH_SCRIPT_SPAWN_ID_NONE && id == (uint32_t)g_parked[i].spawn_id) return 1;
        }
    }
    return g_irs_orig(ai);
}

static int commit_detour(void)
{
    void *tramp;
    if (g_irs_orig && hook_is_installed((void *)g_irs_orig)) return 1;
    if (g_detour_failed || !g_irs_target) return 0;
    if (memcmp((const void *)g_irs_target, k_ai_irs_prologue, SH_AI_IRS_STOLEN) != 0) {
        g_detour_failed = 1;
        ai_log("Mod scripts cannot drive demons in this session: the game's script-state function is not the "
               "expected one (another tool may have changed it).");
        return 0;
    }
    tramp = hook_prepare((void *)g_irs_target, (void *)irs_detour, SH_AI_IRS_STOLEN);
    if (!tramp) {
        g_detour_failed = 1;
        ai_log("Mod scripts cannot drive demons in this session: the hook could not be prepared (%s).",
               hook_last_failure());
        return 0;
    }
    g_irs_orig = (ai_irs_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_irs_orig = NULL;
        g_detour_failed = 1;
        ai_log("Mod scripts cannot drive demons in this session: the hook could not be installed.");
        return 0;
    }
    ai_log("SCRIPTS: demon park hook installed at %p", (void *)g_irs_target);
    return 1;
}

static int e_park(void *ai)
{
    uint32_t id = spawn_id_of(ai);
    int i, free_slot = -1;
    if (id == SH_SCRIPT_SPAWN_ID_NONE || !commit_detour()) return 0;
    for (i = 0; i < AI_PARKED_MAX; i++) {
        void *p = g_parked[i].ai;
        if (p == ai && (uint32_t)g_parked[i].spawn_id == id) return 1;
        if (free_slot < 0 && (!p || spawn_id_of(p) != (uint32_t)g_parked[i].spawn_id)) free_slot = i;
    }
    if (free_slot < 0) {
        static int logged;
        if (!logged) { logged = 1; ai_log("Mod scripts: more than %d demons driven at once; later ones keep their own behaviour.", AI_PARKED_MAX); }
        return 0;
    }
    g_parked[free_slot].ai = NULL;
    InterlockedExchange(&g_parked[free_slot].spawn_id, (LONG)id);
    InterlockedExchangePointer((PVOID volatile *)&g_parked[free_slot].ai, ai);
    return 1;
}

static const sh_script_ai_engine k_engine = {
    e_entity, e_handle_of, e_is_ai, e_origin, e_yaw, e_event, e_park, e_enemy, e_awareness,
    e_move_status, e_trigger_state, e_path, e_frame
};

static int32_t rel32_at(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

int sh_script_ai_install(const uint8_t *module_base)
{
    sig_result r[SH_AI_SIG_COUNT];
    const uint8_t *site;
    int i;
    sh_script_ai_register();
    if (!module_base) return 0;
    g_base = (uint8_t *)module_base;
    for (i = 0; i < SH_AI_SIG_COUNT; i++) {
        if (sig_resolve_one(module_base, &SH_AI_SIGNATURES[i], &r[i]) != SIG_OK) {
            ai_log("Mod scripts cannot drive demons in this session: the game function they need (%s) was not "
                   "found in this executable.", SH_AI_SIGNATURES[i].name);
            return 0;
        }
    }
    g_irs_target = (ai_irs_fn)r[SH_AI_SIG_IS_RUNNING_SCRIPT].addr;
    g_get_awareness = (ai_awareness_fn)r[SH_AI_SIG_GET_AWARENESS].addr;
    g_ptr_get = (ai_ptr_get_fn)r[SH_AI_SIG_ENTITY_PTR_GET].addr;
    site = (const uint8_t *)r[SH_AI_SIG_PATH_SITE].addr;
    g_path_key = (ai_path_key_fn)(site + SH_AI_PATH_SITE_KEY_CALL + 5 + rel32_at(site + SH_AI_PATH_SITE_KEY_CALL + 1));
    g_path_global = (const uint64_t *)(site + SH_AI_PATH_SITE_GLOBAL_LOAD + 7 +
                                       rel32_at(site + SH_AI_PATH_SITE_GLOBAL_LOAD + 3));
    g_path_query = (ai_path_query_fn)(site + SH_AI_PATH_SITE_QUERY_CALL + 5 +
                                      rel32_at(site + SH_AI_PATH_SITE_QUERY_CALL + 1));
    g_ai2_type = AI_IDAI2_TYPEINFO_RVA ? module_base + AI_IDAI2_TYPEINFO_RVA : NULL;
    if (!g_ai2_type || !sh_script_api_engine()) {
        ai_log("Mod scripts cannot drive demons in this session: this is not the game build Snapmap+ supports.");
        return 0;
    }
    if (memcmp((const void *)g_irs_target, k_ai_irs_prologue, SH_AI_IRS_STOLEN) != 0) {
        ai_log("Mod scripts cannot drive demons in this session: the game's script-state function is not the "
               "expected one (another tool may have changed it).");
        return 0;
    }
    sh_script_ai_set_engine(&k_engine);
    backend_log("SCRIPTS: AI calls resolved; the demon park hook is installed at the first command");
    return 1;
}

#endif /* SH_SCRIPT_PURE */
