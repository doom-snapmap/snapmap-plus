/* Add walkable surfaces, BSP leaves and reachabilities to shipped AAS.
 *
 * Step-height differences receive reciprocal walk links. Larger changes use
 * per-demon traversal animations from nav_traversal; downward walk-off links
 * follow maxFallHeight. An area can remain a valid island when no route is
 * available. Tests use synthetic payloads.
 */
#ifndef SNAPMAP_PLUS_AAS_AUGMENT_H
#define SNAPMAP_PLUS_AAS_AUGMENT_H

#include <stddef.h>
#include "aas_edit.h"

/* Ceiling on the boxes one module copy offers and on the walkable pieces they
 * cut into, per demon class. Scratch is about 3 KB per piece plus a 300-byte
 * report entry, 1.5 KB of clip polygons and an 850 KB side buffer, so the
 * ceiling bounds one bake at roughly 11 MB (edge strips aside, see
 * nav_geometry.h). A box past it is left out and reported; it never refuses
 * the module. */
#define SH_AUG_MAX_PLATFORMS   2048
#define SH_AUG_NAME_CAP        64
#define SH_AUG_REASON_CAP      160
#define SH_AUG_MAX_CORNERS     32
#define SH_AUG_MAX_BLAMED      8
#define SH_AUG_MAX_DROPPED     16

/* A convex surface in module-local coordinates, wound clockwise from +Z.
 * Input boxes use four corners; prepared geometry may have more. n is its
 * outward normal and face its OBB face index (4 is an upright box's top).
 */
typedef struct sh_aug_platform {
    float c[SH_AUG_MAX_CORNERS][3];      /* convex boundary, CW seen from +Z */
    float n[3];                         /* unit outward normal */
    int   face;                         /* OBB face index; 4 == upright top */
    float depth;                        /* the solid's extent along -n behind the face */
    char  name[SH_AUG_NAME_CAP];        /* for the report only */
    int   corners;                      /* zero means four input box corners */
    int   prepared;                     /* clearance already applied to the union */
    int   obstacle_only;                /* collide, but never create support */
    float support[4][3];                /* original box face behind a prepared cell */
} sh_aug_platform;

/* Walk-off links: AUTO follows settings.maxFallHeight (zero in shipped
 * monster modules); ALWAYS overrides that limit for testing.
 */
enum { SH_AUG_FALL_AUTO = 0, SH_AUG_FALL_NEVER, SH_AUG_FALL_ALWAYS };

/* Traversal AUTO selects available demon animations; NEVER omits climbs. */
enum { SH_AUG_TRAVERSAL_AUTO = 0, SH_AUG_TRAVERSAL_NEVER = 1 };

typedef struct sh_aug_opts {
    int   fall;             /* SH_AUG_FALL_* */
    int   inset;            /* inset by the agent radius; 1 unless testing */
    int   traversal;        /* SH_AUG_TRAVERSAL_* */
    /* Outgoing routes one area may keep, shipped ones included; 0 means the
     * engine's 256. An area whose own shipped routes already pass it keeps
     * those and gains nothing. Lowered only to fit the payload cap. */
    int   route_budget;
} sh_aug_opts;

/* Per-class outcome for one emitted or refused surface. */
typedef struct sh_aug_platform_result {
    char     name[SH_AUG_NAME_CAP];
    int      emitted;                   /* 1 if an area was added */
    int      area;                      /* the new area index, or -1 */
    int      carrier;                   /* the area the platform sits over */
    int      leaf_slots_carved;
    int      links;                     /* reachabilities touching this area */
    int      climbs;                    /* baked traversals touching it */
    int      demons;                    /* distinct demons offered a climb */
    int      island;                    /* 1 if nothing links it */
    float    tilt_degrees;              /* the chosen face's angle from horizontal */
    int      side_face;                 /* 1 if that face is not the box's top */
    int      neighbours;                /* distinct areas this platform links to */
    int      leaps;                     /* gap links touching it */
    float    centre[3];                 /* emitted piece centre, used to verify BSP lookup */
    int      source;                    /* which requested volume this came from */
    int      pieces;                    /* walkable pieces from the source volume; 1 if unsplit */
    char     reason[SH_AUG_REASON_CAP]; /* why it was not emitted */
} sh_aug_platform_result;

/* Why a floor-side climb onto one face of a box was not placed, most
 * frequent first in the report. */
