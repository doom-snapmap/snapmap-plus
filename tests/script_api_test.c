/* script_api_test.c -- the entity, animation, world and time calls of mod
 * scripts (src/backend/script/script_api_*.c) against a fake engine: an
 * in-memory world of entities (an AI of the mod, a stock AI, a player, a
 * prop of the mod, a wall), the runtime and the hooks' handle table as the
 * game uses them.
 *
 * Covers: every read on own and foreign entities; every write on the mod's
 * own entities and its refusal on another's, with the plain stop line naming
 * the file and line; the player heal exception; argument refusals worded for
 * an author; removed entities answer nil; spawn and its two caps; players();
 * entities_near order and radius; trace hits, misses, the thick line and the
 * ignored entity; animation requests on an AI (the ssaction events and the
 * subweb a bare state name takes) and on other entities (the stock events);
 * anim_current, anim_event and anim_finished from the node observed each
 * frame; view_angle_to on a player's aim and an entity's facing; now() is
 * the time the tick passes. */
#include "heap_out.h"
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script/script_runtime.h"
#include "script/script_hooks.h"
#include "script/script_api_common.h"
#include "script/script_api_ai.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define SALT "apitest1"

/* ------------------------------------------------------------ the source */

static char g_text[16384];

static int src_list(void *ctx, const char *salt, sh_script_file_fn fn, void *fnctx)
{
    (void)ctx;
    if (strcmp(salt, SALT)) return 0;
    fn(fnctx, "main.lua");
    return 1;
}

static int src_read(void *ctx, const char *salt, const char *rel, char **text, size_t *len)
{
    (void)ctx;
    if (strcmp(salt, SALT) || _stricmp(rel, "main.lua")) return 0;
    *len = strlen(g_text);
    *text = (char *)malloc(*len + 1);
    memcpy(*text, g_text, *len + 1);
    return 1;
}

static void src_release(void *ctx, char *text) { (void)ctx; free(text); }
static const sh_script_source g_src = { NULL, src_list, src_read, src_release };

/* ------------------------------------------------------------- the world */

typedef struct fent {
    uint32_t id;
    int      kind;
    int      alive;          /* 0: removed (the lookup no longer finds it) */
    float    pos[3], ax[9], vel[3];
    float    cur, max;
    char     def[128];
    char     node[2048];
    float    view[3];        /* a player's pitch, yaw, roll */
} fent;

#define ENTS 1024
static fent g_ents[ENTS];
static int g_nents;
static int g_ms;
static char g_events[8192];       /* "name(args);" of every event, in order */
static float g_wall_x = 100.0f;   /* the trace's world: a wall at x = 100 */
static uint32_t g_wall_id;         /* the entity standing in the wall, or NONE for the world */

enum { E_AI = 0, E_STOCK_AI, E_PLAYER, E_PROP, E_FAR, E_WALL };

static void yaw_axis(float yaw, float ax[9])
{
    double r = yaw * 3.14159265358979323846 / 180.0;
    memset(ax, 0, 9 * sizeof(float));
    ax[0] = (float)cos(r); ax[1] = (float)sin(r);
    ax[3] = -(float)sin(r); ax[4] = (float)cos(r);
    ax[8] = 1;
}

static fent *add_ent(int kind, const char *def, float x, float y, float z)
{
    fent *e = &g_ents[g_nents];
    memset(e, 0, sizeof *e);
    e->id = 0x10000u + (uint32_t)g_nents * 7u;
    e->kind = kind;
    e->alive = 1;
    e->pos[0] = x; e->pos[1] = y; e->pos[2] = z;
    yaw_axis(0, e->ax);
    strcpy_s(e->def, sizeof e->def, def);
    g_nents++;
    return e;
}

static fent *by_id(uint32_t id)
{
    int i;
    for (i = 0; i < g_nents; i++) if (g_ents[i].id == id && g_ents[i].alive) return &g_ents[i];
    return NULL;
}

