/* Reversible inline detours using a register-preserving 14-byte absolute jump.
 * The jump can reach the backend DLL even when it is more than 2 GB from DOOM. */
#ifndef BACKEND_HOOK_H
#define BACKEND_HOOK_H

#include <stddef.h>
#include "patch.h"

/* Allocate a trampoline without changing target, or return NULL. stolen must be 14..48 bytes.
 * Callers that execute the trampoline must supply whole, position-independent
 * instructions: no RIP-relative operands or relative calls/jumps are relocated. */
void *hook_prepare(void *target, void *detour, size_t stolen);

/* Like hook_prepare, but expand one caller-verified 64-bit RIP-relative LEA
 * at lea_offset into MOV reg,imm64. All other stolen instructions must remain
 * whole and position-independent. Reject any other opcode/addressing form. */
void *hook_prepare_rip_lea(void *target, void *detour, size_t stolen, size_t lea_offset);

/* Expand one caller-verified E8 rel32 call to an indirect absolute call without
 * consuming a scratch register. The callee returns past the inline address.
 * Other stolen instructions must be whole and position-independent; calls back
 * into the stolen interval are refused. Useful for native stack-probe prologues. */
void *hook_prepare_relative_call(void *target, void *detour, size_t stolen, size_t call_offset);

/* Publish the trampoline in the detour's original callback BEFORE committing.
 * Commit never releases ownership. A failed commit must be removed or retried;
 * FAIL_ROLLBACK requires removal before another commit. Callers must quiesce
 * target execution while changing its prologue. */
sh_patch_status hook_commit(void *tramp);

/* True only after a successful commit and before any attempted removal. */
int hook_is_installed(void *tramp);

/* Call only after users have stopped executing the target and trampoline.
 * Return 1 after restoring bytes/protection and freeing the trampoline. On 0,
 * retain the caller's trampoline pointer and retry; its record remains owned. */
int hook_unpatch(void *tramp);

/* Release all live patches in reverse slot order; return the number released. */
int hook_unpatch_all(void);

/* Count successfully committed installations, or all owned records. */
int hook_installed_count(void);
int hook_owned_count(void);

#endif /* BACKEND_HOOK_H */
