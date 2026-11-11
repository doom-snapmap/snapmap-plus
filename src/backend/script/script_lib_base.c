/* script_lib_base.c -- the libraries a mod script sees, timers and
 * coroutines, require, print and warn (script_runtime.h has the list).
 *
 * Library functions that do work in C the clock hook never sees read the
 * wall clock first (sorting, moving and shifting table elements, scanning
 * UTF-8), so a script looping over them is still stopped when one call has
 * run too long; pcall, xpcall, coroutine.resume and coroutine.close are
 * wrapped so a stop decided by the runtime cannot be caught; setmetatable
 * refuses "__gc" because Lua runs finalizers with hooks off, so a finalizer
 * could never be stopped. */
#include "script_internal.h"
#include "script_reason.h"
#include "../../third_party/lua/lualib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ guards */

/* After the wrapped call: a stop the runtime decided goes on up; a memory
 * error the script caught becomes the memory stop. Results start at 1. */
static int guard_k(lua_State *L, int status, lua_KContext ctx)
{
    shs_mod *m = shs_mod_of(L);
    (void)status;
    (void)ctx;
    if (m->stop >= 0) return shs_raise_stop(L, 1);
    if (lua_gettop(L) >= 2 && lua_type(L, 1) == LUA_TBOOLEAN && !lua_toboolean(L, 1) &&
        lua_type(L, 2) == LUA_TSTRING && strcmp(lua_tostring(L, 2), "not enough memory") == 0) {
        m->stop = SH_SCRIPT_STOP_MEMORY;
        return shs_raise_stop(L, 1);
    }
    return lua_gettop(L);
}

/* pcall, xpcall, coroutine.resume, coroutine.close: the original is
 * upvalue 1. lua_callk keeps a yield inside a pcall'd function working. */
static int guard(lua_State *L)
{
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_callk(L, n, LUA_MULTRET, 0, guard_k);
    return guard_k(L, LUA_OK, 0);
}

/* xpcall's message handler, upvalue 1 the script's. A stop raised by the
 * count hook reaches the handler with hooks still off (Lua calls it from
 * inside the hook), so after a stop the script's handler is not run. */
static int xpcall_handler(lua_State *L)
{
    if (shs_mod_of(L)->stop >= 0) return 1;
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

/* xpcall(f, handler, ...): the handler wrapped, then as guard. */
static int sb_xpcall(lua_State *L)
{
    int n;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, xpcall_handler, 1);
    lua_replace(L, 2);
    n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_callk(L, n, LUA_MULTRET, 0, guard_k);
    return guard_k(L, LUA_OK, 0);
}

