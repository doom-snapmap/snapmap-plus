/* script_api_combat.c -- see script_api_combat.h. The pure part (the calls,
 * their argument checks and ownership, the hooks' records) builds alone with
 * SH_SCRIPT_PURE for tests/script_api_combat_test.c; the engine part fills the
 * combat engine on the backend. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script_api_combat.h"
#include "script_api_common.h"
#include "script_hooks.h"

#define PI               3.14159265358979323846
#define HITSCAN_RANGE    8192.0
#define RANGE_MAX        65536.0
#define SPEED_MAX        100000.0
#define JOINT_CAP        64
#define SIGHT_RAISE      40.0f     /* can_see_from aims this far above the other entity's origin */
#define REG_EFFECTS      "snapmap-plus.effects"

/* ------------------------------------------------------------ the engine */

static sh_script_combat_engine g_ce;
static int g_have_ce;

void sh_script_combat_set_engine(const sh_script_combat_engine *engine)
{
    if (engine) { g_ce = *engine; g_have_ce = 1; }
    else { memset(&g_ce, 0, sizeof g_ce); g_have_ce = 0; }
}

#define CE(fn) (g_have_ce && g_ce.fn)

/* -------------------------------------------------- records from the hooks */

#define LAUNCHES 256
#define HITS     256
#define DEATHS   512

typedef struct launch_rec { uint32_t spawn_id; int mod; sh_script_handle owner; uint32_t seq; } launch_rec;
typedef struct hit_rec { sh_script_handle victim; sh_script_handle attacker; char decl[SH_SCRIPT_EVENT_TEXT]; } hit_rec;

static SRWLOCK g_rec_lock = SRWLOCK_INIT;
static launch_rec g_launch[LAUNCHES];
static uint32_t g_launch_seq;
static hit_rec g_hits[HITS];
static unsigned g_hit_next;
static sh_script_handle g_dead[DEATHS];
static unsigned g_dead_next;

void sh_script_combat_reset(void)
{
    AcquireSRWLockExclusive(&g_rec_lock);
    memset(g_launch, 0, sizeof g_launch);
    memset(g_hits, 0, sizeof g_hits);
    memset(g_dead, 0, sizeof g_dead);
    g_launch_seq = 0;
    g_hit_next = 0;
    g_dead_next = 0;
    ReleaseSRWLockExclusive(&g_rec_lock);
}

void sh_script_combat_note_launch(uint32_t spawn_id, sh_script_handle owner, int mod)
{
    launch_rec *r;
    if (spawn_id == SH_SCRIPT_SPAWN_ID_NONE || mod < 0) return;
    AcquireSRWLockExclusive(&g_rec_lock);
    g_launch_seq++;
    r = &g_launch[g_launch_seq % LAUNCHES];
    r->spawn_id = spawn_id;
    r->mod = mod;
    r->owner = owner;
    r->seq = g_launch_seq;
    ReleaseSRWLockExclusive(&g_rec_lock);
}

int sh_script_combat_launched(uint32_t spawn_id, int *mod, sh_script_handle *owner)
{
    int i, found = 0;
    uint32_t best = 0;
    if (spawn_id == SH_SCRIPT_SPAWN_ID_NONE) return 0;
    AcquireSRWLockShared(&g_rec_lock);
    for (i = 0; i < LAUNCHES; i++)
        if (g_launch[i].seq && g_launch[i].spawn_id == spawn_id && g_launch[i].seq > best) {
            best = g_launch[i].seq;
            if (mod) *mod = g_launch[i].mod;
            if (owner) *owner = g_launch[i].owner;
            found = 1;
        }
    ReleaseSRWLockShared(&g_rec_lock);
    return found;
}

/* The newest launch by `owner` after sequence `after`: its spawn id, or NONE. */
static uint32_t launch_after(sh_script_handle owner, uint32_t after)
{
    int i;
    uint32_t best = after, id = SH_SCRIPT_SPAWN_ID_NONE;
    AcquireSRWLockShared(&g_rec_lock);
    for (i = 0; i < LAUNCHES; i++)
        if (g_launch[i].seq > best && g_launch[i].owner == owner) { best = g_launch[i].seq; id = g_launch[i].spawn_id; }
    ReleaseSRWLockShared(&g_rec_lock);
    return id;
}

static uint32_t launch_seq(void)
{
    uint32_t s;
    AcquireSRWLockShared(&g_rec_lock);
    s = g_launch_seq;
    ReleaseSRWLockShared(&g_rec_lock);
    return s;
}

void sh_script_combat_note_hit(sh_script_handle victim, const char *decl, sh_script_handle attacker)
{
    hit_rec *r = NULL;
    int i;
    if (!victim) return;
    AcquireSRWLockExclusive(&g_rec_lock);
    for (i = 0; i < HITS && !r; i++) if (g_hits[i].victim == victim) r = &g_hits[i];
    if (!r) r = &g_hits[g_hit_next++ % HITS];
    r->victim = victim;
    r->attacker = attacker;
    strncpy_s(r->decl, sizeof r->decl, decl ? decl : "", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_rec_lock);
}

