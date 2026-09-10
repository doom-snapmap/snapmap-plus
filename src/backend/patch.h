/* Code patches with saved originals, signature gates, and inline-detour wrappers.
 * Expected-byte checks reject mismatches before writing; guarded copies can still
 * partially modify memory if a write faults. */
#ifndef BACKEND_B2_PATCH_H
#define BACKEND_B2_PATCH_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Caller-owned restore handle. live marks a completed patch; old_protect is
 * restored after patching and unpatching. Fields are public for inline storage. */
#define B2_PATCH_MAX_BYTES 64
typedef struct sh_patch_handle {
    void     *target;                    /* the patched address */
    uint8_t   orig[B2_PATCH_MAX_BYTES];  /* the original bytes (for restore) */
    size_t    len;                       /* how many bytes were saved/overwritten */
    uint32_t  old_protect;               /* the page protection before we touched it */
    int       live;                      /* 1 = patched (restorable), 0 = not / already reverted */
    int       atomic_rel32;              /* E8 operand changed with one aligned interlocked store */
} sh_patch_handle;


typedef enum sh_patch_status {
    B2_PATCH_OK = 0,
    B2_PATCH_REFUSED_BADARG,    /* NULL target/bytes, len 0, or len > B2_PATCH_MAX_BYTES */
    B2_PATCH_REFUSED_SIG,       /* sig-anchored entry: resolve was not a clean unique hit (not SIG_OK) */
    B2_PATCH_REFUSED_VERIFY,    /* expected bytes differ from the target */
    B2_PATCH_FAIL_PROTECT,      /* VirtualProtect failed */
    B2_PATCH_FAIL_SEH,          /* an access violation / SEH fault during read or write */
    B2_PATCH_FAIL_NOTLIVE       /* code_unpatch on a non-live handle */
} sh_patch_status;

/* Patch 1..B2_PATCH_MAX_BYTES after checking expect, if supplied. Passing NULL
 * skips that byte check and requires prior target validation by the caller.
 * out_handle is required and becomes live only on success. Return B2_PATCH_OK
 * or a refusal/failure status. A write fault can leave partial bytes with a
 * non-live handle; this path does not automatically roll back. */
sh_patch_status code_patch(void *target, const uint8_t *expect, const uint8_t *new_bytes,
                           size_t len, sh_patch_handle *out_handle);

/* Restore a live patch and clear live on success. Return B2_PATCH_OK or a failure. */
sh_patch_status code_unpatch(sh_patch_handle *handle);

/* Require SIG_OK before code_patch; hooked or unresolved sites are refused.
 * expect optionally rechecks current bytes after signature resolution. */
sh_patch_status code_patch_sig(const sig_result *r, const uint8_t *expect, const uint8_t *new_bytes,
                               size_t len, sh_patch_handle *out_handle);

/* Redirect an existing E8 call without changing its opcode. The four-byte
 * displacement MUST be aligned. Validate the complete call and publish its
 * operand with a compare-exchange, so concurrent execution cannot see a torn
 * displacement. The destination must be fully initialized before this call.
 * The caller owns destination lifetime until all users have finished. */
sh_patch_status code_patch_call_sig(const sig_result *r, const uint8_t expect[5],
                                    const uint8_t replacement[5], sh_patch_handle *handle);

/* Wrappers around hook.c; trampoline ownership and stolen-byte rules apply there. */
void *sh_install_detour(void *target, void *detour, size_t stolen);
int   sh_uninstall_detour(void *tramp);

/* Install a detour only for SIG_OK; return its trampoline or NULL and log the result. */
void *sh_install_detour_sig(const sig_result *r, void *detour, size_t stolen);

/* Test patching, execution, restoration, and mismatch refusal on scratch memory.
 * Return 1 on success, 0 on failure. No engine memory is modified. */
int sh_patch_selftest(void);

/* Human-readable name for a status (for logs). */
const char *sh_patch_status_str(sh_patch_status s);

#endif /* BACKEND_B2_PATCH_H */
