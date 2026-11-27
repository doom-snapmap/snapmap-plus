/* script_runtime.h -- mod scripts: one sandboxed Lua 5.4 state per scripted mod per map, the queue every engine
 * hook writes into, and the one tick that delivers it. Scripts are a mod's assets\mods\<salt>\scripts\*.lua
 * entry files and scripts\lib\*.lua (loaded only through require), read through the serving layer. */
/* A map's scripted mods are prepared at the Play gate (sh_script_map_open, no script code runs); the first tick
 * runs every entry file once in byte-wise name order, then the on.map_start handlers. After that Lua runs only
 * inside sh_script_tick, never re-entrantly, while hooks on any thread post to the mod's queue (script_queue.h) and
 * the spawn subscriber records entities in the handle table (script_handles.h). */
/* The sandbox (script_lib_base.c) offers base (no dofile, loadfile, load, require, collectgarbage except "count"),
 * string (no dump), table, math (no randomseed), utf8 and coroutine; no io, os, debug or package. Chunks load as
 * text only, and require(name) loads scripts\lib\<name>.lua of the same mod once. */
/* Memory, work, timers, files, log lines and script size have no limits of their own; the one protection is that
 * a single delivery still running after SH_SCRIPT_WALL_MS of wall time is stopped, and pcall, xpcall and
 * coroutine.resume and close cannot catch that stop. The clock is read every SH_SCRIPT_HOOK_STRIDE VM
 * instructions, every 1024 pattern steps, and before library calls the hook cannot see (sorting, table moves). */
/* Any error, the wall-clock stop, running out of memory or a refused call stops that mod's scripts for the rest of
 * the map (state closed, bindings withdrawn, queue refused) and writes one line:
 *   Script stopped: <mod name>, scripts\<file>.lua line <n>: <what>. <what to do>.
 * That line, prints and warnings form the mod's script log, kept until its next map start. Other mods keep running. */
/* Threads: sh_script_map_start, sh_script_tick and sh_script_map_close run Lua under one lock (a call made while
 * Lua runs on the same thread is deferred: map end waits for the running call, a nested tick is skipped);
 * sh_script_binds, sh_script_entity_owner and the log queries are any-thread. The caller runs none of this on a
 * client. SH_SCRIPT_PURE builds without the backend log. */
#ifndef SH_SCRIPT_RUNTIME_H
#define SH_SCRIPT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "../../third_party/lua/lua.h"
#include "script_queue.h"     /* the records hooks post: SH_SCRIPT_EV_*, sh_script_queue_post */
#include "script_handles.h"   /* entity handles: sh_script_handle */

