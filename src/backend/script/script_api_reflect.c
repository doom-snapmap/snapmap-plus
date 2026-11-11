/* Reading and writing the game's objects by field name from a mod script
 * (script_api_reflect.h). The first half is values and the Lua calls, over a
 * host (the game's types and memory, or a test's); the second half, without
 * SH_SCRIPT_PURE, binds them to the engine. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script_api_common.h"
#include "script_api_reflect.h"

static sh_rf_host g_host;
static sh_script_reflect_ops g_ops;
static int g_bound;

void sh_script_api_reflect_set(const sh_rf_host *host, const sh_script_reflect_ops *ops)
{
    if (host && ops) { g_host = *host; g_ops = *ops; g_bound = 1; }
    else { memset(&g_host, 0, sizeof g_host); memset(&g_ops, 0, sizeof g_ops); g_bound = 0; }
}

static void say(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || !cap) return;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    err[cap - 1] = 0;
}

/* 1 when class `type` is `base` or descends from it. */
static int is_a(const char *type, const char *base)
{
    const char *c = type;
    int guard = 0;
    while (c && c[0] && guard++ < 512) {
        const char *super;
        if (!strcmp(c, base)) return 1;
        super = g_host.class_super ? g_host.class_super(g_host.ctx, c) : NULL;
        if (!super || !strcmp(super, c)) break;
        c = super;
    }
    return 0;
}

static int is_resource_class(const char *type)
{
    return is_a(type, "idResource") || is_a(type, "idDecl") || !strncmp(type, "idDecl", 6);
}

/* The game's types answer (they do once a map is up). */
static int ready(void)
{
    uint32_t size = 0;
    return g_bound && g_host.class_size && g_host.class_size(g_host.ctx, "idEntity", &size) && size;
}

static int rd(const void *addr, void *out, size_t n)
{
    return g_host.read && g_host.read(g_host.ctx, addr, out, n);
}

static int wr(const void *addr, const void *in, size_t n)
{
    return g_ops.write && g_ops.write(g_ops.ctx, (void *)addr, in, n);
}

static int read_int(const sh_rf_loc *loc, int is_signed, long long *out)
{
    unsigned char b[8] = { 0 };
    uint32_t n = loc->size;
    if (n != 1 && n != 2 && n != 4 && n != 8) return 0;
    if (!rd(loc->addr, b, n)) return 0;
    switch (n) {
    case 1: *out = is_signed ? (long long)(signed char)b[0] : (long long)b[0]; break;
    case 2: { uint16_t v; memcpy(&v, b, 2); *out = is_signed ? (long long)(int16_t)v : (long long)v; break; }
    case 4: { uint32_t v; memcpy(&v, b, 4); *out = is_signed ? (long long)(int32_t)v : (long long)v; break; }
    default: memcpy(out, b, 8); break;
    }
    return 1;
}

static int write_int(const sh_rf_loc *loc, long long v)
{
    uint32_t n = loc->size;
    if (n != 1 && n != 2 && n != 4 && n != 8) return 0;
    return wr(loc->addr, &v, n);          /* little-endian: the low bytes are the value */
}

static int push_fields(lua_State *L, const uint8_t *object, const char *cls, char *err, size_t cap);

