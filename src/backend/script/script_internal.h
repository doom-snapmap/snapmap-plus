/* script_internal.h -- what script_runtime.c, script_lib_base.c and
 * script_lstrlib.c share. Not for other modules: they use script_runtime.h. */
#ifndef SH_SCRIPT_INTERNAL_H
#define SH_SCRIPT_INTERNAL_H

#include "script_runtime.h"
#include "../../third_party/lua/lauxlib.h"

/* Registry keys of a state's own tables. */
#define SHS_REG_HANDLERS "snapmap-plus.handlers"   /* kind -> name -> {fn...} */
#define SHS_REG_ENTS     "snapmap-plus.entities"   /* handle -> object, for the map */
#define SHS_REG_LIVE     "snapmap-plus.live"       /* handle -> object, bound and alive */
#define SHS_REG_TIMERS   "snapmap-plus.timers"     /* timer id -> function or coroutine */
#define SHS_REG_RUNS     "snapmap-plus.runs"       /* coroutine -> true, weak keys */
#define SHS_REG_LOADED   "snapmap-plus.loaded"     /* require name -> value */
#define SHS_REG_METHODS  "snapmap-plus.methods"    /* entity method name -> function */

/* A script's path as the log names it (scripts, lib, the file name): a
 * file name is at most 255 characters on Windows file systems. */
#define SHS_PATH_CAP 512

#define SHS_MT_ENTITY "entity"
#define SHS_MT_TIMER  "timer"
#define SHS_MT_GAME   "game name"

typedef struct shs_ent {
    uint64_t handle;
    char    *chain;          /* malloc'd: the entityDef names, '\n' between, most derived first */
} shs_ent;

typedef struct shs_bind {
    char     name[SH_SCRIPT_DEFNAME_CAP];
    unsigned kinds;
} shs_bind;

#define SHS_TIMER_AFTER 1
#define SHS_TIMER_EVERY 2
#define SHS_TIMER_RUN   3

typedef struct shs_timer {
    int    id;
    int    kind;
    double due;
    double period;
} shs_timer;

typedef struct shs_mod {
    char salt[SH_SCRIPT_SALT_CAP];
    char name[SH_SCRIPT_MODNAME_CAP];
    int  status;             /* SH_SCRIPT_NONE .. SH_SCRIPT_ENDED */
    int  in_map;             /* listed by the current map */
    lua_State *L;

    /* bounds */
    size_t    mem;
    int       stop;          /* SH_SCRIPT_STOP_ code once a bound is crossed, else -1 */
    int       charging;      /* inside a delivery */
    long long pattern_steps; /* pattern steps since the clock was last read */
    long long wall_start;    /* QueryPerformanceCounter at the delivery's start */
    char      last_src[SHS_PATH_CAP];  /* the last line the clock hook saw, for a stop with no line */
    int       last_line;
    char      err_file[SHS_PATH_CAP];  /* where the current error was raised, by the message handler */
    int       err_line;

    /* this map: the hooks' slot (its queue and its handles), the entry files */
    int       hook;          /* sh_script_hooks_mod_add slot, -1 when none */
    int       prepared;      /* registered for this map; its state opens at the first tick */
    char    **files;         /* entry files, sorted, malloc'd */
    int       nfiles;
    int       files_found;   /* entry files listed for this map (kept after the list is freed) */
    int       files_run;
    int       handlers;
    uint64_t  seed;

    /* driven entities, bindings, timers: game thread */
    shs_ent  *ents;
    int       nents, cents;
    shs_bind *binds;         /* guarded by the runtime's lock */
    int       nbinds, cbinds;
    shs_timer *timers;
    int       ntimers, ctimers, next_timer;

    /* load */
    const sh_script_source *source;

    /* log: every line of the map, whole; guarded by the runtime's lock */
    char    **log;
    int       nlog, clog;
    char     *stop_line;     /* the stop line, whole (malloc'd), or NULL */
} shs_mod;

/* The mod of a state or coroutine. */
shs_mod *shs_mod_of(lua_State *L);
const sh_script_limits *shs_limits(void);
double shs_now(void);

/* Read the clock before C work the clock hook cannot see (a sort): past the
 * wall-clock stop the mod is marked stopped and an error is raised at the
 * calling line. */
void shs_check_clock(lua_State *L);
/* Raise the error of a marked stop (never returns). */
int  shs_raise_stop(lua_State *L, int level);
/* One pattern step (script_lstrlib.c). */
void shs_match_step(lua_State *L);

/* Append a line to the mod's log (and the console). */
void shs_log(shs_mod *m, const char *line);

/* Open the sandboxed libraries into a new state (script_lib_base.c). */
void shs_open_libs(lua_State *L, uint64_t seed);
/* Load scripts/<relpath> of the mod as a text chunk and push the function;
 * on failure push the error message and return the Lua status. */
int  shs_load_file(lua_State *L, shs_mod *m, const char *relpath);

/* Timers and coroutines (script_lib_base.c). */
void shs_timers_run(shs_mod *m);
void shs_timers_free(shs_mod *m);

/* Run the C job `job` with light userdata `ud` as one protected delivery.
 * Returns 1 when it returned, 0 when the mod stopped (the stop line is
 * written and the state closed). Top level only: never from inside Lua. */
int  shs_run_job(shs_mod *m, lua_CFunction job, void *ud);
/* Inside a job: call the function below `nargs` values as one handler call
 * (the per-call bounds start again). Errors propagate to the job. */
void shs_call_handler(lua_State *L, int nargs);
/* Reset the per-call bounds (before a coroutine resume). */
void shs_call_reset(shs_mod *m);
/* Record the innermost script line on L's stack, from `first` up, as where
 * the current error was raised (unless one is recorded already). */
void shs_where_from(shs_mod *m, lua_State *L, int first);

#endif
