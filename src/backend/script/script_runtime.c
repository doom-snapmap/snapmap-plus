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
#include "../../common/text.h"
#include "script_internal.h"
#include "script_fsm.h"
#include "script_reason.h"
#include "script_compile.h"
#include "script_hooks.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef SH_SCRIPT_PURE
#include "../backend_log.h"
#endif

/* ------------------------------------------------------------------ state */

static shs_mod **g_modv;                     /* every mod seen this session, each kept in place */
static int       g_nmods, g_cmods;
static SRWLOCK   g_lock = SRWLOCK_INIT;      /* status, queues, bindings, owners, logs */
static SRWLOCK   g_run = SRWLOCK_INIT;       /* one caller runs Lua at a time */
static volatile DWORD g_run_tid;
static int       g_end_pending;
static int       g_end_handlers;             /* the deferred map end runs end handlers */
static int       g_active;
static int       g_started;                  /* the prepared mods have had their first tick */
static double    g_now, g_dt;
static const sh_script_source *g_source;

static sh_script_map_mods_fn g_provider;
static void     *g_provider_ctx;
static sh_script_line_fn g_console;
static void     *g_console_ctx;

static const sh_script_limits k_limits = { SH_SCRIPT_WALL_MS };
static sh_script_limits g_limits = { SH_SCRIPT_WALL_MS };

typedef struct { int kind; char *name; lua_CFunction fn; } shs_api;
static shs_api *g_api;                       /* the calls other modules added; grows */
static int g_napi, g_capi;

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

/* Free every kept line. Lock held. */
static void log_free(shs_mod *m)
{
    int i;
    for (i = 0; i < m->nlog; i++) free(m->log[i]);
    free(m->log);
    m->log = NULL;
    m->nlog = m->clog = 0;
}

/* The log keeps every line of the map, whole; the window may show a tail. */
void shs_log(shs_mod *m, const char *line)
{
    char *kept, *console;
    size_t n = strlen(line), cn;
    int is_stop = strncmp(line, "Script stopped:", 15) == 0;
    kept = (char *)malloc(n + 1);
    if (kept) memcpy(kept, line, n + 1);
    AcquireSRWLockExclusive(&g_lock);
    if (kept && m->nlog == m->clog) {
        int cap = m->clog ? m->clog * 2 : 64;
        char **grown = (char **)realloc(m->log, (size_t)cap * sizeof *grown);
        if (grown) { m->log = grown; m->clog = cap; }
    }
    if (kept && m->nlog < m->clog) { m->log[m->nlog++] = kept; kept = NULL; }
    if (is_stop) {
        char *copy = _strdup(line);
        if (copy) { free(m->stop_line); m->stop_line = copy; }
    }
    ReleaseSRWLockExclusive(&g_lock);
    free(kept);                                   /* only when memory had no room to keep it */
    cn = n + strlen(m->name) + 16;
    console = (char *)malloc(cn);
    if (!console) return;
    if (is_stop) memcpy(console, line, n + 1);
    else sprintf_s(console, cn, "Script (%s): %s", m->name, line);
#ifndef SH_SCRIPT_PURE
    backend_log(console);
#endif
    if (g_console) g_console(g_console_ctx, console);
    free(console);
}

void sh_script_set_console(sh_script_line_fn fn, void *context)
{
    g_console = fn;
    g_console_ctx = context;
}

void sh_script_set_map_provider(sh_script_map_mods_fn fn, void *context)
{
    g_provider = fn;
    g_provider_ctx = context;
}

/* ----------------------------------------------------------- allocation */

/* Counts every byte (sh_script_bytes_in_use); refuses nothing the process
 * can give. Lua never asks a shrink to fail. */
static void *shs_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    shs_mod *m = (shs_mod *)ud;
    size_t old = ptr ? osize : 0;
    void *p;
    if (nsize == 0) {
        if (ptr) { m->mem -= old; free(ptr); }
        return NULL;
    }
    p = realloc(ptr, nsize);
    if (!p) return (nsize <= old) ? ptr : NULL;
    m->mem = m->mem - old + nsize;
    return p;
}

/* ------------------------------------------------------------- bounds */

/* luaL_where with the script's file whole ("" for a line that is not a script's). */
static void push_where(lua_State *L, int level)
{
    char *file = NULL;
    int line = 0;
    sh_script_where_line(L, level, &file, &line);
    if (file) lua_pushfstring(L, "%s:%d: ", file, line);
    else lua_pushliteral(L, "");
    free(file);
}

int shs_raise_stop(lua_State *L, int level)
{
    push_where(L, level);
    lua_pushstring(L, "stopped");
    lua_concat(L, 2);
    return lua_error(L);
}

/* The one protection against a script that never returns: a single
 * delivery (one handler call, one timer, one coroutine resume) that has run
 * longer than the wall-clock stop is stopped. Nothing counts work. */
static int over_bound(shs_mod *m)
{
    if (m->stop >= 0) return 1;
    if (g_limits.wall_ms && (qpc() - m->wall_start) * 1000 / g_qpf > (long long)g_limits.wall_ms)
        m->stop = SH_SCRIPT_STOP_WALL;
    return m->stop >= 0;
}

void shs_check_clock(lua_State *L)
{
    if (over_bound(shs_mod_of(L))) shs_raise_stop(L, 1);
}

/* A pattern match checks the clock every 1024 of its steps (one pattern can
 * run long inside a single VM instruction). */
void shs_match_step(lua_State *L)
{
    shs_mod *m = shs_mod_of(L);
    if ((++m->pattern_steps & 1023) == 0) over_bound(m);
    if (m->stop >= 0) shs_raise_stop(L, 1);
}

/* The count hook: every SH_SCRIPT_HOOK_STRIDE instructions, in every
 * coroutine of the state, it only reads the clock. In a hook, level 0 is the
 * running Lua function. */
/* A function's file as the product named its chunk ("=scripts\x.lua"),
 * whole: Lua's short_src is cut to LUA_IDSIZE. */
static const char *src_name(const lua_Debug *ar)
{
    return ar->source && ar->source[0] == '=' ? ar->source + 1 : ar->short_src;
}

static void shs_hook(lua_State *L, lua_Debug *ar)
{
    shs_mod *m = shs_mod_of(L);
    if (m->stop < 0 && lua_getinfo(L, "Sl", ar)) {
        const char *src = src_name(ar);
        if (!m->last_src || strcmp(m->last_src, src)) {
            free(m->last_src);
            m->last_src = _strdup(src);
        }
        m->last_line = ar->currentline;
    }
    if (over_bound(m)) shs_raise_stop(L, 0);
}

void shs_call_reset(shs_mod *m)
{
    m->pattern_steps = 0;
    m->wall_start = qpc();
    free(m->last_src);
    m->last_src = NULL;
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
    if (m->err_file) return;
    for (level = first; lua_getstack(L, level, &ar); level++) {
        if (!lua_getinfo(L, "Sl", &ar)) break;
        if (ar.currentline > 0 && starts(src_name(&ar), "scripts\\")) {
            m->err_file = _strdup(src_name(&ar));
            m->err_line = ar.currentline;
            return;
        }
    }
}

static int shs_msgh(lua_State *L)
{
    shs_mod *m = shs_mod_of(L);
    const char *msg = lua_isstring(L, 1) ? lua_tostring(L, 1) : NULL;
    if (!m->err_file) {
        shs_where_msg(m, msg);
        if (!m->err_file) shs_where_from(m, L, 1);
    }
    lua_settop(L, 1);
    return 1;
}

static void close_mod(shs_mod *m, int status);

static void stop_line(shs_mod *m, const char *file, int line, const char *what, const char *todo)
{
    char *disp = _strdup(file ? file : ""), *text;
    size_t i;
    if (!disp) return;
    for (i = 0; disp[i]; i++) if (disp[i] == '/') disp[i] = '\\';
    text = sh_script_stop_text(m->name, disp, line, what ? what : "stopped", todo);   /* the line whole */
    free(disp);
    if (!text) return;
    shs_log(m, text);
    free(text);
}

/* Lua names a chunk in its messages cut to LUA_IDSIZE - 1 bytes (the
 * vendored Lua's luaO_chunkid): a message's file is taken only when it is
 * shorter than that, so it is whole; else the stack says where. */
