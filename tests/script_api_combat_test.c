/* script_api_combat_test.c -- the combat calls of mod scripts
 * (src/backend/script/script_api_combat.c) and the delivery of the damage,
 * death, pain, animation, frame-event and projectile records the hooks post,
 * against a fake engine: a mod's AI, a stock AI, a player, a prop of the mod,
 * a wall and a world; decls of the mod by type.
 *
 * Covers: damage and e:damage (the engine event and its arguments, the world
 * as attacker when none is given), hitscan hits and misses, radius_damage,
 * explode with and without an effect, fx on an entity's own fx decl and as a
 * placed effect (fx_move, fx_stop), sound and its channels, tag and
 * can_see_from, pain, fire at an entity and at a position (the projectile it
 * returns, steer, explode); every refusal is one plain sentence naming the
 * file and line (another mod's or the game's decl, a decl the mod lacks, an
 * attacker that is not the mod's, a projectile the mod did not launch, a
 * condition the game does not have); the hooks' records reach on.damage,
 * on.death, on.anim, on.frame_event, on.pain and on.projectile with their
 * values; the records the hooks keep (last hit, first death, launches), each
 * released with its entity; a script holding projectiles as long as it likes:
 * 300 alive at once steered every frame, 5,000 alive steered in one frame,
 * every call on one that is gone answering nil or false and never stopping
 * the mod, and the launch records gone with them. */
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
#include "script/script_api_combat.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define SALT "combat01"

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
    int      alive;
    float    pos[3], ax[9], vel[3];
    float    cur;
    int      pain;
    char     def[128];
} fent;

typedef struct fdecl { char type[16]; char name[128]; } fdecl;

#define ENTS 8192
static fent g_ents[ENTS];
static int g_nents;
static int g_ms;
static char g_events[16384];
static fent *g_world_ent;
static fdecl g_decls[] = {
    { "damage", "mods/" SALT "/damage/rock" },
    { "damage", "mods/" SALT "/damage/beam" },
    { "projectile", "mods/" SALT "/rocket" },
    { "sound", "mods/" SALT "/brute/roar" },
    { "damage", "mods/othermod/damage/rock" },
};

enum { E_AI = 0, E_STOCK_AI, E_PLAYER, E_PROP, E_WALL };

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
    e->id = 0x20000u + (uint32_t)g_nents * 5u;
    e->kind = kind;
    e->alive = 1;
    e->cur = 100;
    e->pos[0] = x; e->pos[1] = y; e->pos[2] = z;
    yaw_axis(0, e->ax);
    strcpy_s(e->def, sizeof e->def, def);
    g_nents++;
    return e;
}

static fent *by_id(uint32_t id)
{
    uint32_t n;
    if (id == SH_SCRIPT_SPAWN_ID_WORLD) return g_world_ent;
    if (id < 0x20000u || (id - 0x20000u) % 5u) return NULL;
    n = (id - 0x20000u) / 5u;
    return n < (uint32_t)g_nents && g_ents[n].alive ? &g_ents[n] : NULL;
}

static fent *by_name(const char *name)
{
    int n = -1;
    if (sscanf_s(name, "ent%d", &n) != 1 || n < 0 || n >= g_nents) return NULL;
    return &g_ents[n];
}

static int g_launch_from;   /* launches the hook started on a tag */
static void *f_resolve(uint32_t id) { return by_id(id); }
static uint32_t f_spawn_id(void *ent) { return ent ? ((fent *)ent)->id : SH_SCRIPT_SPAWN_ID_NONE; }
static int f_slots(void) { return g_nents; }
static uint32_t f_slot_spawn_id(int n) { return n >= 0 && n < g_nents && g_ents[n].alive ? g_ents[n].id : SH_SCRIPT_SPAWN_ID_NONE; }
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

static void ev_cat(const char *s)
{
    if (strlen(g_events) + strlen(s) + 1 < sizeof g_events) strcat_s(g_events, sizeof g_events, s);
}