static void *f_resolve(uint32_t id) { return by_id(id); }
static uint32_t f_spawn_id(void *ent) { return ent ? ((fent *)ent)->id : SH_SCRIPT_SPAWN_ID_NONE; }
static int g_room_used;   /* entity slots the rest of the game holds, past the fake's own */
static int f_slots(void) { return g_nents + g_room_used; }
static uint32_t f_slot_spawn_id(int n)
{
    if (n >= g_nents && n < g_nents + g_room_used) return 0x7000000u + (uint32_t)n;   /* the game's own entities */
    return n >= 0 && n < g_nents && g_ents[n].alive ? g_ents[n].id : SH_SCRIPT_SPAWN_ID_NONE;
}
static int f_kind(void *ent) { return ent ? ((fent *)ent)->kind : SH_SCRIPT_KIND_GONE; }
static int f_origin(void *ent, float out[3]) { memcpy(out, ((fent *)ent)->pos, 12); return 1; }
static int f_axis(void *ent, float out[9]) { memcpy(out, ((fent *)ent)->ax, 36); return 1; }
static int f_place(void *ent, const float *o, const float *a)
{
    if (o) memcpy(((fent *)ent)->pos, o, 12);
    if (a) memcpy(((fent *)ent)->ax, a, 36);
    return 1;
}
static int f_def_name(void *ent, char *out, size_t cap) { strcpy_s(out, cap, ((fent *)ent)->def); return 1; }

static int f_event(void *ent, const char *name, const sh_script_earg *a, int n, sh_script_earg *ret, int post)
{
    fent *e = (fent *)ent;
    char line[256];
    int i;
    size_t off;
    off = (size_t)_snprintf_s(line, sizeof line, _TRUNCATE, "%s%s(", post ? "post " : "", name);
    for (i = 0; i < n && off < sizeof line; i++) {
        const char *sep = i ? "," : "";
        switch (a[i].type) {
        case 'i': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%d", sep, a[i].i); break;
        case 'f': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%g", sep, a[i].f); break;
        case 'v': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%g %g %g", sep, a[i].v[0], a[i].v[1], a[i].v[2]); break;
        case 's': case '1': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s'%s'", sep, a[i].s); break;
        default: off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s?", sep); break;
        }
    }
    strcat_s(line, sizeof line, ");");
    strcat_s(g_events, sizeof g_events, line);
    if (ret) memset(ret, 0, sizeof *ret);
    if (!strcmp(name, "getLinearVelocity") && ret) { ret->type = 'v'; memcpy(ret->v, e->vel, 12); }
    else if (!strcmp(name, "setLinearVelocity")) memcpy(e->vel, a[0].v, 12);
    else if (!strcmp(name, "getPlayerHealth") && ret && e->kind == SH_SCRIPT_KIND_PLAYER) { ret->type = 'f'; ret->f = e->cur; }
    else if (!strcmp(name, "getPlayerMaxHealth") && ret && e->kind == SH_SCRIPT_KIND_PLAYER) { ret->type = 'f'; ret->f = e->max; }
    else if (!strcmp(name, "increaseHealth")) { e->cur += a[0].f; if (e->cur > e->max) e->cur = e->max; }
    else if (!strcmp(name, "getViewAngles") && ret) { ret->type = 'v'; memcpy(ret->v, e->view, 12); }
    else if (!strcmp(name, "getViewPos") && ret) { ret->type = 'v'; memcpy(ret->v, e->pos, 12); ret->v[2] += 64; }
    else if (!strcmp(name, "animState_IsFinished") && ret) { ret->type = 'i'; ret->i = !strcmp(a[1].s, e->node); }
    else if (!strcmp(name, "remove")) e->alive = 0;
    return 1;
}

static int f_ai_health(void *ai, float *cur, float *max)
{
    fent *e = (fent *)ai;
    if (e->kind != SH_SCRIPT_KIND_AI) return 0;
    *cur = e->cur; *max = e->max;
    return 1;
}
static int f_ai_heal(void *ai, float amount) { fent *e = (fent *)ai; e->cur += amount; if (e->cur > e->max) e->cur = e->max; return 1; }
static int f_ai_set_health(void *ai, float v) { ((fent *)ai)->cur = v; return 1; }
static char *f_anim_node(void *ai)
{
    fent *e = (fent *)ai;
    if (e->kind != SH_SCRIPT_KIND_AI || !e->node[0]) return NULL;
    return _strdup(e->node);
}

