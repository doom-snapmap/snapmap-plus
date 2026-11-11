/* script_api_entity.c -- the entity and animation calls of mod scripts
 * (API version 1): spawn, players, and the entity methods
 *   e:pos() e:set_pos(v) e:yaw() e:set_yaw(deg) e:axis() e:velocity()
 *   e:set_velocity(v) e:health() e:set_health(n) e:alive() e:def()
 *   e:distance(other) e:is_player() e:remove() e:view_angle_to(other, [use_view_pos])
 *   e:anim_state(node, [via]) e:anim_force(node, [blend]) e:anim_finished()
 *   e:anim_current() e:anim_event(name)
 * Reads work on any entity; a call that changes an entity works only on the
 * mod's own (placed from its entityDefs or spawned by its scripts), except
 * that any mod may heal a player. A removed entity answers nil (alive and
 * is_player answer false). Every refusal is one plain sentence at the
 * calling script line, raised before the engine is touched.
 *
 * Engine paths (script_api_common.h has the build 13954591 facts):
 *   position, axis    physics +0x808, vtable +0x80 / +0x88; set: entity SetOrigin +0x338, SetAxis +0x358,
 *                     UpdateVisuals +0x3C0 (the pair the engine's setOrigin event makes)
 *   velocity          getLinearVelocity / setLinearVelocity events
 *   health            idAI2: its idAIHealth (heal through the engine's Heal, lower by the field, never to
 *                     0: death comes from damage); idPlayer: getPlayerHealth / getPlayerMaxHealth /
 *                     increaseHealth events
 *   remove            the engine's remove event, queued
 *   animation         idAI2: ssaction_ChangeAnimState(Via) / ssaction_ForceAnimState (they act while the
 *                     AI is parked in its script state); other entities: changeAnimState(Via) /
 *                     forceAnimState / animState_IsFinished. idAI2 does not handle the latter four
 *                     (its dispatcher and its parents' have no case for events 326..331).
 *   anim_current      the idAI2 animWeb's current node; anim_event and anim_finished are that node
 *                     observed every frame against the last request (script_api_common.c)
 *   view angle        idPlayer: getViewAngles (its aim) and getViewPos; other entities: the axis forward
 *   spawn             idGameLocal +0x448 create, the pose, +0x458 finish, as idTarget_Spawn does */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "script_api_common.h"
#include "script_hooks.h"
#include "script_api_ai.h"   /* a demon's animation goes through its park (sh_script_ai_anim) */

#define PLAYER_SLOTS       64      /* players take the first entity numbers */
#define PATH_CAP           1024    /* a node name the engine reports (its own names) */
#define TEXT_ANY           ((size_t)-1 >> 1)
#define PI                 3.14159265358979323846

/* A call that cannot answer answers nil. */
static int nil1(lua_State *L)
{
    lua_pushnil(L);
    return 1;
}

static const sh_script_engine *eng(void)
{
    return sh_script_api_engine();
}

/* ------------------------------------------------------------ position */

static int origin_of(void *ent, float out[3])
{
    const sh_script_engine *e = eng();
    return ent && e && e->origin && e->origin(ent, out);
}

static int axis_of(void *ent, float out[9])
{
    const sh_script_engine *e = eng();
    return ent && e && e->axis && e->axis(ent, out);
}

static int l_pos(lua_State *L)
{
    float v[3];
    void *ent = sh_script_api_entity(L, 1, "pos", NULL);
    if (!origin_of(ent, v)) return nil1(L);
    sh_script_api_push_vec(L, v);
    return 1;
}

static int l_set_pos(lua_State *L)
{
    float v[3];
    const sh_script_engine *e = eng();
    void *ent;
    sh_script_api_check_vec(L, 2, "set_pos", "the new position", v);
    ent = sh_script_api_owned(L, 1, "set_pos", "move", NULL);
    if (ent && e && e->place) e->place(ent, v, NULL);
    return 0;
}

static double yaw_of_axis(const float ax[9])
{
    return atan2((double)ax[1], (double)ax[0]) * (180.0 / PI);
}

static int l_yaw(lua_State *L)
{
    float ax[9];
    void *ent = sh_script_api_entity(L, 1, "yaw", NULL);
    if (!axis_of(ent, ax)) return nil1(L);
    lua_pushnumber(L, yaw_of_axis(ax));
    return 1;
}

