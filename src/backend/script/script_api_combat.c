/* script_api_combat.c -- see script_api_combat.h. The pure part (the calls, their argument checks and ownership,
 * the hooks' records) builds alone with SH_SCRIPT_PURE for the tests; the engine part fills the combat engine. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../common/text.h"
#include "script_api_combat.h"
#include "script_api_common.h"
#include "script_hooks.h"
#include "script_api_ai.h"   /* ai:fire parks the demon it fires from */

#define PI               3.14159265358979323846
#define HITSCAN_RANGE    8192.0
#define SIGHT_RAISE      40.0f     /* can_see_from aims this far above the other entity's origin */
#define REG_EFFECTS      "snapmap-plus.effects"

static sh_script_combat_engine g_ce;
static int g_have_ce;

void sh_script_combat_set_engine(const sh_script_combat_engine *engine)
{
    if (engine) { g_ce = *engine; g_have_ce = 1; }
    else { memset(&g_ce, 0, sizeof g_ce); g_have_ce = 0; }
}

#define CE(fn) (g_have_ce && g_ce.fn)

#define FX_HARD_MAX 64
static SRWLOCK g_fx_hard_lock = SRWLOCK_INIT;
static char   *g_fx_hard[FX_HARD_MAX];       /* whole names, or prefixes ending in '/' */
static volatile LONG g_fx_hard_n;

int sh_fx_hard_ends(int type, int screen_particle, int waiting)
{
    switch (type) {
    case SH_FX_TYPE_MODEL:
    case SH_FX_TYPE_RENDERPARM:
    case SH_FX_TYPE_BLUR_RADIAL:
    case SH_FX_TYPE_BLUR_GAUSSIAN:
    case SH_FX_TYPE_SCREEN_OVERLAY:
        return 1;
    case SH_FX_TYPE_PARTICLE:
        return screen_particle || waiting;
    case SH_FX_TYPE_LIGHT:
        return waiting != 0;
    default:
        return 0;
    }
}

int sh_fx_sound_held(int type, unsigned flags, int restart_before_end)
{
    const unsigned playing = SH_FX_ACTION_REQUESTED | SH_FX_ACTION_STARTED;
    return type == SH_FX_TYPE_SOUND && (flags & playing) == playing && !restart_before_end;
}

static int fx_hard_matches(const char *entry, const char *name)
{
    size_t n = strlen(entry);
    if (n && entry[n - 1] == '/') return _strnicmp(entry, name, n) == 0;
    return _stricmp(entry, name) == 0;
}

int sh_fx_hard_is(const char *decl_name)
{
    int i, n, is = 0;
    if (!g_fx_hard_n || !decl_name || !decl_name[0]) return 0;
    AcquireSRWLockShared(&g_fx_hard_lock);
    n = (int)g_fx_hard_n;
    for (i = 0; i < n && !is; i++) is = fx_hard_matches(g_fx_hard[i], decl_name);
    ReleaseSRWLockShared(&g_fx_hard_lock);
    return is;
}

int sh_fx_hard_add(const char *name)
{
    int i, n, ok = 0;
    char *copy;
    if (!name || !name[0]) return 0;
    AcquireSRWLockExclusive(&g_fx_hard_lock);
    n = (int)g_fx_hard_n;
    for (i = 0; i < n; i++)
        if (!_stricmp(g_fx_hard[i], name)) { ok = 1; break; }
    if (!ok && n < FX_HARD_MAX && (copy = _strdup(name)) != NULL) {
        g_fx_hard[n] = copy;
        InterlockedExchange(&g_fx_hard_n, n + 1);
        ok = 1;
    }
    ReleaseSRWLockExclusive(&g_fx_hard_lock);
    return ok;
}

int sh_fx_hard_count(void) { return (int)g_fx_hard_n; }

void sh_fx_hard_reset(void)
{
    int i;
    AcquireSRWLockExclusive(&g_fx_hard_lock);
    for (i = 0; i < (int)g_fx_hard_n; i++) { free(g_fx_hard[i]); g_fx_hard[i] = NULL; }
    InterlockedExchange(&g_fx_hard_n, 0);
    ReleaseSRWLockExclusive(&g_fx_hard_lock);
}

/* Three maps keyed by a 32-bit value: launches by projectile spawn id, last hits and deaths by victim handle.
 * Each grows (open addressing, at most half full) and drops a record when the handle table releases its entity
 * (rec_gone), so none has a count of its own. */
typedef struct rec_map {
    uint32_t *keys;          /* key + 1; 0 = empty */
    uint8_t  *vals;
    size_t    vsize;
    uint32_t  cap, n;        /* cap: a power of two, or 0 */
} rec_map;

typedef struct launch_val { int mod; sh_script_handle owner; } launch_val;
typedef struct twitch_row { const char *decl; float limit, scale; } twitch_row;
typedef struct twitch_profile {
    void *owner;
    float *weight;
    float base;
    size_t count;
    twitch_row rows[];
} twitch_profile;
typedef struct hit_val { sh_script_handle attacker; char *decl; twitch_profile *twitch; } hit_val;

static SRWLOCK g_rec_lock = SRWLOCK_INIT;
static rec_map g_launch = { NULL, NULL, sizeof(launch_val), 0, 0 };
static rec_map g_hits = { NULL, NULL, sizeof(hit_val), 0, 0 };
static rec_map g_dead = { NULL, NULL, 1, 0, 0 };
static sh_script_handle g_fire_owner;    /* ai:fire waiting for its own launch, 0 none */
static uint32_t g_fire_id = SH_SCRIPT_SPAWN_ID_NONE;
static int   g_fire_from;                /* 1: that launch starts at g_fire_origin, heading along g_fire_axis */
static float g_fire_origin[3], g_fire_axis[9];

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
    if (v) { free(v->decl); free(v->twitch); }
    rec_del(&g_hits, h);
}

