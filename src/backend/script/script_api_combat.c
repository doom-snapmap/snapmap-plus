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
#include "script_api_ai.h"   /* ai:fire parks the demon it fires from */

#define PI               3.14159265358979323846
#define HITSCAN_RANGE    8192.0
#define NAME_ANY         ((size_t)-1 >> 1)   /* text of any length */
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

/* Three maps keyed by a 32-bit value: launches by projectile spawn id, last
 * hits and deaths by victim handle. Each grows (open addressing, at most half
 * full) and each record is dropped when the handle table releases the entity
 * it concerns (rec_gone), so a record lives exactly as long as its entity and
 * none has a count of its own. */
typedef struct rec_map {
    uint32_t *keys;          /* key + 1; 0 = empty */
    uint8_t  *vals;
    size_t    vsize;
    uint32_t  cap, n;        /* cap: a power of two, or 0 */
} rec_map;

typedef struct launch_val { int mod; sh_script_handle owner; } launch_val;
typedef struct hit_val { sh_script_handle attacker; char *decl; } hit_val;   /* decl malloc'd, whole */

static SRWLOCK g_rec_lock = SRWLOCK_INIT;
static rec_map g_launch = { NULL, NULL, sizeof(launch_val), 0, 0 };
static rec_map g_hits = { NULL, NULL, sizeof(hit_val), 0, 0 };
static rec_map g_dead = { NULL, NULL, 1, 0, 0 };
static sh_script_handle g_fire_owner;    /* ai:fire waiting for its own launch, 0 none */
static uint32_t g_fire_id = SH_SCRIPT_SPAWN_ID_NONE;

static uint32_t rec_slot(const rec_map *m, uint32_t key)
{
    uint32_t h = key + 1;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h & (m->cap - 1);
}

/* The value of key, or NULL. Lock held. */
static void *rec_get(const rec_map *m, uint32_t key)
{
    uint32_t s;
    if (!m->cap) return NULL;
    for (s = rec_slot(m, key); m->keys[s]; s = (s + 1) & (m->cap - 1))
        if (m->keys[s] == key + 1) return m->vals + (size_t)s * m->vsize;
    return NULL;
}

static int rec_grow(rec_map *m)
{
    rec_map g = *m;
    uint32_t i;
    g.cap = m->cap ? m->cap * 2 : 64;
    if (g.cap <= m->cap) return 0;
    g.keys = (uint32_t *)calloc(g.cap, sizeof *g.keys);
    g.vals = (uint8_t *)calloc(g.cap, g.vsize);
    if (!g.keys || !g.vals) { free(g.keys); free(g.vals); return 0; }
    for (i = 0; i < m->cap; i++) {
        uint32_t s;
        if (!m->keys[i]) continue;
        for (s = rec_slot(&g, m->keys[i] - 1); g.keys[s]; s = (s + 1) & (g.cap - 1)) {}
        g.keys[s] = m->keys[i];
        memcpy(g.vals + (size_t)s * g.vsize, m->vals + (size_t)i * m->vsize, m->vsize);
    }
    free(m->keys);
    free(m->vals);
    *m = g;
    return 1;
}

/* The value of key, made (zeroed) when missing; NULL without memory. Lock held. */
static void *rec_put(rec_map *m, uint32_t key)
{
    uint32_t s;
    void *v = rec_get(m, key);
    if (v) return v;
    if ((m->n + 1) * 2 > m->cap && !rec_grow(m)) return NULL;
    for (s = rec_slot(m, key); m->keys[s]; s = (s + 1) & (m->cap - 1)) {}
    m->keys[s] = key + 1;
    memset(m->vals + (size_t)s * m->vsize, 0, m->vsize);
    m->n++;
    return m->vals + (size_t)s * m->vsize;
}

