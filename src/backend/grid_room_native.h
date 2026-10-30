#ifndef SH_GRID_ROOM_NATIVE_H
#define SH_GRID_ROOM_NATIVE_H
#include "grid_room.h"
#include "signatures.h"
#include "grid_room_areas.h"
#include <stdint.h>

/* Bind the private module lookup on a quiescent boot thread. Runtime creation
 * occurs only on the engine thread, through the native module loader. */
int sh_grid_native_install(const sig_result *results,size_t count,const uint8_t *base);
/* Bundle modules. `served(request, module)` answers 1 when an installed mod
 * bundle serves `request` (module 1: as a snapModule decl, the name a map's
 * moduleName holds). A module lookup the stock palette cannot answer and the
 * bundle serves is loaded into its own private palette, as a Grid Room variant
 * is (log line "MODS: module <name> loaded into a private palette"). */
typedef int (*sh_grid_module_served_fn)(const char *request,int module);
void sh_grid_native_set_module_source(sh_grid_module_served_fn served);
/* "maps/modules/<path>.decl" -> "<path>" (the snapModuleInfo name). 1 ok. */
int sh_grid_module_logical(const char *name,char *out,size_t cap);
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
/* world_area_cell.c reports whether Play's world-area cell size follows the
 * map; while it does, resizes are not limited by the 8192 render area limit
 * unless no cell size fits. */
void sh_grid_native_set_adaptive_cells(int live);
/* Count a map's placed modules (map object: records at +0x750, count +0x758)
 * and choose Play's world-area cell size. Engine thread; 0 if not countable. */
int sh_grid_native_render_cell(void *map,sh_render_cell_choice *out);
/* The map open in the editor, or NULL. */
void *sh_grid_native_editor_map(void);
/* The current editor map's module-instance index of a 0x98-byte record, or -1. */
int sh_grid_native_record_instance(const void *record);
/* Rebuild the editor's placement surfaces in a process-heap scope. Engine
 * thread. 1 rebuilt, 0 none built yet, -1 refused. */
int sh_grid_native_refresh_placement(void);
#endif