static void *f_spawn(const char *def, const float o[3], float yaw, const char **why)
{
    fent *e;
    const char *names[1];
    *why = NULL;
    if (strstr(def, "missing")) { *why = "it is not an entityDef of this mod's build"; return NULL; }
    e = add_ent(SH_SCRIPT_KIND_OTHER, def, o[0], o[1], o[2]);
    yaw_axis(yaw, e->ax);
    names[0] = def;
    sh_script_hooks_record(e->id, names, 1, NULL);   /* the spawn subscriber */
    return e;
}

static int f_trace(const float a[3], const float b[3], uint32_t ignore, sh_script_trace *t)
{
    memset(t, 0, sizeof *t);
    t->fraction = 1;
    memcpy(t->end, b, 12);
    t->hit_spawn_id = SH_SCRIPT_SPAWN_ID_NONE;
    if (a[0] < g_wall_x && b[0] >= g_wall_x && fabsf(a[1]) < 50 && !(g_wall_id != SH_SCRIPT_SPAWN_ID_NONE && ignore == g_wall_id)) {
        t->fraction = (g_wall_x - a[0]) / (b[0] - a[0]);
        t->end[0] = g_wall_x;
        t->normal[0] = -1;
        t->hit_spawn_id = g_wall_id;
    }
    return 1;
}

static int f_time(int *ms) { *ms = g_ms; return 1; }

/* The floor for nav_ground: z 0 everywhere, a slope too steep to stand on
 * past x 1000, nothing at all below x -1000. */
static int g_floor_traces;
static int floor_trace(const float a[3], const float b[3], uint32_t ignore, sh_script_trace *t)
{
    (void)ignore;
    g_floor_traces++;
    memset(t, 0, sizeof *t);
    t->fraction = 1;
    memcpy(t->end, b, 12);
    t->hit_spawn_id = SH_SCRIPT_SPAWN_ID_NONE;
    if (a[0] < -1000 || !(a[2] >= 0 && b[2] < 0)) return 1;
    t->fraction = a[2] / (a[2] - b[2]);
    t->end[2] = 0;
    t->hit_spawn_id = SH_SCRIPT_SPAWN_ID_WORLD;
    if (a[0] > 1000) { t->normal[0] = 0.8f; t->normal[2] = 0.6f; }
    else t->normal[2] = 1;
    return 1;
}
static int f_ground(const float pos[3], float out[3]) { return sh_script_api_ground_trace(floor_trace, pos, out); }
static int g_difficulty = 2;
static int f_difficulty(int *level) { if (g_difficulty < 0) return 0; *level = g_difficulty; return 1; }

static const sh_script_engine k_engine = {
    f_resolve, f_spawn_id, f_slots, f_slot_spawn_id, f_kind, f_origin, f_axis, f_place, f_def_name, f_event,
    f_ai_health, f_ai_heal, f_ai_set_health, f_anim_node, f_spawn, f_trace, f_time, f_ground, f_difficulty
};

/* The AI calls' engine seam, as far as a demon's animation needs it: the
 * park answers yes and each command reaches the same event log. */
static int ai_park(void *ai) { (void)ai; return 1; }
static int ai_event(void *ent, int ev, const sh_ai_arg *args, int n, double *ret)
{
    sh_script_earg a[SH_SCRIPT_EVENT_ARGS];
    int i;
    for (i = 0; i < n && i < SH_SCRIPT_EVENT_ARGS; i++) {
        if (args[i].type == 's' || args[i].type == '1') a[i] = sh_script_earg_text(args[i].type, args[i].s);
        else if (args[i].type == 'f') a[i] = sh_script_earg_float(args[i].f);
        else a[i] = sh_script_earg_int(args[i].i);
    }
    if (ret) *ret = 0;
    return f_event(ent, SH_AI_EVENTS[ev].name, a, n, NULL, 0);
}
static const sh_script_ai_engine k_ai_engine = {
    NULL, NULL, NULL, NULL, NULL, ai_event, ai_park, NULL, NULL, NULL, NULL, NULL, NULL
};

/* the handle table's resolver: a removed entity is gone */
static void *h_resolve(uint32_t id) { return by_id(id); }

