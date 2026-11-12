/* The calls that reach the whole game (script_api_game.h): engine events by
 * name, SnapMap events, cvars and decls. The first half is the Lua side over
 * the engine seams; the second half, without SH_SCRIPT_PURE, binds them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script_api_game.h"
#include "script_api_reflect.h"

static sh_script_game_ops g_ops;
static int g_have_ops;
#define OP(fn) (g_have_ops && g_ops.fn)

void sh_script_api_game_set(const sh_script_game_ops *ops)
{
    if (ops) { g_ops = *ops; g_have_ops = 1; }
    else { memset(&g_ops, 0, sizeof g_ops); g_have_ops = 0; }
}

static const char *value_words(lua_State *L, int arg)
{
    if (lua_isnoneornil(L, arg)) return "nothing";
    if (luaL_testudata(L, arg, "entity")) return "an entity";
    if (luaL_testudata(L, arg, "decl")) return "a decl";
    switch (lua_type(L, arg)) {
    case LUA_TNUMBER: return "a number";
    case LUA_TSTRING: return "text";
    case LUA_TBOOLEAN: return "true or false";
    case LUA_TTABLE: return "a table";
    default: return luaL_typename(L, arg);
    }
}

/* ------------------------------------------------------------- decls */

typedef struct decl_ud {
    void *ptr;
    char  type[64];
    char  cls[128];     /* "" when the game does not describe the type */
} decl_ud;

static decl_ud *check_decl_ud(lua_State *L, const char *call)
{
    decl_ud *d = (decl_ud *)luaL_testudata(L, 1, "decl");
    if (!d) sh_script_refuse(L, "\"%s\" is a call on a decl (from decl()), got %s", call, value_words(L, 1));
    return d;
}

static const char *decl_cls(lua_State *L, decl_ud *d, const char *call)
{
    if (!sh_script_reflect_ready())
        sh_script_refuse(L, "\"%s\" is unavailable: the game's type information could not be read in this session", call);
    if (!d->cls[0]) sh_script_refuse(L, "\"%s\" cannot look inside a %s decl: the game does not describe that type", call, d->type);
    return d->cls;
}

static int d_get(lua_State *L)
{
    decl_ud *d = check_decl_ud(L, "get");
    return sh_script_reflect_get(L, d->ptr, decl_cls(L, d, "get"), luaL_optstring(L, 2, ""), "get");
}

static int d_set(lua_State *L)
{
    decl_ud *d = check_decl_ud(L, "set");
    const char *path = luaL_checkstring(L, 2);
    luaL_checkany(L, 3);
    return sh_script_reflect_set(L, d->ptr, decl_cls(L, d, "set"), path, 3, "set", 1);
}

static int d_fields(lua_State *L)
{
    decl_ud *d = check_decl_ud(L, "fields");
    return sh_script_reflect_fields(L, d->ptr, decl_cls(L, d, "fields"), luaL_optstring(L, 2, ""), "fields");
}

static int d_name(lua_State *L)
{
    decl_ud *d = check_decl_ud(L, "name");
    const char *name = OP(decl_name) ? g_ops.decl_name(d->ptr) : NULL;
    if (name) lua_pushstring(L, name);
    else lua_pushnil(L);
    return 1;
}

static int d_type(lua_State *L)
{
    lua_pushstring(L, check_decl_ud(L, "type")->type);
    return 1;
}

static int d_eq(lua_State *L)
{
    decl_ud *a = (decl_ud *)luaL_testudata(L, 1, "decl"), *b = (decl_ud *)luaL_testudata(L, 2, "decl");
    lua_pushboolean(L, a && b && a->ptr == b->ptr);
    return 1;
}

