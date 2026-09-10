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
 * See docs/navigation.md for the author-facing contract and limits.
 */
#ifndef SNAPMAP_PLUS_NAV_BAKE_H
#define SNAPMAP_PLUS_NAV_BAKE_H

#include <stddef.h>

/* for the entity callback types the live-editor seam below is shaped to */
#include "nav_regions.h"

/* Read this map's regions. Called from the deserialize funnel on EVERY map, so
 * the previous map's regions can never survive into the next one -- the same
 * clear-from-empty rule navmesh.c's serving table follows, and for the same
 * reason. */
void sh_nav_bake_set_map(const char *json, size_t len);

/* Reads the bytes the engine would have served for `name`, into a fresh
 * HeapAlloc(GetProcessHeap()) buffer the caller frees, or NULL.
 *
 * Passed in rather than called directly because only the resource-provider hook
 * can do it -- the provider object arrives as that hook's `self` and is not
 * published anywhere else. Keeping it a parameter also stops this module
 * depending on overrides.c, which would otherwise drag the whole shadow into
 * every test that links either one. */
typedef unsigned char *(*sh_nav_bake_reader)(const char *name, size_t *out_len);

/* If `name` is the navigation resource of a module this map marked up, bake and
 * return it. Returns 1 with a HeapAlloc(GetProcessHeap()) buffer the caller
 * frees, else 0. Never raises. */
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped,
                     unsigned char **out_bytes, size_t *out_len);

/* Console report: what this map asked for and what it got. */
void sh_nav_bake_report(void (*out)(const char *fmt, ...));

/* ---- the live editor ---------------------------------------------------
 *
 * Reading the map as loaded is not enough for the author's actual flow: they
 * tick a box and press Play, and Play does not serialize the map, so the marker
 * never reaches the table. Given a way to read the LIVE entities, the bake
 * re-reads the markers from them first.
 *
 * Registered rather than called directly so this module keeps no link
 * dependency on the engine surface -- which is also what lets its tests run
 * without one. Unregistered, the bake simply uses the map as loaded, which is
 * correct for a downloaded map and was the whole behaviour before. */
typedef int (*sh_nav_bake_entity_count)(void *ctx);

/* Re-read the markers from the live editor and re-plan, so a volume ticked THIS
 * SESSION is baked without the author saving and reloading the map.
 *
 * This is a separate entry point on purpose. Pressing Play does not serialize the
 * map -- SnapMapEditToSnapBuild (0x4F27B0) reaches neither DeserializeFromJson nor
 * SerializeToJson, verified against the binary -- so the map JSON the deserialize
 * funnel handed sh_nav_bake_set_map is the map as it was LOADED, and the only place
 * a session's tick exists is the live editor.
 *
 * It must run on DOOM's main thread while the editor still owns its map. The two
 * places it must NOT run are the frontend's UI worker thread (issue #61) and inside
 * the engine's AAS load (issues #87 and #89) -- by the latter the edit map is
 * already being turned into the build map and its entities have no defsub yet.
 */
void sh_nav_bake_refresh_live(void);

typedef int (*sh_nav_bake_snapshot)(char **json, size_t *len, void *ctx);
void sh_nav_bake_set_snapshot(sh_nav_bake_snapshot snapshot, void *ctx);
void sh_nav_bake_build_begin(void);
void sh_nav_bake_build_end(void);

/* Refresh the editor preview from a validated bake for monster48. Lines are
 * world-space; the caller draws them only while the editor is active. */
typedef void (*sh_nav_preview_line)(const float start[3], const float end[3], void *ctx);
void sh_nav_bake_preview(sh_nav_bake_reader read_shipped, sh_nav_preview_line line, void *ctx);
void sh_nav_bake_enable_instances(int enabled);
int sh_nav_bake_instance_name(int instance, const char *name, char *out, size_t capacity);

void sh_nav_bake_set_live_editor(sh_nav_bake_entity_count count,
                                 sh_navr_entity_valid valid,
                                 sh_navr_entity_json get_json,
                                 void *ctx);

#ifdef SH_NAV_BAKE_TESTING
/* The name grammar, exposed so a test can prove it matches what the engine
 * asks for rather than what we hope it asks for. Returns 1 and fills the
 * buffers when `name` is a module navigation resource. */
int sh_nav_bake_test_parse_name(const char *name, char *module, size_t module_cap,
                                char *cls, size_t cls_cap);
void sh_nav_bake_test_reset(void);
int  sh_nav_bake_test_bake_count(void);
void sh_nav_bake_test_copy_map(sh_nav_map *out);
#endif

#endif /* SNAPMAP_PLUS_NAV_BAKE_H */