/* ------------------------------------------------------------ the tests */

/* test only: ent(n) is the entity at index n of the world (as a hook hands it over) */
static int t_ent(lua_State *L)
{
    int n = (int)luaL_checkinteger(L, 1);
    sh_script_handle h;
    if (n < 0 || n >= g_nents) return 0;
    h = sh_script_handles_find(g_ents[n].id, NULL);
    if (!h) h = sh_script_handles_ref(g_ents[n].id);
    sh_script_push_entity(L, h);
    return 1;
}

static char g_log[65536];
static void log_cat(void *ctx, const char *line)
{
    (void)ctx;
    if (strlen(g_log) + strlen(line) + 2 < sizeof g_log) { strcat_s(g_log, sizeof g_log, line); strcat_s(g_log, sizeof g_log, "\n"); }
}
static const char *log_now(void)
{
    g_log[0] = 0;
    sh_script_log_read(SALT, log_cat, NULL);
    return g_log;
}
static char g_stop[4096];
static const char *stop_now(void)
{
    g_stop[0] = 0;
    HOS(g_stop, sh_script_stop_line_dup(SALT));
    return g_stop;
}

/* A fresh world and one mod running `body`; the mod's entities are recorded
 * and the first frame delivers their spawns. */
static void world(void)
{
    const char *names[2];
    g_nents = 0;
    g_events[0] = 0;
    g_ms = 5000;
    g_wall_id = SH_SCRIPT_SPAWN_ID_NONE;
    {
        fent *a = add_ent(SH_SCRIPT_KIND_AI, "mods/" SALT "/ai/brute", 0, 0, 0);
        a->cur = 100; a->max = 150;
        strcpy_s(a->node, sizeof a->node, "combat/idle");
        a->vel[0] = 5;
    }
    { fent *s = add_ent(SH_SCRIPT_KIND_AI, "ai/demon/imp", 300, 0, 0); s->cur = 50; s->max = 60; }
    { fent *p = add_ent(SH_SCRIPT_KIND_PLAYER, "player/sp", 0, 200, 0); p->cur = 40; p->max = 100; p->view[1] = -90; }
    add_ent(SH_SCRIPT_KIND_OTHER, "mods/" SALT "/props/crate", 30, 40, 0);
    add_ent(SH_SCRIPT_KIND_OTHER, "decor/far", 5000, 0, 0);
    { fent *w = add_ent(SH_SCRIPT_KIND_OTHER, "decor/wall", 100, 0, 0); g_wall_id = w->id; }
    (void)names;
}

static void run(const char *body)
{
    sh_script_mod_desc d;
    const char *names[1];
    int i;
    sh_script_map_end();
    world();
    strcpy_s(g_text, sizeof g_text, body);
    d.salt = SALT;
    d.name = "Api Test";
    sh_script_map_start(&d, 1, &g_src, 3, 5.0);
    for (i = 0; i < g_nents; i++) {
        names[0] = g_ents[i].def;
        sh_script_hooks_record(g_ents[i].id, names, 1, NULL);
    }
    sh_script_api_frame();
    sh_script_tick(5.0, 0.016);
}

static void frame(int ms)
{
    g_ms += ms;
    sh_script_api_frame();
    sh_script_tick(g_ms / 1000.0, ms / 1000.0);
}

static int has(const char *hay, const char *needle)
{
    int ok = strstr(hay, needle) != NULL;
    if (!ok) printf("  missing: %s\n  in: %s\n", needle, hay);
    return ok;
}

