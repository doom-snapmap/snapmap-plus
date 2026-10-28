/* Read Blocking Box navigation geometry and instance ownership from map JSON.
 *
 * AI Navigation uses flags.noFlood. The engine's own affectsNavmesh is a
 * separate property this module never reads or writes. Runtime obstacle policy
 * is applied separately. See docs/navigation-markers.md.
 *
 * Ownership comes from instanceEntities, never spatial containment:
 * coordinates are module-local and repeated instances can share the same
 * local geometry.
 */
#ifndef SNAPMAP_PLUS_NAV_REGIONS_H
#define SNAPMAP_PLUS_NAV_REGIONS_H

#include <stddef.h>

/* Ceilings, not array sizes: every table below grows with the map. They bound
 * worst-case memory (documented in docs/capabilities.md): 65536 flagged boxes
 * is the editor's own uniqueId space (84 bytes each, so at most 5.5 MB for the
 * floors and as much again for the walls), and 8192 rooms is far above any
 * SnapMap module budget (152 bytes each, 1.2 MB). A box or room past a ceiling
 * is left out and counted; it never refuses the map. */
#define SH_NAVR_MAX_INSTANCES   8192
#define SH_NAVR_MAX_REGIONS     65536
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
    int   unusable;     /* origin or orientation unreadable: its boxes are left out */
} sh_nav_instance;

/* Why one box was left out of the navigation while the rest of the map kept
 * it. Only the room table itself (instanceEntities) can refuse a whole map. */
enum {
    SH_NAVR_DROP_SHAPE = 0,     /* size, orientation or origin navigation cannot use */
    SH_NAVR_DROP_ROOM,          /* its room's origin or orientation is unreadable */
    SH_NAVR_DROP_CONFLICT,      /* its id is claimed by two rooms, or by a room and no room */
    SH_NAVR_DROP_UNLISTED,      /* the room table does not list it at all */
    SH_NAVR_DROP_OUTSIDE,       /* marked, but placed outside every room */
    SH_NAVR_DROP_UNNAMED,       /* marked, in a room whose module name is unreadable */
    SH_NAVR_DROP_CAPACITY,      /* past a ceiling above */
    SH_NAVR_DROP_UNMEASURED,    /* the editor could not measure it for this read */
    SH_NAVR_DROP_KINDS
};
#define SH_NAVR_DROPPED_KEEP 32

typedef struct sh_nav_dropped {
    unsigned entity;            /* index in the map's entities array */
    int      uid;               /* its uniqueId, or -1 */
    int      instance;          /* the room it claimed, or -1 */
    int      reason;            /* SH_NAVR_DROP_* */
    int      marked;            /* AI Navigation (else Block Demons only) */
} sh_nav_dropped;

/* Heap-backed: initialize with {0}, release with sh_nav_map_free, copy with
 * sh_nav_map_copy. A plain struct assignment shares the tables. */
typedef struct sh_nav_map {
    sh_nav_instance *instances;
    int             instance_count, instance_cap;
    sh_nav_region   *regions;
    int             region_count, region_cap;
    int             truncated;          /* something past a ceiling was left out */
    /* The room table itself is malformed, so no box can be given an owner.
     * This, and only this, refuses the whole map. */
    int             invalid_geometry;
    /* Boxes the editor could not measure yet, which a later read gets. Nonzero
     * means this read is incomplete, not that the map is bad. */
    int             unmeasured;
    /* Two volumes share an id, or one carries none. The shapes are still
     * good; only the per-entity refresh, which addresses a box by id, is
     * not. Duplicating a box in the editor produces this until the map is
     * saved, and it must not cost the map its navigation. */
    int             ids_unusable;
    sh_nav_region   *obstacles;
    int             obstacle_count, obstacle_cap;
    /* Volumes whose clip model is not a box -- a cylinder, say. They carry no
     * navigation and are left out; they never invalidate the map. */
    int             unshaped;
    /* Boxes left out one at a time, by reason, and the first few of them. */
    int             dropped_total;
    int             dropped_by[SH_NAVR_DROP_KINDS];
    int             dropped_kept;
    sh_nav_dropped  dropped[SH_NAVR_DROPPED_KEEP];
} sh_nav_map;

void sh_nav_map_free(sh_nav_map *m);
/* Deep copy into dst, which must be empty (zeroed or freed). 0 on allocation
 * failure, leaving dst empty. */
int  sh_nav_map_copy(sh_nav_map *dst, const sh_nav_map *src);
/* Same tables and flags. */
int  sh_nav_map_equal(const sh_nav_map *a, const sh_nav_map *b);
/* Same rooms and the same boxes, in whatever order the two reads listed them
 * and whatever entity indices they gave them: what a bake depends on. Corners,
 * normals and depth agree within 0.05 units (1e-4 for normals), the float
 * round trip of a converted read. Read flags and the left-out list are not
 * compared. When they match, b's floor
 * and wall tables are reordered into a's order, so the bake of b is the bake
 * of a. Two reads of one map disagree on entity order (the editor's snapshot
 * and the map Play converts), and comparing records whole made every Play
 * re-bake. Returns 0, leaving b untouched, when they differ or on allocation
 * failure. */
int  sh_nav_map_same_shapes(const sh_nav_map *a, sh_nav_map *b);
/* The first box only `a` has and the first only `b` has, comparing shapes as
 * sh_nav_map_same_shapes does; -1 where none. walls selects the obstacle
 * tables. Returns 0 on allocation failure. */
