/* map_uses.c -- see map_uses.h. */
#include <stddef.h>

#include "map_uses.h"
#include "modmanifest.h"
#include "grid_room.h"
#include "nav_regions.h"

unsigned sh_map_uses(const char *json, size_t length, const char *const *names, size_t count)
{
    unsigned uses = 0;
    size_t i;
    /* A placed module named by its size is a resized Grid Room: the stock room keeps its own name. */
    for (i = 0; i < count; i++) {
        sh_grid_size size;
        if (names && names[i] && sh_grid_parse_name(names[i], &size)) { uses |= SH_MODMANIFEST_USES_MODULE_SIZES; break; }
    }
    if (sh_nav_regions_marks(json, length) > 0) uses |= SH_MODMANIFEST_USES_NAVIGATION;
    return uses;
}