static void test_reads(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  local p = self:pos(); print('pos', p.x, p.y, p.z)\n"
        "  print('yaw', self:yaw())\n"
        "  local a = self:axis(); print('fwd', a.forward.x, a.left.y, a.up.z)\n"
        "  local v = self:velocity(); print('vel', v.x)\n"
        "  print('health', self:health(), ent(1):health(), ent(2):health(), ent(3):health())\n"
        "  print('alive', self:alive(), ent(3):alive())\n"
        "  print('def', self:def(), ent(1):def(), ent(3):def())\n"
        "  print('dist', self:distance(ent(2)), self:distance({x = 3, y = 4, z = 0}), self:distance({3, 4, 0}))\n"
        "  print('player', self:is_player(), ent(2):is_player())\n"
        "  print('players', #players(), players()[1] == ent(2))\n"
        "  print('same', ent(1) == ent(1))\n"
        "  print('now', now())\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "pos\t0.0\t0.0\t0.0"));
    CHECK(has(g_log, "yaw\t0.0"));
    CHECK(has(g_log, "fwd\t1.0\t1.0\t1.0"));
    CHECK(has(g_log, "vel\t5.0"));
    CHECK(has(g_log, "health\t100.0\t50.0\t40.0\tnil"));
    CHECK(has(g_log, "alive\ttrue\ttrue"));
    CHECK(has(g_log, "def\tai/brute\tai/demon/imp\tprops/crate"));
    CHECK(has(g_log, "dist\t200.0\t5.0\t5.0"));
    CHECK(has(g_log, "player\tfalse\ttrue"));
    CHECK(has(g_log, "players\t1\ttrue"));
    CHECK(has(g_log, "same\ttrue"));
    CHECK(has(g_log, "now\t5.0"));
}

static void test_writes(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  self:set_pos({x = 10, y = 20, z = 30}); local p = self:pos(); print('moved', p.x, p.y, p.z)\n"
        "  self:set_yaw(90); print('turned', math.floor(self:yaw() + 0.5))\n"
        "  self:set_velocity({1, 2, 3}); print('pushed', self:velocity().z)\n"
        "  self:set_health(140); print('healed', self:health())\n"
        "  self:set_health(500); print('capped', self:health())\n"
        "  self:set_health(20); print('lowered', self:health())\n"
        "  ent(2):set_health(70); print('player healed', ent(2):health())\n"
        "  ent(3):set_pos({x = 1, y = 1, z = 1}); print('prop moved', ent(3):pos().x)\n"
        "  ent(3):remove()\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "moved\t10.0\t20.0\t30.0"));
    CHECK(has(g_log, "turned\t90"));
    CHECK(has(g_log, "pushed\t3.0"));
    CHECK(has(g_log, "healed\t140.0"));
    CHECK(has(g_log, "capped\t150.0"));
    CHECK(has(g_log, "lowered\t20.0"));
    CHECK(has(g_log, "player healed\t70.0"));
    CHECK(has(g_log, "prop moved\t1.0"));
    CHECK(has(g_events, "setLinearVelocity(1 2 3);"));
    CHECK(has(g_events, "increaseHealth(30);"));
    CHECK(has(g_events, "post remove();"));
    /* the prop is gone next frame: its calls answer nil, alive answers false */
    run("api = 1\n"
        "local crate\n"
        "on.spawn('props/crate', function(self) crate = self; self:remove() end)\n"
        "on.think('ai/brute', function(self)\n"
        "  if crate then print('after', crate:pos(), crate:health(), crate:alive(), crate:distance(self)); crate = nil end\n"
        "end)\n");
    frame(16);
    frame(16);
    CHECK(has(log_now(), "after\tnil\tnil\tfalse\tnil"));
}

static void refused(const char *body, const char *line, const char *words)
{
    char want[256];
    run(body);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_STOPPED);
    _snprintf_s(want, sizeof want, _TRUNCATE, "Script stopped: Api Test, scripts\\main.lua line %s: ", line);
    CHECK(has(stop_now(), want));
    CHECK(has(g_stop, words));
}

