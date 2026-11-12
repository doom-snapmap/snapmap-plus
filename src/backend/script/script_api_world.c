/* script_api_world.c -- the world calls of mod scripts (API version 1):
 *   entities_near(pos, radius) -> the entities within radius, nearest first
 *   trace(from, to, [radius], [ignore]) -> what a line (or a thick line) from
 *                                          `from` to `to` hits, or nil
 *   nav_ground(pos) -> the point on the ground under pos, or nil
 *   difficulty() -> the game's difficulty, 0 easy .. 4 nightmare
 * `pos`, `from` and `to` are positions or entities (their positions). These
 * calls only read, so they work on any entity.
 *
 * Engine paths (script_api_common.h):
 *   entities_near   the spawn ids of every entity number (idGameLocal +0x653F8), each resolved through
 *                   the game's spawn-id lookup and measured from its physics origin
 *   trace           the game's trace query run at once (the SnapMap laser hazard's call shape: a point
 *                   trace against the world, players and demons, contents 0x103085). With a radius the
 *                   line is thick: nine parallel traces, one on the line and eight around it at that
 *                   radius, and the nearest hit is the answer. With "demon" as the fifth value the
 *                   line is also stopped by what stops a demon and not a shot: a Blocking Box with
 *                   Block Demons ticked (its contents gain 0x20010), so a script that places or flies
 *                   a demon itself honours the map's blocking volumes.
 *   nav_ground      the same trace straight down from 16 inches above pos for 4,096 inches; the first
 *                   surface hit counts when it is flat enough to stand on (normal at least 0.7 up),
 *                   sh_script_api_ground_trace. It does not consult the navigation areas.
 *   difficulty      idGameLocal vtable +0x670, the game's own difficulty (the g_gameDifficulty cvar when
 *                   set, else the game's current difficulty), VK 0x140391A80 / GL 0x140391350.
 * The time and log calls (now, after, every, run, wait, print, warn) are the
 * runtime's (script_lib_base.c). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "script_api_common.h"

#define TRACE_RIM       8          /* rays around the centre line of a thick trace */
#define PI              3.14159265358979323846

typedef struct near_hit { void *ent; double d2; } near_hit;

static int near_cmp(const void *a, const void *b)
{
    double x = ((const near_hit *)a)->d2, y = ((const near_hit *)b)->d2;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int l_entities_near(lua_State *L)
{
    const sh_script_engine *e = sh_script_api_engine();
    float c[3], o[3];
    double r = 0, r2;
    near_hit *hits = NULL, *grown;
    int n = 0, cap = 0, num, slots, i, buf;
    int have = sh_script_api_check_point(L, 1, "entities_near", "the center", c);
    r = sh_script_api_check_number(L, 2, "entities_near", "the radius");
    if (!(r >= 0))
        sh_script_refuse(L, "\"entities_near\" needs a radius of 0 or more, got %g", r);
    lua_pushnil(L);                         /* the hits, as a userdata the collector frees on any exit */
    buf = lua_gettop(L);
    lua_newtable(L);
    if (!have || !e || !e->slots || !e->slot_spawn_id || !e->resolve || !e->origin) return 1;
    r2 = r * r;
    slots = e->slots();
    for (num = 0; num < slots; num++) {
        uint32_t id = e->slot_spawn_id(num);
        void *ent;
        double dx, dy, dz, d2;
        if (id == SH_SCRIPT_SPAWN_ID_NONE || id == SH_SCRIPT_SPAWN_ID_WORLD) continue;
        ent = e->resolve(id);
        if (!ent || !e->origin(ent, o)) continue;
        dx = (double)o[0] - c[0]; dy = (double)o[1] - c[1]; dz = (double)o[2] - c[2];
        d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) continue;
        if (n == cap) {                     /* every entity in range: the list grows */
            cap = cap ? cap * 2 : 64;
            grown = (near_hit *)lua_newuserdatauv(L, (size_t)cap * sizeof *grown, 0);
            if (n) memcpy(grown, hits, (size_t)n * sizeof *grown);
            lua_replace(L, buf);
            hits = grown;
        }
        hits[n].ent = ent;
        hits[n].d2 = d2;
        n++;
    }
    if (n) qsort(hits, (size_t)n, sizeof hits[0], near_cmp);
    for (i = 0, num = 0; i < n; i++) {
        sh_script_handle h = sh_script_api_handle_of(hits[i].ent);
        if (!h) continue;          /* no memory to record it */
        sh_script_push_entity(L, h);
        lua_rawseti(L, -2, ++num);
    }
    return 1;
}