/* decl(type, name) */
static int l_decl(lua_State *L)
{
    const char *type = sh_script_api_check_text(L, 1, "decl", "the decl's type (such as \"damage\")", sizeof ((decl_ud *)0)->type - 1);
    const char *full, *cls;
    int is_game = 0;
    void *ptr;
    decl_ud *d;
    if (lua_type(L, 2) != LUA_TSTRING && !lua_isuserdata(L, 2))
        sh_script_refuse(L, "\"decl\" needs the decl's name as its second value, got %s", value_words(L, 2));
    full = sh_script_check_name(L, 2, "decl", 1, &is_game);
    if (!OP(decl)) return sh_script_refuse(L, "\"decl\" is unavailable: the game's decls could not be reached in this session");
    ptr = g_ops.decl(type, full);
    if (!ptr)
        return sh_script_refuse(L, is_game ? "\"decl\" found no %s decl named %s in the game"
                                           : "\"decl\" found no %s decl \"%s\" in this mod; a decl of the game is "
                                             "decl(type, game(name))", type, is_game ? full : lua_tostring(L, 2));
    d = (decl_ud *)lua_newuserdatauv(L, sizeof *d, 0);
    memset(d, 0, sizeof *d);
    d->ptr = ptr;
    strncpy_s(d->type, sizeof d->type, type, _TRUNCATE);
    cls = OP(decl_class) ? g_ops.decl_class(type) : NULL;
    if (cls && strlen(cls) < sizeof d->cls) strcpy_s(d->cls, sizeof d->cls, cls);
    luaL_setmetatable(L, "decl");
    return 1;
}

static void open_decl(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "get", d_get }, { "set", d_set }, { "fields", d_fields }, { "name", d_name }, { "type", d_type }, { NULL, NULL }
    };
    luaL_newmetatable(L, "decl");
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, d_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushstring(L, "decl");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

/* ------------------------------------------------------------ events */

static const char *arg_word(char t)
{
    switch (t) {
    case 'i': return "whole number";
    case 'b': return "true or false";
    case 'f': return "number";
    case 'v': return "position";
    case 'a': return "angles";
    case 'c': return "color";
    case 's': return "text";
    case '1': return "animation state";
    case '2': return "joint name";
    case 'e': case 'r': return "entity";
    case 'd': return "decl";
    default: return NULL;
    }
}

/* The event's format, or a refusal. Returns how many values a script passes. */
static int event_shape(lua_State *L, const char *call, const char *name, char *format, size_t cap)
{
    const sh_script_engine *e = sh_script_api_engine();
    int want = 0, i, n;
    if (!e || !e->event_format || !e->event)
        sh_script_refuse(L, "\"%s\" is unavailable: the game's events could not be reached in this session", call);
    if (!e->event_format(name, format, cap))
        sh_script_refuse(L, "\"%s\" found no event named %s in the game", call, name);
    n = (int)strlen(format);
    if (n > SH_SCRIPT_EVENT_ARGS)
        sh_script_refuse(L, "\"%s\" cannot send %s: it takes %d values and the game passes at most %d", call, name, n,
                         SH_SCRIPT_EVENT_ARGS);
    for (i = 0; i < n; i++) {
        if (format[i] == '4') continue;            /* the animation that sent it: none, from a script */
        if (!arg_word(format[i]))
            sh_script_refuse(L, "\"%s\" cannot send %s: value %d is of a kind only the game makes", call, name, want + 1);
        want++;
    }
    return want;
}

static void push_shape(lua_State *L, const char *format)
{
    luaL_Buffer b;
    int i, any = 0;
    luaL_buffinit(L, &b);
    for (i = 0; format[i]; i++) {
        if (format[i] == '4') continue;
        if (any++) luaL_addstring(&b, ", ");
        luaL_addstring(&b, arg_word(format[i]));
    }
    if (!any) luaL_addstring(&b, "nothing");
    luaL_pushresult(&b);
}

static float table_number(lua_State *L, int t, const char *key, int index, int *ok)
{
    double v = 0;
    lua_getfield(L, t, key);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, t, index); }
    if (lua_type(L, -1) == LUA_TNUMBER) v = lua_tonumber(L, -1);
    else *ok = 0;
    lua_pop(L, 1);
    return (float)v;
}

static void bad_value(lua_State *L, const char *call, const char *name, int pos, char t, int arg)
{
    sh_script_refuse(L, "\"%s\" needs %s %s as value %d of %s, got %s", call,
                     strchr("aie", arg_word(t)[0]) ? "an" : "a", arg_word(t), pos, name, value_words(L, arg));
}

