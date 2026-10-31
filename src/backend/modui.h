/* modui.h -- the WebView Mods tab's backend (the `mods` interface slot).
 *
 * One JSON document describes what a player needs to see:
 *   mods      one row per installed mod. The store's folders (modstore
 *             enumeration) are grouped by id; the row shows the build that
 *             serves (sh_modserve_bundle_state, sh_modserve_lookup), else the
 *             newest, and counts the other builds. Two folders declaring one
 *             id (the overrides folder a migrated bundle came from, or a
 *             folder built in place, differ) are a row each, marked
 *             duplicateId. A row is "installed" when its build serves with no
 *             warning, else "attention" with the first cause. Map-delivered
 *             bundles under legacy\ are not listed.
 *   migrate   whether the retired overrides\ folder holds entries the
 *             migration record does not cover (sh_modmigrate_pending)
 *   map       the manifest rows the last map load carried (modmanifest's
 *             current record), each with its state under the asset rule
 *             (modserve.h sh_modserve_row_served: active, superseded,
 *             incomplete, skipped, missing) and the refusal sentence, and the bundles the placed entities resolve to now (the
 *             save-time closure, modserve_bundles_for_names_visit); with the
 *             map's JSON bound (payload v3) also its name, saved-map id, last
 *             save, published and uploaded state (the record's links and
 *             claims), the search tag, and one entry per mod it uses with the
 *             entities that use it and the wires and Timelines pointing at
 *             them (modmap.h)
 * The actions are install a folder, rebuild one, verify one, uninstall one
 * (the open map cleaned of it first), attach a mod to the open map (a
 * map-level row, modmanifest.h) and remove a mod from it, their dry runs,
 * migrate, rescan, and the older palette rebuild, Remove and removal
 * of covered builds, which the tab no longer offers. What the tab stopped showing (every bundle's state,
 * legacy ones included, conflicts with their winners, shadows, verification)
 * is printed by sh_modui_doctor, the `sh_mods doctor` console verb.
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
    /* The overrides migration (modmigrate.h; none in a SH_MODUI_PURE build). */
    int (*migrate_pending)(unsigned *entries);   /* sh_modmigrate_pending */
    int (*migrate_start)(int again);             /* sh_modmigrate_start */
    int (*migrate_busy)(void);                   /* sh_modmigrate_busy */
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
    /* 1 while a native editor action has not committed yet
     * (sh_editor_delete_native_pending): the Uninstall's undo clear waits for
     * the delete's own commit. */
    int (*undo_pending)(void);
} sh_modui_engine;

typedef struct sh_modui_map_info {
    char local_id[64];     /* the saved-map id, "" before the first save */
    char cloud_id[64];     /* its cloud-saved item id, "" when none */
    int  tagged;           /* the search tag "mods": 1, 0, or -1 unknown */
} sh_modui_map_info;

/* Bind the engine operations. NULL binds only the product defaults: no
 * main-thread check and no placed names. */
void sh_modui_bind(const sh_modui_engine *engine);

/* Build the document into the shared buffer. Returns its length, or -1 when
 * refused (off the main thread) or out of memory. */
int  sh_modui_list(void);

/* Run one action (SH_MODS_ACT_*) and keep its result document
 * {"action","ok","message",...} in the shared buffer. `arg` is the folder for
 * INSTALL (absolute), the key for VERIFY ("" verifies every installed build),
 * newline-separated keys for PRUNE_APPLY and a row's key for REMOVE, which
 * removes every build of that row unless the open map uses one. REMOVE,
 * PRUNE_APPLY and UNINSTALL move the folders to the Windows Recycle Bin
 * (sh_ms_delete_mod_folder; deleted outright, and the result says so, when the
 * bin cannot take one) and drop their aliases; the product never brings them
 * back, the player can restore one from the bin by hand. UNINSTALL, in order:
 * the open map's entities of the mod through the native delete (refused, with
 * nothing deleted, while Play runs, a map loads or the editor is busy), the
 * open map's undo and redo history cleared to the current state (after the
 * delete's own commit, on the tick), the folder moved to the Recycle Bin,
 * the rescan. REMOVED_EMPTY and REMOVED_RESTORE are retired with the
 * product's own bin and answer a refusal. MIGRATE starts the migration on its worker (sh_modui_wait
 * follows it). Returns the result length, or -1 when refused before the
 * action ran.
 *
 * MAP_ADD (arg: a key, or an installed id) attaches that mod to the open map:
 * the current manifest gets a map-level row for the build that serves, the
 * map scope is recomputed and applied at once, and the next save writes the
 * row (so the map publishes, downloads and lists the mod); MAP_REMOVE drops
 * the row again, deleting nothing when no entity uses the mod.
 *
 * Map scope: binding also registers the manifest's scope hook, its row
 * resolver and the serving layer's scope source. At every load the listing is
 * the rows and the placed names, applied at once; at every save the rows
 * written, applied on the next editor tick; at the Play entry the current
 * rows and the live placed names. The tick (sh_modui_poll_main) clears the
 * listing when the editor that had the map open is gone (the hub), and sets
 * it again when an editor map is open without one.
 *
 * The uninstall's dry run reports, with the consequences, how many local
 * saves (My Maps, savegame.user\<profile>\SNAPMAPS<id>\map.decl) carry a
 * row of the mod ("savedMaps") and whether the undo history is cleared
 * ("undoCleared": a map is open). */
int  sh_modui_run(int action, const char *arg);

/* The console verbs that share the tab's dry runs and actions:
 *   map                         the open map: details, then each mod it uses
 *                               with its placed entities and references, whether
 *                               it is map-level, its scope and whether it is
 *                               active, then every bundle's scope
 *   map add "<key|id>"          attach the mod to the open map (a map-level row)
 *   map remove "<key|id>" [--yes]   the dry run; with --yes it is performed
 *   uninstall "<key>" [--yes]       the dry run; with --yes it is performed
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

/* Wait up to `ms` for a migration to finish. 1 idle, 0 still running. Any thread. */
int  sh_modui_wait(unsigned ms);


/* `sh_mods doctor [verify]`: every store folder with its state and cause
 * (legacy ones included), duplicate ids, the migration's pending entries,
 * conflicts with their winners, shadows, and with `verify` a rehash of every
 * build. Each line starts "MODS: doctor:". Returns 1, or -1 off the main
 * thread or out of memory. */
int  sh_modui_doctor(int verify, sh_modui_line_fn line, void *context);

/* Copy the shared buffer. Returns 1 copied, -1 when out is NULL or too small
 * (*needed set), 0 when nothing has been built. */
int  sh_modui_fetch(char *out, int out_cap, int *needed, unsigned *generation);

/* Bumped by every action that can change the store or the serving index. */
unsigned sh_modui_generation(void);

/* The interface slot body (see snapmap_plus_iface.h, `mods`). */
int  sh_modui_slot(struct sh_iface *self, struct sh_mods_call *call);

#ifdef SH_MODUI_TESTING
void sh_modui_test_reset(void);
/* Where saved maps are read (SNAPMAPS<id>\game.details under each child
 * folder); NULL restores Saved Games\id Software\DOOM\base\savegame.user. */
void sh_modui_test_set_saves_root(const char *path);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_MODUI_H */