int sh_script_combat_last_hit(sh_script_handle victim, char *decl, size_t cap, sh_script_handle *attacker)
{
    int i, found = 0;
    if (decl && cap) decl[0] = 0;
    if (attacker) *attacker = 0;
    if (!victim) return 0;
    AcquireSRWLockShared(&g_rec_lock);
    for (i = 0; i < HITS; i++) if (g_hits[i].victim == victim) {
        if (decl && cap) strncpy_s(decl, cap, g_hits[i].decl, _TRUNCATE);
        if (attacker) *attacker = g_hits[i].attacker;
        found = 1;
        break;
    }
    ReleaseSRWLockShared(&g_rec_lock);
    return found;
}

int sh_script_combat_first_death(sh_script_handle victim)
{
    int i, first = 1;
    if (!victim) return 0;
    AcquireSRWLockExclusive(&g_rec_lock);
    for (i = 0; i < DEATHS; i++) if (g_dead[i] == victim) { first = 0; break; }
    if (first) g_dead[g_dead_next++ % DEATHS] = victim;
    ReleaseSRWLockExclusive(&g_rec_lock);
    return first;
}

/* ------------------------------------------------------------ helpers */

static int nil1(lua_State *L)
{
    lua_pushnil(L);
    return 1;
}

static int bool1(lua_State *L, int b)
{
    lua_pushboolean(L, b);
    return 1;
}

static const sh_script_engine *eng(void)
{
    return sh_script_api_engine();
}

/* A decl of this mod by its mod-relative name. NULL when the combat engine is
 * not available (the call answers nil); a refusal when the mod has no such
 * decl. */
static void *check_decl(lua_State *L, int arg, const char *call, const char *type, const char *what)
{
    char full[SH_SCRIPT_DEFNAME_CAP];
    void *decl;
    if (lua_type(L, arg) != LUA_TSTRING && !lua_isuserdata(L, arg))
        sh_script_refuse(L, "\"%s\" needs %s (the name of one of this mod's %s decls), got %s", call, what, type,
                         lua_isnoneornil(L, arg) ? "nothing" : luaL_typename(L, arg));
    sh_script_check_name(L, arg, call, 0, full, sizeof full, NULL);
    if (!CE(decl)) return NULL;
    decl = g_ce.decl(type, full);
    if (!decl)
        sh_script_refuse(L, "\"%s\" found no %s decl \"%s\" in this mod; check the name and that the file is under "
                            "decls\\%s", call, type, lua_tostring(L, arg), type);
    return decl;
}

/* The entity that deals the damage: one of the mod's own, or the world when
 * none is given. NULL when it cannot be had (removed, or no world). */
static void *opt_attacker(lua_State *L, int arg, const char *call)
{
    if (lua_isnoneornil(L, arg)) return CE(world) ? g_ce.world() : NULL;
    if (!luaL_testudata(L, arg, "entity"))
        sh_script_refuse(L, "\"%s\" needs the attacker as one of this mod's entities, got %s", call,
                         lua_type(L, arg) == LUA_TSTRING ? "text" : luaL_typename(L, arg));
    return sh_script_api_owned(L, arg, call, "attack with", NULL);
}

static void origin_of(void *ent, float out[3])
{
    const sh_script_engine *e = eng();
    out[0] = out[1] = out[2] = 0;
    if (ent && e && e->origin) e->origin(ent, out);
}

static float vlen(const float v[3])
{
    return (float)sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
}

static int normalize(float v[3])
{
    float n = vlen(v);
    if (!(n > 1e-6f)) return 0;
    v[0] /= n; v[1] /= n; v[2] /= n;
    return 1;
}

/* Direction to angles (pitch, yaw, roll in degrees; pitch down is positive). */
static void dir_angles(const float d[3], float out[3])
{
    double h = sqrt((double)d[0] * d[0] + (double)d[1] * d[1]);
    out[0] = (float)(-atan2((double)d[2], h) * 180.0 / PI) + 0.0f;   /* + 0: no negative zero */
    out[1] = (float)(atan2((double)d[1], (double)d[0]) * 180.0 / PI) + 0.0f;
    out[2] = 0;
}

/* Angles (pitch, yaw, roll) to the forward direction. */
static void angles_forward(const float a[3], float out[3])
{
    double p = a[0] * PI / 180.0, y = a[1] * PI / 180.0;
    out[0] = (float)(cos(p) * cos(y));
    out[1] = (float)(cos(p) * sin(y));
    out[2] = (float)(-sin(p));
}

/* Forward, left, up from a direction (an effect turned to face it). */
static void dir_axis(const float f[3], float ax[9])
{
    float up[3] = { 0, 0, 1 }, l[3];
    memcpy(ax, f, 12);
    if (fabsf(f[2]) > 0.999f) { up[0] = 1; up[2] = 0; }
    l[0] = up[1] * f[2] - up[2] * f[1];
    l[1] = up[2] * f[0] - up[0] * f[2];
    l[2] = up[0] * f[1] - up[1] * f[0];
    normalize(l);
    memcpy(ax + 3, l, 12);
    ax[6] = f[1] * l[2] - f[2] * l[1];
    ax[7] = f[2] * l[0] - f[0] * l[2];
    ax[8] = f[0] * l[1] - f[1] * l[0];
}