static int sb_setmetatable(lua_State *L)
{
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushstring(L, "__gc");
        if (lua_rawget(L, 2) != LUA_TNIL)
            return sh_script_refuse(L, "\"setmetatable\" cannot take a metatable with \"__gc\": scripts cannot run code when a value is freed");
        lua_pop(L, 1);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

static int sb_collectgarbage(lua_State *L)
{
    const char *opt = luaL_optstring(L, 1, "collect");
    if (strcmp(opt, "count") != 0)
        return sh_script_refuse(L, "\"collectgarbage\" only answers \"count\" here; the game collects memory itself");
    lua_pushnumber(L, (lua_Number)lua_gc(L, LUA_GCCOUNT, 0) + (lua_Number)lua_gc(L, LUA_GCCOUNTB, 0) / 1024.0);
    return 1;
}

/* ------------------------------------------------------- print and warn */

/* The arguments joined with tabs, pushed as one string (any length). */
static const char *join_args(lua_State *L)
{
    int n = lua_gettop(L), i;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    return lua_tostring(L, -1);
}

static int sb_print(lua_State *L)
{
    shs_log(shs_mod_of(L), join_args(L));
    return 0;
}

static int sb_warn(lua_State *L)
{
    char file[SHS_PATH_CAP];
    int ln = 0, at;
    join_args(L);
    at = lua_gettop(L);
    luaL_where(L, 1);
    sh_script_split_where(lua_tostring(L, -1), file, sizeof file, &ln);
    lua_pop(L, 1);
    if (file[0] && ln) lua_pushfstring(L, "Warning: %s line %d: %s", file, ln, lua_tostring(L, at));
    else lua_pushfstring(L, "Warning: %s", lua_tostring(L, at));
    shs_log(shs_mod_of(L), lua_tostring(L, -1));
    return 0;
}

/* Lua's own warnings (lua_warning from the core), pieces joined. */
static void core_warn(void *ud, const char *msg, int tocont)
{
    static char *buf;
    static size_t used;
    shs_mod *m = (shs_mod *)ud;
    size_t l = strlen(msg);
    char *grown;
    if (!used && msg[0] == '@') return;          /* control messages */
    grown = (char *)realloc(buf, used + l + 10);
    if (!grown) { free(buf); buf = NULL; used = 0; return; }
    buf = grown;
    if (!used) { memcpy(buf, "Warning: ", 9); used = 9; }
    memcpy(buf + used, msg, l);
    used += l;
    buf[used] = 0;
    if (tocont) return;
    shs_log(m, buf);
    used = 0;
}

/* ----------------------------------------------------- string and table */

/* find, match, gmatch, gsub: upvalue 1 the original, 2 its name. */
static int sb_pattern(lua_State *L)
{
    int n;
    luaL_checkstring(L, 1);
    luaL_checkstring(L, 2);
    shs_check_clock(L);
    n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

/* table.sort, move, insert, remove, concat and utf8.len, offset: upvalue 1
 * the original, 2 which kind (kept for the wrapper's registration). */
#define CH_SORT 1
#define CH_MOVE 2
#define CH_SHIFT 3
#define CH_CONCAT 4
#define CH_SCAN 5
static int sb_charged(lua_State *L)
{
    int n;
    shs_check_clock(L);
    n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

/* Replace t[name] (t at -1) by `fn` with the original as upvalue 1 and
 * `extra` (a pushed value, or none when has_extra is 0) as upvalue 2. */
static void wrap(lua_State *L, const char *name, lua_CFunction fn, int extra_kind, lua_Integer extra_int, const char *extra_str)
{
    int nup = 1;
    lua_getfield(L, -1, name);
    if (extra_kind == 1) { lua_pushinteger(L, extra_int); nup = 2; }
    else if (extra_kind == 2) { lua_pushstring(L, extra_str); nup = 2; }
    lua_pushcclosure(L, fn, nup);
    lua_setfield(L, -2, name);
}

/* ------------------------------------------------------- now and game */

static int sb_now(lua_State *L)
{
    lua_pushnumber(L, shs_now());
    return 1;
}

static int game_tostring(lua_State *L)
{
    lua_getiuservalue(L, 1, 1);
    return 1;
}

static int sb_game(lua_State *L)
{
    size_t n, i;
    const char *s = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : NULL;
    if (!s || !n || n >= SH_SCRIPT_DEFNAME_CAP)
        return sh_script_refuse(L, "\"game\" needs the name of a game decl, as game(\"fx/impacts/rocket\")");
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.' || c == '/' || c == '\\'))
            return sh_script_refuse(L, "\"game\" was given \"%s\", which is not a usable name", s);
    }
    lua_newuserdatauv(L, 1, 1);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, SHS_MT_GAME);
    return 1;
}

/* ------------------------------------------------------------- require */

static int sb_require(lua_State *L)
{
    shs_mod *m = shs_mod_of(L);
    size_t n, i;
    const char *name = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : NULL;
    char rel[96];
    int status;
    if (!name || !n || n > 64)
        return sh_script_refuse(L, "\"require\" needs the name of a file in scripts\\lib, as require(\"fsm\") for scripts\\lib\\fsm.lua");
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return sh_script_refuse(L, "\"require\" was given \"%s\"; use the file's name in scripts\\lib without folders or \".lua\"", name);
    }
    lua_settop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_LOADED);             /* 2 */
    lua_pushvalue(L, 1);
    if (lua_rawget(L, 2) != LUA_TNIL) {
        if (lua_type(L, -1) == LUA_TLIGHTUSERDATA)
            return sh_script_refuse(L, "\"%s\" was required while it was still loading; two files in scripts\\lib require each other", name);
        return 1;
    }
    lua_pop(L, 1);
    lua_pushvalue(L, 1);
    lua_pushlightuserdata(L, (void *)m);
    lua_rawset(L, 2);                                               /* loading */
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "lib/%s.lua", name);
    rel[sizeof rel - 1] = 0;
    status = shs_load_file(L, m, rel);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        lua_pushvalue(L, 1); lua_pushnil(L); lua_rawset(L, 2);
        if (status == LUA_ERRFILE && msg && strstr(msg, "is not in this mod"))
            return sh_script_refuse(L, "\"require\" found no scripts\\lib\\%s.lua in this mod", name);
        return lua_error(L);
    }
    lua_pushvalue(L, 1);
    status = lua_pcall(L, 1, 1, 0);
    if (status != LUA_OK) {
        if (status == LUA_ERRMEM && m->stop < 0) m->stop = SH_SCRIPT_STOP_MEMORY;
        lua_pushvalue(L, 1); lua_pushnil(L); lua_rawset(L, 2);
        return lua_error(L);
    }
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
    lua_pushvalue(L, 1);
    lua_pushvalue(L, -2);
    lua_rawset(L, 2);
    return 1;
}

