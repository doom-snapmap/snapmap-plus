/* script_node.c -- see script_node.h. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "script_node.h"
#include "script_runtime.h"
#include "script_hooks.h"
#include "script_queue.h"
#include "script_reason.h"
#include "../mp_json.h"
#include "../modmap.h"
#include "../../third_party/lua/lauxlib.h"

#define MT_NODE      "script node"
#define REG_MISSED   "snapmap-plus.node_missed"   /* node names fire() found nowhere, once each per map */
#define REG_VARS_OFF "snapmap-plus.vars_off"      /* the "variables not readable" line, once per map */

/* ================================================================ helpers */

static void copyz(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!cap) return;
    n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

static char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (c == '\\' ? '/' : c); }

/* ===================================================== the played map */

typedef struct node_name { int uid; char *name; } node_name;
typedef struct var_name { int type; int index; char *name; } var_name;

static SRWLOCK    g_map_lock = SRWLOCK_INIT;
static node_name *g_nodes;
static int        g_nnodes;
static var_name  *g_vars;
static int        g_nvars;

static void map_clear_locked(void)
{
    int i;
    for (i = 0; i < g_nnodes; i++) free(g_nodes[i].name);
    for (i = 0; i < g_nvars; i++) free(g_vars[i].name);
    free(g_nodes);
    free(g_vars);
    g_nodes = NULL; g_vars = NULL;
    g_nnodes = g_nvars = 0;
}

void sh_script_node_map_clear(void)
{
    AcquireSRWLockExclusive(&g_map_lock);
    map_clear_locked();
    ReleaseSRWLockExclusive(&g_map_lock);
}

static void set_err(char *err, size_t cap, const char *text) { if (err && cap) copyz(err, cap, text); }

static char *str_of(const mpj *v)
{
    return (v && v->type == MPJ_STR && v->text) ? sh_mm_decode(v->text) : NULL;
}

int sh_script_node_map_load(const char *json, size_t len, char *err, size_t err_cap)
{
    static const struct { const char *key; int type; } kinds[] = {
        { "integer", SH_SCRIPT_VAR_INTEGER }, { "number", SH_SCRIPT_VAR_NUMBER },
        { "boolean", SH_SCRIPT_VAR_BOOLEAN }, { "string", SH_SCRIPT_VAR_STRING },
    };
    mpj *root, *ents, *vars;
    node_name *nodes = NULL;
    var_name *vlist = NULL;
    int nn = 0, nv = 0, cv = 0, i, k, ok = 0;

    set_err(err, err_cap, "");
    sh_script_node_map_clear();
    root = json ? mpj_parse(json, len) : NULL;
    if (!root || root->type != MPJ_OBJ) { set_err(err, err_cap, "the map could not be read"); goto done; }
    ents = mpj_get(root, "entities");
    if (!ents || ents->type != MPJ_ARR) { set_err(err, err_cap, "the map lists no entities"); goto done; }
    if (ents->n) {
        nodes = (node_name *)calloc((size_t)ents->n, sizeof *nodes);
        if (!nodes) goto oom;
    }
    for (i = 0; i < ents->n; i++) {
        const mpj *e = ents->items[i], *def;
        char *inherit;
        long long uid;
        int is_node;
        if (!e || e->type != MPJ_OBJ) continue;
        def = mpj_get(e, "entityDef");
        uid = mpj_int(mpj_get(e, "uniqueId"));
        if (uid < 0 || uid > 0x7FFFFFFF || !def) continue;
        inherit = str_of(mpj_get(def, "inherit"));
        is_node = inherit && sh_script_node_is_def(inherit, NULL, 0);
        free(inherit);
        if (!is_node) continue;
        nodes[nn].uid = (int)uid;
        nodes[nn].name = str_of(mpj_get(e, "displayName"));
        if (!nodes[nn].name) nodes[nn].name = _strdup("");
        if (!nodes[nn].name) goto oom;
        nn++;
    }
    vars = mpj_get(root, "variables");
    for (k = 0; vars && vars->type == MPJ_OBJ && k < (int)(sizeof kinds / sizeof kinds[0]); k++) {
        const mpj *list = mpj_get(vars, kinds[k].key);
        if (!list || list->type != MPJ_ARR) continue;
        for (i = 0; i < list->n; i++) {
            const mpj *info = list->items[i] ? mpj_get(list->items[i], "info") : NULL;
            char *name = str_of(info ? mpj_get(info, "name") : NULL);
            if (!name) { if (info && mpj_get(info, "name")) goto oom; continue; }
            if (nv == cv) {
                int c = cv ? cv * 2 : 16;
                var_name *p = (var_name *)realloc(vlist, (size_t)c * sizeof *p);
                if (!p) { free(name); goto oom; }
                vlist = p;
                cv = c;
            }
            vlist[nv].type = kinds[k].type;
            vlist[nv].index = i;
            vlist[nv].name = name;
            nv++;
        }
    }
    AcquireSRWLockExclusive(&g_map_lock);
    map_clear_locked();
    g_nodes = nodes; g_nnodes = nn;
    g_vars = vlist; g_nvars = nv;
    ReleaseSRWLockExclusive(&g_map_lock);
    nodes = NULL; vlist = NULL; nn = nv = 0;
    ok = 1;
    goto done;
oom:
    set_err(err, err_cap, "there was not enough memory to read the map");
done:
    for (i = 0; i < nn; i++) free(nodes[i].name);
    for (i = 0; i < nv; i++) free(vlist[i].name);
    free(nodes);
    free(vlist);
    mpj_free(root);
    return ok;
}

