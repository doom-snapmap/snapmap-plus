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

#define SH_AUG_MAX_PLATFORMS   512
#define SH_AUG_NAME_CAP        64
#define SH_AUG_REASON_CAP      160
#define SH_AUG_MAX_CORNERS     32

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

typedef struct sh_aug_report {
    /* One entry per walkable piece. source identifies its requested volume;
     * source_count counts requested volumes.
     */
    sh_aug_platform_result platforms[SH_AUG_MAX_PLATFORMS];
    int      platform_count;
    int      source_count;
    int      pieces_truncated;          /* geometry capacity exhausted;
                                         * the whole candidate is refused */
    int      dead_gaps;                 /* unlinked pairs beyond step range but below the shortest available jump */
    unsigned areas_before, areas_after;
    unsigned reach_before, reach_after;
    unsigned depth_before, depth_after;
    int      depth_exceeded;            /* past the loader's 0x80 limit */
    int      links_truncated;           /* traversal allocation failed;
                                         * the whole candidate is refused */
    int      reach_limit_exceeded;      /* required routes cannot fit one area */
    int      anchors_reduced;           /* alternative basic/traversal samples removed */
    int      climbs_declined;           /* climbs/leaps omitted because a source area already owns traversal points */
} sh_aug_report;

/* Augment a in place. Returns 1 if the model remains coherent, including when
 * individual platforms are refused. On 0, discard the model and serve shipped
 * bytes. The report records per-platform size, clearance and coordinate-range
 * refusals.
 */
int sh_aas_augment(sh_aas *a, const sh_aug_platform *plats, int n,
                   const sh_aug_opts *opts, sh_aug_report *out);

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
/* Set the per-demon anchor cap; 0 is uncapped. Returns the previous cap. */
int sh_aug_test_set_anchor_cap(int n);

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
