/* Injects custom #str_ mappings into the native language dictionary from the extra.strings.en of every admitted
 * mod bundle (sh_modserve_each_bundle_extra), then baked defaults; keys are case-insensitive, first writer wins, and
 * two bundles giving one key different text log a REFUSED line. A mod's own keys apply only from the build that
 * serves its names for the open map, its string edits only where its edits apply. Each pass plans the whole change
 * (update an owned row, adopt a game row, append, retire) before touching the table, so a pass that cannot complete
 * leaves the table whole; a row the engine's own dictionary rebuild dropped is added again, and pool text is never freed. */
#ifndef BACKEND_STRIDS_H
#define BACKEND_STRIDS_H

#include <stdint.h>
#include <stddef.h>

/* Install the sort-body detour and try an immediate injection. Refuses unless sort_status_ok is a clean SIG_OK
 * and the sort prologue is unhooked; table_lea_fn locates the descriptor, insert_fn/hash_fn/idstr_ctor_fn are the
 * native list, hash and string-pool operations. Returns 1 when the detour is installed. */
int sh_strids_install(void *sort_body_fn, int sort_status_ok,
                      void *table_lea_fn, void *insert_fn, void *hash_fn, void *idstr_ctor_fn);

/* Re-read the layers and re-sort on the engine main thread inside the game's process heap (the table's own;
 * a map's heap is freed with the map). Returns 0 if that heap could not be selected (the refresh stays
 * pending) or reading, mutation or sorting failed. */
int sh_strids_rearm(void);

/* The engine's heap-scope functions (process_heap_scope.h), bound by the
 * serving engine at install; a rearm without them does not run. */
struct sh_process_heap_api;
void sh_strids_set_process_heap(const struct sh_process_heap_api *api);

/* Any thread: a serving index was published, so the bundle layer may have changed. Only marks the refresh pending. */
void sh_strids_request_refresh(void);

/* Any thread: marks the refresh pending when, for a bundle with strings, where its own keys apply or where its
 * edits apply (sh_modserve_bundle_edits_live) changed since the last pass. Returns 1 when marked. */
int sh_strids_scope_check(void);

/* Engine main thread: when a refresh is pending and the first injection has run, clear it and rearm. Returns -1
 * when nothing ran (the first injection reads the index itself), else sh_strids_rearm's result. */
int sh_strids_refresh_pending(void);

#ifdef SH_STRIDS_TESTING
/* The install's table-global decode (the first LEA RCX,[rip+disp32] in StridsTableLea), on a real executable. */
void *sh_strids_test_decode_table_global(const uint8_t *table_lea_fn);
/* Bind test doubles for the four engine entry points and run one inject pass; returns rows appended. */
int sh_strids_test_inject(void *table_desc, void *insert, void *hash, void *idstr_ctor);
void sh_strids_test_set_sort(void *sort);
/* Read back row `index`'s id and owner ("<baked>" for baked defaults, else the bundle's salt). */
int sh_strids_test_row(int index, const char **id_out, const char **owner_out);
#endif

#endif /* BACKEND_STRIDS_H */