enum {
    SH_AUG_FACE_BLOCKED = 0,    /* another solid overhangs or stands in the run-up */
    SH_AUG_FACE_NO_FLOOR,       /* the run-up point is not on walkable floor */
    SH_AUG_FACE_NO_TOP,         /* the landing is not on the box's own top */
    SH_AUG_FACE_HEIGHT,         /* no animation of that demon fits the rise */
    SH_AUG_FACE_INSIDE,         /* the edge is a cut inside the box, not a face */
    SH_AUG_FACE_NO_EDGE,        /* no edge of the top faces the floor there */
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
    /* One entry per walkable piece, on the heap: sh_aug_report_free releases
     * it. source identifies its requested volume; source_count counts
     * requested volumes.
     */
    sh_aug_platform_result *platforms;
    int      platform_count;
    int      source_count;
    int      pieces_truncated;          /* a box's geometry could not be cut;
                                         * the whole candidate is refused */
    /* Degraded, not refused: what was left out so the rest fits. */
    int      pieces_dropped;            /* walkable faces past the piece ceiling */
    int      sources_dropped;           /* boxes that lost a face that way */
    int      dropped_sources[SH_AUG_MAX_BLAMED];
    int      dropped_source_count;
    int      routes_dropped;            /* distinct routes left out at the 256 limit */
    int      route_areas;               /* areas that needed it */
    sh_aug_route_drop dropped[SH_AUG_MAX_DROPPED];
    int      dropped_count;
    int      edges_skipped;             /* edges whose floor walk passed its budget */
    int      file_full;                 /* the area table filled; later pieces skipped */
    int      segments_failed;           /* an edge could not be measured; refused */
    int      dead_gaps;                 /* unlinked pairs beyond step range but below the shortest available jump */
    unsigned areas_before, areas_after;
    unsigned reach_before, reach_after;
    unsigned traversal_points_before;   /* the shipped traversalPoints */
    unsigned depth_before, depth_after;
    int      depth_exceeded;            /* past the loader's 0x80 limit */
    int      links_truncated;           /* traversal allocation failed;
                                         * the whole candidate is refused */
    int      reach_limit_exceeded;      /* the module's own routes already exceed
                                         * 256 from one area, or a route names an
                                         * area that does not exist; refused */
    int      reach_limit_area;          /* that area, or -1 */
    /* Requested volumes routing into the full area: the ones a mapper can
     * move or delete to fit. Empty when the area is shipped floor whose own
     * links already fill it. */
    int      blamed[SH_AUG_MAX_BLAMED];
    int      blamed_count;
    int      anchors_reduced;           /* alternative basic/traversal samples removed */
    /* Module floor areas cut around the boxes standing on them (or hanging
     * lower over them than this class is tall), the areas their pieces added,
     * floors left whole because their pieces did not fit, and shipped routes
     * removed because an endpoint lay in the cut-out ground. */
    int      floors_cut;
    int      floor_pieces_added;
    int      floors_left_whole;
    int      shipped_routes_removed;
    int      floors_covered;            /* floor areas wholly under boxes, closed to routes */
    int      floor_walks_capped;        /* floor-piece walk samples left out to keep a floor well under 256 */
    int      barrier_links;             /* barrier-jump links, when built with them */
    /* Four per requested box (faces[source * 4 + edge]), on the heap with
     * platforms; NULL when no traversal table was ready. */
    sh_aug_face *faces;
    int      climbs_declined;           /* climbs/leaps omitted because a source area already owns traversal points */
} sh_aug_report;

/* Augment a in place. Returns 1 if the model remains coherent, including when
 * individual platforms are refused. On 0, discard the model and serve shipped
 * bytes. The report records per-platform size, clearance and coordinate-range
 * refusals.
 */
int sh_aas_augment(sh_aas *a, const sh_aug_platform *plats, int n,
                   const sh_aug_opts *opts, sh_aug_report *out);
void sh_aug_report_free(sh_aug_report *out);

/* The route budget (for sh_aug_opts.route_budget) that brings an augmented
 * model of `size` bytes under `cap`, judged from its generated routes per
 * area. Always below `current` (0 meaning 256). Returns 0 when no lower budget
 * can help: the budget is already 1, or the model has no generated route. */
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
/* Set the per-demon anchor cap; 0 is uncapped. Returns the previous cap. */
int sh_aug_test_set_anchor_cap(int n);
/* Turn barrier-jump links on or off (SH_AUG_BARRIER_JUMPS builds start on).
 * Returns the previous setting. */
int sh_aug_test_set_barrier(int on);
/* Lower the tree-depth limit the report checks; 0 restores 128. */
unsigned sh_aug_test_set_depth_limit(unsigned limit);

/* Test access to the private aug_quad type. Allocate sh_aug_test_quad_size()
 * bytes for each quad buffer.
 */
size_t sh_aug_test_quad_size(void);
int    sh_aug_test_quad_init(void *quad, const double corners[4][3]);
double sh_aug_test_z_at(const void *quad, double x, double y);
int    sh_aug_test_quad_inset(const void *quad, double r, void *out);
int    sh_aug_test_quad_contains(const void *quad, double x, double y);
void   sh_aug_test_quad_corner(const void *quad, int i, double out[3]);
double sh_aug_test_quad_normal_z(const void *quad);
int    sh_aug_test_node_field4(double plane_c);
#endif

#endif /* SNAPMAP_PLUS_AAS_AUGMENT_H */