static char *msg_file(const char *msg, int *line, const char **rest)
{
    char *file = NULL;
    const char *r = sh_script_split_where(msg, &file, line);
    if (rest) *rest = r;
    if (file && strlen(file) >= LUA_IDSIZE - 1) { free(file); file = NULL; *line = 0; }
    return file;
}

void sh_script_where_line(lua_State *L, int level, char **file, int *line)
{
    lua_Debug ar;
    *file = NULL;
    *line = 0;
    if (lua_getstack(L, level, &ar) && lua_getinfo(L, "Sl", &ar) && ar.currentline > 0 &&
        starts(src_name(&ar), "scripts\\")) {
        *file = _strdup(src_name(&ar));
        *line = ar.currentline;
    }
}

void shs_where_msg(shs_mod *m, const char *msg)
{
    int line = 0;
    char *file;
    if (m->err_file || !msg) return;
    file = msg_file(msg, &line, NULL);
    if (file) { m->err_file = file; m->err_line = line; }
}

/* Word and log the stop of a failed call; the error object is on L's top. */
static void stop_from_error(shs_mod *m, lua_State *L, int status)
{
    char *todo = NULL, *file = NULL, *what = NULL;
    int line = 0;
    const char *msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    if (status == LUA_ERRMEM && m->stop < 0) m->stop = SH_SCRIPT_STOP_MEMORY;
    if (m->stop >= 0) {
        sh_script_reason_stop(m->stop, (unsigned long long)g_limits.wall_ms, &what, &todo);
        if (m->err_file) { file = _strdup(m->err_file); line = m->err_line; }
        else if (msg) file = msg_file(msg, &line, NULL);
        if (!file && m->last_src) { file = _strdup(m->last_src); line = m->last_line; }
    } else if (msg) {
        const char *text = msg;
        file = msg_file(msg, &line, &text);
        if (m->err_file) { free(file); file = _strdup(m->err_file); line = m->err_line; }
        sh_script_reason(text, &what, &todo);
    } else {
        file = m->err_file ? _strdup(m->err_file) : NULL;
        line = m->err_line;
        what = _strdup("the script raised an error whose value is not text");
        todo = _strdup("Give error(...) a text that says what went wrong");
    }
    stop_line(m, file, line, what, todo);
    free(what);
    free(todo);
    free(file);
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
    free(m->err_file);
    m->err_file = NULL;
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

/* The runtime mod whose hooks slot is `hook`, any status. */
static shs_mod *mod_by_hook(int hook)
{
    int i;
    if (hook < 0) return NULL;
    for (i = 0; i < g_nmods; i++) if ((*g_modv[i]).in_map && (*g_modv[i]).hook == hook) return &(*g_modv[i]);
    return NULL;
}

/* Ownership is the handle table's: an entity recorded under the mod's slot
 * (placed from its entityDefs or spawned by its scripts) and not removed. */
int sh_script_entity_owner(uint64_t handle, char *salt_out, size_t cap)
{
    int hook, ok = 0;
    shs_mod *m;
    if (!handle || handle > 0xFFFFFFFFull) return 0;
    hook = sh_script_handle_mod((sh_script_handle)handle);
    if (hook < 0 || !sh_script_handle_known((sh_script_handle)handle)) return 0;
    AcquireSRWLockShared(&g_lock);
    m = mod_by_hook(hook);
    if (m && m->status == SH_SCRIPT_RUNNING) {
        copyz(salt_out, cap, m->salt);
        ok = 1;
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

int sh_script_owns_entity(lua_State *L, uint64_t handle)
{
    shs_mod *m = shs_mod_of(L);
    if (!handle || handle > 0xFFFFFFFFull || m->hook < 0) return 0;
    return sh_script_handle_mod((sh_script_handle)handle) == m->hook &&
           sh_script_handle_known((sh_script_handle)handle);
}

/* -------------------------------------------------------------- naming */

int sh_script_refuse(lua_State *L, const char *fmt, ...)
{
    char *text;
    va_list ap;
    va_start(ap, fmt);
    text = sh_vtextf(fmt, ap);
    va_end(ap);
    push_where(L, 1);
    lua_pushstring(L, text ? text : "out of memory");
    free(text);
    lua_concat(L, 2);
    return lua_error(L);
}

/* Keep the string on top of the stack alive in one of the state's eight
 * name slots and pop it: the stack is as the call found it, so a caller's
 * argument positions (and their nils) stay as they were. */
static const char *anchor_name(lua_State *L)
{
    shs_mod *m = shs_mod_of(L);
    const char *p = lua_tostring(L, -1);
    if (lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_NAMES) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_createtable(L, SHS_NAME_SLOTS, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_NAMES);
    }
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, (lua_Integer)(m->name_slot++ % SHS_NAME_SLOTS) + 1);
    lua_pop(L, 2);
    return p;
}

const char *sh_script_check_name(lua_State *L, int arg, const char *call, int allow_game, int *is_game)
{
    shs_mod *m = shs_mod_of(L);
    const char *name, *buf;
    luaL_Buffer b;
    size_t n, i, sl;
    if (is_game) *is_game = 0;
    if (luaL_testudata(L, arg, SHS_MT_GAME)) {
        lua_getiuservalue(L, arg, 1);
        name = lua_tostring(L, -1);
        if (!allow_game)
            sh_script_refuse(L, "\"%s\" can only use this mod's own content, not the game's \"%s\"", call, name ? name : "");
        if (!name) { lua_pop(L, 1); lua_pushstring(L, ""); }
        if (is_game) *is_game = 1;
        return anchor_name(L);
    }
    if (lua_type(L, arg) != LUA_TSTRING)
        sh_script_refuse(L, "\"%s\" needs a name here, got %s", call,
                         lua_isnoneornil(L, arg) ? "nothing" : luaL_typename(L, arg));
    name = lua_tolstring(L, arg, &n);
    if (n == 0) sh_script_refuse(L, "\"%s\" was given a name that is empty", call);
    luaL_buffinit(L, &b);
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/'))
            sh_script_refuse(L, "\"%s\" was given \"%s\", which is not a usable name (use letters, digits, _ - . and /)", call, name);
        luaL_addchar(&b, c);
    }
    luaL_pushresult(&b);
    buf = lua_tostring(L, -1);
    if (buf[0] == '/' || buf[n - 1] == '/' || strstr(buf, "//") || strstr(buf, ".."))
        sh_script_refuse(L, "\"%s\" was given \"%s\", which is not a usable name", call, name);
    sl = strlen(m->salt);
    if (strncmp(buf, "mods/", 5) == 0) {
        if (strncmp(buf + 5, m->salt, sl) != 0 || buf[5 + sl] != '/' || !buf[6 + sl])
            sh_script_refuse(L, "\"%s\" can only use this mod's own content; \"%s\" belongs to another mod", call, name);
        return anchor_name(L);                         /* already the full name */
    }
    lua_pushfstring(L, "mods/%s/%s", m->salt, buf);
    lua_remove(L, -2);
    return anchor_name(L);
}

/* --------------------------------------------------------- registration */

static const char *const k_kinds[] = { "spawn", "think", "remove", "damage", "death", "anim", "pain", "projectile",
                                       "frame_event" };
static const unsigned k_bits[] = { SH_SCRIPT_BIND_SPAWN, SH_SCRIPT_BIND_THINK, SH_SCRIPT_BIND_REMOVE,
                                   SH_SCRIPT_BIND_DAMAGE, SH_SCRIPT_BIND_DEATH, SH_SCRIPT_BIND_ANIM,
                                   SH_SCRIPT_BIND_PAIN, SH_SCRIPT_BIND_PROJECTILE, SH_SCRIPT_BIND_FRAME };
#define SHS_KINDS      9
#define SHS_KIND_ANIM  5
#define SHS_KIND_FRAME 8
/* The kinds whose second value names what in the animation they wait for. */
#define SHS_KEYED(kind) ((kind) == SHS_KIND_ANIM || (kind) == SHS_KIND_FRAME)

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
        if (ok && !(m->binds[i].name = _strdup(full))) ok = 0;
        if (ok) { m->binds[i].kinds = 0; m->nbinds++; }
    }
    if (ok) m->binds[i].kinds |= bit;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

