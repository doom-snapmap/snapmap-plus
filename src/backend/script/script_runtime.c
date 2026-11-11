/* script_runtime.c -- see script_runtime.h.
 *
 * Every Lua call the runtime makes runs inside lua_pcall: a delivery pushes
 * a C job (one event, one entity's think, one removal, the due timers, a
 * file, a handler list) and a light userdata, and pcalls it with the
 * message handler. The job calls the script's handlers with lua_call and
 * resets the per-call bounds before each one. So an allocation that fails
 * while the runtime builds a handler's arguments is a protected error like
 * any other, and Lua's panic (an abort) is never reached.
 *
 * A stopped or ended mod's state is closed at once, at the top of the tick,
 * map start or map end, never from inside a call into Lua. */
#include "script_internal.h"
#include "script_reason.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef SH_SCRIPT_PURE
#include "../backend_log.h"
#endif

/* ------------------------------------------------------------------ state */

static shs_mod   g_mods[SH_SCRIPT_MAX_MODS];
static SRWLOCK   g_lock = SRWLOCK_INIT;      /* status, queues, bindings, owners, logs */
static SRWLOCK   g_run = SRWLOCK_INIT;       /* one caller runs Lua at a time */
static volatile DWORD g_run_tid;
static int       g_end_pending;
static int       g_active;
static double    g_now, g_dt;
static const sh_script_source *g_source;

static sh_script_entity_ops g_ent_ops;
static int       g_have_ent_ops;
static sh_script_line_fn g_console;
static void     *g_console_ctx;

static const sh_script_limits k_limits = {
    SH_SCRIPT_MEM_CAP, SH_SCRIPT_CALL_STEPS, SH_SCRIPT_TICK_STEPS, SH_SCRIPT_PATTERN_STEPS, SH_SCRIPT_WALL_MS
};
static sh_script_limits g_limits = {
    SH_SCRIPT_MEM_CAP, SH_SCRIPT_CALL_STEPS, SH_SCRIPT_TICK_STEPS, SH_SCRIPT_PATTERN_STEPS, SH_SCRIPT_WALL_MS
};

#define SHS_API_MAX 256
static struct { int kind; char name[48]; lua_CFunction fn; } g_api[SHS_API_MAX];
static int g_napi;

/* Entities a running mod is bound to: handle -> slot, open addressing. */
typedef struct { uint64_t key; int slot; } own_e;     /* slot -1 empty, -2 removed */
static own_e  *g_own;
static size_t  g_own_cap, g_own_used;

static long long g_qpf;

const sh_script_limits *shs_limits(void) { return &g_limits; }
double shs_now(void) { return g_now; }

void sh_script_limits_set(const sh_script_limits *limits) { if (limits) g_limits = *limits; }
void sh_script_limits_reset(void) { g_limits = k_limits; }

static long long qpc(void)
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

