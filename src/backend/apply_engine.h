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

struct sh_package_occurrence_snapshot;
typedef struct sh_apply_prefab_stage sh_apply_prefab_stage;
/* DS calls prepare under its admitted engine-thread renderer/heap scope.
 * JSON is already source-bound and carrier-stripped. Preparation owns a real
 * temporary prefab and exact occurrence metadata; it leaves staging unchanged.
 * Finalize performs a reversible native transfer, calls commit last, and clears
 * *stage only on success. A commit refusal restores the previous staging slot.
 * Abort requires recovery scopes and clears *stage only after safe destruction.
 * Returns 1 success, 0 clean refusal, -1 quarantine. Prepare sets *out on 1 or
 * -1, so callers can retain incomplete native obligations. A commit exception
 * has unknown disk outcome and quarantines both native/source owners.
 * Non-NULL handles after failure retain their native/source ownership. After
 * successful disk commit, cleanup failure is retained internally and cannot
 * convert the completed transaction into a reported refusal.
 */
int sh_apply_prefab_stage_prepare(void *editor, const char *json,
    const struct sh_package_occurrence_snapshot *ownership, int place, sh_apply_prefab_stage **out,
    char *error, size_t capacity);
int sh_apply_prefab_stage_finalize(sh_apply_prefab_stage **stage,
    int (*commit)(char *error, size_t capacity), char *error, size_t capacity);
int sh_apply_prefab_stage_abort(sh_apply_prefab_stage **stage, char *error, size_t capacity);

/* Commit a module-qualified target reference to the source inline, despite
 * the schedule name. Called on the engine main thread by bare-target wiring. */
void ae_schedule_target_write(int source_id, int target_id);

/* Export apply slots for the single shared-interface engine binding. */
void sh_apply_engine_get_slots(sh_serialize_entity_fn *serialize_entity,
                               sh_schedule_apply_fn   *apply_edit,
                               sh_read_prefab_fn      *read_prefab,
                               sh_apply_sync_fn       *apply_sync,   /* +0x290 synchronous apply, with main-thread marshal when available. */
                               sh_normalize_timeline_inherit_fn *normalize_timeline_inherit); /* +0x298 */

/* Export the main-thread call slot. Calls drain in sh_apply_prefab_poll_play,
 * so they time out unrun while the frame hook is missing or suppressed. */
void sh_apply_engine_get_run_on_main(sh_run_on_main_fn *run_on_main);

/* Export selection-to-prefab serialization for the shared interface. */
void sh_apply_engine_get_serialize_selection(sh_serialize_selection_fn *serialize_selection);
void sh_apply_engine_get_prefab_export(sh_prefab_export_fn *prefab_export);

enum { SH_APPLY_FLAT_MAP = 1, SH_APPLY_FLAT_PREFAB = 2, SH_APPLY_FLAT_ENTITY = 3 };
/* Serialize an existing native flat object, without cloning/populating it or
 * invoking the map-save hooks. Main thread only; nested calls are refused.
 * Map/prefab bytes remain exact. An entity is wrapped as {"entities":[...]} so
 * the typed map selector can assign document-local sites. Success returns an
 * owned malloc buffer and its complete byte length; failure sets NULL/zero.
 * No caller-owned object is constructed, destroyed or changed. */
int sh_apply_engine_serialize_flat(const void *flat, unsigned kind, char **out, size_t *length);

/* Live entity callbacks shaped for nav_regions, without a reverse link dependency. */
int sh_apply_engine_entity_count(void *ctx);
int sh_apply_engine_entity_valid(int id, void *ctx);
int sh_apply_engine_entity_json(int id, char *out, int cap, void *ctx);
/* The editor's live transform for an entity. A serialized spawnPosition is the
 * value the box was created with, so only this sees one the user has moved. */
int sh_apply_engine_entity_transform(int id, float origin[3], float m[3][3],
                                     float size[3], int *instance, void *ctx);

/* Detour CommitEdit, the one call every undoable editor action ends in, so
 * navigation geometry is read when the map changes and at no other time. */
void sh_apply_engine_install_edit_hook(const sig_result *results, size_t n);

/* Ask for the navigation view to be brought up to date. The read runs on the
 * game's own thread on the next frame, never on the caller's. */
void sh_apply_engine_nav_request_update(void);

/* One shared update lifecycle for View and the native editor menu. Completion
 * means the worker result was collected, including when preview is disabled. */
enum { SH_NAV_UPDATE_IDLE, SH_NAV_UPDATE_REQUESTED, SH_NAV_UPDATE_BUILDING,
       SH_NAV_UPDATE_READY, SH_NAV_UPDATE_FAILED };
int sh_apply_engine_nav_update_status(void);

/* An entity's decl text was replaced in place, which reaches neither CommitEdit
 * nor GridApplyEntityTree. Main thread only. */
void sh_apply_engine_nav_decl_rewritten(int id);

/* Bit 0: the view no longer matches the map. Bit 1: an update is owed.
 * Bit 2: the map refused every read attempt. */
int sh_apply_engine_nav_view_state(void);

/* How much the editor has cost navigation since this DLL loaded. */
void sh_apply_engine_nav_read_stats(unsigned *reads, unsigned *noticed, double *read_ms,
                                    unsigned *by_edit, unsigned *by_undo,
                                    unsigned *by_start, int *hooked);
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
