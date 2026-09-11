#ifndef SH_GRID_ROOM_NATIVE_H
#define SH_GRID_ROOM_NATIVE_H
#include "grid_room.h"
#include "signatures.h"
#include <stdint.h>

/* Bind the private module lookup on a quiescent boot thread. Runtime creation
 * occurs only on the engine thread, through the native module loader. */
int sh_grid_native_install(const sig_result *results,size_t count,const uint8_t *base);
int sh_grid_native_read(void *edit_data,int instance,sh_grid_size *size);
int sh_grid_native_apply(void *edit_data,int instance,const sh_grid_size *size);
void *sh_grid_native_wrapper(const sh_grid_size *size);
#endif