static int event_call(lua_State *L, int post)
{
    const char *call = post ? "post" : "event";
    void *ent = sh_script_api_entity(L, 1, call, NULL);
    const char *name = sh_script_api_check_text(L, 2, call, "the event's name", 200);
    const sh_script_engine *e = sh_script_api_engine();
    char format[40];
    sh_script_earg a[SH_SCRIPT_EVENT_ARGS], r;
    int want = event_shape(L, call, name, format, sizeof format), given = lua_gettop(L) - 2;
    int n = (int)strlen(format), i, arg = 3, pos = 0, ok;
    if (given != want) {
        push_shape(L, format);
        return sh_script_refuse(L, "\"%s\" got %d value%s for %s, which takes %d (%s)", call, given, given == 1 ? "" : "s",
                                name, want, lua_tostring(L, -1));
    }
    memset(a, 0, sizeof a);
    for (i = 0; i < n; i++) {
        char t = format[i];
        if (t == '4') { a[i] = sh_script_earg_no_anim(); continue; }
        pos++;
        switch (t) {
        case 'i':
            if (lua_isboolean(L, arg)) a[i] = sh_script_earg_int(lua_toboolean(L, arg));
            else {
                int isnum = 0;
                lua_Integer v = lua_tointegerx(L, arg, &isnum);
                if (!isnum) bad_value(L, call, name, pos, t, arg);
                a[i] = sh_script_earg_int((int)v);
            }
            break;
        case 'b':
            if (lua_isboolean(L, arg)) a[i] = sh_script_earg_int(lua_toboolean(L, arg));
            else if (lua_isinteger(L, arg)) a[i] = sh_script_earg_int(lua_tointeger(L, arg) != 0);
            else bad_value(L, call, name, pos, t, arg);
            break;
        case 'f':
            if (lua_type(L, arg) != LUA_TNUMBER) bad_value(L, call, name, pos, t, arg);
            a[i] = sh_script_earg_float((float)lua_tonumber(L, arg));
            break;
        case 'v': {
            float v[3];
            if (!lua_istable(L, arg)) bad_value(L, call, name, pos, t, arg);
            sh_script_api_check_vec(L, arg, call, "the position", v);
            a[i] = sh_script_earg_vec(v);
            break;
        }
        case 'a': {
            float v[3];
            int fine = 1;
            if (!lua_istable(L, arg)) bad_value(L, call, name, pos, t, arg);
            v[0] = table_number(L, arg, "pitch", 1, &fine);
            v[1] = table_number(L, arg, "yaw", 2, &fine);
            v[2] = table_number(L, arg, "roll", 3, &fine);
            if (!fine)
                sh_script_refuse(L, "\"%s\" needs angles with pitch, yaw and roll as numbers as value %d of %s", call, pos, name);
            a[i] = sh_script_earg_vec(v);
            a[i].type = 'a';
            break;
        }
        case 'c': {
            int fine = 1;
            if (!lua_istable(L, arg)) bad_value(L, call, name, pos, t, arg);
            a[i].type = 'c';
            a[i].v[0] = table_number(L, arg, "r", 1, &fine);
            a[i].v[1] = table_number(L, arg, "g", 2, &fine);
            a[i].v[2] = table_number(L, arg, "b", 3, &fine);
            if (!fine)
                sh_script_refuse(L, "\"%s\" needs a color with r, g and b as numbers as value %d of %s", call, pos, name);
            fine = 1;
            a[i].w = table_number(L, arg, "a", 4, &fine);
            if (!fine) a[i].w = 1.0f;
            break;
        }
        case 's': case '1': case '2':
            if (lua_type(L, arg) != LUA_TSTRING) bad_value(L, call, name, pos, t, arg);
            a[i] = sh_script_earg_text('s', lua_tostring(L, arg));
            a[i].type = t;
            break;
        case 'e': case 'r':
            a[i].type = t == 'e' ? 'E' : 'r';
            if (lua_isnoneornil(L, arg)) a[i].p = NULL;
            else if (!luaL_testudata(L, arg, "entity")) bad_value(L, call, name, pos, t, arg);
            else a[i].p = sh_script_api_entity(L, arg, call, NULL);
            if (t == 'r' && !a[i].p)
                sh_script_refuse(L, "\"%s\" needs an entity that is still in the map as value %d of %s", call, pos, name);
            break;
        case 'd': {
            decl_ud *d = (decl_ud *)luaL_testudata(L, arg, "decl");
            if (!d && !lua_isnoneornil(L, arg)) bad_value(L, call, name, pos, t, arg);
            a[i].type = 'd';
            a[i].p = d ? d->ptr : NULL;
            break;
        }
        default: break;
        }
        arg++;
    }
    if (!ent) { lua_pushnil(L); return 1; }
    if (post) {
        lua_pushboolean(L, sh_script_api_post(ent, name, a, n));
        return 1;
    }
    memset(&r, 0, sizeof r);
    ok = sh_script_api_event(ent, name, a, n, &r);
    if (!ok) return sh_script_refuse(L, "\"event\" could not run %s: the game did not take it", name);
    switch (r.type) {
    case 'i': lua_pushinteger(L, r.i); return 1;
    case 'b': lua_pushboolean(L, r.i != 0); return 1;
    case 'f': lua_pushnumber(L, (lua_Number)r.f); return 1;
    case 'v': sh_script_api_push_vec(L, r.v); return 1;
    case 'a':
        lua_createtable(L, 0, 3);
        lua_pushnumber(L, r.v[0]); lua_setfield(L, -2, "pitch");
        lua_pushnumber(L, r.v[1]); lua_setfield(L, -2, "yaw");
        lua_pushnumber(L, r.v[2]); lua_setfield(L, -2, "roll");
        return 1;
    case 's': case '1': case '2':
        if (r.s) lua_pushstring(L, r.s);
        else lua_pushnil(L);
        return 1;
    case 'e':
        sh_script_api_push_engine_entity(L, e && e->resolve ? e->resolve((uint32_t)r.i) : NULL);
        return 1;
    case 'd': {
        const char *dn = r.p && OP(decl_name) ? g_ops.decl_name(r.p) : NULL;
        if (dn) lua_pushstring(L, dn);
        else lua_pushnil(L);
        return 1;
    }
    default: return 0;
    }
}

