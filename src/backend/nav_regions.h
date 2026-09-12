/* Read Blocking Box navigation geometry and instance ownership from map JSON.
 *
 * AI Navigation uses flags.noFlood. Legacy affectsNavmesh markers migrate
 * before native parsing; an explicit new marker, including false, wins.
 * Runtime obstacle policy is applied separately. See docs/navigation-
 * markers.md.
 *
 * Ownership comes from instanceEntities, never spatial containment:
 * coordinates are module-local and repeated instances can share the same
 * local geometry.
 */
#ifndef SNAPMAP_PLUS_NAV_REGIONS_H
#define SNAPMAP_PLUS_NAV_REGIONS_H

#include <stddef.h>

#define SH_NAVR_MAX_INSTANCES   256
#define SH_NAVR_MAX_REGIONS     512
#define SH_NAVR_MODULE_CAP      128     /* A box encoded as one oriented face and
                                         * its depth in module-local space.
                                         * spawnPosition is centred in x/y and at
                                         * the bottom in local z. Corners wind
                                         * clockwise from +Z; face>>1 selects the
                                         * OBB axis and face&1 its negative side.
                                         *
                                         * nav_geometry reconstructs the solid and
                                         * considers every face. Walkability
                                         * depends on the nav class's minFloorCos
                                         * and clearance settings.
                                         */

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

/* Placed module identity and transform used by BuildAAS. idSnapInstance has
 * no separate id field.
 */
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
    /* Two volumes share an id, or one carries none. The shapes are still
     * good; only the per-entity refresh, which addresses a box by id, is
     * not. Duplicating a box in the editor produces this until the map is
     * saved, and it must not cost the map its navigation. */
    int             ids_unusable;
    sh_nav_region   obstacles[SH_NAVR_MAX_REGIONS];
    int             obstacle_count;
} sh_nav_map;

/* Parse into out, overwriting it even on failure. Returns 1 for a map
 * document, including one with no regions, or 0 for invalid input.
 */
int sh_nav_regions_read(const char *json, size_t len, sh_nav_map *out);

/* Effective collision size of the entity at this exact snapshot-array index.
 * The callback and JSON must come from the same native snapshot; uniqueIds
 * cannot identify duplicated boxes reliably. No engine pointers are retained. */
typedef int (*sh_navr_entity_size)(unsigned entity, float size[3], void *ctx);
int sh_nav_regions_read_resolved(const char *json, size_t len, sh_nav_map *out,
                                sh_navr_entity_size read_size, void *ctx);
/* Read edit.clipModelInfo.size from a bounded, inheritance-resolved native
 * declaration. Requires all components and a box type; never supplies sizes. */
int sh_nav_regions_decl_size(const char *text, size_t len, float size[3]);

/* Convert legacy Blocking Box markers before native map parsing. Returns a
 * NUL-terminated HeapAlloc buffer (caller HeapFrees), or NULL for no change or
 * a refusal. Other entities and unrelated bytes are preserved. */
char *sh_nav_regions_migrate(const char *json, size_t len, size_t *out_len);

/* Legacy per-entity refresh for callers without a complete-map snapshot.
 * Production editor baking uses sh_nav_bake_set_snapshot instead: it refreshes
 * ownership and the instance table together with geometry, including new IDs.
 */

/* Serialize live entity id with engine reflection. Returns bytes written, or
 * <= 0 on failure.
 */
typedef int (*sh_navr_entity_json)(int id, char *out, int cap, void *ctx);

/* Is `id` a live entity? */
typedef int (*sh_navr_entity_valid)(int id, void *ctx);

/* Legacy refresh of markers and geometry using the existing instance
 * attribution. Returns the marked-volume count, or -1 without changing m if
 * the live surface cannot be read. highest_id bounds probes through valid.
 */
int sh_nav_regions_refresh_live(sh_nav_map *m, int highest_id,
                                sh_navr_entity_valid valid,
                                sh_navr_entity_json get_json, void *ctx);

/* Refresh all cached Blocking Boxes by their live uniqueIds, including
 * unmarked obstacles. Preserve map-array indices and instance ownership.
 * Returns 0 without changing m when a complete snapshot is required. */
int sh_nav_regions_refresh_known(sh_nav_map *m, sh_navr_entity_valid valid,
                                 sh_navr_entity_json get_json, void *ctx,
                                 int *read_count, const char **why);

/* Move the right to refresh onto `m`, which must already hold a copy of what
 * the last read produced. Only one map at a time may be refreshed, and a
 * caller that reads into scratch and copies the result has to say so or
 * neither refresh above will ever accept its map. */
void sh_nav_regions_adopt(sh_nav_map *m);

/* Return the Nth occurrence of a module in the map's instance array, or -1.
 * This is a table lookup, not an inference from resource-open order. */
int sh_nav_regions_nth_instance(const sh_nav_map *m, const char *module, int n);

#endif /* SNAPMAP_PLUS_NAV_REGIONS_H */
