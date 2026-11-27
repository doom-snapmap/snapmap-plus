/* modui.h -- the WebView Mods tab's backend (the `mods` interface slot): one JSON document (payload v4,
 * docs/webview-ui.md) of the store's mod rows and the open map's manifest rows, plus the tab's actions and the
 * `sh_mods` console verbs. Every operation except FETCH and ROOT runs on DOOM's main thread, except the file part of
 * Install, New mod and Refresh (the window's worker, finished by SH_MODS_ACT_APPLY on the main thread), which the
 * store lock serializes with the sh_mods command; FETCH copies the shared buffer on any thread. */
#ifndef SNAPMAP_PLUS_MODUI_H
#define SNAPMAP_PLUS_MODUI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sh_iface;
struct sh_mods_call;

/* One console line (no newline). */
typedef void (*sh_modui_line_fn)(void *context, const char *line);

/* Called once per placed name the open map holds (duplicates allowed). */
typedef void (*sh_modui_name_fn)(void *context, const char *name);

/* A mod's scripts in the current or last Play (script_runtime.h
 * sh_script_mod_info_get), as the tab shows them. */
#define SH_MODUI_SCRIPT_NONE     0   /* no Play has run them yet */
#define SH_MODUI_SCRIPT_RUNNING  1
#define SH_MODUI_SCRIPT_STOPPED  2   /* stopped by an error: `stop` is the line */
#define SH_MODUI_SCRIPT_ENDED    3   /* the last Play ended with them running */
#define SH_MODUI_SCRIPT_WAITING  4   /* prepared for this map, waiting for Play */
typedef struct sh_modui_script_info {
    int  status;          /* SH_MODUI_SCRIPT_* */
    int  lines;           /* lines in its log */
    char *stop;           /* the stop line while status is STOPPED (malloc'd, whole; the reader frees it), else NULL */
} sh_modui_script_info;

/* Engine operations, bound by iface_engine.c and by fakes in tests. A NULL member takes the product default where
 * there is one (rescan, palette rebuild, stock reader; none in a SH_MODUI_PURE build), else the matching part of
 * the document is omitted or the action refuses. */
struct sh_modserve_refresh;
struct sh_modui_map_info;

typedef struct sh_modui_engine {
    int (*on_main_thread)(void);        /* 1 main, 0 other, negative unknown */
    /* Emit every committed name of the open editor map (module names, entity inherits and their decl text's string values).
     * Returns the entities read, or -1 when no editor map is open. Main thread only. */
    int (*placed_names)(sh_modui_name_fn emit, void *context);
    /* Current committed snapshot and edit revision, or NULL when native edit notifications are unavailable. */
    const void *(*placement_revision)(unsigned long *revision);
    int (*rescan)(void);                /* sh_modserve_index_rescan */
    /* Propagate the rescan's changes into the running game (sh_modserve_refresh_after_rescan): 0 nothing changed,
     * 1 applied, 2 deferred. Main thread, after rescan. */
    int (*refresh)(struct sh_modserve_refresh *out);
    int (*palette_rebuild)(void);       /* modserve_palette_rebuild_requested: 1 ran, 0 refused */
    /* An installed game resource: 1 present, 0 absent, -1 reader unavailable. */
    int (*stock_exists)(const char *request);
    /* The open editor map serialized as a save would, in a process-heap buffer the caller frees with HeapFree.
     * 1 with *out, 0 no editor map, -1 failed. Main thread only. */
    int (*map_json)(char **out, size_t *len);
    /* The open map's saved-map ids and search tag (what the next local save writes). 1 when a map is open;
     * tagged is 1, 0, or -1 unknown. Main thread only. */
    int (*map_info)(struct sh_modui_map_info *out);
    /* Delete editor entities (uniqueIds) through DOOM's own Delete Selection in one undo step
     * (sh_editor_delete_batch); returns SH_EDITOR_DELETE_* (1 dispatched). Main thread only. */
    int (*delete_entities)(const int *ids, int count);
    /* Remove `tag` from the open map's tags. 1 removed, 0 absent, -1 failed. */
    int (*remove_tag)(const char *tag);
    /* 1 with the cause when the game cannot take a change (Play is running, a map is loading), else 0. */
    int (*busy)(char *cause, size_t cap);
    /* The editor toast with a title and literal text; nothing outside the editor. */
    void (*toast)(const char *title, const char *text);
    /* One line in the game console (no newline). */
    void (*print)(const char *line);
    /* Bring a map-scope change into the running game (sh_modserve_scope_apply): 1 applied, 0 nothing pending,
     * -1 refused. Main thread. */
    int (*scope_apply)(const char *phase);
    /* The same for a declared set the author changed (map add, upgrade): the declared builds' decls are registered
     * first, as a map load does (sh_modserve_scope_activate). Unbound: scope_apply. */
    int (*scope_activate)(const char *phase);
    /* Asked when the editor screen is down (map_info 0): 1 the editor has left the map (no editor state runs),
     * 0 a menu screen is over the open map (the Start menu's Settings), -1 unknown. The map scope's exit waits for
     * 1 (or -1, or no op bound). Main thread. */
    int (*map_left)(void);
    /* Empty the open map's undo and redo history to the current state (sh_editor_undo_clear). probe 1: 1 possible,
     * 0 no editor map, -1 unavailable; probe 0: 1 cleared, 0 no editor map, -1 failed. Main thread. */
    int (*clear_undo)(int probe);
    /* Mark the open map modified as a placed entity does: one undo step named `label` through the editor's own
     * commit (sh_editor_mark_modified). 1 marked, 0 no editor map, -1 unavailable. Main thread. */
    int (*mark_modified)(const char *label);
    /* 1 while a native editor action has not committed yet
     * (sh_editor_delete_native_pending): the Uninstall's undo clear waits for
     * the delete's own commit. */
    int (*undo_pending)(void);
    /* A hub launch's pre-load pass (sh_modserve_launch_preload), called once per launch by sh_modui_scope_launch
     * while the game is still at the hub. Unbound: nothing. */
    int (*launch_preload)(const char *why);
    /* Mod scripts (script_runtime.h; none in a SH_MODUI_PURE build). Any thread. script_info: 1 with *out when
     * `salt` has scripts in the current or last Play, else 0. script_log: each line of `salt`'s log, oldest first;
     * returns the count. script_mods: each salt with a log or a state, once; returns the count. */
    int (*script_info)(const char *salt, sh_modui_script_info *out);
    int (*script_log)(const char *salt, sh_modui_line_fn fn, void *context);
    int (*script_mods)(sh_modui_name_fn fn, void *context);
    /* The sound banks whose file changed after the game loaded them (sh_modsound_restart_banks): fn(context, bank
     * file name, owner salt) once each; returns the count. Any thread. */
    int (*restart_banks)(void (*fn)(void *context, const char *bank, const char *owner), void *context);
    void (*finish_prefab_actors)(const char *json, size_t len);
} sh_modui_engine;