/* The entity's own `damage` event: decl, from direction, no radius, full
 * multiplier, the attacker as activator. 1 when the engine ran it. */
static int damage_event(void *victim, void *decl, const float dir[3], void *attacker)
{
    sh_script_earg a[6];
    float ang[3];
    dir_angles(dir, ang);
    memset(a, 0, sizeof a);
    a[0].type = 'd'; a[0].p = decl;
    a[1].type = 'a'; memcpy(a[1].v, ang, 12);
    a[2] = sh_script_earg_float(0.0f);
    a[3] = sh_script_earg_float(1.0f);
    a[4] = sh_script_earg_entity(attacker);
    a[5] = sh_script_earg_int(0);
    return sh_script_api_event(victim, "damage", a, 6, NULL);
}

/* ------------------------------------------------------------ damage */

/* damage(e, damage, [attacker], [from]) and e:damage(damage, [attacker], [from]) */
static int do_damage(lua_State *L, int target, const char *call)
{
    float from[3], to[3], dir[3];
    int have_from;
    void *decl, *victim, *attacker;
    victim = sh_script_api_entity(L, target, call, NULL);
    decl = check_decl(L, target + 1, call, "damage", "the damage");
    attacker = opt_attacker(L, target + 2, call);
    have_from = !lua_isnoneornil(L, target + 3) && sh_script_api_check_point(L, target + 3, call, "where the damage comes from", from);
    if (!victim || !decl || !attacker) return bool1(L, 0);
    origin_of(victim, to);
    if (!have_from) {
        if (lua_isnoneornil(L, target + 2)) { from[0] = to[0]; from[1] = to[1]; from[2] = to[2] + 1; }
        else origin_of(attacker, from);
    }
    dir[0] = to[0] - from[0]; dir[1] = to[1] - from[1]; dir[2] = to[2] - from[2];
    if (!normalize(dir)) { dir[0] = 0; dir[1] = 0; dir[2] = -1; }
    return bool1(L, damage_event(victim, decl, dir, attacker));
}

/* The global and the method take the same values: damage(e, ...) and e:damage(...). */
static int l_damage(lua_State *L)
{
    return do_damage(L, 1, "damage");
}

/* hitscan(from, dir, damage, [range], [attacker]) */
static int l_hitscan(lua_State *L)
{
    const sh_script_engine *e = eng();
    float from[3], dir[3], to[3];
    double range = HITSCAN_RANGE;
    void *decl, *attacker, *hit = NULL;
    uint32_t ignore = SH_SCRIPT_SPAWN_ID_NONE;
    sh_script_trace t;
    int have = sh_script_api_check_point(L, 1, "hitscan", "where the shot starts", from);
    sh_script_api_check_vec(L, 2, "hitscan", "the direction", dir);
    if (!normalize(dir)) sh_script_refuse(L, "\"hitscan\" needs a direction that is not zero");
    decl = check_decl(L, 3, "hitscan", "damage", "the damage");
    if (sh_script_api_opt_number(L, 4, "hitscan", "the range", &range) && (range < 1 || range > RANGE_MAX))
        sh_script_refuse(L, "\"hitscan\" needs a range from 1 to %.0f, got %g", RANGE_MAX, range);
    attacker = opt_attacker(L, 5, "hitscan");
    if (!have || !decl || !attacker || !e || !e->trace) return nil1(L);
    if (!lua_isnoneornil(L, 5) && e->spawn_id) ignore = e->spawn_id(attacker);
    to[0] = from[0] + dir[0] * (float)range;
    to[1] = from[1] + dir[1] * (float)range;
    to[2] = from[2] + dir[2] * (float)range;
    if (!e->trace(from, to, ignore, &t) || !(t.fraction < 1.0f)) return nil1(L);
    if (t.hit_spawn_id != SH_SCRIPT_SPAWN_ID_NONE && t.hit_spawn_id != SH_SCRIPT_SPAWN_ID_WORLD && e->resolve)
        hit = e->resolve(t.hit_spawn_id);
    if (hit) damage_event(hit, decl, dir, attacker);
    lua_createtable(L, 0, 3);
    sh_script_api_push_vec(L, t.end); lua_setfield(L, -2, "pos");
    sh_script_api_push_vec(L, t.normal); lua_setfield(L, -2, "normal");
    if (hit) { sh_script_api_push_engine_entity(L, hit); lua_setfield(L, -2, "entity"); }
    return 1;
}

/* radius_damage(pos, damage, [attacker]) */
static int l_radius_damage(lua_State *L)
{
    float pos[3];
    int have = sh_script_api_check_point(L, 1, "radius_damage", "the center", pos);
    void *decl = check_decl(L, 2, "radius_damage", "damage", "the damage");
    void *attacker = opt_attacker(L, 3, "radius_damage");
    if (!have || !decl || !attacker || !CE(radius_damage)) return bool1(L, 0);
    return bool1(L, g_ce.radius_damage(pos, decl, attacker, attacker));
}

