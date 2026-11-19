/* map_uses.h -- what a map uses of Snapmap+ itself.
 *
 * A map is modded when it places something a mod provides, a mod is set on the whole map, or it uses a feature of
 * Snapmap+ that a game without it cannot load (PLATFORM, Vanilla stays vanilla, owner 2026-11-19): a module resized
 * past its stock size (a Grid Room size, grid_room.h) and navigation of the map's own (a Blocking Box marked for AI
 * navigation, nav_regions.h). Everything else SnapMap can do without mods (cinematic cameras, timelines, water
 * volumes, logic, a stock entity an editor tweak unhid) leaves the map vanilla. The save records these features as
 * the product's own entry of the map's rows (modmanifest.h sh_modmanifest_list.uses), which carries the `mods` search
 * tag. Pure text work, any thread. */
#ifndef SNAPMAP_PLUS_MAP_USES_H
#define SNAPMAP_PLUS_MAP_USES_H

#include <stddef.h>

/* SH_MODMANIFEST_USES_* bits for a map text and its placed names (modmanifest.h sh_modmanifest_placed_names). */
unsigned sh_map_uses(const char *json, size_t length, const char *const *names, size_t count);

#endif /* SNAPMAP_PLUS_MAP_USES_H */