static void hits_free(void)
{
    uint32_t i;
    for (i = 0; i < g_hits.cap; i++) if (g_hits.keys[i]) {
        hit_val *v = (hit_val *)(g_hits.vals + (size_t)i * g_hits.vsize);
        if (v->twitch && sh_script_handle_entity(g_hits.keys[i] - 1) == v->twitch->owner)
            *v->twitch->weight = v->twitch->base;
        free(v->twitch);
        free(v->decl);
    }
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
    sh_fx_hard_reset();
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

/* The launch hook asks before it lets the engine launch: 1 with the place and
 * axis ai:fire wants its own launch to start from (a tag of the model; the
 * engine's event knows joints only and would start at the demon's origin). */
int sh_script_combat_launch_from(sh_script_handle owner, float origin[3], float axis[9])
{
    int have;
    AcquireSRWLockExclusive(&g_rec_lock);
    have = owner && owner == g_fire_owner && g_fire_from == 1;
    if (have) {
        memcpy(origin, g_fire_origin, sizeof g_fire_origin);
        memcpy(axis, g_fire_axis, sizeof g_fire_axis);
        g_fire_from = 2;                 /* taken: the projectile started there */
    }
    ReleaseSRWLockExclusive(&g_rec_lock);
    return have;
}

/* ai:fire: catch the launch its own event makes (the event runs at once on
 * this thread, and the launch hook reports it before the event returns). */
static void fire_begin(sh_script_handle owner, const float *origin, const float *axis)
{
    AcquireSRWLockExclusive(&g_rec_lock);
    g_fire_owner = owner;
    g_fire_id = SH_SCRIPT_SPAWN_ID_NONE;
    g_fire_from = origin && axis;
    if (g_fire_from) {
        memcpy(g_fire_origin, origin, sizeof g_fire_origin);
        memcpy(g_fire_axis, axis, sizeof g_fire_axis);
    }
    ReleaseSRWLockExclusive(&g_rec_lock);
}

static uint32_t fire_end(int *started_there)
{
    uint32_t id;
    AcquireSRWLockExclusive(&g_rec_lock);
    id = g_fire_id;
    if (started_there) *started_there = g_fire_from == 2;
    g_fire_from = 0;
    g_fire_owner = 0;
    g_fire_id = SH_SCRIPT_SPAWN_ID_NONE;
    ReleaseSRWLockExclusive(&g_rec_lock);
    return id;
}

static float twitch_scale(const twitch_profile *p, const char *decl, float damage)
{
    size_t i;
    float scale = 1;
    int matched = 0;
    for (i = 0; i < p->count; i++) {
        const twitch_row *r = p->rows + i;
        if (r->decl) {
            if (decl && !_stricmp(decl, r->decl)) return r->scale;
        } else if (!matched && damage <= r->limit) {
            scale = r->scale;
            matched = 1;
        }
    }
    return scale;
}

void sh_script_combat_note_hit(sh_script_handle victim, const char *decl, sh_script_handle attacker, float damage)
{
    hit_val *v;
    char *copy;
    if (!victim) return;
    copy = _strdup(decl ? decl : "");
    AcquireSRWLockExclusive(&g_rec_lock);
    v = (hit_val *)rec_put(&g_hits, victim);
    if (v) {
        if (v->twitch) *v->twitch->weight = v->twitch->base * twitch_scale(v->twitch, decl, damage);
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
    if (v && v->decl) {
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
    const char *full;
    void *decl;
    int is_game = 0;
    if (lua_type(L, arg) != LUA_TSTRING && !lua_isuserdata(L, arg))
        sh_script_refuse(L, "\"%s\" needs %s (the name of a %s decl of this mod, or of DOOM's with native(name)), "
                            "got %s", call, what, type, lua_isnoneornil(L, arg) ? "nothing" : luaL_typename(L, arg));
    full = strcmp(type, "sound") ? sh_script_check_name(L, arg, call, 1, &is_game)
                                 : sh_script_check_sound_name(L, arg, call, 1, &is_game);
    if (!CE(decl)) return NULL;
    decl = g_ce.decl(type, full);
    if (!decl && is_game)
        sh_script_refuse(L, "\"%s\" found no %s decl named %s in the game", call, type, full);
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
        sh_script_refuse(L, "\"%s\" needs the attacker as an entity, got %s", call,
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
    out[0] = d[2] == 0.0f ? 0.0f : (float)(-atan2((double)d[2], h) * 180.0 / PI);
    out[1] = d[1] == 0.0f && d[0] >= 0.0f ? 0.0f : (float)(atan2((double)d[1], (double)d[0]) * 180.0 / PI);
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

/* Damage from a script. When the script names the attacker and the victim is an actor (a player, a bot or a
 * demon), the victim's own Damage takes it with the attacker as inflictor and attacker, so the game's team rule
 * (idAI2 Damage_Calculate, VK 0x1403FB1D0), its percepts and its kill credit see the attacker. Anything else gets
 * its own `damage` event (decl, from direction, no radius, full multiplier, the attacker as activator).
 * 1 when the engine ran it. */
static int damage_event(void *victim, void *decl, const float dir[3], void *attacker, int named_attacker)
{
    sh_script_earg a[6];
    float ang[3];
    if (named_attacker && g_ce.actor_damage && g_ce.actor_damage(victim, decl, attacker, attacker, dir)) return 1;
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

/* What damage() answers: for a demon or a player whose health reads, true when its health fell (a hit the game's
 * rules stop answers false). The game applies the hit inside the call, since idAI2's Damage defers only inside the
 * AI think phase (VK 0x145B147A8) and the script tick runs after it. Anything without health answers whether the
 * engine ran the damage. */
static int damage_landed(void *victim, void *decl, const float dir[3], void *attacker, int named_attacker)
{
    float before = 0, after = 0, max = 0;
    int measured = sh_script_api_health(victim, &before, &max);
    if (!damage_event(victim, decl, dir, attacker, named_attacker)) return 0;
    if (!measured) return 1;
    return sh_script_api_health(victim, &after, &max) && after < before;
}

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
    return bool1(L, damage_landed(victim, decl, dir, attacker, !lua_isnoneornil(L, target + 2)));
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
    if (hit) damage_event(hit, decl, dir, attacker, !lua_isnoneornil(L, 5));
    lua_createtable(L, 0, 3);
    sh_script_api_push_vec(L, t.end); lua_setfield(L, -2, "position");
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
    void *ent = sh_script_api_entity(L, ent_index, "play_effect", NULL);
    if (!ent || !e || !e->place) return 0;
    if (dir) {
        memcpy(d, dir, 12);
        if (!normalize(d)) return e->place(ent, pos, NULL);
        dir_axis(d, ax);
        return e->place(ent, pos, ax);
    }
    return e->place(ent, pos, NULL);
}

int sh_script_combat_fx_condition(const char *name)
{
    return CE(fx_condition) && CE(start_fx) ? g_ce.fx_condition(name) : -1;
}

static int fx_condition(lua_State *L, int arg, const char *call)
{
    const char *name = sh_script_api_check_text(L, arg, call, "the effect condition (FX_... of the entity's fx decl)", SH_SCRIPT_TEXT_ANY);
    int v;
    if (!CE(fx_condition)) return -2;
    v = g_ce.fx_condition(name);
    if (v < 0)
        sh_script_refuse(L, "\"%s\" knows no effect condition \"%s\"; use one of the FX_ conditions the entity's fx "
                            "decl lists", call, name);
    return v;
}

/* An actor (an AI, a player) starts and stops its effects by the animation events its clips use. Any other entity
 * (an effect entity made by play_effect(effect, pos) or explode) answers neither event: the engine's own entity call. */
static int fx_by_entity_call(void *ent)
{
    int kind = sh_script_api_kind(ent);
    return kind != SH_SCRIPT_KIND_AI && kind != SH_SCRIPT_KIND_PLAYER;
}

/* One of the mod's Graphics SDK effects (an fx decl written `type effect`): its emitter spawned at a point, or at
 * an entity and bound to it (to one of its joints when named); `options` { colour = {r, g, b} } tints this play
 * through the effect entity's setColor (VK FUN_1416f0240). Skipped with one log line a map near the entity limit.
 * Leaves the effect entity (or nil) on the stack. */
static int g_effect_skipped_logged;

static int play_sdk_effect(lua_State *L, const char *emitter)
{
    const sh_script_engine *e = eng();
    float pos[3], dir[3], axis[9];
    int have_dir = 0, at_entity = luaL_testudata(L, 2, "entity") != NULL, opt = 0, top;
    void *master = NULL;
    const char *joint = NULL;
    if (at_entity) {
        master = sh_script_api_owned(L, 2, "play_effect", "play an effect on", NULL);
        if (!master || !e || !e->origin || !e->origin(master, pos)) { lua_pushnil(L); return 1; }
        if (lua_type(L, 3) == LUA_TSTRING) { joint = lua_tostring(L, 3); opt = 4; }
        else opt = 3;
    } else {
        sh_script_api_check_vec(L, 2, "play_effect", "where to show the effect", pos);
        /* a third table is the options when it names one, else the direction */
        if (lua_istable(L, 3) && lua_getfield(L, 3, "colour") != LUA_TNIL) { lua_pop(L, 1); opt = 3; }
        else {
            if (lua_istable(L, 3)) lua_pop(L, 1);
            have_dir = sh_script_api_opt_vec(L, 3, "play_effect", "the direction the effect faces", dir);
            opt = 4;
        }
    }
    if (!sh_script_api_has_room()) {
        if (!g_effect_skipped_logged) {
            if (sh_script_sdk_runtime_get()->log) sh_script_sdk_runtime_get()->log("Mod scripts: an effect was not played because the game holds nearly all the entities it can; "
                        "effects play again when entities are freed");
            g_effect_skipped_logged = 1;
        }
        lua_pushnil(L);
        return 1;
    }
    sh_script_api_spawn_def(L, emitter, pos, have_dir ? atan2((double)dir[1], (double)dir[0]) * 180.0 / PI : 0.0);
    top = lua_gettop(L);
    if (lua_isnil(L, top)) return 1;
    if (have_dir) place_effect(L, top, pos, dir);
    (void)axis;
    {
        void *fx = sh_script_api_entity(L, top, "play_effect", NULL);
        if (fx && master) {
            sh_script_earg a[4];
            if (joint) {
                a[0] = sh_script_earg_entity(master);
                a[1] = sh_script_earg_text('s', "");
                a[2] = sh_script_earg_text('s', joint);
                a[3] = sh_script_earg_int(0);
                (void)sh_script_api_event(fx, "bindToJoint", a, 4, NULL);
            } else {
                a[0] = sh_script_earg_entity(master);
                (void)sh_script_api_event(fx, "bind", a, 1, NULL);
            }
        }
        if (fx && opt && lua_istable(L, opt)) {
            if (lua_getfield(L, opt, "colour") == LUA_TTABLE) {
                float c[3];
                int k;
                sh_script_earg a[3];
                for (k = 0; k < 3; k++) {
                    lua_rawgeti(L, -1, k + 1);
                    c[k] = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 1.0f;
                    lua_pop(L, 1);
                }
                for (k = 0; k < 3; k++) a[k] = sh_script_earg_float(c[k]);
                (void)sh_script_api_event(fx, "setColor", a, 3, NULL);
            }
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
    effects_mark(L, top);
    return 1;
}

int sh_script_combat_start_owner_fx(void *ent, int condition)
{
    sh_script_earg args[2];
    if (!ent || condition < 0) return 0;
    if (fx_by_entity_call(ent)) return CE(start_fx) && g_ce.start_fx(ent, condition);
    args[0] = sh_script_earg_no_anim();
    args[1] = sh_script_earg_int(condition);
    return sh_script_api_event(ent, "ae_startFX_Networked", args, 2, NULL);
}

/* play_effect(e, condition) or play_effect(effect, pos, [dir], [options]) or play_effect(effect, e, [joint], [options]) */
static int l_fx(lua_State *L)
{
    if (luaL_testudata(L, 1, "entity")) {
        void *ent = sh_script_api_owned(L, 1, "play_effect", "start effects on", NULL);
        int cond = fx_condition(L, 2, "play_effect");
        return bool1(L, sh_script_combat_start_owner_fx(ent, cond));
    } else {
        float pos[3], dir[3];
        int have_dir;
        if (lua_type(L, 1) != LUA_TSTRING)
            sh_script_refuse(L, "\"play_effect\" needs an entity and an effect condition, or the name of one of this mod's "
                                "effect entities and a position; got %s", lua_isnoneornil(L, 1) ? "nothing" : luaL_typename(L, 1));
        {
            /* one of the mod's Graphics SDK effects: its emitter */
            const sh_script_sdk_runtime *rt = sh_script_sdk_runtime_get();
            if (rt->effect_emitter) {
                char *full = sh_textf("mods/%s/%s", sh_script_salt(L), lua_tostring(L, 1)), *em;
                em = full && strncmp(lua_tostring(L, 1), "mods/", 5) ? rt->effect_emitter(full) :
                     (full ? rt->effect_emitter(lua_tostring(L, 1)) : NULL);
                free(full);
                if (em) {
                    int r;
                    lua_pushstring(L, em);
                    free(em);
                    lua_replace(L, 1);
                    r = play_sdk_effect(L, lua_tostring(L, 1));
                    return r;
                }
            }
        }
        sh_script_api_check_vec(L, 2, "play_effect", "where to show the effect", pos);
        have_dir = sh_script_api_opt_vec(L, 3, "play_effect", "the direction the effect faces", dir);
        lua_settop(L, 3);
        call_spawn(L, 1, pos, have_dir ? atan2((double)dir[1], (double)dir[0]) * 180.0 / PI : 0.0);
        if (lua_isnil(L, -1)) return 1;
        if (have_dir) place_effect(L, lua_gettop(L), pos, dir);
        effects_mark(L, lua_gettop(L));
        return 1;
    }
}

/* move_effect(e, pos, [dir]) */
static int l_fx_move(lua_State *L)
{
    float pos[3], dir[3];
    int have_dir;
    sh_script_api_owned(L, 1, "move_effect", "move", NULL);
    sh_script_api_check_vec(L, 2, "move_effect", "where to move the effect", pos);
    have_dir = sh_script_api_opt_vec(L, 3, "move_effect", "the direction the effect faces", dir);
    if (!effects_has(L, 1))
        sh_script_refuse(L, "\"move_effect\" moves an effect made by play_effect(effect, pos); this entity is not one");
    return bool1(L, place_effect(L, 1, pos, have_dir ? dir : NULL));
}

/* stop_effect(e, [condition]) */
static int l_fx_stop(lua_State *L)
{
    void *ent = sh_script_api_owned(L, 1, "stop_effect", "stop effects on", NULL);
    if (lua_isnoneornil(L, 2)) {
        if (!effects_has(L, 1))
            sh_script_refuse(L, "\"stop_effect\" without a condition removes an effect made by play_effect(effect, pos); for an "
                                "entity's own effect give the condition it was started with");
        if (!ent) return bool1(L, 0);
        return bool1(L, sh_script_api_post(ent, "remove", NULL, 0));
    } else {
        sh_script_earg a[2];
        int cond = fx_condition(L, 2, "stop_effect");
        if (!ent || cond < 0) return bool1(L, 0);
        if (fx_by_entity_call(ent)) return bool1(L, CE(stop_fx) && g_ce.stop_fx(ent, cond));
        a[0] = sh_script_earg_no_anim();
        a[1] = sh_script_earg_int(cond);
        return bool1(L, sh_script_api_event(ent, "ae_stopFX_Networked", a, 2, NULL));
    }
}

/* end_effects_at_once([effect]): the hard stop rule (script_api_combat.h) for one fx decl of this mod, or for all of
 * them. true when the rule is in place for it. */
static int l_fx_hard_stops(lua_State *L)
{
    const char *name;
    if (lua_isnoneornil(L, 1)) {
        lua_settop(L, 0);
        lua_pushfstring(L, "mods/%s/", sh_script_salt(L));
        name = lua_tostring(L, -1);
    } else {
        name = sh_script_check_name(L, 1, "end_effects_at_once", 0, NULL);
    }
    if (!CE(fx_hard_stops) || !g_ce.fx_hard_stops()) return bool1(L, 0);
    return bool1(L, sh_fx_hard_add(name));
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
        const char *c = sh_script_api_check_text(L, 3, "sound", "the channel", SH_SCRIPT_TEXT_ANY);
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

/* The point `name` names on `ent` in world space: a tag of the demon's
 * md6Def (its own position and axis, as the engine aims from it) or else a
 * joint. 2 a tag, 1 a joint, 0 neither. */
static int point_of(void *ent, const char *name, float pos[3], float fwd[3])
{
    const sh_script_engine *e = eng();
    float axis[9];
    if (e && e->tag && e->tag(ent, name, pos, axis) == 1) {
        if (fwd) memcpy(fwd, axis, 3 * sizeof(float));   /* the axis' first row: forward */
        return 2;
    }
    return joint_of(ent, name, pos, fwd);
}

/* e:tag(name) -> pos, forward: an md6Def tag or a joint */
static int l_tag(lua_State *L)
{
    float pos[3], fwd[3];
    void *ent = sh_script_api_entity(L, 1, "tag", NULL);
    const char *name = sh_script_api_check_text(L, 2, "tag", "the tag or joint name", SH_SCRIPT_TEXT_ANY);
    if (!ent || !point_of(ent, name, pos, fwd)) return nil1(L);
    sh_script_api_push_vec(L, pos);
    sh_script_api_push_vec(L, fwd);
    return 2;
}

/* The entity must belong to this mod. Its ribbon declaration supplies width and material. */
static int l_draw_ribbon(lua_State *L)
{
    const sh_script_engine *engine = eng();
    void *emitter = sh_script_api_owned(L, 1, "draw_ribbon", "draw with", NULL);
    float points[128][3], rgba[4] = {1, 1, 1, 1};
    size_t i, n;
    int now;
    luaL_checktype(L, 2, LUA_TTABLE);
    n = lua_rawlen(L, 2);
    if (n < 2 || n > 128) return sh_script_refuse(L, "draw_ribbon needs between 2 and 128 points");
    for (i = 0; i < n; i++) {
        lua_rawgeti(L, 2, (lua_Integer)(i + 1));
        sh_script_api_check_vec(L, -1, "draw_ribbon", "a point on the ribbon", points[i]);
        lua_pop(L, 1);
        if (!isfinite(points[i][0]) || !isfinite(points[i][1]) || !isfinite(points[i][2]))
            return sh_script_refuse(L, "draw_ribbon needs finite point coordinates");
    }
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        for (i = 0; i < 4; i++) {
            lua_rawgeti(L, 3, (lua_Integer)(i + 1));
            rgba[i] = (float)luaL_checknumber(L, -1);
            lua_pop(L, 1);
            if (!isfinite(rgba[i]) || rgba[i] < 0 || rgba[i] > 1)
                return sh_script_refuse(L, "draw_ribbon needs four colour channels between zero and one");
        }
    }
    lua_pushboolean(L, emitter && CE(draw_ribbon) && engine && engine->game_time_ms && engine->game_time_ms(&now)
                    && g_ce.draw_ribbon(emitter, now, points, n, rgba));
    return 1;
}

/* Install an owned demon's damage bands and named overrides once at spawn. */
static int l_twitch_response(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai = sh_script_api_owned(L, 1, "twitch_response", "set hit reactions on", &h);
    size_t n, i, names = 0, bytes;
    float last = -1, *weight;
    twitch_row *rows;
    twitch_profile *p = NULL;
    hit_val *v;
    char *name;
    if (ai && sh_script_api_kind(ai) != SH_SCRIPT_KIND_AI)
        return sh_script_refuse(L, "twitch_response needs a demon");
    luaL_checktype(L, 2, LUA_TTABLE);
    n = lua_rawlen(L, 2);
    if (n > (SIZE_MAX - sizeof *p) / sizeof *rows) return luaL_error(L, "twitch response is too large");
    rows = (twitch_row *)lua_newuserdatauv(L, n * sizeof *rows, 0);
    for (i = 0; i < n; i++) {
        size_t len;
        lua_rawgeti(L, 2, (lua_Integer)i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_rawgeti(L, -1, 1);
        rows[i].decl = NULL;
        if (lua_type(L, -1) == LUA_TSTRING) {
            rows[i].decl = lua_tolstring(L, -1, &len);
            if (!len || strlen(rows[i].decl) != len || names > SIZE_MAX - len - 1)
                return sh_script_refuse(L, "twitch_response needs a damage name");
            names += len + 1;
        } else {
            rows[i].limit = (float)sh_script_api_check_number(L, -1, "twitch_response", "the upper damage limit");
            if (rows[i].limit < 0 || rows[i].limit <= last)
                return sh_script_refuse(L, "twitch_response damage limits must increase from zero or above");
            last = rows[i].limit;
        }
        lua_pop(L, 1);
        lua_rawgeti(L, -1, 2);
        rows[i].scale = (float)sh_script_api_check_number(L, -1, "twitch_response", "the animation weight multiplier");
        if (rows[i].scale < 0) return sh_script_refuse(L, "twitch_response needs a nonnegative weight multiplier");
        lua_pop(L, 2);
    }
    if (!ai || !CE(twitch_weight) || !(weight = g_ce.twitch_weight(ai))) return bool1(L, 0);
    if (n) {
        bytes = sizeof *p + n * sizeof *rows;
        if (names > SIZE_MAX - bytes || !(p = (twitch_profile *)malloc(bytes + names))) return luaL_error(L, "out of memory");
        p->owner = ai;
        p->weight = weight;
        p->count = n;
        memcpy(p->rows, rows, n * sizeof *rows);
        name = (char *)(p->rows + n);
        for (i = 0; i < n; i++) if (rows[i].decl) {
            size_t len = strlen(rows[i].decl) + 1;
            p->rows[i].decl = name;
            memcpy(name, rows[i].decl, len);
            name += len;
        }
    }
    AcquireSRWLockExclusive(&g_rec_lock);
    v = (hit_val *)(n ? rec_put(&g_hits, h) : rec_get(&g_hits, h));
    if (v) {
        if (v->twitch) *weight = v->twitch->base;
        if (p) p->base = *weight;
        free(v->twitch);
        v->twitch = p;
    }
    ReleaseSRWLockExclusive(&g_rec_lock);
    if (!v && n) { free(p); return luaL_error(L, "out of memory"); }
    return bool1(L, 1);
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

/* ai:can_see_from(other, tag or joint) */
static int l_can_see_from(lua_State *L)
{
    const sh_script_engine *e = eng();
    float from[3], to[3];
    sh_script_handle h = 0;
    sh_script_trace t;
    void *self = sh_script_api_entity(L, 1, "can_see_from", NULL);
    void *other = sh_script_api_entity(L, 2, "can_see_from", &h);
    const char *joint = sh_script_api_check_text(L, 3, "can_see_from", "the tag or joint to look from", SH_SCRIPT_TEXT_ANY);
    if (!self || !other || !e || !e->trace || !e->spawn_id) return nil1(L);
    if (!point_of(self, joint, from, NULL)) return nil1(L);
    origin_of(other, to);
    to[2] += SIGHT_RAISE;
    if (!e->trace(from, to, e->spawn_id(self), &t)) return nil1(L);
    return bool1(L, !(t.fraction < 1.0f) || t.hit_spawn_id == e->spawn_id(other));
}

/* The projectile at arg that one of this mod's entities launched. NULL when it no longer exists (the call answers
 * nil); a refusal only for an existing entity that is not such a projectile. */
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

/* p:gravity(units per second squared): the projectile falls at that rate whatever the map's gravity, so a script
 * that solved its arc under its source game's gravity throws it there. The setGravity event (13954591 event 245)
 * takes a vector straight down; 0 lets it fly unpulled. */
static int l_p_gravity(lua_State *L)
{
    double g = sh_script_api_check_number(L, 2, "gravity", "the gravity in units per second squared");
    float down[3];
    sh_script_earg a;
    void *p;
    if (!(g >= 0)) sh_script_refuse(L, "\"gravity\" needs a gravity of 0 or more, got %g", g);
    p = own_projectile(L, 1, "gravity");
    if (!p) return nil1(L);
    down[0] = 0; down[1] = 0; down[2] = (float)-g;
    a = sh_script_earg_vec(down);
    return bool1(L, sh_script_api_event(p, "setGravity", &a, 1, NULL));
}

/* p:explode() */
static int l_p_explode(lua_State *L)
{
    void *p = own_projectile(L, 1, "explode");
    if (!p) return nil1(L);
    if (!CE(explode)) return bool1(L, 0);
    return bool1(L, g_ce.explode(p));
}

/* The axis (rows forward, left, up) of something at `from` heading for `to`; 0 when they coincide. */
static int aim_axis(const float from[3], const float to[3], float ax[9])
{
    float f[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] }, l[3], u[3];
    if (!normalize(f)) return 0;
    l[0] = -f[1]; l[1] = f[0]; l[2] = 0;             /* up (0 0 1) x forward */
    if (!normalize(l)) { l[0] = 0; l[1] = 1; l[2] = 0; }
    u[0] = f[1] * l[2] - f[2] * l[1];
    u[1] = f[2] * l[0] - f[0] * l[2];
    u[2] = f[0] * l[1] - f[1] * l[0];
    memcpy(ax, f, sizeof f); memcpy(ax + 3, l, sizeof l); memcpy(ax + 6, u, sizeof u);
    return 1;
}

/* How a launch leaves the tag: toward a target (fire), or with a velocity of
 * its own (launch). */
typedef struct launch_aim {
    void *target;           /* fire at an entity: the engine aims at it */
    const float *point;     /* fire at a position: launched at the demon itself, then steered there */
    const float *velocity;  /* launch: the projectile's exact velocity, units per second */
} launch_aim;

static const char *launch_origin(lua_State *L, const char *call, float pos[3])
{
    if (lua_type(L, 3) != LUA_TTABLE)
        return sh_script_api_check_text(L, 3, call, "the tag or joint to launch from", SH_SCRIPT_TEXT_ANY);
    sh_script_api_check_vec(L, 3, call, "the launch position", pos);
    return NULL;
}

/* The launch ai:fire and ai:launch share. The engine's launch event fires from a joint and takes any other name
 * for the demon's origin (VK 0x14076E780), so for a tag the launch hook hands idProjectile::Launch the tag's place
 * and axis and the projectile and its trail start on the tag. Pushes the projectile, or nil. */
static int launch_from(lua_State *L, const char *call, void *self, sh_script_handle self_h, void *decl,
                       const char *joint, const float origin[3], const launch_aim *how)
{
    const sh_script_engine *e = eng();
    sh_script_earg a[5];
    char name[128];
    uint32_t id;
    int from_tag = 0, started_there = 0;
    float tag_pos[3], tag_axis[9], aim[3];
    if (!self || !decl || !CE(entity_name) || (!how->point && !how->velocity && !how->target)) return nil1(L);
    sh_script_api_check_room(L, call);
    if (!g_ce.entity_name(how->target ? how->target : self, name, sizeof name) || !name[0]) return nil1(L);
    if (!joint) memcpy(tag_pos, origin, sizeof tag_pos);
    if (!joint || (e && e->tag && e->tag(self, joint, tag_pos, NULL) == 1)) {
        if (how->velocity) { aim[0] = tag_pos[0] + how->velocity[0]; aim[1] = tag_pos[1] + how->velocity[1]; aim[2] = tag_pos[2] + how->velocity[2]; }
        else if (how->point) memcpy(aim, how->point, sizeof aim);
        else if (!e || !e->bounds_center || !e->bounds_center(how->target, aim)) return nil1(L);
        from_tag = aim_axis(tag_pos, aim, tag_axis);
    }
    (void)sh_script_ai_park(self, self_h);   /* a demon a script fires from is the script's, as one it moves */
    a[0] = sh_script_earg_no_anim();
    memset(&a[1], 0, sizeof a[1]); a[1].type = 'd'; a[1].p = decl;
    memset(&a[2], 0, sizeof a[2]); a[2].type = '2'; a[2].s = joint ? joint : "origin";
    a[3] = sh_script_earg_text('s', name);
    a[4] = sh_script_earg_int(0);
    fire_begin(self_h, from_tag ? tag_pos : NULL, from_tag ? tag_axis : NULL);
    if (!sh_script_api_event(self, "ae_launchProjectileAtTarget", a, 5, NULL)) { (void)fire_end(NULL); return nil1(L); }
    id = fire_end(&started_there);
    if (id == SH_SCRIPT_SPAWN_ID_NONE || !e || !e->resolve) return nil1(L);
    {
        void *p = e->resolve(id);
        if (!p) return nil1(L);
        /* without the launch hook the projectile started at the origin: put it on the tag */
        if (from_tag && !started_there && e->place) (void)e->place(p, tag_pos, tag_axis);
        if (how->velocity) {
            sh_script_earg v = sh_script_earg_vec(how->velocity);
            (void)sh_script_api_event(p, "setLinearVelocity", &v, 1, NULL);
        } else if (from_tag || how->point) {
            steer_to(p, from_tag ? aim : how->point, 0);
        }
        sh_script_api_push_engine_entity(L, p);
    }
    return 1;
}

/* ai:fire(projectile, tag or joint, target) -> p: toward an entity or a
 * position, at the projectile's own speed. */
static int l_fire(lua_State *L)
{
    sh_script_handle self_h = 0;
    float point[3], origin[3];
    launch_aim how = { NULL, NULL, NULL };
    void *self = sh_script_api_owned(L, 1, "fire", "fire from", &self_h);
    void *decl = check_decl(L, 2, "fire", "projectile", "the projectile");
    const char *joint = launch_origin(L, "fire", origin);
    if (luaL_testudata(L, 4, "entity")) {
        how.target = sh_script_api_entity(L, 4, "fire", NULL);
    } else {
        if (lua_type(L, 4) != LUA_TTABLE)
            sh_script_refuse(L, "\"fire\" needs the target (an entity, or a position with x, y and z), got %s",
                             lua_isnoneornil(L, 4) ? "nothing" : luaL_typename(L, 4));
        sh_script_api_check_vec(L, 4, "fire", "the target", point);
        how.point = point;
    }
    if (self && sh_script_api_kind(self) != SH_SCRIPT_KIND_AI)
        sh_script_refuse(L, "\"fire\" works on demons; this entity is not one");
    return launch_from(L, "fire", self, self_h, decl, joint, origin, &how);
}

/* ai:launch(projectile, tag or joint, velocity) -> p: the projectile leaves
 * with exactly that velocity (units per second), so a thrown projectile can
 * fly an arc its script solved. */
static int l_launch(lua_State *L)
{
    sh_script_handle self_h = 0;
    float vel[3], origin[3];
    launch_aim how = { NULL, NULL, NULL };
    void *self = sh_script_api_owned(L, 1, "launch", "launch from", &self_h);
    void *decl = check_decl(L, 2, "launch", "projectile", "the projectile");
    const char *joint = launch_origin(L, "launch", origin);
    if (lua_type(L, 4) != LUA_TTABLE)
        sh_script_refuse(L, "\"launch\" needs the velocity (x, y and z in units per second), got %s",
                         lua_isnoneornil(L, 4) ? "nothing" : luaL_typename(L, 4));
    sh_script_api_check_vec(L, 4, "launch", "the velocity", vel);
    if (!(vlen(vel) > 0)) sh_script_refuse(L, "\"launch\" needs a velocity that is not zero");
    if (self && sh_script_api_kind(self) != SH_SCRIPT_KIND_AI)
        sh_script_refuse(L, "\"launch\" works on demons; this entity is not one");
    how.velocity = vel;
    return launch_from(L, "launch", self, self_h, decl, joint, origin, &how);
}

typedef struct incoming_hit { void *p; double seconds; } incoming_hit;

static int incoming_cmp(const void *a, const void *b)
{
    double x = ((const incoming_hit *)a)->seconds, y = ((const incoming_hit *)b)->seconds;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Where a ray from `o` along unit `d` enters and leaves a box (center c, axis
 * rows forward, left, up, half sizes h): 1 with both distances, 0 when it
 * misses. */
int sh_script_combat_ray_box(const float o[3], const float d[3], const float c[3], const float ax[9],
                             const float h[3], double *enter, double *leave)
{
    double t0 = -1e30, t1 = 1e30;
    int k;
    for (k = 0; k < 3; k++) {
        const float *u = ax + 3 * k;
        double p = (double)u[0] * (c[0] - o[0]) + (double)u[1] * (c[1] - o[1]) + (double)u[2] * (c[2] - o[2]);
        double s = (double)u[0] * d[0] + (double)u[1] * d[1] + (double)u[2] * d[2];
        if (fabs(s) < 1e-9) {
            if (fabs(p) > h[k]) return 0;   /* parallel to this slab and outside it */
        } else {
            double a = (p - h[k]) / s, b = (p + h[k]) / s, t;
            if (a > b) { t = a; a = b; b = t; }
            if (a > t0) t0 = a;
            if (b < t1) t1 = b;
            if (t0 > t1) return 0;
        }
    }
    *enter = t0;
    *leave = t1;
    return 1;
}

/* e:incoming_projectiles(size) -> { {projectile = p, seconds = t}, ... }, soonest first: projectiles not launched by
 * `e` whose line (velocity less e's) enters a box of `size` on e's origin, turned with it; `seconds` is the distance
 * to the box's center over the relative speed. One already inside or past the box is not incoming. */
static int l_incoming(lua_State *L)
{
    const sh_script_engine *e = eng();
    sh_script_handle self_h = 0, owner;
    float size[3], h[3], c[3], ax[9], ev[3] = { 0, 0, 0 };
    sh_script_earg r;
    incoming_hit *hits = NULL, *grown;
    int n = 0, cap = 0, slots, num, i, buf;
    void *self = sh_script_api_entity(L, 1, "incoming_projectiles", &self_h);
    if (lua_type(L, 2) != LUA_TTABLE)
        sh_script_refuse(L, "\"incoming_projectiles\" needs the box's size (x, y and z in units), got %s",
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    sh_script_api_check_vec(L, 2, "incoming_projectiles", "the box's size", size);
    if (!(size[0] > 0 && size[1] > 0 && size[2] > 0))
        sh_script_refuse(L, "\"incoming_projectiles\" needs a box size above 0 on x, y and z");
    lua_pushnil(L);                         /* the hits, as a userdata the collector frees on any exit */
    buf = lua_gettop(L);
    lua_newtable(L);
    if (!self || !e || !e->slots || !e->slot_spawn_id || !e->resolve || !e->origin || !e->axis || !CE(is_projectile))
        return 1;
    if (!e->origin(self, c) || !e->axis(self, ax)) return 1;
    h[0] = size[0] * 0.5f; h[1] = size[1] * 0.5f; h[2] = size[2] * 0.5f;
    for (i = 0; i < 3; i++) c[i] += ax[6 + i] * h[2];   /* the box stands on the origin */
    if (sh_script_api_event(self, "getLinearVelocity", NULL, 0, &r) && r.type == 'v') memcpy(ev, r.v, sizeof ev);
    slots = e->slots();
    for (num = 0; num < slots; num++) {
        uint32_t id = e->slot_spawn_id(num);
        void *p;
        float o[3], d[3];
        double speed, enter, leave, dist;
        if (id == SH_SCRIPT_SPAWN_ID_NONE || id == SH_SCRIPT_SPAWN_ID_WORLD) continue;
        if (!(p = e->resolve(id)) || p == self || sh_script_api_kind(p) != SH_SCRIPT_KIND_OTHER || !g_ce.is_projectile(p))
            continue;
        if (self_h && sh_script_combat_launched(id, NULL, &owner) && owner == self_h) continue;
        if (!e->origin(p, o) || !sh_script_api_event(p, "getLinearVelocity", NULL, 0, &r) || r.type != 'v') continue;
        d[0] = r.v[0] - ev[0]; d[1] = r.v[1] - ev[1]; d[2] = r.v[2] - ev[2];
        speed = vlen(d);
        if (!(speed > 0) || !normalize(d)) continue;
        if (!sh_script_combat_ray_box(o, d, c, ax, h, &enter, &leave) || enter < 0 || leave < 0) continue;
        dist = sqrt(((double)c[0] - o[0]) * (c[0] - o[0]) + ((double)c[1] - o[1]) * (c[1] - o[1]) +
                    ((double)c[2] - o[2]) * (c[2] - o[2]));
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            grown = (incoming_hit *)lua_newuserdatauv(L, (size_t)cap * sizeof *grown, 0);
            if (n) memcpy(grown, hits, (size_t)n * sizeof *grown);
            lua_replace(L, buf);
            hits = grown;
        }
        hits[n].p = p;
        hits[n].seconds = dist / speed;
        n++;
    }
    if (n) qsort(hits, (size_t)n, sizeof hits[0], incoming_cmp);
    for (i = 0; i < n; i++) {
        lua_createtable(L, 0, 2);
        sh_script_api_push_engine_entity(L, hits[i].p);
        lua_setfield(L, -2, "projectile");
        lua_pushnumber(L, hits[i].seconds);
        lua_setfield(L, -2, "seconds");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

void sh_script_combat_register(void)
{
    static int done;
    static const struct { int kind; const char *name; lua_CFunction fn; } calls[] = {
        { SH_SCRIPT_API_GLOBAL, "hitscan", l_hitscan },
        { SH_SCRIPT_API_GLOBAL, "radius_damage", l_radius_damage },
        { SH_SCRIPT_API_GLOBAL, "damage", l_damage },
        { SH_SCRIPT_API_GLOBAL, "explode", l_explode },
        { SH_SCRIPT_API_GLOBAL, "play_effect", l_fx },
        { SH_SCRIPT_API_GLOBAL, "move_effect", l_fx_move },
        { SH_SCRIPT_API_GLOBAL, "stop_effect", l_fx_stop },
        { SH_SCRIPT_API_GLOBAL, "end_effects_at_once", l_fx_hard_stops },
        { SH_SCRIPT_API_GLOBAL, "sound", l_sound },
        { SH_SCRIPT_API_METHOD, "damage", l_damage },
        { SH_SCRIPT_API_METHOD, "tag", l_tag },
        { SH_SCRIPT_API_METHOD, "draw_ribbon", l_draw_ribbon },
        { SH_SCRIPT_API_METHOD, "pain", l_pain },
        { SH_SCRIPT_API_METHOD, "twitch_response", l_twitch_response },
        { SH_SCRIPT_API_METHOD, "can_see_from", l_can_see_from },
        { SH_SCRIPT_API_METHOD, "fire", l_fire },
        { SH_SCRIPT_API_METHOD, "launch", l_launch },
        { SH_SCRIPT_API_METHOD, "incoming_projectiles", l_incoming },
        { SH_SCRIPT_API_METHOD, "steer", l_steer },
        { SH_SCRIPT_API_METHOD, "gravity", l_p_gravity },
        { SH_SCRIPT_API_METHOD, "explode", l_p_explode },
    };
    int i;
    if (done) return;
    done = 1;
    for (i = 0; i < (int)(sizeof calls / sizeof calls[0]); i++) sh_script_api_add(calls[i].kind, calls[i].name, calls[i].fn);
}

#ifndef SH_SCRIPT_PURE /* the engine side */

#include "../backend_log.h"
#include "../feature_health.h"
#include "../entity.h"
#include "../engine_build.h"
#include "../hook.h"
#include "script_api_reflect.h"

#define DECL_STATE     0x2Cu       /* bit 0 failed to load, bit 1 not loaded */
#define FX_MAX         1024

typedef void  (*radius_fn)(void *game, const float *origin, void *inflictor, void *attacker, const void *parms,
                           float scale, unsigned char flag, void *ignore);
typedef void  (*explode_fn)(void *projectile, void *ignore, const void *collision);
typedef void *(*type_fn)(void *registry, const char *type);
typedef void *(*find_decl_fn)(void *manager, const char *name, unsigned char make_default);
typedef void  (*ent_fx_fn)(void *entity, int condition, int extra_condition);
typedef void  (*actor_damage_fn)(void *actor, void *inflictor, void *attacker, void *parms, float scale,
                                  const float *dir, void *hit);

static const uint8_t *g_base;
static size_t        g_text_lo, g_text_hi;   /* the image's executable range, RVAs */
static radius_fn     g_radius;
static ent_fx_fn     g_ent_start_fx, g_ent_stop_fx;   /* idEntity StartFX / StopFX */
static actor_damage_fn g_player_damage;               /* idPlayer::Damage */
static actor_damage_fn g_ai_damage;                   /* idAI2's Damage (its vtable slot's function) */
static const uint8_t *g_decl_anchor;
static void         *g_decl_register;
static type_fn       g_decl_type;
static find_decl_fn  g_decl_find;
static uintptr_t     g_vt_ai;
static const uint8_t *g_fx_table;
static int           g_fx_count;
static char *g_log_last;   /* the last line logged, malloc'd */

static void combat_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (!line) return;
    /* the same line again says nothing new */
    if (g_log_last && !strcmp(line, g_log_last)) { free(line); return; }
    backend_log(line);
    free(g_log_last);
    g_log_last = line;
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

void *sh_script_combat_decl(const char *type, const char *name)
{
    return g_decl_find ? c_decl(type, name) : NULL;
}

/* The victim's own Damage (vtable slot SH_COMBAT_DAMAGE_VSLOT, the same in every actor class) when it is the
 * player's or idAI2's as this executable has them. Both take (actor, inflictor, attacker, parms, scale, direction,
 * hit); idAI2's body copies hits only from a non-NULL hit (VK 0x1403F8C40), and Damage_Calculate (VK 0x1403FB1D0)
 * gives a NULL hit the engine's own no-location case. 1 when the call returned; whether it landed is the health
 * the caller measures (damage_landed). */
static int c_actor_damage(void *victim, void *decl, void *inflictor, void *attacker, const float dir[3])
{
    const uint8_t *parms = (const uint8_t *)decl + SH_COMBAT_DECL_PARMS;
    void *slot = NULL;
    actor_damage_fn fn;
    float d[3];
    int ok = 0;
    if ((!g_player_damage && !g_ai_damage) || !victim || !decl || !inflictor) return 0;
    __try { slot = (*(void ***)victim)[SH_COMBAT_DAMAGE_VSLOT]; }
    __except (av_only(GetExceptionCode())) { slot = NULL; }
    if (!slot || (slot != (void *)g_player_damage && slot != (void *)g_ai_damage)) return 0;
    fn = (actor_damage_fn)slot;
    /* the call takes the decl's parms, which name their own decl (as radius damage checks) */
    if (read_ptr(parms, SH_COMBAT_PARMS_DECL) != decl) return 0;
    memcpy(d, dir, sizeof d);
    __try { fn(victim, inflictor, attacker, (void *)parms, 1.0f, d, NULL); ok = 1; }
    __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
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

/* The entity is an idProjectile, by the game's own type information (every
 * projectile class descends from it). */
static int c_is_projectile(void *ent)
{
    const char *cls = ent ? sh_script_reflect_class_of(ent) : NULL;
    return cls && sh_script_reflect_is_a(cls, "idProjectile");
}

static int c_draw_ribbon(void *ent, int time_ms, const float (*points)[3], size_t count, const float rgba[4])
{
    typedef void (*release_fn)(void *);
    typedef void (*begin_fn)(void *, int, const float *);
    typedef void (*add_fn)(void *, int, const float *, const float *, const float *, const float *);
    typedef void (*build_fn)(void *, int, const float *);
    static const float identity[9] = {1,0,0, 0,1,0, 0,0,1}, zero[3] = {0,0,0};
    const char *cls = sh_script_reflect_class_of(ent);
    uint32_t release = SH_RVA(0x1717c30, 0x1718690);
    uint32_t begin = SH_RVA(0x1717bd0, 0x1718630);
    uint32_t add = SH_RVA(0x1717c70, 0x17186d0);
    uint32_t build = SH_RVA(0x1717c40, 0x17186a0);
    unsigned char *manager;
    size_t i;
    if (!g_base || !release || !begin || !add || !build || !cls || strcmp(cls, "idRibbonEmitter")) return 0;
    manager = (unsigned char *)ent + 0x1140;
    if (!read_ptr(manager, 8) || !read_ptr(manager, 16) || !read_ptr(manager, 24)) return 0;
    /* The stock laser rebuilds its current centre line this way on both supported renderers. */
    ((release_fn)(g_base + release))(manager);
    ((begin_fn)(g_base + begin))(manager, time_ms, points[0]);
    if (!read_ptr(manager, 0)) return 0;
    for (i = 0; i < count; i++)
        ((add_fn)(g_base + add))(manager, time_ms, points[i], identity, zero, rgba);
    ((build_fn)(g_base + build))(manager, time_ms, identity);
    return 1;
}

static int c_pain_reaction(void *ai)
{
    int r = -1;
    if (!ai || !g_vt_ai || (uintptr_t)read_ptr(ai, 0) != g_vt_ai) return -1;
    __try { r = *(const int *)((const uint8_t *)ai + SH_COMBAT_AI_PAIN); } __except (av_only(GetExceptionCode())) { r = -1; }
    return r;
}

static float *c_twitch_weight(void *ai)
{
    const sh_rf_host *host = sh_script_reflect_host();
    sh_rf_loc slot = {0};
    char err[128];
    float *weight = NULL;
    if (!ai || !g_vt_ai || (uintptr_t)read_ptr(ai, 0) != g_vt_ai || !host) return NULL;
    if (sh_rf_resolve(host, ai, "idAI2", "aiVolatile.animation.painAnimator", &slot, err, sizeof err)) {
        /* The native pain animator copies its maximum weight into the new blend when a twitch starts. */
        if (slot.size == 248) weight = (float *)(slot.addr + 0x98);
        sh_rf_loc_free(&slot);
    }
    return weight;
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

static int c_start_fx(void *ent, int condition)
{
    int ok = 0;
    if (!ent || !g_ent_start_fx) return 0;
    __try { g_ent_start_fx(ent, condition, 0); ok = 1; } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int c_stop_fx(void *ent, int condition)
{
    int ok = 0;
    if (!ent || !g_ent_stop_fx) return 0;
    __try { g_ent_stop_fx(ent, condition, 0); ok = 1; } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

/* The FX manager's stop of one action. For an action of a decl named with end_effects_at_once that the rule ends
 * at once, the fading bit is set first, so the game's own stop skips its fade branch and clears the action. */
typedef void (*fx_action_stop_fn)(void *mgr, int index, int now, unsigned char immediate, unsigned char recycle);
static fx_action_stop_fn g_fx_stop_target, g_fx_stop_orig;
static int g_fx_stop_failed;

static void fx_action_stop_detour(void *mgr, int index, int now, unsigned char immediate, unsigned char recycle)
{
    if (!immediate && g_fx_hard_n && mgr && index >= 0) {
        __try {
            const uint8_t *decl = *(const uint8_t * const *)((const uint8_t *)mgr + SH_FX_MGR_DECL);
            uint8_t *acts = *(uint8_t * const *)((const uint8_t *)mgr + SH_FX_MGR_ACTIONS);
            if (decl && acts && index < *(const int *)(decl + SH_FX_DECL_COUNT)
                && sh_fx_hard_is(*(const char * const *)(decl + SH_COMBAT_DECL_NAME))) {
                const uint8_t *item = *(const uint8_t * const *)(decl + SH_FX_DECL_ITEMS) + (size_t)index * SH_FX_ITEM_SIZE;
                uint8_t *act = acts + (size_t)index * SH_FX_ACTION_SIZE;
                uint32_t flags = *(const uint32_t *)(act + SH_FX_ACTION_FLAGS);
                if ((flags & SH_FX_ACTION_REQUESTED) && !(flags & SH_FX_ACTION_FADING)) {
                    int start = *(const int *)(act + SH_FX_ACTION_DELAY) + *(const int *)(act + SH_FX_ACTION_REQUEST);
                    if (sh_fx_hard_ends(*(const int *)(item + SH_FX_ITEM_TYPE), item[SH_FX_ITEM_SCREEN_PRT] != 0, now < start))
                        *(uint32_t *)(act + SH_FX_ACTION_FLAGS) = flags | SH_FX_ACTION_FADING;
                }
            }
        } __except (av_only(GetExceptionCode())) { }
    }
    g_fx_stop_orig(mgr, index, now, immediate, recycle);
}

/* The FX manager's request of one action, eight arguments forwarded unchanged. A request of a sound item of a
 * decl named with end_effects_at_once whose sound is playing is not passed on (sh_fx_sound_held): the game would
 * start a second sound over the first and never stop it. */
typedef void (*fx_action_request_fn)(void *mgr, intptr_t index, void *a3, void *a4, intptr_t a5, intptr_t a6,
                                     void *a7, intptr_t a8);
static fx_action_request_fn g_fx_request_target, g_fx_request_orig;
static int g_fx_request_failed;

static void fx_action_request_detour(void *mgr, intptr_t index, void *a3, void *a4, intptr_t a5, intptr_t a6,
                                     void *a7, intptr_t a8)
{
    int i = (int)index;
    if (g_fx_hard_n && mgr && i >= 0) {
        int held = 0;
        __try {
            const uint8_t *decl = *(const uint8_t * const *)((const uint8_t *)mgr + SH_FX_MGR_DECL);
            const uint8_t *acts = *(const uint8_t * const *)((const uint8_t *)mgr + SH_FX_MGR_ACTIONS);
            if (decl && acts && i < *(const int *)(decl + SH_FX_DECL_COUNT)) {
                const uint8_t *item = *(const uint8_t * const *)(decl + SH_FX_DECL_ITEMS) + (size_t)i * SH_FX_ITEM_SIZE;
                const uint8_t *act = acts + (size_t)i * SH_FX_ACTION_SIZE;
                if (sh_fx_sound_held(*(const int *)(item + SH_FX_ITEM_TYPE), *(const uint32_t *)(act + SH_FX_ACTION_FLAGS),
                                     item[SH_FX_ITEM_RESTART])
                    && sh_fx_hard_is(*(const char * const *)(decl + SH_COMBAT_DECL_NAME)))
                    held = 1;
            }
        } __except (av_only(GetExceptionCode())) { held = 0; }
        if (held) return;
    }
    g_fx_request_orig(mgr, index, a3, a4, a5, a6, a7, a8);
}

/* The request hook, once per session; a failure leaves the stop hook as it is. */
static int fx_request_hook(void)
{
    void *tramp;
    if (g_fx_request_orig && hook_is_installed((void *)g_fx_request_orig)) return 1;
    if (g_fx_request_failed) return 0;
    if (!g_fx_request_target) {
        g_fx_request_failed = 1;
        combat_log("Mod scripts: end_effects_at_once leaves sound requests to the game in this session: the game function "
                   "it needs was not found in this executable.");
        return 0;
    }
    if (memcmp((const void *)g_fx_request_target, k_combat_fx_request_prologue, SH_COMBAT_FX_REQUEST_STOLEN)) {
        g_fx_request_failed = 1;
        combat_log("Mod scripts: end_effects_at_once leaves sound requests to the game in this session: the game function "
                   "it needs is not the expected one (another tool may have changed it).");
        return 0;
    }
    tramp = hook_prepare((void *)g_fx_request_target, (void *)fx_action_request_detour, SH_COMBAT_FX_REQUEST_STOLEN);
    if (!tramp) {
        g_fx_request_failed = 1;
        combat_log("Mod scripts: end_effects_at_once leaves sound requests to the game in this session; the hook could not "
                   "be prepared (%s).", hook_last_failure());
        return 0;
    }
    g_fx_request_orig = (fx_action_request_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_fx_request_orig = NULL;
        g_fx_request_failed = 1;
        combat_log("Mod scripts: end_effects_at_once leaves sound requests to the game in this session; the hook could not "
                   "be installed.");
        return 0;
    }
    combat_log("SCRIPTS: end_effects_at_once request hook installed at %p", (void *)g_fx_request_target);
    return 1;
}

/* The stop hook, once per session. */
static int fx_stop_hook(void)
{
    void *tramp;
    if (g_fx_stop_orig && hook_is_installed((void *)g_fx_stop_orig)) return 1;
    if (g_fx_stop_failed || !g_fx_stop_target) return 0;
    if (memcmp((const void *)g_fx_stop_target, k_combat_fx_stop_prologue, SH_COMBAT_FX_STOP_STOLEN)) {
        g_fx_stop_failed = 1;
        combat_log("Mod scripts: end_effects_at_once is unavailable in this session: the game function it needs is not the "
                   "expected one (another tool may have changed it).");
        return 0;
    }
    tramp = hook_prepare((void *)g_fx_stop_target, (void *)fx_action_stop_detour, SH_COMBAT_FX_STOP_STOLEN);
    if (!tramp) {
        g_fx_stop_failed = 1;
        combat_log("Mod scripts: end_effects_at_once is unavailable in this session; the hook could not be prepared (%s).",
                   hook_last_failure());
        return 0;
    }
    g_fx_stop_orig = (fx_action_stop_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_fx_stop_orig = NULL;
        g_fx_stop_failed = 1;
        combat_log("Mod scripts: end_effects_at_once is unavailable in this session; the hook could not be installed.");
        return 0;
    }
    combat_log("SCRIPTS: end_effects_at_once hook installed at %p", (void *)g_fx_stop_target);
    return 1;
}

static int c_fx_hard_stops(void)
{
    int stop = fx_stop_hook();
    int request = fx_request_hook();
    return stop || request;
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
        c_decl, c_radius_damage, c_explode, c_pain_reaction, c_entity_name, c_fx_condition, c_world,
        c_start_fx, c_stop_fx, c_actor_damage, c_fx_hard_stops, c_is_projectile, c_draw_ribbon, c_twitch_weight
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
    if (sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_PLAYER_DAMAGE], &r) == SIG_OK)
        g_player_damage = (actor_damage_fn)r.addr;      /* without it a player takes the damage event */
    if (sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_AI_DAMAGE], &r) == SIG_OK)
        g_ai_damage = (actor_damage_fn)r.addr;          /* without it a demon takes the damage event */
    if (sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_ENT_START_FX], &r) == SIG_OK)
        g_ent_start_fx = (ent_fx_fn)r.addr;
    if (sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_ENT_STOP_FX], &r) == SIG_OK)
        g_ent_stop_fx = (ent_fx_fn)r.addr;
    if (!g_ent_start_fx || !g_ent_stop_fx)
        combat_off("the game's entity effect calls were not found in this executable, so play_effect(e, condition) and "
                   "stop_effect(e, condition) do nothing on an effect entity");
    if (sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_FX_ACTION_STOP], &r) == SIG_OK)
        g_fx_stop_target = (fx_action_stop_fn)r.addr;   /* without it and the request end_effects_at_once answers false */
    if (sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_FX_ACTION_REQUEST], &r) == SIG_OK)
        g_fx_request_target = (fx_action_request_fn)r.addr;   /* without it sound requests stay the game's */
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
        combat_off("the game's effect conditions were not found in this executable, so play_effect(e, condition) refuses "
                   "every condition");
    }
    sh_script_combat_set_engine(&engine);
    sh_script_combat_register();
    backend_log("SCRIPTS: combat calls resolved");
    return 1;
}

#endif /* SH_SCRIPT_PURE */
