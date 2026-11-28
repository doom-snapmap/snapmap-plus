/* script_api_entity.c -- the entity and animation calls of mod scripts
 * (API version 1): spawn, players, and the entity methods
 *   e:position() e:set_position(v) e:yaw() e:set_yaw(deg) e:axis() e:velocity()
 *   e:set_velocity(v) e:health() e:set_health(n) e:alive() e:definition()
 *   e:distance(other) e:is_player() e:remove() e:view_angle_to(other, [use_view_pos])
 *   e:animation_state(node, [via]) e:force_animation(node, [blend]) e:animation_finished()
 *   e:current_animation() e:animation_event(name) e:tint(r, g, b, [seconds]) e:glow(r, g, b, [seconds])
 * Reads work on any entity; a call that changes an entity works only on the mod's own (placed from its entityDefs
 * or spawned by its scripts), except that any mod may heal a player. A removed entity answers nil (alive and
 * is_player answer false). Every refusal is one plain sentence at the calling script line, raised before the
 * engine is touched.
 *
 * Engine paths (script_api_common.h has the build 13954591 facts):
 *   position, axis    physics +0x808, vtable +0x80 / +0x88; set: SetOrigin +0x338, SetAxis +0x358, UpdateVisuals +0x3C0
 *   velocity          getLinearVelocity / setLinearVelocity events
 *   health            idAI2: its idAIHealth (heal through Heal, lower by the field, never to 0: death comes from
 *                     damage); idPlayer: getPlayerHealth / getPlayerMaxHealth / increaseHealth events
 *   remove            the engine's remove event, queued
 *   animation         idAI2: ssaction_ChangeAnimState(Via) / ssaction_ForceAnimState (they act while the AI is
 *                     parked in its script state; the stock events 326..331 have no idAI2 case); other entities:
 *                     changeAnimState(Via) / forceAnimState / animState_IsFinished
 *   current_animation the idAI2 animWeb's current node; animation_event and animation_finished observe it every
 *                     frame against the last request (script_api_common.c)
 *   view angle        idPlayer: getViewAngles and getViewPos; other entities: the axis forward
 *   tint, glow        the lerpRenderParmFrom event on the parms color, bloomcolor and highlightcolor, found as
 *                     renderParm decls by name (script_api_tint.h)
 *   spawn             idGameLocal +0x448 create, the pose, +0x458 finish, as idTarget_Spawn does */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../common/text.h"
#include "script_api_common.h"
#include "script_internal.h"
#include "script_hooks.h"
#include "script_api_ai.h"   /* a demon's animation goes through its park (sh_script_ai_anim) */
#include "script_api_tint.h"

#define PLAYER_SLOTS       64      /* players take the first entity numbers */
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
    void *ent = sh_script_api_entity(L, 1, "position", NULL);
    if (!origin_of(ent, v)) return nil1(L);
    sh_script_api_push_vec(L, v);
    return 1;
}