/* Append the function at `fn` to the list t[key] of the table at `t`. */
/* Push a lower-case copy of s (ASCII letters; any length) and return it. */
static const char *push_lower(lua_State *L, const char *s)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (; *s; s++) luaL_addchar(&b, (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s);
    luaL_pushresult(&b);
    return lua_tostring(L, -1);
}

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

/* on.spawn / think / remove / damage / death / pain / projectile (name, fn);
 * on.anim(name, notify, fn); on.frame_event(name, command, fn). */
static int on_entity(lua_State *L)
{
    shs_mod *m = shs_mod_of(L);
    int kind = (int)lua_tointeger(L, lua_upvalueindex(1));
    char call[24];
    const char *full, *notify = NULL;
    int fn = SHS_KEYED(kind) ? 3 : 2;
    _snprintf_s(call, sizeof call, _TRUNCATE, "on.%s", k_kinds[kind]);
    call[sizeof call - 1] = 0;
    full = sh_script_check_name(L, 1, call, 0, NULL);
    if (kind == SHS_KIND_FRAME) {
        size_t n;
        const char *s = lua_type(L, 2) == LUA_TSTRING ? lua_tolstring(L, 2, &n) : NULL;
        if (!s || !n)
            sh_script_refuse(L, "\"on.frame_event\" needs the command name its ae_notify rows start with as its second value");
        if (strchr(s, ':') || strchr(s, ' '))
            sh_script_refuse(L, "\"on.frame_event\" needs a command name without \":\" or spaces");
        notify = push_lower(L, s);
    } else if (kind == SHS_KIND_ANIM) {
        size_t n;
        const char *s = lua_type(L, 2) == LUA_TSTRING ? lua_tolstring(L, 2, &n) : NULL;
        if (!s || !n)
            sh_script_refuse(L, "\"on.anim\" needs the notify text of an ae_notify row as its second value");
        (void)n;
        notify = push_lower(L, s);
    }
    if (lua_type(L, fn) != LUA_TFUNCTION)
        sh_script_refuse(L, "\"%s\" needs a function as its %s value, got %s", call, fn == 3 ? "third" : "second",
                         lua_isnoneornil(L, fn) ? "nothing" : luaL_typename(L, fn));
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    lua_getfield(L, -1, k_kinds[kind]);
    if (SHS_KEYED(kind)) {
        if (lua_getfield(L, -1, full) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, full);
        }
        list_append(L, lua_gettop(L), notify, fn);
    } else list_append(L, lua_gettop(L), full, fn);
    if (!add_bind(m, full, k_bits[kind])) luaL_error(L, "not enough memory");
    m->handlers++;
    return 0;
}

static int on_input(lua_State *L)
{
    const char *name;
    size_t n;
    const char *s = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : NULL;
    if (!s || !n)
        sh_script_refuse(L, "\"on.input\" needs the name of a Script node as its first value");
    if (lua_type(L, 2) != LUA_TFUNCTION)
        sh_script_refuse(L, "\"on.input\" needs a function as its second value, got %s",
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    name = push_lower(L, s);
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    lua_getfield(L, -1, "input");
    list_append(L, lua_gettop(L), name, 2);
    shs_mod_of(L)->handlers++;
    return 0;
}

static int (*g_signal_known)(const char *event);
static void (*g_signal_wanted)(void);

void sh_script_set_signal_ops(int (*known)(const char *event), void (*wanted)(void))
{
    g_signal_known = known;
    g_signal_wanted = wanted;
}

/* Push the event's key (lower case, without snap_event_) and return it. */
static const char *push_signal_key(lua_State *L, const char *s)
{
    const char *low = push_lower(L, s);
    if (!strncmp(low, "snap_event_", 11)) {
        lua_pushstring(L, low + 11);
        lua_remove(L, -2);
        low = lua_tostring(L, -1);
    }
    return low;
}

static int on_signal(lua_State *L)
{
    const char *name;
    size_t n = 0;
    const char *s = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : NULL;
    if (!s || !n)
        sh_script_refuse(L, "\"on.signal\" needs the name of a SnapMap event as its first value, such as \"ai_killed\"");
    if (lua_type(L, 2) != LUA_TFUNCTION)
        sh_script_refuse(L, "\"on.signal\" needs a function as its second value, got %s",
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    name = push_signal_key(L, s);
    if (g_signal_known && !g_signal_known(name))
        sh_script_refuse(L, "\"on.signal\" found no SnapMap event named %s in the game", s);
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    lua_getfield(L, -1, "signal");
    list_append(L, lua_gettop(L), name, 2);
    lua_pushfstring(L, "signal:%s", name);
    if (!add_bind(shs_mod_of(L), lua_tostring(L, -1), SH_SCRIPT_BIND_SIGNAL)) luaL_error(L, "not enough memory");
    shs_mod_of(L)->handlers++;
    if (g_signal_wanted) g_signal_wanted();
    return 0;
}

/* ------------------------------------------- state-machine classes (script_fsm.h) */

#define SHS_REG_FSM "snapmap-plus.fsm"          /* "<kind>:<class>" -> function, or a state's table */
static const char *const k_fsm_kinds[] = { "condition", "snippet", "transition", "state" };

/* fsm.condition(name, fn), fsm.snippet(name, fn), fsm.transition(name, fn),
 * fsm.state(name, { enter = fn, update = fn, exit = fn }): a class the game's own AI state
 * machines ask for by name (smp.<kind>.<salt>.<name>.<n> in the mod's aifsmmanager decl).
 * fn(demon, n): a condition answers true or false, a transition the code of the link it fires
 * or false, a state's update or enter a code for its machine or nothing. */
static int fsm_register(lua_State *L)
{
    int kind = (int)lua_tointeger(L, lua_upvalueindex(1));
    const char *call = k_fsm_kinds[kind];
    size_t n = 0, i;
    const char *name = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : NULL;
    if (!name || !n)
        sh_script_refuse(L, "\"fsm.%s\" needs the class name as its first value, such as \"ATRON_UseGrenades\"", call);
    if (n >= SH_FSM_CLASS_CAP)
        sh_script_refuse(L, "\"fsm.%s\" takes a class name of at most %d characters; %s has %d", call,
                         SH_FSM_CLASS_CAP - 1, name, (int)n);
    for (i = 0; i < n; i++) {
        int c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            sh_script_refuse(L, "\"fsm.%s\" takes a class name of letters, digits and _ only; %s has another character",
                             call, name);
    }
    if (kind == SH_FSM_STATE) {
        if (lua_type(L, 2) != LUA_TTABLE)
            sh_script_refuse(L, "\"fsm.state\" needs a table with enter, update or exit functions as its second "
                                "value, got %s", lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    } else if (lua_type(L, 2) != LUA_TFUNCTION) {
        sh_script_refuse(L, "\"fsm.%s\" needs a function as its second value, got %s", call,
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    }
    if (lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_FSM) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_FSM);
    }
    lua_pushfstring(L, "%s:%s", call, name);
    lua_pushvalue(L, -1);
    if (lua_rawget(L, -3) != LUA_TNIL)
        sh_script_refuse(L, "\"fsm.%s\" was already given the class %s in this mod; a class is registered once", call,
                         name);
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    lua_rawset(L, -3);
    shs_mod_of(L)->handlers++;
    return 0;
}

static int fsm_job(lua_State *L)
{
    sh_script_fsm_request *rq = (sh_script_fsm_request *)lua_touserdata(L, 1);
    if (lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_FSM) != LUA_TTABLE) return 0;
    lua_pushfstring(L, "%s:%s", k_fsm_kinds[rq->kind], rq->name);
    lua_rawget(L, -2);
    if (rq->kind == SH_FSM_STATE) {
        if (lua_type(L, -1) != LUA_TTABLE) return 0;
        lua_getfield(L, -1, rq->hook ? rq->hook : "update");
        if (lua_type(L, -1) != LUA_TFUNCTION) {      /* a state need not have every hook */
            rq->found = lua_isnil(L, -1);
            return 0;
        }
    } else if (lua_type(L, -1) != LUA_TFUNCTION) {
        return 0;
    }
    rq->found = 1;
    sh_script_push_entity(L, rq->self);
    lua_pushinteger(L, rq->index);
    shs_call_reset(shs_mod_of(L));
    lua_call(L, 2, 1);
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        rq->truth = lua_toboolean(L, -1);
    } else if (lua_type(L, -1) == LUA_TNUMBER) {
        rq->has_code = 1;
        rq->code = (int)lua_tonumber(L, -1);
        rq->truth = rq->code != 0;
    }
    return 0;
}

