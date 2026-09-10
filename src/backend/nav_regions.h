/* nav_regions.h -- the author's navigation regions, read out of the map.
 *
 * WHAT AN AUTHOR ACTUALLY DOES
 * ----------------------------
 * They build their arena out of Blocking Boxes, as they always have, and tick
 * "AI Navigation" on the ones demons are meant to walk on. That tick sets
 * `flags.noFlood` on the volume, an existing reflected idEntity boolean.
 * The runtime access audit found no gameplay consumer of its bit in either
 * supported executable. The Blocking Box decl and base constructor default it
 * to false. This uses vanilla typeinfo and serialization, not a new field.
 *
 * The former marker, `affectsNavmesh`, is NOT inert: the native blocking-volume
 * setup clears CONTENTS_OBSTACLE when it is true. Old marked boxes migrate at
 * map load to flags.noFlood, with affectsNavmesh cleared before native parsing.
 * An explicit new marker wins, including false. The bake reader itself never
 * falls back to affectsNavmesh. See docs/navigation-markers.md for scope and
 * compatibility limits.
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

/* One walkable surface an author asked for, in module-local coordinates.
 *
 * NOT A RECTANGLE. A Blocking Box carries a full `idMat3 spawnOrientation`, and
 * 12.4% of the volumes in a real map are not upright, so the walkable surface is
 * an ORIENTED QUAD -- four corners each with their own z. Which face of the box
 * that is depends on the rotation: the top for an upright box, a SIDE face for a
 * box on its side (58 of the 82 non-upright volumes in one real map), the
 * underside for one rotated past vertical.
 *
 * A blocking volume's `spawnPosition` is the box's BOTTOM in z and its CENTRE in
 * x and y. That asymmetry is the engine's, not ours, and it is why a rotated box
 * hangs somewhere other than where an upright one would.
 *
 * `c` is wound CLOCKWISE seen from +Z, the winding every Grid Room floor area
 * uses. `n` is that face's unit outward normal. `face` is the OBB face index:
 * `face >> 1` is the axis and `face & 1` the negative side, so an UPRIGHT box's
 * top face is 4, not 0.
 *
 * Whether the face is WALKABLE is not decided here. The threshold is
 * `minFloorCos`, a per-nav-class AAS setting, and one region feeds all three
 * monster classes -- so this module reports the geometry and the augmenter
 * judges it. */
typedef struct sh_nav_region {
    float c[4][3];          /* the face, module-local, CW seen from +Z */
    float n[3];             /* unit outward normal of that face */
    int   face;             /* OBB face index 0..5; 4 is an upright box's top */
    float depth;            /* the box's extent along -n; the face plus this is the whole solid */
    int   instance;         /* index into the instance table below */
    int   block_demons;     /* the volume's blockDemons; 0 means a demon falls through it */
    unsigned entity;        /* index in the map's entities array, for diagnostics */
    int marked;            /* distinguishes support from an unmarked obstacle */
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
    int             invalid_geometry;   /* invalid solid, transform or ownership */
    sh_nav_region   obstacles[SH_NAVR_MAX_REGIONS];
    int             obstacle_count;
} sh_nav_map;

/* Read `json` into `out`. Returns 1 if the map parsed (even with zero regions),
 * 0 if it is not a map document at all. Never raises; the input is a stranger's
 * map. `out` is fully overwritten, including on failure. */
int sh_nav_regions_read(const char *json, size_t len, sh_nav_map *out);

/* Convert legacy Blocking Box markers before native map parsing. Returns a
 * NUL-terminated HeapAlloc buffer (caller HeapFrees), or NULL for no change or
 * a refusal. Other entities and unrelated bytes are preserved. */
char *sh_nav_regions_migrate(const char *json, size_t len, size_t *out_len);

/* Legacy per-entity refresh for callers without a complete-map snapshot.
 * Production editor baking uses sh_nav_bake_set_snapshot instead: it refreshes
 * ownership and the instance table together with geometry, including new IDs.
 */

/* Serialize live entity `id` to JSON. Returns the length written, or <= 0.
 * The engine's own reflection does this; see apply_engine.c's serialize_entity
 * slot, which is what the entity-state editor already uses in production. */
typedef int (*sh_navr_entity_json)(int id, char *out, int cap, void *ctx);

/* Is `id` a live entity? */
typedef int (*sh_navr_entity_valid)(int id, void *ctx);

/* Re-read every Blocking Box's marker from the live entities, replacing `m`'s
 * region set while keeping its instance table and attribution. Returns the
 * number of marked volumes found, or -1 if the live surface could not be read
 * (in which case `m` is left exactly as it was, so a failure falls back to what
 * the map was loaded with rather than to nothing).
 *
 * `highest_id` bounds the scan; ids are probed through `valid`. */
int sh_nav_regions_refresh_live(sh_nav_map *m, int highest_id,
                                sh_navr_entity_valid valid,
                                sh_navr_entity_json get_json, void *ctx);

/* Return the Nth occurrence of a module in the map's instance array, or -1.
 * This is a table lookup, not an inference from resource-open order. */
int sh_nav_regions_nth_instance(const sh_nav_map *m, const char *module, int n);

#endif /* SNAPMAP_PLUS_NAV_REGIONS_H */