/* -------------------------------------------------- timers, coroutines */

typedef struct { int id; } shs_timer_ud;

static int timer_index(shs_mod *m, int id)
{
    int i;
    for (i = 0; i < m->ntimers; i++) if (m->timers[i].id == id) return i;
    return -1;
}

static void timer_drop(lua_State *L, shs_mod *m, int id)
{
    int i = timer_index(m, id);
    if (i >= 0) {
        memmove(&m->timers[i], &m->timers[i + 1], (size_t)(m->ntimers - i - 1) * sizeof m->timers[0]);
        m->ntimers--;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_TIMERS);
    lua_pushnil(L);
    lua_rawseti(L, -2, id);
    lua_pop(L, 1);
}

static void push_timer(lua_State *L, int id)
{
    shs_timer_ud *t = (shs_timer_ud *)lua_newuserdatauv(L, sizeof *t, 0);
    t->id = id;
    luaL_setmetatable(L, SHS_MT_TIMER);
}

/* Add a timer whose function or coroutine is at `value`; push its handle. */
static void add_timer(lua_State *L, const char *call, int kind, double due, double period, int value)
{
    shs_mod *m = shs_mod_of(L);
    int id;
    (void)call;
    if (m->ntimers == m->ctimers) {
        int c = m->ctimers ? m->ctimers * 2 : 16;
        shs_timer *p = (shs_timer *)realloc(m->timers, (size_t)c * sizeof *p);
        if (!p) luaL_error(L, "not enough memory");
        m->timers = p;
        m->ctimers = c;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_TIMERS);
    id = ++m->next_timer;
    lua_pushvalue(L, value);
    lua_rawseti(L, -2, id);
    lua_pop(L, 1);
    m->timers[m->ntimers].id = id;
    m->timers[m->ntimers].kind = kind;
    m->timers[m->ntimers].due = due;
    m->timers[m->ntimers].period = period;
    m->ntimers++;
    push_timer(L, id);
}

static double check_seconds(lua_State *L, int arg, const char *call)
{
    double s;
    if (lua_type(L, arg) != LUA_TNUMBER)
        sh_script_refuse(L, "\"%s\" needs a number of seconds as its first value, got %s", call,
                         lua_isnoneornil(L, arg) ? "nothing" : luaL_typename(L, arg));
    s = (double)lua_tonumber(L, arg);
    if (!(s >= 0) || s > 1e9)
        sh_script_refuse(L, "\"%s\" needs a number of seconds of 0 or more", call);
    return s;
}