static void lower(char *s)
{
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

static void copyz(char *out, size_t cap, const char *s)
{
    size_t n;
    if (!cap) return;
    if (!s) s = "";
    n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

shs_mod *shs_mod_of(lua_State *L)
{
    return *(shs_mod **)lua_getextraspace(L);
}

const char *sh_script_salt(lua_State *L) { return shs_mod_of(L)->salt; }
const char *sh_script_mod_name(lua_State *L) { return shs_mod_of(L)->name; }

/* -------------------------------------------------------------------- log */

void shs_log(shs_mod *m, const char *line)
{
    char console[SH_SCRIPT_LINE_CAP + SH_SCRIPT_MODNAME_CAP + 16];
    int is_stop = strncmp(line, "Script stopped:", 15) == 0;
    int keep = 1;
    AcquireSRWLockExclusive(&g_lock);
    if (!m->log) m->log = (char (*)[SH_SCRIPT_LINE_CAP])calloc(SH_SCRIPT_LOG_LINES + 2, SH_SCRIPT_LINE_CAP);
    if (!m->log) keep = 0;
    else if (m->nlog < SH_SCRIPT_LOG_LINES) copyz(m->log[m->nlog++], SH_SCRIPT_LINE_CAP, line);
    else if (!m->log_full) {
        m->log_full = 1;
        _snprintf_s(m->log[m->nlog++], SH_SCRIPT_LINE_CAP, _TRUNCATE,
                  "More than %d lines were written in this map; the rest are not kept.", SH_SCRIPT_LOG_LINES);
        if (!is_stop) keep = 0;
        else copyz(m->log[m->nlog++], SH_SCRIPT_LINE_CAP, line);
    } else if (is_stop && m->nlog < SH_SCRIPT_LOG_LINES + 2) copyz(m->log[m->nlog++], SH_SCRIPT_LINE_CAP, line);
    else keep = 0;
    if (is_stop) copyz(m->stop_line, sizeof m->stop_line, line);
    ReleaseSRWLockExclusive(&g_lock);
    if (!keep) return;
    if (is_stop) copyz(console, sizeof console, line);
    else _snprintf_s(console, sizeof console, _TRUNCATE, "Script (%s): %s", m->name, line), console[sizeof console - 1] = 0;
#ifndef SH_SCRIPT_PURE
    backend_log(console);
#endif
    if (g_console) g_console(g_console_ctx, console);
}

void sh_script_set_console(sh_script_line_fn fn, void *context)
{
    g_console = fn;
    g_console_ctx = context;
}

void sh_script_set_entity_ops(const sh_script_entity_ops *ops)
{
    if (ops) { g_ent_ops = *ops; g_have_ent_ops = 1; }
    else { memset(&g_ent_ops, 0, sizeof g_ent_ops); g_have_ent_ops = 0; }
}

/* ----------------------------------------------------------- allocation */

/* Counts every byte; refuses past the cap (a stopping state may borrow the
 * reserve to write its error); while a delivery runs, charges allocated
 * bytes as steps so work that only allocates (a long concatenation in a
 * loop) is bounded too. Lua never asks a shrink to fail. */
static void *shs_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    shs_mod *m = (shs_mod *)ud;
    size_t old = ptr ? osize : 0;
    void *p;
    if (nsize == 0) {
        if (ptr) { m->mem -= old; free(ptr); }
        return NULL;
    }
    if (nsize > old) {
        size_t grow = nsize - old;
        size_t cap = g_limits.mem_cap + (m->stop >= 0 ? SH_SCRIPT_MEM_RESERVE : 0);
        if (m->mem + grow > cap) return NULL;
        if (m->charging && m->stop < 0) {
            long long steps = (long long)(grow / SH_SCRIPT_ALLOC_STEP);
            m->call_steps += steps;
            m->tick_steps += steps;
            if (m->call_steps > g_limits.call_steps) { m->stop = SH_SCRIPT_STOP_STEPS; return NULL; }
            if (m->tick_steps > g_limits.tick_steps) { m->stop = SH_SCRIPT_STOP_TICK; return NULL; }
        }
    }
    p = realloc(ptr, nsize);
    if (!p) return (nsize <= old) ? ptr : NULL;
    m->mem = m->mem - old + nsize;
    return p;
}

/* ------------------------------------------------------------- bounds */

int shs_raise_stop(lua_State *L, int level)
{
    luaL_where(L, level);
    lua_pushstring(L, "stopped");
    lua_concat(L, 2);
    return lua_error(L);
}

static int over_bound(shs_mod *m)
{
    if (m->stop >= 0) return 1;
    if (m->call_steps > g_limits.call_steps) m->stop = SH_SCRIPT_STOP_STEPS;
    else if (m->tick_steps > g_limits.tick_steps) m->stop = SH_SCRIPT_STOP_TICK;
    else if (g_limits.wall_ms && (qpc() - m->wall_start) * 1000 / g_qpf > (long long)g_limits.wall_ms)
        m->stop = SH_SCRIPT_STOP_WALL;
    return m->stop >= 0;
}

void shs_charge(lua_State *L, long long steps)
{
    shs_mod *m = shs_mod_of(L);
    if (m->stop < 0) {
        m->call_steps += steps;
        m->tick_steps += steps;
    }
    if (over_bound(m)) shs_raise_stop(L, 1);
}

void shs_match_step(lua_State *L)
{
    shs_mod *m = shs_mod_of(L);
    if (++m->pattern_steps > g_limits.pattern_steps && m->stop < 0) m->stop = SH_SCRIPT_STOP_PATTERN;
    if ((m->pattern_steps & 1023) == 0 && m->stop < 0) {
        m->call_steps += 64;
        m->tick_steps += 64;
        over_bound(m);
    }
    if (m->stop >= 0) shs_raise_stop(L, 1);
}

/* The count hook: every SH_SCRIPT_HOOK_STRIDE instructions, in every
 * coroutine of the state. In a hook, level 0 is the running Lua function. */
static void shs_hook(lua_State *L, lua_Debug *ar)
{
    shs_mod *m = shs_mod_of(L);
    if (m->stop < 0) {
        m->call_steps += SH_SCRIPT_HOOK_STRIDE;
        m->tick_steps += SH_SCRIPT_HOOK_STRIDE;
        if (lua_getinfo(L, "Sl", ar)) {
            copyz(m->last_src, sizeof m->last_src, ar->short_src);
            m->last_line = ar->currentline;
        }
    }
    if (over_bound(m)) shs_raise_stop(L, 0);
}

void shs_call_reset(shs_mod *m)
{
    m->call_steps = 0;
    m->pattern_steps = 0;
    m->wall_start = qpc();
    m->last_src[0] = 0;
    m->last_line = 0;
}

static void call_begin(shs_mod *m)
{
    shs_call_reset(m);
    m->charging = 1;
}

/* ---------------------------------------------------------- delivery */

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* Where the error was raised: the message's own "scripts\x.lua:N:" prefix,
 * else the innermost script line on the stack. */
void shs_where_from(shs_mod *m, lua_State *L, int first)
{
    lua_Debug ar;
    int level;
    if (m->err_file[0]) return;
    for (level = first; lua_getstack(L, level, &ar); level++) {
        if (!lua_getinfo(L, "Sl", &ar)) break;
        if (ar.currentline > 0 && starts(ar.short_src, "scripts\\")) {
            copyz(m->err_file, sizeof m->err_file, ar.short_src);
            m->err_line = ar.currentline;
            return;
        }
    }
}

static int shs_msgh(lua_State *L)
{
    shs_mod *m = shs_mod_of(L);
    const char *msg = lua_isstring(L, 1) ? lua_tostring(L, 1) : NULL;
    if (!m->err_file[0]) {
        if (msg) sh_script_split_where(msg, m->err_file, sizeof m->err_file, &m->err_line);
        if (!m->err_file[0]) shs_where_from(m, L, 1);
    }
    lua_settop(L, 1);
    return 1;
}

static void close_mod(shs_mod *m, int status);

static void stop_line(shs_mod *m, const char *file, int line, const char *what, const char *todo)
{
    char text[SH_SCRIPT_LINE_CAP];
    char disp[128];
    size_t i;
    copyz(disp, sizeof disp, file);
    for (i = 0; disp[i]; i++) if (disp[i] == '/') disp[i] = '\\';
    sh_script_stop_text(m->name, disp, line, what, todo, text, sizeof text);
    shs_log(m, text);
}

/* Word and log the stop of a failed call; the error object is on L's top. */
static void stop_from_error(shs_mod *m, lua_State *L, int status)
{
    char what[320], todo[200], file[128];
    int line = 0;
    const char *msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    file[0] = 0;
    if (status == LUA_ERRMEM && m->stop < 0) m->stop = SH_SCRIPT_STOP_MEMORY;
    if (m->stop >= 0) {
        unsigned long long limit =
            m->stop == SH_SCRIPT_STOP_STEPS   ? (unsigned long long)g_limits.call_steps :
            m->stop == SH_SCRIPT_STOP_TICK    ? (unsigned long long)g_limits.tick_steps :
            m->stop == SH_SCRIPT_STOP_MEMORY  ? (unsigned long long)g_limits.mem_cap :
            m->stop == SH_SCRIPT_STOP_PATTERN ? (unsigned long long)g_limits.pattern_steps :
                                                (unsigned long long)g_limits.wall_ms;
        sh_script_reason_stop(m->stop, limit, what, sizeof what, todo, sizeof todo);
        if (m->err_file[0]) { copyz(file, sizeof file, m->err_file); line = m->err_line; }
        else if (msg) sh_script_split_where(msg, file, sizeof file, &line);
        if (!file[0] && m->last_src[0]) { copyz(file, sizeof file, m->last_src); line = m->last_line; }
    } else if (msg) {
        const char *text = sh_script_split_where(msg, file, sizeof file, &line);
        if (m->err_file[0]) { copyz(file, sizeof file, m->err_file); line = m->err_line; }
        sh_script_reason(text, what, sizeof what, todo, sizeof todo);
    } else {
        copyz(file, sizeof file, m->err_file);
        line = m->err_line;
        copyz(what, sizeof what, "the script raised an error whose value is not text");
        copyz(todo, sizeof todo, "Give error(...) a text that says what went wrong");
    }
    stop_line(m, file, line, what, todo);
}

/* Run the C job `job` with `ud` as one protected call. 1 when it returned,
 * 0 when the mod stopped (its state is closed). */
static int run_job(shs_mod *m, lua_CFunction job, void *ud)
{
    lua_State *L = m->L;
    int base, status;
    if (!L || m->status != SH_SCRIPT_RUNNING) return 0;
    base = lua_gettop(L);
    lua_pushcfunction(L, shs_msgh);
    lua_pushcfunction(L, job);
    lua_pushlightuserdata(L, ud);
    m->err_file[0] = 0;
    m->err_line = 0;
    call_begin(m);
    status = lua_pcall(L, 1, 0, base + 1);
    m->charging = 0;
    if (status == LUA_OK) {
        lua_settop(L, base);
        return 1;
    }
    stop_from_error(m, L, status);
    close_mod(m, SH_SCRIPT_STOPPED);
    return 0;
}

/* Call the function below nargs arguments, as one handler of a job. */
static void call_handler(lua_State *L, int nargs)
{
    shs_call_reset(shs_mod_of(L));
    lua_call(L, nargs, 0);
}

/* Push H[kind][key] (a list or nil). */
static int push_handlers(lua_State *L, const char *kind, const char *key)
{
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    lua_getfield(L, -1, kind);
    lua_remove(L, -2);
    if (!key) return lua_type(L, -1) == LUA_TTABLE;
    if (lua_type(L, -1) != LUA_TTABLE) return 0;
    lua_getfield(L, -1, key);
    lua_remove(L, -2);
    return lua_type(L, -1) == LUA_TTABLE;
}

/* Call each function of the list at `list` with the `nargs` values at
 * args..args+nargs-1. */
static void call_list(lua_State *L, int list, int args, int nargs)
{
    lua_Integer i;
    for (i = 1;; i++) {
        int k;
        if (lua_rawgeti(L, list, i) != LUA_TFUNCTION) { lua_pop(L, 1); break; }
        for (k = 0; k < nargs; k++) lua_pushvalue(L, args + k);
        call_handler(L, nargs);
    }
}

/* ------------------------------------------------------------ entities */

typedef struct shs_entity_ud { uint64_t handle; int dead; } shs_entity_ud;

void sh_script_push_entity(lua_State *L, uint64_t handle)
{
    shs_entity_ud *e;
    if (!handle) { lua_pushnil(L); return; }
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_ENTS);
    if (lua_rawgeti(L, -1, (lua_Integer)handle) == LUA_TUSERDATA) { lua_remove(L, -2); return; }
    lua_pop(L, 1);
    e = (shs_entity_ud *)lua_newuserdatauv(L, sizeof *e, 1);
    e->handle = handle;
    e->dead = 0;
    lua_newtable(L);
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, SHS_MT_ENTITY);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, (lua_Integer)handle);
    lua_remove(L, -2);
}

