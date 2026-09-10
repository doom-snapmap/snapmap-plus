/* Reversible inline detours using a register-preserving 14-byte absolute jump.
 * The jump can reach the backend DLL even when it is more than 2 GB from DOOM. */
#ifndef BACKEND_HOOK_H
#define BACKEND_HOOK_H

#include <stddef.h>

/* Detour target and return its trampoline, or NULL. stolen must be 14..48 bytes.
 * Callers that execute the trampoline must supply whole, position-independent
 * instructions: no RIP-relative operands or relative calls/jumps are relocated. */
void *install_inline_hook(void *target, void *detour, size_t stolen);

/* Release the patch identified by its trampoline. Return 1 if found, 0 otherwise.
 * Restoration is attempted before the trampoline is freed. */
int hook_unpatch(void *tramp);

/* Release all live patches in reverse slot order; return the number released. */
int hook_unpatch_all(void);

/* Count live patch records. */
int hook_installed_count(void);

#endif /* BACKEND_HOOK_H */