int sh_script_signal_post(const char *event, sh_script_handle self, sh_script_handle activator)
{
    int hooks[64], n = 0, i, j;
    size_t len = event ? strlen(event) : 0;
    if (!len) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmods && n < 64; i++) {
        shs_mod *m = &(*g_modv[i]);
        if (m->status != SH_SCRIPT_RUNNING || m->hook < 0) continue;
        for (j = 0; j < m->nbinds; j++) {
            const char *b = m->binds[j].name;
            if ((m->binds[j].kinds & SH_SCRIPT_BIND_SIGNAL) && !strncmp(b, "signal:", 7) && !strcmp(b + 7, event)) {
                hooks[n++] = m->hook;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    for (i = 0; i < n; i++) sh_script_queue_post(hooks[i], SH_SCRIPT_EV_SIGNAL, self, activator, 0, 0.0f, event, NULL);
    return n;
}

static sh_script_closer_fn *g_closers;
static int g_nclosers, g_cclosers;

int sh_script_api_add_closer(sh_script_closer_fn fn)
{
    int i;
    if (!fn) return 0;
    for (i = 0; i < g_nclosers; i++) if (g_closers[i] == fn) return 1;
    if (g_nclosers == g_cclosers) {
        int cap = g_cclosers ? g_cclosers * 2 : 8;
        sh_script_closer_fn *grown = (sh_script_closer_fn *)realloc(g_closers, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        g_closers = grown;
        g_cclosers = cap;
    }
    g_closers[g_nclosers++] = fn;
    return 1;
}

static int on_map(lua_State *L)
{
    const char *kind = lua_tostring(L, lua_upvalueindex(1));
    if (lua_type(L, 1) != LUA_TFUNCTION)
        sh_script_refuse(L, "\"on.%s\" needs a function, got %s", kind,
                         lua_isnoneornil(L, 1) ? "nothing" : luaL_typename(L, 1));
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    list_append(L, lua_gettop(L), kind, 1);
    shs_mod_of(L)->handlers++;
    return 0;
}

static int on_newindex(lua_State *L)
{
    return sh_script_refuse(L, "\"on\" lists what a script handles and cannot be changed; call on.spawn(name, function) and the others");
}

int sh_script_api_add(int kind, const char *name, lua_CFunction fn)
{
    size_t n = name ? strlen(name) : 0;
    char *copy;
    if ((kind != SH_SCRIPT_API_GLOBAL && kind != SH_SCRIPT_API_METHOD) || !fn || !n) return 0;
    if (g_napi == g_capi) {
        int cap = g_capi ? g_capi * 2 : 128;
        shs_api *grown = (shs_api *)realloc(g_api, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        g_api = grown;
        g_capi = cap;
    }
    if (!(copy = _strdup(name))) return 0;
    g_api[g_napi].kind = kind;
    g_api[g_napi].name = copy;
    g_api[g_napi].fn = fn;
    g_napi++;
    return 1;
}

static sh_script_opener_fn *g_openers;
static int g_nopeners, g_copeners;

int sh_script_api_add_opener(sh_script_opener_fn fn)
{
    int i;
    if (!fn) return 0;
    for (i = 0; i < g_nopeners; i++) if (g_openers[i] == fn) return 1;
    if (g_nopeners == g_copeners) {
        int cap = g_copeners ? g_copeners * 2 : 8;
        sh_script_opener_fn *grown = (sh_script_opener_fn *)realloc(g_openers, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        g_openers = grown;
        g_copeners = cap;
    }
    g_openers[g_nopeners++] = fn;
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
    for (i = 0; i < SHS_KINDS; i++) { lua_newtable(L); lua_setfield(L, -2, k_kinds[i]); }
    lua_newtable(L); lua_setfield(L, -2, "input");
    lua_newtable(L); lua_setfield(L, -2, "signal");
    lua_newtable(L); lua_setfield(L, -2, "map_start");
    lua_newtable(L); lua_setfield(L, -2, "map_end");
    lua_newtable(L); lua_setfield(L, -2, "missed");
    lua_setfield(L, LUA_REGISTRYINDEX, SHS_REG_HANDLERS);
    /* entity objects: one per handle while its entity lives, so e.state
     * lasts; each goes when the handle table releases its entity */
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
    /* globals other modules open themselves (sh_script_api_add_opener) */
    for (i = 0; i < g_nopeners; i++) g_openers[i](L);
    /* fsm: the registration calls for state-machine classes */
    lua_newtable(L);
    for (i = 0; i < SH_FSM_KINDS; i++) {
        lua_pushinteger(L, i);
        lua_pushcclosure(L, fsm_register, 1);
        lua_setfield(L, -2, k_fsm_kinds[i]);
    }
    lua_setglobal(L, "fsm");
    /* on: a read-only table of the registration calls */
    lua_newtable(L);
    for (i = 0; i < SHS_KINDS; i++) {
        lua_pushinteger(L, i);
        lua_pushcclosure(L, on_entity, 1);
        lua_setfield(L, -2, k_kinds[i]);
    }
    lua_pushcfunction(L, on_input); lua_setfield(L, -2, "input");
    lua_pushcfunction(L, on_signal); lua_setfield(L, -2, "signal");
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

typedef struct { char **names; int n, cap, total; } shs_list;

static void list_keep(shs_list *l, const char *relpath)
{
    char *copy;
    if (l->n == l->cap) {
        int c = l->cap ? l->cap * 2 : 16;
        char **p = (char **)realloc(l->names, (size_t)c * sizeof *p);
        if (!p) return;
        l->names = p;
        l->cap = c;
    }
    if ((copy = _strdup(relpath)) != NULL) l->names[l->n++] = copy;
}

static void list_free(shs_list *l)
{
    int i;
    for (i = 0; i < l->n; i++) free(l->names[i]);
    free(l->names);
    l->names = NULL;
    l->n = l->cap = 0;
}

static void list_cb(void *ctx, const char *relpath)
{
    shs_list *l = (shs_list *)ctx;
    size_t n = relpath ? strlen(relpath) : 0;
    l->total++;
    if (n < 5 || strchr(relpath, '/') || strchr(relpath, '\\')) return;
    if (_stricmp(relpath + n - 4, ".lua") != 0) return;
    list_keep(l, relpath);
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

typedef struct { const char *relpath; const char *text; size_t len; } shs_file_job;

int shs_load_file(lua_State *L, shs_mod *m, const char *relpath)
{
    char *chunk, *text = NULL;
    size_t len = 0, i;
    int r, status;
    lua_pushfstring(L, "=scripts\\%s", relpath);   /* the chunk name, whole, held on the stack */
    chunk = (char *)lua_tostring(L, -1);
    for (i = 0; chunk[i]; i++) if (chunk[i] == '/') chunk[i] = '\\';
    if (!m->source || !m->source->read) {
        lua_pushfstring(L, "%s: the file could not be read", chunk + 1);
        lua_remove(L, -2);
        return LUA_ERRFILE;
    }
    r = m->source->read(m->source->context, m->salt, relpath, &text, &len);
    if (r <= 0) {
        lua_pushfstring(L, r == 0 ? "%s: the file is not in this mod" : "%s: the file could not be read", chunk + 1);
        lua_remove(L, -2);
        return LUA_ERRFILE;
    }
    {
        const char *t = text;                 /* a UTF-8 byte order mark is not Lua */
        size_t n = len;
        if (n >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) { t += 3; n -= 3; }
        status = luaL_loadbufferx(L, t, n, chunk, "t");
    }
    lua_remove(L, -2);   /* the chunk name; the loaded function or the error stays on top */
    if (m->source->release) m->source->release(m->source->context, text);
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
static int check_api_file(shs_mod *m, const char *relpath, const char *file);

static int check_api(shs_mod *m, const char *relpath)
{
    char *file;
    size_t i;
    int ok;
    if (!(file = sh_textf("scripts\\%s", relpath))) return 0;
    for (i = 0; file[i]; i++) if (file[i] == '/') file[i] = '\\';
    ok = check_api_file(m, relpath, file);
    free(file);
    return ok;
}

static int check_api_file(shs_mod *m, const char *relpath, const char *file)
{
    char *text = NULL;
    size_t len = 0;
    long version = 0;
    int line = 1, r, ok;
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
    ok = sh_script_api_declared(text, len, &version, &line);
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

static void set_status(shs_mod *m, int status)
{
    AcquireSRWLockExclusive(&g_lock);
    m->status = status;
    ReleaseSRWLockExclusive(&g_lock);
}

/* Close the state and drop what the map held for it; keep the log. Its
 * queue closes, so hooks stop routing records to it. */
static void close_mod(shs_mod *m, int status)
{
    if (m->L) {
        lua_sethook(m->L, NULL, 0, 0);
        lua_close(m->L);
        m->L = NULL;
    }
    free_mod_tables(m);
    if (m->hook >= 0) sh_script_queue_close(m->hook);
    AcquireSRWLockExclusive(&g_lock);
    m->status = status;
    m->prepared = 0;
    {
        int k;
        for (k = 0; k < m->nbinds; k++) free(m->binds[k].name);
    }
    free(m->binds);
    m->binds = NULL;
    m->nbinds = m->cbinds = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

/* The first tick of the map: open the state, run the entry files in name
 * order, then the map_start handlers. */
static void open_mod(shs_mod *m)
{
    int i;
    m->prepared = 0;
    m->stop = -1;
    m->charging = 0;
    m->mem = 0;
    m->files_run = 0;
    m->handlers = 0;
    m->L = lua_newstate(shs_alloc, m);
    if (!m->L) {
        open_failed(m, "there was not enough memory to start this mod's scripts");
        close_mod(m, SH_SCRIPT_STOPPED);
        return;
    }
    *(shs_mod **)lua_getextraspace(m->L) = m;
    lua_sethook(m->L, shs_hook, LUA_MASKCOUNT, SH_SCRIPT_HOOK_STRIDE);
    set_status(m, SH_SCRIPT_RUNNING);
    if (!run_job(m, job_open, &m->seed)) return;
    for (i = 0; i < m->nfiles; i++) {
        shs_file_job f;
        if (!check_api(m, m->files[i])) { close_mod(m, SH_SCRIPT_STOPPED); return; }
        f.relpath = m->files[i];
        f.text = NULL;
        f.len = 0;
        if (!run_job(m, job_file, &f)) return;
        m->files_run++;
    }
    run_job(m, job_list, (void *)"map_start");
}

static void free_files(shs_mod *m)
{
    int i;
    for (i = 0; i < m->nfiles; i++) free(m->files[i]);
    free(m->files);
    m->files = NULL;
    m->nfiles = 0;
}

/* List the mod's scripts and register it with the hooks. 1 when it has
 * entry files and a slot; 0 otherwise (a reason is in its log when there is
 * one to give). */
static int prepare_mod(shs_mod *m)
{
    shs_list files;
    int n;
    memset(&files, 0, sizeof files);
    m->source = g_source;
    if (!g_source || !g_source->list || !g_source->read) return 0;
    n = g_source->list(g_source->context, m->salt, list_cb, &files);
    if (n < 0) {
        stop_line(m, "", 0, "this mod's scripts could not be read, so they did not run",
                  "Press Refresh in the Mods tab and play the map again");
        set_status(m, SH_SCRIPT_STOPPED);
        list_free(&files);
        return 0;
    }
    if (!files.n) { list_free(&files); return 0; }
    qsort(files.names, (size_t)files.n, sizeof *files.names, name_cmp);
    n = sh_script_hooks_mod_add(m->salt, m->name);
    AcquireSRWLockExclusive(&g_lock);              /* read by owner queries on any thread */
    m->hook = n;
    ReleaseSRWLockExclusive(&g_lock);
    if (m->hook < 0) {
        open_failed(m, "this mod's scripts could not be started in this map");
        set_status(m, SH_SCRIPT_STOPPED);
        list_free(&files);
        return 0;
    }
    m->files = files.names;                    /* the mod keeps the names */
    files.names = NULL;
    m->nfiles = files.n;
    m->files_found = files.n;
    m->prepared = 1;
    return 1;
}

/* ------------------------------------------------------------- the map */

static int enter_run(void)
{
    if (g_run_tid == GetCurrentThreadId()) return 0;
    AcquireSRWLockExclusive(&g_run);
    g_run_tid = GetCurrentThreadId();
    return 1;
}

static void end_all(int run_end_handlers);

static void leave_run(void)
{
    if (g_end_pending) { g_end_pending = 0; end_all(g_end_handlers); }
    g_run_tid = 0;
    ReleaseSRWLockExclusive(&g_run);
}

static int find_slot(const char *salt)
{
    int i, free_slot = -1;
    for (i = 0; i < g_nmods; i++) {
        if ((*g_modv[i]).salt[0] && strcmp((*g_modv[i]).salt, salt) == 0) return i;
        if (free_slot < 0 && !(*g_modv[i]).salt[0]) free_slot = i;
    }
    if (free_slot >= 0) return free_slot;
    /* a new salt: one more mod (the table grows; readers hold g_lock) */
    {
        shs_mod *m = (shs_mod *)calloc(1, sizeof *m);
        if (!m) return -1;
        AcquireSRWLockExclusive(&g_lock);
        if (g_nmods == g_cmods) {
            int cap = g_cmods ? g_cmods * 2 : 16;
            shs_mod **grown = (shs_mod **)realloc(g_modv, (size_t)cap * sizeof *grown);
            if (!grown) { ReleaseSRWLockExclusive(&g_lock); free(m); return -1; }
            g_modv = grown;
            g_cmods = cap;
        }
        m->hook = -1;
        g_modv[g_nmods] = m;
        i = g_nmods++;
        ReleaseSRWLockExclusive(&g_lock);
        return i;
    }
}

static void start_all(void)
{
    int i;
    for (i = 0; i < g_nmods && !g_end_pending; i++)
        if ((*g_modv[i]).in_map && (*g_modv[i]).prepared) open_mod(&(*g_modv[i]));
    g_started = 1;
}

int sh_script_map_prepare(const sh_script_mod_desc *mods, int n, const sh_script_source *source,
                          uint64_t seed)
{
    int i, prepared = 0;
    if (!g_qpf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = f.QuadPart ? f.QuadPart : 1; }
    if (!enter_run()) return 0;
    end_all(1);
    sh_script_hooks_mods_reset();
    sh_script_handles_reset();
    g_now = 0;
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
        m = &(*g_modv[slot]);
        if (m->in_map) continue;                 /* listed twice */
        AcquireSRWLockExclusive(&g_lock);
        log_free(m);
        free(m->stop_line);
        free(m->name);
        free(m->last_src);
        free(m->err_file);
        memset(m, 0, sizeof *m);
        copyz(m->salt, sizeof m->salt, salt);
        m->name = _strdup(mods[i].name && mods[i].name[0] ? mods[i].name : salt);
        if (!m->name) { m->salt[0] = 0; ReleaseSRWLockExclusive(&g_lock); continue; }
        m->in_map = 1;
        m->stop = -1;
        m->hook = -1;
        m->seed = seed;
        ReleaseSRWLockExclusive(&g_lock);
        if (prepare_mod(m)) prepared++;
    }
    g_active = 1;
    leave_run();
    return prepared;
}

int sh_script_map_open(void)
{
    sh_script_mod_desc *d = NULL;
    const sh_script_source *source = NULL;
    uint64_t seed = 0;
    int n, cap = 32, r;
    if (!g_provider) return 0;
    for (;;) {
        sh_script_mod_desc *grown = (sh_script_mod_desc *)realloc(d, (size_t)cap * sizeof *d);
        if (!grown) { free(d); return -1; }
        d = grown;
        memset(d, 0, (size_t)cap * sizeof *d);
        n = g_provider(g_provider_ctx, d, cap, &source, &seed);
        if (n <= cap) break;
        cap = n;                                  /* the provider counts every mod: ask again with room */
    }
    r = n < 0 ? -1 : sh_script_map_prepare(d, n, source, seed);
    free(d);
    return r;
}

int sh_script_map_start(const sh_script_mod_desc *mods, int n, const sh_script_source *source,
                        uint64_t seed, double now)
{
    int i, running = 0;
    sh_script_map_prepare(mods, n, source, seed);
    if (!enter_run()) return 0;
    g_now = now;
    start_all();
    leave_run();
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmods; i++) if ((*g_modv[i]).in_map && (*g_modv[i]).status == SH_SCRIPT_RUNNING) running++;
    ReleaseSRWLockShared(&g_lock);
    return running;
}

int sh_script_active(void)
{
    int i, any = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmods; i++)
        if ((*g_modv[i]).in_map && ((*g_modv[i]).status == SH_SCRIPT_RUNNING || (*g_modv[i]).prepared)) any = 1;
    ReleaseSRWLockShared(&g_lock);
    return g_active && any;
}

int sh_script_started(void)
{
    return g_started;
}

static void end_all(int run_end_handlers)
{
    int i;
    for (i = 0; i < g_nmods; i++) {
        shs_mod *m = &(*g_modv[i]);
        if (m->status == SH_SCRIPT_RUNNING && m->L && run_end_handlers && g_started) {
            if (run_job(m, job_list, (void *)"map_end")) close_mod(m, SH_SCRIPT_ENDED);
        }
        if (m->L) close_mod(m, m->status == SH_SCRIPT_STOPPED ? SH_SCRIPT_STOPPED : SH_SCRIPT_ENDED);
        else if (m->prepared) close_mod(m, m->status);   /* prepared, never started: nothing ran */
        free_files(m);
        AcquireSRWLockExclusive(&g_lock);
        m->in_map = 0;
        m->hook = -1;
        ReleaseSRWLockExclusive(&g_lock);
    }
    g_active = 0;
    g_started = 0;
    for (i = 0; i < g_nclosers; i++) g_closers[i]();
}

void sh_script_map_close(int run_end_handlers)
{
    if (g_run_tid == GetCurrentThreadId()) { g_end_pending = 1; g_end_handlers = run_end_handlers; return; }
    if (!enter_run()) return;
    end_all(run_end_handlers);
    leave_run();
}

void sh_script_map_end(void)
{
    sh_script_map_close(1);
}

/* ------------------------------------------------------------ bindings */

int sh_script_binds(const char *full_name, char *salt_out, size_t cap, unsigned *kinds)
{
    int i, j, found = 0;
    if (!full_name) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmods && !found; i++) {
        shs_mod *m = &(*g_modv[i]);
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

/* 1 when the mod binds any name of the (lower-case) chain. */
static int chain_bound(shs_mod *m, const char *chain)
{
    const char *p = chain;
    int bound = 0;
    AcquireSRWLockShared(&g_lock);
    while (p && *p && !bound) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int j;
        for (j = 0; j < m->nbinds && !bound; j++)
            if (strlen(m->binds[j].name) == n && _strnicmp(m->binds[j].name, p, n) == 0) bound = 1;
        p = e ? e + 1 : NULL;
    }
    ReleaseSRWLockShared(&g_lock);
    return bound;
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
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n) {
            lua_pushlstring(L, p, n);              /* the name, whatever its length */
            if (push_handlers(L, kind, lua_tostring(L, -1))) call_list(L, lua_gettop(L), args, nargs);
            lua_pop(L, 2);
        }
        p = e ? e + 1 : NULL;
    }
}

static void push_vec3(lua_State *L, const float *v)
{
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v[0]); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v[1]); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v[2]); lua_setfield(L, -2, "z");
}

static void push_hit(lua_State *L, const sh_script_event *ev)
{
    lua_createtable(L, 0, 6);
    sh_script_push_entity(L, ev->other);
    lua_setfield(L, -2, "attacker");
    sh_script_push_entity(L, ev->third);
    lua_setfield(L, -2, "inflictor");
    lua_pushnumber(L, (lua_Number)ev->amount);
    lua_setfield(L, -2, "amount");
    lua_pushstring(L, ev->text);
    lua_setfield(L, -2, "damage");
    lua_pushstring(L, ev->text2);
    lua_setfield(L, -2, "location");
    if (ev->flags & SH_SCRIPT_EVF_VEC) {
        push_vec3(L, ev->vec);
        lua_setfield(L, -2, "direction");
    }
}

/* An ae_notify text "<command>:<a>:<b>" as frame-event arguments: the
 * pieces after the command, each a number when it reads as one, else the
 * text as written in the row. Pushes the table. */
static void push_frame_args(lua_State *L, const char *text)
{
    const char *p = strchr(text, ':');
    lua_Integer i = 0;
    lua_newtable(L);
    while (p) {
        const char *q = strchr(p + 1, ':');
        size_t n = q ? (size_t)(q - p - 1) : strlen(p + 1);
        lua_pushlstring(L, p + 1, n);
        if (n && lua_stringtonumber(L, lua_tostring(L, -1))) lua_remove(L, -2);
        lua_rawseti(L, -2, ++i);
        p = q;
    }
}

/* Call H[kind][name][key] for each name of the chain (anim, frame_event)
 * with the values at args..args+nargs-1. */
static void call_chain_keyed(lua_State *L, const char *kind, const char *chain, const char *key, int args, int nargs)
{
    const char *p = chain;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n) {
            lua_pushlstring(L, p, n);              /* the name, whatever its length */
            if (push_handlers(L, kind, lua_tostring(L, -1))) {
                if (lua_getfield(L, -1, key) == LUA_TTABLE) call_list(L, lua_gettop(L), args, nargs);
                lua_pop(L, 1);
            }
            lua_pop(L, 2);
        }
        p = e ? e + 1 : NULL;
    }
}