uint64_t sh_script_check_entity(lua_State *L, int arg, const char *call)
{
    shs_entity_ud *e = (shs_entity_ud *)luaL_testudata(L, arg, SHS_MT_ENTITY);
    if (!e) {
        sh_script_refuse(L, "\"%s\" needs an entity here, got %s", call ? call : "this call",
                         lua_isnoneornil(L, arg) ? "nothing" : luaL_typename(L, arg));
    }
    return e->dead ? 0 : e->handle;
}

static void mark_dead(lua_State *L, uint64_t handle)
{
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_ENTS);
    if (lua_rawgeti(L, -1, (lua_Integer)handle) == LUA_TUSERDATA) {
        shs_entity_ud *e = (shs_entity_ud *)lua_touserdata(L, -1);
        e->dead = 1;
    }
    lua_pop(L, 2);
}

static int ent_index(lua_State *L)
{
    const char *key = lua_tostring(L, 2);
    if (key && strcmp(key, "state") == 0) {
        lua_getiuservalue(L, 1, 1);
        return 1;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_METHODS);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

static int ent_newindex(lua_State *L)
{
    const char *key = lua_tostring(L, 2);
    return sh_script_refuse(L, "an entity's \"%s\" cannot be set; keep your own values in its state table, as e.state.%s",
                            key ? key : "?", key ? key : "value");
}

static int ent_tostring(lua_State *L)
{
    shs_entity_ud *e = (shs_entity_ud *)lua_touserdata(L, 1);
    lua_pushstring(L, e->dead ? "entity (removed)" : "entity");
    return 1;
}

static unsigned long long mix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/* Owners: call with g_lock held exclusive (put, drop) or shared (find). */
static int own_find(uint64_t key)
{
    size_t i;
    if (!g_own_cap || !key) return -1;
    for (i = (size_t)(mix64(key) & (g_own_cap - 1));; i = (i + 1) & (g_own_cap - 1)) {
        if (g_own[i].slot == -1) return -1;
        if (g_own[i].slot >= 0 && g_own[i].key == key) return g_own[i].slot;
    }
}

static int own_put(uint64_t key, int slot)
{
    size_t i;
    if (!key) return 0;
    if ((g_own_used + 1) * 2 > g_own_cap) {
        size_t cap = g_own_cap ? g_own_cap * 2 : 256, j;
        own_e *t = (own_e *)malloc(cap * sizeof *t), *old = g_own;
        size_t old_cap = g_own_cap;
        if (!t) return 0;
        for (j = 0; j < cap; j++) t[j].slot = -1;
        g_own = t;
        g_own_cap = cap;
        g_own_used = 0;
        for (j = 0; j < old_cap; j++) if (old[j].slot >= 0) {
            size_t k = (size_t)(mix64(old[j].key) & (cap - 1));
            while (g_own[k].slot != -1) k = (k + 1) & (cap - 1);
            g_own[k] = old[j];
            g_own_used++;
        }
        free(old);
    }
    for (i = (size_t)(mix64(key) & (g_own_cap - 1));; i = (i + 1) & (g_own_cap - 1)) {
        if (g_own[i].slot >= 0 && g_own[i].key == key) { g_own[i].slot = slot; return 1; }
        if (g_own[i].slot == -1) break;
    }
    for (i = (size_t)(mix64(key) & (g_own_cap - 1));; i = (i + 1) & (g_own_cap - 1)) {
        if (g_own[i].slot < 0) {
            if (g_own[i].slot == -1) g_own_used++;
            g_own[i].key = key;
            g_own[i].slot = slot;
            return 1;
        }
    }
}

static void own_drop(uint64_t key)
{
    size_t i;
    if (!g_own_cap || !key) return;
    for (i = (size_t)(mix64(key) & (g_own_cap - 1));; i = (i + 1) & (g_own_cap - 1)) {
        if (g_own[i].slot == -1) return;
        if (g_own[i].slot >= 0 && g_own[i].key == key) { g_own[i].slot = -2; return; }
    }
}

static void own_drop_slot(int slot)
{
    size_t i;
    for (i = 0; i < g_own_cap; i++) if (g_own[i].slot == slot) g_own[i].slot = -2;
}

int sh_script_entity_owner(uint64_t handle, char *salt_out, size_t cap)
{
    int slot, ok = 0;
    AcquireSRWLockShared(&g_lock);
    slot = own_find(handle);
    if (slot >= 0 && g_mods[slot].status == SH_SCRIPT_RUNNING) {
        copyz(salt_out, cap, g_mods[slot].salt);
        ok = 1;
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void sh_script_adopt_entity(lua_State *L, uint64_t handle)
{
    shs_mod *m = shs_mod_of(L);
    AcquireSRWLockExclusive(&g_lock);
    own_put(handle, (int)(m - g_mods));
    ReleaseSRWLockExclusive(&g_lock);
}

int sh_script_owns_entity(lua_State *L, uint64_t handle)
{
    shs_mod *m = shs_mod_of(L);
    int slot;
    AcquireSRWLockShared(&g_lock);
    slot = own_find(handle);
    ReleaseSRWLockShared(&g_lock);
    return slot >= 0 && &g_mods[slot] == m;
}

/* -------------------------------------------------------------- naming */

int sh_script_refuse(lua_State *L, const char *fmt, ...)
{
    char text[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, ap);
    va_end(ap);
    text[sizeof text - 1] = 0;
    luaL_where(L, 1);
    lua_pushstring(L, text);
    lua_concat(L, 2);
    return lua_error(L);
}

void sh_script_check_name(lua_State *L, int arg, const char *call, int allow_game,
                          char *out, size_t cap, int *is_game)
{
    shs_mod *m = shs_mod_of(L);
    const char *name;
    char buf[SH_SCRIPT_DEFNAME_CAP];
    size_t n, i, prefix;
    if (is_game) *is_game = 0;
    if (luaL_testudata(L, arg, SHS_MT_GAME)) {
        lua_getiuservalue(L, arg, 1);
        name = lua_tostring(L, -1);
        if (!allow_game)
            sh_script_refuse(L, "\"%s\" can only use this mod's own content, not the game's \"%s\"", call, name ? name : "");
        copyz(out, cap, name);
        lua_pop(L, 1);
        if (is_game) *is_game = 1;
        return;
    }
    if (lua_type(L, arg) != LUA_TSTRING)
        sh_script_refuse(L, "\"%s\" needs a name here, got %s", call,
                         lua_isnoneornil(L, arg) ? "nothing" : luaL_typename(L, arg));
    name = lua_tolstring(L, arg, &n);
    if (n == 0 || n >= sizeof buf)
        sh_script_refuse(L, "\"%s\" was given a name that is %s", call, n ? "too long" : "empty");
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/'))
            sh_script_refuse(L, "\"%s\" was given \"%s\", which is not a usable name (use letters, digits, _ - . and /)", call, name);
        buf[i] = c;
    }
    buf[n] = 0;
    if (buf[0] == '/' || buf[n - 1] == '/' || strstr(buf, "//") || strstr(buf, ".."))
        sh_script_refuse(L, "\"%s\" was given \"%s\", which is not a usable name", call, name);
    if (strncmp(buf, "mods/", 5) == 0) {
        size_t sl = strlen(m->salt);
        if (strncmp(buf + 5, m->salt, sl) != 0 || buf[5 + sl] != '/' || !buf[6 + sl])
            sh_script_refuse(L, "\"%s\" can only use this mod's own content; \"%s\" belongs to another mod", call, name);
        copyz(out, cap, buf);
        return;
    }
    prefix = 6 + strlen(m->salt);
    if (prefix + n >= cap)
        sh_script_refuse(L, "\"%s\" was given a name that is too long", call);
    _snprintf_s(out, cap, _TRUNCATE, "mods/%s/%s", m->salt, buf);
    out[cap - 1] = 0;
}

/* --------------------------------------------------------- registration */

static const char *const k_kinds[] = { "spawn", "think", "remove", "damage", "death", "anim" };
static const unsigned k_bits[] = { SH_SCRIPT_BIND_SPAWN, SH_SCRIPT_BIND_THINK, SH_SCRIPT_BIND_REMOVE,
                                   SH_SCRIPT_BIND_DAMAGE, SH_SCRIPT_BIND_DEATH, SH_SCRIPT_BIND_ANIM };

static int add_bind(shs_mod *m, const char *full, unsigned bit)
{
    int i, ok = 1;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < m->nbinds; i++) if (strcmp(m->binds[i].name, full) == 0) break;
    if (i == m->nbinds) {
        if (m->nbinds == m->cbinds) {
            int c = m->cbinds ? m->cbinds * 2 : 16;
            shs_bind *b = (shs_bind *)realloc(m->binds, (size_t)c * sizeof *b);
            if (!b) ok = 0;
            else { m->binds = b; m->cbinds = c; }
        }
        if (ok) { copyz(m->binds[i].name, sizeof m->binds[i].name, full); m->binds[i].kinds = 0; m->nbinds++; }
    }
    if (ok) m->binds[i].kinds |= bit;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

/* Append the function at `fn` to the list t[key] of the table at `t`. */
static void list_append(lua_State *L, int t, const char *key, int fn)
{
    if (lua_getfield(L, t, key) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, t, key);
    }
    lua_pushvalue(L, fn);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    lua_pop(L, 1);
}

/* on.spawn / think / remove / damage / death (name, fn); on.anim(name, notify, fn). */
static int on_entity(lua_State *L)
{
    shs_mod *m = shs_mod_of(L);
    int kind = (int)lua_tointeger(L, lua_upvalueindex(1));
    char call[24], full[SH_SCRIPT_DEFNAME_CAP], notify[SH_SCRIPT_TEXT_CAP];
    int fn = kind == 5 ? 3 : 2;
    _snprintf_s(call, sizeof call, _TRUNCATE, "on.%s", k_kinds[kind]);
    call[sizeof call - 1] = 0;
    sh_script_check_name(L, 1, call, 0, full, sizeof full, NULL);
    if (kind == 5) {
        size_t n;
        const char *s = lua_type(L, 2) == LUA_TSTRING ? lua_tolstring(L, 2, &n) : NULL;
        if (!s || !n || n >= sizeof notify)
            sh_script_refuse(L, "\"on.anim\" needs the notify text of an ae_notify row as its second value");
        copyz(notify, sizeof notify, s);
        lower(notify);
    }
    if (lua_type(L, fn) != LUA_TFUNCTION)
        sh_script_refuse(L, "\"%s\" needs a function as its %s value, got %s", call, fn == 3 ? "third" : "second",
                         lua_isnoneornil(L, fn) ? "nothing" : luaL_typename(L, fn));
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    lua_getfield(L, -1, k_kinds[kind]);
    if (kind == 5) {
        if (lua_getfield(L, -1, full) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, full);
        }
        list_append(L, lua_gettop(L), notify, fn);
    } else list_append(L, lua_gettop(L), full, fn);
    if (!add_bind(m, full, k_bits[kind])) luaL_error(L, "not enough memory");
    return 0;
}

