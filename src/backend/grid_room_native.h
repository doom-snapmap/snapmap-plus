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
/* A published package module variant's native record (maps/modules/smp/<token>/
 * <R>.decl), loaded into its own palette on first use. Engine thread only, with
 * the editor's module palette built; NULL when it cannot be admitted. */
void *sh_grid_native_variant_record(const char *name);
/* 1 when wrapper belongs to an admitted module variant. */
int sh_grid_native_variant_wrapper(const void *wrapper);
#endif
