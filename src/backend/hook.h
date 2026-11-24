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

/* Why the most recent preparation returned NULL ("none" after a success). */
const char *hook_last_failure(void);

/* Like hook_prepare, but expand one caller-verified RIP-relative LEA at lea_offset into MOV reg,imm64.
 * Other stolen instructions must be whole and position-independent. */
void *hook_prepare_rip_lea(void *target, void *detour, size_t stolen, size_t lea_offset);

/* Like hook_prepare_rip_lea, for one MOV reg,[RIP+disp32] load at load_offset (a security-cookie read):
 * it becomes MOV reg,imm64 then MOV reg,[reg]. RSP, RBP, R12 and R13 destinations are refused. */
void *hook_prepare_rip_load(void *target, void *detour, size_t stolen, size_t load_offset);

/* Expand one caller-verified E8 rel32 call at call_offset to an indirect absolute call that uses no scratch
 * register; the callee returns past the inline address. A call back into the stolen interval is refused. */
void *hook_prepare_relative_call(void *target, void *detour, size_t stolen, size_t call_offset);

/* Expand one caller-verified near (0F 8x rel32) or short (7x rel8) conditional jump at jcc_offset into the
 * inverse short jump over an absolute jump to the same target. A jump into the stolen interval is refused. */
void *hook_prepare_relative_jcc(void *target, void *detour, size_t stolen, size_t jcc_offset);

/* Publish the trampoline in the detour's original callback BEFORE committing; the prologue is written with
 * every other thread suspended and outside it. A failed commit is removed or retried (FAIL_ROLLBACK: removed). */
sh_patch_status hook_commit(void *tramp);

/* True only after a successful commit and before any attempted removal. */
int hook_is_installed(void *tramp);

/* Call only after users have stopped executing the target and trampoline.
 * Return 1 after restoring bytes/protection and freeing the trampoline. On 0,
 * retain the caller's trampoline pointer and retry; its record remains owned. */
int hook_unpatch(void *tramp);

/* Count successfully committed installations. */
int hook_installed_count(void);

#ifdef SH_PATCH_TESTING
/* Release all live patches in reverse slot order; return the number released. */
int hook_unpatch_all(void);
/* Count every owned record, committed or not. */
int hook_owned_count(void);
#endif

#endif /* BACKEND_HOOK_H */