/* Drop key, shifting the run after it back. Lock held. */
static void rec_del(rec_map *m, uint32_t key)
{
    uint32_t mask, s, j;
    if (!m->cap) return;
    mask = m->cap - 1;
    for (s = rec_slot(m, key); m->keys[s] != key + 1; s = (s + 1) & mask)
        if (!m->keys[s]) return;
    j = s;
    for (;;) {
        uint32_t k;
        j = (j + 1) & mask;
        if (!m->keys[j]) break;
        k = rec_slot(m, m->keys[j] - 1);
        if (s <= j ? (s < k && k <= j) : (s < k || k <= j)) continue;
        m->keys[s] = m->keys[j];
        memcpy(m->vals + (size_t)s * m->vsize, m->vals + (size_t)j * m->vsize, m->vsize);
        s = j;
    }
    m->keys[s] = 0;
    m->n--;
}

static void rec_free(rec_map *m)
{
    free(m->keys);
    free(m->vals);
    m->keys = NULL;
    m->vals = NULL;
    m->cap = m->n = 0;
}

/* A last hit's decl text goes with its record. Lock held. */
static void hit_forget(sh_script_handle h)
{
    hit_val *v = (hit_val *)rec_get(&g_hits, h);
    if (v) free(v->decl);
    rec_del(&g_hits, h);
}

static void hits_free(void)
{
    uint32_t i;
    for (i = 0; i < g_hits.cap; i++) if (g_hits.keys[i]) free(((hit_val *)(g_hits.vals + (size_t)i * g_hits.vsize))->decl);
    rec_free(&g_hits);
}

/* The handle table released an entity: its records go. */
static void rec_gone(sh_script_handle h, uint32_t spawn_id)
{
    AcquireSRWLockExclusive(&g_rec_lock);
    rec_del(&g_launch, spawn_id);
    hit_forget(h);
    rec_del(&g_dead, h);
    ReleaseSRWLockExclusive(&g_rec_lock);
}

void sh_script_combat_reset(void)
{
    AcquireSRWLockExclusive(&g_rec_lock);
    rec_free(&g_launch);
    hits_free();
    rec_free(&g_dead);
    g_fire_owner = 0;
    g_fire_id = SH_SCRIPT_SPAWN_ID_NONE;
    ReleaseSRWLockExclusive(&g_rec_lock);
    sh_script_handles_on_gone(rec_gone);
}

void sh_script_combat_note_launch(uint32_t spawn_id, sh_script_handle owner, int mod)
{
    launch_val *v;
    if (spawn_id == SH_SCRIPT_SPAWN_ID_NONE || mod < 0) return;
    AcquireSRWLockExclusive(&g_rec_lock);
    v = (launch_val *)rec_put(&g_launch, spawn_id);
    if (v) { v->mod = mod; v->owner = owner; }
    if (owner && owner == g_fire_owner) g_fire_id = spawn_id;
    ReleaseSRWLockExclusive(&g_rec_lock);
}

sh_script_handle sh_script_combat_launch(uint32_t spawn_id, sh_script_handle owner, int mod)
{
    sh_script_handle p;
    if (spawn_id == SH_SCRIPT_SPAWN_ID_NONE || mod < 0) return 0;
    p = sh_script_handles_find(spawn_id, NULL);
    if (!p) p = sh_script_handles_ref(spawn_id);
    if (!p) return 0;   /* no handle, no record: nothing would release it */
    sh_script_combat_note_launch(spawn_id, owner, mod);
    return p;
}

int sh_script_combat_launched(uint32_t spawn_id, int *mod, sh_script_handle *owner)
{
    const launch_val *v;
    int found = 0;
    if (spawn_id == SH_SCRIPT_SPAWN_ID_NONE) return 0;
    AcquireSRWLockShared(&g_rec_lock);
    v = (const launch_val *)rec_get(&g_launch, spawn_id);
    if (v) {
        if (mod) *mod = v->mod;
        if (owner) *owner = v->owner;
        found = 1;
    }
    ReleaseSRWLockShared(&g_rec_lock);
    return found;
}

