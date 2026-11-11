/* script_runtime.h -- mod scripts: one sandboxed Lua 5.4 state per scripted
 * mod per map, the queue every engine hook writes into, and the one tick that
 * delivers it.
 *
 * A script is a carried text file of a mod, assets\mods\<salt>\scripts\*.lua
 * (entry files) and scripts\lib\*.lua (loaded only through require). The
 * runtime reads them through the serving layer (sh_script_source below), runs
 * every entry file once at map start in byte-wise name order, then the
 * on.map_start handlers. After that Lua runs only inside sh_script_tick, on
 * the thread that calls it, never re-entrantly: hooks on any thread call
 * sh_script_enqueue, and the tick delivers the records.
 *
 * The sandbox (script_lib_base.c): base (no dofile, loadfile, load, require,
 * collectgarbage except "count"; print and warn write the mod's log), string
 * (no dump; find, match, gmatch and gsub refuse a subject or pattern longer
 * than SH_SCRIPT_PATTERN_CAP bytes and every pattern step is counted), table,
 * math (no randomseed; random seeded from the map seed), utf8, coroutine. No
 * io, os, debug, package. Chunks load as text only (mode "t"). The product's
 * require(name) loads scripts\lib\<name>.lua of the same mod, once.
 *
 * Bounds, per mod: SH_SCRIPT_MEM_CAP bytes through the state's allocator;
 * SH_SCRIPT_CALL_STEPS steps per delivery (one handler call, one timer, one
 * coroutine resume) and SH_SCRIPT_TICK_STEPS per tick, counted by the count
 * hook every SH_SCRIPT_HOOK_STRIDE VM instructions plus one step per
 * SH_SCRIPT_ALLOC_STEP bytes allocated and the work of C library calls the
 * hook cannot see (sorting, pattern steps), so the count is deterministic. A
 * delivery that still runs SH_SCRIPT_WALL_MS of wall time (an instruction
 * comparing very long strings) is stopped as well. A script's pcall,
 * xpcall, coroutine.resume and coroutine.close cannot catch a stop.
 *
 * Errors: any error, an exhausted budget, the memory cap or a refused call
 * stops that mod's scripts for the rest of the map (its state is closed, its
 * bindings withdrawn, its queue refused) and writes one line:
 *   Script stopped: <mod name>, scripts\<file>.lua line <n>: <what>. <what to do>.
 * The line, prints and warnings are the mod's script log (SH_SCRIPT_LOG_LINES
 * per map), kept until the mod's next map start and read by the UI through
 * sh_script_log_read and sh_script_stop_line. Other mods keep running.
 *
 * Threads: sh_script_map_start, sh_script_tick and sh_script_map_end run Lua
 * and are serialized by one lock (a call made while Lua runs on the same
 * thread is deferred: map end runs when the running call returns, a nested
 * tick is skipped). sh_script_enqueue, sh_script_binds, sh_script_entity_owner
 * and the log queries may be called from any thread.
 *
 * Co-op: the caller (the tick detour, the hooks) runs none of this on a client.
 *
 * SH_SCRIPT_PURE builds without the backend log (the tests). */
#ifndef SH_SCRIPT_RUNTIME_H
#define SH_SCRIPT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "../../third_party/lua/lua.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SH_SCRIPT_API_VERSION   1
#define SH_SCRIPT_MAX_MODS      64
#define SH_SCRIPT_SALT_CAP      16
#define SH_SCRIPT_MODNAME_CAP   96
#define SH_SCRIPT_DEFNAME_CAP   192        /* a full entityDef name, mods/<salt>/... */
#define SH_SCRIPT_LINE_CAP      512        /* one log line, truncated past this */

#define SH_SCRIPT_MEM_CAP       (32u << 20)
#define SH_SCRIPT_MEM_RESERVE   (256u << 10) /* lent to a stopping state to write its error */
#define SH_SCRIPT_CALL_STEPS    2000000
#define SH_SCRIPT_TICK_STEPS    5000000
#define SH_SCRIPT_HOOK_STRIDE   1000
#define SH_SCRIPT_ALLOC_STEP    256        /* bytes allocated per counted step */
#define SH_SCRIPT_PATTERN_CAP   4096       /* subject and pattern bytes */
#define SH_SCRIPT_PATTERN_STEPS 1000000    /* pattern steps per delivery */
#define SH_SCRIPT_WALL_MS       1000       /* wall time per delivery */
#define SH_SCRIPT_QUEUE_CAP     1024       /* records per mod */
#define SH_SCRIPT_PASSES        4          /* queue passes per tick */
#define SH_SCRIPT_LOG_LINES     200
#define SH_SCRIPT_FILE_MAX      (256u << 10)
#define SH_SCRIPT_MOD_TEXT_MAX  (1u << 20)
#define SH_SCRIPT_MOD_FILES_MAX 64
#define SH_SCRIPT_TIMERS_MAX    4096       /* live timers and coroutines per mod */

/* ------------------------------------------------------------ file access */