int sh_script_reflect_push(lua_State *L, const sh_rf_loc *loc, char *err, size_t cap)
{
    long long i = 0;
    switch (loc->kind) {
    case SH_RF_BOOL: {
        unsigned char b = 0;
        if (!rd(loc->addr, &b, 1)) break;
        lua_pushboolean(L, b != 0);
        return 1;
    }
    case SH_RF_INT:
    case SH_RF_UINT:
        if (!read_int(loc, loc->kind == SH_RF_INT, &i)) break;
        lua_pushinteger(L, (lua_Integer)i);
        return 1;
    case SH_RF_FLOAT: {
        float f = 0;
        if (!rd(loc->addr, &f, sizeof f)) break;
        lua_pushnumber(L, (lua_Number)f);
        return 1;
    }
    case SH_RF_DOUBLE: {
        double d = 0;
        if (!rd(loc->addr, &d, sizeof d)) break;
        lua_pushnumber(L, (lua_Number)d);
        return 1;
    }
    case SH_RF_ENUM: {
        const char *name;
        if (!read_int(loc, 1, &i)) break;
        name = g_host.enum_name ? g_host.enum_name(g_host.ctx, loc->type, i) : NULL;
        if (name) lua_pushstring(L, name);
        else lua_pushinteger(L, (lua_Integer)i);
        return 1;
    }
    case SH_RF_STR: {
        size_t len = 0;
        char *text = g_ops.str_read ? g_ops.str_read(g_ops.ctx, loc->addr, &len) : NULL;
        if (!text) break;
        lua_pushlstring(L, text, len);
        free(text);
        return 1;
    }
    case SH_RF_STRUCT:
        return push_fields(L, loc->addr, loc->type, err, cap);
    case SH_RF_POINTER: {
        const void *p = NULL;
        if (!rd(loc->addr, (void *)&p, sizeof p)) break;
        if (!p) { lua_pushnil(L); return 1; }
        if (is_a(loc->type, "idEntity") && g_ops.push_entity) { g_ops.push_entity(L, g_ops.ctx, (void *)p); return 1; }
        if (is_resource_class(loc->type) && g_ops.resource_name) {
            const char *name = g_ops.resource_name(g_ops.ctx, p);
            if (name) { lua_pushstring(L, name); return 1; }
        }
        lua_pushboolean(L, 1);     /* set, and of a class only a path can step into */
        return 1;
    }
    case SH_RF_REF: {
        const void *obj = NULL;
        const char *cls = NULL;
        if (g_host.follow_ref && g_host.follow_ref(g_host.ctx, loc->addr, loc->type, &obj, &cls) && obj && g_ops.push_entity)
            g_ops.push_entity(L, g_ops.ctx, (void *)obj);
        else lua_pushnil(L);
        return 1;
    }
    case SH_RF_LIST:
    case SH_RF_ARRAY: {
        long long n = sh_rf_count(&g_host, loc), k;
        if (n < 0) break;
        lua_createtable(L, n > 0x7fffffff ? 0 : (int)n, 0);
        for (k = 0; k < n; k++) {
            sh_rf_loc el;
            int ok;
            if (!sh_rf_element(&g_host, loc, (uint32_t)k, &el, err, cap)) { lua_pop(L, 1); return 0; }
            ok = sh_script_reflect_push(L, &el, err, cap);
            sh_rf_loc_free(&el);
            if (!ok) { lua_pop(L, 1); return 0; }
            lua_rawseti(L, -2, (lua_Integer)(k + 1));
        }
        return 1;
    }
    default:
        lua_pushnil(L);            /* a type the game does not describe has no value a script can hold */
        return 1;
    }
    say(err, cap, "the game's memory there could not be read");
    return 0;
}

/* A table of every field of the object (ancestors first). */
static int push_fields(lua_State *L, const uint8_t *object, const char *cls, char *err, size_t cap)
{
    const char *chain[512];
    int depth = 0, d;
    const char *c = cls;
    while (c && c[0] && depth < 512) {
        const char *super;
        chain[depth++] = c;
        super = g_host.class_super ? g_host.class_super(g_host.ctx, c) : NULL;
        if (!super || !strcmp(super, c)) break;
        c = super;
    }
    lua_newtable(L);
    for (d = depth - 1; d >= 0; d--) {
        uint32_t i;
        sh_rf_field f;
        for (i = 0; g_host.class_field && g_host.class_field(g_host.ctx, chain[d], i, &f); i++) {
            sh_rf_loc loc;
            int ok;
            if (!f.name || !f.name[0]) continue;
            if (!sh_rf_classify(&g_host, f.type, f.ops, f.size, object + f.offset, &loc)) {
                lua_pop(L, 1);
                say(err, cap, "there was not enough memory");
                return 0;
            }
            ok = sh_script_reflect_push(L, &loc, err, cap);
            sh_rf_loc_free(&loc);
            if (!ok) { lua_pop(L, 1); return 0; }
            lua_setfield(L, -2, f.name);
        }
    }
    return 1;
}