static int f_event(void *ent, const char *name, const sh_script_earg *a, int n, sh_script_earg *ret, int post)
{
    fent *e = (fent *)ent;
    char line[512];
    int i;
    size_t off = (size_t)_snprintf_s(line, sizeof line, _TRUNCATE, "%s%s@%s(", post ? "post " : "", name, e->def);
    for (i = 0; i < n && off < sizeof line; i++) {
        const char *sep = i ? "," : "";
        switch (a[i].type) {
        case 'i': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%d", sep, a[i].i); break;
        case 'f': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%g", sep, a[i].f); break;
        case 'v': case 'a':
            off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%c%.0f %.0f %.0f", sep, a[i].type, a[i].v[0],
                               a[i].v[1], a[i].v[2]);
            break;
        case 's': case '1': case '2': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s'%s'", sep, a[i].s); break;
        case 'd': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%sd:%s", sep, ((fdecl *)a[i].p)->name); break;
        case 'e': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%se:%s", sep, ((fent *)a[i].p)->def); break;
        case '4': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%sNULL", sep); break;
        default: off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s?", sep); break;
        }
    }
    strcat_s(line, sizeof line, ");");
    ev_cat(line);
    if (ret) memset(ret, 0, sizeof *ret);
    if (!strcmp(name, "getLinearVelocity") && ret) { ret->type = 'v'; memcpy(ret->v, e->vel, 12); }
    else if (!strcmp(name, "setLinearVelocity")) memcpy(e->vel, a[0].v, 12);
    else if (!strcmp(name, "remove")) e->alive = 0;
    else if (!strcmp(name, "damage")) e->cur -= 10;
    else if (!strcmp(name, "getJointHandle") && ret) { ret->type = 'i'; ret->i = !strcmp(a[0].s, "muzzle") ? 3 : -1; }
    else if (!strcmp(name, "getJointPos") && ret) {
        ret->type = 'v';
        ret->v[0] = e->pos[0] + 10; ret->v[1] = e->pos[1]; ret->v[2] = e->pos[2] + 50;
    } else if (!strcmp(name, "getJointAngle") && ret) { ret->type = 'v'; ret->v[0] = 0; ret->v[1] = 90; ret->v[2] = 0; }
    else if (!strcmp(name, "ae_launchProjectileAtTarget")) {
        /* the engine spawns the projectile at the joint and its launch hook notes it */
        fent *t = by_name(a[3].s), *p;
        int mod = -1;
        sh_script_handle owner = sh_script_handles_find(e->id, &mod);
        p = add_ent(SH_SCRIPT_KIND_OTHER, "projectile/stock_rocket", e->pos[0] + 10, e->pos[1], e->pos[2] + 50);
        if (t) {
            float d[3] = { t->pos[0] - p->pos[0], t->pos[1] - p->pos[1], t->pos[2] - p->pos[2] };
            float l = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l > 0) { p->vel[0] = d[0] / l * 600; p->vel[1] = d[1] / l * 600; p->vel[2] = d[2] / l * 600; }
        }
        {   /* as the launch hook does: a launch ai:fire asked to start on a tag starts there */
            float o[3], ax[9];
            if (sh_script_combat_launch_from(owner, o, ax)) { memcpy(p->pos, o, 12); memcpy(p->ax, ax, 36); g_launch_from++; }
        }
        sh_script_combat_launch(p->id, owner, mod);
    }
    return 1;
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
    sh_script_hooks_record(e->id, names, 1, NULL);
    return e;
}

/* a wall at x = 100 (the wall entity), the world nowhere else */
static int f_trace(const float a[3], const float b[3], uint32_t ignore, sh_script_trace *t)
{
    memset(t, 0, sizeof *t);
    t->fraction = 1;
    memcpy(t->end, b, 12);
    t->hit_spawn_id = SH_SCRIPT_SPAWN_ID_NONE;
    if (a[0] < 100 && b[0] >= 100 && fabsf(a[1]) < 50 && ignore != g_ents[E_WALL].id) {
        float f = (100 - a[0]) / (b[0] - a[0]);
        t->fraction = f;
        t->end[0] = 100; t->end[1] = a[1] + (b[1] - a[1]) * f; t->end[2] = a[2] + (b[2] - a[2]) * f;
        t->normal[0] = -1;
        t->hit_spawn_id = g_ents[E_WALL].id;
    }
    return 1;
}

static int f_time(int *ms) { *ms = g_ms; return 1; }

/* md6Def tags: every demon's model has "laser_source_1", 20 inches ahead of
 * its origin and 80 up, pointing straight up (its own axis, unlike the
 * parent joint's); "muzzle" is a joint, not a tag. */
static int f_tag(void *ent, const char *name, float pos[3], float axis[9])
{
    fent *e = (fent *)ent;
    static const float up[9] = { 0, 0, 1, 0, 1, 0, -1, 0, 0 };
    if (e->kind != SH_SCRIPT_KIND_AI || strcmp(name, "laser_source_1")) return 0;
    pos[0] = e->pos[0] + 20; pos[1] = e->pos[1]; pos[2] = e->pos[2] + 80;
    if (axis) memcpy(axis, up, sizeof up);
    return 1;
}

static const sh_script_engine k_engine = {
    f_resolve, f_spawn_id, f_slots, f_slot_spawn_id, f_kind, f_origin, f_axis, f_place, f_def_name, f_event,
    NULL, NULL, NULL, NULL, f_spawn, f_trace, f_time, NULL, NULL, f_tag
};

/* the combat engine */
static void *c_decl(const char *type, const char *name)
{
    int i;
    for (i = 0; i < (int)(sizeof g_decls / sizeof g_decls[0]); i++)
        if (!strcmp(g_decls[i].type, type) && !strcmp(g_decls[i].name, name)) return &g_decls[i];
    return NULL;
}
static int c_radius(const float p[3], void *decl, void *inflictor, void *attacker)
{
    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE, "radius(%.0f %.0f %.0f,%s,%s,%s);", p[0], p[1], p[2], ((fdecl *)decl)->name,
                ((fent *)inflictor)->def, ((fent *)attacker)->def);
    ev_cat(line);
    return 1;
}
static int c_explode(void *p)
{
    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE, "explode(%s);", ((fent *)p)->def);
    ev_cat(line);
    ((fent *)p)->alive = 0;
    return 1;
}
static int c_pain(void *ai) { return ((fent *)ai)->pain; }
static int c_name(void *ent, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "ent%d", (int)((fent *)ent - g_ents));
    return 1;
}
static int c_fx(const char *name)
{
    if (!strcmp(name, "FX_CINEMATIC_8")) return 222;
    if (!strcmp(name, "FX_NONE")) return 0;
    return -1;
}
static void *c_world(void) { return g_world_ent; }

static int c_start_fx(void *ent, int cond)
{
    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE, "entity StartFX@%s(%d);", ((fent *)ent)->def, cond);
    ev_cat(line);
    return 1;
}
static int c_stop_fx(void *ent, int cond)
{
    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE, "entity StopFX@%s(%d);", ((fent *)ent)->def, cond);
    ev_cat(line);
    return 1;
}

