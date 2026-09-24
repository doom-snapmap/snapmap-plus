/* Inject custom #str_ mappings into the native language dictionary. Read the
 * user strings/strids.json, compiled package.json strings, then baked
 * defaults. Keys are case-insensitive and first-writer-wins. Use the native
 * hash, string pool and list append functions, then re-sort the table.
 *
 * Installation supplies local and baked text before package compilation.
 * Published package text joins on runtime rearm, which updates keys owned by this
 * injector and appends new keys before sorting. Existing vanilla keys are
 * replaced in place with their original records retained. Removing a mapping
 * restores that record or removes a newly introduced row; engine-pool text is
 * never freed. Complete input and ownership validation precedes retirement.
 * IDs and bookkeeping grow dynamically; native signed count/length fields
 * and successful allocation bound injection, not fixed application quotas.
 */
#ifndef BACKEND_B1_STRIDS_H
#define BACKEND_B1_STRIDS_H

#include <stdint.h>
#include <stddef.h>
#include "package_strings.h"

/* Main-thread, read-only reservations using the installed native hash and
 * dictionary, plus user/baked keys. No native row is inserted by this service.
 * Keep the transaction through candidate preparation, validate at activation,
 * and free it on success or failure. Exact owned mappings may reuse aliases. */
typedef struct sh_strids_reservation sh_strids_reservation;
sh_strids_reservation *sh_strids_reservation_open(char *error, size_t capacity);
sh_package_string_reservations sh_strids_reservation_adapter(sh_strids_reservation *reservation,
    size_t max_attempts);
int sh_strids_reservation_validate(const sh_strids_reservation *reservation, char *error, size_t capacity);
struct sh_package_policy;
/* Seed all private and historical/global keys of another pending provider.
 * Call before allocating peer aliases, then check the final combined policy. */
int sh_strids_reservation_policy(sh_strids_reservation *reservation,
    const struct sh_package_policy *policy, char *error, size_t capacity);
void sh_strids_reservation_free(sh_strids_reservation *reservation);

typedef struct sh_strids_localization {
    void *context;
    /* Complete verified locale order, stable until this synchronous refresh
     * returns. No implicit English or regional fallback is added. */
    int (*locales)(void *context, const char *const **order, size_t *count,
        char *error, size_t capacity);
    /* Original native lookup result under that same language snapshot, including
     * the engine's missing-key behavior. Return malloc-owned exact native bytes;
     * do not read a package-overridden row or transcode a byte string. */
    int (*original)(void *context, const char *key, unsigned char **text, size_t *length,
        char *error, size_t capacity);
} sh_strids_localization;
/* Bind before publishing scoped strings; callback state outlives every rearm.
 * A language/dictionary change requires rearm at the admitted native boundary. */
void sh_strids_set_localization(const sh_strids_localization *localization);
/* Production adapter for the verified single native language table. The
 * resolved cvar-system pointer slot is borrowed; no native setter is called.
 * Known language names accept the documented descriptor locale aliases, with
 * no implicit English translation. Original values bypass injector-owned rows. */
int sh_strids_bind_native_localization(const void *cvar_system_slot);

/* Install the sort-body detour and attempt immediate injection/resort.
 * sort_status_ok must be clean SIG_OK; missing dependencies or a hooked sort
 * prologue refuse. table_lea_fn locates the descriptor; insert_fn, hash_fn
 * and idstr_ctor_fn provide the native list/hash/string-pool operations.
 * Returns 1 when the detour is installed; injection count is logged
 * separately.
 */
int sh_strids_install(void *sort_body_fn, int sort_status_ok,
                      void *table_lea_fn, void *insert_fn, void *hash_fn, void *idstr_ctor_fn);

/* Refresh on the engine main thread at the package-registration boundary.
 * Returns 0 if discovery, native mutation, or sorting failed. */
int sh_strids_rearm(void);

/* Set the user document path; NULL restores %LOCALAPPDATA%\snapmap-
 * plus\strings\strids.json. Returns 1 when set.
 */
int sh_strids_set_source(const char *path);

/* How many #str_ rows the injector has appended (observability for the test harness). */
unsigned long sh_strids_injected_count(void);

#ifdef SH_STRIDS_TESTING
/* Bind test doubles for the four engine entry points and run one inject pass; returns rows appended. */
int sh_strids_test_inject(void *table_desc, void *insert, void *hash, void *idstr_ctor);
void sh_strids_test_set_sort(void *sort);
/* Exercise the native parser's sort boundary after replacing the test table. */
int sh_strids_test_native_sort(void);
/* Read back how row `index` was attributed: its id and its owning package ("<user>" if not a package). */
int sh_strids_test_row(int index, const char **id_out, const char **owner_out);
#endif

#endif /* BACKEND_B1_STRIDS_H */
