/* Reads Blocking Box navigation geometry and instance ownership from map JSON. Ownership comes from
 * instanceEntities, never spatial containment, because coordinates are module-local. */
#ifndef SNAPMAP_PLUS_NAV_REGIONS_H
#define SNAPMAP_PLUS_NAV_REGIONS_H

#include <stddef.h>

/* Ceilings, not array sizes: the tables grow with the map. 65536 boxes is the editor's uniqueId space; a box or
 * room past a ceiling is left out and counted, never refusing the map. */
#define SH_NAVR_MAX_INSTANCES   8192
#define SH_NAVR_MAX_REGIONS     65536
#define SH_NAVR_MODULE_CAP      128     /* longest "category/module" name */

/* One box face in module-local coordinates: an oriented quad with a z per corner, because a Blocking Box has a
 * full spawnOrientation. `face` is the OBB face index (`face >> 1` the axis, `face & 1` the negative side), so
 * an upright box's top is 4; walkability is judged later against each nav class's minFloorCos. */
typedef struct sh_nav_region {
    float c[4][3];          /* the face, module-local, CW seen from +Z */
    float n[3];             /* unit outward normal of that face */
    int   face;             /* OBB face index 0..5; 4 is an upright box's top */
    float depth;            /* the box's extent along -n */
    int   instance;         /* index into the instance table below */
    int   block_demons;     /* the volume's blockDemons; 0 means a demon falls through it */
    unsigned entity;        /* index in the map's entities array, for diagnostics */
    int marked;             /* support, as opposed to an unmarked obstacle */
} sh_nav_region;

/* Placed module identity and transform used by BuildAAS. */
typedef struct sh_nav_instance {
    char  module[SH_NAVR_MODULE_CAP];   /* "category/module", no path, no .decl */
    float origin[3];
    int   orientation;
    int   region_count;
    int   unusable;     /* origin or orientation unreadable: its boxes are left out */
} sh_nav_instance;

/* Why one box is left out of navigation while the rest of the map stays. Only a malformed room table
 * (instanceEntities) refuses a whole map. */
