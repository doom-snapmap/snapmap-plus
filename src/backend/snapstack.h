/* Shared backend SnapStack stores and handlers. The frontend uses this same
 * numbered-stack storage through interface slots; named groups belong to commands. */
#ifndef BACKEND_SNAPSTACK_H
#define BACKEND_SNAPSTACK_H

#include "snapmap_plus_iface.h"

/* Register SnapStack and its diagnostics once before the frontend loads.
 * Every command receives iface as its context. */
void sh_register_snapstack_commands_backend(sh_iface *iface);

/* Add IDs to a numbered stack under its lock, deduplicating existing IDs. */
void sh_snapstack_push_ids_backend(int index, const int *ids, int count);

/* Clear a numbered stack under its lock; return its previous ID count. */
int sh_snapstack_clear_stack_backend(int index);

#endif /* BACKEND_SNAPSTACK_H */