static const char *lua_kind(lua_State *L, int idx)
{
    switch (lua_type(L, idx)) {
    case LUA_TNIL: return "nothing";
    case LUA_TBOOLEAN: return "true or false";
    case LUA_TNUMBER: return "a number";
    case LUA_TSTRING: return "text";
    case LUA_TTABLE: return "a table";
    default: return luaL_typename(L, idx);
    }
}

int sh_script_reflect_store(lua_State *L, const sh_rf_loc *loc, int idx, char *err, size_t cap)
{
    idx = lua_absindex(L, idx);
    switch (loc->kind) {
    case SH_RF_BOOL: {
        unsigned char b;
        if (!lua_isboolean(L, idx)) { say(err, cap, "it takes true or false, not %s", lua_kind(L, idx)); return 0; }
        b = (unsigned char)lua_toboolean(L, idx);
        if (!wr(loc->addr, &b, 1)) break;
        return 1;
    }
    case SH_RF_INT:
    case SH_RF_UINT: {
        int isnum = 0;
        lua_Integer v = lua_tointegerx(L, idx, &isnum);
        if (!isnum) { say(err, cap, "it takes a whole number, not %s", lua_kind(L, idx)); return 0; }
        if (!write_int(loc, (long long)v)) break;
        return 1;
    }
    case SH_RF_FLOAT: {
        float f;
        if (lua_type(L, idx) != LUA_TNUMBER) { say(err, cap, "it takes a number, not %s", lua_kind(L, idx)); return 0; }
        f = (float)lua_tonumber(L, idx);
        if (!wr(loc->addr, &f, sizeof f)) break;
        return 1;
    }
    case SH_RF_DOUBLE: {
        double d;
        if (lua_type(L, idx) != LUA_TNUMBER) { say(err, cap, "it takes a number, not %s", lua_kind(L, idx)); return 0; }
        d = (double)lua_tonumber(L, idx);
        if (!wr(loc->addr, &d, sizeof d)) break;
        return 1;
    }
    case SH_RF_ENUM: {
        long long v = 0;
        if (lua_type(L, idx) == LUA_TSTRING) {
            const char *name = lua_tostring(L, idx);
            if (!g_host.enum_value || !g_host.enum_value(g_host.ctx, loc->type, name, &v)) {
                say(err, cap, "%s has no member named %s", loc->type, name);
                return 0;
            }
        } else {
            int isnum = 0;
            v = (long long)lua_tointegerx(L, idx, &isnum);
            if (!isnum) { say(err, cap, "it takes a member of %s by name or number, not %s", loc->type, lua_kind(L, idx)); return 0; }
        }
        if (!write_int(loc, v)) break;
        return 1;
    }
    case SH_RF_STR:
        if (lua_type(L, idx) != LUA_TSTRING) { say(err, cap, "it takes text, not %s", lua_kind(L, idx)); return 0; }
        if (!g_ops.str_assign || !g_ops.str_assign(g_ops.ctx, (void *)loc->addr, lua_tostring(L, idx))) {
            say(err, cap, "the game's text could not be changed in this session");
            return 0;
        }
        return 1;
    case SH_RF_STRUCT:
        if (!lua_istable(L, idx)) { say(err, cap, "it is an object and takes a table of its fields, not %s", lua_kind(L, idx)); return 0; }
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            sh_rf_loc field;
            char why[256];
            int ok;
            if (lua_type(L, -2) != LUA_TSTRING) { lua_pop(L, 2); say(err, cap, "an object's fields are named with text"); return 0; }
            if (!sh_rf_resolve(&g_host, loc->addr, loc->type, lua_tostring(L, -2), &field, why, sizeof why)) {
                say(err, cap, "%s", why);
                lua_pop(L, 2);
                return 0;
            }
            ok = sh_script_reflect_store(L, &field, -1, why, sizeof why);
            sh_rf_loc_free(&field);
            if (!ok) { say(err, cap, "%s: %s", lua_tostring(L, -2), why); lua_pop(L, 2); return 0; }
            lua_pop(L, 1);
        }
        return 1;
    case SH_RF_POINTER: {
        void *p = NULL;
        if (lua_isnil(L, idx)) p = NULL;
        else if (is_a(loc->type, "idEntity")) {
            p = g_ops.to_entity ? g_ops.to_entity(L, g_ops.ctx, idx) : NULL;
            if (!p) { say(err, cap, "it takes an entity or nil, not %s", lua_kind(L, idx)); return 0; }
        } else if (is_resource_class(loc->type)) {
            if (lua_type(L, idx) != LUA_TSTRING) { say(err, cap, "it takes the name of a %s or nil, not %s", loc->type, lua_kind(L, idx)); return 0; }
            p = g_ops.resource_find ? g_ops.resource_find(g_ops.ctx, loc->type, lua_tostring(L, idx)) : NULL;
            if (!p) { say(err, cap, "the game has no %s named %s", loc->type, lua_tostring(L, idx)); return 0; }
        } else {
            say(err, cap, "it points at a %s, which a script cannot make; set the fields inside it", loc->type);
            return 0;
        }
        if (!wr(loc->addr, &p, sizeof p)) break;
        return 1;
    }
    case SH_RF_LIST:
    case SH_RF_ARRAY: {
        long long n = sh_rf_count(&g_host, loc), k;
        if (n < 0) break;
        if (!lua_istable(L, idx)) { say(err, cap, "it is a list and takes a list, not %s", lua_kind(L, idx)); return 0; }
        if ((long long)lua_rawlen(L, idx) != n) {
            say(err, cap, "the list holds %lld element%s and the value has %lld; a script sets a list's elements, not its length",
                n, n == 1 ? "" : "s", (long long)lua_rawlen(L, idx));
            return 0;
        }
        for (k = 0; k < n; k++) {
            sh_rf_loc el;
            char why[256];
            int ok;
            if (!sh_rf_element(&g_host, loc, (uint32_t)k, &el, err, cap)) return 0;
            lua_rawgeti(L, idx, (lua_Integer)(k + 1));
            ok = sh_script_reflect_store(L, &el, -1, why, sizeof why);
            lua_pop(L, 1);
            sh_rf_loc_free(&el);
            if (!ok) { say(err, cap, "[%lld]: %s", k + 1, why); return 0; }
        }
        return 1;
    }
    case SH_RF_REF:
        say(err, cap, "it is a reference the game keeps by itself; send the event that sets it instead");
        return 0;
    default:
        say(err, cap, "the game does not describe the type %s, so a script cannot write it", loc->type ? loc->type : "there");
        return 0;
    }
    say(err, cap, "the game's memory there could not be written");
    return 0;
}