static int l_event(lua_State *L) { return event_call(L, 0); }
static int l_post(lua_State *L) { return event_call(L, 1); }

/* event_args(name) -> { "entity", "decl" } or nil */
static int l_event_args(lua_State *L)
{
    const char *name = sh_script_api_check_text(L, 1, "event_args", "the event's name", 200);
    const sh_script_engine *e = sh_script_api_engine();
    char format[40];
    int i, n = 0;
    if (!e || !e->event_format || !e->event_format(name, format, sizeof format)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    for (i = 0; format[i]; i++) {
        if (format[i] == '4') continue;
        lua_pushstring(L, arg_word(format[i]) ? arg_word(format[i]) : "a value only the game makes");
        lua_rawseti(L, -2, (lua_Integer)++n);
    }
    return 1;
}

/* ----------------------------------------------------------- signals */

static int g_signals_wanted;   /* a script of this map handles a SnapMap event */

/* Copy the event's key (lower case, without snap_event_). */
static void signal_key(const char *s, char *out, size_t cap)
{
    size_t i = 0;
    for (; *s && i + 1 < cap; s++) out[i++] = (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s;
    out[i] = 0;
    if (!strncmp(out, "snap_event_", 11)) memmove(out, out + 11, strlen(out + 11) + 1);
}

static int signal_known(const char *event)
{
    return !OP(signal_number) || g_ops.signal_number(event) >= 0;
}

void sh_script_game_signal_seen(void *ent, const char *event, void *activator)
{
    sh_script_handle self;
    if (!g_signals_wanted || !ent || !event || !event[0]) return;
    self = sh_script_api_handle_of(ent);
    if (!self) return;
    sh_script_signal_post(event, self, activator ? sh_script_api_handle_of(activator) : 0);
}

/* e:signal(event[, activator]) */
static int l_signal(lua_State *L)
{
    void *ent = sh_script_api_entity(L, 1, "signal", NULL), *by = NULL;
    const char *name = sh_script_api_check_text(L, 2, "signal", "the SnapMap event's name (such as \"ai_killed\")", 120);
    char key[128];
    int number;
    if (!lua_isnoneornil(L, 3)) {
        if (!luaL_testudata(L, 3, "entity"))
            sh_script_refuse(L, "\"signal\" needs the activator as an entity, got %s", value_words(L, 3));
        by = sh_script_api_entity(L, 3, "signal", NULL);
    }
    if (!OP(signal_number) || !OP(signal_fire))
        return sh_script_refuse(L, "\"signal\" is unavailable: SnapMap's events could not be reached in this session");
    signal_key(name, key, sizeof key);
    number = g_ops.signal_number(key);
    if (number < 0) return sh_script_refuse(L, "\"signal\" found no SnapMap event named %s in the game", name);
    if (!ent) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, g_ops.signal_fire(ent, number, by));
    return 1;
}

/* ------------------------------------------------------------- cvars */

/* The cvars a script set this map, with the value each had. */
typedef struct cvar_undo { char *name, *was; } cvar_undo;
static cvar_undo *g_cvars;
static int g_ncvars, g_ccvars;

static int cvar_keep(const char *name, const char *was)
{
    int i;
    for (i = 0; i < g_ncvars; i++) if (!strcmp(g_cvars[i].name, name)) return 1;
    if (g_ncvars == g_ccvars) {
        int cap = g_ccvars ? g_ccvars * 2 : 16;
        cvar_undo *grown = (cvar_undo *)realloc(g_cvars, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        g_cvars = grown;
        g_ccvars = cap;
    }
    g_cvars[g_ncvars].name = _strdup(name);
    g_cvars[g_ncvars].was = _strdup(was);
    if (!g_cvars[g_ncvars].name || !g_cvars[g_ncvars].was) {
        free(g_cvars[g_ncvars].name);
        free(g_cvars[g_ncvars].was);
        return 0;
    }
    g_ncvars++;
    return 1;
}

static void map_ended(void)
{
    while (g_ncvars > 0) {
        cvar_undo *u = &g_cvars[--g_ncvars];
        if (OP(cvar_set)) g_ops.cvar_set(u->name, u->was);
        free(u->name);
        free(u->was);
    }
    g_signals_wanted = 0;
}

static void signals_wanted(void)
{
    g_signals_wanted = 1;
}

/* cvar(name[, value]) */
static int l_cvar(lua_State *L)
{
    const char *name = sh_script_api_check_text(L, 1, "cvar", "the cvar's name", 200);
    char was[1024];
    if (!OP(cvar_get)) return sh_script_refuse(L, "\"cvar\" is unavailable: the game's cvars could not be reached in this session");
    if (!g_ops.cvar_get(name, was, sizeof was)) return sh_script_refuse(L, "\"cvar\" found no cvar named %s in the game", name);
    if (lua_gettop(L) < 2) {
        char *end = NULL;
        double v = was[0] ? strtod(was, &end) : 0;
        if (was[0] && end && !*end) {
            if (v == (double)(lua_Integer)v && !strpbrk(was, ".eE")) lua_pushinteger(L, (lua_Integer)v);
            else lua_pushnumber(L, v);
        } else lua_pushstring(L, was);
        return 1;
    }
    {
        const char *text;
        int r;
        if (lua_isboolean(L, 2)) text = lua_toboolean(L, 2) ? "1" : "0";
        else if (lua_type(L, 2) == LUA_TNUMBER || lua_type(L, 2) == LUA_TSTRING) text = lua_tostring(L, 2);
        else return sh_script_refuse(L, "\"cvar\" needs the value as a number, true or false, or text, got %s", value_words(L, 2));
        if (!OP(cvar_set) || !cvar_keep(name, was))
            return sh_script_refuse(L, "\"cvar\" could not set %s: the game's cvars cannot be changed in this session", name);
        r = g_ops.cvar_set(name, text);
        if (r == -1) return sh_script_refuse(L, "\"cvar\" cannot set %s: the game only takes it when it starts", name);
        if (r != 1) return sh_script_refuse(L, "\"cvar\" could not set %s: the game refused the value %s", name, text);
        lua_pushboolean(L, 1);
        return 1;
    }
}

/* entities() -> every entity of the map now, players first */
static int l_entities(lua_State *L)
{
    const sh_script_engine *e = sh_script_api_engine();
    int num, n = 0, slots;
    lua_newtable(L);
    if (!e || !e->slot_spawn_id || !e->resolve || !e->slots) return 1;
    slots = e->slots();
    for (num = 0; num < slots; num++) {
        uint32_t id = e->slot_spawn_id(num);
        void *ent;
        sh_script_handle h;
        if (id == SH_SCRIPT_SPAWN_ID_NONE || id == SH_SCRIPT_SPAWN_ID_WORLD) continue;
        ent = e->resolve(id);
        h = ent ? sh_script_api_handle_of(ent) : 0;
        if (!h) continue;
        sh_script_push_entity(L, h);
        lua_rawseti(L, -2, (lua_Integer)++n);
    }
    return 1;
}

void sh_script_api_game_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sh_script_api_add(SH_SCRIPT_API_METHOD, "event", l_event);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "post", l_post);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "signal", l_signal);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "event_args", l_event_args);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "cvar", l_cvar);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "decl", l_decl);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "entities", l_entities);
    sh_script_api_add_opener(open_decl);
    sh_script_api_add_closer(map_ended);
    sh_script_set_signal_ops(signal_known, signals_wanted);
}