static int g_hard_hook = 1, g_hard_asked;
static int c_fx_hard_stops(void) { g_hard_asked++; return g_hard_hook; }

static const sh_script_combat_engine k_combat = { c_decl, c_radius, c_explode, c_pain, c_name, c_fx, c_world,
                                                 c_start_fx, c_stop_fx, NULL, c_fx_hard_stops };

static void *h_resolve(uint32_t id) { return by_id(id); }

/* ------------------------------------------------------------ harness */

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

static void world(void)
{
    static fent world_ent;
    g_nents = 0;
    g_events[0] = 0;
    g_ms = 5000;
    memset(&world_ent, 0, sizeof world_ent);
    world_ent.id = SH_SCRIPT_SPAWN_ID_WORLD;
    world_ent.alive = 1;
    strcpy_s(world_ent.def, sizeof world_ent.def, "world");
    g_world_ent = &world_ent;
    add_ent(SH_SCRIPT_KIND_AI, "mods/" SALT "/ai/brute", 0, 0, 0);
    add_ent(SH_SCRIPT_KIND_AI, "ai/demon/imp", 300, 0, 0);
    add_ent(SH_SCRIPT_KIND_PLAYER, "player/sp", 0, 200, 0);
    add_ent(SH_SCRIPT_KIND_OTHER, "mods/" SALT "/props/crate", 30, 40, 0);
    add_ent(SH_SCRIPT_KIND_OTHER, "decor/wall", 100, 0, 0);
}

static void run(const char *body)
{
    sh_script_mod_desc d;
    const char *names[1];
    int i;
    sh_script_map_end();
    sh_script_combat_reset();
    world();
    strcpy_s(g_text, sizeof g_text, body);
    d.salt = SALT;
    d.name = "Combat Test";
    sh_script_map_start(&d, 1, &g_src, 3, 5.0);
    for (i = 0; i < g_nents; i++) {
        names[0] = g_ents[i].def;
        sh_script_hooks_record(g_ents[i].id, names, 1, NULL);
    }
    sh_script_api_frame();
    sh_script_tick(5.0, 0.016);
}

static void frame(void)
{
    g_ms += 16;
    sh_script_api_frame();
    sh_script_tick(g_ms / 1000.0, 0.016);
}

static int has(const char *hay, const char *needle)
{
    int ok = strstr(hay, needle) != NULL;
    if (!ok) printf("  missing: %s\n  in: %s\n", needle, hay);
    return ok;
}

static void refused(const char *body, const char *line, const char *words)
{
    char want[256];
    run(body);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_STOPPED);
    _snprintf_s(want, sizeof want, _TRUNCATE, "Script stopped: Combat Test, scripts\\main.lua line %s: ", line);
    CHECK(has(stop_now(), want));
    CHECK(has(g_stop, words));
}

static sh_script_handle handle_of(int n)
{
    return sh_script_handles_find(g_ents[n].id, NULL);
}

/* ------------------------------------------------------------ the tests */

static void test_damage(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  print('d1', damage(ent(1), 'damage/rock', self))\n"
        "  print('d2', ent(2):damage('damage/beam'))\n"
        "  print('d3', ent(1):damage('mods/" SALT "/damage/rock', self, {x = 300, y = -100, z = 0}))\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "d1\ttrue"));
    CHECK(has(g_log, "d2\ttrue"));
    CHECK(has(g_log, "d3\ttrue"));
    /* from the attacker at the origin toward the imp at x = 300: yaw 0 */
    CHECK(has(g_events, "damage@ai/demon/imp(d:mods/" SALT "/damage/rock,a0 0 0,0,1,e:mods/" SALT "/ai/brute,0);"));
    /* no attacker: the world, straight down */
    CHECK(has(g_events, "damage@player/sp(d:mods/" SALT "/damage/beam,a90 0 0,0,1,e:world,0);"));
    /* from (300, -100) up the y axis: yaw 90 */
    CHECK(has(g_events, "damage@ai/demon/imp(d:mods/" SALT "/damage/rock,a0 90 0,0,1,e:mods/" SALT "/ai/brute,0);"));
}

static void test_hitscan_radius(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  local h = hitscan({x = 0, y = 0, z = 10}, {x = 1, y = 0, z = 0}, 'damage/beam', nil, self)\n"
        "  print('hit', h.pos.x, h.normal.x, h.entity:def())\n"
        "  print('miss', hitscan({x = 0, y = 0, z = 10}, {x = 0, y = 1, z = 0}, 'damage/beam'))\n"
        "  print('short', hitscan({x = 0, y = 0, z = 10}, {x = 1, y = 0, z = 0}, 'damage/beam', 50))\n"
        "  print('radius', radius_damage({x = 5, y = 6, z = 7}, 'damage/rock', self))\n"
        "  print('world', radius_damage(self, 'damage/rock'))\n"
        "  local fx = explode({x = 1, y = 2, z = 3}, 'damage/rock', 'fx/blast', self)\n"
        "  print('explode', fx:def(), fx:pos().x)\n"
        "  print('bare', explode({x = 1, y = 2, z = 3}, 'damage/rock'))\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "hit\t100.0\t-1.0\tdecor/wall"));
    CHECK(has(g_log, "miss\tnil"));
    CHECK(has(g_log, "short\tnil"));
    CHECK(has(g_events, "damage@decor/wall(d:mods/" SALT "/damage/beam,a0 0 0,0,1,e:mods/" SALT "/ai/brute,0);"));
    CHECK(has(g_events, "radius(5 6 7,mods/" SALT "/damage/rock,mods/" SALT "/ai/brute,mods/" SALT "/ai/brute);"));
    CHECK(has(g_events, "radius(0 0 0,mods/" SALT "/damage/rock,world,world);"));
    CHECK(has(g_log, "radius\ttrue"));
    CHECK(has(g_log, "explode\tfx/blast\t1.0"));
    CHECK(has(g_log, "bare\tnil"));
}

