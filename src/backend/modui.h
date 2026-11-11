/* modui.h -- the WebView Mods tab's backend (the `mods` interface slot).
 *
 * One JSON document (payload v4, docs/webview-ui.md) describes what a player
 * needs to see:
 *   mods      one row per mod folder of the store (modstore enumeration):
 *             the player's own mods at the root of mods\ (with their
 *             organizing folders as "group"), and the mods a downloaded map
 *             brought, in that map's place mods\maps\<map title>\ ("place"
 *             and "placeTitle"). A row is "ready" when its build serves, else
 *             "attention" with one plain sentence saying what is wrong and
 *             what to do ("cause"). It carries what the details pane shows:
 *             the Toybox rows the mod adds, what it changes in the game (its
 *             derived edits, editor tweaks, replacements), what it borrows
 *             from the game, its console settings, the builder's warnings in
 *             plain words, and the informational same-files hint (another of
 *             the player's mods carries at least half of its files). "salt"
 *             is the mod's identity for the page's Copy mod id and the Files
 *             view; no sentence names a salt, a hash or a build number.
 *   map       the manifest rows the last map load carried (modmanifest's
 *             current record), each with its state under the asset rule
 *             (modserve.h sh_modserve_row_served: active, incomplete,
 *             skipped, missing) and the refusal sentence, and the builds the
 *             placed entities resolve to now (the save-time closure,
 *             modserve_bundles_for_names_visit); with the map's JSON bound
 *             also its name, last save, published and uploaded state (the
 *             record's links and claims), the search tag, and one entry per
 *             mod it uses with the entities that use it and the wires and
 *             Timelines pointing at them (modmap.h)
 * The actions are install a folder, rebuild one, uninstall one (the open map
 * cleaned of it first), attach a mod to the open map (a map-level row,
 * modmanifest.h) and remove a mod from it, their dry runs, the registry
 * choice, rescan (the tab's Refresh: there is no folder watcher, so Refresh
 * is how edits are picked up), New mod, the read-only Files view, and the
 * console's verify and palette rebuild. What the tab does not show (every
 * folder's state, conflicts with their winners, the game files mods replace,
 * verification) is printed by sh_modui_doctor, the `sh_mods doctor` verb.
 *
 * Threads: every operation except FETCH and ROOT runs on DOOM's main thread
 * (the frontend calls the slot inside run_on_main), serialized with the
 * sh_mods console command and the map-load conversion that also use the store.
 * The document and the action result are kept in one buffer that FETCH copies
 * on any thread.
 *
 * File work only, apart from the bound engine operations below. */
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

/* Engine operations, bound by iface_engine.c in the product and by fakes in
 * tests. A NULL member takes the product default where there is one (rescan,
 * palette rebuild and the stock reader; none in a SH_MODUI_PURE build) and is
 * otherwise unavailable: the matching part of the document is omitted or the
 * action refuses. */
struct sh_modserve_refresh;
struct sh_modui_map_info;

