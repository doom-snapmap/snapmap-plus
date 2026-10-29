/* nav_geometry.h -- solid-space clipping and walkable support construction. */
#ifndef SNAPMAP_PLUS_NAV_GEOMETRY_H
#define SNAPMAP_PLUS_NAV_GEOMETRY_H
#include "aas_augment.h"

/* Boxes are one face plus its extrusion. Consider all upward faces and apply
 * agent clearance once across support unions and step-height contacts.
 * Returns -1 for invalid geometry or exhausted capacity; discard the entire
 * candidate bake.
 */
int sh_nav_geometry_build(const sh_aug_platform *boxes, int count,
    double radius, double height, double floor_cos, double step, sh_aug_platform *out,
    int *source, int *pieces, unsigned char *buried, int capacity);

/* Existing module floors contribute support at contacts within step height.
 * These convex polygons are neither emitted again nor treated as solid boxes. */
int sh_nav_geometry_build_supported(const sh_aug_platform *boxes, int count,
    const sh_aug_platform *support, int support_count,
    double radius, double height, double floor_cos, double step, sh_aug_platform *out,
    int *source, int *pieces, unsigned char *buried, int capacity);

/* Ceiling on the agent-footprint strips cut along exposed edges: 200 bytes
 * each, so about 26 MB at worst. */
#define SH_NAV_GEOMETRY_MAX_STRIPS 131072

typedef struct sh_nav_geometry_stats {
    int faces_dropped;              /* walkable faces left out whole */
    int strips_dropped;             /* edge strips past the strip ceiling */
    unsigned char *source_dropped;  /* optional, one per box: 1 when it lost a face */
} sh_nav_geometry_stats;

/* As sh_nav_geometry_build_supported, but degrading: a face whose pieces, clip
 * polygons or edge strips do not fit is left out whole and counted, and the
 * rest is built. A face is never emitted partly cut. The boxes behind a
 * dropped face still subtract from every other face. Returns -1 only for
 * invalid input. */
int sh_nav_geometry_build_budget(const sh_aug_platform *boxes, int count,
    const sh_aug_platform *support, int support_count,
    double radius, double height, double floor_cos, double step, sh_aug_platform *out,
    int *source, int *pieces, unsigned char *buried, int capacity,
    sh_nav_geometry_stats *stats);

/* Cut walkable support polygons -- a module's own floor areas -- around the
 * marked solids standing on them or hanging lower over them than the agent is
 * tall, by exactly the rule a box face is cut around the solids standing in
 * it: the occupied standing space of each solid, grown by the agent's
 * clearance, is subtracted and the remainder merged into convex pieces.
 * A solid whose top is within step height of the support, or whose underside
 * is at least the agent's height above it, leaves the support whole.
 * Walls that only block (obstacle_only) are not cut out.
 *
 * Pieces of support[i] go to out[] with owner[k] = i; cut[i] is 1 when support
 * i lost ground, 0 when it is untouched (and then it emits nothing), 2 when
 * its pieces would not fit `per_support` and it was left whole, and 3 when the
 * solids cover all of it (nothing standable is left; it emits nothing).
 * Returns the piece count, or -1 for invalid input. */
int sh_nav_geometry_cut_support(const sh_aug_platform *boxes, int count,
    const sh_aug_platform *support, int support_count,
    double radius, double height, double floor_cos, double step,
    sh_aug_platform *out, int *owner, unsigned char *cut, int capacity,
    int per_support);

/* Continuous segment/solid test for a standing agent. Support solids at the
 * two traversal endpoints may be excluded by their source indices. */
int sh_nav_geometry_path_clear(const sh_aug_platform *boxes, int count,
    int skip_a, int skip_b, const double start[3], const double end[3],
    double radius, double height);

/* Last intersection along an outward standing-body ray, in direction units.
 * A clear ray returns distance zero. Used to place floor-side traversal
 * endpoints outside the real solid, including tilted overhangs. */
int sh_nav_geometry_ray_exit(const sh_aug_platform *box,const double start[3],
    const double direction[3],double radius,double height,double *distance);

#endif