static void test_effects_sound(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  print('fx', fx(self, 'FX_CINEMATIC_8'), fx_stop(self, 'FX_CINEMATIC_8'))\n"
        "  local e = fx('fx/beam_impact', {x = 10, y = 0, z = 0}, {x = 0, y = 1, z = 0})\n"
        "  print('placed', e:def(), e:pos().x, math.floor(e:yaw() + 0.5))\n"
        "  print('moved', fx_move(e, {x = 20, y = 0, z = 0}, {x = -1, y = 0, z = 0}), e:pos().x, math.floor(e:yaw() + 0.5))\n"
        "  print('effect', fx(e, 'FX_CINEMATIC_8'), fx_stop(e, 'FX_NONE'))\n"
        "  print('stopped', fx_stop(e))\n"
        "  print('sound', sound(self, 'brute/roar'), sound(self, 'brute/roar', 'weapon'))\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "fx\ttrue\ttrue"));
    CHECK(has(g_events, "ae_startFX_Networked@mods/" SALT "/ai/brute(NULL,222);"));
    CHECK(has(g_events, "ae_stopFX_Networked@mods/" SALT "/ai/brute(NULL,222);"));
    CHECK(has(g_log, "placed\tfx/beam_impact\t10.0\t90"));
    CHECK(has(g_log, "moved\ttrue\t20.0\t180"));
    /* an effect entity answers no animation event: its conditions go to the engine's entity StartFX / StopFX */
    CHECK(has(g_log, "effect\ttrue\ttrue"));
    CHECK(has(g_events, "entity StartFX@mods/" SALT "/fx/beam_impact(222);"));
    CHECK(has(g_events, "entity StopFX@mods/" SALT "/fx/beam_impact(0);"));
    CHECK(!strstr(g_events, "ae_startFX_Networked@mods/" SALT "/fx/beam_impact"));
    CHECK(!strstr(g_events, "entity StartFX@mods/" SALT "/ai/brute"));
    CHECK(has(g_log, "stopped\ttrue"));
    CHECK(has(g_events, "post remove@mods/" SALT "/fx/beam_impact();"));
    CHECK(has(g_log, "sound\ttrue\ttrue"));
    CHECK(has(g_events, "startSoundShader@mods/" SALT "/ai/brute(d:mods/" SALT "/brute/roar,0);"));
    CHECK(has(g_events, "startSoundShader@mods/" SALT "/ai/brute(d:mods/" SALT "/brute/roar,7);"));
}

/* The hard stop rule: which stops end an action at once, which decls are named, and the call. */
static void test_fx_hard_stops(void)
{
    int type;
    /* a model, a render parm item, the two blurs and a screen overlay end at the stop, waiting or running */
    CHECK(sh_fx_hard_ends(SH_FX_TYPE_MODEL, 0, 0) == 1 && sh_fx_hard_ends(SH_FX_TYPE_MODEL, 0, 1) == 1);
    CHECK(sh_fx_hard_ends(SH_FX_TYPE_RENDERPARM, 0, 0) == 1);
    CHECK(sh_fx_hard_ends(SH_FX_TYPE_BLUR_RADIAL, 0, 0) == 1 && sh_fx_hard_ends(SH_FX_TYPE_BLUR_GAUSSIAN, 0, 0) == 1);
    CHECK(sh_fx_hard_ends(SH_FX_TYPE_SCREEN_OVERLAY, 0, 0) == 1);
    /* a particle fades as the game fades it, unless it is a screen particle or still waits out its delay */
    CHECK(sh_fx_hard_ends(SH_FX_TYPE_PARTICLE, 0, 0) == 0);
    CHECK(sh_fx_hard_ends(SH_FX_TYPE_PARTICLE, 1, 0) == 1);
    CHECK(sh_fx_hard_ends(SH_FX_TYPE_PARTICLE, 0, 1) == 1);
    /* a light fades; one that waits is never started */
    CHECK(sh_fx_hard_ends(SH_FX_TYPE_LIGHT, 0, 0) == 0 && sh_fx_hard_ends(SH_FX_TYPE_LIGHT, 0, 1) == 1);
    /* every other kind keeps the game's stop: decals, sounds, shakes, ribbons ... */
    for (type = 0; type <= 24; type++) {
        if (type == SH_FX_TYPE_LIGHT || type == SH_FX_TYPE_PARTICLE || type == SH_FX_TYPE_MODEL
            || type == SH_FX_TYPE_RENDERPARM || type == SH_FX_TYPE_BLUR_RADIAL || type == SH_FX_TYPE_BLUR_GAUSSIAN
            || type == SH_FX_TYPE_SCREEN_OVERLAY) continue;
        CHECK(sh_fx_hard_ends(type, 0, 0) == 0 && sh_fx_hard_ends(type, 1, 1) == 0);
    }

    /* names: whole, or a prefix that ends in a slash; the game's own decls are never named */
    sh_fx_hard_reset();
    CHECK(sh_fx_hard_count() == 0 && !sh_fx_hard_is("mods/" SALT "/character/brute"));
    CHECK(!sh_fx_hard_add(NULL) && !sh_fx_hard_add(""));
    CHECK(sh_fx_hard_add("mods/" SALT "/character/brute") && sh_fx_hard_add("mods/" SALT "/character/brute"));
    CHECK(sh_fx_hard_count() == 1);
    CHECK(sh_fx_hard_is("mods/" SALT "/character/brute") && sh_fx_hard_is("MODS/" SALT "/Character/Brute"));
    CHECK(!sh_fx_hard_is("mods/" SALT "/character/brute2") && !sh_fx_hard_is("mods/" SALT "/character"));
    CHECK(!sh_fx_hard_is("character/hellknight") && !sh_fx_hard_is(NULL) && !sh_fx_hard_is(""));
    CHECK(sh_fx_hard_add("mods/othersalt/"));
    CHECK(sh_fx_hard_is("mods/othersalt/weapon/rocket") && !sh_fx_hard_is("mods/othersal/weapon/rocket"));
    sh_fx_hard_reset();
    CHECK(sh_fx_hard_count() == 0 && !sh_fx_hard_is("mods/othersalt/weapon/rocket"));

    /* the call: one decl of the mod, then every decl of the mod; it asks the engine for the hook each time */
    g_hard_asked = 0;
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  print('one', fx_hard_stops('character/brute'))\n"
        "  print('all', fx_hard_stops())\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "one\ttrue"));
    CHECK(has(g_log, "all\ttrue"));
    CHECK(g_hard_asked == 2 && sh_fx_hard_count() == 2);
    CHECK(sh_fx_hard_is("mods/" SALT "/character/brute") && sh_fx_hard_is("mods/" SALT "/weapon/anything"));
    CHECK(!sh_fx_hard_is("mods/othersalt/weapon/rocket") && !sh_fx_hard_is("character/hellknight"));

    /* without the hook nothing is named and the call says so */
    sh_fx_hard_reset();
    g_hard_hook = 0;
    run("api = 1\non.spawn('ai/brute', function(self)\n  print('off', fx_hard_stops())\nend)\n");
    CHECK(has(log_now(), "off\tfalse"));
    CHECK(sh_fx_hard_count() == 0);
    g_hard_hook = 1;
    sh_fx_hard_reset();
}