static void check_fn(lua_State *L, int arg, const char *call, const char *which)
{
    if (lua_type(L, arg) != LUA_TFUNCTION)
        sh_script_refuse(L, "\"%s\" needs a function as its %s value, got %s", call, which,
                         lua_isnoneornil(L, arg) ? "nothing" : luaL_typename(L, arg));
}

static int sb_after(lua_State *L)
{
    double s = check_seconds(L, 1, "after");
    check_fn(L, 2, "after", "second");
    add_timer(L, "after", SHS_TIMER_AFTER, shs_now() + s, 0, 2);
    return 1;
}

static int sb_every(lua_State *L)
{
    double s = check_seconds(L, 1, "every");
    check_fn(L, 2, "every", "second");
    add_timer(L, "every", SHS_TIMER_EVERY, shs_now() + s, s, 2);
    return 1;
}

/* How long a coroutine that yielded asked to wait: a number it yielded,
 * else until the next frame. */
static double yielded_wait(lua_State *co, int nres)
{
    if (nres >= 1 && lua_type(co, -nres) == LUA_TNUMBER) {
        double s = (double)lua_tonumber(co, -nres);
        if (s >= 0 && s <= 1e9) return s;
    }
    return 0;
}

/* A coroutine failed: carry its error (and where it was raised) to L. */
static int co_error(lua_State *L, lua_State *co, int status)
{
    shs_mod *m = shs_mod_of(L);
    char file[128];
    int line = 0;
    file[0] = 0;
    if (status == LUA_ERRMEM && m->stop < 0) m->stop = SH_SCRIPT_STOP_MEMORY;
    if (m->stop >= 0) {
        /* a stop cannot be caught, so it reaches the message handler: record
         * the coroutine's line, which L's stack does not hold */
        if (!m->err_file[0]) {
            if (lua_isstring(co, -1)) sh_script_split_where(lua_tostring(co, -1), m->err_file, sizeof m->err_file, &m->err_line);
            if (!m->err_file[0]) shs_where_from(m, co, 0);
        }
        return shs_raise_stop(L, 1);
    }
    /* an ordinary error may be caught by the script: carry the coroutine's
     * line in the message itself */
    if (lua_type(co, -1) == LUA_TSTRING) sh_script_split_where(lua_tostring(co, -1), file, sizeof file, &line);
    if (!file[0] && lua_type(co, -1) == LUA_TSTRING) {
        lua_Debug ar;
        int level;
        for (level = 0; lua_getstack(co, level, &ar); level++) {
            if (!lua_getinfo(co, "Sl", &ar)) break;
            if (ar.currentline > 0 && strncmp(ar.short_src, "scripts\\", 8) == 0) {
                lua_pushfstring(L, "%s:%d: %s", ar.short_src, ar.currentline, lua_tostring(co, -1));
                return lua_error(L);
            }
        }
    }
    lua_xmove(co, L, 1);
    return lua_error(L);
}

static int sb_run(lua_State *L)
{
    shs_mod *m = shs_mod_of(L);
    lua_State *co;
    int nres = 0, status;
    check_fn(L, 1, "run", "first");
    lua_settop(L, 1);
    co = lua_newthread(L);                                          /* 2 */
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_RUNS);
    lua_pushvalue(L, 2);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);
    status = lua_resume(co, L, 0, &nres);
    if (status == LUA_YIELD) {
        double s = yielded_wait(co, nres);
        lua_pop(co, nres);
        add_timer(L, "run", SHS_TIMER_RUN, shs_now() + s, 0, 2);
        return 1;
    }
    if (status != LUA_OK) return co_error(L, co, status);
    lua_pop(co, nres);
    (void)m;
    push_timer(L, 0);
    return 1;
}

static int sb_wait(lua_State *L)
{
    double s = lua_isnoneornil(L, 1) ? 0 : check_seconds(L, 1, "wait");
    int ours;
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_RUNS);
    lua_pushthread(L);
    ours = lua_rawget(L, -2) == LUA_TBOOLEAN;
    lua_pop(L, 2);
    if (!ours || !lua_isyieldable(L))
        return sh_script_refuse(L, "\"wait\" can only be used inside run(function() ... end)");
    lua_pushnumber(L, s);
    return lua_yield(L, 1);
}