/* ------------------------------------------------------------ the calls */

static const char *kind_name(int kind)
{
    switch (kind) {
    case SH_RF_BOOL: return "boolean";
    case SH_RF_INT: case SH_RF_UINT: return "integer";
    case SH_RF_FLOAT: case SH_RF_DOUBLE: return "number";
    case SH_RF_ENUM: return "enum";
    case SH_RF_STR: return "text";
    case SH_RF_STRUCT: return "object";
    case SH_RF_POINTER: return "pointer";
    case SH_RF_REF: return "entity";
    case SH_RF_LIST: case SH_RF_ARRAY: return "list";
    default: return "unknown";
    }
}

/* The entity of argument 1 and its class; refuses when reflection is not bound. */
static void *subject(lua_State *L, const char *call, const char **cls)
{
    void *ent = sh_script_api_entity(L, 1, call, NULL);
    if (!ready()) sh_script_refuse(L, "\"%s\" is unavailable: the game's type information could not be read in this session", call);
    if (!ent) return NULL;
    *cls = g_ops.entity_class ? g_ops.entity_class(g_ops.ctx, ent) : NULL;
    if (!*cls || !(*cls)[0]) *cls = "idEntity";
    return ent;
}

static int l_get(lua_State *L)
{
    const char *cls = NULL, *path = luaL_optstring(L, 2, "");
    void *ent = subject(L, "get", &cls);
    sh_rf_loc loc;
    char err[320];
    int ok;
    if (!ent) { lua_pushnil(L); return 1; }
    if (!sh_rf_resolve(&g_host, ent, cls, path, &loc, err, sizeof err))
        return sh_script_refuse(L, "\"get\" could not read \"%s\": %s", path, err);
    ok = sh_script_reflect_push(L, &loc, err, sizeof err);
    sh_rf_loc_free(&loc);
    if (!ok) return sh_script_refuse(L, "\"get\" could not read \"%s\": %s", path, err);
    return 1;
}