/* ------------------------------------------------------------ effects */

/* Call the API's own spawn (its caps and ownership) with name, pos, yaw.
 * Leaves the entity (or nil) on the stack. */
static void call_spawn(lua_State *L, int name_arg, const float pos[3], double yaw)
{
    lua_getglobal(L, "spawn");
    if (!lua_iscfunction(L, -1))
        sh_script_refuse(L, "the effect could not be placed because \"spawn\" was replaced by the script; do not "
                            "assign to spawn");
    lua_pushvalue(L, name_arg);
    sh_script_api_push_vec(L, pos);
    lua_pushnumber(L, yaw);
    lua_call(L, 3, 1);
}

static void effects_mark(lua_State *L, int ent_index)
{
    if (lua_getfield(L, LUA_REGISTRYINDEX, REG_EFFECTS) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, REG_EFFECTS);
    }
    lua_pushvalue(L, ent_index);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

static int effects_has(lua_State *L, int ent_index)
{
    int has;
    if (lua_getfield(L, LUA_REGISTRYINDEX, REG_EFFECTS) != LUA_TTABLE) { lua_pop(L, 1); return 0; }
    lua_pushvalue(L, ent_index);
    has = lua_rawget(L, -2) == LUA_TBOOLEAN;
    lua_pop(L, 2);
    return has;
}

static int place_effect(lua_State *L, int ent_index, const float pos[3], const float *dir)
{
    const sh_script_engine *e = eng();
    float ax[9], d[3];
    void *ent = sh_script_api_entity(L, ent_index, "fx", NULL);
    if (!ent || !e || !e->place) return 0;
    if (dir) {
        memcpy(d, dir, 12);
        if (!normalize(d)) return e->place(ent, pos, NULL);
        dir_axis(d, ax);
        return e->place(ent, pos, ax);
    }
    return e->place(ent, pos, NULL);
}

static int fx_condition(lua_State *L, int arg, const char *call)
{
    const char *name = sh_script_api_check_text(L, arg, call, "the effect condition (FX_... of the entity's fx decl)", 63);
    int v;
    if (!CE(fx_condition)) return -2;
    v = g_ce.fx_condition(name);
    if (v < 0)
        sh_script_refuse(L, "\"%s\" knows no effect condition \"%s\"; use one of the FX_ conditions the entity's fx "
                            "decl lists", call, name);
    return v;
}

/* fx(e, condition) or fx(effect, pos, [dir]) */
static int l_fx(lua_State *L)
{
    if (luaL_testudata(L, 1, "entity")) {
        sh_script_earg a[2];
        int cond;
        void *ent = sh_script_api_owned(L, 1, "fx", "start effects on", NULL);
        cond = fx_condition(L, 2, "fx");
        if (!ent || cond < 0) return bool1(L, 0);
        a[0] = sh_script_earg_no_anim();
        a[1] = sh_script_earg_int(cond);
        return bool1(L, sh_script_api_event(ent, "ae_startFX_Networked", a, 2, NULL));
    } else {
        float pos[3], dir[3];
        int have_dir;
        if (lua_type(L, 1) != LUA_TSTRING)
            sh_script_refuse(L, "\"fx\" needs an entity and an effect condition, or the name of one of this mod's "
                                "effect entities and a position; got %s", lua_isnoneornil(L, 1) ? "nothing" : luaL_typename(L, 1));
        sh_script_api_check_vec(L, 2, "fx", "where to show the effect", pos);
        have_dir = sh_script_api_opt_vec(L, 3, "fx", "the direction the effect faces", dir);
        lua_settop(L, 3);
        call_spawn(L, 1, pos, have_dir ? atan2((double)dir[1], (double)dir[0]) * 180.0 / PI : 0.0);
        if (lua_isnil(L, -1)) return 1;
        if (have_dir) place_effect(L, lua_gettop(L), pos, dir);
        effects_mark(L, lua_gettop(L));
        return 1;
    }
}

/* fx_move(e, pos, [dir]) */
static int l_fx_move(lua_State *L)
{
    float pos[3], dir[3];
    int have_dir;
    sh_script_api_owned(L, 1, "fx_move", "move", NULL);
    sh_script_api_check_vec(L, 2, "fx_move", "where to move the effect", pos);
    have_dir = sh_script_api_opt_vec(L, 3, "fx_move", "the direction the effect faces", dir);
    if (!effects_has(L, 1))
        sh_script_refuse(L, "\"fx_move\" moves an effect made by fx(effect, pos); this entity is not one");
    return bool1(L, place_effect(L, 1, pos, have_dir ? dir : NULL));
}

/* fx_stop(e, [condition]) */
static int l_fx_stop(lua_State *L)
{
    void *ent = sh_script_api_owned(L, 1, "fx_stop", "stop effects on", NULL);
    if (lua_isnoneornil(L, 2)) {
        if (!effects_has(L, 1))
            sh_script_refuse(L, "\"fx_stop\" without a condition removes an effect made by fx(effect, pos); for an "
                                "entity's own effect give the condition it was started with");
        if (!ent) return bool1(L, 0);
        return bool1(L, sh_script_api_post(ent, "remove", NULL, 0));
    } else {
        sh_script_earg a[2];
        int cond = fx_condition(L, 2, "fx_stop");
        if (!ent || cond < 0) return bool1(L, 0);
        a[0] = sh_script_earg_no_anim();
        a[1] = sh_script_earg_int(cond);
        return bool1(L, sh_script_api_event(ent, "ae_stopFX_Networked", a, 2, NULL));
    }
}

