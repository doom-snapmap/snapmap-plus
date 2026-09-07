/* nav_bake.h -- bake the author's marked regions into navigation, at map load.
 *
 * WHERE THIS SITS
 * ---------------
 * navmesh.c serves navigation a map CARRIES, as smnav1 shards baked by an
 * offline tool. This module serves navigation a map DESCRIBES: the author ticks
 * "AI Navigation" on their Blocking Boxes (nav_regions.c reads that), and the
 * bytes are produced here, on the spot, when the engine asks for the module's
 * navmesh.
 *
 * WHY BAKE AT LOAD RATHER THAN AT SAVE
 * ------------------------------------
 * Baking at save would mean the finished payload rides in the map, which sounds
 * tidier but is worse in every direction that matters:
 *
 *   - the map would carry ~10 KB per platform per demon size against an 8 MiB
 *     budget; describing the regions instead costs a boolean per volume;
 *   - a saved bake goes stale the moment the author moves a box, so it needs a
 *     geometry digest and a staleness rule, and an author who ignores the
 *     warning ships navigation for geometry that is not there;
 *   - the shipped payload we must add to is right here at open time -- the
 *     engine is literally asking for it -- whereas at save time we would have to
 *     go find it;
 *   - and the same client that can USE custom navigation is the one baking it,
 *     because no vanilla client can use it at all. There is no audience that
 *     benefits from the bytes being pre-made.
 *
 * The cost is a bake per module per demon class per map load. That is three
 * augment passes over a payload of a few tens of KB for the usual one-module
 * custom map, which is nothing against a map load that already takes seconds.
 *
 * ONE INSTANCE PER MODULE MAY CARRY REGIONS
 * -----------------------------------------
 * A module's navigation is keyed by RESOURCE NAME, and every instance of that
 * module is built from the same resource. idDeclSnapMap::BuildAAS (RVA
 * 0x4EBFB0) opens it inside its per-instance loop and frees what it loaded on
 * every iteration, which says each instance loads afresh and could therefore be
 * served its own bytes -- but that is read from the disassembly, not measured,
 * and being wrong about it means serving one grid room's platforms to another,
 * where demons would walk on thin air.
 *
 * So until it is measured: regions may live in at most one instance of a given
 * module. A map may place a module twelve times; only one of those may carry
 * marked volumes. That covers the way custom maps are actually built -- the
 * author builds their arena in one room -- and it is correct whichever way
 * BuildAAS turns out to behave. A map that breaks the rule is refused for that
 * module, loudly, and plays on its shipped navigation.
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
#endif

#endif /* SNAPMAP_PLUS_NAV_BAKE_H */