static void test_tag_pain_sight(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  local o, f = self:tag('muzzle')\n"
        "  print('tag', o.x, o.z, math.floor(f.x * 100 + 0.5), math.floor(f.y * 100 + 0.5))\n"
        "  print('none', self:tag('nosuch'))\n"
        "  print('pain', self:pain(), ent(1):pain(), ent(3):pain())\n"
        "  print('sight', self:can_see_from(ent(2), 'muzzle'), self:can_see_from(ent(1), 'muzzle'))\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "tag\t10.0\t50.0\t0\t100"));
    CHECK(has(g_log, "none\tnil"));
    CHECK(has(g_log, "pain\tnil\tnil\tnil"));
    /* the player at (0, 200) is in the open; the imp at x = 300 is behind the wall */
    CHECK(has(g_log, "sight\ttrue\tfalse"));
    /* a reaction the engine set, read in a later frame */
    run("api = 1\n"
        "on.think('ai/brute', function(self) if self:pain() then print('painnow', self:pain(), ent(1):pain()) end end)\n");
    g_ents[E_AI].pain = 6;
    g_ents[E_STOCK_AI].pain = 9;
    frame();
    CHECK(has(log_now(), "painnow\tPAIN_FALTER\tPAIN_STAGGER"));
}

/* A tag is its own point and axis; every call that takes a joint takes one. */
static void test_tags(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  local o, f = self:tag('laser_source_1')\n"
        "  print('tag', o.x, o.z, f.x, f.z)\n"
        "  local j, g = self:tag('muzzle')\n"
        "  print('joint', j.x, j.z, math.floor(g.y * 100 + 0.5))\n"
        "  print('prop', ent(3):tag('laser_source_1'))\n"
        "  print('sight', self:can_see_from(ent(2), 'laser_source_1'))\n"
        "  local p = self:fire('rocket', 'laser_source_1', ent(2))\n"
        "  local v = p:velocity()\n"
        "  print('from tag', p:pos().x, p:pos().z, math.floor(v.y + 0.5), math.floor(v.z + 0.5))\n"
        "  local f = p:axis().forward\n"
        "  print('faces', (f.x * v.x + f.y * v.y + f.z * v.z) > 0.99 * math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z))\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "tag\t20.0\t80.0\t0.0\t1.0"));
    CHECK(has(g_log, "joint\t10.0\t50.0\t100"));      /* the joint form still answers */
    CHECK(has(g_log, "prop\tnil"));                       /* a prop is no demon: no tags, no such joint */
    CHECK(has(g_log, "sight\ttrue"));
    /* launched by the engine's event, then placed on the tag and sent to the player at (0, 200, 0) */
    CHECK(has(g_events, "ae_launchProjectileAtTarget@mods/" SALT "/ai/brute(NULL,d:mods/" SALT "/rocket,'laser_source_1','ent2',0);"));
    CHECK(has(g_log, "from tag\t20.0\t80.0\t555\t-222"));
    CHECK(g_launch_from == 1);                              /* started on the tag, not moved there */
    CHECK(has(g_log, "faces\ttrue"));              /* and turned to where it flies */
}

