/* nav_bake.h -- current editor geometry, preview and per-instance AAS baking.
 *
 * Complete game-thread snapshots capture boxes, transforms and ownership.
 * Geometry changes invalidate the cached preview. Before the engine converts
 * the editor map for Play, a final snapshot freezes the build revision.
 *
 * Each marked module instance receives a private temporary resource name.
 * BuildAAS loads, transforms and merges that instance's payload, then frees
 * the temporary resource. Stock name-based cached resources stay shared.
 *
 * navmesh.c separately serves the older smnav1 embedded-payload format.
 */
#ifndef SNAPMAP_PLUS_NAV_BAKE_H
#define SNAPMAP_PLUS_NAV_BAKE_H

#include <stddef.h>

/* Live entity callback types. */
#include "nav_regions.h"

/* Replace the map's regions, including when the input is empty. Called for
 * every deserialization so state cannot leak between maps.
 */
void sh_nav_bake_set_map(const char *json, size_t len);

/* Read the original resource into a process-heap buffer the caller frees, or
 * return NULL. The provider hook supplies this callback because it owns the
 * provider instance.
 */
typedef unsigned char *(*sh_nav_bake_reader)(const char *name, size_t *out_len);

/* If `name` is the navigation resource of a module this map marked up, bake and
 * return it. Returns 1 with a HeapAlloc(GetProcessHeap()) buffer the caller
 * frees, else 0. Never raises. */
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped,
                     unsigned char **out_bytes, size_t *out_len);

/* Console report: what this map asked for and what it got. */
void sh_nav_bake_report(void (*out)(const char *fmt, ...));

/* Legacy per-entity callbacks avoid a direct dependency on engine code.
 * Production editor baking uses the complete-map snapshot callback below.
 */
typedef int (*sh_nav_bake_entity_count)(void *ctx);

/* Refresh geometry and ownership on DOOM's main thread before map conversion.
 * Play does not serialize the editor map, so the loaded JSON alone misses
 * edits. Never call from the UI worker or inside the AAS loader.
 */
void sh_nav_bake_refresh_live(void);

/* Re-read all boxes the last complete refresh found, including obstacles, a small
 * part of the map and correspondingly cheaper. Returns 1 when it committed an
 * answer, or 0 when it could not -- a new or deleted volume, a moved module, a
 * map it has not read yet -- which the caller answers with a complete refresh.
 *
 * Main thread only, same as the complete refresh. Reports how many volumes it
 * read through `volumes` when that is not NULL, so a caller can weigh this
 * against the complete refresh from its own timings. */
int sh_nav_bake_refresh_volumes(int *volumes);

/* Why the last volumes-only refresh could not answer, for the console. */
const char *sh_nav_bake_volumes_reason(void);

/* Counter that advances whenever a refresh finds the editor geometry different
 * from what the bake holds. A caller compares it across a refresh to learn
 * whether that refresh changed anything. */
unsigned long sh_nav_bake_geometry_revision(void);

/* Return a complete attributed map with effective native collision sizes.
 * The callback owns engine access; the bake keeps only copied geometry. */
typedef int (*sh_nav_bake_snapshot)(sh_nav_map *out, void *ctx);
void sh_nav_bake_set_snapshot(sh_nav_bake_snapshot snapshot, void *ctx);
void sh_nav_bake_build_begin(void);
void sh_nav_bake_build_end(void);

/* Refresh the editor preview from a validated bake for monster48. Lines are
 * world-space; the caller draws them only while the editor is active. */
typedef void (*sh_nav_preview_line)(const float start[3], const float end[3], void *ctx);
/* Emit the green lines for the geometry as it stands. Returns 0 when the bake
 * behind them is still on the worker, in which case nothing was emitted and
 * the caller must keep showing what it has: publishing an empty set instead
 * blinks every line off until the worker lands. */
/* Colour for the lines that follow. Supplied by the caller, like the line sink
 * itself, so the bake stays independent of how anything is drawn. A caller
 * that passes NULL gets the walkable surfaces only: a refused volume drawn in
 * the same colour as a working one would say the opposite of the truth. */
typedef void (*sh_nav_preview_colour_fn)(float r, float g, float b, void *ctx);

int sh_nav_bake_preview(sh_nav_bake_reader read_shipped, sh_nav_preview_line line,
                        sh_nav_preview_colour_fn colour, void *ctx);

/* Request worker shutdown outside DllMain; test/process teardown only. */
void sh_nav_bake_preview_stop(void);

/* 1 when the worker has finished a bake nobody has collected. Cheap enough to
 * ask every frame, and worth asking: the worker finishes on its own clock, so
 * waiting for the next read to collect it leaves the green stale for as long
 * as that read is away. */
int sh_nav_bake_preview_pending(void);

/* Mark the first `count` marked volumes of every module as unplaced, the way
 * a refused bake does, so the marks can be seen without a map that refuses.
 * Zero clears them. Returns how many are marked. */
int sh_nav_bake_show_marks(int count);
void sh_nav_bake_enable_instances(int enabled);
int sh_nav_bake_instance_name(int instance, const char *name, char *out, size_t capacity);

void sh_nav_bake_set_live_editor(sh_nav_bake_entity_count count,
                                 sh_navr_entity_valid valid,
                                 sh_navr_entity_json get_json,
                                 void *ctx);

#ifdef SH_NAV_BAKE_TESTING
/* Test the module resource-name grammar. Returns 1 and fills the outputs for
 * a recognized name.
 */
int sh_nav_bake_test_parse_name(const char *name, char *module, size_t module_cap,
                                char *cls, size_t cls_cap);
void sh_nav_bake_test_reset(void);
int  sh_nav_bake_test_bake_count(void);
void sh_nav_bake_test_copy_map(sh_nav_map *out);
#endif

#endif /* SNAPMAP_PLUS_NAV_BAKE_H */
