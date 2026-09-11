#ifndef SH_GRID_ROOM_EDIT_H
#define SH_GRID_ROOM_EDIT_H
#include "grid_room.h"

/* Change only the origin in the native origin+mat3 local transform. A cap's
 * serialized edit state can be null after a native door/cap replacement. */
int sh_grid_cap_transform(const sh_grid_size *before,const sh_grid_size *after,
                          const float source[12],float result[12]);
#endif