/* explode(pos, damage, [effect], [attacker]) */
static int l_explode(lua_State *L)
{
    float pos[3];
    int have = sh_script_api_check_point(L, 1, "explode", "where the explosion is", pos);
    void *decl = check_decl(L, 2, "explode", "damage", "the damage");
    void *attacker;
    if (!lua_isnoneornil(L, 3) && lua_type(L, 3) != LUA_TSTRING)
        sh_script_refuse(L, "\"explode\" needs the effect as the name of one of this mod's entities, got %s",
                         luaL_typename(L, 3));
    attacker = opt_attacker(L, 4, "explode");
    if (!have || !decl || !attacker || !CE(radius_damage)) return nil1(L);
    g_ce.radius_damage(pos, decl, attacker, attacker);
    if (lua_isnoneornil(L, 3)) return nil1(L);
    lua_settop(L, 4);
    call_spawn(L, 3, pos, 0.0);
    if (!lua_isnil(L, -1)) effects_mark(L, lua_gettop(L));
    return 1;
}

/* sound(e, sound, [channel]) */
static int l_sound(lua_State *L)
{
    sh_script_earg a[2];
    int channel = 0;
    void *ent = sh_script_api_owned(L, 1, "sound", "play sounds on", NULL);
    void *decl = check_decl(L, 2, "sound", "sound", "the sound");
    if (!lua_isnoneornil(L, 3)) {
        const char *c = sh_script_api_check_text(L, 3, "sound", "the channel", 32);
        channel = sh_script_combat_channel(c);
        if (channel < 0)
            sh_script_refuse(L, "\"sound\" knows no channel \"%s\"; use any, voice, body, weapon or item", c);
    }
    if (!ent || !decl) return bool1(L, 0);
    memset(a, 0, sizeof a);
    a[0].type = 'd'; a[0].p = decl;
    a[1] = sh_script_earg_int(channel);
    return bool1(L, sh_script_api_event(ent, "startSoundShader", a, 2, NULL));
}

/* ------------------------------------------------------------ joints, pain */

/* The joint `name` of `ent` in world space. 1 when the entity has it. */
static int joint_of(void *ent, const char *name, float pos[3], float fwd[3])
{
    sh_script_earg a, r;
    a = sh_script_earg_text('s', name);
    if (!sh_script_api_event(ent, "getJointHandle", &a, 1, &r) || r.type != 'i' || r.i < 0) return 0;
    a = sh_script_earg_int(r.i);
    if (!sh_script_api_event(ent, "getJointPos", &a, 1, &r) || r.type != 'v') return 0;
    memcpy(pos, r.v, 12);
    if (fwd) {
        if (!sh_script_api_event(ent, "getJointAngle", &a, 1, &r) || r.type != 'v') return 0;
        angles_forward(r.v, fwd);
    }
    return 1;
}

/* e:tag(joint) -> pos, forward */
static int l_tag(lua_State *L)
{
    float pos[3], fwd[3];
    void *ent = sh_script_api_entity(L, 1, "tag", NULL);
    const char *name = sh_script_api_check_text(L, 2, "tag", "the joint name", JOINT_CAP - 1);
    if (!ent || !joint_of(ent, name, pos, fwd)) return nil1(L);
    sh_script_api_push_vec(L, pos);
    sh_script_api_push_vec(L, fwd);
    return 2;
}

/* e:pain() -> the pain reaction playing, or nil */
static int l_pain(lua_State *L)
{
    void *ent = sh_script_api_entity(L, 1, "pain", NULL);
    int r;
    if (!ent || sh_script_api_kind(ent) != SH_SCRIPT_KIND_AI || !CE(pain_reaction)) return nil1(L);
    r = g_ce.pain_reaction(ent);
    if (r <= SH_COMBAT_PAIN_NONE || !sh_script_combat_pain_name(r)) return nil1(L);
    lua_pushstring(L, sh_script_combat_pain_name(r));
    return 1;
}

/* ai:can_see_from(other, joint) */
static int l_can_see_from(lua_State *L)
{
    const sh_script_engine *e = eng();
    float from[3], to[3];
    sh_script_handle h = 0;
    sh_script_trace t;
    void *self = sh_script_api_entity(L, 1, "can_see_from", NULL);
    void *other = sh_script_api_entity(L, 2, "can_see_from", &h);
    const char *joint = sh_script_api_check_text(L, 3, "can_see_from", "the joint to look from", JOINT_CAP - 1);
    if (!self || !other || !e || !e->trace || !e->spawn_id) return nil1(L);
    if (!joint_of(self, joint, from, NULL)) return nil1(L);
    origin_of(other, to);
    to[2] += SIGHT_RAISE;
    if (!e->trace(from, to, e->spawn_id(self), &t)) return nil1(L);
    return bool1(L, !(t.fraction < 1.0f) || t.hit_spawn_id == e->spawn_id(other));
}