/* How the runtime reads a mod's scripts. The serving layer supplies it: list
 * names every file the serving build of `salt` carries under
 * mods/<salt>/scripts/ (the map's own place first, else the root, as every
 * served file), and read returns one through the serving index
 * (sh_modserve_open of mods/<salt>/scripts/<relpath>). relpath is relative to
 * scripts/, '/'-separated, as "door.lua" or "lib/fsm.lua".
 *   list: calls fn once per file; returns the count, or -1 when the build
 *         could not be read.
 *   read: 1 with the bytes in *text (released with release), 0 when the build
 *         carries no such file, -1 when it could not be read.
 * The runtime calls these only from sh_script_map_start and from require. */
typedef void (*sh_script_file_fn)(void *fn_context, const char *relpath);
typedef struct sh_script_source {
    void *context;
    int  (*list)(void *context, const char *salt, sh_script_file_fn fn, void *fn_context);
    int  (*read)(void *context, const char *salt, const char *relpath, char **text, size_t *len);
    void (*release)(void *context, char *text);
} sh_script_source;

/* --------------------------------------------------------------- the map */

typedef struct sh_script_mod_desc {
    const char *salt;        /* the mod's identity, mods/<salt>/ */
    const char *name;        /* the author's display name, for the log and the stop line */
} sh_script_mod_desc;

/* Start scripts for a map: close anything left from the last map, then for
 * each mod open a state, read and run its entry files and its on.map_start
 * handlers. A mod whose build carries no entry file gets no state. `seed`
 * seeds math.random; `now` is the game time in seconds that now() returns
 * until the first tick. Returns the number of mods whose scripts are running. */
int sh_script_map_start(const sh_script_mod_desc *mods, int n, const sh_script_source *source,
                        uint64_t seed, double now);

/* One game frame: for every running mod, deliver its queue, then think(self,
 * dt) for every bound live entity, then due timers and coroutines, then what
 * those raised, up to SH_SCRIPT_PASSES queue passes; what is left waits for
 * the next tick. An entity that the entity ops report gone gets remove(self)
 * once. `now` is the game time in seconds, `dt` the frame time. */
void sh_script_tick(double now, double dt);

/* End of map: run each running mod's on.map_end handlers, then close every
 * state and free every queue, timer and binding. The logs stay readable. */
void sh_script_map_end(void);

/* 1 between a map start that opened at least one state and its map end. */
int sh_script_active(void);

/* ---------------------------------------------------------------- events */

#define SH_SCRIPT_EV_SPAWN   1   /* entity: spawned; names: its entityDef chain */
#define SH_SCRIPT_EV_DAMAGE  2   /* entity, other = attacker, inflictor, amount, text = damage decl, location */
#define SH_SCRIPT_EV_DEATH   3   /* entity, other = attacker, inflictor, text = damage decl */
#define SH_SCRIPT_EV_ANIM    4   /* entity, text = the ae_notify string */
#define SH_SCRIPT_EV_INPUT   5   /* entity = the Script node, other = activator, names = the node's name */

#define SH_SCRIPT_NAMES_CAP  512
#define SH_SCRIPT_TEXT_CAP   128
#define SH_SCRIPT_LOC_CAP    64

/* One queued record. Entities are opaque handles (0 is none); the runtime
 * never dereferences them. For SPAWN, names holds the entity's entityDef and
 * the names it inherits, most derived first, separated by '\n'. */
typedef struct sh_script_event {
    int      kind;
    uint64_t entity;
    uint64_t other;
    uint64_t inflictor;
    double   amount;
    char     names[SH_SCRIPT_NAMES_CAP];
    char     text[SH_SCRIPT_TEXT_CAP];
    char     location[SH_SCRIPT_LOC_CAP];
} sh_script_event;

/* Queue `ev` for the scripted mod `salt`. Any thread; copies the record. A
 * SPAWN makes the entity known to sh_script_entity_owner at once. Returns 1
 * queued, 0 refused: no running mod by that salt, a kind out of range, or the
 * queue full (counted; one log line per map). */
int sh_script_enqueue(const char *salt, const sh_script_event *ev);

/* What the running mods bind (for the spawn subscriber, which walks an
 * entity's inherit chain): 1 when a running mod registered a handler for the
 * full entityDef name, with that mod's salt in salt_out and the kinds bound
 * (SH_SCRIPT_BIND_ bits) in *kinds. Any thread. */
#define SH_SCRIPT_BIND_SPAWN  0x01u
#define SH_SCRIPT_BIND_THINK  0x02u
#define SH_SCRIPT_BIND_REMOVE 0x04u
#define SH_SCRIPT_BIND_DAMAGE 0x08u
#define SH_SCRIPT_BIND_DEATH  0x10u
#define SH_SCRIPT_BIND_ANIM   0x20u
int sh_script_binds(const char *full_name, char *salt_out, size_t cap, unsigned *kinds);

