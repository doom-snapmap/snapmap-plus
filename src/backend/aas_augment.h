/* aas_augment.h -- give an author's own geometry real AI navigation.
 *
 * THE PROBLEM
 * -----------
 * A SnapMap module ships its navigation baked by id's tools, and the engine's
 * AI cannot occupy space that has no AAS area. The Grid Room -- the box most
 * custom maps are built in -- ships nine areas, all flat: seven floor slabs at
 * z=0 and a ceiling plane at z=3360. Everything an author stacks inside it is
 * invisible to demons.
 *
 * This module takes a shipped payload and ADDS to it: one walkable area per
 * marked surface, spliced into the BSP tree so the engine can actually find it,
 * and the reachabilities that join it to what is already there.
 *
 * WHAT IT EMITS, AND WHAT IT DELIBERATELY DOES NOT
 * -----------------------------------------------
 * Three link regimes, chosen by the height difference to the floor outside each
 * platform edge:
 *
 *   drop <= maxStepHeight   a reciprocated pair of plain 0x20 walk links, no
 *                           animation at all. maxStepHeight is 18 in every
 *                           SnapMap monster class, and across 94,327 shipped
 *                           plain-walk records joining two flat areas not one
 *                           spans more than that. This is why short volumes
 *                           have always half-worked.
 *   drop downward           a walk-off-ledge link, gated on maxFallHeight
 *                           (0 in every monster-class module, so `fall=auto`
 *                           emits none -- see sh_aas_aug_opts).
 *   drop over maxStepHeight a baked TRAVERSAL: the animated climb, emitted once
 *                           per demon per usable edge, in both directions. The
 *                           demon is named arithmetically in the reachability's
 *                           travel_flags, and the animation is looked up in the
 *                           player's own universal traversal table -- see
 *                           nav_traversal.h. With no table loaded, or for a demon
 *                           with no animation reaching that height, the platform
 *                           is simply an ISLAND for that demon.
 *
 * An island is not an error. An area with no reachability is legal -- the Grid
 * Room's own ceiling is one in the shipped file -- and demons on it hunt and
 * fight across it normally; they simply cannot walk on or off. Authors reach a
 * tall platform today either by spawning demons on it or by chaining ledges no
 * more than maxStepHeight apart, which the step regime links for free.
 *
 * EVERY MAGIC NUMBER HERE WAS MEASURED
 * ------------------------------------
 * The constants in the .c cite the shipped data they came from -- the modal BSP
 * floor epsilon, the node field4 values per split-plane orientation, the walk
 * reachability spacing and end insets, the cluster bookkeeping invariant. None
 * of them is a guess, and where a value is copied without being understood the
 * comment says so.
 *
 * NO GAME BYTES: tests synthesize payloads. See README.md.
 */
#ifndef SNAPMAP_PLUS_AAS_AUGMENT_H
#define SNAPMAP_PLUS_AAS_AUGMENT_H

#include <stddef.h>
#include "aas_edit.h"

#define SH_AUG_MAX_PLATFORMS   512
#define SH_AUG_NAME_CAP        64
#define SH_AUG_REASON_CAP      160

/* One surface to make walkable, in the payload's own module-local space: an
 * ORIENTED CONVEX QUAD, four corners each with their own z, wound CLOCKWISE
 * seen from +Z.
 *
 * Not a rect. A Blocking Box carries a full rotation, and 12.4% of the volumes
 * in a real map are not upright, so the surface can be yawed, tilted, or a side
 * face of a box lying down. `n` is the face's unit outward normal and `face` its
 * OBB index -- 4 is an UPRIGHT box's top, which is what `side_face` compares
 * against. */
typedef struct sh_aug_platform {
    float c[4][3];                      /* the face, CW seen from +Z */
    float n[3];                         /* unit outward normal */
    int   face;                         /* OBB face index; 4 == upright top */
    char  name[SH_AUG_NAME_CAP];        /* for the report only */
} sh_aug_platform;

/* `fall`: emit walk-off-ledge links. AUTO honours settings.maxFallHeight, which
 * is 0 in every monster-class module, so AUTO emits none -- ALWAYS is an
 * off-precedent deviation offered for testing, not for authors. */
enum { SH_AUG_FALL_AUTO = 0, SH_AUG_FALL_NEVER, SH_AUG_FALL_ALWAYS };

/* `traversal`: emit baked climbs. AUTO offers every demon the table says can
 * reach that height; NEVER makes every out-of-step-range platform an island,
 * which is what the earlier build did unconditionally. */
enum { SH_AUG_TRAVERSAL_AUTO = 0, SH_AUG_TRAVERSAL_NEVER = 1 };

typedef struct sh_aug_opts {
    int   fall;             /* SH_AUG_FALL_* */
    int   inset;            /* inset by the agent radius; 1 unless testing */
    int   traversal;        /* SH_AUG_TRAVERSAL_* */
} sh_aug_opts;

/* What one platform became, per class, so the author can be told the truth. */
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
    char     reason[SH_AUG_REASON_CAP]; /* why it was not emitted */
} sh_aug_platform_result;

typedef struct sh_aug_report {
    sh_aug_platform_result platforms[SH_AUG_MAX_PLATFORMS];
    int      platform_count;
    unsigned areas_before, areas_after;
    unsigned reach_before, reach_after;
    unsigned depth_before, depth_after;
    int      depth_exceeded;            /* past the loader's 0x80 limit */
} sh_aug_report;

/* Augment `a` in place. Returns 1 if the model is still coherent (even if every
 * platform was refused), 0 only if the model was left unusable -- in which case
 * the caller must discard it and serve the shipped bytes.
 *
 * A platform is refused, never fudged: too small for the agent once inset, too
 * little headroom for the class, or outside the int16 range areaBounds uses.
 * Each refusal is a line in the report, because refusing one box must not cost
 * the author the other forty. */
int sh_aas_augment(sh_aas *a, const sh_aug_platform *plats, int n,
                   const sh_aug_opts *opts, sh_aug_report *out);

/* Which area a point resolves to by walking the BSP -- the same query the
 * engine's own aas_findArea makes. 0 means void. Exposed because the serving
 * path uses it to prove a generated area is actually findable before trusting
 * the payload. */
int sh_aas_point_area(const sh_aas *a, float x, float y, float z);

/* The BSP tree's depth, and the loader's hard limit on it. */
unsigned sh_aas_tree_depth(const sh_aas *a);

#ifdef SH_AUG_TESTING
/* The candidate anchor positions along one platform edge, midpoint first. A
 * climb link's outer endpoint sits the chosen animation's own offset out from
 * the wall, and that offset differs per demon, so one sampled position is not
 * enough: it decides for every demon at once. Exposed so the contract that
 * matters -- midpoint first, then distinct positions spanning the edge -- is
 * pinned rather than assumed. */
int sh_aug_test_trav_anchors(double lo, double hi, double *out, int cap);
/* Cap the anchors tried per demon (1 = the old single-midpoint behaviour).
 * Returns the previous cap. 0 = uncapped. */
int sh_aug_test_set_anchor_cap(int n);

/* The quad geometry, through an opaque buffer: `aug_quad` is internal to the
 * .c and the tests are a separate translation unit. Size a local array with
 * sh_aug_test_quad_size() and pass it as `quad`. */
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
