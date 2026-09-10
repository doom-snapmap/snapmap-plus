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

/* Continuous segment/solid test for a standing agent. Support solids at the
 * two traversal endpoints may be excluded by their source indices. */
int sh_nav_geometry_path_clear(const sh_aug_platform *boxes, int count,
    int skip_a, int skip_b, const double start[3], const double end[3],
    double radius, double height);

#endif
