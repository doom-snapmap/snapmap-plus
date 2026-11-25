/* module_prefab.h -- Blueprint module prefabs: save the selected modules with their entities, place them in
 * another map's Blueprint mode.
 *
 * Place merges the file into a fresh serialization of the open map and installs the result through the
 * engine's own Redo step (the undo list points at a two-entry stand-in for one call), then starts the native
 * Move grab with the duplicate flag set so Space commits "Duplicate Modules" and Esc deletes the group.
 * A placement over the editor budget is taken back by installing the map as it was. */
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

/* The Prefabs tab's watch (sh_prefab_watch_fn): the modules selected in
 * Blueprint (0 outside Blueprint, -1 unreadable), and 1 while a placement is
 * held, from the moment its modules are installed (the editor budget counts
 * them from then on) until Space or Esc ends the grab. Any thread. */
int sh_module_prefab_selected_count(void);
int sh_module_prefab_held(void);

#endif
