/* saved_map_guard.h -- DOOM never deletes a saved map it cannot read. The guard replaces
 * idSnapBrowserHelper::DeleteBadSaveSlots: each slot stays untouched, a verified copy goes to
 * %LOCALAPPDATA%\snapmap-plus\kept-maps\SNAPMAPS<id>\, and the player is told once per session where it is.
 * The player's own Delete in My Maps is a different engine path and still works. */
#ifndef SHIELD_SAVED_MAP_GUARD_H
#define SHIELD_SAVED_MAP_GUARD_H

#include <stddef.h>
#include "../backend/signatures.h"

/* Detour DeleteBadSaveSlots. Returns 1 when the guard is in place. */
int saved_map_guard_install(const sig_result *results, size_t count);

#endif /* SHIELD_SAVED_MAP_GUARD_H */
