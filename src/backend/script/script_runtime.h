/* script_runtime.h -- mod scripts: one sandboxed Lua 5.4 state per scripted
 * mod per map, the queue every engine hook writes into, and the one tick that
 * delivers it.
 *
 * A script is a carried text file of a mod, assets\mods\<salt>\scripts\*.lua
 * (entry files) and scripts\lib\*.lua (loaded only through require). The
 * runtime reads them through the serving layer (sh_script_source below). A
 * map's scripted mods are prepared at the Play gate (sh_script_map_open: no
 * script code runs there); the first tick runs every entry file once in
 * byte-wise name order, then the on.map_start handlers. After that Lua runs
 * only inside sh_script_tick, on the thread that calls it, never
 * re-entrantly: hooks on any thread post records to the mod's queue
 * (script_queue.h), the spawn subscriber records entities in the handle table
 * (script_handles.h), and the tick delivers both.
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
 * Threads: sh_script_map_start, sh_script_tick and sh_script_map_close run
 * Lua and are serialized by one lock (a call made while Lua runs on the same
 * thread is deferred: map end runs when the running call returns, a nested
 * tick is skipped). sh_script_binds, sh_script_entity_owner and the log
 * queries may be called from any thread.
 *
 * Co-op: the caller (the tick detour, the hooks) runs none of this on a client.
 *
 * SH_SCRIPT_PURE builds without the backend log (the tests). */
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

/* Prepare scripts for a map: end anything left from the last map (with its
 * end handlers), empty the hooks' mod table and the handle table, then for
 * each mod list its scripts; a mod with at least one entry file is
 * registered with the hooks (sh_script_hooks_mod_add: its queue opens and its
 * entities are recorded from now on). No script code runs. `seed` seeds
 * math.random. Returns the mods prepared. Main thread. */
int sh_script_map_prepare(const sh_script_mod_desc *mods, int n, const sh_script_source *source,
                          uint64_t seed);

/* Where sh_script_map_open finds the map's mods: fill out[] (up to cap) with
 * the served mods that carry scripts, set *source and *seed, and return the
 * count (-1 when the map's mods could not be read: nothing runs). The strings
 * need to live only until it returns. */
typedef int (*sh_script_map_mods_fn)(void *context, sh_script_mod_desc *out, int cap,
                                     const sh_script_source **source, uint64_t *seed);
void sh_script_set_map_provider(sh_script_map_mods_fn fn, void *context);

/* The hooks' Play gate (script_hooks.h): prepare the mods the provider names.
 * Returns the mods prepared, 0 when none carries scripts, -1 when they could
 * not be read (the reason is logged). Main thread. */
int sh_script_map_open(void);

/* Prepare and start at once (the files and map_start run now, with `now` as
 * the game time): the tests. Returns the mods whose scripts are running. */
int sh_script_map_start(const sh_script_mod_desc *mods, int n, const sh_script_source *source,
                        uint64_t seed, double now);

/* One game frame (the hooks' tick: main thread, host, Play). On the first
 * frame of a map, each prepared mod's state opens and its files and
 * map_start run. Then the handle table's changes since the last frame are
 * delivered (a spawned entity whose entityDef chain the mod binds gets
 * spawn(self) and is driven from then on; a removed one gets remove(self)
 * once and its object answers nothing), then for every running mod its
 * queued records, think(self, dt) for every driven entity, due timers and
 * coroutines, then what those raised, up to SH_SCRIPT_PASSES queue passes;
 * what is left waits for the next frame. `now` is the game time in seconds,
 * `dt` the frame time. */
void sh_script_tick(double now, double dt);

/* The map ended: run each running mod's on.map_end handlers (within their
 * budget) when run_end_handlers is 1 and the scripts had started, then close
 * every state and free every timer, binding and file list; each mod's queue
 * closes. The logs stay readable. Called while Lua runs, it waits until the
 * running call returns. */
