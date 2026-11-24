#ifndef SH_GRID_ROOM_ASSET_H
#define SH_GRID_ROOM_ASSET_H

#include "grid_room.h"

typedef unsigned char *(*sh_grid_asset_read_fn)(void *context,const char *name,size_t *length);
typedef void (*sh_grid_asset_release_fn)(void *context,void *bytes);

/* One canonical identity for every admitted private alias; the input must already use engine slash and case
 * normalization. Returns 1 with a malloc-owned path, 0 for an unrelated name, -1 for an invalid private identity or
 * allocation failure. */
int sh_grid_asset_canonical(const char *name, char **path);

/* Decodes only the feature's private names, then transforms the effective stock source. Returns 0 for an unrelated
 * name, 1 with malloc-owned output, -1 when a claimed name could not be served; a claimed failure never falls through
 * to stock geometry. */
int sh_grid_asset_open(const char *name,sh_grid_asset_read_fn read,
                        sh_grid_asset_release_fn release,void *context,
                        unsigned char **out,size_t *length);

/* Module stand-ins: a mod module whose unit is unavailable stands in as the Grid Room of its recorded size under its
 * own name (sh_cache_module_standin in modcache.h answers which, bound at install). `logical` is the module name
 * without maps/modules/ and .decl; returns 1 with the room's size, and the decl and snapModuleInfo stay the cache's. */
typedef int (*sh_grid_standin_fn)(const char *logical, sh_grid_size *size);
void sh_grid_asset_set_standins(sh_grid_standin_fn fn);
/* Returns 1 with the Grid Room's own name of the file in `out` when `name` is a stand-in's derived file, else 0. */
int  sh_grid_asset_standin_name(const char *name, char *out, size_t cap);

#endif