static void input_missed(lua_State *L, shs_mod *m, const char *node)
{
    luaL_Buffer list;
    int count = 0, top = lua_gettop(L);
    push_handlers(L, "missed", NULL);
    if (lua_getfield(L, -1, node) != LUA_TNIL) { lua_settop(L, top); return; }
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, node);
    lua_pop(L, 1);
    luaL_buffinit(L, &list);
    if (push_handlers(L, "input", NULL)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                lua_pop(L, 1);
                if (count++) luaL_addstring(&list, ", ");
                luaL_addstring(&list, k);
            } else {
                lua_pop(L, 1);
            }
        }
    }
    lua_pop(L, 1);
    luaL_pushresult(&list);
    if (count) lua_pushfstring(L, "Script node \"%s\" (%s): no script handles it. The script handles: %s.", node, m->name,
                               lua_tostring(L, -1));
    else lua_pushfstring(L, "Script node \"%s\" (%s): no script handles it. The scripts handle no Script node.", node, m->name);
    shs_log(m, lua_tostring(L, -1));
    lua_settop(L, top);
}

/* Job: deliver one queued record. */
static int job_event(lua_State *L)
{
    const sh_script_event *ev = (const sh_script_event *)lua_touserdata(L, 1);
    shs_mod *m = shs_mod_of(L);
    int base = lua_gettop(L), at;
    if (ev->kind == SH_SCRIPT_EV_INPUT) {
        const char *node = push_lower(L, ev->text);
        sh_script_push_entity(L, ev->self);
        sh_script_push_entity(L, ev->other);
        at = lua_gettop(L) - 1;
        if (push_handlers(L, "input", node)) call_list(L, lua_gettop(L), at, 2);
        else { lua_pop(L, 1); input_missed(L, m, node); }
        lua_settop(L, base);
        return 0;
    }
    if (ev->kind == SH_SCRIPT_EV_SIGNAL) {
        const char *event = push_lower(L, ev->text);
        sh_script_push_entity(L, ev->self);
        sh_script_push_entity(L, ev->other);
        at = lua_gettop(L) - 1;
        if (push_handlers(L, "signal", event)) call_list(L, lua_gettop(L), at, 2);
        lua_settop(L, base);
        return 0;
    }
    {
        int i = find_ent(m, ev->self);
        char *chain;
        if (i < 0) return 0;                       /* not driven, or removed before the record was delivered */
        chain = _strdup(m->ents[i].chain);
        if (!chain) return luaL_error(L, "not enough memory");
        sh_script_push_entity(L, ev->self);
        at = lua_gettop(L);
        if (ev->kind == SH_SCRIPT_EV_DAMAGE || ev->kind == SH_SCRIPT_EV_DEATH) {
            push_hit(L, ev);
            call_chain(L, ev->kind == SH_SCRIPT_EV_DAMAGE ? "damage" : "death", chain, at, 2);
        } else if (ev->kind == SH_SCRIPT_EV_ANIM) {
            const char *notify = push_lower(L, ev->text), *colon;
            const char *command;
            int self_at;
            call_chain_keyed(L, "anim", chain, notify, at, 1);
            /* a frame event: "<command>:<args>", the command matched without case */
            colon = strchr(notify, ':');
            lua_pushlstring(L, notify, colon ? (size_t)(colon - notify) : strlen(notify));
            command = lua_tostring(L, -1);
            if (command[0]) {
                lua_pushvalue(L, at);                  /* self and the arguments, side by side */
                self_at = lua_gettop(L);
                push_frame_args(L, ev->text);
                call_chain_keyed(L, "frame_event", chain, command, self_at, 2);
            }
        } else if (ev->kind == SH_SCRIPT_EV_PAIN) {
            lua_pushstring(L, ev->text);
            if (ev->text2 && ev->text2[0]) lua_pushstring(L, ev->text2); else lua_pushnil(L);
            sh_script_push_entity(L, ev->other);
            call_chain(L, "pain", chain, at, 4);
        } else if (ev->kind == SH_SCRIPT_EV_PROJECTILE) {
            sh_script_push_entity(L, ev->other);
            lua_pushstring(L, ev->text);
            if (ev->flags & SH_SCRIPT_EVF_VEC) { push_vec3(L, ev->vec); push_vec3(L, ev->vec + 3); }
            else { lua_pushnil(L); lua_pushnil(L); }
            call_chain(L, "projectile", chain, at, 5);
        }
        free(chain);
        lua_settop(L, base);
        return 0;
    }
}

