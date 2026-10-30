/* modui.h -- the WebView Mods tab's backend (the `mods` interface slot).
 *
 * One JSON document describes the installed mods and the open map:
 *   bundles   every folder in the store (modstore enumeration) with its state
 *             in the serving index (sh_modserve_bundle_state, sh_modserve_lookup):
 *             active, legacy (an active converted package), superseded (a newer
 *             build of the same mod serves its paths), unindexed (installed
 *             after the last rescan) or skipped (with the first cause)
 *   map       the manifest rows the last map load carried (modmanifest's
 *             current record), each with whether its bundle is installed and
 *             active, and the bundles the placed entities resolve to now (the
 *             save-time closure, modserve_bundles_for_names_visit)
 *   shadows   request paths an admitted bundle shadows, grouped by path: stock
 *             shadows, legacy shadows and conflicts (several mods provide the
 *             path with different bytes: "resolution" says whether one copy was
 *             chosen, the list entries of all were composed, the winner was
 *             withdrawn, or nothing serves it, and "reason" says why)
 * and the actions are the sh_mods verbs the console runs: install a folder,
 * verify, rescan, palette rebuild, and the conservative removal of builds that
 * a newer build of the same mod fully covers.
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

/* Called once per placed name the open map holds (duplicates allowed). */
typedef void (*sh_modui_name_fn)(void *context, const char *name);

/* Engine operations, bound by iface_engine.c in the product and by fakes in
 * tests. A NULL member takes the product default where there is one (rescan,
 * palette rebuild and the stock reader; none in a SH_MODUI_PURE build) and is
 * otherwise unavailable: the matching part of the document is omitted or the
 * action refuses. */
typedef struct sh_modui_engine {
    int (*on_main_thread)(void);        /* 1 main, 0 other, negative unknown */
    /* Emit every placed name of the open editor map: each entity's inherit
     * and the string values of its decl text. Returns the number of entities
     * read, or -1 when no editor map is open. Main thread only. */
    int (*placed_names)(sh_modui_name_fn emit, void *context);
    int (*rescan)(void);                /* sh_modserve_index_rescan */
    int (*palette_rebuild)(void);       /* modserve_palette_rebuild_requested: 1 ran, 0 refused */
    /* Existence of an installed game resource: 1 present, 0 absent, -1 the
     * reader is unavailable (stock shadows and needs cannot be checked). */
    int (*stock_exists)(const char *request);
} sh_modui_engine;

/* Bind the engine operations. NULL binds only the product defaults: no
 * main-thread check and no placed names. */
void sh_modui_bind(const sh_modui_engine *engine);

/* Build the document into the shared buffer. Returns its length, or -1 when
 * refused (off the main thread) or out of memory. */
int  sh_modui_list(void);

/* Run one action (SH_MODS_ACT_*) and keep its result document
 * {"action","ok","message",...} in the shared buffer. `arg` is the folder for
 * INSTALL (absolute), the key for VERIFY ("" verifies every installed build)
 * and newline-separated keys for PRUNE_APPLY. Returns the result length, or -1
 * when refused before the action ran. */
int  sh_modui_run(int action, const char *arg);

/* Copy the shared buffer. Returns 1 copied, -1 when out is NULL or too small
 * (*needed set), 0 when nothing has been built. */
int  sh_modui_fetch(char *out, int out_cap, int *needed, unsigned *generation);

/* Bumped by every action that can change the store or the serving index. */
unsigned sh_modui_generation(void);

/* The interface slot body (see snapmap_plus_iface.h, `mods`). */
int  sh_modui_slot(struct sh_iface *self, struct sh_mods_call *call);

#ifdef SH_MODUI_TESTING
void sh_modui_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_MODUI_H */