int sh_script_combat_launches(void)
{
    int n;
    AcquireSRWLockShared(&g_rec_lock);
    n = (int)g_launch.n;
    ReleaseSRWLockShared(&g_rec_lock);
    return n;
}

/* ai:fire: catch the launch its own event makes (the event runs at once on
 * this thread, and the launch hook reports it before the event returns). */
static void fire_begin(sh_script_handle owner)
{
    AcquireSRWLockExclusive(&g_rec_lock);
    g_fire_owner = owner;
    g_fire_id = SH_SCRIPT_SPAWN_ID_NONE;
    ReleaseSRWLockExclusive(&g_rec_lock);
}

static uint32_t fire_end(void)
{
    uint32_t id;
    AcquireSRWLockExclusive(&g_rec_lock);
    id = g_fire_id;
    g_fire_owner = 0;
    g_fire_id = SH_SCRIPT_SPAWN_ID_NONE;
    ReleaseSRWLockExclusive(&g_rec_lock);
    return id;
}

void sh_script_combat_note_hit(sh_script_handle victim, const char *decl, sh_script_handle attacker)
{
    hit_val *v;
    char *copy;
    if (!victim) return;
    copy = _strdup(decl ? decl : "");
    AcquireSRWLockExclusive(&g_rec_lock);
    v = (hit_val *)rec_put(&g_hits, victim);
    if (v) {
        v->attacker = attacker;
        free(v->decl);
        v->decl = copy;
        copy = NULL;
    }
    ReleaseSRWLockExclusive(&g_rec_lock);
    free(copy);
}

int sh_script_combat_last_hit(sh_script_handle victim, char **decl, sh_script_handle *attacker)
{
    const hit_val *v;
    int found = 0;
    if (decl) *decl = NULL;
    if (attacker) *attacker = 0;
    if (!victim) return 0;
    AcquireSRWLockShared(&g_rec_lock);
    v = (const hit_val *)rec_get(&g_hits, victim);
    if (v) {
        if (decl) *decl = _strdup(v->decl ? v->decl : "");
        if (attacker) *attacker = v->attacker;
        found = 1;
    }
    ReleaseSRWLockShared(&g_rec_lock);
    return found;
}