static int on_input(lua_State *L)
{
    char name[SH_SCRIPT_NAMES_CAP];
    size_t n;
    const char *s = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : NULL;
    if (!s || !n || n >= 128)
        sh_script_refuse(L, "\"on.input\" needs the name of a Script node as its first value");
    if (lua_type(L, 2) != LUA_TFUNCTION)
        sh_script_refuse(L, "\"on.input\" needs a function as its second value, got %s",
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    copyz(name, sizeof name, s);
    lower(name);
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    lua_getfield(L, -1, "input");
    list_append(L, lua_gettop(L), name, 2);
    return 0;
}

static int on_map(lua_State *L)
{
    const char *kind = lua_tostring(L, lua_upvalueindex(1));
    if (lua_type(L, 1) != LUA_TFUNCTION)
        sh_script_refuse(L, "\"on.%s\" needs a function, got %s", kind,
                         lua_isnoneornil(L, 1) ? "nothing" : luaL_typename(L, 1));
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    list_append(L, lua_gettop(L), kind, 1);
    return 0;
}

static int on_newindex(lua_State *L)
{
    return sh_script_refuse(L, "\"on\" lists what a script handles and cannot be changed; call on.spawn(name, function) and the others");
}

int sh_script_api_add(int kind, const char *name, lua_CFunction fn)
{
    size_t n = name ? strlen(name) : 0;
    if ((kind != SH_SCRIPT_API_GLOBAL && kind != SH_SCRIPT_API_METHOD) || !fn || !n || n >= sizeof g_api[0].name) return 0;
    if (g_napi >= SHS_API_MAX) return 0;
    g_api[g_napi].kind = kind;
    copyz(g_api[g_napi].name, sizeof g_api[0].name, name);
    g_api[g_napi].fn = fn;
    g_napi++;
    return 1;
}

/* ------------------------------------------------------------- opening */

static int job_open(lua_State *L)
{
    uint64_t seed = *(uint64_t *)lua_touserdata(L, 1);
    int i;
    shs_open_libs(L, seed);
    /* handler tables */
    lua_newtable(L);
    for (i = 0; i < 6; i++) { lua_newtable(L); lua_setfield(L, -2, k_kinds[i]); }
    lua_newtable(L); lua_setfield(L, -2, "input");
    lua_newtable(L); lua_setfield(L, -2, "map_start");
    lua_newtable(L); lua_setfield(L, -2, "map_end");
    lua_newtable(L); lua_setfield(L, -2, "missed");
    lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    /* entity objects: one per handle for the map, so e.state lasts (a
     * map holds a bounded number of entities; all go at map end) */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_ENTS);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_LIVE);
    luaL_newmetatable(L, SHS_MT_ENTITY);
    lua_pushcfunction(L, ent_index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ent_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, ent_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0); lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    /* functions other modules added with sh_script_api_add */
    lua_newtable(L);
    for (i = 0; i < g_napi; i++) if (g_api[i].kind == SH_SCRIPT_API_METHOD) {
        lua_pushcfunction(L, g_api[i].fn);
        lua_setfield(L, -2, g_api[i].name);
    }
    lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_METHODS);
    for (i = 0; i < g_napi; i++) if (g_api[i].kind == SH_SCRIPT_API_GLOBAL) {
        lua_pushcfunction(L, g_api[i].fn);
        lua_setglobal(L, g_api[i].name);
    }
    /* on: a read-only table of the registration calls */
    lua_newtable(L);
    for (i = 0; i < 6; i++) {
        lua_pushinteger(L, i);
        lua_pushcclosure(L, on_entity, 1);
        lua_setfield(L, -2, k_kinds[i]);
    }
    lua_pushcfunction(L, on_input); lua_setfield(L, -2, "input");
    lua_pushstring(L, "map_start"); lua_pushcclosure(L, on_map, 1); lua_setfield(L, -2, "map_start");
    lua_pushstring(L, "map_end"); lua_pushcclosure(L, on_map, 1); lua_setfield(L, -2, "map_end");
    lua_newtable(L);                                 /* a proxy, so "on" itself cannot be edited */
    lua_newtable(L);
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, on_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "on");
    lua_pop(L, 1);
    return 0;
}