#ifndef SH_SCRIPT_PURE
/* ============================================================ the engine */
#include <windows.h>
#include "../backend_log.h"
#include "../hook.h"
#include "../engine_globals.h"
#include "../engine_build.h"
#include "../engine_cvar_read.h"
#include "../typeinfo.h"

#define DECL_NAME_OFF       0x08u
#define IDCVAR_VALUE_TEXT   0x10u       /* idCVar: its value as text */
#define CVAR_READ_ONLY      0x8000u
#define CVAR_INIT_ONLY      0x4000u
#define SIGNAL_ENUM         "snapListenerEvent_t"
#define SIGNALS_MAX         256
#define FIRE_CALL_OFF       0x17u       /* FireOutput: the call to the walk over the entity's listeners */
#define WALK_STOLEN         14

int   sh_script_node_fire_output(void *ent, int event, void *activator);   /* script_node.c */
void *sh_script_node_fire_address(void);
void *sh_script_combat_decl(const char *type, const char *name);           /* script_api_combat.c */

typedef unsigned char (*cvar_set_fn)(void *cvar, const char *text, unsigned char force);
typedef void (*walk_fn)(void *ent, int *event, void **activator);

static const void *g_cvar_slot;
static cvar_set_fn g_cvar_set;
static char *g_signal_names[SIGNALS_MAX];   /* by number: the key, or NULL */
static int g_signals_read;
static void *g_walk_target;
static walk_fn g_walk_orig;
static int g_walk_failed;

