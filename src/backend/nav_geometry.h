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
