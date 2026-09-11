/* Per-demon clearance for the two admitted installed Grid Room AAS files. */
#ifndef SH_GRID_ROOM_NAV_H
#define SH_GRID_ROOM_NAV_H
#include "grid_room.h"
#include "aas_edit.h"
/* Mutates a private parsed payload. Failure invalidates it. The resource
 * provider validates serialized output before giving it to the engine. */
int sh_grid_nav_resize(sh_aas *aas, const sh_grid_size *size);
#endif
