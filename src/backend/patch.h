/* patch.h -- the SnapHak backend's reusable engine-code PATCH/DETOUR layer.
 *
 * This is the SAFETY layer the upcoming engine-code-patch consumers ride on:
 *   [15][16] devmode  -- patch a hand-assembled code region at engine RVA 0x18a31d0 (memcpy-to-RX).
 *   [11] render-logging + [18] cs_dontuse -- install jmp-detours over engine code.
 * (Both clusters are later work; this builds + self-tests the LAYER only -- NO engine patches.)
 *
 * It mirrors OG SnapHak's two self-modifying-code primitives (evidence b2-decompiles/
 * t5-devmode-codepatch.txt; OG base 0x180000000):
 *   FUN_180001790(dst,len,src)  = memcpy-to-RX   (VirtualProtect RWX -> copy -> restore)   == code_patch
 *   FUN_180001850(hookFn,rva)   = inline-detour installer  (jmp at engineBase+rva -> hook) == install_detour
 *   FUN_180001920(tramp)        = uninstall                                                == uninstall_detour
 * The detour family is ALREADY cloned in hook.c (install_inline_hook / hook_unpatch); this layer REUSES
 * it (sh_install_detour is a thin pass-through, NOT a reimplementation) and adds code_patch/code_unpatch.
 *
 * THE WHOLE POINT IS SAFETY -- a stale/shifted target = engine corruption. So:
 *  - The caller resolves the patch TARGET by SIGNATURE (signatures.h resolver), NOT a raw RVA. A unique
 *    sig match BY CONSTRUCTION verifies the expected original bytes are present at the site. The two
 *    sig-anchored entry points (code_patch_sig / install_detour_sig) take a resolved sig_result and
 *    REFUSE (no write) unless the resolve was a clean unique hit (SIG_OK). This is the verify-before-write.
 *  - code_patch additionally re-checks `expect` (the bytes the caller asserts are currently there) against
 *    the live target before writing -- a second, byte-exact verify. Mismatch => REFUSE, no write, logged.
 *  - Every op records the original bytes (a restore-handle) so the patch is reversible.
 *  - Every memory touch is SEH-guarded; any fault logs + returns a clean error, never a partial write.
 *
 * Clean-room: ported from our own RE (the evidence above + hook.c). Zero OG SnapHak bytes.
 */
#ifndef BACKEND_B2_PATCH_H
#define BACKEND_B2_PATCH_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"   /* sig_result -- the sig-anchored verify-before-write input */

/* Restore-handle for a code_patch. Opaque to consumers except via code_unpatch; fields exposed so the
 * caller can keep it inline (no allocation). `live`==0 means a free/reverted handle. `old_protect` is
 * the page protection in force BEFORE the patch (restored after the write + on un-patch). */
#define B2_PATCH_MAX_BYTES 64
typedef struct sh_patch_handle {
    void     *target;                    /* the patched address */
    uint8_t   orig[B2_PATCH_MAX_BYTES];  /* the original bytes (for restore) */
    size_t    len;                       /* how many bytes were saved/overwritten */
    uint32_t  old_protect;               /* the page protection before we touched it */
    int       live;                      /* 1 = patched (restorable), 0 = not / already reverted */
} sh_patch_handle;

/* Status codes for the patch ops. */
typedef enum sh_patch_status {
    B2_PATCH_OK = 0,
    B2_PATCH_REFUSED_BADARG,    /* NULL target/bytes, len 0, or len > B2_PATCH_MAX_BYTES */
    B2_PATCH_REFUSED_SIG,       /* sig-anchored entry: resolve was not a clean unique hit (not SIG_OK) */
    B2_PATCH_REFUSED_VERIFY,    /* `expect` did not match the live target bytes -- THE verify-before-write */
    B2_PATCH_FAIL_PROTECT,      /* VirtualProtect failed */
    B2_PATCH_FAIL_SEH,          /* an access violation / SEH fault during read or write */
    B2_PATCH_FAIL_NOTLIVE       /* code_unpatch on a non-live handle */
} sh_patch_status;