enum {
    SH_NAVR_DROP_SHAPE = 0,     /* unusable size, orientation or origin */
    SH_NAVR_DROP_ROOM,          /* its room's origin or orientation is unreadable */
    SH_NAVR_DROP_CONFLICT,      /* claimed by two rooms, or by a room and the orphan bucket */
    SH_NAVR_DROP_UNLISTED,      /* the room table does not list it */
    SH_NAVR_DROP_OUTSIDE,       /* marked, but outside every room */
    SH_NAVR_DROP_UNNAMED,       /* marked, in a room with an unreadable module name */
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
    int             truncated;          /* something past a ceiling is left out */
    /* The room table itself is malformed, so no box can be given an owner.
     * This, and only this, refuses the whole map. */
    int             invalid_geometry;
    /* Boxes the editor could not measure yet, which a later read gets. Nonzero
     * means this read is incomplete, not that the map is bad. */
    int             unmeasured;
    /* Two volumes share an id, or one carries none: the shapes are good but the per-entity refresh, which
     * addresses a box by id, is not. */
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
/* Same rooms and boxes regardless of read order and entity indices (corners and depth within 0.05, normals
 * 1e-4); flags and the left-out list are not compared. On a match b's tables are reordered into a's order;
 * returns 0, leaving b untouched, on a difference or allocation failure. */
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

/* Parse into out, overwriting it even on failure; out's previous tables are not freed. Returns 1 for a map
 * document, including one with no regions, or 0 for invalid input. */
int sh_nav_regions_read(const char *json, size_t len, sh_nav_map *out);

/* 1 when the map text marks a Blocking Box for AI navigation (flags.noFlood): the map carries navigation of its own,
 * which only Snapmap+ builds. 0 when none does, -1 when the text is not a map document. */
int sh_nav_regions_marks(const char *json, size_t len);

/* The collision size an omitted clipModelInfo.size member inherits from the
 * box's entityDef, for map-text reads that have no engine snapshot. 0 leaves
 * the member unknown, which drops that box. */
typedef int (*sh_navr_default_size)(const char *inherit, float size[3], void *ctx);
int sh_nav_regions_read_defaults(const char *json, size_t len, sh_nav_map *out,
                                 sh_navr_default_size defaults, void *ctx);

/* Effective collision size of the entity at this snapshot-array index; the callback and JSON must come from
 * the same snapshot. Returns >0 with the size, 0 when not measurable yet (incomplete read), or <0 when it is
 * not a usable box (that box alone is left out). */
typedef int (*sh_navr_entity_size)(unsigned entity, float size[3], void *ctx);
int sh_nav_regions_read_resolved(const char *json, size_t len, sh_nav_map *out,
                                sh_navr_entity_size read_size, void *ctx);
/* Read edit.clipModelInfo.size from a bounded, inheritance-resolved native
 * declaration. Requires all components and a box type; never supplies sizes. */
int sh_nav_regions_decl_size(const char *text, size_t len, float size[3]);

#ifdef SH_NAVR_TESTING
/* Lower the ceilings so a test can reach them; 0 restores the defaults. */
void sh_nav_regions_test_limits(int regions, int instances);
#endif

/* The per-box surface: a complete-map snapshot (sh_nav_bake_set_snapshot)
 * reads ownership and geometry; these callbacks let one box be re-read. */

/* Serialize live entity id with engine reflection. Returns bytes written, or
 * <= 0 on failure.
 */
typedef int (*sh_navr_entity_json)(int id, char *out, int cap, void *ctx);

/* Is `id` a live entity? */
typedef int (*sh_navr_entity_valid)(int id, void *ctx);

/* The editor's current transform for `id`: origin at the box bottom, and the 3x3 whose rows are the box's
 * local axes, in module space. Serialized spawnPosition is the creation value, so only this sees a moved or
 * turned box; returns 0, outputs untouched, when unreadable. */
typedef int (*sh_navr_entity_transform)(int id, float origin[3], float m[3][3],
                                        float size[3], int *instance, void *ctx);

/* Encode a box as one representative face and its depth. Rejects a non-finite
 * or non-positive size, and a matrix that is not a rotation. */
int sh_nav_regions_box_face(const float origin[3], const float m[3][3],
                            const float size[3], sh_nav_region *r);


/* Refresh all cached Blocking Boxes, unmarked obstacles included, by live uniqueId, preserving map-array indices
 * and instance ownership. Geometry comes from the editor's memory and each box's flags are re-read. Returns 0
 * without changing m when a complete snapshot is required. */
int sh_nav_regions_refresh_known(sh_nav_map *m, sh_navr_entity_valid valid,
                                 sh_navr_entity_json get_json,
                                 sh_navr_entity_transform get_transform, void *ctx,
                                 int *read_count, const char **why);

/* Does the volume `uid` carry a navigation flag? Asked about one the cached
 * inventory has never seen, where the answer decides whether a complete
 * snapshot is owed. Costs one entity read. */
int sh_nav_regions_volume_flagged(int uid, sh_navr_entity_json get_json, void *ctx);

/* Writes the uniqueIds of the volumes the last complete read recorded; returns how many. */
int sh_nav_regions_volume_uids(int *out, int cap);
int sh_nav_regions_knows_volume(int uid);

/* Moves the right to refresh onto `m`, which must already hold a copy of the last read. Only one map at a time
 * may be refreshed. */
void sh_nav_regions_adopt(sh_nav_map *m);

/* Return the Nth occurrence of a module in the map's instance array, or -1.
 * This is a table lookup, not an inference from resource-open order. */
int sh_nav_regions_nth_instance(const sh_nav_map *m, const char *module, int n);

#endif /* SNAPMAP_PLUS_NAV_REGIONS_H */