typedef struct { uint64_t handle; const char *chain; } shs_ent_job;

/* Job: an entity the handle table reported spawned and the mod binds. */
static int job_spawn(lua_State *L)
{
    shs_ent_job *j = (shs_ent_job *)lua_touserdata(L, 1);
    shs_mod *m = shs_mod_of(L);
    int base = lua_gettop(L);
    if (find_ent(m, j->handle) >= 0) return 0;
    if (m->nents == m->cents) {
        int c = m->cents ? m->cents * 2 : 32;
        shs_ent *p = (shs_ent *)realloc(m->ents, (size_t)c * sizeof *p);
        if (!p) return luaL_error(L, "not enough memory");
        m->ents = p;
        m->cents = c;
    }
    m->ents[m->nents].handle = j->handle;
    m->ents[m->nents].chain = _strdup(j->chain);
    if (!m->ents[m->nents].chain) return luaL_error(L, "not enough memory");
    m->nents++;
    lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_LIVE);
    sh_script_push_entity(L, j->handle);
    lua_rawseti(L, -2, (lua_Integer)j->handle);
    sh_script_push_entity(L, j->handle);
    call_chain(L, "spawn", j->chain, lua_gettop(L), 1);
    lua_settop(L, base);
    return 0;
}

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

static void spawn_one(shs_mod *m, sh_script_handle h)
{
    char *chain;
    shs_ent_job j;
    int n = sh_script_handle_names(h, NULL, 0);
    if (n <= 0 || !(chain = (char *)malloc((size_t)n + 1))) return;
    sh_script_handle_names(h, chain, (size_t)n + 1);   /* the chain whole */
    lower(chain);
    if (chain_bound(m, chain)) {                   /* else none of its names is bound: not driven */
        j.handle = h;
        j.chain = chain;
        run_job(m, job_spawn, &j);
    }
    free(chain);
}