typedef struct sh_modui_map_info {
    char local_id[64];     /* the saved-map id, "" before the first save */
    char cloud_id[64];     /* its cloud-saved item id, "" when none */
    int  tagged;           /* the search tag "mods": 1, 0, or -1 unknown */
    /* The rawmap file the open map was opened from while no save has named it (sh_rawmap_open_unsaved), else "";
     * the edit's ids then name the saved map the reload borrowed, not the open map's. */
    char opened_from[260];
} sh_modui_map_info;

/* Bind the engine operations. NULL binds only the product defaults: no
 * main-thread check and no placed names. */
void sh_modui_bind(const sh_modui_engine *engine);

/* Build the document into the shared buffer. Returns its length, or -1 when
 * refused (off the main thread) or out of memory. */
int  sh_modui_list(void);

/* Run one action (SH_MODS_ACT_*), keeping its result document {"action","ok","message",...} in the shared buffer.
 * `arg` is a folder for INSTALL (absolute; several separated by newlines) or a row's key (its folder name under
 * mods\) for UNINSTALL and the map actions. INSTALL, NEW_MOD and RESCAN may run off the main thread; their result
 * carries "apply": true until SH_MODS_ACT_APPLY finishes it. Returns the result length, or -1 when refused first. */
/* UNINSTALL deletes the mod's entities from the open map through the native delete (refused while Play runs, a map
 * loads or the editor is busy), clears the undo history once that delete commits, recycles the folder (deleted
 * outright when the bin cannot take it) and rescans. MAP_ADD attaches a mod to the open map as a row the next save
 * writes; MAP_REMOVE drops the row and deletes nothing. */
/* FILES (arg: a key) lists the mod folder's and the game's files its build reads; FILE_TEXT (arg: "<key>\t<path>")
 * answers one text file up to 1 MiB; NEW_MOD (arg: a display name) makes a mod folder with a working example. */
int  sh_modui_run(int action, const char *arg);

/* The console verbs sharing the tab's dry runs and actions (argv[0] is the verb; lines start "MODS: "):
 *   map                         the open map: details, then each mod it uses
 *   map add "<mod>"             attach the mod to the open map (a map-level row)
 *   map remove "<mod>" [--yes]  the dry run; with --yes it is performed
 *   uninstall "<mod>" [--yes]   the dry run; with --yes it is performed
 * <mod> is a mod's folder (its path under mods\, or a nested folder's own name). Returns 1 when the verb was
 * handled, 0 for another verb, -1 off the main thread. */
int  sh_modui_command(int argc, const char *const *argv, sh_modui_line_fn line, void *context);

/* The main-thread tick after each native frame: sh_modui_poll_due is cheap (0 when nothing is owed or under 250 ms
 * since the last map-scope tick; every frame while an Uninstall's undo clear is owed). When it returns 1 the
 * caller runs sh_modui_poll_main inside the process heap scope its main-thread calls use. */