int sh_script_combat_first_death(sh_script_handle victim)
{
    int first = 0;
    if (!victim) return 0;
    AcquireSRWLockExclusive(&g_rec_lock);
    if (!rec_get(&g_dead, victim)) { (void)rec_put(&g_dead, victim); first = 1; }
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
    if (sh_script_api_opt_number(L, 4, "hitscan", "the range", &range) && !(range >= 1))
        sh_script_refuse(L, "\"hitscan\" needs a range of at least 1, got %g", range);
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

/* Call the API's own spawn (its ownership and the engine's room) with name, pos, yaw.
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
    const char *name = sh_script_api_check_text(L, arg, call, "the effect condition (FX_... of the entity's fx decl)", NAME_ANY);
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
        const char *c = sh_script_api_check_text(L, 3, "sound", "the channel", NAME_ANY);
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
    const char *name = sh_script_api_check_text(L, 2, "tag", "the joint name", NAME_ANY);
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
    const char *joint = sh_script_api_check_text(L, 3, "can_see_from", "the joint to look from", NAME_ANY);
    if (!self || !other || !e || !e->trace || !e->spawn_id) return nil1(L);
    if (!joint_of(self, joint, from, NULL)) return nil1(L);
    origin_of(other, to);
    to[2] += SIGHT_RAISE;
    if (!e->trace(from, to, e->spawn_id(self), &t)) return nil1(L);
    return bool1(L, !(t.fraction < 1.0f) || t.hit_spawn_id == e->spawn_id(other));
}

/* ------------------------------------------------------------ projectiles */

/* The projectile at arg, one this mod's entities launched. NULL when it no
 * longer exists (the call answers nil, as every call on a removed entity
 * does); its launch record lives exactly as long as it does, so a projectile
 * that still exists always has one. A refusal only for an entity that exists
 * and is not such a projectile. */
static void *own_projectile(lua_State *L, int arg, const char *call)
{
    sh_script_handle h = 0;
    void *p = sh_script_api_entity(L, arg, call, &h);
    int mod = -1;
    if (!h || !p) return NULL;
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
    void *p;
    int have = sh_script_api_check_point(L, 2, "steer", "where to fly (an entity or a position)", point);
    if (sh_script_api_opt_number(L, 3, "steer", "the speed", &speed) && !(speed > 0))
        sh_script_refuse(L, "\"steer\" needs a speed above 0, got %g", speed);
    p = own_projectile(L, 1, "steer");
    if (!p) return nil1(L);
    if (!have) return bool1(L, 0);
    return bool1(L, steer_to(p, point, speed));
}

/* p:explode() */
static int l_p_explode(lua_State *L)
{
    void *p = own_projectile(L, 1, "explode");
    if (!p) return nil1(L);
    if (!CE(explode)) return bool1(L, 0);
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
    uint32_t id;
    int at_point;
    self = sh_script_api_owned(L, 1, "fire", "fire from", &self_h);
    decl = check_decl(L, 2, "fire", "projectile", "the projectile");
    joint = sh_script_api_check_text(L, 3, "fire", "the joint to fire from", NAME_ANY);
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
    sh_script_api_check_room(L, "fire");
    if (!g_ce.entity_name(at_point ? self : target, name, sizeof name) || !name[0]) return nil1(L);
    (void)sh_script_ai_park(self, self_h);   /* a demon a script fires from is the script's, as one it moves */
    a[0] = sh_script_earg_no_anim();
    memset(&a[1], 0, sizeof a[1]); a[1].type = 'd'; a[1].p = decl;
    memset(&a[2], 0, sizeof a[2]); a[2].type = '2'; a[2].s = joint;
    a[3] = sh_script_earg_text('s', name);
    a[4] = sh_script_earg_int(0);
    fire_begin(self_h);
    if (!sh_script_api_event(self, "ae_launchProjectileAtTarget", a, 5, NULL)) { (void)fire_end(); return nil1(L); }
    id = fire_end();
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
#include "../feature_health.h"
#include "../entity.h"
#include "../engine_build.h"

#define DECL_STATE     0x2Cu       /* bit 0 failed to load, bit 1 not loaded */
#define FX_MAX         1024

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
static char          g_log_last[512];   /* the line written last */

static void combat_log(const char *fmt, ...)
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

/* A part of the combat calls this executable cannot serve: one log line, and
 * the feature registry's row says why (feature_health.h). */
static void combat_off(const char *why)
{
    combat_log("Mod scripts: %s.", why);
    sh_feature_off(SH_FEATURE_SCRIPT_COMBAT, why);
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
        combat_off("the game's radius damage was not found in this executable, so radius_damage and explode do "
                   "nothing");
    g_decl_anchor = (const uint8_t *)clean(results, count, "DeclRegistryAnchor");
    g_decl_register = clean(results, count, "DeclRegisterFile");
    g_decl_type = (type_fn)clean(results, count, "DeclTypeByName");
    g_decl_find = (find_decl_fn)clean(results, count, "DeclFind");
    if (!g_decl_anchor || !g_decl_register || !g_decl_type || !g_decl_find)
        combat_off("the game's decl lookup was not found in this executable, so the combat calls that name a decl "
                   "answer nothing");
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
        combat_off("the game's effect conditions were not found in this executable, so fx(e, condition) refuses "
                   "every condition");
    }
    sh_script_combat_set_engine(&engine);
    sh_script_combat_register();
    backend_log("SCRIPTS: combat calls resolved");
    return 1;
}

#endif /* SH_SCRIPT_PURE */
