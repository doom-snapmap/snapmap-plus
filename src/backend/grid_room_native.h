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
/* The sentence most recently logged by an edit: applied, unchanged, limited or refused. */
const char *sh_grid_native_last_message(void);
/* Console entry (sh_gridsize). instance<0 selects the Blueprint selection, else
 * the module whose properties are open. requested NULL only reads the size. */
int sh_grid_native_selected(int instance,const float *requested,sh_grid_size *out,int *resolved);
/* Print the stock modern room's and every cached variant's module entity
 * templates (the entities a loaded room receives), one line each. */
int sh_grid_native_templates(void (*print)(const char *line));
#endif
