/* Code patches with saved originals, signature gates, and inline-detour wrappers.
 * Every write and restore runs with the process's other threads suspended and
 * outside the changed bytes (patch.c), so concurrent execution never sees a
 * partly written instruction. Expected-byte checks reject mismatches before
 * writing; a failed rollback keeps a retryable restore handle. */
#ifndef BACKEND_PATCH_H
#define BACKEND_PATCH_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Caller-owned restore handle. live includes an unfinished rollback; do not
 * discard it or release its destination until code_unpatch succeeds. */
#define B2_PATCH_MAX_BYTES 64
typedef struct sh_patch_handle {
    void     *target;                    /* the patched address */
    uint8_t   orig[B2_PATCH_MAX_BYTES];  /* the original bytes (for restore) */
    size_t    len;                       /* how many bytes were saved/overwritten */
    uint32_t  old_protect;               /* the page protection before we touched it */
    int       live;                      /* 1 = patched (restorable), 0 = not / already reverted */
} sh_patch_handle;


typedef enum sh_patch_status {
    B2_PATCH_OK = 0,
    B2_PATCH_REFUSED_BADARG,    /* NULL target/bytes, len 0, or len > B2_PATCH_MAX_BYTES */
    B2_PATCH_REFUSED_SIG,       /* sig-anchored entry: resolve was not a clean unique hit (not SIG_OK) */
    B2_PATCH_REFUSED_VERIFY,    /* expected bytes differ from the target */
    B2_PATCH_FAIL_PROTECT,      /* VirtualProtect failed */
    B2_PATCH_FAIL_SEH,          /* an access violation / SEH fault during read or write */
    B2_PATCH_FAIL_NOTLIVE,      /* code_unpatch on a non-live handle */
    B2_PATCH_FAIL_ROLLBACK,     /* failed apply could not fully restore; handle stays live */
    B2_PATCH_FAIL_BUSY          /* a thread stayed inside the bytes for every attempt; nothing changed */
} sh_patch_status;

/* Patch 1..B2_PATCH_MAX_BYTES after checking expect, if supplied. Passing NULL
 * skips that byte check and requires prior target validation by the caller.
 * out_handle is required and must not already own a patch. On failure, rollback
 * is attempted. FAIL_ROLLBACK retains a live handle for code_unpatch to retry. */
sh_patch_status code_patch(void *target, const uint8_t *expect, const uint8_t *new_bytes,
                           size_t len, sh_patch_handle *out_handle);

/* Restore a live patch and clear live on success. Return B2_PATCH_OK or a failure. */
sh_patch_status code_unpatch(sh_patch_handle *handle);

/* Require SIG_OK before code_patch; hooked or unresolved sites are refused.
 * expect optionally rechecks current bytes after signature resolution. */
sh_patch_status code_patch_sig(const sig_result *r, const uint8_t *expect, const uint8_t *new_bytes,
                               size_t len, sh_patch_handle *out_handle);

/* Redirect an existing E8 call: require SIG_OK and both calls to be E8 rel32,
 * then code_patch the five bytes. The destination must be fully initialized
 * before this call, and the caller owns its lifetime until all users finish. */
sh_patch_status code_patch_call_sig(const sig_result *r, const uint8_t expect[5],
                                    const uint8_t replacement[5], sh_patch_handle *handle);

/* Wrappers around hook.c; trampoline ownership and stolen-byte rules apply there. */
int   sh_uninstall_detour(void *tramp);
sh_patch_status sh_commit_detour(void *tramp);
int sh_detour_is_installed(void *tramp);

/* Prepare only for SIG_OK, without changing the target. Publish the returned
 * trampoline to the original callback before sh_commit_detour. */
void *sh_prepare_detour_sig(const sig_result *r, void *detour, size_t stolen);

/* Test patching, execution, restoration, and mismatch refusal on scratch memory.
 * Return 1 on success, 0 on failure. No engine memory is modified. */
int sh_patch_selftest(void);

/* Human-readable name for a status (for logs). */
const char *sh_patch_status_str(sh_patch_status s);

#ifdef SH_PATCH_TESTING
/* Bit N fails the Nth protection or write operation; a failed write copies prefix bytes. */
void sh_patch_test_faults(uint64_t protect_calls, uint64_t write_calls, size_t write_prefix);
/* Observe a byte write immediately before it occurs, including rollback writes. */
void sh_patch_test_observe_write(void (*observer)(void *target));
#endif

#endif /* BACKEND_PATCH_H */
