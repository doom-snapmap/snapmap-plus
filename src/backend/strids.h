/* Inject custom #str_ mappings into the native language dictionary. Read the
 * user strings/strids.json, installed package strings/*.json, then baked
 * defaults. Keys are case-insensitive and first-writer-wins. Use the native
 * hash, string pool and list append functions, then re-sort the table.
 *
 * Injection runs once during installation because the normal startup sort has
 * already passed. A sort-body detour provides a one-shot fallback without
 * duplicating rows on recursive or later sorts.
 */
#ifndef BACKEND_B1_STRIDS_H
#define BACKEND_B1_STRIDS_H

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

/* Set the user document path; NULL restores %LOCALAPPDATA%\snapmap-
 * plus\strings\strids.json. Returns 1 when set.
 */
int sh_strids_set_source(const char *path);

/* How many #str_ rows the injector has appended (observability for the test harness). */
unsigned long sh_strids_injected_count(void);

#ifdef SH_STRIDS_TESTING
/* Bind test doubles for the four engine entry points and run one inject pass; returns rows appended. */
int sh_strids_test_inject(void *table_desc, void *insert, void *hash, void *idstr_ctor);
/* Read back how row `index` was attributed: its id and its owning package ("<user>" if not a package). */
int sh_strids_test_row(int index, const char **id_out, const char **owner_out);
#endif

#endif /* BACKEND_B1_STRIDS_H */
