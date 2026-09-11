#ifndef SH_GRID_ROOM_ASSET_H
#define SH_GRID_ROOM_ASSET_H

#include "grid_room.h"

typedef unsigned char *(*sh_grid_asset_read_fn)(void *context,const char *name,size_t *length);
typedef void (*sh_grid_asset_release_fn)(void *context,void *bytes);

/* Decode only the feature's private names, then transform the installed stock
 * source. 0 = unrelated name, 1 = malloc-owned output, -1 = claimed name could
 * not be served. A claimed failure must never fall through to stock geometry.
 * Readers must bypass generated resources to avoid recursion. */
int sh_grid_asset_open(const char *name,sh_grid_asset_read_fn read,
                        sh_grid_asset_release_fn release,void *context,
                        unsigned char **out,size_t *length);

#endif
