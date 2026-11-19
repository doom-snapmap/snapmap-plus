#ifndef SH_GRID_ROOM_ASSET_H
#define SH_GRID_ROOM_ASSET_H

#include "grid_room.h"

typedef unsigned char *(*sh_grid_asset_read_fn)(void *context,const char *name,size_t *length);
typedef void (*sh_grid_asset_release_fn)(void *context,void *bytes);

/* One canonical identity for every admitted private alias. Input must already
 * use engine slash/case normalization. 1 returns a malloc-owned path, 0 means
 * unrelated, -1 means an invalid private identity or allocation failure. */
int sh_grid_asset_canonical(const char *name, char **path);

/* Decode only the feature's private names, then transform the effective stock
 * source. 0 = unrelated name, 1 = malloc-owned output, -1 = claimed name could
 * not be served. A claimed failure must never fall through to stock geometry.
 * Readers resolve product defaults/originals below generated
 * resources, so an override of a stock input survives the transform without
 * recursively requesting the derived resource itself. */
int sh_grid_asset_open(const char *name,sh_grid_asset_read_fn read,
                        sh_grid_asset_release_fn release,void *context,
                        unsigned char **out,size_t *length);

/* Module stand-ins (D-A30, R-DS6). A mod module whose unit cannot be had stands in as the Grid Room of its recorded
 * size under its own name (modcache.h sh_cache_module_standin answers which, bound at install): `logical` is the
 * module's name without maps/modules/ and .decl ("mods/<salt>/rooms/hall"); 1 with the room's size. Every file the
 * native module loader derives from that name (its collision, geometry, light probe, navigation and palette render
 * model) is then the room's file of that size; its decl and snapModuleInfo stay the content cache's. */
typedef int (*sh_grid_standin_fn)(const char *logical, sh_grid_size *size);
void sh_grid_asset_set_standins(sh_grid_standin_fn fn);
/* `name` as the Grid Room's own name of the file when it is a stand-in's derived file (1, in `out`), else 0. */
int  sh_grid_asset_standin_name(const char *name, char *out, size_t cap);

#endif