/* ------------------------------------------------------------ projectiles */

/* The projectile at arg: one this mod's entities launched, or the mod's own. */
static void *own_projectile(lua_State *L, int arg, const char *call)
{
    sh_script_handle h = 0;
    void *p = sh_script_api_entity(L, arg, call, &h);
    int mod = -1;
    if (!h) return NULL;
    if (sh_script_combat_launched(sh_script_handle_spawn_id(h), &mod, NULL)) {
        if (mod != sh_script_hooks_mod_find(sh_script_salt(L)))
            sh_script_refuse(L, "\"%s\" can only steer projectiles this mod's entities launched; this one is not", call);
        return p;
    }
    sh_script_refuse(L, "\"%s\" works on a projectile this mod's entities launched; this entity is not one", call);
    return NULL;
}

static int steer_to(void *p, const float point[3], double speed)
{
    sh_script_earg r, a;
    float pos[3], d[3];
    if (speed <= 0) {
        if (!sh_script_api_event(p, "getLinearVelocity", NULL, 0, &r) || r.type != 'v') return 0;
        speed = vlen(r.v);
        if (!(speed > 0)) return 0;
    }
    origin_of(p, pos);
    d[0] = point[0] - pos[0]; d[1] = point[1] - pos[1]; d[2] = point[2] - pos[2];
    if (!normalize(d)) return 0;
    d[0] *= (float)speed; d[1] *= (float)speed; d[2] *= (float)speed;
    a = sh_script_earg_vec(d);
    return sh_script_api_event(p, "setLinearVelocity", &a, 1, NULL);
}

/* p:steer(target, [speed]) */
static int l_steer(lua_State *L)
{
    float point[3];
    double speed = 0;
    void *p = own_projectile(L, 1, "steer");
    int have = sh_script_api_check_point(L, 2, "steer", "where to fly (an entity or a position)", point);
    if (sh_script_api_opt_number(L, 3, "steer", "the speed", &speed) && (speed <= 0 || speed > SPEED_MAX))
        sh_script_refuse(L, "\"steer\" needs a speed above 0 and at most %.0f, got %g", SPEED_MAX, speed);
    if (!p || !have) return bool1(L, 0);
    return bool1(L, steer_to(p, point, speed));
}

/* p:explode() */
static int l_p_explode(lua_State *L)
{
    void *p = own_projectile(L, 1, "explode");
    if (!p || !CE(explode)) return bool1(L, 0);
    return bool1(L, g_ce.explode(p));
}

/* ai:fire(projectile, joint, target) -> p */
static int l_fire(lua_State *L)
{
    const sh_script_engine *e = eng();
    sh_script_handle self_h = 0;
    sh_script_earg a[5];
    char name[128];
    float point[3];
    void *self, *decl, *target = NULL;
    const char *joint;
    uint32_t before, id;
    int at_point;
    self = sh_script_api_owned(L, 1, "fire", "fire from", &self_h);
    decl = check_decl(L, 2, "fire", "projectile", "the projectile");
    joint = sh_script_api_check_text(L, 3, "fire", "the joint to fire from", JOINT_CAP - 1);
    if (luaL_testudata(L, 4, "entity")) {
        at_point = 0;
        target = sh_script_api_entity(L, 4, "fire", NULL);
    } else {
        at_point = 1;
        if (lua_type(L, 4) != LUA_TTABLE)
            sh_script_refuse(L, "\"fire\" needs the target (an entity, or a position with x, y and z), got %s",
                             lua_isnoneornil(L, 4) ? "nothing" : luaL_typename(L, 4));
        sh_script_api_check_vec(L, 4, "fire", "the target", point);
    }
    if (self && sh_script_api_kind(self) != SH_SCRIPT_KIND_AI)
        sh_script_refuse(L, "\"fire\" works on demons; this entity is not one");
    if (!self || !decl || !CE(entity_name) || (!at_point && !target)) return nil1(L);
    if (!g_ce.entity_name(at_point ? self : target, name, sizeof name) || !name[0]) return nil1(L);
    a[0] = sh_script_earg_no_anim();
    memset(&a[1], 0, sizeof a[1]); a[1].type = 'd'; a[1].p = decl;
    memset(&a[2], 0, sizeof a[2]); a[2].type = '2'; a[2].s = joint;
    a[3] = sh_script_earg_text('s', name);
    a[4] = sh_script_earg_int(0);
    before = launch_seq();
    if (!sh_script_api_event(self, "ae_launchProjectileAtTarget", a, 5, NULL)) return nil1(L);
    id = launch_after(self_h, before);
    if (id == SH_SCRIPT_SPAWN_ID_NONE || !e || !e->resolve) return nil1(L);
    {
        void *p = e->resolve(id);
        if (!p) return nil1(L);
        if (at_point) steer_to(p, point, 0);
        sh_script_api_push_engine_entity(L, p);
    }
    return 1;
}

