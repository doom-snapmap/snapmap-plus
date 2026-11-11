/* Inject custom #str_ mappings into the native language dictionary. Read the
 * extra.strings.en object of every admitted mod bundle (sh_modserve_each_bundle_extra: root folders
 * before maps' places, then salt, then folder), then baked defaults. A mod's
 * own keys come only from a build that serves its names for the open map
 * (another build of its salt is inert there), its string edits only where its
 * edits apply. Keys are case-insensitive and first-writer-wins; two bundles
 * giving one key different text log one REFUSED line naming both. Use the
 * native hash, string pool and list append functions, then re-sort the table.
 *
 * Installation supplies the text; a rearm re-reads it. Each pass gathers
 * every row first and plans the whole change (update a row this injector
 * owns, adopt an existing game row keeping its original record, append a new
 * one, retire one no layer wants) with all its memory before touching the
 * table, so a pass that cannot complete leaves the previous table whole. A
 * row the table no longer holds went with the engine's own rebuild of its
 * dictionary and is added again. Engine-pool text is never freed. IDs and
 * bookkeeping grow dynamically; native signed count/length fields and
 * successful allocation bound injection, not fixed application quotas.
 */
#ifndef BACKEND_STRIDS_H
#define BACKEND_STRIDS_H

#include <stdint.h>
#include <stddef.h>

/* Install the sort-body detour and attempt immediate injection/resort.
 * sort_status_ok must be clean SIG_OK; missing dependencies or a hooked sort
 * prologue refuse. table_lea_fn locates the descriptor; insert_fn, hash_fn
 * and idstr_ctor_fn provide the native list/hash/string-pool operations.
 * Returns 1 when the detour is installed; injection count is logged
 * separately.
 */
int sh_strids_install(void *sort_body_fn, int sort_status_ok,
                      void *table_lea_fn, void *insert_fn, void *hash_fn, void *idstr_ctor_fn);

/* Re-read the layers and re-sort on the engine main thread, inside the
 * game's process heap (the table's own heap: an append that grows it
 * allocates from the current heap, and a map's heap is freed with the map).
 * Returns 0 if the process heap could not be selected (the refresh stays
 * pending), or reading, native mutation, or sorting failed. */
int sh_strids_rearm(void);

/* The engine's heap-scope functions (process_heap_scope.h), bound by the
 * serving engine at install; a rearm without them does not run. */
struct sh_process_heap_api;
void sh_strids_set_process_heap(const struct sh_process_heap_api *api);

/* Any thread: a serving index was published (boot, rescan, install, migrate),
 * so the bundle layer may have changed. Only marks the refresh pending. */
void sh_strids_request_refresh(void);

/* Any thread: a mod's own keys apply only where its build serves its names
 * and its string edits only where its edits apply
 * (sh_modserve_bundle_edits_live). When either changed for a bundle with
 * strings since the last pass (a map opened or closed, another map
 * declared), mark the refresh pending. Returns 1 when marked. */
int sh_strids_scope_check(void);

/* Engine main thread: when a refresh is pending and the first injection has
 * run, clear the request and rearm. Returns -1 when nothing ran (no request,
 * or the first injection is still to come and will read the index itself),
 * else what sh_strids_rearm returned. */
int sh_strids_refresh_pending(void);

#ifdef SH_STRIDS_TESTING
/* Bind test doubles for the four engine entry points and run one inject pass; returns rows appended. */
int sh_strids_test_inject(void *table_desc, void *insert, void *hash, void *idstr_ctor);
void sh_strids_test_set_sort(void *sort);
/* Read back how row `index` was attributed: its id and its owner ("<baked>"
 * for the baked defaults, the salt for a bundle). */
int sh_strids_test_row(int index, const char **id_out, const char **owner_out);
#endif

#endif /* BACKEND_STRIDS_H */
