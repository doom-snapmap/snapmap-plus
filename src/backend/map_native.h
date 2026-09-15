/* Owned published-map metadata and temporary native snapshots. */
#ifndef BACKEND_MAP_NATIVE_H
#define BACKEND_MAP_NATIVE_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

typedef struct sh_map_native_selection sh_map_native_selection;
/* Bind verified native constructors and destructors without executing them. */
int sh_map_native_bind(const sig_result *results, size_t count, const uint8_t *module_base);
/* Main engine thread. Deep-copy the metadata and retain the exact cached JSON.
 * Neither operation parses a map, activates packages or changes selection. */
sh_map_native_selection *sh_map_native_capture(const void *metadata, char *error, size_t capacity);
const char *sh_map_native_source(const sh_map_native_selection *selection, size_t *length);
/* Compare both native map identifiers by value, never by object address. */
int sh_map_native_matches(const sh_map_native_selection *selection, const void *metadata);
int sh_map_native_matches_ids(const sh_map_native_selection *selection, const void *id, const void *revision);
int sh_map_native_matches_level(const sh_map_native_selection *selection, const void *level_parameters);
/* Decode after package activation, then lend the native metadata/snapshot to a
 * synchronous original launch. They may not escape the callback except through
 * a native deep copy. Return 1 on success. A failed parse never calls launch.
 * The callback returns 1 for success, 0 for refusal. Snapshot cleanup is always
 * attempted after construction, including callback exceptions. */
typedef int (*sh_map_native_enter_fn)(void *context, const void *metadata, void *snapshot);
int sh_map_native_enter(const sh_map_native_selection *selection, const char *json,
    sh_map_native_enter_fn enter, void *context, char *error, size_t capacity);
/* Preliminary session/lobby launch: the native empty snapshot receives only
 * the authored lobby slots. The full retained map must replace the cache after
 * package activation, before native gameplay conversion. */
int sh_map_native_session(const sh_map_native_selection *selection,
    sh_map_native_enter_fn enter, void *context, char *error, size_t capacity);
/* Initial published read, on the engine thread and inside an inspection scope.
 * The caller owns a newly constructed empty snapshot. Populate only lobby
 * settings, without resolving authored entities or taking snapshot ownership. */
int sh_map_native_read_session(const char *json, void *snapshot, char *error, size_t capacity);
/* Main engine thread; leave ownership in *selection when its allocator scope
 * cannot be entered. Never retry a destructor that may have partially run. */
int sh_map_native_release(sh_map_native_selection **selection);
#endif