static void test_refusals(void)
{
    refused("api = 1\non.spawn('ai/brute', function(self)\n  ent(1):set_pos({x = 1, y = 2, z = 3})\nend)\n",
            "3", "\"set_pos\" can only move this mod's own entities; this one is not");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  ent(1):remove()\nend)\n",
            "3", "\"remove\" can only remove this mod's own entities");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  self:set_pos(nil)\nend)\n",
            "3", "\"set_pos\" needs the new position (a position with x, y and z), got nothing");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  self:set_pos({x = 1, y = 'up'})\nend)\n",
            "3", "one of them is missing or not a number");
    refused("api = 1\non.spawn('ai/brute', function(self)\n\n  self:set_health(0)\nend)\n",
            "4", "\"set_health\" cannot take health to 0; kill with damage(...) so the death plays");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  ent(2):set_health(10)\nend)\n",
            "3", "\"set_health\" can only heal a player");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  ent(1):set_health(10)\nend)\n",
            "3", "can only change the health of this mod's own entities and heal players");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  ent(3):set_health(10)\nend)\n",
            "3", "\"set_health\" was given an entity that has no health");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  self:distance('ai/imp')\nend)\n",
            "3", "\"distance\" needs what to measure to (an entity, or a position with x, y and z), got text");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  self:pos(5)\n  print(5):pos()\nend)\n",
            "4", "");   /* a method on nothing: the runtime's own wording */
    refused("api = 1\non.spawn('ai/brute', function(self)\n  ent(1):anim_state('melee')\nend)\n",
            "3", "\"anim_state\" can only animate this mod's own entities");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  self:anim_event('DEST_BLEND_END')\nend)\n",
            "3", "\"anim_event\" knows DEST_BLEND_START, DEST_REACHED, VIA_BLEND_IN_START, VIA_BLEND_OUT_START and FINISHED, not \"DEST_BLEND_END\"");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  self:set_yaw(0/0)\nend)\n",
            "3", "\"set_yaw\" needs the new facing in degrees as an ordinary number");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  pos()\nend)\n",
            "3", "");
}

static void test_spawn(void)
{
    run("api = 1\n"
        "on.map_start(function()\n"
        "  local c = spawn('props/crate', {x = 7, y = 8, z = 9}, 90)\n"
        "  print('spawned', c ~= nil, c:pos().x, math.floor(c:yaw() + 0.5), c:def())\n"
        "  c:set_pos({x = 1, y = 1, z = 1}); print('own', c:pos().x)\n"
        "end)\n"
        "on.spawn('props/crate', function(self) print('spawn handler', self:def()) end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "spawned\ttrue\t7.0\t90\tprops/crate"));
    CHECK(has(g_log, "own\t1.0"));
    frame(16);
    CHECK(has(log_now(), "spawn handler\tprops/crate"));
    /* another mod's content and game content are refused before the engine */
    refused("api = 1\non.map_start(function()\n  spawn('mods/othermod/x', {0, 0, 0})\nend)\n",
            "3", "\"spawn\" can only use this mod's own content; \"mods/othermod/x\" belongs to another mod");
    refused("api = 1\non.map_start(function()\n  spawn('props/missing', {0, 0, 0})\nend)\n",
            "3", "\"spawn\" could not spawn \"props/missing\": it is not an entityDef of this mod's build");
    /* no count or rate of its own: 200 in one call, 400 alive */
    run("api = 1\n"
        "local n = 0\n"
        "every(1.5, function() for i = 1, 200 do if spawn('props/crate', {i, 0, 0}) then n = n + 1 end end; print('n', n) end)\n");
    frame(1600);
    frame(1600);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "n\t400"));
    /* the game's own entity table is the only bound: it is nearly full */
    g_room_used = 16382 - 256;
    frame(1600);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_STOPPED);
    CHECK(has(stop_now(), "\"spawn\" cannot add an entity now: the game holds at most 16382 entities at once and nearly "
                          "all are in use; remove some first"));
    g_room_used = 0;
}