static int l_set_yaw(lua_State *L)
{
    const sh_script_engine *e = eng();
    double deg = sh_script_api_check_number(L, 2, "set_yaw", "the new facing in degrees");
    float ax[9];
    double r;
    void *ent = sh_script_api_owned(L, 1, "set_yaw", "turn", NULL);
    if (!ent || !e || !e->place) return 0;
    deg = fmod(deg, 360.0);
    r = deg * (PI / 180.0);
    ax[0] = (float)cos(r);  ax[1] = (float)sin(r); ax[2] = 0;
    ax[3] = -(float)sin(r); ax[4] = (float)cos(r); ax[5] = 0;
    ax[6] = 0;              ax[7] = 0;             ax[8] = 1;
    e->place(ent, NULL, ax);
    return 0;
}

static int l_axis(lua_State *L)
{
    float ax[9];
    void *ent = sh_script_api_entity(L, 1, "axis", NULL);
    if (!axis_of(ent, ax)) return nil1(L);
    lua_createtable(L, 0, 3);
    sh_script_api_push_vec(L, ax);     lua_setfield(L, -2, "forward");
    sh_script_api_push_vec(L, ax + 3); lua_setfield(L, -2, "left");
    sh_script_api_push_vec(L, ax + 6); lua_setfield(L, -2, "up");
    return 1;
}

static int l_velocity(lua_State *L)
{
    sh_script_earg r;
    void *ent = sh_script_api_entity(L, 1, "velocity", NULL);
    if (!ent || !sh_script_api_event(ent, "getLinearVelocity", NULL, 0, &r) || r.type != 'v') return nil1(L);
    sh_script_api_push_vec(L, r.v);
    return 1;
}

static int l_set_velocity(lua_State *L)
{
    float v[3];
    sh_script_earg a;
    void *ent;
    sh_script_api_check_vec(L, 2, "set_velocity", "the new velocity", v);
    ent = sh_script_api_owned(L, 1, "set_velocity", "push", NULL);
    if (!ent) return 0;
    a = sh_script_earg_vec(v);
    sh_script_api_event(ent, "setLinearVelocity", &a, 1, NULL);
    return 0;
}

static int l_distance(lua_State *L)
{
    float a[3], b[3];
    void *ent = sh_script_api_entity(L, 1, "distance", NULL);
    int have_b = sh_script_api_check_point(L, 2, "distance", "what to measure to", b);
    double dx, dy, dz;
    if (!have_b || !origin_of(ent, a)) return nil1(L);
    dx = (double)a[0] - b[0];
    dy = (double)a[1] - b[1];
    dz = (double)a[2] - b[2];
    lua_pushnumber(L, sqrt(dx * dx + dy * dy + dz * dz));
    return 1;
}

/* -------------------------------------------------------------- health */

static float earg_number(const sh_script_earg *r, int *ok)
{
    *ok = 1;
    if (r->type == 'f') return r->f;
    if (r->type == 'i') return (float)r->i;
    *ok = 0;
    return 0;
}

/* The entity's health and its maximum. 1 when it has health. */
static int health_of(void *ent, float *cur, float *max)
{
    const sh_script_engine *e = eng();
    int kind = sh_script_api_kind(ent);
    if (kind == SH_SCRIPT_KIND_AI) return e && e->ai_health && e->ai_health(ent, cur, max);
    if (kind == SH_SCRIPT_KIND_PLAYER) {
        sh_script_earg r;
        int ok = 0, ok2 = 0;
        if (!sh_script_api_event(ent, "getPlayerHealth", NULL, 0, &r)) return 0;
        *cur = earg_number(&r, &ok);
        if (!ok || !sh_script_api_event(ent, "getPlayerMaxHealth", NULL, 0, &r)) return 0;
        *max = earg_number(&r, &ok2);
        return ok2;
    }
    return 0;
}

static int l_health(lua_State *L)
{
    float cur, max;
    void *ent = sh_script_api_entity(L, 1, "health", NULL);
    if (!ent || !health_of(ent, &cur, &max)) return nil1(L);
    lua_pushnumber(L, cur);
    return 1;
}