static int l_set(lua_State *L)
{
    const char *cls = NULL, *path = luaL_checkstring(L, 2);
    void *ent = subject(L, "set", &cls);
    sh_rf_loc loc;
    char err[320];
    int ok;
    luaL_checkany(L, 3);
    if (!ent) { lua_pushboolean(L, 0); return 1; }
    if (!path[0]) return sh_script_refuse(L, "\"set\" needs the field to write, such as \"team\"");
    if (!sh_rf_resolve(&g_host, ent, cls, path, &loc, err, sizeof err))
        return sh_script_refuse(L, "\"set\" could not write \"%s\": %s", path, err);
    ok = sh_script_reflect_store(L, &loc, 3, err, sizeof err);
    sh_rf_loc_free(&loc);
    if (!ok) return sh_script_refuse(L, "\"set\" could not write \"%s\": %s", path, err);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_fields(lua_State *L)
{
    const char *cls = NULL, *path = luaL_optstring(L, 2, "");
    void *ent = subject(L, "fields", &cls);
    sh_rf_loc loc;
    char err[320];
    const char *chain[512];
    int depth = 0, d, n = 0;
    const char *c;
    if (!ent) { lua_pushnil(L); return 1; }
    if (!sh_rf_resolve(&g_host, ent, cls, path, &loc, err, sizeof err))
        return sh_script_refuse(L, "\"fields\" could not read \"%s\": %s", path, err);
    if (loc.kind != SH_RF_STRUCT) {
        lua_newtable(L);
        lua_pushstring(L, kind_name(loc.kind)); lua_setfield(L, -2, "kind");
        lua_pushstring(L, loc.type ? loc.type : ""); lua_setfield(L, -2, "type");
        sh_rf_loc_free(&loc);
        return 1;
    }
    c = loc.type;
    while (c && c[0] && depth < 512) {
        const char *super;
        chain[depth++] = c;
        super = g_host.class_super ? g_host.class_super(g_host.ctx, c) : NULL;
        if (!super || !strcmp(super, c)) break;
        c = super;
    }
    lua_newtable(L);
    for (d = depth - 1; d >= 0; d--) {
        uint32_t i;
        sh_rf_field f;
        for (i = 0; g_host.class_field && g_host.class_field(g_host.ctx, chain[d], i, &f); i++) {
            sh_rf_loc fl;
            if (!f.name || !f.name[0]) continue;
            if (!sh_rf_classify(&g_host, f.type, f.ops, f.size, loc.addr + f.offset, &fl)) continue;
            lua_createtable(L, 0, 3);
            lua_pushstring(L, f.name); lua_setfield(L, -2, "name");
            lua_pushfstring(L, "%s%s", f.type ? f.type : "", f.ops ? f.ops : ""); lua_setfield(L, -2, "type");
            lua_pushstring(L, kind_name(fl.kind)); lua_setfield(L, -2, "kind");
            lua_rawseti(L, -2, (lua_Integer)++n);
            sh_rf_loc_free(&fl);
        }
    }
    sh_rf_loc_free(&loc);
    return 1;
}

static int l_class(lua_State *L)
{
    const char *cls = NULL;
    void *ent = subject(L, "class", &cls);
    if (!ent) { lua_pushnil(L); return 1; }
    lua_pushstring(L, cls);
    return 1;
}

/* enum(name) -> { MEMBER = number, ... } */
static int l_enum(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1), *member = NULL;
    long long value = 0;
    uint32_t i;
    if (!ready()) return sh_script_refuse(L, "\"enum\" is unavailable: the game's type information could not be read in this session");
    if (!g_ops.enum_member || !g_ops.enum_member(g_ops.ctx, name, 0, &member, &value))
        return sh_script_refuse(L, "\"enum\" found no enum named %s in the game", name);
    lua_newtable(L);
    for (i = 0; g_ops.enum_member(g_ops.ctx, name, i, &member, &value); i++) {
        lua_pushinteger(L, (lua_Integer)value);
        lua_setfield(L, -2, member);
    }
    return 1;
}