static const uint8_t k_walk_prologue[20] = {
    0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20
};

static const char *g_decl_name(void *decl)
{
    const char *name = NULL;
    __try { name = *(const char *const *)((const uint8_t *)decl + DECL_NAME_OFF); if (name && !name[0]) name = NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { name = NULL; }
    return name;
}

static void signals_read(void)
{
    uint32_t i;
    const char *member = NULL;
    long long value = 0;
    if (g_signals_read) return;
    for (i = 0; sh_typeinfo_enum_member(SIGNAL_ENUM, i, &member, &value); i++) {
        char key[128];
        if (!member || value < 0 || value >= SIGNALS_MAX || g_signal_names[value]) continue;
        signal_key(member, key, sizeof key);
        g_signal_names[value] = _strdup(key);
    }
    if (i) g_signals_read = 1;        /* the types answer once a map is up: ask again until they do */
}

static int g_signal_number(const char *event)
{
    int i;
    signals_read();
    for (i = 1; i < SIGNALS_MAX; i++)
        if (g_signal_names[i] && !strcmp(g_signal_names[i], event)) return i;
    return -1;
}

static int g_signal_fire(void *ent, int number, void *activator)
{
    return sh_script_node_fire_output(ent, number, activator);
}

static void walk_detour(void *ent, int *event, void **activator)
{
    if (g_signals_wanted && ent && event) {
        int number = -1;
        void *by = NULL;
        __try { number = *event; by = activator ? *activator : NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { number = -1; }
        if (number > 0 && number < SIGNALS_MAX) {
            signals_read();
            if (g_signal_names[number]) sh_script_game_signal_seen(ent, g_signal_names[number], by);
        }
    }
    g_walk_orig(ent, event, activator);
}

/* Where the game walks an entity's listeners: the call inside FireOutput. */
static void *walk_find(void)
{
    const uint8_t *fire = (const uint8_t *)sh_script_node_fire_address();
    void *found = NULL;
    if (!fire) return NULL;
    __try {
        if (fire[FIRE_CALL_OFF] == 0xE8) {
            int32_t rel;
            const uint8_t *walk;
            memcpy(&rel, fire + FIRE_CALL_OFF + 1, sizeof rel);
            walk = fire + FIRE_CALL_OFF + 5 + rel;
            if (!memcmp(walk, k_walk_prologue, sizeof k_walk_prologue)) found = (void *)walk;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { found = NULL; }
    return found;
}

/* A script handles a SnapMap event: watch the game fire them (once). */
static void signals_hook(void)
{
    void *tramp;
    char line[256];
    signals_wanted();
    if (g_walk_orig || g_walk_failed) return;
    g_walk_target = walk_find();
    if (!g_walk_target) {
        g_walk_failed = 1;
        backend_log("SCRIPTS: on.signal is unavailable: SnapMap's event function is not the expected one in this executable");
        return;
    }
    tramp = hook_prepare(g_walk_target, (void *)walk_detour, WALK_STOLEN);
    if (!tramp || (g_walk_orig = (walk_fn)tramp, hook_commit(tramp) != B2_PATCH_OK)) {
        g_walk_orig = NULL;
        g_walk_failed = 1;
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "SCRIPTS: on.signal is unavailable: the hook on SnapMap's events could not be placed (%s)",
                    hook_last_failure());
        backend_log(line);
        return;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE, "SCRIPTS: SnapMap event hook installed at %p", g_walk_target);
    backend_log(line);
}

static int signals_known(const char *event)
{
    return g_signal_number(event) >= 0;
}

static void *cvar_find(const char *name)
{
    return sh_engine_cvar_find(g_cvar_slot, SH_IDCVAR_NAME_FOR_LOOKUP, name);
}

static int g_cvar_get(const char *name, char *out, size_t cap)
{
    const uint8_t *c = (const uint8_t *)cvar_find(name);
    int ok = 0;
    if (!c || !cap) return 0;
    out[0] = 0;
    __try {
        const char *text = *(const char *const *)(c + IDCVAR_VALUE_TEXT);
        if (text) strncpy_s(out, cap, text, _TRUNCATE);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

static int g_cvar_set_text(const char *name, const char *value)
{
    uint8_t *c = (uint8_t *)cvar_find(name);
    unsigned flags = 0;
    unsigned char ok = 0;
    if (!c) return 0;
    if (!g_cvar_set) return -2;
    __try { flags = *(const unsigned *)(c + SH_IDCVAR_FLAGS); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
    if (flags & (CVAR_READ_ONLY | CVAR_INIT_ONLY)) return -1;
    __try { ok = g_cvar_set(c, value, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok ? 1 : -2;
}

/* The class of a decl type: idDecl<Type>, else id<Type>, whatever the case
 * of the type's letters (the game's own class names decide). */
static const char *g_decl_class(const char *type)
{
    static const char **names;
    static int count;
    char a[160], b[160];
    int i;
    if (!names) {
        count = sh_typeinfo_collect_classnames(&names);
        if (count <= 0) { names = NULL; return NULL; }
    }
    if (strlen(type) + 8 > sizeof a) return NULL;
    sprintf_s(a, sizeof a, "idDecl%s", type);
    sprintf_s(b, sizeof b, "id%s", type);
    for (i = 0; i < count; i++) if (names[i] && !_stricmp(names[i], a)) return names[i];
    for (i = 0; i < count; i++) if (names[i] && !_stricmp(names[i], b)) return names[i];
    return NULL;
}

int sh_script_api_game_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    static const sh_script_game_ops ops = {
        g_decl_name, g_signal_number, g_signal_fire, g_cvar_get, g_cvar_set_text, sh_script_combat_decl, g_decl_class
    };
    sh_script_api_game_register();
    sh_script_api_game_set(&ops);
    g_cvar_slot = module_base ? (const void *)glb_resolve(module_base, "cvar_system_slot", NULL) : NULL;
    g_cvar_set = (cvar_set_fn)sig_addr_by_name(results, count, "CvarSetString");
    if (!g_cvar_slot || !g_cvar_set) backend_log("SCRIPTS: cvar() is unavailable: the game's cvars were not found");
    sh_script_set_signal_ops(signals_known, signals_hook);
    return 1;
}
#endif /* SH_SCRIPT_PURE */