/* 1 when `handle` is an entity a running mod's scripts are bound to (queued
 * or delivered SPAWN, not yet removed), with its salt in salt_out. Any thread. */
int sh_script_entity_owner(uint64_t handle, char *salt_out, size_t cap);

/* --------------------------------------------------------------- engine */

/* The engine questions the tick asks. alive: 1 while `handle` still resolves
 * to a live entity. NULL ops (the default) report every entity alive. */
typedef struct sh_script_entity_ops {
    void *context;
    int (*alive)(void *context, uint64_t handle);
} sh_script_entity_ops;
void sh_script_set_entity_ops(const sh_script_entity_ops *ops);

/* Where log lines also go (the console); NULL for none. Lines arrive on the
 * thread that ran the script. */
typedef void (*sh_script_line_fn)(void *context, const char *line);
void sh_script_set_console(sh_script_line_fn fn, void *context);

/* ------------------------------------------------------------- API seam */

/* Functions other modules add to every state opened after the call: a
 * global (spawn, players, ...) or an entity method (e:pos(), self:move_to).
 * Call before sh_script_map_start, from one thread. Returns 1, or 0 when the
 * table is full or the name is not usable. */
#define SH_SCRIPT_API_GLOBAL 1
#define SH_SCRIPT_API_METHOD 2
int sh_script_api_add(int kind, const char *name, lua_CFunction fn);

/* The mod a state (or any of its coroutines) belongs to. */
const char *sh_script_salt(lua_State *L);
const char *sh_script_mod_name(lua_State *L);

/* Push the Lua object for an entity handle: the same object for the same
 * handle within one state, carrying its own e.state table. 0 pushes nil. */
void sh_script_push_entity(lua_State *L, uint64_t handle);

/* The handle of the entity object at `arg`, or 0 when it was removed (calls
 * on a removed entity return nil). Raises a plain error when the value is not
 * an entity; `call` names the API call for that sentence. */
uint64_t sh_script_check_entity(lua_State *L, int arg, const char *call);

/* 1 when the entity is one this mod's scripts are bound to (spawned from its
 * entityDefs, or by it). */
int sh_script_owns_entity(lua_State *L, uint64_t handle);

/* Record an entity the script itself spawned as owned by its mod, so
 * sh_script_owns_entity answers 1 before (or without) a SPAWN delivery. */
void sh_script_adopt_entity(lua_State *L, uint64_t handle);

/* The mod-relative naming rule. Resolve the name at `arg` into out:
 * "ai/tyrant" -> "mods/<salt>/ai/tyrant"; "mods/<salt>/..." with this mod's
 * own salt is accepted as written. Any other "mods/..." name is refused. A
 * game("...") reference resolves to the game name when allow_game (then
 * *is_game = 1) and is refused otherwise. Raises a plain error naming `call`
 * on any refusal, before the caller touches the engine. */
void sh_script_check_name(lua_State *L, int arg, const char *call, int allow_game,
                          char *out, size_t cap, int *is_game);

/* Raise a plain error at the calling script line: `fmt` is one sentence the
 * stop line shows as written ("\"spawn\" can only spawn this mod's
 * entities; \"ai/imp\" is not one"). Never returns. */
int sh_script_refuse(lua_State *L, const char *fmt, ...);

/* ------------------------------------------------------------ UI queries */

#define SH_SCRIPT_NONE    0      /* no scripts for this mod in the current or last map */
#define SH_SCRIPT_RUNNING 1
#define SH_SCRIPT_STOPPED 2
#define SH_SCRIPT_ENDED   3      /* the map ended with the scripts running */
int sh_script_status(const char *salt);

/* Copy the stop line of `salt` into out: 1 when its scripts stopped in the
 * current or last map, else 0 (out holds ""). Any thread. */
int sh_script_stop_line(const char *salt, char *out, size_t cap);

/* Call fn for each line of `salt`'s script log, oldest first (prints,
 * warnings, the stop line). Returns the line count. Any thread; fn runs under
 * the log lock and must not call back into the runtime. */
int sh_script_log_read(const char *salt, sh_script_line_fn fn, void *context);

/* Call fn(context, salt, name) for every mod with a log from the current or
 * last map. Returns the count. Any thread. */
typedef void (*sh_script_mod_fn)(void *context, const char *salt, const char *name);
int sh_script_mods(sh_script_mod_fn fn, void *context);

/* ------------------------------------------------------------ for tests */

/* Bytes the runtime holds now: every state's allocator count plus queues,
 * timers, bindings and entity tables (not the kept logs). 0 after
 * sh_script_map_end. */
size_t sh_script_bytes_in_use(void);

/* The limits above, adjustable for a test; sh_script_limits_reset restores them. */
typedef struct sh_script_limits {
    size_t   mem_cap;
    long long call_steps;
    long long tick_steps;
    long long pattern_steps;
    unsigned wall_ms;
} sh_script_limits;
void sh_script_limits_set(const sh_script_limits *limits);
void sh_script_limits_reset(void);

#ifdef __cplusplus
}
#endif
#endif