int  sh_modui_poll_due(void);
int  sh_modui_poll_main(void *context);

/* The names the open editor map places now (live, not its last save): *names malloc'd, each freed with free(), as
 * sh_modmanifest_names_free does. The count, -1 outside the editor or on failure. Main thread. */
int  sh_modui_placed_names(char ***names, size_t *count);
/* A hub launch (a My Maps play, a published launch) passed its gate: the map scope declares exactly the launched
 * map's rows and the serving mode is Play, kept until the tick clears it once that Play has run and ended.
 * Main thread. Returns the builds whose serving changed. */
int  sh_modui_scope_launch(const char *why);

/* A hub launch the published-map gate refused: nothing was launched, so its parse's rows are not declared. A
 * map still showing in the editor gets its rows back (sh_modmanifest_current_restore_previous); elsewhere the
 * current manifest is closed. The serving mode returns to EDITOR. Main thread. Returns the bundles whose serving
 * changed. */
int  sh_modui_scope_launch_refused(const char *why);

/* What the tick reads (sh_modui_poll_main): 1 while the editor shows a map (map_info), 0 not, -1 unbound; and 1 while
 * the game is busy (a map loads or Play runs), else 0. Main thread. */
int  sh_modui_editor_shows_map(void);
int  sh_modui_game_busy(void);

/* `sh_mods doctor [verify]`: every mod folder with its state and its plain
 * cause, conflicts with their winners, the game files mods replace, and with
 * `verify` a check of every mod's files. Each line starts "MODS: doctor:".
 * Returns 1, or -1 off the main thread or out of memory. */
int  sh_modui_doctor(int verify, sh_modui_line_fn line, void *context);

/* Copy the shared buffer. Returns 1 copied, -1 when out is NULL or too small
 * (*needed set), 0 when nothing has been built. */
int  sh_modui_fetch(char *out, int out_cap, int *needed, unsigned *generation);

/* The script log op (SH_MODS_SCRIPTS), any thread, read only: copies into out {"mods":[{"salt","status","stop",
 * "lines"}],"salt","total","full","log":[...]}, the log being `salt`'s last SH_MODUI_SCRIPT_TAIL lines (all with
 * the SH_MODS_SCRIPTS_FULL prefix). Returns 1 copied, -1 when out is NULL or too small (*needed set), 0 when out of
 * memory; it never touches the shared buffer. */
#define SH_MODUI_SCRIPT_TAIL  1000
int  sh_modui_scripts(const char *salt, char *out, int out_cap, int *needed);

/* Bumped by every action that can change the store or the serving index. */
unsigned sh_modui_generation(void);

/* A change the tab did not make (a console verb: sh_mods build, rebuild, install, rescan): bumps the generation
 * so the window pushes a fresh document to an open Mods tab. Any thread. */
void sh_modui_note_changed(void);
/* The publishing key's Settings requests (SH_MODS_ACT_KEY) and its state in the list document ("publishingKey"),
 * modclaim.h modclaim_key_action and modclaim_key_status_json: each answer malloc'd. Unbound, neither is offered. */
void sh_modui_set_key(char *(*action)(const char *arg), char *(*status)(void));
/* Refresh the listing and register its current declarations and palette rows before a prefab is placed.
 * Main thread: 1 if the listing changed, 0 if it stayed the same, -1 off the main thread. */
int  sh_modui_scope_refresh(const char *why);
void sh_modui_prefab_finished(void);

/* The interface slot body (see snapmap_plus_iface.h, `mods`). */
int  sh_modui_slot(struct sh_iface *self, struct sh_mods_call *call);

/* The player's setting "mods.scripted_maps" (on by default): while sh_modui_scripted_maps_allowed() is 0, a map
 * whose served content carries scripts is not played, and a rawmap or prefab ZIP import whose mods carry scripts
 * is refused and installs nothing, both with SH_MODUI_SCRIPTS_OFF. */
#define SH_MODUI_SCRIPTS_OFF \
    "This map's mods include scripts, which you turned off. To play it, turn on " \
    "\"Play Maps Whose Mods Include Scripts\" in the Settings menu of the Snapmap+ window."
/* The same refusal of a prefab's .zip import whose content carries scripts. */
#define SH_MODUI_SCRIPTS_OFF_PREFAB \
    "This prefab's mods include scripts, which you turned off. To use it, turn on " \
    "\"Play Maps Whose Mods Include Scripts\" in the Settings menu of the Snapmap+ window."
int  sh_modui_scripted_maps_allowed(void);

#ifdef SH_MODUI_TESTING
/* The hub launch state machine (modui.c): "none", "launched", "loading",
 * "playing", "running" or "ended". */
const char *sh_modui_test_launch_state(void);
/* The Mods tab's sentence for a build or store cause (malloc'd; NULL for none). */
char *sh_modui_test_plain_cause(const char *cause);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_MODUI_H */
