/* Flies the editor camera to an entity (Object Mode, cursor on it). */
#ifndef SH_FLY_TO_H
#define SH_FLY_TO_H

#include <stddef.h>
#include "signatures.h"

struct sh_iface;

/* Resolves GridSetEditorState for the Blueprint to Object Mode switch. Returns 1 when resolved; without it a request
 * made from Blueprint is refused. */
int sh_fly_to_install(const sig_result *results, size_t count);

/* Queues a fly-to for entity `id`, serviced by the next editor frames. `iface` (may be NULL) is kept for toasts.
 * Returns 1 when queued. */
int sh_fly_to_request(int id, struct sh_iface *iface);

/* Called by the EditorFrame detour after the engine's frame, on the game thread. */
void sh_fly_to_tick(void *editor);

#endif