typedef struct sh_modui_engine {
    int (*on_main_thread)(void);        /* 1 main, 0 other, negative unknown */
    /* Emit every placed name of the open editor map: each entity's inherit
     * and the string values of its decl text. Returns the number of entities
     * read, or -1 when no editor map is open. Main thread only. */
    int (*placed_names)(sh_modui_name_fn emit, void *context);
    int (*rescan)(void);                /* sh_modserve_index_rescan */
    /* Propagate what the rescan changed into the running game
     * (sh_modserve_refresh_after_rescan): 0 nothing changed, 1 applied,
     * 2 deferred. Main thread, after rescan. */
    int (*refresh)(struct sh_modserve_refresh *out);
    int (*palette_rebuild)(void);       /* modserve_palette_rebuild_requested: 1 ran, 0 refused */
    /* Existence of an installed game resource: 1 present, 0 absent, -1 the
     * reader is unavailable (stock shadows and needs cannot be checked). */
    int (*stock_exists)(const char *request);
    /* The open editor map serialized as a save would (no save side effects),
     * in a process-heap buffer the caller frees with HeapFree. 1 with *out,
     * 0 no editor map, -1 failed. Main thread only. */
    int (*map_json)(char **out, size_t *len);
    /* The open map's saved-map ids and search tag (the editor's own model,
     * which the next local save writes). 1 when a map is open; tagged is 1,
     * 0, or -1 unknown. Main thread only. */
    int (*map_info)(struct sh_modui_map_info *out);
    /* Delete editor entities (uniqueIds) through DOOM's own Delete Selection
     * in one undo step (sh_editor_delete_batch). Returns its status
     * (SH_EDITOR_DELETE_*: 1 dispatched). Main thread only. */
    int (*delete_entities)(const int *ids, int count);
    /* Remove `tag` from the open map's tags. 1 removed, 0 absent, -1 failed. */
    int (*remove_tag)(const char *tag);
    /* 1 with the cause when the game cannot take a change now (Play is
     * running, a map is loading), else 0. */
    int (*busy)(char *cause, size_t cap);
    /* 1 when the open editor map holds the entity with this uniqueId. Main
     * thread. Bound by iface_engine.c; nothing reads it since the undo watch
     * was retired with the recycle bin. */
    int (*entity_valid)(int uid);
    /* The editor toast with a title and literal text; nothing outside the editor. */
    void (*toast)(const char *title, const char *text);
    /* One line in the game console (no newline). */
    void (*print)(const char *line);
    /* Bring a map-scope change into the running game (modserve.h,
     * sh_modserve_scope_apply; none in a SH_MODUI_PURE build): 1 applied,
     * 0 nothing pending, -1 refused. Main thread. */
    int (*scope_apply)(const char *phase);
    /* The same for a declared set the author changed (map add, upgrade):
     * the declared builds' decls are registered first, as a map load does
     * (modserve.h sh_modserve_scope_activate). Unbound: scope_apply. */
    int (*scope_activate)(const char *phase);
    /* Whether the editor has left the map it had open, asked when the editor
     * screen is down (map_info 0): 1 left (no editor state runs: the hub or
     * the map browser, or the editor is exiting or shutting down), 0 a menu
     * screen over the open map (the Start menu's Settings), -1 unknown. The
     * map scope's exit waits for 1 (or -1, or no op bound). Main thread. */
    int (*map_left)(void);
    /* Empty the open map's undo and redo history to the current state
     * (editor_undo.h sh_editor_undo_clear; none in a SH_MODUI_PURE build).
     * probe 1: 1 possible, 0 no editor map, -1 unavailable; probe 0: 1
     * cleared, 0 no editor map, -1 failed. Main thread. */
    int (*clear_undo)(int probe);
    /* Mark the open map modified, as a placed entity does: one undo step
     * named `label` through the editor's own commit (editor_undo.h
     * sh_editor_mark_modified; none in a SH_MODUI_PURE build), so leaving the
     * editor asks to save and the map's next save writes the change. 1
     * marked, 0 no editor map, -1 unavailable. Main thread. */
    int (*mark_modified)(const char *label);
    /* 1 while a native editor action has not committed yet
     * (sh_editor_delete_native_pending): the Uninstall's undo clear waits for
     * the delete's own commit. */
    int (*undo_pending)(void);
    /* A hub launch's pre-load pass (modserve.h sh_modserve_launch_preload):
     * called once per launch by sh_modui_scope_launch, before the launched
     * map's listing and the PLAY mode are set, while the game is still at
     * the hub. Unbound: nothing (the level load's own passes parse). */
    int (*launch_preload)(const char *why);
} sh_modui_engine;

typedef struct sh_modui_map_info {
    char local_id[64];     /* the saved-map id, "" before the first save */
    char cloud_id[64];     /* its cloud-saved item id, "" when none */
    int  tagged;           /* the search tag "mods": 1, 0, or -1 unknown */
    /* The rawmap file the open map was opened from, while no save has named
     * it (sh_rawmap_open_unsaved), else "". The edit's ids then still name
     * the saved map the reload borrowed, so they are not the open map's. */
    char opened_from[260];
} sh_modui_map_info;