static void remove_one(shs_mod *m, sh_script_handle h)
{
    shs_ent_job j;
    char *chain;
    int i = find_ent(m, h);
    if (i < 0) return;
    chain = m->ents[i].chain;
    j.handle = h;
    j.chain = chain;
    if (!run_job(m, job_remove, &j)) return;      /* the mod stopped: its table is gone */
    i = find_ent(m, h);
    if (i < 0) return;
    memmove(&m->ents[i], &m->ents[i + 1], (size_t)(m->nents - i - 1) * sizeof m->ents[0]);
    m->nents--;
    free(chain);
}

/* A record the handle table released (a removed entity, or a reference to a
 * player, an attacker or a projectile that is gone): every state drops its
 * object for that handle, so the objects a state keeps are the entities alive
 * in the map. A script still holding the object keeps it; its calls answer
 * nil. */
static void forget_object(sh_script_handle h)
{
    int i;
    for (i = 0; i < g_nmods; i++) {
        lua_State *L = g_modv[i]->L;
        if (!g_modv[i]->in_map || g_modv[i]->status != SH_SCRIPT_RUNNING || !L) continue;
        lua_getfield(L, LUA_REGISTRYINDEX, SHS_REG_ENTS);
        lua_pushnil(L);
        lua_rawseti(L, -2, (lua_Integer)h);
        lua_pop(L, 1);
    }
}

#define SPAWN_FNS 4
static void (*g_spawn_fns[SPAWN_FNS])(void *ent);

void sh_script_api_on_spawn(void (*fn)(void *ent))
{
    int i;
    for (i = 0; i < SPAWN_FNS; i++) {
        if (g_spawn_fns[i] == fn) return;
        if (!g_spawn_fns[i]) { g_spawn_fns[i] = fn; return; }
    }
}

static void spawned(sh_script_handle h)
{
    void *ent = sh_script_handle_entity(h);
    int i;
    if (!ent) return;
    for (i = 0; i < SPAWN_FNS; i++) if (g_spawn_fns[i]) g_spawn_fns[i](ent);
}

/* The handle table's spawns and removals since the last frame. */
static void deliver_changes(void)
{
    enum { BATCH = 128 };
    sh_script_handle_change ch[BATCH];
    int n, i;
    do {
        n = sh_script_handles_sweep(ch, BATCH);
        for (i = 0; i < n; i++) {
            shs_mod *m;
            m = ch[i].kind == SH_SCRIPT_HANDLE_GONE ? NULL : mod_by_hook(ch[i].mod);
            if (m && m->status == SH_SCRIPT_RUNNING && m->L) {
                if (ch[i].kind == SH_SCRIPT_HANDLE_SPAWNED) {
                    spawned(ch[i].handle);
                    spawn_one(m, ch[i].handle);
                }
                else remove_one(m, ch[i].handle);
                if (g_end_pending) return;
            }
            if (ch[i].kind != SH_SCRIPT_HANDLE_SPAWNED) forget_object(ch[i].handle);
        }
    } while (n == BATCH);
}

/* Deliver what is queued now; 0 when the mod stopped. */
static int drain(shs_mod *m)
{
    int n = m->hook >= 0 ? sh_script_queue_pending(m->hook) : 0, k;
    sh_script_event ev;
    for (k = 0; k < n; k++) {
        int ran;
        if (sh_script_queue_drain(m->hook, &ev, 1) != 1) break;
        ran = run_job(m, job_event, &ev);
        sh_script_event_release(&ev);
        if (!ran || g_end_pending) return 0;
    }
    return 1;
}

static int queued(shs_mod *m)
{
    return m->hook >= 0 && sh_script_queue_pending(m->hook) > 0;
}

static void report_drops(shs_mod *m)
{
    char line[200];
    int first = 0;
    long n = m->hook >= 0 ? sh_script_queue_take_dropped(m->hook, &first) : 0;
    if (!n || !first) return;
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "Some game events were not given to this mod's scripts because there was not enough memory to hold them.");
    line[sizeof line - 1] = 0;
    shs_log(m, line);
}