static int timer_cancel(lua_State *L)
{
    shs_timer_ud *t = (shs_timer_ud *)luaL_checkudata(L, 1, SHS_MT_TIMER);
    if (t->id) timer_drop(L, shs_mod_of(L), t->id);
    return 0;
}

static int timer_tostring(lua_State *L)
{
    lua_pushstring(L, "timer");
    return 1;
}

typedef struct { int id; } shs_fire;

/* Job: fire one due timer or resume one waiting coroutine. */
static int job_timer(lua_State *L)
{
    shs_fire *f = (shs_fire *)lua_touserdata(L, 1);
    shs_mod *m = shs_mod_of(L);
    int i = timer_index(m, f->id);
    shs_timer t;
    if (i < 0) return 0;                            /* cancelled by an earlier one */
    t = m->timers[i];
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_TIMERS);
    lua_rawgeti(L, -1, t.id);
    if (t.kind == SHS_TIMER_AFTER) {
        timer_drop(L, m, t.id);
        shs_call_handler(L, 0);
    } else if (t.kind == SHS_TIMER_EVERY) {
        double due = t.due + t.period;
        if (due <= shs_now()) due = shs_now() + t.period;
        m->timers[i].due = due;
        shs_call_handler(L, 0);
    } else {
        lua_State *co = lua_tothread(L, -1);
        int nres = 0, status;
        if (!co) { timer_drop(L, m, t.id); return 0; }
        shs_call_reset(m);
        status = lua_resume(co, L, 0, &nres);
        if (status == LUA_YIELD) {
            double s = yielded_wait(co, nres);
            lua_pop(co, nres);
            i = timer_index(m, t.id);
            if (i >= 0) m->timers[i].due = shs_now() + s;
        } else if (status == LUA_OK) {
            lua_pop(co, nres);
            timer_drop(L, m, t.id);
        } else return co_error(L, co, status);
    }
    return 0;
}

static int due_cmp(const void *a, const void *b)
{
    const shs_timer *x = (const shs_timer *)a, *y = (const shs_timer *)b;
    if (x->due < y->due) return -1;
    if (x->due > y->due) return 1;
    return x->id - y->id;
}

void shs_timers_run(shs_mod *m)
{
    shs_timer *due;
    int n = 0, i, limit = m->next_timer;
    double now = shs_now();
    if (!m->ntimers || m->status != SH_SCRIPT_RUNNING) return;
    due = (shs_timer *)malloc((size_t)m->ntimers * sizeof *due);
    if (!due) return;
    for (i = 0; i < m->ntimers; i++)
        if (m->timers[i].due <= now && m->timers[i].id <= limit) due[n++] = m->timers[i];
    qsort(due, (size_t)n, sizeof *due, due_cmp);
    for (i = 0; i < n; i++) {
        shs_fire f;
        f.id = due[i].id;
        if (!shs_run_job(m, job_timer, &f)) break;
    }
    free(due);
}

void shs_timers_free(shs_mod *m)
{
    free(m->timers);
    m->timers = NULL;
    m->ntimers = m->ctimers = 0;
}

/* ------------------------------------------------------------- opening */