void sh_script_map_close(int run_end_handlers);

/* sh_script_map_close(1). */
void sh_script_map_end(void);

/* 1 between a map start that opened at least one state and its map end. */
int sh_script_active(void);

/* 1 once the prepared mods of this map have started (the first tick). */
int sh_script_started(void);

/* ---------------------------------------------------------------- events */

/* An entityDef chain (the entityDef's name, then its inherits, a newline
 * between them) fits in this many bytes. */
#define SH_SCRIPT_NAMES_CAP  512

/* Records come in through the mod's queue (script_queue.h), posted by the
 * engine hooks with handles from the handle table:
 *   SH_SCRIPT_EV_DAMAGE  self, other = attacker, third = inflictor, amount, text = damage decl, text2 = location
 *   SH_SCRIPT_EV_DEATH   self, other = attacker, third = inflictor, text = damage decl
 *   SH_SCRIPT_EV_ANIM    self, text = the ae_notify string
 *   SH_SCRIPT_EV_INPUT   self = the Script node, other = activator, text = the node's name
 * A record for an entity the mod's scripts do not drive is dropped at
 * delivery. Spawns and removals are the handle table's (sh_script_handles_sweep).
 * Names, notify strings and node names are at most SH_SCRIPT_EVENT_TEXT - 1
 * bytes; registration refuses longer ones. */

/* What the running mods bind: 1 when a running mod registered a handler for the
 * full entityDef name, with that mod's salt in salt_out and the kinds bound
 * (SH_SCRIPT_BIND_ bits) in *kinds. Any thread. */
#define SH_SCRIPT_BIND_SPAWN  0x01u
#define SH_SCRIPT_BIND_THINK  0x02u
#define SH_SCRIPT_BIND_REMOVE 0x04u
#define SH_SCRIPT_BIND_DAMAGE 0x08u
#define SH_SCRIPT_BIND_DEATH  0x10u
#define SH_SCRIPT_BIND_ANIM   0x20u
int sh_script_binds(const char *full_name, char *salt_out, size_t cap, unsigned *kinds);

/* 1 when `handle` is an entity of a running mod (placed from its entityDefs
 * or spawned by it: recorded in the handle table under it, not yet
 * removed), with its salt in salt_out. Any thread. */
int sh_script_entity_owner(uint64_t handle, char *salt_out, size_t cap);

/* --------------------------------------------------------------- engine */

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

/* 1 when the entity is this mod's: recorded in the handle table under it
 * (placed from its entityDefs, or spawned by its scripts, which the spawn
 * subscriber records the same way) and not yet removed. */
int sh_script_owns_entity(lua_State *L, uint64_t handle);

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

/* One mod's state for the console and the UI: 1 when `salt` has scripts in
 * the current or last map. Any thread. */
typedef struct sh_script_mod_info {
    char salt[SH_SCRIPT_SALT_CAP];
    char name[SH_SCRIPT_MODNAME_CAP];
    int  status;             /* SH_SCRIPT_NONE .. SH_SCRIPT_ENDED */
    int  waiting;            /* prepared for this map, not started yet (waiting for Play) */
    int  files;              /* entry files found for the map */
    int  files_run;          /* entry files that ran */
    int  handlers;           /* handlers registered (on.*) */
    int  entities;           /* entities its scripts drive now */
    int  log_lines;
    char stop_line[SH_SCRIPT_LINE_CAP];
} sh_script_mod_info;
int sh_script_mod_info_get(const char *salt, sh_script_mod_info *out);

/* Compile every script file of `salt` without running anything (the
 * console's reload outside Play): calls fn with one plain line per file that
 * does not compile, worded as the stop line words it. Returns the files that
 * compiled, or -1 when the files could not be listed. Not while Lua runs. */
int sh_script_check_files(const char *salt, const char *name, const sh_script_source *source,
                          sh_script_line_fn fn, void *context);

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
