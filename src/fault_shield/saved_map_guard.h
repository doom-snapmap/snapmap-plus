/* saved_map_guard.h -- DOOM never deletes a saved map it cannot read.
 *
 * Every time the My Maps list is built, DOOM's own scan collects the save
 * slots its savegame manager could not read and deletes them
 * (idSnapBrowserHelper::DeleteBadSaveSlots, then the "removed corrupt data"
 * notice). The guard replaces that function: each slot stays in the saved
 * games folder untouched, a verified copy of its files goes to
 * %LOCALAPPDATA%\snapmap-plus\kept-maps\SNAPMAPS<id>\, and the player is told
 * once per session, in one sentence, what happened and where the copy is. The
 * player's own Delete in My Maps is a different engine path and still works. */
#ifndef SHIELD_SAVED_MAP_GUARD_H
#define SHIELD_SAVED_MAP_GUARD_H

#include <stddef.h>
#include "../backend/signatures.h"

/* Detour DeleteBadSaveSlots. Returns 1 when the guard is in place. */
int saved_map_guard_install(const sig_result *results, size_t count);

#endif /* SHIELD_SAVED_MAP_GUARD_H */