/* ------------------------------------------------------------ registration */

void sh_script_combat_register(void)
{
    static int done;
    static const struct { int kind; const char *name; lua_CFunction fn; } calls[] = {
        { SH_SCRIPT_API_GLOBAL, "hitscan", l_hitscan },
        { SH_SCRIPT_API_GLOBAL, "radius_damage", l_radius_damage },
        { SH_SCRIPT_API_GLOBAL, "damage", l_damage },
        { SH_SCRIPT_API_GLOBAL, "explode", l_explode },
        { SH_SCRIPT_API_GLOBAL, "fx", l_fx },
        { SH_SCRIPT_API_GLOBAL, "fx_move", l_fx_move },
        { SH_SCRIPT_API_GLOBAL, "fx_stop", l_fx_stop },
        { SH_SCRIPT_API_GLOBAL, "sound", l_sound },
        { SH_SCRIPT_API_METHOD, "damage", l_damage },
        { SH_SCRIPT_API_METHOD, "tag", l_tag },
        { SH_SCRIPT_API_METHOD, "pain", l_pain },
        { SH_SCRIPT_API_METHOD, "can_see_from", l_can_see_from },
        { SH_SCRIPT_API_METHOD, "fire", l_fire },
        { SH_SCRIPT_API_METHOD, "steer", l_steer },
        { SH_SCRIPT_API_METHOD, "explode", l_p_explode },
    };
    int i;
    if (done) return;
    done = 1;
    for (i = 0; i < (int)(sizeof calls / sizeof calls[0]); i++) sh_script_api_add(calls[i].kind, calls[i].name, calls[i].fn);
}

#ifndef SH_SCRIPT_PURE /* the engine side ------------------------------------ */

#include "../backend_log.h"
#include "../entity.h"
#include "../engine_build.h"

#define DECL_STATE     0x2Cu       /* bit 0 failed to load, bit 1 not loaded */
#define FX_MAX         1024
#define LOG_LINES_MAX  16

typedef void  (*radius_fn)(void *game, const float *origin, void *inflictor, void *attacker, const void *parms,
                           float scale, unsigned char flag, void *ignore);
typedef void  (*explode_fn)(void *projectile, void *ignore, const void *collision);
typedef void *(*type_fn)(void *registry, const char *type);
typedef void *(*find_decl_fn)(void *manager, const char *name, unsigned char make_default);

static const uint8_t *g_base;
static size_t        g_text_lo, g_text_hi;   /* the image's executable range, RVAs */
static radius_fn     g_radius;
static const uint8_t *g_decl_anchor;
static void         *g_decl_register;
static type_fn       g_decl_type;
static find_decl_fn  g_decl_find;
static uintptr_t     g_vt_ai;
static const uint8_t *g_fx_table;
static int           g_fx_count;
static int           g_log_lines;