static void test_fire(void)
{
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  local p = self:fire('rocket', 'muzzle', ent(2))\n"
        "  print('fired', p:def(), p:pos().x, p:pos().z)\n"
        "  print('steer', p:steer({x = 10, y = 0, z = 1000}))\n"
        "  local v = p:velocity(); print('vel', math.floor(v.x + 0.5), math.floor(v.z + 0.5))\n"
        "  print('fast', p:steer(ent(2), 1200))\n"
        "  v = p:velocity(); print('vel2', math.floor(v.y + 0.5))\n"
        "  print('boom', p:explode(), p:alive())\n"
        "  local q = self:fire('rocket', 'muzzle', {x = 10, y = 0, z = 1000})\n"
        "  v = q:velocity(); print('at point', math.floor(v.x + 0.5), math.floor(v.z + 0.5))\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "fired\tprojectile/stock_rocket\t10.0\t50.0"));
    CHECK(has(g_events, "ae_launchProjectileAtTarget@mods/" SALT "/ai/brute(NULL,d:mods/" SALT "/rocket,'muzzle','ent2',0);"));
    CHECK(has(g_log, "steer\ttrue"));
    CHECK(has(g_log, "vel\t0\t600"));
    CHECK(has(g_log, "fast\ttrue"));
    CHECK(has(g_log, "vel2\t1163"));   /* toward the player at (0, 200, 0) from (10, 0, 50) at 1200 */
    CHECK(has(g_log, "boom\ttrue\tfalse"));
    /* a position target: launched at the AI itself (its own name), then steered at the position */
    CHECK(has(g_events, "ae_launchProjectileAtTarget@mods/" SALT "/ai/brute(NULL,d:mods/" SALT "/rocket,'muzzle','ent0',0);"));
    CHECK(has(g_log, "at point\t0\t600"));
}

static void test_refusals(void)
{
    refused("api = 1\non.spawn('ai/brute', function(self)\n  damage(ent(1), 'damage/nosuch', self)\nend)\n",
            "3", "\"damage\" found no damage decl \"damage/nosuch\" in this mod; check the name and that the file is under decls\\damage");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  damage(ent(1), 'mods/othermod/damage/rock')\nend)\n",
            "3", "\"damage\" can only use this mod's own content; \"mods/othermod/damage/rock\" belongs to another mod");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  damage(ent(1), 'damage/rock', 'self')\nend)\n",
            "3", "\"damage\" needs the attacker as an entity, got text");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  damage(ent(1))\nend)\n",
            "3", "\"damage\" needs the damage (the name of a damage decl of this mod, or of the game with game(name)), got nothing");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  hitscan(self, {x = 0, y = 0, z = 0}, 'damage/rock')\nend)\n",
            "3", "\"hitscan\" needs a direction that is not zero");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  hitscan(self, {x = 1, y = 0, z = 0}, 'damage/rock', 0)\nend)\n",
            "3", "\"hitscan\" needs a range of at least 1, got 0");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  fx(self, 'FX_NOPE')\nend)\n",
            "3", "\"fx\" knows no effect condition \"FX_NOPE\"; use one of the FX_ conditions the entity's fx decl lists");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  fx(42)\nend)\n",
            "3", "\"fx\" needs an entity and an effect condition, or the name of one of this mod's effect entities and a position; got number");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  fx_stop(self)\nend)\n",
            "3", "\"fx_stop\" without a condition removes an effect made by fx(effect, pos)");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  fx_hard_stops('mods/othersalt/character/x')\nend)\n",
            "3", "\"fx_hard_stops\" can only use this mod's own content");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  fx_move(ent(3), {x = 1, y = 2, z = 3})\nend)\n",
            "3", "\"fx_move\" moves an effect made by fx(effect, pos); this entity is not one");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  sound(self, 'brute/roar', 'loud')\nend)\n",
            "3", "\"sound\" knows no channel \"loud\"; use any, voice, body, weapon or item");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  ent(3):fire('rocket', 'muzzle', ent(2))\nend)\n",
            "3", "\"fire\" works on demons; this entity is not one");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  self:fire('damage/rock', 'muzzle', ent(2))\nend)\n",
            "3", "\"fire\" found no projectile decl \"damage/rock\" in this mod");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  self:fire('rocket', 'muzzle')\nend)\n",
            "3", "\"fire\" needs the target (an entity, or a position with x, y and z), got nothing");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  ent(3):steer({x = 1, y = 2, z = 3})\nend)\n",
            "3", "\"steer\" works on a projectile this mod's entities launched; this entity is not one");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  local p = self:fire('rocket', 'muzzle', ent(2))\n  p:steer(ent(2), -5)\nend)\n",
            "4", "\"steer\" needs a speed above 0, got -5");
    refused("api = 1\non.spawn('ai/brute', function(self)\n  self:tag(5)\nend)\n",
            "3", "\"tag\" needs the tag or joint name as text, got a number");
    refused("api = 1\non.frame_event('ai/brute', 'a:b', function(self, args) end)\n",
            "2", "\"on.frame_event\" needs a command name without \":\" or spaces");
}