static void cross(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int l_trace(lua_State *L)
{
    const sh_script_engine *e = sh_script_api_engine();
    float from[3], to[3];
    double radius = 0;
    uint32_t ignore = SH_SCRIPT_SPAWN_ID_NONE;
    sh_script_trace best, t;
    int have_from, have_to, rays, k, have = 0, as_demon = 0;
    double dir[3], u[3], v[3], len, up[3] = { 0, 0, 1 }, side[3] = { 1, 0, 0 };
    have_from = sh_script_api_check_point(L, 1, "trace", "where the line starts", from);
    have_to = sh_script_api_check_point(L, 2, "trace", "where the line ends", to);
    if (sh_script_api_opt_number(L, 3, "trace", "the radius", &radius) && !(radius >= 0))
        sh_script_refuse(L, "\"trace\" needs a radius of 0 or more, got %g", radius);
    if (!lua_isnoneornil(L, 4)) {
        void *ent = sh_script_api_entity(L, 4, "trace", NULL);
        if (ent && e && e->spawn_id) ignore = e->spawn_id(ent);
    } else if (luaL_testudata(L, 1, "entity")) {
        void *ent = sh_script_api_entity(L, 1, "trace", NULL);   /* a line from an entity does not hit it */
        if (ent && e && e->spawn_id) ignore = e->spawn_id(ent);
    }
    if (!lua_isnoneornil(L, 5)) {
        const char *as = lua_type(L, 5) == LUA_TSTRING ? lua_tostring(L, 5) : NULL;
        if (!as || strcmp(as, "demon"))
            sh_script_refuse(L, "\"trace\" takes \"demon\" as its fifth value (a line stopped by what stops a demon), "
                                "or nothing");
        as_demon = 1;
    }
    if (!have_from || !have_to || !e || !e->trace) { lua_pushnil(L); return 1; }
    dir[0] = (double)to[0] - from[0]; dir[1] = (double)to[1] - from[1]; dir[2] = (double)to[2] - from[2];
    len = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    rays = radius > 0 && len > 1e-6 ? 1 + TRACE_RIM : 1;
    if (rays > 1) {
        for (k = 0; k < 3; k++) dir[k] /= len;
        cross(dir, fabs(dir[2]) < 0.9 ? up : side, u);
        len = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (k = 0; k < 3; k++) u[k] /= len;
        cross(dir, u, v);
    }
    memset(&best, 0, sizeof best);
    best.fraction = 1.0f;
    for (k = 0; k < rays; k++) {
        float a[3], b[3];
        int j;
        double off[3] = { 0, 0, 0 };
        if (k > 0) {
            double ang = (k - 1) * (2.0 * PI / TRACE_RIM);
            for (j = 0; j < 3; j++) off[j] = radius * (cos(ang) * u[j] + sin(ang) * v[j]);
        }
        for (j = 0; j < 3; j++) { a[j] = (float)(from[j] + off[j]); b[j] = (float)(to[j] + off[j]); }
        if (!(as_demon && e->trace_as_demon ? e->trace_as_demon(a, b, ignore, &t) : e->trace(a, b, ignore, &t))) continue;
        have = 1;
        if (t.fraction < best.fraction) {
            best = t;
            for (j = 0; j < 3; j++) best.end[j] = from[j] + (to[j] - from[j]) * t.fraction;   /* on the line itself */
        }
    }
    if (!have || !(best.fraction < 1.0f)) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 4);
    sh_script_api_push_vec(L, best.end);   lua_setfield(L, -2, "pos");
    sh_script_api_push_vec(L, best.normal); lua_setfield(L, -2, "normal");
    lua_pushnumber(L, best.fraction);       lua_setfield(L, -2, "fraction");
    if (best.hit_spawn_id != SH_SCRIPT_SPAWN_ID_NONE && best.hit_spawn_id != SH_SCRIPT_SPAWN_ID_WORLD && e->resolve) {
        sh_script_api_push_engine_entity(L, e->resolve(best.hit_spawn_id));
        lua_setfield(L, -2, "entity");
    }
    return 1;
}

/* nav_ground(pos) -> the point on the ground under pos, or nil */
static int l_nav_ground(lua_State *L)
{
    const sh_script_engine *e = sh_script_api_engine();
    float pos[3], at[3];
    if (!sh_script_api_check_point(L, 1, "nav_ground", "the position to look under", pos) || !e || !e->ground ||
        !e->ground(pos, at)) {
        lua_pushnil(L);
        return 1;
    }
    sh_script_api_push_vec(L, at);
    return 1;
}

/* difficulty() -> 0 easy, 1 medium, 2 hard, 3 ultra-violent, 4 nightmare;
 * nil when the game's difficulty cannot be read */
static int l_difficulty(lua_State *L)
{
    const sh_script_engine *e = sh_script_api_engine();
    int level = 0;
    if (lua_gettop(L) > 0) sh_script_refuse(L, "\"difficulty\" takes no values; call it as difficulty()");
    if (!e || !e->difficulty || !e->difficulty(&level)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, level);
    return 1;
}

void sh_script_api_world_register(void)
{
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "entities_near", l_entities_near);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "trace", l_trace);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "nav_ground", l_nav_ground);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "difficulty", l_difficulty);
}