static void combat_log(const char *fmt, ...)
{
    char line[400];
    va_list ap;
    if (g_log_lines >= LOG_LINES_MAX) return;
    g_log_lines++;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

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

static int in_text(const void *fn)
{
    uintptr_t a = (uintptr_t)fn, b = (uintptr_t)g_base;
    return g_base && a >= b + g_text_lo && a < b + g_text_hi;
}

/* The decl manager of `type` through the registry the product validates for
 * entityDefs (script_api_common.c), or NULL. */
static void *decl_manager(const char *type)
{
    int32_t displacement;
    void *r = NULL;
    void **vt;
    void *m = NULL;
    if (!g_decl_anchor || !g_decl_type || !g_decl_find) return NULL;
    __try {
        if (memcmp(g_decl_anchor + 0x10, "\x48\x8b\x0d", 3)) return NULL;
        memcpy(&displacement, g_decl_anchor + 0x13, 4);
        r = *(void **)(g_decl_anchor + 0x17 + displacement);
        if (!r) return NULL;
        vt = *(void ***)r;
        if (!vt || vt[7] != g_decl_register || vt[11] != (void *)g_decl_type) return NULL;
        m = g_decl_type(r, type);
    } __except (av_only(GetExceptionCode())) { m = NULL; }
    return m;
}

static void *c_decl(const char *type, const char *name)
{
    void *manager = decl_manager(type), *decl = NULL;
    uint8_t state = 0xff;
    if (!manager) return NULL;
    __try { decl = g_decl_find(manager, name, 0); } __except (av_only(GetExceptionCode())) { decl = NULL; }
    if (!decl) return NULL;
    __try { state = *((const uint8_t *)decl + DECL_STATE); } __except (av_only(GetExceptionCode())) { state = 0xff; }
    return (state & 3) ? NULL : decl;
}

static int c_radius_damage(const float pos[3], void *decl, void *inflictor, void *attacker)
{
    void *gm = sh_entity_gamemgr();
    const uint8_t *parms = (const uint8_t *)decl + SH_COMBAT_DECL_PARMS;
    int ok = 0;
    if (!gm || !g_radius || !decl) return 0;
    /* a damage decl's parms name their own decl (the copies the damage code
     * makes keep it at parms + 0x618) */
    if (read_ptr(parms, SH_COMBAT_PARMS_DECL) != decl) {
        combat_log("Mod scripts: a damage decl is not laid out as this version of Snapmap+ expects, so radius damage "
                   "from scripts is not applied.");
        return 0;
    }
    __try { g_radius(gm, pos, inflictor, attacker, parms, 1.0f, 1, NULL); ok = 1; }
    __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int c_explode(void *p)
{
    void **vt = (void **)read_ptr(p, 0);
    explode_fn fn = vt ? (explode_fn)read_ptr(vt, SH_COMBAT_PROJ_EXPLODE) : NULL;
    int ok = 0;
    if (!fn || !in_text((const void *)fn)) return 0;
    __try { fn(p, p, NULL); ok = 1; } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int c_pain_reaction(void *ai)
{
    int r = -1;
    if (!ai || !g_vt_ai || (uintptr_t)read_ptr(ai, 0) != g_vt_ai) return -1;
    __try { r = *(const int *)((const uint8_t *)ai + SH_COMBAT_AI_PAIN); } __except (av_only(GetExceptionCode())) { r = -1; }
    return r;
}

static int c_entity_name(void *ent, char *out, size_t cap)
{
    return read_cstr((const char *)read_ptr(ent, SH_COMBAT_ENT_NAME), out, cap) && out[0];
}

static int c_fx_condition(const char *name)
{
    int i;
    if (!g_fx_table || !name) return -1;
    for (i = 0; i < g_fx_count; i++) {
        const uint8_t *row = g_fx_table + (size_t)i * 0x18;
        char n[96], v[16];
        if (!read_cstr((const char *)read_ptr(row, 0), n, sizeof n)) continue;
        if (_stricmp(n, name)) continue;
        if (!read_cstr((const char *)read_ptr(row, 8), v, sizeof v) || !v[0]) return -1;
        return atoi(v);
    }
    return -1;
}

static void *c_world(void)
{
    const sh_script_engine *e = sh_script_api_engine();
    return e && e->resolve ? e->resolve(SH_SCRIPT_SPAWN_ID_WORLD) : NULL;
}

static void *clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; r && i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (void *)r[i].addr;
    return NULL;
}

int sh_script_combat_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    static const sh_script_combat_engine engine = {
        c_decl, c_radius_damage, c_explode, c_pain_reaction, c_entity_name, c_fx_condition, c_world
    };
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_SECTION_HEADER *sec;
    sig_result r;
    uint32_t fx;
    int i;
    if (!module_base) return 0;
    g_base = module_base;
    nt = (const IMAGE_NT_HEADERS64 *)(module_base + ((const IMAGE_DOS_HEADER *)module_base)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    g_text_lo = (size_t)-1;
    g_text_hi = 0;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            if (sec[i].VirtualAddress < g_text_lo) g_text_lo = sec[i].VirtualAddress;
            if (sec[i].VirtualAddress + sec[i].Misc.VirtualSize > g_text_hi)
                g_text_hi = sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
        }
    if (sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_RADIUS_DAMAGE], &r) == SIG_OK)
        g_radius = (radius_fn)r.addr;
    else
        combat_log("Mod scripts: the game's radius damage was not found in this executable, so radius_damage and "
                   "explode do nothing.");
    g_decl_anchor = (const uint8_t *)clean(results, count, "DeclRegistryAnchor");
    g_decl_register = clean(results, count, "DeclRegisterFile");
    g_decl_type = (type_fn)clean(results, count, "DeclTypeByName");
    g_decl_find = (find_decl_fn)clean(results, count, "DeclFind");
    if (!g_decl_anchor || !g_decl_register || !g_decl_type || !g_decl_find)
        combat_log("Mod scripts: the game's decl lookup was not found in this executable, so the combat calls that "
                   "name a decl answer nothing.");
    g_vt_ai = SH_RVA(0x2035B30u, 0x204F900u) ? (uintptr_t)module_base + SH_RVA(0x2035B30u, 0x204F900u) : 0;
    fx = sh_script_combat_enum_find(module_base, (uint64_t)(uintptr_t)module_base, "FX_NONE", "FX_EXPLOSIVE_ARMED",
                                    "FX_", &g_fx_count);
    g_fx_table = fx ? module_base + fx : NULL;
    for (i = 1; g_fx_table && i < g_fx_count; i++) {
        /* the tables of several FX_ enums follow one another: fxCondition_t ends where a value 0 starts the next */
        char v[16];
        if (read_cstr((const char *)read_ptr(g_fx_table + (size_t)i * 0x18, 8), v, sizeof v) && !strcmp(v, "0")) {
            g_fx_count = i;
            break;
        }
    }
    if (!g_fx_table || g_fx_count < 2 || g_fx_count > FX_MAX) {
        g_fx_table = NULL;
        g_fx_count = 0;
        combat_log("Mod scripts: the game's effect conditions were not found in this executable, so fx(e, condition) "
                   "refuses every condition.");
    }
    sh_script_combat_set_engine(&engine);
    sh_script_combat_register();
    backend_log("SCRIPTS: combat calls resolved");
    return 1;
}

#endif /* SH_SCRIPT_PURE */