/* The hooks' records, posted as the detours post them, reach the handlers. */
static void test_hook_records(void)
{
    sh_script_event ev;
    int mod;
    sh_script_handle self, imp, player, rocket;
    run("api = 1\n"
        "on.damage('ai/brute', function(self, hit)\n"
        "  print('damage', hit.amount, hit.damage, hit.location, hit.attacker:def(), hit.inflictor:def(), hit.direction.y)\n"
        "end)\n"
        "on.death('ai/brute', function(self, hit) print('death', hit.damage, hit.attacker:def()) end)\n"
        "on.anim('ai/brute', 'throw', function(self) print('anim throw') end)\n"
        "on.anim('ai/brute', 'fsm_window:Combat:0.5', function(self) print('anim whole') end)\n"
        "on.frame_event('ai/brute', 'FSM_Window', function(self, args)\n"
        "  if #args == 1 then print('long frame', #args[1]) return end\n"
        "  print('frame', #args, args[1], args[2] + 1, math.type(args[2]))\n"
        "end)\n"
        "on.frame_event('ai/brute', 'throw', function(self, args) print('frame throw', #args) end)\n"
        "on.pain('ai/brute', function(self, reaction, damage, attacker)\n"
        "  print('pain', reaction, damage, attacker and attacker:def())\n"
        "end)\n"
        "on.projectile('ai/brute', function(self, p, decl, origin, dir)\n"
        "  print('projectile', p:def(), decl, origin.x, dir.x)\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    mod = sh_script_hooks_mod_find(SALT);
    self = handle_of(E_AI);
    imp = sh_script_handles_ref(g_ents[E_STOCK_AI].id);
    player = sh_script_handles_ref(g_ents[E_PLAYER].id);
    rocket = sh_script_handles_ref(add_ent(SH_SCRIPT_KIND_OTHER, "projectile/stock_rocket", 7, 0, 0)->id);

    memset(&ev, 0, sizeof ev);
    ev.kind = SH_SCRIPT_EV_DAMAGE; ev.self = self; ev.other = player; ev.third = imp; ev.amount = 25;
    ev.text = "damage/rock";
    ev.text2 = "left_forearm";
    ev.vec[1] = 1; ev.flags = SH_SCRIPT_EVF_VEC;
    CHECK(sh_script_queue_push(mod, &ev) == 1);
    CHECK(sh_script_queue_post(mod, SH_SCRIPT_EV_ANIM, self, 0, 0, 0, "throw", NULL) == 1);
    CHECK(sh_script_queue_post(mod, SH_SCRIPT_EV_ANIM, self, 0, 0, 0, "fsm_window:Combat:0.5", NULL) == 1);
    {
        /* a row of 400 characters (the old record cut it at 127): the command and its argument arrive whole */
        static char longrow[420];
        memcpy(longrow, "fsm_window:", 11);
        memset(longrow + 11, 'x', 389);
        longrow[400] = 0;
        CHECK(sh_script_queue_post(mod, SH_SCRIPT_EV_ANIM, self, 0, 0, 0, longrow, NULL) == 1);
    }
    memset(&ev, 0, sizeof ev);
    ev.kind = SH_SCRIPT_EV_PAIN; ev.self = self; ev.other = player;
    ev.text = "PAIN_FALTER";
    ev.text2 = "damage/rock";
    CHECK(sh_script_queue_push(mod, &ev) == 1);
    memset(&ev, 0, sizeof ev);
    ev.kind = SH_SCRIPT_EV_PROJECTILE; ev.self = self; ev.other = rocket;
    ev.text = "rocket";
    ev.vec[0] = 7; ev.vec[3] = 1; ev.flags = SH_SCRIPT_EVF_VEC;
    CHECK(sh_script_queue_push(mod, &ev) == 1);
    CHECK(sh_script_queue_post(mod, SH_SCRIPT_EV_DEATH, self, player, imp, 0, "damage/rock", NULL) == 1);
    frame();
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "damage\t25.0\tdamage/rock\tleft_forearm\tplayer/sp\tai/demon/imp\t1.0"));
    CHECK(has(g_log, "anim throw"));
    CHECK(has(g_log, "frame throw\t0"));
    CHECK(has(g_log, "anim whole"));
    /* the command matches without case; the arguments keep the row's case; numbers are numbers */
    CHECK(has(g_log, "frame\t2\tCombat\t1.5\tfloat"));
    CHECK(has(g_log, "long frame\t389"));
    CHECK(has(g_log, "pain\tPAIN_FALTER\tdamage/rock\tplayer/sp"));
    CHECK(has(g_log, "projectile\tprojectile/stock_rocket\trocket\t7.0\t1.0"));
    CHECK(has(g_log, "death\tdamage/rock\tplayer/sp"));
}