static void open_failed(shs_mod *m, const char *what)
{
    stop_line(m, "", 0, what, "Restart the map; if it happens again, restart the game");
}

/* -------------------------------------------------------------- loading */

typedef struct { char (*names)[128]; int n, cap, total; } shs_list;

static void list_cb(void *ctx, const char *relpath)
{
    shs_list *l = (shs_list *)ctx;
    size_t n = relpath ? strlen(relpath) : 0;
    l->total++;
    if (n < 5 || n >= 128 || strchr(relpath, '/') || strchr(relpath, '\\')) return;
    if (_stricmp(relpath + n - 4, ".lua") != 0) return;
    if (l->n == l->cap) {
        int c = l->cap ? l->cap * 2 : 16;
        char (*p)[128] = (char (*)[128])realloc(l->names, (size_t)c * 128);
        if (!p) return;
        l->names = p;
        l->cap = c;
    }
    copyz(l->names[l->n++], 128, relpath);
}

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* The first statement must be "api = <integer>" (comments, blank lines and
 * a UTF-8 byte order mark may come before it). 1 with the version and its
 * line; 0 when the file does not start with it. */
static int api_declared(const char *t, size_t n, long *version, int *line)
{
    size_t i = 0;
    int ln = 1;
    if (n >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) i = 3;
    for (;;) {
        while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n' || t[i] == ';')) { if (t[i] == '\n') ln++; i++; }
        if (i + 1 < n && t[i] == '-' && t[i + 1] == '-') {
            size_t eq = 0, j = i + 2;
            if (j < n && t[j] == '[') {
                size_t k = j + 1;
                while (k < n && t[k] == '=') { eq++; k++; }
                if (k < n && t[k] == '[') {
                    /* long comment: find ]=*] with the same level */
                    for (k = k + 1; k < n; k++) {
                        if (t[k] == '\n') ln++;
                        if (t[k] == ']') {
                            size_t e = 0, q = k + 1;
                            while (q < n && t[q] == '=') { e++; q++; }
                            if (e == eq && q < n && t[q] == ']') { i = q + 1; break; }
                        }
                    }
                    if (k >= n) return 0;
                    continue;
                }
            }
            while (i < n && t[i] != '\n') i++;
            continue;
        }
        break;
    }
    *line = ln;
    if (i + 3 > n || strncmp(t + i, "api", 3) != 0) return 0;
    i += 3;
    if (i < n && ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9') || t[i] == '_')) return 0;
    while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
    if (i >= n || t[i] != '=' || (i + 1 < n && t[i + 1] == '=')) return 0;
    i++;
    while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
    if (i >= n || t[i] < '0' || t[i] > '9') return 0;
    {
        long v = 0;
        while (i < n && t[i] >= '0' && t[i] <= '9') { if (v < 1000000) v = v * 10 + (t[i] - '0'); i++; }
        if (i < n && (t[i] == '.' || t[i] == 'x' || t[i] == 'X' || t[i] == 'e' || t[i] == 'E' ||
                      (t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || t[i] == '_')) return 0;
        *version = v;
    }
    return 1;
}

typedef struct { const char *relpath; const char *text; size_t len; } shs_file_job;

int shs_load_file(lua_State *L, shs_mod *m, const char *relpath)
{
    char chunk[160], *text = NULL;
    size_t len = 0, i;
    int r, status;
    _snprintf_s(chunk, sizeof chunk, _TRUNCATE, "=scripts\\%s", relpath);
    chunk[sizeof chunk - 1] = 0;
    for (i = 0; chunk[i]; i++) if (chunk[i] == '/') chunk[i] = '\\';
    if (!m->source || !m->source->read) { lua_pushfstring(L, "%s: the file could not be read", chunk + 1); return LUA_ERRFILE; }
    r = m->source->read(m->source->context, m->salt, relpath, &text, &len);
    if (r <= 0) {
        lua_pushfstring(L, r == 0 ? "%s: the file is not in this mod" : "%s: the file could not be read", chunk + 1);
        return LUA_ERRFILE;
    }
    if (len > SH_SCRIPT_FILE_MAX) status = -1;
    else if (m->text_total + len > SH_SCRIPT_MOD_TEXT_MAX) status = -2;
    else if (++m->files_read > SH_SCRIPT_MOD_FILES_MAX) status = -3;
    else {
        const char *t = text;                 /* a UTF-8 byte order mark is not Lua */
        size_t n = len;
        m->text_total += len;
        if (n >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) { t += 3; n -= 3; }
        status = luaL_loadbufferx(L, t, n, chunk, "t");
    }
    if (m->source->release) m->source->release(m->source->context, text);
    if (status < 0) {
        lua_pushfstring(L, status == -1 ? "%s:1: the file is larger than 256 KB, so it did not run" :
                           status == -2 ? "%s:1: this mod's scripts are larger than 1 MB in all, so they did not run" :
                                          "%s:1: this mod has more than 64 script files, so they did not run", chunk + 1);
        return LUA_ERRFILE;
    }
    return status;
}

/* Job: read, check and run one entry file. */
static int job_file(lua_State *L)
{
    shs_file_job *f = (shs_file_job *)lua_touserdata(L, 1);
    shs_mod *m = shs_mod_of(L);
    int status = shs_load_file(L, m, f->relpath);
    if (status != LUA_OK) return lua_error(L);
    call_handler(L, 0);
    return 0;
}

/* Job: call every function of H[kind]. */
static int job_list(lua_State *L)
{
    const char *kind = (const char *)lua_touserdata(L, 1);
    if (push_handlers(L, kind, NULL)) call_list(L, lua_gettop(L), 0, 0);
    return 0;
}

/* Read the entry file's text once for the api check (a refusal is a stop
 * with the file and line, before anything runs). */
static int check_api(shs_mod *m, const char *relpath)
{
    char *text = NULL, file[160];
    size_t len = 0, i;
    long version = 0;
    int line = 1, r, ok;
    _snprintf_s(file, sizeof file, _TRUNCATE, "scripts\\%s", relpath);
    file[sizeof file - 1] = 0;
    for (i = 0; file[i]; i++) if (file[i] == '/') file[i] = '\\';
    r = m->source->read(m->source->context, m->salt, relpath, &text, &len);
    if (r <= 0) {
        stop_line(m, file, 0, "the file could not be read, so this mod's scripts did not run",
                  "Press Refresh in the Mods tab and play the map again");
        return 0;
    }
    if (len >= 4 && memcmp(text, "\x1bLua", 4) == 0) {
        if (m->source->release) m->source->release(m->source->context, text);
        stop_line(m, file, 0, "the file is compiled Lua, not script text, so it did not run",
                  "Put the .lua source text in the file");
        return 0;
    }
    ok = api_declared(text, len, &version, &line);
    if (m->source->release) m->source->release(m->source->context, text);
    if (!ok) {
        stop_line(m, file, line, "the file does not start with \"api = 1\", so it did not run",
                  "Add \"api = 1\" as its first line");
        return 0;
    }
    if (version > SH_SCRIPT_API_VERSION) {
        char what[200];
        _snprintf_s(what, sizeof what, _TRUNCATE, "the file asks for scripting version %ld, but this Snapmap+ has version %d, so it did not run",
                  version, SH_SCRIPT_API_VERSION);
        what[sizeof what - 1] = 0;
        stop_line(m, file, line, what, "Update Snapmap+ to run it");
        return 0;
    }
    if (version < 1) {
        char what[160];
        _snprintf_s(what, sizeof what, _TRUNCATE, "the file asks for scripting version %ld, which does not exist, so it did not run", version);
        what[sizeof what - 1] = 0;
        stop_line(m, file, line, what, "Write \"api = 1\"");
        return 0;
    }
    return 1;
}

static void free_mod_tables(shs_mod *m)
{
    int i;
    for (i = 0; i < m->nents; i++) free(m->ents[i].chain);
    free(m->ents);
    m->ents = NULL;
    m->nents = m->cents = 0;
    shs_timers_free(m);
}

/* Close the state and drop what the map held for it; keep the log. */
static void close_mod(shs_mod *m, int status)
{
    int slot = (int)(m - g_mods);
    if (m->L) {
        lua_sethook(m->L, NULL, 0, 0);
        lua_close(m->L);
        m->L = NULL;
    }
    free_mod_tables(m);
    AcquireSRWLockExclusive(&g_lock);
    m->status = status;
    free(m->queue);
    m->queue = NULL;
    m->qhead = m->qcount = 0;
    free(m->binds);
    m->binds = NULL;
    m->nbinds = m->cbinds = 0;
    own_drop_slot(slot);
    ReleaseSRWLockExclusive(&g_lock);
}

static void open_mod(shs_mod *m, uint64_t seed)
{
    shs_list files;
    int i, n;
    memset(&files, 0, sizeof files);
    m->source = g_source;
    m->text_total = 0;
    m->files_read = 0;
    if (!g_source || !g_source->list || !g_source->read) return;
    n = g_source->list(g_source->context, m->salt, list_cb, &files);
    if (n < 0) {
        stop_line(m, "", 0, "this mod's scripts could not be read, so they did not run",
                  "Press Refresh in the Mods tab and play the map again");
        AcquireSRWLockExclusive(&g_lock); m->status = SH_SCRIPT_STOPPED; ReleaseSRWLockExclusive(&g_lock);
        free(files.names);
        return;
    }
    if (!files.n) { free(files.names); return; }
    if (files.total > SH_SCRIPT_MOD_FILES_MAX) {
        stop_line(m, "", 0, "this mod has more than 64 script files, so they did not run", "Combine some of them");
        AcquireSRWLockExclusive(&g_lock); m->status = SH_SCRIPT_STOPPED; ReleaseSRWLockExclusive(&g_lock);
        free(files.names);
        return;
    }
    qsort(files.names, (size_t)files.n, 128, name_cmp);
    m->stop = -1;
    m->charging = 0;
    m->mem = 0;
    m->queue = (sh_script_event *)calloc(SH_SCRIPT_QUEUE_CAP, sizeof *m->queue);
    m->L = m->queue ? lua_newstate(shs_alloc, m) : NULL;
    if (!m->L) {
        free(m->queue);
        m->queue = NULL;
        open_failed(m, "there was not enough memory to start this mod's scripts");
        AcquireSRWLockExclusive(&g_lock); m->status = SH_SCRIPT_STOPPED; ReleaseSRWLockExclusive(&g_lock);
        free(files.names);
        return;
    }
    *(shs_mod **)lua_getextraspace(m->L) = m;
    lua_sethook(m->L, shs_hook, LUA_MASKCOUNT, SH_SCRIPT_HOOK_STRIDE);
    AcquireSRWLockExclusive(&g_lock);
    m->status = SH_SCRIPT_RUNNING;
    ReleaseSRWLockExclusive(&g_lock);
    m->tick_steps = 0;
    if (!run_job(m, job_open, &seed)) { free(files.names); return; }
    for (i = 0; i < files.n; i++) {
        shs_file_job f;
        if (!check_api(m, files.names[i])) { close_mod(m, SH_SCRIPT_STOPPED); free(files.names); return; }
        f.relpath = files.names[i];
        f.text = NULL;
        f.len = 0;
        if (!run_job(m, job_file, &f)) { free(files.names); return; }
    }
    free(files.names);
    run_job(m, job_list, (void *)"map_start");
}

/* ------------------------------------------------------------- the map */

static int enter_run(void)
{
    if (g_run_tid == GetCurrentThreadId()) return 0;
    AcquireSRWLockExclusive(&g_run);
    g_run_tid = GetCurrentThreadId();
    return 1;
}

static void end_all(void);

static void leave_run(void)
{
    if (g_end_pending) { g_end_pending = 0; end_all(); }
    g_run_tid = 0;
    ReleaseSRWLockExclusive(&g_run);
}

static int find_slot(const char *salt)
{
    int i, free_slot = -1;
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++) {
        if (g_mods[i].salt[0] && strcmp(g_mods[i].salt, salt) == 0) return i;
        if (free_slot < 0 && !g_mods[i].salt[0]) free_slot = i;
    }
    if (free_slot >= 0) return free_slot;
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++) if (!g_mods[i].in_map && !g_mods[i].L) return i;
    return -1;
}