/* Bind the engine operations. NULL binds only the product defaults: no
 * main-thread check and no placed names. */
void sh_modui_bind(const sh_modui_engine *engine);

/* Build the document into the shared buffer. Returns its length, or -1 when
 * refused (off the main thread) or out of memory. */
int  sh_modui_list(void);

/* Run one action (SH_MODS_ACT_*) and keep its result document
 * {"action","ok","message",...} in the shared buffer. `arg` is the folder for
 * INSTALL (absolute; several separated by newlines), a row's key (its folder
 * name under mods\) for REBUILD, UNINSTALL and the map actions, and "" or a
 * key for VERIFY (the console's check). UNINSTALL, in order: the open map's
 * entities of the mod through the native delete (refused, with nothing
 * deleted, while Play runs, a map loads or the editor is busy), the open
 * map's undo and redo history cleared to the current state (after the
 * delete's own commit, on the tick), the folder moved to the Windows Recycle
 * Bin (sh_ms_delete_mod_folder; deleted outright, and the result says so,
 * when the bin cannot take it), the rescan. The product never brings a
 * folder back; the player can restore one from the bin by hand. The retired
 * actions (MIGRATE, PRUNE_*, REMOVE, REMOVED_*) answer a refusal. Returns the
 * result length, or -1 when refused before the action ran.
 *
 * MAP_ADD (arg: a key) attaches that mod to the open map: the current
 * manifest gets a map-level row for the mod's build, the map scope is
 * recomputed and applied at once, and the next save writes the row (so the
 * map publishes, downloads and lists the mod); MAP_REMOVE drops the row
 * again, deleting nothing when no entity uses the mod. A mod a downloaded map
 * brought (a place folder) serves only that map and is not attached.
 *
 * Map scope: binding also registers the manifest's scope hook, its row
 * resolver and the serving layer's scope source. At every load the listing is
 * the rows and the placed names, applied at once; at every save the rows
 * written, applied on the next editor tick; at the editor's Play entry the
 * current rows and the live placed names; at a hub launch the launched map's
 * rows alone (sh_modui_scope_launch). The tick (sh_modui_poll_main) clears
 * the listing and the current rows when the editor that had the map open is
 * gone (the hub) or a hub launch's Play has run and is over (its Play began
 * and the game was busy with it, then idle; an idle poll before that, such
 * as the gap between the gate and the level load, keeps them), and sets the
 * listing again when an editor map is open without one. A load activates
 * its rows and the builds its placed names list beyond them
 * (sh_modmanifest_set_listed).
 *
 * The uninstall's dry run reports, with the consequences, how many local
 * saves (My Maps, savegame.user\<profile>\SNAPMAPS<id>\map.decl) carry a
 * row of the mod ("savedMaps") and whether the undo history is cleared
 * ("undoCleared": a map is open).
 *
 * FILES (arg: a key) lists the mod folder's files and the game files its
 * build reads from the install: {key, name, cause, files: [{path, size,
 * kind, class, game, container, inFolder, cause}], truncated}. FILE_TEXT
 * (arg: "<key>\t<path below the folder>") answers one text file, at most
 * 1 MiB of UTF-8 ({key, path, size, text, productBlock, cause}; mod.json
 * without the part Snapmap+ writes); a path outside the folder, a binary
 * kind and a file the folder does not hold are refused. Both read only and
 * change nothing. NEW_MOD (arg: a display name) makes a mod folder with a
 * working example (sh_ms_new_mod_example) and rescans, so its first build
 * runs: {key, file: the example Toybox row's path}. */
int  sh_modui_run(int action, const char *arg);

/* The console verbs that share the tab's dry runs and actions:
 *   map                         the open map: details, then each mod it uses
 *                               with its placed entities and references and
 *                               whether the whole map uses it
 *   map add "<mod>"             attach the mod to the open map (a map-level row)
 *   map remove "<mod>" [--yes]  the dry run; with --yes it is performed
 *   uninstall "<mod>" [--yes]   the dry run; with --yes it is performed
 * where <mod> is a mod's folder (its path under mods\, or a nested folder's
 * own name). Lines name mods by their names and folders.
 * argv[0] is the verb. Lines start "MODS: ". Returns 1 when the verb was
 * handled (whatever its outcome), 0 for a verb that is not one of these, -1
 * off the main thread. */