static int l_set_health(lua_State *L)
{
    const sh_script_engine *e = eng();
    double n = sh_script_api_check_number(L, 2, "set_health", "the new health");
    sh_script_handle h = 0;
    void *ent = sh_script_api_entity(L, 1, "set_health", &h);
    float cur, max;
    int kind;
    if (n < 0) sh_script_refuse(L, "\"set_health\" needs a health of 0 or more, got %g", n);
    if (!ent) return 0;
    kind = sh_script_api_kind(ent);
    if (kind == SH_SCRIPT_KIND_PLAYER) {
        sh_script_earg a;
        if (!health_of(ent, &cur, &max)) return 0;
        if (n < cur)
            sh_script_refuse(L, "\"set_health\" can only heal a player; hurt players with damage(...) and this "
                                "mod's damage decl");
        if (n > max) n = max;
        if (n <= cur) return 0;
        a = sh_script_earg_float((float)(n - cur));
        sh_script_api_event(ent, "increaseHealth", &a, 1, NULL);
        return 0;
    }
    if (!sh_script_api_owns(L, h))
        sh_script_refuse(L, "\"set_health\" can only change the health of this mod's own entities and heal players; "
                            "this one is neither");
    if (kind != SH_SCRIPT_KIND_AI || !health_of(ent, &cur, &max))
        sh_script_refuse(L, "\"set_health\" was given an entity that has no health");
    if (n < 1)
        sh_script_refuse(L, "\"set_health\" cannot take health to 0; kill with damage(...) so the death plays");
    if (n > cur) { if (e->ai_heal) e->ai_heal(ent, (float)(n - cur)); }
    else if (n < cur) { if (e->ai_set_health) e->ai_set_health(ent, (float)n); }
    return 0;
}

static int l_alive(lua_State *L)
{
    float cur, max;
    void *ent = sh_script_api_entity(L, 1, "alive", NULL);
    int kind = sh_script_api_kind(ent);
    if (kind == SH_SCRIPT_KIND_GONE) lua_pushboolean(L, 0);
    else if (kind == SH_SCRIPT_KIND_AI || kind == SH_SCRIPT_KIND_PLAYER)
        lua_pushboolean(L, health_of(ent, &cur, &max) ? cur > 0 : 1);
    else lua_pushboolean(L, 1);
    return 1;
}

/* ---------------------------------------------------------------- name */

static int l_def(lua_State *L)
{
    const sh_script_engine *e = eng();
    char name[SH_SCRIPT_DEFNAME_CAP], own[SH_SCRIPT_SALT_CAP + 8];
    void *ent = sh_script_api_entity(L, 1, "def", NULL);
    size_t n;
    if (!ent || !e || !e->def_name || !e->def_name(ent, name, sizeof name)) return nil1(L);
    _snprintf_s(own, sizeof own, _TRUNCATE, "mods/%s/", sh_script_salt(L));
    n = strlen(own);
    if (!_strnicmp(name, own, n) && name[n]) lua_pushstring(L, name + n);   /* the mod's own name, as the script writes it */
    else lua_pushstring(L, name);
    return 1;
}

static int l_is_player(lua_State *L)
{
    void *ent = sh_script_api_entity(L, 1, "is_player", NULL);
    lua_pushboolean(L, sh_script_api_kind(ent) == SH_SCRIPT_KIND_PLAYER);
    return 1;
}

static int l_remove(lua_State *L)
{
    void *ent = sh_script_api_owned(L, 1, "remove", "remove", NULL);
    if (ent) sh_script_api_post(ent, "remove", NULL, 0);
    return 0;
}

/* ------------------------------------------------------------ aiming */

static void angles_dir(const float ang[3], float dir[3])
{
    double p = ang[0] * (PI / 180.0), y = ang[1] * (PI / 180.0);
    dir[0] = (float)(cos(p) * cos(y));
    dir[1] = (float)(cos(p) * sin(y));
    dir[2] = (float)(-sin(p));
}

static int l_view_angle_to(lua_State *L)
{
    float origin[3], dir[3], target[3], ax[9];
    int use_view = lua_toboolean(L, 3);
    void *ent = sh_script_api_entity(L, 1, "view_angle_to", NULL);
    int have = sh_script_api_check_point(L, 2, "view_angle_to", "what to measure the angle to", target);
    double tx, ty, tz, len, dot;
    if (!have || !ent || !origin_of(ent, origin)) return nil1(L);
    if (sh_script_api_kind(ent) == SH_SCRIPT_KIND_PLAYER) {
        sh_script_earg a = sh_script_earg_int(0), r;   /* includeWeaponKick = false */
        if (!sh_script_api_event(ent, "getViewAngles", &a, 1, &r) || r.type != 'v') return nil1(L);
        angles_dir(r.v, dir);
        if (use_view && sh_script_api_event(ent, "getViewPos", NULL, 0, &r) && r.type == 'v')
            memcpy(origin, r.v, sizeof origin);
    } else {
        if (!axis_of(ent, ax)) return nil1(L);
        memcpy(dir, ax, sizeof dir);
    }
    tx = (double)target[0] - origin[0];
    ty = (double)target[1] - origin[1];
    tz = (double)target[2] - origin[2];
    len = sqrt(tx * tx + ty * ty + tz * tz);
    if (len < 1e-6) { lua_pushnumber(L, 0); return 1; }
    dot = (tx * dir[0] + ty * dir[1] + tz * dir[2]) / len;
    len = sqrt((double)dir[0] * dir[0] + (double)dir[1] * dir[1] + (double)dir[2] * dir[2]);
    if (len < 1e-6) return nil1(L);
    dot /= len;
    if (dot > 1) dot = 1;
    if (dot < -1) dot = -1;
    lua_pushnumber(L, acos(dot) * (180.0 / PI));
    return 1;
}

