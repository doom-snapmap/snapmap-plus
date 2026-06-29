/* hook.h -- a minimal hand-rolled inline-detour installer (SnapHak-style, abs-jmp variant). */
#ifndef SHIELD_HOOK_H
#define SHIELD_HOOK_H

#include <stddef.h>

/* Detour `target` to `detour`. `stolen` = the byte count of WHOLE, position-independent instructions at
 * the target's start (>=14; no RIP-relative / relative jmp|call in that range). Returns a trampoline
 * (call it to invoke the original), or NULL on failure. */
void *install_inline_hook(void *target, void *detour, size_t stolen);

#endif /* SHIELD_HOOK_H */
