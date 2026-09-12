/* Engine reflection, decl commits, prefab staging, and command-drain routing.
 * Callers edit serialized text; this module owns engine objects and lifetimes. */
#ifndef B2_APPLY_ENGINE_H
#define B2_APPLY_ENGINE_H

#include <stdint.h>
#include "signatures.h"
#include "snapmap_plus_iface.h"
#include "nav_regions.h"

/* Resolve dependencies once after signatures and typeinfo bind. cmdsys may be
 * null, disabling deferred transport. Return 1 for a complete binding; each
 * slot checks its own dependencies when installation is partial. */
int sh_apply_engine_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys);

/* Most recent kind-2 result: 0 stage failed, 1 staged-only, 2 native paste
 * action armed. Arming does not confirm consumption or entry into grab; the
 * engine runs the action on a later frame and the user positions the result. */
int sh_apply_last_place_result(void);

/* Maintenance after a successful native Frame, outside recovery. Unknown or
 * off-main callers do nothing. On leaving RUNNING, preserve verified persistent
 * storage or reset our matching unsafe staged-prefab slot. */
void sh_apply_prefab_poll_play(void);

/* Commit a module-qualified target reference to the source inline, despite
 * the schedule name. Called on the engine main thread by bare-target wiring. */
void ae_schedule_target_write(int source_id, int target_id);

/* Export apply slots for the single shared-interface engine binding. */
void sh_apply_engine_get_slots(sh_serialize_entity_fn *serialize_entity,
                               sh_schedule_apply_fn   *apply_edit,
                               sh_read_prefab_fn      *read_prefab,
                               sh_apply_sync_fn       *apply_sync,   /* +0x290 synchronous apply, with main-thread marshal when available. */
                               sh_normalize_timeline_inherit_fn *normalize_timeline_inherit); /* +0x298 */

/* Export selection-to-prefab serialization for the shared interface. */
void sh_apply_engine_get_serialize_selection(sh_serialize_selection_fn *serialize_selection);

/* Live entity callbacks shaped for nav_regions, without a reverse link dependency. */
int sh_apply_engine_entity_count(void *ctx);
int sh_apply_engine_entity_valid(int id, void *ctx);
int sh_apply_engine_entity_json(int id, char *out, int cap, void *ctx);
/* A complete current edit map, including live instanceEntities ownership.
 * Main thread only. Successful output is malloc-owned by the caller. */
int sh_apply_engine_nav_snapshot(char **out, size_t *len, void *ctx);
/* Navigation geometry with collision dimensions resolved by the native entity
 * declarations in that same temporary snapshot. Main thread only. */
int sh_apply_engine_nav_regions(sh_nav_map *out, void *ctx);

/* Time both ways of reading current editor geometry and report them. Main
 * thread, editor open; costs one whole-map read plus one read per entity. */
void sh_apply_engine_read_probe(void (*out)(const char *fmt, ...));

#endif /* B2_APPLY_ENGINE_H */