void sh_script_api_reflect_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sh_script_api_add(SH_SCRIPT_API_METHOD, "get", l_get);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "set", l_set);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "fields", l_fields);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "class", l_class);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "enum", l_enum);
}

#ifndef SH_SCRIPT_PURE
/* ============================================================ the engine */
#include <windows.h>
#include "../typeinfo.h"

#define ENT_DECL_OFF        0x6D0u      /* idEntity: its entityDef */
#define DECL_CLASSNAME_OFF  0x60u       /* idDeclEntityDef: the class name */
#define RESOURCE_NAME_OFF   0x08u       /* idResource: its name */
#define IDSTR_LEN_OFF       0x08u
#define IDSTR_DATA_OFF      0x10u

int sh_script_node_idstr_assign(void *str, const char *text);   /* script_node.c: the engine's idStr assign */
void *sh_script_combat_decl(const char *type, const char *name); /* script_api_combat.c: a decl by type and name */

static int e_class_size(void *c, const char *type, uint32_t *size) { (void)c; return sh_typeinfo_class(type, size, NULL); }
static const char *e_class_super(void *c, const char *type)
{
    const char *super = NULL;
    (void)c;
    return sh_typeinfo_class(type, NULL, &super) ? super : NULL;
}
static int e_class_field(void *c, const char *type, uint32_t index, sh_rf_field *out)
{
    sh_ti_field f;
    (void)c;
    if (!sh_typeinfo_class_field(type, index, &f)) return 0;
    out->type = f.type; out->ops = f.ops; out->name = f.name; out->offset = f.offset; out->size = f.size;
    return 1;
}
static int e_enum_member(void *c, const char *type, uint32_t index, const char **member, long long *value)
{
    (void)c;
    return sh_typeinfo_enum_member(type, index, member, value);
}
static int e_enum_value(void *c, const char *type, const char *member, long long *value)
{
    uint32_t i;
    const char *name = NULL;
    long long v = 0;
    (void)c;
    for (i = 0; sh_typeinfo_enum_member(type, i, &name, &v); i++) {
        if (!member) return 1;
        if (name && !_stricmp(name, member)) { if (value) *value = v; return 1; }
    }
    return 0;
}
static const char *e_enum_name(void *c, const char *type, long long value)
{
    uint32_t i;
    const char *name = NULL;
    long long v = 0;
    (void)c;
    for (i = 0; sh_typeinfo_enum_member(type, i, &name, &v); i++)
        if (v == value) return name;
    return NULL;
}
static int e_read(void *c, const void *addr, void *out, size_t n)
{
    (void)c;
    if (!addr) return 0;
    __try { memcpy(out, addr, n); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
static int e_write(void *c, void *addr, const void *in, size_t n)
{
    (void)c;
    if (!addr) return 0;
    __try { memcpy(addr, in, n); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
static const char *e_entity_class(void *c, void *ent)
{
    const char *name = NULL;
    (void)c;
    __try {
        const uint8_t *decl = *(const uint8_t *const *)((const uint8_t *)ent + ENT_DECL_OFF);
        if (decl) name = *(const char *const *)(decl + DECL_CLASSNAME_OFF);
        if (name && !name[0]) name = NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { name = NULL; }
    return name;
}
static int e_follow_ref(void *c, const void *addr, const char *declared, const void **object, const char **cls)
{
    const sh_script_engine *e = sh_script_api_engine();
    uint32_t id = 0;
    void *ent;
    (void)c;
    if (!e || !e->resolve || !e_read(NULL, (const uint8_t *)addr + SH_RF_REF_SPAWN_ID, &id, sizeof id)) return 0;
    ent = e->resolve(id);
    if (!ent) return 0;
    *object = ent;
    *cls = e_entity_class(NULL, ent);
    if (!*cls) *cls = declared;
    return 1;
}
static const char *e_object_class(void *c, const void *object, const char *declared)
{
    (void)c;
    if (is_a(declared, "idEntity")) return e_entity_class(NULL, (void *)object);
    return NULL;
}
static char *e_str_read(void *c, const void *idstr, size_t *len)
{
    char *out = NULL;
    (void)c;
    __try {
        int n = *(const int *)((const uint8_t *)idstr + IDSTR_LEN_OFF);
        const char *data = *(const char *const *)((const uint8_t *)idstr + IDSTR_DATA_OFF);
        if (n >= 0 && data && (out = (char *)malloc((size_t)n + 1)) != NULL) {
            memcpy(out, data, (size_t)n);
            out[n] = 0;
            *len = (size_t)n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(out); out = NULL; }
    return out;
}
static int e_str_assign(void *c, void *idstr, const char *text) { (void)c; return sh_script_node_idstr_assign(idstr, text); }
static const char *e_resource_name(void *c, const void *object)
{
    const char *name = NULL;
    (void)c;
    __try { name = *(const char *const *)((const uint8_t *)object + RESOURCE_NAME_OFF); if (name && !name[0]) name = NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { name = NULL; }
    return name;
}
/* The decl type a class names: idDeclSnapAIEncounter -> snapAIEncounter, idMaterial -> material. */
static void *e_resource_find(void *c, const char *cls, const char *name)
{
    char type[256];
    const char *t = cls;
    (void)c;
    if (!strncmp(t, "idDecl", 6)) t += 6;
    else if (!strncmp(t, "id", 2)) t += 2;
    if (!t[0] || strlen(t) >= sizeof type) return NULL;
    strcpy_s(type, sizeof type, t);
    type[0] = (char)tolower((unsigned char)type[0]);
    return sh_script_combat_decl(type, name);
}
static void e_push_entity(lua_State *L, void *c, void *ent) { (void)c; sh_script_api_push_engine_entity(L, ent); }
static void *e_to_entity(lua_State *L, void *c, int idx)
{
    (void)c;
    if (!luaL_testudata(L, idx, "entity")) return NULL;
    return sh_script_api_entity(L, idx, "set", NULL);
}

int sh_script_api_reflect_install(void)
{
    static const sh_rf_host host = { NULL, e_class_size, e_class_super, e_class_field, e_enum_value, e_enum_name, e_read,
                                     e_follow_ref, e_object_class };
    static const sh_script_reflect_ops ops = { NULL, e_entity_class, e_write, e_str_read, e_str_assign, e_resource_name,
                                               e_resource_find, e_push_entity, e_to_entity, e_enum_member };
    sh_script_api_reflect_register();
    sh_script_api_reflect_set(&host, &ops);
    return 1;
}
#endif /* SH_SCRIPT_PURE */
