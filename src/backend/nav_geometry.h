/* nav_geometry.h -- solid-space clipping and walkable support construction. */
#ifndef SNAPMAP_PLUS_NAV_GEOMETRY_H
#define SNAPMAP_PLUS_NAV_GEOMETRY_H
#include "aas_augment.h"

/* Boxes are supplied as one face and its extrusion, not axis-aligned bounds.
 * All upward faces are considered. Output convex cells have agent clearance
 * applied once, across unions and contacts within the native step height.
 * Returns -1 on invalid geometry or a
 * capacity failure; callers must discard the entire candidate bake. */
int sh_nav_geometry_build(const sh_aug_platform *boxes, int count,
    double radius, double height, double floor_cos, double step, sh_aug_platform *out,
    int *source, int *pieces, unsigned char *buried, int capacity);

/* Continuous segment/solid test for a standing agent. Support solids at the
 * two traversal endpoints may be excluded by their source indices. */
int sh_nav_geometry_path_clear(const sh_aug_platform *boxes, int count,
    int skip_a, int skip_b, const double start[3], const double end[3],
    double radius, double height);

#endif
