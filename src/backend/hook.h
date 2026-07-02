/* hook.h -- the SnapHak backend's inline-detour installer (port of OG FUN_180001790).
 *
 * The backend places the real engine detours (rawmap save/load swap, the sh apply chain in
 * the feature ops). This installer ports SnapHak's own installer (truth snaphak/hook-install-mechanism.md):
 * VirtualProtect RWX -> overwrite the target prologue with a jump to the detour -> restore page
 * protection -> record the original bytes in a global un-patch list so every patch is reversible
 * (OG's DAT_18003e588 chain; OG reverses the whole list on unload).
 *
 * One deliberate divergence from OG's 12-byte `mov rax,h; push rax; ret`: a 14-byte abs-jmp
 * (`FF 25 00000000 <addr64>`) instead, because our DLL sits >2GB from DOOMx64vk.exe (a rel32 jmp
 * can't reach) and the abs-jmp clobbers no register. The reversible un-patch list is the OG-faithful
 * part the backend spine relies on.
 */
#ifndef BACKEND_HOOK_H
#define BACKEND_HOOK_H

#include <stddef.h>

/* Detour `target` to `detour`. `stolen` = the byte count of WHOLE, position-independent instructions
 * at the target's start (>=14; no RIP-relative / relative jmp|call in that range). Records the
 * originals in the un-patch list. Returns a trampoline (call it to invoke the original), or NULL. */
void *install_inline_hook(void *target, void *detour, size_t stolen);

/* Reverse ONE installed patch: restore the original bytes + free its trampoline. `tramp` is the
 * value install_inline_hook returned. Returns 1 on success, 0 if not a known patch. */
int hook_unpatch(void *tramp);

/* Reverse EVERY installed patch (LIFO, like OG's FUN_1800019c0 param_2==0 branch). Returns the count
 * reverted. Call on unload to leave the engine byte-clean. */
int hook_unpatch_all(void);

/* Number of patches currently installed (un-reverted). */
int hook_installed_count(void);

#endif /* BACKEND_HOOK_H */