char *sh_script_node_name_dup(int uid)
{
    int i;
    char *out = NULL;
    AcquireSRWLockShared(&g_map_lock);
    for (i = 0; i < g_nnodes; i++) {
        if (g_nodes[i].uid != uid) continue;
        out = _strdup(g_nodes[i].name);
        break;
    }
    ReleaseSRWLockShared(&g_map_lock);
    return out;
}

int sh_script_node_name_of(int uid, char *out, size_t cap)
{
    int i, found = 0;
    if (out && cap) out[0] = 0;
    AcquireSRWLockShared(&g_map_lock);
    for (i = 0; i < g_nnodes; i++) {
        if (g_nodes[i].uid != uid) continue;
        if (out) copyz(out, cap, g_nodes[i].name);
        found = 1;
        break;
    }
    ReleaseSRWLockShared(&g_map_lock);
    return found;
}

int sh_script_node_var_find(const char *name, int *type, int *index, int *other)
{
    int i, found = 0;
    if (type) *type = -1;
    if (index) *index = -1;
    if (other) *other = -1;
    if (!name) return 0;
    AcquireSRWLockShared(&g_map_lock);
    for (i = 0; i < g_nvars; i++) {
        if (_stricmp(g_vars[i].name, name)) continue;
        if (!found) {
            if (type) *type = g_vars[i].type;
            if (index) *index = g_vars[i].index;
            found = 1;
        } else {
            if (other) *other = g_vars[i].type;
            found = -1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_map_lock);
    return found;
}

int sh_script_node_uid_from_name(const char *game_name)
{
    const char *u, *p;
    long long v = 0;
    if (!game_name) return -1;
    u = strrchr(game_name, '_');
    if (!u || !u[1]) return -1;
    for (p = u + 1; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
        if (v > 0x7FFFFFFF) return -1;
    }
    return (int)v;
}

/* ========================================================== the engine */

static sh_script_node_ops g_ops;

void sh_script_node_set_ops(const sh_script_node_ops *ops)
{
    if (ops) g_ops = *ops;
    else memset(&g_ops, 0, sizeof g_ops);
}

int sh_script_node_post_input(int mod, sh_script_handle node, sh_script_handle activator, int uid)
{
    char *name = uid >= 0 ? sh_script_node_name_dup(uid) : NULL;
    int r = sh_script_queue_post(mod, SH_SCRIPT_EV_INPUT, node, activator, 0, 0.0f, name ? name : "", NULL);
    free(name);
    return r;
}

/* =============================================================== the API */

typedef struct node_ud { char name[1]; } node_ud;   /* the whole name follows, NUL-terminated */

static const char *var_type_name(int type)
{
    switch (type) {
    case SH_SCRIPT_VAR_INTEGER: return "an Integer";
    case SH_SCRIPT_VAR_NUMBER:  return "a Number";
    case SH_SCRIPT_VAR_BOOLEAN: return "a Boolean";
    case SH_SCRIPT_VAR_STRING:  return "a String";
    default:                    return "a";
    }
}

/* One line in the mod's log, once per map for `key` in the registry table
 * `reg` (a warning naming the calling script line). */
static void warn_once(lua_State *L, const char *reg, const char *key, const char *text)
{
    char file[512];
    int ln = 0;
    if (lua_getfield(L, LUA_REGISTRYINDEX, reg) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, reg);
    }
    if (lua_getfield(L, -1, key) != LUA_TNIL) { lua_pop(L, 2); return; }
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
    file[0] = 0;
    luaL_where(L, 1);
    sh_script_split_where(lua_tostring(L, -1), file, sizeof file, &ln);
    lua_pop(L, 1);
    if (file[0] && ln) lua_pushfstring(L, "%s line %d: %s", file, ln, text);
    else lua_pushstring(L, text);
    lua_warning(L, lua_tostring(L, -1), 0);
    lua_pop(L, 1);
}

static const char *check_node_name(lua_State *L, int arg, const char *call, size_t *len)
{
    size_t n = 0;
    const char *s = lua_type(L, arg) == LUA_TSTRING ? lua_tolstring(L, arg, &n) : NULL;
    if (!s || !n)
        sh_script_refuse(L, "\"%s\" needs the name of a Script node, got %s", call,
                         lua_isnoneornil(L, arg) ? "nothing" : (s ? "an empty name" : luaL_typename(L, arg)));
    *len = n;
    return s;
}