static void tick_mod(shs_mod *m)
{
    int i, pass;
    report_drops(m);
    if (!drain(m)) return;
    for (i = 0; i < m->nents; i++) {
        shs_ent_job j;
        j.handle = m->ents[i].handle;
        j.chain = m->ents[i].chain;
        if (!run_job(m, job_think, &j)) return;
        if (g_end_pending) return;
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
    sh_script_queue_set_drain_thread(GetCurrentThreadId());
    g_now = now;
    g_dt = dt;
    if (!g_started) start_all();
    if (!g_end_pending) deliver_changes();
    for (i = 0; i < g_nmods && !g_end_pending; i++)
        if ((*g_modv[i]).in_map && (*g_modv[i]).status == SH_SCRIPT_RUNNING && (*g_modv[i]).L) tick_mod(&(*g_modv[i]));
    leave_run();
}

int sh_script_fsm_call(const char *salt, sh_script_fsm_request *rq)
{
    shs_mod *m = NULL;
    int i, ok = 0;
    if (!salt || !rq || !rq->name || rq->kind < 0 || rq->kind >= SH_FSM_KINDS) return 0;
    /* A script call that made the engine ask this class: Lua cannot be entered again. */
    if (!enter_run()) return 0;
    for (i = 0; i < g_nmods; i++) {
        shs_mod *c = &(*g_modv[i]);
        if (c->in_map && c->status == SH_SCRIPT_RUNNING && c->L && !_stricmp(c->salt, salt)) { m = c; break; }
    }
    if (m) ok = run_job(m, fsm_job, rq);
    leave_run();
    return ok && rq->found;
}

int shs_run_job(shs_mod *m, lua_CFunction job, void *ud) { return run_job(m, job, ud); }
void shs_call_handler(lua_State *L, int nargs) { call_handler(L, nargs); }

/* ------------------------------------------------------------ queries */

int sh_script_status(const char *salt)
{
    int i, st = SH_SCRIPT_NONE;
    if (!salt) return st;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmods; i++) if ((*g_modv[i]).salt[0] && _stricmp((*g_modv[i]).salt, salt) == 0) st = (*g_modv[i]).status;
    ReleaseSRWLockShared(&g_lock);
    return st;
}

char *sh_script_stop_line_dup(const char *salt)
{
    int i;
    char *line = NULL;
    if (!salt) return NULL;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmods; i++)
        if ((*g_modv[i]).salt[0] && _stricmp((*g_modv[i]).salt, salt) == 0 && (*g_modv[i]).stop_line) {
            free(line);
            line = _strdup((*g_modv[i]).stop_line);
        }
    ReleaseSRWLockShared(&g_lock);
    return line;
}

int sh_script_log_read(const char *salt, sh_script_line_fn fn, void *context)
{
    int i, k, n = 0;
    if (!salt) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmods; i++)
        if ((*g_modv[i]).salt[0] && _stricmp((*g_modv[i]).salt, salt) == 0 && (*g_modv[i]).log) {
            for (k = 0; k < (*g_modv[i]).nlog; k++) { if (fn) fn(context, (*g_modv[i]).log[k]); n++; }
        }
    ReleaseSRWLockShared(&g_lock);
    return n;
}

int sh_script_mods(sh_script_mod_fn fn, void *context)
{
    int i, n = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmods; i++)
        if ((*g_modv[i]).salt[0] && ((*g_modv[i]).log || (*g_modv[i]).status != SH_SCRIPT_NONE || (*g_modv[i]).prepared)) {
            if (fn) fn(context, (*g_modv[i]).salt, (*g_modv[i]).name);
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
    for (i = 0; i < g_nmods; i++) {
        shs_mod *m = &(*g_modv[i]);
        total += m->mem;
        total += (size_t)m->cents * sizeof *m->ents + (size_t)m->cbinds * sizeof *m->binds +
                 (size_t)m->ctimers * sizeof *m->timers + (size_t)m->nfiles * sizeof *m->files;
        for (k = 0; k < m->nents; k++) total += strlen(m->ents[k].chain) + 1;
    }
    ReleaseSRWLockShared(&g_lock);
    return total;
}

void sh_script_mod_info_free(sh_script_mod_info *info)
{
    if (!info) return;
    free(info->name);
    info->name = NULL;
}

int sh_script_mod_info_get(const char *salt, sh_script_mod_info *out)
{
    int i, found = 0;
    if (out) memset(out, 0, sizeof *out);
    if (!salt || !out) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmods && !found; i++) {
        const shs_mod *m = &(*g_modv[i]);
        if (!m->salt[0] || _stricmp(m->salt, salt) != 0) continue;
        if (!m->log && m->status == SH_SCRIPT_NONE && !m->prepared) continue;
        copyz(out->salt, sizeof out->salt, m->salt);
        out->name = _strdup(m->name);
        out->status = m->status;
        out->waiting = m->prepared && !m->L;
        out->files = m->files_found;
        out->files_run = m->files_run;
        out->handlers = m->handlers;
        out->entities = m->nents;
        out->log_lines = m->nlog;
        out->stopped_line = m->stop_line != NULL;
        found = 1;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/* Every .lua the build carries under scripts/, lib/ included. */
static void list_all_cb(void *ctx, const char *relpath)
{
    shs_list *l = (shs_list *)ctx;
    size_t n = relpath ? strlen(relpath) : 0;
    l->total++;
    if (n < 5 || _stricmp(relpath + n - 4, ".lua") != 0) return;
    list_keep(l, relpath);
}

static void check_line(sh_script_line_fn fn, void *context, const char *mod, const char *file, int line,
                       const char *what, const char *todo)
{
    char *text;
    size_t cap;
    if (!fn) return;
    cap = strlen(mod) + strlen(file) + strlen(what) + (todo ? strlen(todo) : 0) + 48;
    if (!(text = (char *)malloc(cap))) return;
    if (line > 0)
        _snprintf_s(text, cap, _TRUNCATE, "%s, %s line %d: %s.%s%s%s", mod, file, line, what,
                    todo && todo[0] ? " " : "", todo ? todo : "", todo && todo[0] ? "." : "");
    else
        _snprintf_s(text, cap, _TRUNCATE, "%s, %s: %s.%s%s%s", mod, file, what,
                    todo && todo[0] ? " " : "", todo ? todo : "", todo && todo[0] ? "." : "");
    fn(context, text);
    free(text);
}

int sh_script_check_files(const char *salt, const char *name, const sh_script_source *source,
                          sh_script_line_fn fn, void *context)
{
    shs_list files;
    shs_mod *tmp;
    lua_State *L;
    int i, ok = 0, n;
    const char *mod = name && name[0] ? name : (salt ? salt : "");
    if (!salt || !source || !source->list || !source->read) return -1;
    memset(&files, 0, sizeof files);
    n = source->list(source->context, salt, list_all_cb, &files);
    if (n < 0) { list_free(&files); return -1; }
    qsort(files.names, (size_t)files.n, sizeof *files.names, name_cmp);
    tmp = (shs_mod *)calloc(1, sizeof *tmp);
    L = tmp ? lua_newstate(shs_alloc, tmp) : NULL;
    if (!L) { free(tmp); list_free(&files); return -1; }
    tmp->stop = -1;
    for (i = 0; i < files.n; i++) {
        char *text = NULL, *chunk, *file;
        size_t len = 0, k;
        long version = 0;
        int line = 1, r, status;
        const char *t;
        if (!(chunk = sh_textf("=scripts\\%s", files.names[i]))) continue;
        for (k = 0; chunk[k]; k++) if (chunk[k] == '/') chunk[k] = '\\';
        file = chunk + 1;
        r = source->read(source->context, salt, files.names[i], &text, &len);
        if (r <= 0) {
            check_line(fn, context, mod, file, 0, "the file could not be read", "Press Refresh in the Mods tab");
            free(chunk);
            continue;
        }
        t = text;
        if (len >= 4 && memcmp(text, "\x1bLua", 4) == 0) {
            check_line(fn, context, mod, file, 0, "the file is compiled Lua, not script text", "Put the .lua source text in the file");
        } else if (!strchr(files.names[i], '/') && !sh_script_api_declared(text, len, &version, &line)) {
            check_line(fn, context, mod, file, line, "the file does not start with \"api = 1\"", "Add \"api = 1\" as its first line");
        } else {
            size_t m = len;
            if (m >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) { t += 3; m -= 3; }
            status = luaL_loadbufferx(L, t, m, chunk, "t");
            if (status == LUA_OK) ok++;
            else {
                char *what = NULL, *todo = NULL, *where = NULL;
                int ln = 0;
                const char *msg = lua_tostring(L, -1);
                const char *rest = sh_script_split_where(msg ? msg : "", &where, &ln);
                sh_script_reason(rest, &what, &todo);
                check_line(fn, context, mod, file, ln, what ? what : "the file does not compile", todo);
                free(what);
                free(todo);
                free(where);
            }
            lua_settop(L, 0);
        }
        if (source->release) source->release(source->context, text);
        free(chunk);
    }
    lua_close(L);
    free(tmp);
    list_free(&files);
    return ok;
}
