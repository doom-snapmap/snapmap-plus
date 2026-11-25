/* Adds walkable surfaces, BSP leaves and reachabilities to shipped AAS: reciprocal walk links for step-height
 * differences, per-demon traversal animations (nav_traversal) for larger ones, and walk-off links by
 * maxFallHeight. */
#ifndef SNAPMAP_PLUS_AAS_AUGMENT_H
#define SNAPMAP_PLUS_AAS_AUGMENT_H

#include <stddef.h>
#include "aas_edit.h"

/* Ceiling on the boxes one module copy offers and on the walkable pieces they cut into, per demon class,
 * bounding one bake's scratch at roughly 11 MB. A box past it is left out and reported; it never refuses
 * the module. */
#define SH_AUG_MAX_PLATFORMS   2048
#define SH_AUG_NAME_CAP        64
#define SH_AUG_REASON_CAP      160
#define SH_AUG_MAX_CORNERS     32
#define SH_AUG_MAX_BLAMED      8
#define SH_AUG_MAX_DROPPED     16

/* A convex surface in module-local coordinates, wound clockwise from +Z. Input boxes use four corners (corners
 * is zero); n is its outward normal and face its OBB face index (4 is an upright box's top). */
typedef struct sh_aug_platform {
    float c[SH_AUG_MAX_CORNERS][3];      /* convex boundary, CW seen from +Z */
    float n[3];
    int   face;                         /* OBB face index; 4 == upright top */
    float depth;                        /* the solid's extent along -n behind the face */
    char  name[SH_AUG_NAME_CAP];
    int   corners;
    int   obstacle_only;                /* collide, but never create support */
    float support[4][3];                /* original box face behind a prepared cell */
} sh_aug_platform;

/* Per-class outcome for one emitted or refused surface. */
typedef struct sh_aug_platform_result {
    char     name[SH_AUG_NAME_CAP];
    int      emitted;
    int      area;                      /* the new area index, or -1 */
    int      carrier;                   /* the area the platform sits over */
    int      leaf_slots_carved;
    int      links;                     /* reachabilities touching this area */
    int      climbs;                    /* baked traversals touching it */
    int      demons;                    /* distinct demons offered a climb */
    int      island;
    float    tilt_degrees;              /* the chosen face's angle from horizontal */
    int      side_face;                 /* 1 if that face is not the box's top */
    int      neighbours;                /* distinct areas this platform links to */
    int      leaps;
    float    centre[3];                 /* emitted piece centre, used to verify BSP lookup */
    int      source;
    int      pieces;                    /* walkable pieces from the source volume; 1 if unsplit */
    char     reason[SH_AUG_REASON_CAP]; /* why it is not emitted */
} sh_aug_platform_result;

/* Why a floor-side climb onto one face of a box is not placed, most frequent first in the report. */
enum {
    SH_AUG_FACE_BLOCKED = 0,    /* another solid overhangs or stands in the run-up */
    SH_AUG_FACE_NO_FLOOR,
    SH_AUG_FACE_NO_TOP,         /* the landing is not on the box's own top */
    SH_AUG_FACE_HEIGHT,         /* no animation of that demon fits the rise */
    SH_AUG_FACE_INSIDE,         /* the edge is a cut inside the box, not a face */
    SH_AUG_FACE_NO_EDGE,
    SH_AUG_FACE_REASONS
};

/* Floor climbs onto one side face of one requested box, per class: links
 * placed, which demons (bit k is traversal monster k) and the reasons the
 * other attempts failed. Faces are the box's four top edges in input order. */
typedef struct sh_aug_face {
    int      climbs;
    unsigned demons;
    int      attempts;
    int      walks;             /* step-high: walk links, no climb needed */
    int      reasons[SH_AUG_FACE_REASONS];
} sh_aug_face;

/* Routes left out of one area so it stays inside the engine's 256 outgoing
 * reachabilities: `routes` of them leave `area` toward or from `source`. */
typedef struct sh_aug_route_drop { int source, area, routes; } sh_aug_route_drop;