/* node:fire([activator]) */
static int node_fire(lua_State *L)
{
    node_ud *self = (node_ud *)luaL_testudata(L, 1, MT_NODE);
    sh_script_handle act = 0, nodes[SH_SCRIPT_NODE_PER_MOD];
    int mod, n, i, fired = 0;
    if (!self)
        return sh_script_refuse(L, "\"fire\" is called on a Script node, as node(\"name\"):fire(); it was called on %s",
                                lua_isnoneornil(L, 1) ? "nothing" : luaL_typename(L, 1));
    if (!lua_isnoneornil(L, 2)) act = (sh_script_handle)sh_script_check_entity(L, 2, "fire");
    /* every check is done: from here on nothing raises */
    mod = sh_script_hooks_mod_find(sh_script_salt(L));
    n = (mod >= 0 && g_ops.nodes) ? g_ops.nodes(g_ops.context, mod, nodes, SH_SCRIPT_NODE_PER_MOD) : 0;
    for (i = 0; i < n; i++) {
        int uid = g_ops.node_uid ? g_ops.node_uid(g_ops.context, nodes[i]) : -1;
        char *name = uid >= 0 ? sh_script_node_name_dup(uid) : NULL;
        int same = name && !_stricmp(name, self->name);
        free(name);
        if (!same) continue;
        if (g_ops.fire && g_ops.fire(g_ops.context, nodes[i], act)) fired++;
    }
    if (!fired && n >= 0) {
        luaL_Buffer key;
        const char *p;
        int top = lua_gettop(L);
        luaL_buffinit(L, &key);
        for (p = self->name; *p; p++) luaL_addchar(&key, fold(*p));
        luaL_pushresult(&key);
        lua_pushfstring(L, "node(\"%s\"):fire() found no Script node of %s named \"%s\" in this map, so nothing fired. "
                           "Place a Script node from %s and name it \"%s\".",
                        self->name, sh_script_mod_name(L), self->name, sh_script_mod_name(L), self->name);
        warn_once(L, REG_MISSED, lua_tostring(L, top + 1), lua_tostring(L, top + 2));
        lua_settop(L, top);
    }
    lua_pushinteger(L, fired);
    return 1;
}

static int node_tostring(lua_State *L)
{
    node_ud *self = (node_ud *)luaL_checkudata(L, 1, MT_NODE);
    lua_pushfstring(L, "Script node \"%s\"", self->name);
    return 1;
}

static int node_index(lua_State *L)
{
    node_ud *self = (node_ud *)luaL_checkudata(L, 1, MT_NODE);
    const char *key = lua_tostring(L, 2);
    if (key && !strcmp(key, "fire")) { lua_pushcfunction(L, node_fire); return 1; }
    if (key && !strcmp(key, "name")) { lua_pushstring(L, self->name); return 1; }
    lua_pushnil(L);
    return 1;
}

static int node_newindex(lua_State *L)
{
    return sh_script_refuse(L, "a Script node from node(\"name\") cannot be changed; keep your own values in a table");
}

int sh_script_node_lua_node(lua_State *L)
{
    size_t n = 0;
    const char *name = check_node_name(L, 1, "node", &n);
    node_ud *ud = (node_ud *)lua_newuserdatauv(L, sizeof *ud + n, 0);
    memcpy(ud->name, name, n + 1);
    if (luaL_newmetatable(L, MT_NODE)) {
        lua_pushcfunction(L, node_index); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, node_newindex); lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, node_tostring); lua_setfield(L, -2, "__tostring");
        lua_pushboolean(L, 0); lua_setfield(L, -2, "__metatable");
    }
    lua_setmetatable(L, -2);
    return 1;
}

/* The variable named at arg 1: its type and index, or a refusal. */
static const char *check_var(lua_State *L, const char *call, int *type, int *index)
{
    size_t n = 0;
    int other = -1, found;
    const char *s = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : NULL;
    if (!s || !n)
        sh_script_refuse(L, "\"%s\" needs the name of a map variable, got %s", call,
                         lua_isnoneornil(L, 1) ? "nothing" : (s ? "an empty name" : luaL_typename(L, 1)));
    found = sh_script_node_var_find(s, type, index, &other);
    if (found == 0)
        sh_script_refuse(L, "\"%s\": this map has no Integer, Number, Boolean or String variable named \"%s\"; "
                         "check the name in the map's variables", call, s);
    if (found < 0)
        sh_script_refuse(L, "\"%s\": this map has more than one variable named \"%s\" (%s and %s variable); "
                         "rename one of them in the editor", call, s, var_type_name(*type), var_type_name(other));
    return s;
}

/* 1 when the map's values can be read for this variable; otherwise its one
 * line (once per map) and 0. */
static int var_ready(lua_State *L, int type, int index, const char *name)
{
    int count = g_ops.var_count ? g_ops.var_count(g_ops.context, type) : -1;
    if (count > index) return 1;
    lua_pushfstring(L, "the map's variable \"%s\" could not be read here, so vars left it alone. "
                       "Map variables are available while the map is played.", name);
    warn_once(L, REG_VARS_OFF, name, lua_tostring(L, -1));
    lua_pop(L, 1);
    return 0;
}

static void push_value(lua_State *L, const sh_script_node_value *v)
{
    switch (v->type) {
    case SH_SCRIPT_VAR_INTEGER: lua_pushinteger(L, (lua_Integer)v->i); break;
    case SH_SCRIPT_VAR_NUMBER:  lua_pushnumber(L, (lua_Number)v->n); break;
    case SH_SCRIPT_VAR_BOOLEAN: lua_pushboolean(L, v->b); break;
    case SH_SCRIPT_VAR_STRING:  lua_pushstring(L, v->s); break;
    default:                    lua_pushnil(L); break;
    }
}