int sh_script_map_start(const sh_script_mod_desc *mods, int n, const sh_script_source *source,
                        uint64_t seed, double now)
{
    int i, running = 0;
    if (!g_qpf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = f.QuadPart ? f.QuadPart : 1; }
    if (!enter_run()) return 0;
    end_all();
    g_now = now;
    g_dt = 0;
    g_source = source;
    for (i = 0; i < n && mods; i++) {
        int slot;
        shs_mod *m;
        char salt[SH_SCRIPT_SALT_CAP];
        if (!mods[i].salt || !mods[i].salt[0] || strlen(mods[i].salt) >= SH_SCRIPT_SALT_CAP) continue;
        copyz(salt, sizeof salt, mods[i].salt);
        lower(salt);
        slot = find_slot(salt);
        if (slot < 0) continue;
        m = &g_mods[slot];
        if (m->in_map) continue;                 /* listed twice */
        AcquireSRWLockExclusive(&g_lock);
        free(m->log);
        memset(m, 0, sizeof *m);
        copyz(m->salt, sizeof m->salt, salt);
        copyz(m->name, sizeof m->name, mods[i].name && mods[i].name[0] ? mods[i].name : salt);
        m->in_map = 1;
        m->stop = -1;
        ReleaseSRWLockExclusive(&g_lock);
        open_mod(m, seed);
        if (m->status == SH_SCRIPT_RUNNING) running++;
    }
    g_active = 1;
    leave_run();
    return running;
}

int sh_script_active(void)
{
    int i, any = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++) if (g_mods[i].status == SH_SCRIPT_RUNNING) any = 1;
    ReleaseSRWLockShared(&g_lock);
    return g_active && any;
}