/* ----------------------------------------------------------- animation */

/* "subweb/state", or the state alone in the AI's current subweb. */
/* The node's full "subweb/state" (malloc'd; NULL without memory). */
static char *anim_path(void *ent, const char *node)
{
    const sh_script_engine *e = eng();
    char cur[PATH_CAP];
    const char *slash;
    char *out;
    size_t n;
    if (strchr(node, '/') || sh_script_api_kind(ent) != SH_SCRIPT_KIND_AI || !e || !e->anim_node ||
        !e->anim_node(ent, cur, sizeof cur) || !(slash = strchr(cur, '/')))
        return _strdup(node);
    n = (size_t)(slash - cur) + 1 + strlen(node) + 1;
    out = (char *)malloc(n);
    if (out) _snprintf_s(out, n, _TRUNCATE, "%.*s/%s", (int)(slash - cur), cur, node);
    return out;
}

/* Split "subweb/state" in place for the stock entity events (no subweb: ""). */
static void split_path(char *path, const char **sub, const char **state)
{
    char *slash = strrchr(path, '/');
    *sub = "";
    *state = path;
    if (!slash) return;
    *slash = 0;
    *sub = path;
    *state = slash + 1;
}

static int l_anim_state(lua_State *L)
{
    const char *node = sh_script_api_check_text(L, 2, "anim_state", "the animWeb node to play", TEXT_ANY);
    const char *via = lua_isnoneornil(L, 3) ? NULL :
                      sh_script_api_check_text(L, 3, "anim_state", "the node to pass through", TEXT_ANY);
    sh_script_handle h = 0;
    void *ent = sh_script_api_owned(L, 1, "anim_state", "animate", &h);
    char *dest, *vpath = NULL;
    if (!ent) return 0;
    dest = anim_path(ent, node);
    if (via) vpath = anim_path(ent, via);
    if (!dest || (via && !vpath)) { free(dest); free(vpath); return luaL_error(L, "not enough memory"); }
    sh_script_api_anim_request(h, dest, via ? vpath : NULL);
    if (sh_script_api_kind(ent) == SH_SCRIPT_KIND_AI) {
        /* idAI2 ignores the stock animWeb events: the designer commands act
         * on a demon parked in its script state, so the AI calls park it and
         * run them (ssaction_ChangeAnimState, ..Via). */
        (void)sh_script_ai_anim(ent, h, dest, via ? vpath : NULL, 0, 0);
    } else {
        sh_script_earg a[4];
        const char *sub, *state, *vsub, *vstate;
        split_path(dest, &sub, &state);
        a[0] = sh_script_earg_text('s', sub);
        a[1] = sh_script_earg_text('s', state);
        if (via) {
            split_path(vpath, &vsub, &vstate);
            a[2] = sh_script_earg_text('s', vsub);
            a[3] = sh_script_earg_text('s', vstate);
            sh_script_api_post(ent, "changeAnimStateVia", a, 4);
        } else {
            sh_script_api_post(ent, "changeAnimState", a, 2);
        }
    }
    free(dest);
    free(vpath);
    return 0;
}

