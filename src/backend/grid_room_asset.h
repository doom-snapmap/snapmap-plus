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
 * Readers resolve compiled packages/product defaults/originals below generated
 * resources, so an override of a stock input survives the transform without
 * recursively requesting the derived resource itself. */
int sh_grid_asset_open(const char *name,sh_grid_asset_read_fn read,
                        sh_grid_asset_release_fn release,void *context,
                        unsigned char **out,size_t *length);

#endif