static void end_all(void)
{
    int i;
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++) {
        shs_mod *m = &g_mods[i];
        if (m->status == SH_SCRIPT_RUNNING && m->L) {
            m->tick_steps = 0;
            if (run_job(m, job_list, (void *)"map_end")) close_mod(m, SH_SCRIPT_ENDED);
        }
        if (m->L) close_mod(m, m->status == SH_SCRIPT_STOPPED ? SH_SCRIPT_STOPPED : SH_SCRIPT_ENDED);
        m->in_map = 0;
    }
    AcquireSRWLockExclusive(&g_lock);
    free(g_own);
    g_own = NULL;
    g_own_cap = g_own_used = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_active = 0;
}

void sh_script_map_end(void)
{
    if (g_run_tid == GetCurrentThreadId()) { g_end_pending = 1; return; }
    if (!enter_run()) return;
    end_all();
    leave_run();
}

/* ---------------------------------------------------------------- queue */

static shs_mod *running_mod(const char *salt)
{
    int i;
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++)
        if (g_mods[i].status == SH_SCRIPT_RUNNING && g_mods[i].queue && _stricmp(g_mods[i].salt, salt) == 0) return &g_mods[i];
    return NULL;
}

int sh_script_enqueue(const char *salt, const sh_script_event *ev)
{
    shs_mod *m;
    int queued = 0, log_drop = 0;
    char line[200];
    if (!salt || !ev || ev->kind < SH_SCRIPT_EV_SPAWN || ev->kind > SH_SCRIPT_EV_INPUT) return 0;
    AcquireSRWLockExclusive(&g_lock);
    m = running_mod(salt);
    if (m) {
        if (m->qcount < SH_SCRIPT_QUEUE_CAP) {
            sh_script_event *slot = &m->queue[(m->qhead + m->qcount) % SH_SCRIPT_QUEUE_CAP];
            *slot = *ev;
            slot->names[SH_SCRIPT_NAMES_CAP - 1] = 0;
            slot->text[SH_SCRIPT_TEXT_CAP - 1] = 0;
            slot->location[SH_SCRIPT_LOC_CAP - 1] = 0;
            m->qcount++;
            queued = 1;
            if (ev->kind == SH_SCRIPT_EV_SPAWN) own_put(ev->entity, (int)(m - g_mods));
        } else {
            m->dropped++;
            if (!m->drop_logged) { m->drop_logged = 1; log_drop = 1; }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (log_drop) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                  "Some game events were not given to this mod's scripts because more than %d arrived in one frame.",
                  SH_SCRIPT_QUEUE_CAP);
        line[sizeof line - 1] = 0;
        shs_log(m, line);
    }
    return queued;
}

int sh_script_binds(const char *full_name, char *salt_out, size_t cap, unsigned *kinds)
{
    int i, j, found = 0;
    if (!full_name) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_SCRIPT_MAX_MODS && !found; i++) {
        shs_mod *m = &g_mods[i];
        if (m->status != SH_SCRIPT_RUNNING) continue;
        for (j = 0; j < m->nbinds; j++) if (_stricmp(m->binds[j].name, full_name) == 0) {
            if (salt_out) copyz(salt_out, cap, m->salt);
            if (kinds) *kinds = m->binds[j].kinds;
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/* ------------------------------------------------------------ the tick */

static int find_ent(shs_mod *m, uint64_t handle)
{
    int i;
    for (i = 0; i < m->nents; i++) if (m->ents[i].handle == handle) return i;
    return -1;
}

/* Call H[kind][name] for each name of the chain with the values at
 * args..args+nargs-1 (the entity first). */
static void call_chain(lua_State *L, const char *kind, const char *chain, int args, int nargs)
{
    const char *p = chain;
    while (p && *p) {
        char name[SH_SCRIPT_DEFNAME_CAP];
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n < sizeof name) {
            memcpy(name, p, n);
            name[n] = 0;
            if (push_handlers(L, kind, name)) call_list(L, lua_gettop(L), args, nargs);
            lua_pop(L, 1);
        }
        p = e ? e + 1 : NULL;
    }
}

static void push_hit(lua_State *L, const sh_script_event *ev)
{
    lua_createtable(L, 0, 5);
    sh_script_push_entity(L, ev->other);
    lua_setfield(L, -2, "attacker");
    sh_script_push_entity(L, ev->inflictor);
    lua_setfield(L, -2, "inflictor");
    lua_pushnumber(L, ev->amount);
    lua_setfield(L, -2, "amount");
    lua_pushstring(L, ev->text);
    lua_setfield(L, -2, "damage");
    lua_pushstring(L, ev->location);
    lua_setfield(L, -2, "location");
}

static void input_missed(lua_State *L, shs_mod *m, const char *node)
{
    char line[SH_SCRIPT_LINE_CAP], list[300];
    size_t used = 0;
    int count = 0;
    push_handlers(L, "missed", NULL);
    if (lua_getfield(L, -1, node) != LUA_TNIL) { lua_pop(L, 2); return; }
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, node);
    lua_pop(L, 1);
    list[0] = 0;
    if (push_handlers(L, "input", NULL)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                int w = _snprintf_s(list + used, sizeof list - used, _TRUNCATE, "%s%s", count ? ", " : "", k);
                if (w > 0 && used + (size_t)w < sizeof list - 1) used += (size_t)w;
                count++;
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    list[sizeof list - 1] = 0;
    if (count) _snprintf_s(line, sizeof line, _TRUNCATE, "Script node \"%s\" (%s): no script handles it. The script handles: %s.", node, m->name, list);
    else _snprintf_s(line, sizeof line, _TRUNCATE, "Script node \"%s\" (%s): no script handles it. The scripts handle no Script node.", node, m->name);
    line[sizeof line - 1] = 0;
    shs_log(m, line);
}

/* Job: deliver one queued record. */
static int job_event(lua_State *L)
{
    const sh_script_event *ev = (const sh_script_event *)lua_touserdata(L, 1);
    shs_mod *m = shs_mod_of(L);
    int base = lua_gettop(L), at;
    if (ev->kind == SH_SCRIPT_EV_SPAWN) {
        char chain[SH_SCRIPT_NAMES_CAP];
        if (find_ent(m, ev->entity) >= 0 || !ev->entity) return 0;
        copyz(chain, sizeof chain, ev->names);
        lower(chain);
        if (m->nents == m->cents) {
            int c = m->cents ? m->cents * 2 : 32;
            shs_ent *p = (shs_ent *)realloc(m->ents, (size_t)c * sizeof *p);
            if (!p) return luaL_error(L, "not enough memory");
            m->ents = p;
            m->cents = c;
        }
        m->ents[m->nents].handle = ev->entity;
        m->ents[m->nents].chain = _strdup(chain);
        if (!m->ents[m->nents].chain) return luaL_error(L, "not enough memory");
        m->nents++;
        lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_LIVE);
        sh_script_push_entity(L, ev->entity);
        lua_rawseti(L, -2, (lua_Integer)ev->entity);
        sh_script_push_entity(L, ev->entity);
        call_chain(L, "spawn", chain, lua_gettop(L), 1);
        lua_settop(L, base);
        return 0;
    }
    if (ev->kind == SH_SCRIPT_EV_INPUT) {
        char node[SH_SCRIPT_NAMES_CAP];
        copyz(node, sizeof node, ev->names);
        lower(node);
        sh_script_push_entity(L, ev->entity);
        sh_script_push_entity(L, ev->other);
        at = lua_gettop(L) - 1;
        if (push_handlers(L, "input", node)) call_list(L, lua_gettop(L), at, 2);
        else { lua_pop(L, 1); input_missed(L, m, node); }
        lua_settop(L, base);
        return 0;
    }
    {
        int i = find_ent(m, ev->entity);
        char *chain;
        if (i < 0) return 0;                       /* removed before the record was delivered */
        chain = _strdup(m->ents[i].chain);         /* a handler may change the table */
        if (!chain) return luaL_error(L, "not enough memory");
        sh_script_push_entity(L, ev->entity);
        at = lua_gettop(L);
        if (ev->kind == SH_SCRIPT_EV_DAMAGE || ev->kind == SH_SCRIPT_EV_DEATH) {
            push_hit(L, ev);
            call_chain(L, ev->kind == SH_SCRIPT_EV_DAMAGE ? "damage" : "death", chain, at, 2);
        } else {
            char notify[SH_SCRIPT_TEXT_CAP];
            const char *p = chain;
            copyz(notify, sizeof notify, ev->text);
            lower(notify);
            while (p && *p) {
                char name[SH_SCRIPT_DEFNAME_CAP];
                const char *e = strchr(p, '\n');
                size_t n = e ? (size_t)(e - p) : strlen(p);
                if (n && n < sizeof name) {
                    memcpy(name, p, n);
                    name[n] = 0;
                    if (push_handlers(L, "anim", name)) {
                        if (lua_getfield(L, -1, notify) == LUA_TTABLE) call_list(L, lua_gettop(L), at, 1);
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }
                p = e ? e + 1 : NULL;
            }
        }
        free(chain);
        lua_settop(L, base);
        return 0;
    }
}

typedef struct { uint64_t handle; const char *chain; } shs_ent_job;

/* Job: think(self, dt) for one entity. */
static int job_think(lua_State *L)
{
    shs_ent_job *j = (shs_ent_job *)lua_touserdata(L, 1);
    sh_script_push_entity(L, j->handle);
    lua_pushnumber(L, g_dt);
    call_chain(L, "think", j->chain, lua_gettop(L) - 1, 2);
    return 0;
}

/* Job: remove(self) for an entity that is gone. */
static int job_remove(lua_State *L)
{
    shs_ent_job *j = (shs_ent_job *)lua_touserdata(L, 1);
    mark_dead(L, j->handle);
    sh_script_push_entity(L, j->handle);
    call_chain(L, "remove", j->chain, lua_gettop(L), 1);
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_LIVE);
    lua_pushnil(L);
    lua_rawseti(L, -2, (lua_Integer)j->handle);
    return 0;
}