static int l_anim_force(lua_State *L)
{
    const char *node = sh_script_api_check_text(L, 2, "anim_force", "the animWeb node to play", TEXT_ANY);
    double blend = 0;
    sh_script_handle h = 0;
    void *ent;
    char *dest;
    sh_script_earg a[4];
    int ms;
    if (sh_script_api_opt_number(L, 3, "anim_force", "the blend time in seconds", &blend) && !(blend >= 0))
        sh_script_refuse(L, "\"anim_force\" needs a blend time of 0 seconds or more, got %g", blend);
    ent = sh_script_api_owned(L, 1, "anim_force", "animate", &h);
    if (!ent) return 0;
    ms = (int)(blend * 1000.0 + 0.5);
    dest = anim_path(ent, node);
    if (!dest) return luaL_error(L, "not enough memory");
    sh_script_api_anim_request(h, dest, NULL);
    if (sh_script_api_kind(ent) == SH_SCRIPT_KIND_AI) {
        (void)sh_script_ai_anim(ent, h, dest, NULL, 1, ms);   /* ssaction_ForceAnimState, parked */
    } else {
        const char *sub, *state;
        split_path(dest, &sub, &state);
        a[0] = sh_script_earg_text('s', sub);
        a[1] = sh_script_earg_text('s', state);
        a[2] = sh_script_earg_int(ms);
        sh_script_api_post(ent, "forceAnimState", a, 3);
    }
    free(dest);
    return 0;
}

static int l_anim_current(lua_State *L)
{
    const sh_script_engine *e = eng();
    char node[PATH_CAP];
    void *ent = sh_script_api_entity(L, 1, "anim_current", NULL);
    if (sh_script_api_kind(ent) != SH_SCRIPT_KIND_AI || !e || !e->anim_node || !e->anim_node(ent, node, sizeof node))
        return nil1(L);
    lua_pushstring(L, node);
    return 1;
}

static const struct { const char *name; unsigned bit; } k_anim_events[] = {
    { "DEST_BLEND_START", SH_SCRIPT_ANIM_DEST },
    { "DEST_REACHED", SH_SCRIPT_ANIM_DEST },
    { "VIA_BLEND_IN_START", SH_SCRIPT_ANIM_VIA_IN },
    { "VIA_BLEND_OUT_START", SH_SCRIPT_ANIM_VIA_OUT },
    { "FINISHED", SH_SCRIPT_ANIM_FINISHED },
};

static int l_anim_event(lua_State *L)
{
    const char *name = sh_script_api_check_text(L, 2, "anim_event", "the event name", TEXT_ANY);
    const char *bare = _strnicmp(name, "AWCMD_EVENT_", 12) ? name : name + 12;
    sh_script_handle h = 0;
    void *ent;
    unsigned bit = 0, flags;
    int i, requested;
    for (i = 0; i < (int)(sizeof k_anim_events / sizeof k_anim_events[0]); i++)
        if (!_stricmp(bare, k_anim_events[i].name)) bit = k_anim_events[i].bit;
    if (!bit)
        sh_script_refuse(L, "\"anim_event\" knows DEST_BLEND_START, DEST_REACHED, VIA_BLEND_IN_START, "
                            "VIA_BLEND_OUT_START and FINISHED, not \"%s\"", name);
    ent = sh_script_api_entity(L, 1, "anim_event", &h);
    if (sh_script_api_kind(ent) != SH_SCRIPT_KIND_AI) return nil1(L);
    flags = sh_script_api_anim_flags(h, ent, &requested);
    lua_pushboolean(L, requested && (flags & bit) != 0);
    return 1;
}

static int l_anim_finished(lua_State *L)
{
    sh_script_handle h = 0;
    void *ent = sh_script_api_entity(L, 1, "anim_finished", &h);
    int requested = 0;
    unsigned flags;
    if (!ent) return nil1(L);
    if (sh_script_api_kind(ent) == SH_SCRIPT_KIND_AI) {
        flags = sh_script_api_anim_flags(h, ent, &requested);
        lua_pushboolean(L, requested && (flags & SH_SCRIPT_ANIM_FINISHED) != 0);
        return 1;
    }
    {
        /* other entities: the engine's own answer for the last request */
        const char *held = sh_script_api_anim_dest_of(h), *sub, *state;
        char *dest;
        sh_script_earg a[2], r;
        int ok = 0, ran;
        float v;
        if (!held) { lua_pushboolean(L, 0); return 1; }
        if (!(dest = _strdup(held))) return luaL_error(L, "not enough memory");
        split_path(dest, &sub, &state);
        a[0] = sh_script_earg_text('s', sub);
        a[1] = sh_script_earg_text('s', state);
        ran = sh_script_api_event(ent, "animState_IsFinished", a, 2, &r);
        free(dest);
        if (!ran) return nil1(L);
        v = earg_number(&r, &ok);
        if (!ok && r.type == 'b') { ok = 1; v = (float)r.i; }
        if (!ok) return nil1(L);
        lua_pushboolean(L, v != 0);
        return 1;
    }
}

