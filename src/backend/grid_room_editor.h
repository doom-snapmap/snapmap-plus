#ifndef SH_GRID_ROOM_EDITOR_H
#define SH_GRID_ROOM_EDITOR_H

#include "grid_room.h"
#include "signatures.h"

/* The native properties panel owns selection. The resource adapter resolves
 * a placed module on the engine thread and applies one native edit transaction.
 * Returning zero hides/refuses the control for unavailable or non-grid rooms. */
typedef int (*sh_grid_editor_read_fn)(void *map_data,int instance,sh_grid_size *size);
typedef int (*sh_grid_editor_apply_fn)(void *map_data,int instance,const sh_grid_size *size);

int sh_grid_editor_install(const sig_result *results,size_t count,
                            sh_grid_editor_read_fn read,
                            sh_grid_editor_apply_fn apply);

#endif
