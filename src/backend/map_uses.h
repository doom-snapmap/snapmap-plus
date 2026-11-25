/* map_uses.h -- what a map uses of Snapmap+ itself that a game without it cannot load: a resized Grid Room
 * (grid_room.h) or its own navigation (nav_regions.h). The save records it as sh_modmanifest_list.uses; pure text work. */
#ifndef SNAPMAP_PLUS_MAP_USES_H
#define SNAPMAP_PLUS_MAP_USES_H

#include <stddef.h>

/* SH_MODMANIFEST_USES_* bits for a map text and its placed names (modmanifest.h sh_modmanifest_placed_names). */
unsigned sh_map_uses(const char *json, size_t length, const char *const *names, size_t count);

#endif /* SNAPMAP_PLUS_MAP_USES_H */