/* -------------------------------------------------------------- spawn */

static int l_spawn(lua_State *L)
{
    const sh_script_engine *e = eng();
    char def[SH_SCRIPT_DEFNAME_CAP];
    float pos[3];
    double yaw = 0;
    const char *why = NULL;
    void *ent;
    int mod = -1;
    sh_script_handle h;
    sh_script_check_name(L, 1, "spawn", 0, def, sizeof def, NULL);
    sh_script_api_check_vec(L, 2, "spawn", "where to spawn", pos);
    sh_script_api_opt_number(L, 3, "spawn", "the facing in degrees", &yaw);
    sh_script_api_check_room(L, "spawn");
    if (!e || !e->spawn) return nil1(L);
    ent = e->spawn(def, pos, (float)fmod(yaw, 360.0), &why);
    if (!ent) {
        if (why) sh_script_refuse(L, "\"spawn\" could not spawn \"%s\": %s", lua_tostring(L, 1), why);
        return nil1(L);
    }
    h = e->spawn_id ? sh_script_handles_find(e->spawn_id(ent), &mod) : 0;
    if (!h || mod != sh_script_hooks_mod_find(sh_script_salt(L))) {
        /* the spawn hook could not record it for this mod (no memory) */
        sh_script_api_post(ent, "remove", NULL, 0);
        return nil1(L);
    }
    sh_script_push_entity(L, h);
    return 1;
}

/* ------------------------------------------------------------ players */

static int l_players(lua_State *L)
{
    const sh_script_engine *e = eng();
    int num, n = 0;
    lua_newtable(L);
    if (!e || !e->slot_spawn_id || !e->resolve) return 1;
    for (num = 0; num < PLAYER_SLOTS && num < (e->slots ? e->slots() : PLAYER_SLOTS); num++) {
        uint32_t id = e->slot_spawn_id(num);
        void *ent;
        sh_script_handle h;
        if (id == SH_SCRIPT_SPAWN_ID_NONE || id == SH_SCRIPT_SPAWN_ID_WORLD) continue;
        ent = e->resolve(id);
        if (sh_script_api_kind(ent) != SH_SCRIPT_KIND_PLAYER) continue;
        h = sh_script_api_handle_of(ent);
        if (!h) continue;
        sh_script_push_entity(L, h);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

/* ------------------------------------------------------- registration */

void sh_script_api_entity_register(void)
{
    static const struct { int kind; const char *name; lua_CFunction fn; } calls[] = {
        { SH_SCRIPT_API_GLOBAL, "spawn", l_spawn },
        { SH_SCRIPT_API_GLOBAL, "players", l_players },
        { SH_SCRIPT_API_METHOD, "pos", l_pos },
        { SH_SCRIPT_API_METHOD, "set_pos", l_set_pos },
        { SH_SCRIPT_API_METHOD, "yaw", l_yaw },
        { SH_SCRIPT_API_METHOD, "set_yaw", l_set_yaw },
        { SH_SCRIPT_API_METHOD, "axis", l_axis },
        { SH_SCRIPT_API_METHOD, "velocity", l_velocity },
        { SH_SCRIPT_API_METHOD, "set_velocity", l_set_velocity },
        { SH_SCRIPT_API_METHOD, "health", l_health },
        { SH_SCRIPT_API_METHOD, "set_health", l_set_health },
        { SH_SCRIPT_API_METHOD, "alive", l_alive },
        { SH_SCRIPT_API_METHOD, "def", l_def },
        { SH_SCRIPT_API_METHOD, "distance", l_distance },
        { SH_SCRIPT_API_METHOD, "is_player", l_is_player },
        { SH_SCRIPT_API_METHOD, "remove", l_remove },
        { SH_SCRIPT_API_METHOD, "view_angle_to", l_view_angle_to },
        { SH_SCRIPT_API_METHOD, "anim_state", l_anim_state },
        { SH_SCRIPT_API_METHOD, "anim_force", l_anim_force },
        { SH_SCRIPT_API_METHOD, "anim_finished", l_anim_finished },
        { SH_SCRIPT_API_METHOD, "anim_current", l_anim_current },
        { SH_SCRIPT_API_METHOD, "anim_event", l_anim_event },
    };
    size_t i;
    for (i = 0; i < sizeof calls / sizeof calls[0]; i++) sh_script_api_add(calls[i].kind, calls[i].name, calls[i].fn);
}