static void test_world(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  local near = entities_near(self, 260)\n"
        "  local d = {}\n"
        "  for i, e in ipairs(near) do d[i] = e:def() end\n"
        "  print('near', #near, table.concat(d, ' '))\n"
        "  print('none', #entities_near({x = -9000, y = 0, z = 0}, 10))\n"
        "  local hit = trace({x = 0, y = 0, z = 0}, {x = 200, y = 0, z = 0})\n"
        "  print('hit', hit.pos.x, hit.normal.x, hit.fraction, hit.entity:def())\n"
        "  print('miss', trace({x = 0, y = 0, z = 0}, {x = 50, y = 0, z = 0}))\n"
        "  print('beside', trace({x = 0, y = 60, z = 0}, {x = 200, y = 60, z = 0}))\n"
        "  local thick = trace({x = 0, y = 60, z = 0}, {x = 200, y = 60, z = 0}, 20)\n"
        "  print('thick', thick and thick.pos.x, thick and thick.pos.y)\n"
        "  print('ignored', trace({x = 0, y = 0, z = 0}, {x = 200, y = 0, z = 0}, 0, ent(5)))\n"
        "  print('from entity', trace(self, {x = 200, y = 0, z = 0}).pos.x)\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "near\t4\tai/brute props/crate decor/wall player/sp"));
    CHECK(has(g_log, "none\t0"));
    CHECK(has(g_log, "hit\t100.0\t-1.0\t0.5\tdecor/wall"));
    CHECK(has(g_log, "miss\tnil"));
    CHECK(has(g_log, "beside\tnil"));
    CHECK(has(g_log, "thick\t100.0\t60.0"));
    CHECK(has(g_log, "ignored\tnil"));
    CHECK(has(g_log, "from entity\t100.0"));
    refused("api = 1\non.spawn('ai/brute', function(self)\n  trace(self, self, -1)\nend)\n",
            "3", "\"trace\" needs a radius of 0 or more, got -1");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  entities_near(self)\nend)\n",
            "3", "\"entities_near\" needs the radius as a number, got nothing");
}

/* nav_ground: the trace straight down, a flat enough surface only;
 * difficulty: the engine's value, nil when it cannot be read. */
static void test_ground_and_difficulty(void)
{
    float pos[3] = { 10, 20, 300 }, out[3];
    printf("-- nav_ground and difficulty\n");
    g_floor_traces = 0;
    CHECK(sh_script_api_ground_trace(floor_trace, pos, out) == 1 && out[0] == 10 && out[1] == 20 && out[2] == 0);
    CHECK(g_floor_traces == 1);
    pos[2] = -10;    /* just under the floor: the trace starts SH_SCRIPT_GROUND_ABOVE higher and finds it */
    CHECK(sh_script_api_ground_trace(floor_trace, pos, out) == 1 && out[2] == 0);
    pos[0] = 2000; pos[2] = 50;
    CHECK(sh_script_api_ground_trace(floor_trace, pos, out) == 0);   /* too steep */
    pos[0] = -2000;
    CHECK(sh_script_api_ground_trace(floor_trace, pos, out) == 0);   /* nothing below */
    CHECK(sh_script_api_ground_trace(NULL, pos, out) == 0);
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  local g = nav_ground({x = 5, y = 6, z = 100})\n"
        "  print('ground', g.x, g.y, g.z)\n"
        "  print('steep', nav_ground({x = 1500, y = 0, z = 100}))\n"
        "  print('under self', nav_ground(self).z)\n"
        "  print('difficulty', difficulty())\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "ground\t5.0\t6.0\t0.0"));
    CHECK(has(g_log, "steep\tnil"));
    CHECK(has(g_log, "under self\t0.0"));
    CHECK(has(g_log, "difficulty\t2"));
    g_difficulty = -1;
    run("api = 1\non.spawn('ai/brute', function(self) print('unread', difficulty()) end)\n");
    CHECK(has(log_now(), "unread\tnil"));
    g_difficulty = 2;
    refused("api = 1\non.spawn('ai/brute', function(self)\n  difficulty(1)\nend)\n",
            "3", "\"difficulty\" takes no values; call it as difficulty()");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  nav_ground(5)\nend)\n",
            "3", "\"nav_ground\" needs the position to look under");
}