int  sh_nav_map_shape_difference(const sh_nav_map *a, const sh_nav_map *b, int walls,
                                 int *only_a, int *only_b);
/* Free dst, then hand it src's tables; src is left empty. */
void sh_nav_map_move(sh_nav_map *dst, sh_nav_map *src);
/* Heap bytes the tables hold. */
size_t sh_nav_map_bytes(const sh_nav_map *m);
/* Short text for SH_NAVR_DROP_*. */
const char *sh_nav_regions_drop_reason(int reason);

/* Parse into out, overwriting it even on failure. out's previous tables are
 * NOT freed (it may be uninitialized): free them first. Returns 1 for a map
 * document, including one with no regions, or 0 for invalid input.
 */
int sh_nav_regions_read(const char *json, size_t len, sh_nav_map *out);

/* The collision size an omitted clipModelInfo.size member inherits from the
 * box's entityDef, for map-text reads that have no engine snapshot. 0 leaves
 * the member unknown, which drops that box. */
typedef int (*sh_navr_default_size)(const char *inherit, float size[3], void *ctx);
int sh_nav_regions_read_defaults(const char *json, size_t len, sh_nav_map *out,
                                 sh_navr_default_size defaults, void *ctx);

/* Effective collision size of the entity at this exact snapshot-array index.
 * The callback and JSON must come from the same native snapshot; uniqueIds
 * cannot identify duplicated boxes reliably. No engine pointers are retained.
 * Returns >0 with the size, 0 when the entity cannot be measured yet (the read
 * is incomplete), or <0 when it was measured and is not a usable box (that box
 * alone is left out). */
typedef int (*sh_navr_entity_size)(unsigned entity, float size[3], void *ctx);
int sh_nav_regions_read_resolved(const char *json, size_t len, sh_nav_map *out,
                                sh_navr_entity_size read_size, void *ctx);
/* Read edit.clipModelInfo.size from a bounded, inheritance-resolved native
 * declaration. Requires all components and a box type; never supplies sizes. */
int sh_nav_regions_decl_size(const char *text, size_t len, float size[3]);
/* One level of an unresolved entityDef file: the clipModelInfo.size members it
 * spells (bit i of *have for axis i), its clip type (1 box, -1 other, 0 not
 * given) and its inherit name. The text may carry the file's outer braces.
 * Returns 0 for text that does not tokenize. */
int sh_nav_regions_decl_level(const char *text, size_t len, float size[3],
                              unsigned *have, int *type, char *inherit, size_t cap);

#ifdef SH_NAVR_TESTING
/* Lower the ceilings so a test can reach them; 0 restores the defaults. */
void sh_nav_regions_test_limits(int regions, int instances);
#endif

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

/* The editor's current transform for `id`: origin at the box bottom in local z,
 * and the 3x3 whose rows are the box's local axes, both in module space. A
 * serialized spawnPosition is the value the box was created with, so this is
 * the only reader that sees a box the user has moved or turned. Returns 0 when
 * the transform cannot be read, leaving the outputs untouched. */
typedef int (*sh_navr_entity_transform)(int id, float origin[3], float m[3][3],
                                        float size[3], int *instance, void *ctx);

/* Encode a box as one representative face and its depth. Rejects a non-finite
 * or non-positive size, and a matrix that is not a rotation. */
int sh_nav_regions_box_face(const float origin[3], const float m[3][3],
                            const float size[3], sh_nav_region *r);

/* Legacy refresh of markers and geometry using the existing instance
 * attribution. Returns the marked-volume count, or -1 without changing m if
 * the live surface cannot be read. highest_id bounds probes through valid.
 */
int sh_nav_regions_refresh_live(sh_nav_map *m, int highest_id,
                                sh_navr_entity_valid valid,
                                sh_navr_entity_json get_json, void *ctx);

/* Refresh all cached Blocking Boxes by their live uniqueIds, including
 * unmarked obstacles. Preserve map-array indices and instance ownership.
 * Returns 0 without changing m when a complete snapshot is required.
 *
 * Geometry comes from the editor's own memory, so a box that has been moved,
 * turned or resized costs nothing to follow; each box's flags are re-read. */
int sh_nav_regions_refresh_known(sh_nav_map *m, sh_navr_entity_valid valid,
                                 sh_navr_entity_json get_json,
                                 sh_navr_entity_transform get_transform, void *ctx,
                                 int *read_count, const char **why);

/* Does the volume `uid` carry a navigation flag? Asked about one the cached
 * inventory has never seen, where the answer decides whether a complete
 * snapshot is owed. Costs one entity read. */
int sh_nav_regions_volume_flagged(int uid, sh_navr_entity_json get_json, void *ctx);

/* The uniqueIds of the volumes the last complete read recorded. Returns how
 * many were written. A volume created since, including a placement preview, is
 * not among them. */
int sh_nav_regions_volume_uids(int *out, int cap);
int sh_nav_regions_knows_volume(int uid);

/* Move the right to refresh onto `m`, which must already hold a copy of what
 * the last read produced. Only one map at a time may be refreshed, and a
 * caller that reads into scratch and copies the result has to say so or
 * neither refresh above will ever accept its map. */
void sh_nav_regions_adopt(sh_nav_map *m);

/* Return the Nth occurrence of a module in the map's instance array, or -1.
 * This is a table lookup, not an inference from resource-open order. */
int sh_nav_regions_nth_instance(const sh_nav_map *m, const char *module, int n);

#endif /* SNAPMAP_PLUS_NAV_REGIONS_H */