void shs_open_libs(lua_State *L, uint64_t seed)
{
    static const luaL_Reg libs[] = {
        { LUA_GNAME, luaopen_base }, { LUA_STRLIBNAME, luaopen_string }, { LUA_TABLIBNAME, luaopen_table },
        { LUA_MATHLIBNAME, luaopen_math }, { LUA_UTF8LIBNAME, luaopen_utf8 }, { LUA_COLIBNAME, luaopen_coroutine },
    };
    static const char *const removed[] = { "dofile", "loadfile", "load", "require", "collectgarbage", "print", "warn" };
    size_t i;
    shs_mod *m = shs_mod_of(L);
    for (i = 0; i < sizeof libs / sizeof libs[0]; i++) {
        luaL_requiref(L, libs[i].name, libs[i].func, 1);
        lua_pop(L, 1);
    }
    lua_setwarnf(L, core_warn, m);

    lua_pushglobaltable(L);
    for (i = 0; i < sizeof removed / sizeof removed[0]; i++) { lua_pushnil(L); lua_setfield(L, -2, removed[i]); }
    wrap(L, "pcall", guard, 0, 0, NULL);
    wrap(L, "xpcall", sb_xpcall, 0, 0, NULL);
    wrap(L, "setmetatable", sb_setmetatable, 0, 0, NULL);
    lua_pushcfunction(L, sb_collectgarbage); lua_setfield(L, -2, "collectgarbage");
    lua_pushcfunction(L, sb_print); lua_setfield(L, -2, "print");
    lua_pushcfunction(L, sb_warn); lua_setfield(L, -2, "warn");
    lua_pushcfunction(L, sb_require); lua_setfield(L, -2, "require");
    lua_pushcfunction(L, sb_now); lua_setfield(L, -2, "now");
    lua_pushcfunction(L, sb_game); lua_setfield(L, -2, "game");
    lua_pushcfunction(L, sb_after); lua_setfield(L, -2, "after");
    lua_pushcfunction(L, sb_every); lua_setfield(L, -2, "every");
    lua_pushcfunction(L, sb_run); lua_setfield(L, -2, "run");
    lua_pushcfunction(L, sb_wait); lua_setfield(L, -2, "wait");
    lua_pop(L, 1);

    /* string: no dump; the pattern functions bounded; the string metatable locked */
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L); lua_setfield(L, -2, "dump");
    wrap(L, "find", sb_pattern, 2, 0, "find");
    wrap(L, "match", sb_pattern, 2, 0, "match");
    wrap(L, "gmatch", sb_pattern, 2, 0, "gmatch");
    wrap(L, "gsub", sb_pattern, 2, 0, "gsub");
    lua_pop(L, 1);
    lua_pushliteral(L, "");
    if (lua_getmetatable(L, -1)) { lua_pushboolean(L, 0); lua_setfield(L, -2, "__metatable"); lua_pop(L, 1); }
    lua_pop(L, 1);

    lua_getglobal(L, LUA_TABLIBNAME);
    wrap(L, "sort", sb_charged, 1, CH_SORT, NULL);
    wrap(L, "move", sb_charged, 1, CH_MOVE, NULL);
    wrap(L, "insert", sb_charged, 1, CH_SHIFT, NULL);
    wrap(L, "remove", sb_charged, 1, CH_SHIFT, NULL);
    wrap(L, "concat", sb_charged, 1, CH_CONCAT, NULL);
    lua_pop(L, 1);

    lua_getglobal(L, LUA_UTF8LIBNAME);
    wrap(L, "len", sb_charged, 1, CH_SCAN, NULL);
    wrap(L, "offset", sb_charged, 1, CH_SCAN, NULL);
    lua_pop(L, 1);

    lua_getglobal(L, LUA_COLIBNAME);
    wrap(L, "resume", guard, 0, 0, NULL);
    wrap(L, "close", guard, 0, 0, NULL);
    lua_pop(L, 1);

    /* math.random from the map seed; no reseeding from the clock */
    lua_getglobal(L, LUA_MATHLIBNAME);
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, (lua_Integer)(seed & 0x7fffffffffffffffull));
    lua_pushinteger(L, 0);
    lua_call(L, 2, 0);
    lua_pushnil(L);
    lua_setfield(L, -2, "randomseed");
    lua_pop(L, 1);

    /* the runtime's own objects and tables */
    luaL_newmetatable(L, SHS_MT_TIMER);
    lua_newtable(L);
    lua_pushcfunction(L, timer_cancel);
    lua_setfield(L, -2, "cancel");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, timer_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0); lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    luaL_newmetatable(L, SHS_MT_GAME);
    lua_pushcfunction(L, game_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0); lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_TIMERS);
    lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_LOADED);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_RUNS);
}