int sh_script_node_lua_vars_get(lua_State *L)
{
    const char *name;
    int type, index;
    sh_script_node_value *v;
    name = check_var(L, "vars.get", &type, &index);
    if (!var_ready(L, type, index, name) || !g_ops.var_get) { lua_pushnil(L); return 1; }
    v = (sh_script_node_value *)calloc(1, sizeof *v);
    if (!v) return luaL_error(L, "not enough memory");
    if (g_ops.var_get(g_ops.context, type, index, v) && v->type == type) push_value(L, v);
    else lua_pushnil(L);
    free(v->s);
    free(v);
    return 1;
}

int sh_script_node_lua_vars_set(lua_State *L)
{
    const char *name;
    int type, index, ok;
    sh_script_node_value in, *stored;
    name = check_var(L, "vars.set", &type, &index);
    memset(&in, 0, sizeof in);
    in.type = type;
    switch (type) {
    case SH_SCRIPT_VAR_INTEGER:
        if (lua_isinteger(L, 2)) in.i = (long long)lua_tointeger(L, 2);
        else {
            lua_Number d = lua_type(L, 2) == LUA_TNUMBER ? lua_tonumber(L, 2) : 0.5;
            if (lua_type(L, 2) != LUA_TNUMBER || d != floor(d))
                sh_script_refuse(L, "\"vars.set\": \"%s\" is an Integer variable, so it needs a whole number, got %s",
                                 name, lua_isnoneornil(L, 2) ? "nothing" :
                                 lua_type(L, 2) == LUA_TNUMBER ? luaL_tolstring(L, 2, NULL) : luaL_typename(L, 2));
            if (d < -2147483648.0 || d > 2147483647.0)
                sh_script_refuse(L, "\"vars.set\": \"%s\" is an Integer variable, and %s is too large for it",
                                 name, luaL_tolstring(L, 2, NULL));
            in.i = (long long)d;
        }
        if (in.i < -2147483647LL - 1 || in.i > 2147483647LL)
            sh_script_refuse(L, "\"vars.set\": \"%s\" is an Integer variable, and %s is too large for it", name,
                             luaL_tolstring(L, 2, NULL));
        break;
    case SH_SCRIPT_VAR_NUMBER:
        if (lua_type(L, 2) != LUA_TNUMBER)
            sh_script_refuse(L, "\"vars.set\": \"%s\" is a Number variable, so it needs a number, got %s", name,
                             lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
        in.n = (double)lua_tonumber(L, 2);
        if (in.n != in.n || in.n > 3.4e38 || in.n < -3.4e38)
            sh_script_refuse(L, "\"vars.set\": \"%s\" is a Number variable, and the value given is not a usable number", name);
        break;
    case SH_SCRIPT_VAR_BOOLEAN:
        if (lua_type(L, 2) != LUA_TBOOLEAN)
            sh_script_refuse(L, "\"vars.set\": \"%s\" is a Boolean variable, so it needs true or false, got %s", name,
                             lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
        in.b = lua_toboolean(L, 2);
        break;
    case SH_SCRIPT_VAR_STRING: {
        size_t n = 0;
        const char *s = lua_type(L, 2) == LUA_TSTRING ? lua_tolstring(L, 2, &n) : NULL;
        if (!s)
            sh_script_refuse(L, "\"vars.set\": \"%s\" is a String variable, so it needs text, got %s", name,
                             lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
        if (strlen(s) != n)
            sh_script_refuse(L, "\"vars.set\": \"%s\" is a String variable, and the text given holds a zero byte", name);
        in.s = (char *)s;   /* the script's text, alive for this call */
        break;
    }
    default:
        return sh_script_refuse(L, "\"vars.set\" cannot write \"%s\"", name);
    }
    /* every check is done: from here on nothing raises but memory */
    if (!var_ready(L, type, index, name) || !g_ops.var_set) { lua_pushnil(L); return 1; }
    stored = (sh_script_node_value *)calloc(1, sizeof *stored);
    if (!stored) return luaL_error(L, "not enough memory");
    ok = g_ops.var_set(g_ops.context, type, index, &in, stored);
    if (ok && stored->type == type) push_value(L, stored);
    else lua_pushnil(L);
    free(stored->s);
    free(stored);
    return 1;
}

static int vars_newindex(lua_State *L)
{
    return sh_script_refuse(L, "\"vars\" reads and writes the map's variables through vars.get and vars.set; it cannot be changed");
}

void sh_script_node_open_vars(lua_State *L)
{
    lua_newtable(L);                                   /* the functions */
    lua_pushcfunction(L, sh_script_node_lua_vars_get); lua_setfield(L, -2, "get");
    lua_pushcfunction(L, sh_script_node_lua_vars_set); lua_setfield(L, -2, "set");
    lua_newtable(L);                                   /* a read-only proxy */
    lua_newtable(L);
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, vars_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "vars");
    lua_pop(L, 1);
}

int sh_script_node_api_register(void)
{
    static int done;
    if (done) return 1;
    if (!sh_script_api_add(SH_SCRIPT_API_GLOBAL, "node", sh_script_node_lua_node)) return 0;
    if (!sh_script_api_add_opener(sh_script_node_open_vars)) return 0;
    done = 1;
    return 1;
}

/* ======================================================== signatures */

/* Build 13954591 (shared Ghidra, snaphak-re logs/shared-ghidra/SC7-*; each
 * pattern is the only match in each executable, tests/script_node_test.c):
 *   NodeFireOutput   VK 0x140A09AD0  GL 0x140A08CC0  FireOutput(entity, event, activator): stores event and
 *                    activator in its home slots and calls the target walk with their addresses.
 *   NodeVarChanged   VK 0x140891C60  GL 0x140890BA0  the logic manager's variable-changed(manager, type,
 *                    index); its sibling at +0x60 differs at +0x11 (+0x7EA0). */
const sig_entry SH_SCRIPT_NODE_SIGNATURES[] = {
    { "NodeFireOutput",
      "4C 89 44 24 18 89 54 24 10 48 83 EC 28 4C 8D 44 24 40 48 8D 54 24 38 E8 ?? ?? ?? ?? 48 83 C4 28 C3",
      0xA09AD0u, 0xA08CC0u },
    { "NodeVarChanged",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 99 80 7E 00 00 8B F2 8B 03 48 8D 53 04 41 8B F8 "
      "3B 02 75 1B 3D FE FF FF 01 74 1C 48 83 7B 08 00 75 15 48 83 3D ?? ?? ?? ?? 00 74 0B 48 8B D3 48 8B CB "
      "E8 ?? ?? ?? ?? 48 8B 4B 08 44 8B C7 8B D6",
      0x891C60u, 0x890BA0u },
    { NULL, NULL, 0, 0 }
};

/* Relay DoAction's first 16 bytes, whole and position-independent on both
 * renderers: mov [rsp+8],rbx; push rdi; sub rsp,0x20; mov rbx,rcx; mov rdi,rdx. */
const uint8_t SH_SCRIPT_NODE_RELAY_PROLOGUE[SH_SCRIPT_RELAY_STOLEN] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xFA
};

#ifndef SH_SCRIPT_PURE
/* ================================================== engine side (the DLL) */
#include "../backend_log.h"
#include "../entity.h"
#include "../hook.h"
#include "../companion_engine.h"

#define ENT_NAME_DATA_OFF     0x388u      /* idEntity: name idStr +0x378, its char data */
#define GAME_VARS_SLOT        0x720u      /* idGameLocal vtable: the idSnapGameVariables block */
#define GAME_LOGIC_OFF        0x298128u   /* idGameLocal -> SnapMap game; +0x48 the logic manager */
#define LOGIC_MANAGER_OFF     0x48u
#define VARS_INT_OFF          0x138u
#define VARS_INT_BOUNDS_OFF   0x150u
#define VARS_NUM_OFF          0x168u
#define VARS_NUM_BOUNDS_OFF   0x180u
#define VARS_STR_OFF          0x198u
#define VARS_BOOL_OFF         0x1C8u
#define IDSTR_SIZE            0x30u
#define IDSTR_LEN_OFF         0x08u
#define IDSTR_DATA_OFF        0x10u
#define VARS_MAX              100000      /* the engine's variable list read from its memory: a larger count
                                               means the layout is not the expected one, so nothing is read */

typedef void  (*relay_fn)(void *self, void *activator);
typedef void  (*fire_fn)(void *entity, int event, void *activator);
typedef void  (*var_changed_fn)(void *logic, int type, int index);
typedef void *(*vars_fn)(void *game);
typedef void  (*idstr_assign_fn)(void *str, const char *text);

static void           *g_relay_target;
static relay_fn        g_orig_relay;
static fire_fn         g_fire;
static var_changed_fn  g_var_changed;
static idstr_assign_fn g_idstr_assign;
static int             g_resolved;
static int             g_commit_failed;
static volatile LONG   g_unrun_logged;   /* a Script node fired while its scripts were not running */
static volatile LONG   g_fault_logged;

static void node_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

/* Only access violations are contained; an engine idException must reach
 * the engine's own handler. */
static int av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static int rd_ptr(const void *at, void **out)
{
    __try { *out = *(void *const *)at; return 1; }
    __except (av_only(GetExceptionCode())) { *out = NULL; return 0; }
}

static int rd_u32(const void *at, uint32_t *out)
{
    __try { *out = *(const uint32_t *)at; return 1; }
    __except (av_only(GetExceptionCode())) { *out = 0; return 0; }
}

/* The entity's spawn id, as the handle table records it. */
static uint32_t spawn_id_of(void *entity)
{
    void *gm = sh_entity_gamemgr();
    uint32_t num = 0, id = SH_SCRIPT_SPAWN_ID_NONE;
    if (!gm || !entity || !rd_u32((const uint8_t *)entity + SH_SCRIPT_ENT_NUMBER, &num) ||
        num >= SH_SCRIPT_ENT_NUMBER_WORLD) return SH_SCRIPT_SPAWN_ID_NONE;
    if (!rd_u32((const uint8_t *)gm + SH_SCRIPT_GAME_SPAWN_IDS + (size_t)num * 4, &id)) return SH_SCRIPT_SPAWN_ID_NONE;
    return id;
}

/* The uniqueId at the end of the entity's game name (a name of any length:
 * only the part after its last '_' is copied, which a uniqueId keeps to the
 * ten digits of an int). */
static int entity_uid(void *entity)
{
    char tail[16];
    void *data = NULL;
    size_t i, last = (size_t)-1, k = 0;
    const char *name;
    if (!entity || !rd_ptr((const uint8_t *)entity + ENT_NAME_DATA_OFF, &data) || !data) return -1;
    name = (const char *)data;
    __try {
        for (i = 0; name[i]; i++) if (name[i] == '_') last = i;
        if (last == (size_t)-1) return -1;
        tail[k++] = '_';
        for (i = last + 1; name[i]; i++) {
            if (k + 1 >= sizeof tail) return -1;   /* more digits than an int holds: no uniqueId */
            tail[k++] = name[i];
        }
        tail[k] = 0;
    } __except (av_only(GetExceptionCode())) { return -1; }
    return sh_script_node_uid_from_name(tail);
}

/* 1 when the recorded entityDef chain's first name is a Script node's. */
static int handle_is_node(sh_script_handle h)
{
    char names[SH_SCRIPT_NAMES_CAP], *nl;
    if (sh_script_handle_names(h, names, sizeof names) <= 0) return 0;
    nl = strchr(names, '\n');
    if (nl) *nl = 0;
    return sh_script_node_is_def(names, NULL, 0);
}

/* ---- the Relay DoAction detour (main thread, the game frame) ---- */

static void relay_detour(void *self, void *activator)
{
    int mod = -1;
    sh_script_handle node = self ? sh_script_hooks_handle_of(self, &mod) : 0;
    if (node && mod >= 0 && handle_is_node(node)) {
        sh_script_handle act = 0;
        if (activator) {
            uint32_t id = spawn_id_of(activator);
            act = sh_script_handles_find(id, NULL);
            if (!act) act = sh_script_handles_ref(id);
        }
        (void)sh_script_node_post_input(mod, node, act, entity_uid(self));
        return;                                   /* the script decides what the node does */
    }
    if (self) {
        const char *names[9];
        int n = sh_companions_def_chain(self, names, 9);
        if (n > 0 && sh_script_node_is_def(names[0], NULL, 0)) {
            /* A Script node whose scripts are not running here (a client, a
             * stopped mod, or scripts unavailable): it does nothing, as its
             * script would not run. */
            if (!InterlockedExchange(&g_unrun_logged, 1))
                node_log("SCRIPTS: a Script node received a signal while its mod's scripts were not running here; "
                         "it did nothing.");
            return;
        }
    }
    g_orig_relay(self, activator);
}

/* ---- the operations ---- */

static int op_nodes(void *context, int mod, sh_script_handle *out, int cap)
{
    void *gm = sh_entity_gamemgr();
    int num, n = 0;
    (void)context;
    if (!gm || mod < 0 || !out || cap <= 0) return 0;
    for (num = 0; num < SH_SCRIPT_ENT_NUMBER_WORLD && n < cap; num++) {
        uint32_t id = SH_SCRIPT_SPAWN_ID_NONE;
        sh_script_handle h;
        int m = -1;
        if (!rd_u32((const uint8_t *)gm + SH_SCRIPT_GAME_SPAWN_IDS + (size_t)num * 4, &id)) return n;
        if (id == SH_SCRIPT_SPAWN_ID_NONE || id == SH_SCRIPT_SPAWN_ID_WORLD) continue;
        h = sh_script_handles_find(id, &m);
        /* known, not only live: map_start runs before the first sweep reports the placed nodes */
        if (!h || m != mod || !sh_script_handle_known(h) || !handle_is_node(h)) continue;
        out[n++] = h;
    }
    return n;
}

/* The entity a handle names now, through idGameLocal vtable +0x7F8 (the
 * spawn-id lookup and the idEntity cast, script_handles.h): unlike
 * sh_script_handle_entity it also answers before the tick's first sweep has
 * reported the entity, which a map_start handler needs. NULL when gone. */
typedef void *(*lookup_fn)(void *gm, const uint32_t *spawn_id);
static void *entity_of(sh_script_handle h)
{
    void *gm = sh_entity_gamemgr(), **vt = NULL, *fn = NULL, *ent = NULL;
    uint32_t id = h ? sh_script_handle_spawn_id(h) : SH_SCRIPT_SPAWN_ID_NONE;
    if (!gm || id == SH_SCRIPT_SPAWN_ID_NONE || !sh_script_handle_known(h)) return NULL;
    if (!rd_ptr(gm, (void **)&vt) || !vt || !rd_ptr((const uint8_t *)vt + SH_SCRIPT_GAME_LOOKUP_SLOT + 8, &fn) || !fn)
        return NULL;
    __try { ent = ((lookup_fn)fn)(gm, &id); }
    __except (av_only(GetExceptionCode())) { ent = NULL; }
    return ent;
}

static int op_node_uid(void *context, sh_script_handle node)
{
    (void)context;
    return entity_uid(entity_of(node));
}

static int op_fire(void *context, sh_script_handle node, sh_script_handle activator)
{
    void *ent = entity_of(node), *act = activator ? entity_of(activator) : NULL;
    (void)context;
    if (!ent || !g_fire) return 0;
    __try { g_fire(ent, SH_SCRIPT_NODE_FIRED, act); }
    __except (av_only(GetExceptionCode())) {
        if (!InterlockedExchange(&g_fault_logged, 1)) node_log("SCRIPTS: firing a Script node's outputs faulted");
        return 0;
    }
    return 1;
}

/* The variables block, the logic manager, and the list of `type`. */
static int vars_of(int type, uint8_t **vars, void **logic, uint8_t **data, uint32_t *count)
{
    void *gm = sh_entity_gamemgr(), *game = NULL, **vt = NULL, *fn = NULL;
    size_t off;
    *vars = NULL; *logic = NULL; *data = NULL; *count = 0;
    switch (type) {
    case SH_SCRIPT_VAR_INTEGER: off = VARS_INT_OFF; break;
    case SH_SCRIPT_VAR_NUMBER:  off = VARS_NUM_OFF; break;
    case SH_SCRIPT_VAR_BOOLEAN: off = VARS_BOOL_OFF; break;
    case SH_SCRIPT_VAR_STRING:  off = VARS_STR_OFF; break;
    default: return 0;
    }
    if (!gm || !rd_ptr((const uint8_t *)gm + GAME_LOGIC_OFF, &game) || !game ||
        !rd_ptr((const uint8_t *)game + LOGIC_MANAGER_OFF, logic) || !*logic) return 0;
    if (!rd_ptr(gm, (void **)&vt) || !vt || !rd_ptr((const uint8_t *)vt + GAME_VARS_SLOT, &fn) || !fn) return 0;
    __try { *vars = (uint8_t *)((vars_fn)fn)(gm); }
    __except (av_only(GetExceptionCode())) { *vars = NULL; }
    if (!*vars || !rd_ptr(*vars + off, (void **)data) || !rd_u32(*vars + off + 8, count) ||
        *count > VARS_MAX || (*count && !*data)) return 0;
    return 1;
}

static int op_var_count(void *context, int type)
{
    uint8_t *vars, *data;
    void *logic;
    uint32_t count;
    (void)context;
    return vars_of(type, &vars, &logic, &data, &count) ? (int)count : -1;
}

static int read_value(int type, const uint8_t *data, int index, sh_script_node_value *out)
{
    out->type = type;
    __try {
        switch (type) {
        case SH_SCRIPT_VAR_INTEGER: out->i = ((const int32_t *)data)[index]; break;
        case SH_SCRIPT_VAR_NUMBER:  out->n = ((const float *)data)[index]; break;
        case SH_SCRIPT_VAR_BOOLEAN: out->b = data[index] != 0; break;
        case SH_SCRIPT_VAR_STRING: {
            const uint8_t *str = data + (size_t)index * IDSTR_SIZE;
            uint32_t len = *(const uint32_t *)(str + IDSTR_LEN_OFF);
            const char *text = *(const char *const *)(str + IDSTR_DATA_OFF);
            if (len && !text) return 0;
            out->s = (char *)malloc((size_t)len + 1);   /* the whole text, whatever its length */
            if (!out->s) return 0;
            if (len) memcpy(out->s, text, len);
            out->s[len] = 0;
            break;
        }
        default: return 0;
        }
    } __except (av_only(GetExceptionCode())) { return 0; }
    return 1;
}

static int op_var_get(void *context, int type, int index, sh_script_node_value *out)
{
    uint8_t *vars, *data;
    void *logic;
    uint32_t count;
    (void)context;
    if (!vars_of(type, &vars, &logic, &data, &count) || index < 0 || (uint32_t)index >= count) return 0;
    return read_value(type, data, index, out);
}

static int op_var_set(void *context, int type, int index, const sh_script_node_value *in, sh_script_node_value *stored)
{
    uint8_t *vars, *data, *bounds = NULL;
    void *logic;
    uint32_t count;
    int changed = 0;
    (void)context;
    if (!vars_of(type, &vars, &logic, &data, &count) || index < 0 || (uint32_t)index >= count) return 0;
    if (type == SH_SCRIPT_VAR_STRING && !g_idstr_assign) return 0;
    if ((type == SH_SCRIPT_VAR_INTEGER || type == SH_SCRIPT_VAR_NUMBER) &&
        !rd_ptr(vars + (type == SH_SCRIPT_VAR_INTEGER ? VARS_INT_BOUNDS_OFF : VARS_NUM_BOUNDS_OFF), (void **)&bounds))
        return 0;
    __try {
        switch (type) {
        case SH_SCRIPT_VAR_INTEGER: {
            int32_t *slot = (int32_t *)data + index;
            long long v = in->i, lo = LLONG_MIN, hi = LLONG_MAX;
            if (bounds) { lo = ((const int32_t *)bounds)[index * 2]; hi = ((const int32_t *)bounds)[index * 2 + 1]; }
            if (v < lo) v = lo;              /* the stock Operation's order: the minimum, then the maximum */
            if (v > hi) v = hi;
            if (*slot != (int32_t)v) { *slot = (int32_t)v; changed = 1; }
            break;
        }
        case SH_SCRIPT_VAR_NUMBER: {
            float *slot = (float *)data + index;
            double v = in->n;
            if (bounds) {
                double lo = ((const float *)bounds)[index * 2], hi = ((const float *)bounds)[index * 2 + 1];
                if (v < lo) v = lo;
                if (v > hi) v = hi;
            }
            if (*slot != (float)v) { *slot = (float)v; changed = 1; }
            break;
        }
        case SH_SCRIPT_VAR_BOOLEAN: {
            uint8_t *slot = data + index;
            uint8_t v = in->b ? 1 : 0;
            if (*slot != v) { *slot = v; changed = 1; }
            break;
        }
        case SH_SCRIPT_VAR_STRING: {
            uint8_t *str = data + (size_t)index * IDSTR_SIZE;
            uint32_t len = *(const uint32_t *)(str + IDSTR_LEN_OFF);
            const char *text = *(const char *const *)(str + IDSTR_DATA_OFF);
            size_t want = strlen(in->s);
            if (len != want || (len && (!text || memcmp(text, in->s, len)))) {
                g_idstr_assign(str, in->s);
                changed = 1;
            }
            break;
        }
        default:
            return 0;
        }
        if (changed && g_var_changed) g_var_changed(logic, type, index);
    } __except (av_only(GetExceptionCode())) {
        if (!InterlockedExchange(&g_fault_logged, 1)) node_log("SCRIPTS: writing a map variable faulted");
        return 0;
    }
    return read_value(type, data, index, stored);
}

static const sh_script_node_ops k_engine_ops = {
    NULL, op_nodes, op_node_uid, op_fire, op_var_count, op_var_get, op_var_set
};

static int relay_prologue_ok(void)
{
    __try { return g_relay_target && !memcmp(g_relay_target, SH_SCRIPT_NODE_RELAY_PROLOGUE, sizeof SH_SCRIPT_NODE_RELAY_PROLOGUE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int commit_relay(void)
{
    void *tramp;
    if (g_orig_relay && hook_is_installed((void *)g_orig_relay)) return 1;
    if (g_commit_failed) return 0;
    if (!relay_prologue_ok()) {
        node_log("Script nodes are unavailable: the game's Relay function is not the expected one (another tool may "
                 "have changed it).");
        g_commit_failed = 1;
        return 0;
    }
    tramp = hook_prepare(g_relay_target, (void *)relay_detour, SH_SCRIPT_RELAY_STOLEN);
    if (!tramp) {
        node_log("Script nodes are unavailable: the Relay hook could not be prepared (%s).", hook_last_failure());
        g_commit_failed = 1;
        return 0;
    }
    g_orig_relay = (relay_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_orig_relay = NULL;
        node_log("Script nodes are unavailable: the Relay hook could not be installed.");
        g_commit_failed = 1;
        return 0;
    }
    node_log("SCRIPTS: Script node hook installed at %p", g_relay_target);
    return 1;
}

void sh_script_node_play_begin(const char *json, size_t len)
{
    char err[160];
    if (!g_resolved) return;
    if (!json) {
        sh_script_node_map_clear();
        node_log("SCRIPTS: the played map could not be written, so its Script nodes have no names and its variables "
                 "cannot be named this Play.");
    } else if (!sh_script_node_map_load(json, len, err, sizeof err)) {
        node_log("SCRIPTS: the played map's Script nodes and variables could not be read: %s.", err);
    }
    (void)commit_relay();
}

int sh_script_node_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    sig_result r;
    int i;
    if (g_resolved) return 1;
    if (!module_base) return 0;
    if (sig_resolve_one(module_base, &SH_SCRIPT_SIGNATURES[SH_SCRIPT_SIG_RELAY_DOACTION], &r) != SIG_OK) {
        node_log("Script nodes are unavailable in this session: the game function they need (%s) was not found in "
                 "this executable.", SH_SCRIPT_SIGNATURES[SH_SCRIPT_SIG_RELAY_DOACTION].name);
        return 0;
    }
    g_relay_target = (void *)r.addr;
    for (i = 0; i < SH_SCRIPT_NODE_SIG_COUNT; i++) {
        if (sig_resolve_one(module_base, &SH_SCRIPT_NODE_SIGNATURES[i], &r) != SIG_OK) {
            node_log("Script nodes are unavailable in this session: the game function they need (%s) was not found "
                     "in this executable.", SH_SCRIPT_NODE_SIGNATURES[i].name);
            return 0;
        }
        if (i == SH_SCRIPT_NODE_SIG_FIRE_OUTPUT) g_fire = (fire_fn)r.addr;
        else g_var_changed = (var_changed_fn)r.addr;
    }
    g_idstr_assign = (idstr_assign_fn)sig_addr_by_name(results, count, "IdStrAssignCStr");
    if (!relay_prologue_ok()) {
        node_log("Script nodes are unavailable in this session: the game's Relay function is not the expected one "
                 "(another tool may have changed it).");
        return 0;
    }
    if (!sh_script_node_api_register()) {
        node_log("Script nodes are unavailable in this session: the script API table is full.");
        return 0;
    }
    sh_script_node_set_ops(&k_engine_ops);
    g_resolved = 1;
    backend_log(g_idstr_assign ? "SCRIPTS: Script nodes resolved; the Relay hook is installed at the first Play with scripts"
                               : "SCRIPTS: Script nodes resolved; String variables cannot be written in this session");
    return 1;
}
#endif /* SH_SCRIPT_PURE */