/* Deliver what is queued now; 0 when the mod stopped. */
static int drain(shs_mod *m)
{
    unsigned n, k;
    sh_script_event ev;
    AcquireSRWLockShared(&g_lock);
    n = m->qcount;
    ReleaseSRWLockShared(&g_lock);
    for (k = 0; k < n; k++) {
        int have = 0;
        AcquireSRWLockExclusive(&g_lock);
        if (m->queue && m->qcount) {
            ev = m->queue[m->qhead];
            m->qhead = (m->qhead + 1) % SH_SCRIPT_QUEUE_CAP;
            m->qcount--;
            have = 1;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (!have) break;
        if (!run_job(m, job_event, &ev)) return 0;
        if (g_end_pending) return 0;
    }
    return 1;
}

static int queued(shs_mod *m)
{
    unsigned n;
    AcquireSRWLockShared(&g_lock);
    n = m->queue ? m->qcount : 0;
    ReleaseSRWLockShared(&g_lock);
    return n != 0;
}

static void tick_mod(shs_mod *m)
{
    int i, pass;
    m->tick_steps = 0;
    if (!drain(m)) return;
    /* think, and removal of what is gone */
    for (i = 0; i < m->nents;) {
        shs_ent_job j;
        uint64_t h = m->ents[i].handle;
        int alive = g_have_ent_ops && g_ent_ops.alive ? g_ent_ops.alive(g_ent_ops.context, h) : 1;
        j.handle = h;
        j.chain = m->ents[i].chain;
        if (!alive) {
            char *chain = m->ents[i].chain;
            if (!run_job(m, job_remove, &j)) return;
            /* only a spawn delivery changes the table, and none runs here */
            memmove(&m->ents[i], &m->ents[i + 1], (size_t)(m->nents - i - 1) * sizeof m->ents[0]);
            m->nents--;
            free(chain);
            AcquireSRWLockExclusive(&g_lock);
            own_drop(h);
            ReleaseSRWLockExclusive(&g_lock);
            if (g_end_pending) return;
            continue;
        }
        if (!run_job(m, job_think, &j)) return;
        if (g_end_pending) return;
        i++;
    }
    shs_timers_run(m);
    if (m->status != SH_SCRIPT_RUNNING || g_end_pending) return;
    for (pass = 1; pass < SH_SCRIPT_PASSES && queued(m); pass++)
        if (!drain(m)) return;
}

void sh_script_tick(double now, double dt)
{
    int i;
    if (!enter_run()) return;
    g_now = now;
    g_dt = dt;
    for (i = 0; i < SH_SCRIPT_MAX_MODS && !g_end_pending; i++)
        if (g_mods[i].status == SH_SCRIPT_RUNNING && g_mods[i].L) tick_mod(&g_mods[i]);
    leave_run();
}

int shs_run_job(shs_mod *m, lua_CFunction job, void *ud) { return run_job(m, job, ud); }
void shs_call_handler(lua_State *L, int nargs) { call_handler(L, nargs); }

/* ------------------------------------------------------------ queries */

int sh_script_status(const char *salt)
{
    int i, st = SH_SCRIPT_NONE;
    if (!salt) return st;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++) if (g_mods[i].salt[0] && _stricmp(g_mods[i].salt, salt) == 0) st = g_mods[i].status;
    ReleaseSRWLockShared(&g_lock);
    return st;
}

int sh_script_stop_line(const char *salt, char *out, size_t cap)
{
    int i, found = 0;
    if (cap) out[0] = 0;
    if (!salt) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++)
        if (g_mods[i].salt[0] && _stricmp(g_mods[i].salt, salt) == 0 && g_mods[i].stop_line[0]) {
            copyz(out, cap, g_mods[i].stop_line);
            found = 1;
        }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

int sh_script_log_read(const char *salt, sh_script_line_fn fn, void *context)
{
    int i, k, n = 0;
    if (!salt) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++)
        if (g_mods[i].salt[0] && _stricmp(g_mods[i].salt, salt) == 0 && g_mods[i].log) {
            for (k = 0; k < g_mods[i].nlog; k++) { if (fn) fn(context, g_mods[i].log[k]); n++; }
        }
    ReleaseSRWLockShared(&g_lock);
    return n;
}

int sh_script_mods(sh_script_mod_fn fn, void *context)
{
    int i, n = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++)
        if (g_mods[i].salt[0] && (g_mods[i].log || g_mods[i].status != SH_SCRIPT_NONE)) {
            if (fn) fn(context, g_mods[i].salt, g_mods[i].name);
            n++;
        }
    ReleaseSRWLockShared(&g_lock);
    return n;
}

size_t sh_script_bytes_in_use(void)
{
    size_t total = 0;
    int i, k;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_SCRIPT_MAX_MODS; i++) {
        shs_mod *m = &g_mods[i];
        total += m->mem;
        if (m->queue) total += SH_SCRIPT_QUEUE_CAP * sizeof *m->queue;
        total += (size_t)m->cents * sizeof *m->ents + (size_t)m->cbinds * sizeof *m->binds +
                 (size_t)m->ctimers * sizeof *m->timers;
        for (k = 0; k < m->nents; k++) total += strlen(m->ents[k].chain) + 1;
    }
    total += g_own_cap * sizeof *g_own;
    ReleaseSRWLockShared(&g_lock);
    return total;
}