static int l_set_pos(lua_State *L)
{
    float v[3];
    const sh_script_engine *e = eng();
    void *ent;
    sh_script_api_check_vec(L, 2, "set_position", "the new position", v);
    ent = sh_script_api_owned(L, 1, "set_position", "move", NULL);
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

/* Blend the forward direction in the body's own plane, retaining its up axis on slopes. */
static int l_face_body(lua_State *L)
{
    const sh_script_engine *e = eng();
    float goal[3], at[3], axis[9];
    double factor = 0.1, min_angle = 0, forward = 0, left = 0, distance_sq = 0, length;
    void *ent;
    int i, wrote = 0;
    int has_goal = sh_script_api_check_point(L, 2, "face_body", "the target", goal);
    sh_script_api_opt_number(L, 3, "face_body", "the blend fraction", &factor);
    sh_script_api_opt_number(L, 4, "face_body", "the minimum angle in degrees", &min_angle);
    if (factor < 0 || factor > 1)
        sh_script_refuse(L, "\"face_body\" needs the blend fraction between 0 and 1");
    ent = sh_script_api_owned(L, 1, "face_body", "turn", NULL);
    if (!has_goal || !origin_of(ent, at) || !e->body_axis || !e->place || !e->body_axis(ent, axis))
        goto done;
    for (i = 0; i < 9; i++) if (!isfinite(axis[i])) goto done;
    for (i = 0; i < 3; i++) {
        double delta = (double)goal[i] - at[i];
        distance_sq += delta * delta;
        forward += delta * axis[i];
        left += delta * axis[i + 3];
    }
    length = hypot(forward, left);
    if (!isfinite(length) || !(length > 1e-6 * fmax(1, sqrt(distance_sq)))) goto done;
    forward /= length;
    left /= length;
    if (min_angle > 0 && forward > cos(fmod(min_angle, 360.0) * (PI / 180.0))) goto done;
    forward = 1 + factor * (forward - 1);
    left *= factor;
    length = hypot(forward, left);
    if (!(length > 1e-12)) goto done;
    forward /= length;
    left /= length;
    for (i = 0; i < 3; i++) {
        double f = axis[i], l = axis[i + 3];
        axis[i] = (float)(forward * f + left * l);
        axis[i + 3] = (float)(forward * l - left * f);
    }
    wrote = e->place(ent, NULL, axis);
done:
    lua_pushboolean(L, wrote);
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
    return sh_script_api_health(ent, cur, max);
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

static int l_def(lua_State *L)
{
    const sh_script_engine *e = eng();
    char own[SH_SCRIPT_SALT_CAP + 8], *name = NULL;
    void *ent = sh_script_api_entity(L, 1, "definition", NULL);
    size_t n, cap;
    if (!ent || !e || !e->def_name) return nil1(L);
    /* the engine's name whole: read again with twice the room while it fills the buffer */
    for (cap = 256;; cap *= 2) {
        char *grown = (char *)realloc(name, cap);
        if (!grown) { free(name); return luaL_error(L, "not enough memory"); }
        name = grown;
        if (!e->def_name(ent, name, cap)) { free(name); return nil1(L); }
        if (strlen(name) + 1 < cap) break;
    }
    _snprintf_s(own, sizeof own, _TRUNCATE, "mods/%s/", sh_script_salt(L));
    n = strlen(own);
    lua_pushstring(L, !_strnicmp(name, own, n) && name[n] ? name + n : name);   /* the mod's own name, as the script writes it */
    free(name);
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

/* The node's full "subweb/state" from "subweb/state" or the state alone in the AI's current subweb (malloc'd;
 * NULL without memory). */
static char *anim_path(void *ent, const char *node)
{
    const sh_script_engine *e = eng();
    char *cur, *out;
    const char *slash;
    if (strchr(node, '/') || sh_script_api_kind(ent) != SH_SCRIPT_KIND_AI || !e || !e->anim_node ||
        !(cur = e->anim_node(ent)))
        return _strdup(node);
    slash = strchr(cur, '/');
    out = slash ? sh_textf("%.*s/%s", (int)(slash - cur), cur, node) : _strdup(node);
    free(cur);
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
    const char *node = sh_script_api_check_text(L, 2, "animation_state", "the animWeb node to play", SH_SCRIPT_TEXT_ANY);
    const char *via = lua_isnoneornil(L, 3) ? NULL :
                      sh_script_api_check_text(L, 3, "animation_state", "the node to pass through", SH_SCRIPT_TEXT_ANY);
    sh_script_handle h = 0;
    void *ent = sh_script_api_owned(L, 1, "animation_state", "animate", &h);
    char *dest, *vpath = NULL;
    if (!ent) return 0;
    dest = anim_path(ent, node);
    if (via) vpath = anim_path(ent, via);
    if (!dest || (via && !vpath)) { free(dest); free(vpath); return luaL_error(L, "not enough memory"); }
    sh_script_api_anim_request(h, dest, via ? vpath : NULL);
    if (sh_script_api_kind(ent) == SH_SCRIPT_KIND_AI) {
        /* idAI2 ignores the stock animWeb events: its ssaction_ChangeAnimState(Via) act on a demon parked in its
         * script state, so the AI calls park it and run them. */
        (void)sh_script_ai_anim(ent, h, dest, via ? vpath : NULL, SH_AI_ORDER_CHANGE, 0);
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
    const char *node = sh_script_api_check_text(L, 2, "force_animation", "the animWeb node to play", SH_SCRIPT_TEXT_ANY);
    double blend = 0;
    sh_script_handle h = 0;
    void *ent;
    char *dest;
    sh_script_earg a[4];
    int ms;
    if (sh_script_api_opt_number(L, 3, "force_animation", "the blend time in seconds", &blend) && !(blend >= 0))
        sh_script_refuse(L, "\"force_animation\" needs a blend time of 0 seconds or more, got %g", blend);
    ent = sh_script_api_owned(L, 1, "force_animation", "animate", &h);
    if (!ent) return 0;
    ms = (int)(blend * 1000.0 + 0.5);
    dest = anim_path(ent, node);
    if (!dest) return luaL_error(L, "not enough memory");
    sh_script_api_anim_request(h, dest, NULL);
    if (sh_script_api_kind(ent) == SH_SCRIPT_KIND_AI) {
        (void)sh_script_ai_anim(ent, h, dest, NULL, SH_AI_ORDER_FORCE, ms);   /* ssaction_ForceAnimState, parked */
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
    char *node;
    void *ent = sh_script_api_entity(L, 1, "current_animation", NULL);
    if (sh_script_api_kind(ent) != SH_SCRIPT_KIND_AI || !e || !e->anim_node || !(node = e->anim_node(ent)))
        return nil1(L);
    lua_pushstring(L, node);
    free(node);
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
    const char *name = sh_script_api_check_text(L, 2, "animation_event", "the event name", SH_SCRIPT_TEXT_ANY);
    const char *bare = _strnicmp(name, "AWCMD_EVENT_", 12) ? name : name + 12;
    sh_script_handle h = 0;
    void *ent;
    unsigned bit = 0, flags;
    int i, requested;
    for (i = 0; i < (int)(sizeof k_anim_events / sizeof k_anim_events[0]); i++)
        if (!_stricmp(bare, k_anim_events[i].name)) bit = k_anim_events[i].bit;
    if (!bit)
        sh_script_refuse(L, "\"animation_event\" knows DEST_BLEND_START, DEST_REACHED, VIA_BLEND_IN_START, "
                            "VIA_BLEND_OUT_START and FINISHED, not \"%s\"", name);
    ent = sh_script_api_entity(L, 1, "animation_event", &h);
    if (sh_script_api_kind(ent) != SH_SCRIPT_KIND_AI) return nil1(L);
    flags = sh_script_api_anim_flags(h, ent, &requested);
    lua_pushboolean(L, requested && (flags & bit) != 0);
    return 1;
}

static int l_anim_finished(lua_State *L)
{
    sh_script_handle h = 0;
    void *ent = sh_script_api_entity(L, 1, "animation_finished", &h);
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

/* Spawns `def` and records it for the mod; `is_game` when the name is one of the game's (or one the product
 * serves), which the spawn hook does not record by itself. `shown` names it in a refusal. */
static int spawn_recorded(lua_State *L, const char *def, const char *shown, const float pos[3], double yaw, int is_game)
{
    const sh_script_engine *e = eng();
    const char *why = NULL;
    void *ent;
    int mod = -1;
    sh_script_handle h;
    if (!e || !e->spawn) return nil1(L);
    ent = e->spawn(def, pos, (float)fmod(yaw, 360.0), &why);
    if (!ent) {
        if (why) sh_script_refuse(L, "\"spawn\" could not spawn \"%s\": %s", shown, why);
        return nil1(L);
    }
    h = e->spawn_id ? sh_script_handles_find(e->spawn_id(ent), &mod) : 0;
    if (is_game && e->spawn_id && (!h || mod != sh_script_hooks_mod_find(sh_script_salt(L)))) {
        /* one of the game's own entityDefs: the script that spawned it owns it */
        mod = sh_script_hooks_mod_find(sh_script_salt(L));
        h = mod >= 0 ? sh_script_handles_add_named(mod, e->spawn_id(ent), def, NULL) : 0;
    }
    if (!h || mod != sh_script_hooks_mod_find(sh_script_salt(L))) {
        /* the spawn hook could not record it for this mod (no memory) */
        sh_script_api_post(ent, "remove", NULL, 0);
        return nil1(L);
    }
    sh_script_push_entity(L, h);
    return 1;
}

int sh_script_api_spawn_def(lua_State *L, const char *def, const float pos[3], double yaw)
{
    return spawn_recorded(L, def, def, pos, yaw, 1);
}

static int l_spawn(lua_State *L)
{
    const char *def;
    float pos[3];
    double yaw = 0;
    int is_game = 0;
    def = sh_script_check_name(L, 1, "spawn", 1, &is_game);
    sh_script_api_check_vec(L, 2, "spawn", "where to spawn", pos);
    sh_script_api_opt_number(L, 3, "spawn", "the facing in degrees", &yaw);
    sh_script_api_check_room(L, "spawn");
    return spawn_recorded(L, def, lua_tostring(L, 1), pos, yaw, is_game);
}

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

static sh_tint_table g_tint;

/* tint(r, g, b, [seconds]) and glow(r, g, b, [seconds], [strength]): the entity's look (script_api_tint.h). Answers
 * true when the engine took all three parms, false when it did not (no render model), nil when the entity is gone
 * or the render parms are not found. */
static int tint_call(lua_State *L, const char *call, int kind)
{
    static const char *const what[3] = { "the red", "the green", "the blue" };
    const sh_script_engine *e = eng();
    const double max = kind ? SH_TINT_MAX_GLOW : SH_TINT_MAX_TINT;
    sh_script_handle h = 0;
    double seconds = 0.0, c;
    char err[160];
    float rgb[3];
    int i, ok = 0, now = 0, ms;
    void *ent, *decl[SH_TINT_PARMS];
    sh_tint_state from, to;
    sh_tint_parm a[SH_TINT_PARMS], b[SH_TINT_PARMS];
    ent = sh_script_api_owned(L, 1, call, kind ? "make glow" : "tint", &h);
    for (i = 0; i < 3; i++) {
        c = sh_script_api_check_number(L, 2 + i, call, what[i]);
        if (!sh_tint_check_channel(call, what[i], c, max, err, sizeof err)) sh_script_refuse(L, "%s", err);
        rgb[i] = (float)c;
    }
    if (sh_script_api_opt_number(L, 5, call, "the seconds to fade over", &seconds) &&
        !sh_tint_check_seconds(call, seconds, err, sizeof err))
        sh_script_refuse(L, "%s", err);
    if (kind) {
        /* glow's strength: the soft default unless the script names one (script_api_tint.h) */
        double strength = SH_TINT_GLOW_STRENGTH_DEFAULT;
        if (sh_script_api_opt_number(L, 6, call, "the strength", &strength) &&
            !sh_tint_check_strength(call, strength, err, sizeof err))
            sh_script_refuse(L, "%s", err);
        if (!sh_tint_glow_scaled(call, rgb, strength, err, sizeof err)) sh_script_refuse(L, "%s", err);
    }
    if (!ent || !e || !e->decl) return nil1(L);
    sh_tint_neutral(&from);
    sh_tint_parms(&from, a);
    for (i = 0; i < SH_TINT_PARMS; i++) {
        decl[i] = e->decl("renderParm", a[i].name);
        if (!decl[i]) return nil1(L);
    }
    (void)sh_script_api_now_ms(&now);
    ms = sh_tint_duration_ms(seconds);
    sh_tint_begin(&g_tint, h, kind, rgb, now, ms, &from, &to);
    sh_tint_parms(&from, a);
    sh_tint_parms(&to, b);
    ok = 1;
    for (i = 0; i < SH_TINT_PARMS; i++) {
        sh_script_earg ev[6];
        memset(ev, 0, sizeof ev);
        ev[0].type = 'd'; ev[0].p = decl[i];
        ev[1] = sh_script_earg_vec(a[i].v);
        ev[2] = sh_script_earg_float(a[i].v[3]);
        ev[3] = sh_script_earg_vec(b[i].v);
        ev[4] = sh_script_earg_float(b[i].v[3]);
        ev[5] = sh_script_earg_int(ms);
        if (!sh_script_api_event(ent, "lerpRenderParmFrom", ev, 6, NULL)) ok = 0;
    }
    lua_pushboolean(L, ok);
    return 1;
}

/* e:recolour{ primary = {r,g,b}, secondary = ..., pattern = { {r,g,b}, {r,g,b} }, scratches = ..., dirt = ...,
 * glow = ..., seconds = s }: the colour regions of DOOM 2016's customization template (skinvmtr_custom and its
 * family), which the engine writes per render entity (VK FUN_140fdeb70 / FUN_140fdf770) through the parm set
 * lerpRenderParmFrom uses. Colours are sRGB decoded; a region keeps its last colour per entity so a fade starts
 * from it. Answers true when every parm was sent; an entity without colour regions is unchanged. */
#include "../sdkwords.h"

typedef struct recolour_rec { sh_script_handle h; int parm; float v[4]; } recolour_rec;
static recolour_rec g_recolour[512];
static int g_recolour_next;

static recolour_rec *recolour_find(sh_script_handle h, int parm, int make)
{
    int i;
    for (i = 0; i < (int)(sizeof g_recolour / sizeof g_recolour[0]); i++)
        if (g_recolour[i].h == h && g_recolour[i].parm == parm) return &g_recolour[i];
    if (!make) return NULL;
    i = g_recolour_next;
    g_recolour_next = (g_recolour_next + 1) % (int)(sizeof g_recolour / sizeof g_recolour[0]);
    g_recolour[i].h = h;
    g_recolour[i].parm = parm;
    return &g_recolour[i];
}

static int read_colour(lua_State *L, int idx, const char *call, const char *what, float out[4], int linear)
{
    int k, n;
    if (!lua_istable(L, idx)) sh_script_refuse(L, "\"%s\" needs %s as { r, g, b }, got %s", call, what, luaL_typename(L, idx));
    n = (int)lua_rawlen(L, idx);
    if (n < 3 || n > 4) sh_script_refuse(L, "\"%s\" needs %s as { r, g, b }", call, what);
    out[3] = 1.0f;
    for (k = 0; k < n; k++) {
        double x;
        lua_rawgeti(L, idx, k + 1);
        if (!lua_isnumber(L, -1)) sh_script_refuse(L, "\"%s\" needs numbers in %s", call, what);
        x = lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (!(x >= 0.0 && x <= 1.0)) sh_script_refuse(L, "\"%s\" needs %s from 0 to 1, got %g", call, what, x);
        out[k] = linear && k < 3 ? (float)sdk_srgb_decode(x) : (float)x;
    }
    return 1;
}

static int l_recolour(lua_State *L)
{
    static const struct { sdk_word_id word; const char *parm[2]; } REGIONS[] = {
        { SDK_R_PRIMARY, { "customprimaryalbedo", NULL } },     { SDK_R_SECONDARY, { "customsecondaryalbedo", NULL } },
        { SDK_R_PATTERN, { "custompatternalbedo0", "custompatternalbedo1" } },
        { SDK_R_SCRATCHES, { "customscratchalbedo", NULL } },   { SDK_R_DIRT, { "customdirtalbedo", NULL } },
        { SDK_R_GLOW, { "custombloomalbedo", NULL } },
    };
    const char *call = SDK_W(C_RECOLOUR);
    const sh_script_engine *e = eng();
    sh_script_handle h;
    void *ent = sh_script_api_owned(L, 1, call, "recolour", &h);
    double seconds = 0;
    int ms, ok = 1, r, i;
    if (!lua_istable(L, 2))
        sh_script_refuse(L, "\"%s\" takes a table of colours, as in e:%s{ %s = { 1, 0, 0 } }", call, call, SDK_W(R_PRIMARY));
    /* every key is a region or the seconds */
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        int known = k && !strcmp(k, SDK_W(R_SECONDS));
        for (r = 0; k && r < (int)(sizeof REGIONS / sizeof REGIONS[0]); r++) if (!strcmp(k, sdk_word(REGIONS[r].word))) known = 1;
        if (!known)
            sh_script_refuse(L, "\"%s\" knows no region \"%s\"; write %s, %s, %s, %s, %s or %s, and %s", call,
                             k ? k : "(a number)", SDK_W(R_PRIMARY), SDK_W(R_SECONDARY), SDK_W(R_PATTERN),
                             SDK_W(R_SCRATCHES), SDK_W(R_DIRT), SDK_W(R_GLOW), SDK_W(R_SECONDS));
        lua_pop(L, 1);
    }
    lua_getfield(L, 2, SDK_W(R_SECONDS));
    if (!lua_isnil(L, -1)) {
        if (!lua_isnumber(L, -1)) sh_script_refuse(L, "\"%s\" needs the seconds as a number", call);
        seconds = lua_tonumber(L, -1);
        if (!(seconds >= 0.0 && seconds <= SH_TINT_MAX_SECONDS))
            sh_script_refuse(L, "\"%s\" needs the seconds to fade over from 0 to %g, got %g", call, SH_TINT_MAX_SECONDS, seconds);
    }
    lua_pop(L, 1);
    if (!ent || !e || !e->decl) return nil1(L);
    ms = sh_tint_duration_ms(seconds);
    for (r = 0; r < (int)(sizeof REGIONS / sizeof REGIONS[0]); r++) {
        float c[2][4];
        int n = REGIONS[r].parm[1] ? 2 : 1;
        if (lua_getfield(L, 2, sdk_word(REGIONS[r].word)) == LUA_TNIL) { lua_pop(L, 1); continue; }
        if (n == 2) {
            if (!lua_istable(L, -1) || lua_rawlen(L, -1) != 2)
                sh_script_refuse(L, "\"%s\" needs %s as two colours, { { r, g, b }, { r, g, b } }", call, sdk_word(REGIONS[r].word));
            for (i = 0; i < 2; i++) {
                lua_rawgeti(L, -1, i + 1);
                read_colour(L, lua_gettop(L), call, sdk_word(REGIONS[r].word), c[i], 1);
                lua_pop(L, 1);
            }
        } else read_colour(L, lua_gettop(L), call, sdk_word(REGIONS[r].word), c[0], 1);
        lua_pop(L, 1);
        for (i = 0; i < n; i++) {
            void *decl = e->decl("renderParm", REGIONS[r].parm[i]);
            recolour_rec *rec = recolour_find(h, r * 2 + i, 1);
            sh_script_earg ev[6];
            float from[4];
            if (!decl) { ok = 0; continue; }
            memcpy(from, rec->h == h && (rec->v[0] || rec->v[1] || rec->v[2] || rec->v[3]) ? rec->v : c[i], sizeof from);
            memset(ev, 0, sizeof ev);
            ev[0].type = 'd'; ev[0].p = decl;
            ev[1] = sh_script_earg_vec(from);
            ev[2] = sh_script_earg_float(from[3]);
            ev[3] = sh_script_earg_vec(c[i]);
            ev[4] = sh_script_earg_float(c[i][3]);
            ev[5] = sh_script_earg_int(ms);
            if (!sh_script_api_event(ent, "lerpRenderParmFrom", ev, 6, NULL)) ok = 0;
            memcpy(rec->v, c[i], sizeof rec->v);
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}

/* set_environment(name): the mod's environment (an env decl written `type environment`, modenv.h) laid over the
 * level's own on the players' views, blending in over its `blend`, by the fx it compiles to (FX_ENV_CHANGE items,
 * VK FUN_1416f0240 case 0xb) played by its emitter. clear_environment() releases the values with the same blend; a
 * mod has one environment at a time, and near the entity limit it is skipped with a log line. */
#define SHS_REG_ENV "sh.sdk.environment"

typedef struct {
    uint32_t player;
    char name[48];
    int active, timer, slot;
} hud_text;

static void hud_remove(hud_text *h)
{
    const sh_script_engine *e = sh_script_api_engine();
    if (h->active && e->resolve && e->hud_text)
        e->hud_text(e->resolve(h->player), h->name, h->slot, "", NULL);
    h->active = 0;
}

static int hud_close(lua_State *L)
{
    hud_remove((hud_text *)lua_touserdata(L, 1));
    return 0;
}

static int hud_expire(lua_State *L)
{
    hud_text *h = (hud_text *)lua_touserdata(L, lua_upvalueindex(1));
    h->timer = 0;
    hud_remove(h);
    lua_pushnil(L);
    lua_seti(L, lua_upvalueindex(2), lua_tointeger(L, lua_upvalueindex(3)));
    return 0;
}

static int l_show_text(lua_State *L)
{
    const char *call = SDK_W(C_SHOW_TEXT);
    const sh_script_engine *e = sh_script_api_engine();
    void *player = sh_script_api_entity(L, 1, call, NULL);
    const char *place = sh_script_api_check_text(L, 2, call, "the place on screen", SH_SCRIPT_TEXT_ANY);
    const char *text = lua_type(L, 3) == LUA_TSTRING && lua_rawlen(L, 3) == 0 ? "" :
                       sh_script_api_check_text(L, 3, call, "the text to show", SH_SCRIPT_TEXT_ANY);
    int slot = !strcmp(place, "top left") ? 0 : !strcmp(place, "top right") ? 1 : !strcmp(place, "bottom") ? 2 : -1;
    int table, entry, ok;
    lua_Integer key;
    uint32_t id;
    double seconds = 0;
    float rgba[4] = { 1, 1, 1, 1 };
    hud_text *h;

    if (slot < 0) return sh_script_refuse(L, "\"%s\" takes \"top left\", \"top right\" or \"bottom\" as its place", call);
    if (!lua_isnoneornil(L, 4)) read_colour(L, 4, call, "the text colour", rgba, 0);
    if (sh_script_api_opt_number(L, 5, call, "the seconds to show the text", &seconds) &&
        !(seconds >= 0 && seconds <= 1e9)) return sh_script_refuse(L, "\"%s\" needs a time of 0 to 1000000000 seconds", call);
    if (player && sh_script_api_kind(player) != SH_SCRIPT_KIND_PLAYER)
        return sh_script_refuse(L, "\"%s\" needs a player as its first value", call);
    if (!player || !e->spawn_id || (id = e->spawn_id(player)) == SH_SCRIPT_SPAWN_ID_NONE) {
        lua_pushboolean(L, 0);
        return 1;
    }
    key = (lua_Integer)id * 3 + slot;
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "sh.sdk.playerText");
    table = lua_gettop(L);
    lua_geti(L, table, key);
    h = (hud_text *)lua_touserdata(L, -1);
    if (!text[0]) {
        if (h) {
            shs_cancel_timer(L, h->timer);
            hud_remove(h);
            lua_pushnil(L);
            lua_seti(L, table, key);
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    if (!h) {
        lua_pop(L, 1);
        h = (hud_text *)lua_newuserdatauv(L, sizeof *h, 0);
        memset(h, 0, sizeof *h);
        h->player = id;
        h->slot = slot;
        snprintf(h->name, sizeof h->name, "smp_%s_text_%d", sh_script_salt(L), slot);
        if (luaL_newmetatable(L, "sh.sdk.hudText")) {
            lua_pushcfunction(L, hud_close);
            lua_setfield(L, -2, "__gc");
        }
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_seti(L, table, key);
    }
    entry = lua_gettop(L);
    ok = e->hud_text && e->hud_text(player, h->name, slot, text, rgba);
    if (ok) h->active = 1;
    shs_cancel_timer(L, h->timer);
    h->timer = 0;
    if (!ok) {
        hud_remove(h);
        lua_pushnil(L);
        lua_seti(L, table, key);
    } else if (seconds > 0) {
        lua_pushvalue(L, entry);
        lua_pushvalue(L, table);
        lua_pushinteger(L, key);
        lua_pushcclosure(L, hud_expire, 3);
        h->timer = shs_after(L, seconds, lua_gettop(L));
    }
    lua_pushboolean(L, ok);
    return 1;
}

static void env_stop(lua_State *L)
{
    if (lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_ENV) != LUA_TNIL) {
        void *ent = sh_script_api_entity(L, lua_gettop(L), SDK_W(C_CLEAR_ENVIRONMENT), NULL);
        if (ent) sh_script_api_post(ent, "remove", NULL, 0);
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_ENV);
}

#define ENV_AREA_MT "snapmap-plus.environment-area"
#define ENV_AREAS "sh.sdk.environmentAreas"

static int env_area_close(lua_State *L)
{
    sh_script_env_area *area = (sh_script_env_area *)luaL_checkudata(L, 1, ENV_AREA_MT);
    const sh_script_sdk_runtime *rt = sh_script_sdk_runtime_get();
    int ok = !area->decl || (rt->remove_area && rt->remove_area(area));
    if (ok) area->decl = NULL;
    lua_pushboolean(L, ok);
    return 1;
}

static int env_area_remove(lua_State *L)
{
    env_area_close(L);
    if (lua_toboolean(L, -1)) {
        luaL_getsubtable(L, LUA_REGISTRYINDEX, ENV_AREAS);
        lua_pushvalue(L, 1);
        lua_pushnil(L);
        lua_rawset(L, -3);
        lua_pop(L, 1);
    }
    return 1;
}

static int l_environment_area(lua_State *L)
{
    const char *call = SDK_W(C_ENVIRONMENT_AREA);
    const char *name = sh_script_check_name(L, 1, call, 0, NULL);
    const sh_script_sdk_runtime *rt = sh_script_sdk_runtime_get();
    float a[3], b[3], bounds[6], volume = 1;
    sh_script_api_check_vec(L, 2, call, "first corner", a);
    sh_script_api_check_vec(L, 3, call, "opposite corner", b);
    for (unsigned i = 0; i < 3; i++) {
        bounds[i] = fminf(a[i], b[i]);
        bounds[i + 3] = fmaxf(a[i], b[i]);
        volume *= bounds[i + 3] - bounds[i];
    }
    if (!(volume > 0) || !isfinite(volume))
        sh_script_refuse(L, "\"%s\" needs two corners enclosing a box with width, length and height", call);
    if (!rt->add_area || !rt->remove_area) return nil1(L);
    if (luaL_newmetatable(L, ENV_AREA_MT)) {
        lua_pushcfunction(L, env_area_close);
        lua_setfield(L, -2, "__gc");
        lua_newtable(L);
        lua_pushcfunction(L, env_area_remove);
        lua_setfield(L, -2, "remove");
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    sh_script_env_area *area = (sh_script_env_area *)lua_newuserdatauv(L, sizeof *area, 0);
    memset(area, 0, sizeof *area);
    memcpy(area->bounds, bounds, sizeof bounds);
    int status = rt->add_area(name, area);
    if (status < 0) sh_script_refuse(L, "\"%s\" needs an environment from this mod's environments folder", call);
    if (!status) return nil1(L);
    luaL_setmetatable(L, ENV_AREA_MT);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, ENV_AREAS);
    lua_pushvalue(L, -2);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return 1;
}

static int l_set_environment(lua_State *L)
{
    const char *call = SDK_W(C_SET_ENVIRONMENT);
    const char *name = sh_script_check_name(L, 1, call, 0, NULL);
    const sh_script_sdk_runtime *rt = sh_script_sdk_runtime_get();
    float origin[3] = { 0, 0, 0 };
    char *em = rt->environment_emitter ? rt->environment_emitter(name) : NULL;
    if (!em) {
        if (!rt->environment_emitter) return nil1(L);
        sh_script_refuse(L, "\"%s\" knows no environment \"%s\" of this mod; write one as environments/%s.decl "
                            "whose first key is 'type environment'", call, lua_tostring(L, 1), lua_tostring(L, 1));
    }
    env_stop(L);
    if (!sh_script_api_has_room()) {
        free(em);
        if (rt->log) rt->log("Mod scripts: an environment was not set because the game holds nearly all the entities it can");
        return nil1(L);
    }
    lua_pushstring(L, em);
    free(em);
    sh_script_api_spawn_def(L, lua_tostring(L, -1), origin, 0.0);
    lua_remove(L, -2);
    if (lua_isnil(L, -1)) return 1;
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_ENV);
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_clear_environment(lua_State *L)
{
    env_stop(L);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_tint(lua_State *L) { return tint_call(L, "tint", 0); }
static int l_glow(lua_State *L) { return tint_call(L, "glow", 1); }

void sh_script_api_entity_register(void)
{
    static const struct { int kind; const char *name; lua_CFunction fn; } calls[] = {
        { SH_SCRIPT_API_GLOBAL, "spawn", l_spawn },
        { SH_SCRIPT_API_GLOBAL, "players", l_players },
        { SH_SCRIPT_API_METHOD, "position", l_pos },
        { SH_SCRIPT_API_METHOD, "set_position", l_set_pos },
        { SH_SCRIPT_API_METHOD, "yaw", l_yaw },
        { SH_SCRIPT_API_METHOD, "set_yaw", l_set_yaw },
        { SH_SCRIPT_API_METHOD, "face_body", l_face_body },
        { SH_SCRIPT_API_METHOD, "axis", l_axis },
        { SH_SCRIPT_API_METHOD, "velocity", l_velocity },
        { SH_SCRIPT_API_METHOD, "set_velocity", l_set_velocity },
        { SH_SCRIPT_API_METHOD, "health", l_health },
        { SH_SCRIPT_API_METHOD, "set_health", l_set_health },
        { SH_SCRIPT_API_METHOD, "alive", l_alive },
        { SH_SCRIPT_API_METHOD, "definition", l_def },
        { SH_SCRIPT_API_METHOD, "distance", l_distance },
        { SH_SCRIPT_API_METHOD, "is_player", l_is_player },
        { SH_SCRIPT_API_METHOD, "remove", l_remove },
        { SH_SCRIPT_API_METHOD, "view_angle_to", l_view_angle_to },
        { SH_SCRIPT_API_METHOD, "animation_state", l_anim_state },
        { SH_SCRIPT_API_METHOD, "force_animation", l_anim_force },
        { SH_SCRIPT_API_METHOD, "animation_finished", l_anim_finished },
        { SH_SCRIPT_API_METHOD, "current_animation", l_anim_current },
        { SH_SCRIPT_API_METHOD, "animation_event", l_anim_event },
        { SH_SCRIPT_API_METHOD, "tint", l_tint },
        { SH_SCRIPT_API_METHOD, "glow", l_glow },
    };
    size_t i;
    for (i = 0; i < sizeof calls / sizeof calls[0]; i++) sh_script_api_add(calls[i].kind, calls[i].name, calls[i].fn);
    /* the Graphics SDK's calls, named from its words table (sdkwords.h) */
    sh_script_api_add(SH_SCRIPT_API_METHOD, SDK_W(C_RECOLOUR), l_recolour);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, SDK_W(C_SET_ENVIRONMENT), l_set_environment);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, SDK_W(C_ENVIRONMENT_AREA), l_environment_area);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, SDK_W(C_CLEAR_ENVIRONMENT), l_clear_environment);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, SDK_W(C_SHOW_TEXT), l_show_text);
}