/* ---- code_patch: the memcpy-to-RX primitive (OG FUN_180001790), verify-before-write ----------------
 *
 * Overwrite `len` bytes at `target` with `new_bytes`, having FIRST verified `expect` (the bytes the
 * caller asserts are currently live there, `len` of them) match the target byte-for-byte. On a match:
 * record the originals into *out_handle, VirtualProtect RWX, write, FlushInstructionCache, restore the
 * original page protection. On a mismatch (or any bad arg / fault): NO bytes are written and a specific
 * status is returned. SEH-guarded throughout.
 *
 *   target      -- the address to patch (caller resolved it by SIGNATURE; see *_sig wrappers).
 *   expect      -- the `len` bytes that MUST currently be at target (the verify-before-write). May be
 *                  NULL to skip the byte-exact check (sig-anchored callers already verified by sig
 *                  construction; pass NULL only when the sig IS the verification).
 *   new_bytes   -- the `len` replacement bytes.
 *   len         -- byte count (1..B2_PATCH_MAX_BYTES).
 *   out_handle  -- receives the restore-record (required). On any non-OK return, *out_handle.live == 0.
 *
 * Returns B2_PATCH_OK on a successful write, else a B2_PATCH_REFUSED_x / FAIL_x code. */
sh_patch_status code_patch(void *target, const uint8_t *expect, const uint8_t *new_bytes,
                           size_t len, sh_patch_handle *out_handle);

/* Reverse a code_patch: restore the saved original bytes (VirtualProtect RWX -> copy orig -> restore
 * protection -> flush icache). Marks the handle not-live. SEH-guarded. Returns B2_PATCH_OK or a FAIL_*. */
sh_patch_status code_unpatch(sh_patch_handle *handle);

/* ---- sig-anchored code_patch: the verify-before-write entry point ----------------------------------
 *
 * The SAFE way the engine-patch consumers will call code_patch: pass the sig_result the
 * resolver produced for the patch site. REFUSES (B2_PATCH_REFUSED_SIG, no write) unless `r->status ==
 * SIG_OK` -- a clean UNIQUE scan hit, which by construction means the expected original bytes are present
 * at r->addr (SIG_OK_HOOKED/anything else => the live prologue is a detour or absent => never patch).
 * On a clean hit it patches at r->addr (+ optional `expect` byte-recheck, may be NULL). */
sh_patch_status code_patch_sig(const sig_result *r, const uint8_t *expect, const uint8_t *new_bytes,
                               size_t len, sh_patch_handle *out_handle);

/* ---- detour family: REUSE hook.c's inline-detour installer (NOT reimplemented) ---------------------
 *
 * These are thin pass-throughs to hook.c so the engine-patch consumers have ONE patch-layer header to
 * include. install_detour records the original prologue in hook.c's reversible un-patch list and returns
 * a trampoline (call it to invoke the original); uninstall_detour reverses one. */
void *sh_install_detour(void *target, void *detour, size_t stolen);
int   sh_uninstall_detour(void *tramp);

/* Sig-anchored detour install: the verify-before-write form. REFUSES (returns NULL) unless
 * `r->status == SIG_OK` (clean unique scan hit => the expected prologue bytes are present). On a clean
 * hit, installs the detour at r->addr via sh_install_detour. Logs the refuse/installed reason. */
void *sh_install_detour_sig(const sig_result *r, void *detour, size_t stolen);

/* ---- in-DLL self-test (run at install, like sh_smoke; NO engine side effects) ---------------------
 *
 * Patches a SCRATCH RX site only:
 *  (1) alloc an RX stub with a known byte pattern; code_patch it (with a matching `expect`) to a
 *      different known pattern; call through / read back -> confirm the patch took; code_unpatch ->
 *      confirm restored.
 *  (2) NEGATIVE: code_patch the scratch with an `expect` that does NOT match -> confirm REFUSED, no write.
 * Emits "B2: patch-layer self-test PASS (apply/restore ok; refuse-on-mismatch ok)" or a specific FAIL.
 * Returns 1 on PASS, 0 on FAIL. Safe to call from the bootstrap thread; touches no engine memory. */
int sh_patch_selftest(void);

/* Human-readable name for a status (for logs). */
const char *sh_patch_status_str(sh_patch_status s);

#endif /* BACKEND_B2_PATCH_H */