typedef struct sh_aug_report {
    /* One entry per walkable piece, on the heap (sh_aug_report_free releases it); source identifies its
     * requested volume and source_count counts them. */
    sh_aug_platform_result *platforms;
    int      platform_count;
    int      source_count;
    int      pieces_truncated;          /* a box's geometry could not be cut; candidate refused */
    int      pieces_dropped;            /* walkable faces past the ceiling: degraded, not refused */
    int      sources_dropped;           /* boxes that lost a face that way */
    int      dropped_sources[SH_AUG_MAX_BLAMED];
    int      dropped_source_count;
    int      routes_dropped;            /* distinct routes left out at the 256 limit */
    int      route_areas;               /* areas that needed it */
    sh_aug_route_drop dropped[SH_AUG_MAX_DROPPED];
    int      dropped_count;
    int      edges_skipped;             /* edges whose floor walk passed its budget */
    int      file_full;
    int      segments_failed;           /* an edge could not be measured; refused */
    int      dead_gaps;                 /* unlinked pairs beyond step range but below the shortest available jump */
    unsigned areas_before, areas_after;
    unsigned reach_before, reach_after;
    unsigned traversal_points_before;   /* the shipped traversalPoints */
    unsigned depth_before, depth_after;
    int      depth_exceeded;
    int      links_truncated;           /* traversal allocation failed; candidate refused */
    int      reach_limit_exceeded;      /* shipped routes over 256 from one area, or a missing area; refused */
    int      reach_limit_area;
    /* Requested volumes routing into the full area, which a mapper can move or delete to fit; empty when
     * the area is shipped floor whose own links already fill it. */
    int      blamed[SH_AUG_MAX_BLAMED];
    int      blamed_count;
    int      anchors_reduced;           /* alternative basic/traversal samples removed */
    /* Floor cuts: areas cut around boxes on or over them, pieces added, floors left whole because pieces did
     * not fit, and shipped routes removed for an endpoint in cut-out ground. */
    int      floors_cut;
    int      floor_pieces_added;
    int      floors_left_whole;
    int      shipped_routes_removed;
    int      floors_covered;            /* floor areas wholly under boxes, closed to routes */
    int      floor_walks_capped;        /* floor-piece walk samples left out to keep a floor well under 256 */
    /* faces[source * 4 + edge]: four per box, on the heap with platforms; NULL without a traversal table. */
    sh_aug_face *faces;
    int      climbs_declined;           /* omitted: the source area already owns traversal points */
    /* Climbs, drops and leaps refused because their path would pass through a
     * solid or start from a cut inside a box rather than a real edge. */
    int      through_solids;
    /* Leaps dropped because shorter leaps through a nearer landing of the
     * same area reach their target. */
    int      leaps_farther;
} sh_aug_report;

/* Augments a in place. Returns 1 if the model remains coherent, including when individual platforms are
 * refused (the report records why). A zero route budget uses the engine limit of 256. On failure discard the model and serve shipped bytes. */
int sh_aas_augment(sh_aas *a, const sh_aug_platform *plats, int n,
                   int route_budget, sh_aug_report *out);
void sh_aug_report_free(sh_aug_report *out);

/* The route budget that brings an augmented model of `size` bytes under `cap`,
 * judged from its generated routes per area; always below `current` (0 meaning 256). Returns 0 when no lower
 * budget can help. */
int sh_aug_route_budget_for(const sh_aas *a, const sh_aug_report *rep,
                            size_t size, size_t cap, int current);

/* Resolve a point through the BSP as the engine does; 0 means void. The
 * serving path uses this to verify that generated areas are findable.
 */
int sh_aas_point_area(const sh_aas *a, float x, float y, float z);

/* The BSP tree's depth, and the loader's hard limit on it. */
unsigned sh_aas_tree_depth(const sh_aas *a);

#ifdef SH_AUG_TESTING
/* Candidate positions along an edge, midpoint first. Multiple anchors
 * accommodate each demon animation's different floor-side offset.
 */
int sh_aug_test_trav_anchors(double lo, double hi, double *out, int cap);
/* Lower the per-class floor piece ceiling (1024); 0 restores it. Returns the previous one. */
int sh_aug_test_set_floor_piece_cap(int cap);
/* Corners of `area` as the augmenter reads its floor polygon, or -1. */
int sh_aug_test_floor_corners(const sh_aas *a, unsigned area);
/* Lower the tree-depth limit the report checks; 0 restores 128. */
unsigned sh_aug_test_set_depth_limit(unsigned limit);
/* Turn the balanced rebuild of the generated tree on or off (on by default).
 * Returns the previous setting. */
int sh_aug_test_set_rebuild(int on);
/* The nearest-leap rule on bare leaps: from and to area, demon flags, start
 * and end. Returns the count kept and writes the kept input indices, in order. */
int sh_aug_test_nearest_leaps(const int *from, const int *to, const unsigned *flags,
                              const double (*start)[3], const double (*end)[3],
                              int n, int *kept);

/* Test access to the private aug_quad type. Allocate sh_aug_test_quad_size()
 * bytes for each quad buffer.
 */
size_t sh_aug_test_quad_size(void);
int    sh_aug_test_quad_init(void *quad, const double corners[4][3]);
double sh_aug_test_z_at(const void *quad, double x, double y);
int    sh_aug_test_quad_contains(const void *quad, double x, double y);
double sh_aug_test_quad_normal_z(const void *quad);
int    sh_aug_test_node_field4(double plane_c);
#endif

#endif /* SNAPMAP_PLUS_AAS_AUGMENT_H */