#ifdef __cplusplus
extern "C" {
#endif

#define SH_SCRIPT_API_VERSION   1
#define SH_SCRIPT_SALT_CAP      16

#define SH_SCRIPT_HOOK_STRIDE   1000       /* VM instructions between clock reads */
#define SH_SCRIPT_WALL_MS       5000       /* wall time one delivery may run before it is stopped */
#define SH_SCRIPT_PASSES        4          /* queue passes per tick; later records wait for the next frame */

/* ------------------------------------------------------------ file access */

/* How the runtime reads a mod's scripts, supplied by the serving layer; relpath is relative to
 * mods/<salt>/scripts/, '/'-separated ("door.lua", "lib/fsm.lua"). Called only from sh_script_map_start and require.
 *   list: calls fn once per file the build of `salt` carries; returns the count, or -1 when unreadable.
 *   read: 1 with the bytes in *text (released with release), 0 when there is no such file, -1 when unreadable. */
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

/* Prepare scripts for a map: end the last map's scripts (with end handlers), empty the hooks' mod table and the
 * handle table, then register each mod with an entry file via sh_script_hooks_mod_add. No script code runs;
 * `seed` seeds math.random. Returns the mods prepared. Main thread. */
int sh_script_map_prepare(const sh_script_mod_desc *mods, int n, const sh_script_source *source,
                          uint64_t seed);

/* Where sh_script_map_open finds the map's mods: fill out[] (up to cap) with the served mods that carry scripts,
 * set *source and *seed, and return the count (-1 when unreadable: nothing runs). The strings live until return. */
typedef int (*sh_script_map_mods_fn)(void *context, sh_script_mod_desc *out, int cap,
                                     const sh_script_source **source, uint64_t *seed);
void sh_script_set_map_provider(sh_script_map_mods_fn fn, void *context);

/* The hooks' Play gate (script_hooks.h): prepare the mods the provider names. Returns the mods prepared, 0 when
 * none carries scripts, -1 when unreadable (logged). Main thread. */
int sh_script_map_open(void);

/* Prepare and start at once, with `now` as the game time (tests). Returns the mods whose scripts are running. */
int sh_script_map_start(const sh_script_mod_desc *mods, int n, const sh_script_source *source,
                        uint64_t seed, double now);

/* One game frame (main thread, host, Play); `now` and `dt` are seconds. The first frame opens each prepared mod's
 * state and runs its files and map_start; each frame delivers handle-table spawns and removals, then per mod its
 * queued records, think(self, dt), timers and coroutines, in up to SH_SCRIPT_PASSES passes. */
void sh_script_tick(double now, double dt);

/* The map ended: run each running mod's on.map_end handlers when run_end_handlers is 1 and the scripts had
 * started, then close every state and queue and free every timer, binding and file list. The logs stay readable;
 * called while Lua runs, it waits for the running call to return. */
void sh_script_map_close(int run_end_handlers);

/* sh_script_map_close(1). */
void sh_script_map_end(void);

/* 1 between a map start that opened at least one state and its map end. */
int sh_script_active(void);

/* 1 once the prepared mods of this map have started (the first tick). */
int sh_script_started(void);

/* ---------------------------------------------------------------- events */

/* Records arrive through the mod's queue (script_queue.h), posted by the engine hooks with handle-table handles:
 *   SH_SCRIPT_EV_DAMAGE  self, other = attacker, third = inflictor, amount, text = damage decl, text2 = location
 *   SH_SCRIPT_EV_DEATH   self, other = attacker, third = inflictor, text = damage decl
 *   SH_SCRIPT_EV_ANIM    self, text = the ae_notify string; on.animation handlers get it whole, and a text
 *                        "<command>:<a>:<b>" also reaches on.frame_event(name, command, fn(self, args))
 *                        with args = { a, b } (a piece that reads as a number is a number)
 *   SH_SCRIPT_EV_INPUT   self = the Script node, other = activator, text = the node's name
 *   SH_SCRIPT_EV_PAIN    self, other = attacker, text = the pain reaction, text2 = damage decl:
 *                        on.pain(name, fn(self, reaction, damage, attacker))
 *   SH_SCRIPT_EV_SIGNAL  self fired a SnapMap event, other = activator, text = the event
 *                        (lower case, without SNAP_EVENT_): on.signal(event, fn(entity, activator)),
 *                        for any entity of the map
 *   SH_SCRIPT_EV_PROJECTILE self, other = the projectile, text = its decl, vec = origin and direction:
 *                        on.projectile(name, fn(self, p, decl, origin, dir))
 * A damage record's vec is the hit direction. A record for an entity the mod does not drive is dropped at
 * delivery; spawns and removals are the handle table's (sh_script_handles_sweep). */

/* What the running mods bind: 1 when a running mod registered a handler for the full entityDef name, with its
 * salt in salt_out and the kinds bound (SH_SCRIPT_BIND_ bits) in *kinds. Any thread. */
#define SH_SCRIPT_BIND_SPAWN  0x01u
#define SH_SCRIPT_BIND_THINK  0x02u
#define SH_SCRIPT_BIND_REMOVE 0x04u
#define SH_SCRIPT_BIND_DAMAGE 0x08u
#define SH_SCRIPT_BIND_DEATH  0x10u
#define SH_SCRIPT_BIND_ANIM   0x20u
#define SH_SCRIPT_BIND_PAIN   0x40u
#define SH_SCRIPT_BIND_PROJECTILE 0x80u
#define SH_SCRIPT_BIND_FRAME  0x100u
#define SH_SCRIPT_BIND_SIGNAL 0x200u   /* bound as "signal:<event>" */
#define SH_SCRIPT_BIND_HIT    0x400u
/* Hook slot of a subscribed attacker, or -1. Only owned actors can receive outgoing hits. */
int sh_script_hit_recipient(uint32_t spawn_id, sh_script_handle *attacker);
int sh_script_binds(const char *full_name, char *salt_out, size_t cap, unsigned *kinds);

/* Read a bound actor's own served AI behavior at map start. Scan existing actors after registration. */
void sh_script_set_enemy_ops(char *(*behavior_of)(const char *salt, const char *entitydef), void (*scan)(void));
int sh_script_enemy_bindings_ready(void);
sh_script_handle sh_script_enemy_record(uint32_t spawn_id, const char *behavior, int *full);
sh_script_handle sh_script_record_actor(void *entity, uint32_t spawn_id, const char *const *mod_chain,
                                       int count, const char *(*behavior_of)(void *), int *full);

/* 1 when `handle` is an entity of a running mod (recorded in the handle table under it, not yet removed), with
 * its salt in salt_out. Any thread. */
int sh_script_entity_owner(uint64_t handle, char *salt_out, size_t cap);

/* --------------------------------------------------------------- engine */

/* Where log lines also go (the console); NULL for none. Lines arrive on the
 * thread that ran the script. */
typedef void (*sh_script_line_fn)(void *context, const char *line);
void sh_script_set_console(sh_script_line_fn fn, void *context);

/* ------------------------------------------------------------- API seam */

/* Functions other modules add to every state opened after the call: a global (spawn, players) or an entity
 * method (e:position()). Call before sh_script_map_start, from one thread; 0 when the table is full or the name is
 * unusable. */
#define SH_SCRIPT_API_GLOBAL 1
#define SH_SCRIPT_API_METHOD 2
int sh_script_api_add(int kind, const char *name, lua_CFunction fn);

/* A function that adds a module's own globals to every state opened after the call; it runs inside the state's
 * protected open, after the added functions and before "on". Same rules as sh_script_api_add; once per function. */
typedef void (*sh_script_opener_fn)(lua_State *L);
int sh_script_api_add_opener(sh_script_opener_fn fn);

/* SnapMap events: `known` answers 1 for an event name the game has (lower case, without SNAP_EVENT_; NULL accepts
 * any); `wanted` runs when a script registers on.signal, so the engine side installs its hook then. */
void sh_script_set_signal_ops(int (*known)(const char *event), void (*wanted)(void));
/* Queue a SnapMap event for every running mod that handles it: the number of
 * mods it reached. Any thread. */
int sh_script_signal_post(const char *event, sh_script_handle self, sh_script_handle activator);

/* A function run at every map end, after every state closed (a module undoes
 * what its calls changed in the game for the map). Adding one twice adds it
 * once. */
typedef void (*sh_script_closer_fn)(void);
int sh_script_api_add_closer(sh_script_closer_fn fn);
/* Run fn for every spawned entity of a mod before its spawn handler, to give it what the game's spawn left out.
 * Up to 4; idempotent. */
void sh_script_api_on_spawn(void (*fn)(void *ent));

/* The mod a state (or any of its coroutines) belongs to. */
const char *sh_script_salt(lua_State *L);
const char *sh_script_mod_name(lua_State *L);

/* Push the Lua object for an entity handle: the same object per handle within a state, with its own e.state table. */
void sh_script_push_entity(lua_State *L, uint64_t handle);

/* The handle of the entity object at `arg`, or 0 when it was removed. Raises a plain error naming `call` when the
 * value is not an entity. */
uint64_t sh_script_check_entity(lua_State *L, int arg, const char *call);

/* 1 when the entity is this mod's: recorded in the handle table under it and not yet removed. */
int sh_script_owns_entity(lua_State *L, uint64_t handle);

/* The mod-relative naming rule: resolve the name at `arg` to its full name, valid for the next seven resolutions of
 * this state: "ai/tyrant" -> "mods/<salt>/ai/tyrant"; this mod's own "mods/<salt>/..." is accepted, any other
 * "mods/..." refused. A native("...") reference gives DOOM's name when allow_game (*is_game = 1), else is refused;
 * a refusal raises a plain error naming `call`. */
const char *sh_script_check_name(lua_State *L, int arg, const char *call, int allow_game, int *is_game);
/* The same for a sound decl: a short name's leaf takes the namespace ("play_x" -> "mods/<salt>/<salt>_play_x"). */
const char *sh_script_check_sound_name(lua_State *L, int arg, const char *call, int allow_game, int *is_game);

/* Raise a plain error at the calling script line; `fmt` is the one sentence the stop line shows. Never returns. */
int sh_script_refuse(lua_State *L, const char *fmt, ...);

/* The script line at `level` (luaL_where's level): its whole file name in *file (malloc'd; NULL when it is not a
 * script line) and its line; luaL_where cuts the name to LUA_IDSIZE. */
void sh_script_where_line(lua_State *L, int level, char **file, int *line);

/* ------------------------------------------------------------ UI queries */

#define SH_SCRIPT_NONE    0      /* no scripts for this mod in the current or last map */
#define SH_SCRIPT_RUNNING 1
#define SH_SCRIPT_STOPPED 2
#define SH_SCRIPT_ENDED   3      /* the map ended with the scripts running */
int sh_script_status(const char *salt);

/* The stop line of `salt`, whole, malloc'd (the caller frees it), when its
 * scripts stopped in the current or last map; else NULL. Any thread. */
char *sh_script_stop_line_dup(const char *salt);

/* Call fn for each line of `salt`'s script log, oldest first (prints,
 * warnings, the stop line). Returns the line count. Any thread; fn runs under
 * the log lock and must not call back into the runtime. */
int sh_script_log_read(const char *salt, sh_script_line_fn fn, void *context);

/* Call fn(context, salt, name) for every mod with a log from the current or
 * last map. Returns the count. Any thread. */
typedef void (*sh_script_mod_fn)(void *context, const char *salt, const char *name);
int sh_script_mods(sh_script_mod_fn fn, void *context);

/* One mod's state for the console and the UI: 1 when `salt` has scripts in
 * the current or last map. Any thread. */
typedef struct sh_script_mod_info {
    char salt[SH_SCRIPT_SALT_CAP];
    char *name;              /* malloc'd, any length: sh_script_mod_info_free */
    int  status;             /* SH_SCRIPT_NONE .. SH_SCRIPT_ENDED */
    int  waiting;            /* prepared for this map, not started yet (waiting for Play) */
    int  files;              /* entry files found for the map */
    int  files_run;          /* entry files that ran */
    int  handlers;           /* handlers registered (on.*) */
    int  entities;           /* entities its scripts drive now */
    int  log_lines;
    int  stopped_line;       /* a stop line is kept: sh_script_stop_line_dup gives it whole */
} sh_script_mod_info;
int sh_script_mod_info_get(const char *salt, sh_script_mod_info *out);
void sh_script_mod_info_free(sh_script_mod_info *info);

/* Compile every script file of `salt` without running anything (the console's reload outside Play): fn gets one
 * line per file that fails, worded as the stop line. Returns the files that compiled, or -1 when unlistable. */
int sh_script_check_files(const char *salt, const char *name, const sh_script_source *source,
                          sh_script_line_fn fn, void *context);

/* ------------------------------------------------------------ for tests */

/* Bytes the runtime holds: every state's allocator count plus queues, timers, bindings and entity tables (not the
 * logs); 0 after sh_script_map_end. */
size_t sh_script_bytes_in_use(void);

/* The wall-clock stop, adjustable for a test; sh_script_limits_reset restores it. */
typedef struct sh_script_limits {
    unsigned wall_ms;
} sh_script_limits;
void sh_script_limits_set(const sh_script_limits *limits);
void sh_script_limits_reset(void);

#ifdef __cplusplus
}
#endif
#endif