static void test_animation(void)
{
    fent *ai;
    run("api = 1\n"
        "local brute\n"
        "on.spawn('ai/brute', function(self)\n"
        "  brute = self\n"
        "  print('current', self:anim_current(), ent(3):anim_current())\n"
        "  self:anim_state('melee', 'combat/windup')\n"
        "  print('requested', self:anim_event('DEST_BLEND_START'), self:anim_finished())\n"
        "end)\n"
        "on.think('ai/brute', function(self)\n"
        "  print('frame', self:anim_current(), self:anim_event('VIA_BLEND_IN_START'), self:anim_event('AWCMD_EVENT_VIA_BLEND_OUT_START'),\n"
        "        self:anim_event('dest_reached'), self:anim_event('FINISHED'), self:anim_finished())\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "current\tcombat/idle\tnil"));
    CHECK(has(g_log, "requested\tfalse\tfalse"));
    CHECK(has(g_events, "setScriptAbort(0,0);setScriptAbortFlags(0,0);ssaction_ChangeAnimStateVia(0,'combat/melee','combat/windup',0);"));
    ai = &g_ents[E_AI];
    /* the web goes through the via node to the destination, then on */
    strcpy_s(ai->node, sizeof ai->node, "combat/windup");
    frame(16);
    CHECK(has(log_now(), "frame\tcombat/windup\ttrue\tfalse\tfalse\tfalse\tfalse"));
    strcpy_s(ai->node, sizeof ai->node, "combat/melee");
    frame(16);
    CHECK(has(log_now(), "frame\tcombat/melee\ttrue\ttrue\ttrue\tfalse\tfalse"));
    strcpy_s(ai->node, sizeof ai->node, "combat/idle");
    sh_script_api_frame();   /* observed between the script's looks */
    strcpy_s(ai->node, sizeof ai->node, "combat/melee");
    frame(16);
    CHECK(has(log_now(), "frame\tcombat/melee\ttrue\ttrue\ttrue\ttrue\ttrue"));
    {
        /* a node name of any length the animWeb gives it is reported whole */
        char want[1600];
        memset(ai->node, 0, sizeof ai->node);
        strcpy_s(ai->node, sizeof ai->node, "combat/");
        memset(ai->node + 7, 'x', 1500);
        frame(16);
        sprintf_s(want, sizeof want, "frame\t%s\t", ai->node);
        CHECK(has(log_now(), want));
    }

    /* a node the web passes through between two frames of the script still counts */
    run("api = 1\n"
        "on.spawn('ai/brute', function(self) self:anim_force('combat/stagger', 0.2) end)\n"
        "on.think('ai/brute', function(self) print('dest', self:anim_event('DEST_BLEND_START'), self:anim_finished()) end)\n");
    CHECK(has(g_events, "ssaction_ForceAnimState(0,'combat/stagger',200,0);"));
    ai = &g_ents[E_AI];
    strcpy_s(ai->node, sizeof ai->node, "combat/stagger");
    sh_script_api_frame();
    strcpy_s(ai->node, sizeof ai->node, "combat/idle");
    frame(16);
    CHECK(has(log_now(), "dest\ttrue\ttrue"));

    /* other entities use the stock animWeb events */
    run("api = 1\n"
        "on.spawn('props/crate', function(self)\n"
        "  self:anim_state('doors/open')\n"
        "  print('crate finished', self:anim_finished())\n"
        "  self:anim_force('closed', 1)\n"
        "end)\n");
    CHECK(has(g_events, "post changeAnimState('doors','open');"));
    CHECK(has(g_events, "animState_IsFinished('doors','open');"));
    CHECK(has(g_events, "post forceAnimState('','closed',1000);"));
    CHECK(has(log_now(), "crate finished\tfalse"));
}

static void test_view_angle(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  print('player aims', math.floor(ent(2):view_angle_to(self) + 0.5))\n"
        "  print('from eyes', math.floor(ent(2):view_angle_to(self, true) + 0.5))\n"
        "  print('facing', math.floor(self:view_angle_to(ent(1)) + 0.5), math.floor(self:view_angle_to({x = 0, y = 10, z = 0}) + 0.5))\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "player aims\t0"));
    CHECK(has(g_log, "from eyes\t18"));
    CHECK(has(g_log, "facing\t0\t90"));
}

int main(void)
{
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "ent", t_ent);
    sh_script_api_set_engine(&k_engine);
    sh_script_ai_set_engine(&k_ai_engine);
    sh_script_api_install(NULL, 0, NULL);
    sh_script_api_register();   /* idempotent */
    sh_script_handles_set_resolver(h_resolve);

    test_reads();
    test_writes();
    test_refusals();
    test_spawn();
    test_world();
    test_ground_and_difficulty();
    test_animation();
    test_view_angle();
    sh_script_map_end();
    CHECK(sh_script_bytes_in_use() == 0);
    printf(g_fail ? "script_api_test: FAILED\n" : "script_api_test: ok\n");
    return g_fail;
}
