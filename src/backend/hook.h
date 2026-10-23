/* Reversible inline detours using a register-preserving 14-byte absolute jump.
 * The jump can reach the backend DLL even when it is more than 2 GB from DOOM. */
#ifndef BACKEND_HOOK_H
#define BACKEND_HOOK_H

#include <stddef.h>
#include "patch.h"

/* Allocate a trampoline without changing target, or return NULL. stolen must be 14..48 bytes.
 * Callers that execute the trampoline must supply whole, position-independent
 * instructions: no RIP-relative operands or relative calls/jumps are relocated.
 * Every variant refuses a target whose live first instruction is a redirect
 * (see hook_target_is_redirect), whatever an earlier scan observed. */
void *hook_prepare(void *target, void *detour, size_t stolen);

/* Nonzero when target currently begins with a control transfer: a relative,
 * indirect or register jump, push/ret, or a relative call. Another component's
 * inline detour looks like this, and its displacement or continuation is only
 * valid at its own address, so its bytes can never be stolen as instructions. */
int hook_target_is_redirect(const void *target);

/* Like hook_prepare, but expand one caller-verified 64-bit RIP-relative LEA
 * at lea_offset into MOV reg,imm64. All other stolen instructions must remain
 * whole and position-independent. Reject any other opcode/addressing form. */
void *hook_prepare_rip_lea(void *target, void *detour, size_t stolen, size_t lea_offset);

/* Expand one caller-verified E8 rel32 call to an indirect absolute call without
 * consuming a scratch register. The callee returns past the inline address.
 * Other stolen instructions must be whole and position-independent; calls back
 * into the stolen interval are refused. Useful for native stack-probe prologues. */
void *hook_prepare_relative_call(void *target, void *detour, size_t stolen, size_t call_offset);

/* Relocate one exact REX.W MOV r64,[RIP+disp32]. Only 48/4c 8b /r with
 * ModRM mode00/rm101 is accepted. The destination is its sole scratch register;
 * RSP is refused. The expanded load preserves registers and flags. Other
 * instructions in the supplied window must be position independent. */
void *hook_prepare_rip_load(void *target, void *detour, size_t stolen, size_t load_offset);
/* Relocate one Jcc rel8 or0f Jcc rel32 to a target outside the stolen window.
 * An inverted short condition skips a register/flags-preserving absolute jump.
 * Both taken and fallthrough paths keep their native destination. */
void *hook_prepare_conditional_jump(void *target, void *detour, size_t stolen, size_t branch_offset);
/* Relocate exact 83 3d disp32 imm8: CMP dword[RIP+disp32],signext imm8.
 * Push/MOV/load/pop preserves RAX and the comparison's native flags; no other
 * register changes. The caller supplies valid native stack storage as for
 * its original prologue. Other stolen instructions remain position independent. */
void *hook_prepare_rip_cmp32_imm8(void *target, void *detour, size_t stolen, size_t compare_offset);

/* Relocate one exact FF15 [RIP+disp32] call, re-reading its dynamic pointer
 * slot on every call. Supported unwind contract: source starts with positive
 * 48 83 EC imm8 (8 mod16), and its native version1/flags0 unwind entry contains
 * only that allocation at prologue offset4. Caller proves other stolen whole
 * instructions are position independent, have no stack/nonvolatile changes
 * or epilogue, and use that allocation throughout. The complete native function
 * must end after the window with ADD RSP,imm8/RET, optionally preceded by exact
 * TEST RAX,RAX/SETNE AL. That verified suffix is also copied. Allocation within
 * disp32 reach retains the native FF15 and its dynamic slot/ABI. The complete
 * copied function has registered unwind data retained until safe unpatch.
 * Unknown/chained/handler/frame-pointer prologues are refused. */
void *hook_prepare_rip_indirect_call(void *target, void *detour, size_t stolen, size_t call_offset);

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

/* Release all live patches in reverse preparation order; return the number released. */
int hook_unpatch_all(void);

/* Count successfully committed installations, or all owned records. */
int hook_installed_count(void);
int hook_owned_count(void);

#endif /* BACKEND_HOOK_H */
