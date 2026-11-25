#pragma once
#include "nav_regions.h"
#include <string.h>

static int sh_nav_map_equal(const sh_nav_map *a, const sh_nav_map *b)
{
    if (!a || !b) return a == b;
    if (a->instance_count != b->instance_count || a->region_count != b->region_count ||
        a->obstacle_count != b->obstacle_count || a->truncated != b->truncated ||
        a->invalid_geometry != b->invalid_geometry || a->unmeasured != b->unmeasured ||
        a->ids_unusable != b->ids_unusable || a->unshaped != b->unshaped ||
        a->dropped_total != b->dropped_total || a->dropped_kept != b->dropped_kept ||
        memcmp(a->dropped_by, b->dropped_by, sizeof a->dropped_by) ||
        memcmp(a->dropped, b->dropped, (size_t)a->dropped_kept * sizeof a->dropped[0]))
        return 0;
    if (a->instance_count &&
        memcmp(a->instances, b->instances, (size_t)a->instance_count * sizeof *a->instances))
        return 0;
    if (a->region_count &&
        memcmp(a->regions, b->regions, (size_t)a->region_count * sizeof *a->regions))
        return 0;
    if (a->obstacle_count &&
        memcmp(a->obstacles, b->obstacles, (size_t)a->obstacle_count * sizeof *a->obstacles))
        return 0;
    return 1;
}