/* A script holds its projectiles for as long as it likes. */
static void test_projectiles_held(void)
{
    int i, steered, gone_ok;
    run("api = 1\n"
        "rockets = {}\n"
        "on.spawn('ai/brute', function(self)\n"
        "  for i = 1, 300 do rockets[i] = self:fire('rocket', 'muzzle', ent(2)) end\n"
        "  every(0, function()\n"
        "    local n = 0\n"
        "    for i = 1, #rockets do if rockets[i]:steer({x = i, y = 0, z = 100}) then n = n + 1 end end\n"
        "    print('steered', n)\n"
        "  end)\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(sh_script_combat_launches() == 300);
    for (i = 0; i < 30; i++) frame();
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "steered\t300"));
    /* every rocket goes (they hit something); the script keeps its handles */
    for (i = 0; i < g_nents; i++) if (!strcmp(g_ents[i].def, "projectile/stock_rocket")) g_ents[i].alive = 0;
    for (i = 0; i < 5; i++) frame();
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "steered\t0"));
    CHECK(sh_script_combat_launches() == 0);   /* the records went with their projectiles */
    steered = 0;
    (void)steered;

    /* A handle used after its projectile is gone: nil, nil, nil, false; the mod runs on. */
    run("api = 1\n"
        "on.spawn('ai/brute', function(self)\n"
        "  local p = self:fire('rocket', 'muzzle', ent(2))\n"
        "  after(0.05, function()\n"
        "    print('gone', p:steer({x = 1, y = 2, z = 3}), p:explode(), p:pos(), p:alive(), tostring(p:steer(ent(2), 900)))\n"
        "  end)\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    for (i = 0; i < g_nents; i++) if (!strcmp(g_ents[i].def, "projectile/stock_rocket")) g_ents[i].alive = 0;
    for (i = 0; i < 6; i++) frame();
    gone_ok = sh_script_status(SALT) == SH_SCRIPT_RUNNING;
    CHECK(gone_ok);
    CHECK(has(log_now(), "gone\tnil\tnil\tnil\tfalse\tnil"));
    CHECK(sh_script_combat_launches() == 0);

    /* 5,000 alive at once, all steered in one frame. */
    run("api = 1\n"
        "rockets = {}\n"
        "on.spawn('ai/brute', function(self)\n"
        "  for i = 1, 5000 do rockets[i] = self:fire('rocket', 'muzzle', ent(2)) end\n"
        "  after(0, function()\n"
        "    local n = 0\n"
        "    for i = 1, #rockets do if rockets[i]:steer(ent(2), 800) then n = n + 1 end end\n"
        "    print('five thousand', n)\n"
        "  end)\n"
        "end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    frame();
    frame();
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "five thousand\t5000"));
    CHECK(sh_script_combat_launches() == 5000);
}

static void test_records(void)
{
    char *decl = NULL, longdecl[600];
    sh_script_handle a = 0;
    int mod = -1;
    sh_script_combat_reset();
    CHECK(!sh_script_combat_last_hit(7, &decl, &a) && decl == NULL);
    sh_script_combat_note_hit(7, "damage/rock", 9);
    sh_script_combat_note_hit(7, "damage/beam", 11);
    CHECK(sh_script_combat_last_hit(7, &decl, &a) && decl && !strcmp(decl, "damage/beam") && a == 11);
    free(decl);
    /* a decl name of any length is kept whole */
    memset(longdecl, 'd', sizeof longdecl - 1);
    longdecl[sizeof longdecl - 1] = 0;
    sh_script_combat_note_hit(7, longdecl, 12);
    CHECK(sh_script_combat_last_hit(7, &decl, &a) && decl && strlen(decl) == sizeof longdecl - 1 && a == 12);
    free(decl);
    CHECK(sh_script_combat_first_death(7) == 1);
    CHECK(sh_script_combat_first_death(7) == 0);
    CHECK(sh_script_combat_first_death(8) == 1);
    CHECK(!sh_script_combat_launched(0x1234, &mod, &a));
    sh_script_combat_note_launch(0x1234, 5, 2);
    CHECK(sh_script_combat_launched(0x1234, &mod, &a) && mod == 2 && a == 5);
    CHECK(!strcmp(sh_script_combat_pain_name(6), "PAIN_FALTER"));
    CHECK(sh_script_combat_pain_name(14) == NULL && sh_script_combat_pain_name(-1) == NULL);
    CHECK(sh_script_combat_channel("Weapon") == 7 && sh_script_combat_channel("item") == 14 && sh_script_combat_channel("x") == -1);
    sh_script_combat_reset();
    CHECK(!sh_script_combat_launched(0x1234, NULL, NULL));
    /* Many records at once, each released with its entity by the handle table's sweep. */
    {
        sh_script_handle_change ch[256];
        int i, n = 0;
        uint32_t base;
        run("api = 1\n");
        base = 0x20000u + (uint32_t)g_nents * 5u;
        for (i = 0; i < 3000; i++) {
            fent *p = add_ent(SH_SCRIPT_KIND_OTHER, "projectile/stock_rocket", 0, 0, 0);
            sh_script_handle h = sh_script_combat_launch(p->id, 5, 0);
            CHECK(h != 0);
            sh_script_combat_note_hit(h, "damage/rock", 9);
            CHECK(sh_script_combat_first_death(h) == 1);
        }
        CHECK(sh_script_combat_launches() == 3000);
        for (i = 0; i < 3000; i++) g_ents[(base - 0x20000u) / 5u + (uint32_t)i].alive = 0;
        while ((n = sh_script_handles_sweep(ch, 256)) > 0) {}
        CHECK(sh_script_combat_launches() == 0);
        CHECK(!sh_script_combat_launched(base, NULL, NULL));
    }
}

int main(void)
{
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "ent", t_ent);
    sh_script_api_set_engine(&k_engine);
    sh_script_api_install(NULL, 0, NULL);
    sh_script_combat_set_engine(&k_combat);
    sh_script_combat_register();
    sh_script_combat_register();   /* idempotent */
    sh_script_handles_set_resolver(h_resolve);

    test_damage();
    test_hitscan_radius();
    test_effects_sound();
    test_fx_hard_stops();
    test_tag_pain_sight();
    test_tags();
    test_fire();
    test_refusals();
    test_hook_records();
    test_projectiles_held();
    test_records();
    sh_script_map_end();
    CHECK(sh_script_bytes_in_use() == 0);
    printf(g_fail ? "script_api_combat_test: FAILED\n" : "script_api_combat_test: ok\n");
    return g_fail;
}
