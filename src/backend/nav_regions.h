/* nav_regions.h -- the author's navigation regions, read out of the map.
 *
 * WHAT AN AUTHOR ACTUALLY DOES
 * ----------------------------
 * They build their arena out of Blocking Boxes, as they always have, and tick
 * "AI Navigation" on the ones demons are meant to walk on. That tick sets
 * `affectsNavmesh` on the volume.
 *
 * That field is not ours. `snapmaps/volume/blocking` has carried a bool
 * `affectsNavmesh` since release, and the shipped editor tile already declares
 * `affectsNavmeshPath = "affectsNavmesh"` beside `showOnSpawnPath` and
 * `networkStaticPath`. id wired the whole path and then never exposed the
 * property sheet row, and nothing in the shipped binary consumes the value: the
 * one function that references the string (RVA 0x545120) is the editor's
 * property-write dispatcher, which copies it into the entity's spawn args, and
 * no AAS or obstacle code reads it back.
 *
 * That is what makes this marker VANILLA-SAFE BY CONSTRUCTION. Snapmap+ only
 * adds the missing property-sheet row; the field, its typeinfo path and its
 * serialization already exist in every player's game. A vanilla client loads a
 * map full of ticked volumes, plumbs the bool to spawn args exactly as it always
 * did, and nothing reads it. No new entityDef, no palette entry, no unknown
 * inherit -- the three things that could make a stock client refuse a map.
 *
 * WHAT THIS MODULE DOES
 * ---------------------
 * Reads the map JSON once and answers: which module instances are placed, and
 * which ticked volumes belong to each. Attribution is by `instanceEntities` --
 * the engine's OWN record of which entities belong to which instance -- and
 * never by containment, because an entity's coordinates are module-local and
 * two instances of one module have identical local coordinates.
 *
 * A region is reported in MODULE-LOCAL space, which is the space the module's
 * AAS is baked in, so the caller can hand it straight to the augmenter without
 * knowing where the instance sits in the world.
 *
 * NO GAME BYTES: tests synthesize map JSON, they do not embed a real map.
 */
#ifndef SNAPMAP_PLUS_NAV_REGIONS_H
#define SNAPMAP_PLUS_NAV_REGIONS_H

#include <stddef.h>

#define SH_NAVR_MAX_INSTANCES   256
#define SH_NAVR_MAX_REGIONS     512
#define SH_NAVR_MODULE_CAP      128     /* "category/module" + NUL */

/* One walkable surface an author asked for: the TOP FACE of a ticked Blocking
 * Box, in module-local coordinates.
 *
 * A blocking volume's `spawnPosition` is the box's BOTTOM in z and its CENTRE in
 * x and y, so the walkable surface is the rectangle at spawnPosition.z + size.z.
 * That asymmetry is the engine's, not ours; getting it wrong puts the navigation
 * at the floor of the box instead of its roof. */
typedef struct sh_nav_region {
    float x0, y0, x1, y1;   /* module-local rect, normalised so x0<x1, y0<y1 */
    float top_z;            /* module-local z of the walkable surface */
    int   instance;         /* index into the instance table below */
    int   block_demons;     /* the volume's blockDemons; 0 means a demon falls through it */
    unsigned entity;        /* index in the map's entities array, for diagnostics */
} sh_nav_region;

/* A placed module. `origin`/`orientation` are what BuildAAS applies to that
 * instance's navigation, and together with `module` they are the only identity
 * an instance has -- idSnapInstance carries no id field. */
typedef struct sh_nav_instance {
    char  module[SH_NAVR_MODULE_CAP];   /* "category/module", no path, no .decl */
    float origin[3];
    int   orientation;
    int   region_count;
} sh_nav_instance;

typedef struct sh_nav_map {
    sh_nav_instance instances[SH_NAVR_MAX_INSTANCES];
    int             instance_count;
    sh_nav_region   regions[SH_NAVR_MAX_REGIONS];
    int             region_count;
    int             truncated;          /* a cap was hit; the caller should say so */
} sh_nav_map;

/* Read `json` into `out`. Returns 1 if the map parsed (even with zero regions),
 * 0 if it is not a map document at all. Never raises; the input is a stranger's
 * map. `out` is fully overwritten, including on failure. */
int sh_nav_regions_read(const char *json, size_t len, sh_nav_map *out);

/* The Nth instance OF A GIVEN MODULE, in map order -- the mapping BuildAAS
 * itself uses. It opens each instance's navigation resource inside its
 * per-instance loop (RVA 0x4EBFB0), in instance-array order, so the Nth open of
 * one resource name is the Nth instance of that module. Returns the index into
 * `m->instances`, or -1. */
int sh_nav_regions_nth_instance(const sh_nav_map *m, const char *module, int n);

#endif /* SNAPMAP_PLUS_NAV_REGIONS_H */