int  sh_modui_command(int argc, const char *const *argv, sh_modui_line_fn line, void *context);

/* The main-thread tick after each native frame: sh_modui_poll_due is cheap
 * (nothing owed, or under 250 ms since the last map-scope tick: 0; every
 * frame while an Uninstall's undo clear is owed); when it returns 1 the
 * caller runs sh_modui_poll_main inside the process heap scope its
 * main-thread calls use. The tick waits out Play and loads, then clears an
 * owed undo history once the editor's delete has committed, and runs the map
 * scope's part. */
int  sh_modui_poll_due(void);
int  sh_modui_poll_main(void *context);

/* A hub launch (a My Maps play, a published launch) passed its gate: the map
 * scope declares exactly the launched map's rows (its parse made them the
 * current manifest; the editor's exit cleared the editor map's), and the
 * serving mode is Play. The Play entry then keeps this listing instead of
 * reading the editor, and the tick clears it once that Play has run and is
 * over (launched, loading, playing, running, ended: modui.c). Main thread.
 * Returns the builds whose serving changed. sh_modui_scope_launch keeps the
 * serving context the gate set; sh_modui_scope_launch_at first sets it
 * (sh_modserve_scope_set_place): `place` the launched map's place
 * ("maps\<segment>", NULL for none) and `own` 1 for the player's own map (a
 * My Maps play, a map this machine published), whose rows follow the root
 * folders. When the editor leaves its map, or a launched Play is over, the
 * context returns to no place, own. */
int  sh_modui_scope_launch(const char *why);
int  sh_modui_scope_launch_at(const char *why, const char *place, int own);

/* A hub launch the published-map gate refused (denied, or dropped from its
 * hold): nothing was launched, so its parse's rows are not declared. With
 * the editor still showing a map, that map's rows come back
 * (sh_modmanifest_current_restore_previous); elsewhere the current manifest
 * is closed and nothing is declared. The serving mode returns to EDITOR.
 * Main thread. Returns the bundles whose serving changed. */
int  sh_modui_scope_launch_refused(const char *why);

/* The slot's WAIT: nothing of the tab runs in the background (no migration,
 * no folder watcher), so it answers 1 (idle) at once. Any thread. */
int  sh_modui_wait(unsigned ms);


/* `sh_mods doctor [verify]`: every mod folder with its state and its plain
 * cause, conflicts with their winners, the game files mods replace, and with
 * `verify` a check of every mod's files. Each line starts "MODS: doctor:".
 * Returns 1, or -1 off the main thread or out of memory. */
int  sh_modui_doctor(int verify, sh_modui_line_fn line, void *context);

/* Copy the shared buffer. Returns 1 copied, -1 when out is NULL or too small
 * (*needed set), 0 when nothing has been built. */
int  sh_modui_fetch(char *out, int out_cap, int *needed, unsigned *generation);

/* Bumped by every action that can change the store or the serving index. */
unsigned sh_modui_generation(void);

/* A change the tab did not make (a console verb: sh_mods build, rebuild,
 * install, rescan): bumps the generation, so the window pushes a fresh
 * document to an open Mods tab, as after the tab's own actions. The verbs
 * the tab shares (map, uninstall, choose) bump it themselves. Any thread. */
void sh_modui_note_changed(void);

/* The interface slot body (see snapmap_plus_iface.h, `mods`). */
int  sh_modui_slot(struct sh_iface *self, struct sh_mods_call *call);

#ifdef SH_MODUI_TESTING
/* The hub launch state machine (modui.c): "none", "launched", "loading",
 * "playing", "running" or "ended". */
const char *sh_modui_test_launch_state(void);
/* Where saved maps are read (SNAPMAPS<id>\game.details under each child
 * folder); NULL restores Saved Games\id Software\DOOM\base\savegame.user. */
void sh_modui_test_set_saves_root(const char *path);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_MODUI_H */
