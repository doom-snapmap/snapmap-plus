/* module_prefab.h -- Blueprint module prefabs: save the selected modules with
 * every entity inside them, place them in another map's Blueprint mode.
 *
 * Save serializes the open map (EditorMapToJson) on the game thread and cuts
 * the selected modules out of it in product code (module_prefab_map.c).
 * Place merges the file into a fresh serialization of the open map, builds an
 * idSnapMap and an idSnapMapEdit from the result, and installs it through the
 * engine's own Redo step: the undo list is pointed at a two-entry stand-in for
 * one call, so the native assignment, selection re-init and editor refresh all
 * run exactly as they do on Ctrl+Y, and the real undo history is restored
 * untouched. The new modules are then selected and the native Move grab is
 * requested; once Edit has entered, its duplicate flag is set so Space commits
 * "Duplicate Modules" (one undo step) and Esc deletes the placed group, as
 * after a native Duplicate. The editor budget is recomputed after the install;
 * a placement over budget is taken back by installing the map as it was. */
#ifndef SH_MODULE_PREFAB_H
#define SH_MODULE_PREFAB_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"
#include "snapmap_plus_iface.h"

/* Resolve the engine functions. Returns 1 when save and place are available. */
int sh_module_prefab_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* The +0x360 slot body (see snapmap_plus_iface.h). */
int sh_module_prefab_slot(struct sh_iface *self, sh_module_prefab_call *call);

/* Called by the EditorFrame detour after the engine's frame, on the game thread. */
void sh_module_prefab_tick(void *editor);

/* 1 while a placement is installing or its grab is live. */
int sh_module_prefab_active(void);

/* The Prefabs tab's watch (sh_prefab_watch_fn): the modules selected in
 * Blueprint (0 outside Blueprint, -1 unreadable), and 1 while a placement is
 * held, from the moment its modules are installed (the editor budget counts
 * them from then on) until Space or Esc ends the grab. Any thread. */
int sh_module_prefab_selected_count(void);
int sh_module_prefab_held(void);

#endif
