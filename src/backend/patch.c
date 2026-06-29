/* patch.c -- see patch.h. The reusable engine-code PATCH/DETOUR layer.
 *
 * code_patch/code_unpatch = the memcpy-to-RX primitive (port of OG FUN_180001790, evidence
 * b2-decompiles/t5-devmode-codepatch.txt): VirtualProtect RWX -> write -> FlushInstructionCache ->
 * restore protection, with a verify-before-write guard and a recorded restore-handle. The detour family
 * (sh_install_detour / sh_uninstall_detour) is a thin REUSE of hook.c's already-cloned inline-detour
 * installer (OG FUN_180001850 / FUN_180001920) -- NOT reimplemented here.
 *
 * SAFETY (the whole point): the target is resolved by SIGNATURE upstream (a unique sig match verifies the
 * expected bytes are present BY CONSTRUCTION); the *_sig entry points REFUSE unless the resolve was a
 * clean unique hit (SIG_OK); code_patch additionally re-checks the caller's `expect` against the live
 * bytes; every memory touch is SEH-guarded so any fault logs + returns a clean error, never a partial
 * write. This layer installs NO engine patches -- it only builds + self-tests itself on a scratch site.
 *
 * Clean-room: ported from our own RE (the evidence + hook.c). Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "patch.h"
#include "hook.h"
#include "backend_log.h"

const char *sh_patch_status_str(sh_patch_status s)
{
    switch (s) {
        case B2_PATCH_OK:              return "OK";
        case B2_PATCH_REFUSED_BADARG:  return "REFUSED_BADARG";
        case B2_PATCH_REFUSED_SIG:     return "REFUSED_SIG";
        case B2_PATCH_REFUSED_VERIFY:  return "REFUSED_VERIFY";
        case B2_PATCH_FAIL_PROTECT:    return "FAIL_PROTECT";
        case B2_PATCH_FAIL_SEH:        return "FAIL_SEH";
        case B2_PATCH_FAIL_NOTLIVE:    return "FAIL_NOTLIVE";
        default:                       return "?";
    }
}

/* ---- SEH-guarded memory helpers ----------------------------------------------------------------- */

/* SEH-guarded compare of `n` bytes a[] vs b[]. *match=1 if equal. Returns 1 if both ranges were
 * readable, 0 if an access violation hit during the read (then *match is meaningless). The compare is
 * inside the __try so an unreadable target page is reported as a fault, not a crash. */
static int safe_memcmp(const uint8_t *a, const uint8_t *b, size_t n, int *match)
{
    __try {
        int m = 1;
        for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) { m = 0; break; } }
        *match = m;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* SEH-guarded copy of `n` bytes src->dst. Returns 1 on success, 0 if a fault hit (partial copy is
 * possible on a fault, so callers only use this AFTER VirtualProtect succeeded -- a fault there means the
 * page genuinely can't be written and is reported as FAIL_SEH). */
static int safe_memcpy(uint8_t *dst, const uint8_t *src, size_t n)
{
    __try {
        for (size_t i = 0; i < n; i++) dst[i] = src[i];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Append up to `n` bytes of `buf` as hex into `out` (out must hold ~3*n+1). For log diagnostics. */
static void hexdump(const uint8_t *buf, size_t n, char *out, size_t outcap)
{
    size_t p = 0;
    for (size_t i = 0; i < n && p + 3 < outcap; i++)
        p += (size_t)_snprintf_s(out + p, outcap - p, _TRUNCATE, "%02X ", buf[i]);
    if (p && out[p - 1] == ' ') out[p - 1] = '\0';
    else if (!p && outcap) out[0] = '\0';
}

/* ---- code_patch / code_unpatch ----------------------------------------------------------------- */

sh_patch_status code_patch(void *target, const uint8_t *expect, const uint8_t *new_bytes,
                           size_t len, sh_patch_handle *out_handle)
{
    if (out_handle) out_handle->live = 0;
    if (!target || !new_bytes || !out_handle || len == 0 || len > B2_PATCH_MAX_BYTES)
        return B2_PATCH_REFUSED_BADARG;

    uint8_t *t = (uint8_t *)target;

    /* (1) verify-before-write: the caller-asserted `expect` bytes MUST currently be live at target.
     * A mismatch means the site shifted / is already patched / is the wrong address -> REFUSE, no write.
     * (A read fault here is also a refuse-class outcome: the page isn't what we expected.) */
    if (expect) {
        int match = 0;
        if (!safe_memcmp(t, expect, len, &match)) {
            backend_log("B2: code_patch FAIL_SEH (target unreadable during verify) -- no write");
            return B2_PATCH_FAIL_SEH;
        }
        if (!match) {
            char line[160], exp_hex[64], got_hex[64];
            uint8_t got[B2_PATCH_MAX_BYTES];
            /* re-read the live bytes for the diagnostic; if that faults, mark unreadable */
            int rd = safe_memcpy(got, t, len);
            hexdump(expect, len, exp_hex, sizeof exp_hex);
            if (rd) hexdump(got, len, got_hex, sizeof got_hex);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: code_patch REFUSED_VERIFY @%p exp=[%s] got=[%s] -- no write",
                target, exp_hex, rd ? got_hex : "<unreadable>");
            backend_log(line);
            return B2_PATCH_REFUSED_VERIFY;
        }
    }

    /* (2) record the originals BEFORE touching protection (so a restore-record exists even if the write
     * faults). Read under SEH; an unreadable target aborts with no write. */
    if (!safe_memcpy(out_handle->orig, t, len)) {
        backend_log("B2: code_patch FAIL_SEH (target unreadable during orig-record) -- no write");
        return B2_PATCH_FAIL_SEH;
    }

    /* (3) VirtualProtect RWX. */
    DWORD old = 0;
    if (!VirtualProtect(t, len, PAGE_EXECUTE_READWRITE, &old)) {
        char line[96];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: code_patch FAIL_PROTECT @%p (err %lu) -- no write", target, GetLastError());
        backend_log(line);
        return B2_PATCH_FAIL_PROTECT;
    }

    /* (4) write under SEH. On a fault, restore protection and report -- the originals are already
     * recorded so the caller could code_unpatch a partial write if it wanted, but live stays 0. */
    int wrote = safe_memcpy(t, new_bytes, len);
    if (!wrote) {
        DWORD tmp;
        VirtualProtect(t, len, old, &tmp);
        backend_log("B2: code_patch FAIL_SEH (write faulted) -- protection restored");
        return B2_PATCH_FAIL_SEH;
    }

    FlushInstructionCache(GetCurrentProcess(), t, len);

    /* (5) restore the original page protection. */
    {
        DWORD tmp;
        VirtualProtect(t, len, old, &tmp);
    }

    out_handle->target      = target;
    out_handle->len         = len;
    out_handle->old_protect = (uint32_t)old;
    out_handle->live        = 1;
    return B2_PATCH_OK;
}

sh_patch_status code_unpatch(sh_patch_handle *handle)
{
    if (!handle || !handle->live) return B2_PATCH_FAIL_NOTLIVE;
    if (!handle->target || handle->len == 0 || handle->len > B2_PATCH_MAX_BYTES)
        return B2_PATCH_REFUSED_BADARG;

    uint8_t *t = (uint8_t *)handle->target;

    DWORD old = 0;
    if (!VirtualProtect(t, handle->len, PAGE_EXECUTE_READWRITE, &old)) {
        backend_log("B2: code_unpatch FAIL_PROTECT -- original NOT restored");
        return B2_PATCH_FAIL_PROTECT;
    }

    int restored = safe_memcpy(t, handle->orig, handle->len);
    FlushInstructionCache(GetCurrentProcess(), t, handle->len);

    /* restore the saved page protection (the one in force before the patch). */
    {
        DWORD tmp;
        VirtualProtect(t, handle->len, handle->old_protect, &tmp);
    }

    if (!restored) {
        backend_log("B2: code_unpatch FAIL_SEH (restore write faulted)");
        return B2_PATCH_FAIL_SEH;
    }
    handle->live = 0;
    return B2_PATCH_OK;
}

/* ---- sig-anchored code_patch (the verify-before-write entry point) ------------------------------ */

sh_patch_status code_patch_sig(const sig_result *r, const uint8_t *expect, const uint8_t *new_bytes,
                               size_t len, sh_patch_handle *out_handle)
{
    if (out_handle) out_handle->live = 0;
    if (!r || r->addr == 0) {
        backend_log("B2: code_patch_sig REFUSED_SIG (target not resolved) -- no write");
        return B2_PATCH_REFUSED_SIG;
    }
    /* The verify-before-write gate: ONLY a clean unique scan hit. SIG_OK_HOOKED (the live prologue is
     * already a detour) / anything else => the expected original bytes are NOT guaranteed present => REFUSE.
     * A unique SIG_OK match BY CONSTRUCTION proves the fixed signature bytes are at r->addr. */
    if (r->status != SIG_OK) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: code_patch_sig REFUSED_SIG %s status=%d (not a clean unique hit) -- no write",
            r->name ? r->name : "?", (int)r->status);
        backend_log(line);
        return B2_PATCH_REFUSED_SIG;
    }
    return code_patch((void *)r->addr, expect, new_bytes, len, out_handle);
}

/* ---- detour family: thin REUSE of hook.c (NOT reimplemented) ------------------------------------ */

void *sh_install_detour(void *target, void *detour, size_t stolen)
{
    return install_inline_hook(target, detour, stolen);
}

int sh_uninstall_detour(void *tramp)
{
    return hook_unpatch(tramp);
}

void *sh_install_detour_sig(const sig_result *r, void *detour, size_t stolen)
{
    if (!r || r->addr == 0) {
        backend_log("B2: install_detour_sig REFUSED_SIG (target not resolved)");
        return NULL;
    }
    if (r->status != SIG_OK) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: install_detour_sig REFUSED_SIG %s status=%d (not a clean unique hit)",
            r->name ? r->name : "?", (int)r->status);
        backend_log(line);
        return NULL;
    }
    void *tramp = sh_install_detour((void *)r->addr, detour, stolen);
    if (!tramp) {
        char line[128];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: install_detour_sig %s FAILED (installer returned NULL)", r->name ? r->name : "?");
        backend_log(line);
    }
    return tramp;
}

/* ---- in-DLL self-test (scratch site only; NO engine side effects) ------------------------------- */

/* The scratch target is a small RX stub with a KNOWN byte pattern -- a position-independent function we
 * can call through to confirm a patch took effect.
 *
 * int scratch(void): return 0x11;
 *   B8 11 00 00 00   mov eax, 0x11      (5)
 *   C3               ret                (1)
 * We patch the imm32 (offset 1..4) from 0x11 to 0x22 so a call-through returns the NEW value -- proof the
 * patch executes -- then unpatch and confirm it returns 0x11 again. Padding NOPs round the stub out so a
 * patch window never runs past the meaningful bytes. */
static const uint8_t SCRATCH_CODE[] = {
    0xB8, 0x11, 0x00, 0x00, 0x00,   /* mov eax, 0x11 */
    0xC3,                           /* ret */
    0x90, 0x90                      /* pad */
};
#define SCRATCH_PATCH_OFF 1   /* the imm32 starts at byte 1 */

int sh_patch_selftest(void)
{
    typedef int (*scratch_fn)(void);
    char line[224];
    char whybuf[160];             /* detailed FAIL detail (kept SEPARATE from `line` to avoid aliasing) */
    const char *why = "not run";

    uint8_t *stub = (uint8_t *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
    if (!stub) {
        backend_log("B2: patch-layer self-test FAIL (scratch alloc failed)");
        return 0;
    }
    memcpy(stub, SCRATCH_CODE, sizeof SCRATCH_CODE);
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    scratch_fn fn = (scratch_fn)stub;

    int ok = 0;

    /* baseline: the known pattern returns 0x11. */
    if (fn() != 0x11) {
        why = "baseline scratch wrong";
        goto done;
    }

    /* (1) POSITIVE: code_patch the imm32 0x11 -> 0x22 with a MATCHING expect. */
    {
        const uint8_t expect[4]   = { 0x11, 0x00, 0x00, 0x00 };
        const uint8_t newbytes[4] = { 0x22, 0x00, 0x00, 0x00 };
        sh_patch_handle h;
        sh_patch_status st = code_patch(stub + SCRATCH_PATCH_OFF, expect, newbytes, 4, &h);
        if (st != B2_PATCH_OK || !h.live) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE,
                "apply returned %s (expected OK)", sh_patch_status_str(st));
            why = whybuf;
            goto done;
        }
        /* call through -> the patched value must execute. */
        if (fn() != 0x22) { why = "patch did not take (call-through != 0x22)"; goto done; }
        /* read back -> the bytes must be the new pattern. */
        if (stub[SCRATCH_PATCH_OFF] != 0x22) { why = "patch readback wrong"; goto done; }

        /* (1b) code_unpatch -> the original must be restored. */
        sh_patch_status us = code_unpatch(&h);
        if (us != B2_PATCH_OK || h.live) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE,
                "restore returned %s (expected OK)", sh_patch_status_str(us));
            why = whybuf;
            goto done;
        }
        if (fn() != 0x11) { why = "restore did not take (call-through != 0x11)"; goto done; }
        if (stub[SCRATCH_PATCH_OFF] != 0x11) { why = "restore readback wrong"; goto done; }
    }

    /* (2) NEGATIVE: code_patch with an expect that does NOT match the (now-restored) scratch bytes.
     * The verify-before-write guard must REFUSE -- no write, status REFUSED_VERIFY, bytes untouched. */
    {
        const uint8_t wrong_expect[4] = { 0xAA, 0xBB, 0xCC, 0xDD };   /* not what's live (0x11 00 00 00) */
        const uint8_t newbytes[4]     = { 0x33, 0x00, 0x00, 0x00 };
        sh_patch_handle h;
        sh_patch_status st = code_patch(stub + SCRATCH_PATCH_OFF, wrong_expect, newbytes, 4, &h);
        if (st != B2_PATCH_REFUSED_VERIFY) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE,
                "negative test got %s (expected REFUSED_VERIFY)", sh_patch_status_str(st));
            why = whybuf;
            goto done;
        }
        if (h.live) { why = "negative test produced a live handle"; goto done; }
        /* prove NO write happened: bytes + call-through still the original. */
        if (stub[SCRATCH_PATCH_OFF] != 0x11) { why = "negative test wrote bytes (should refuse)"; goto done; }
        if (fn() != 0x11) { why = "negative test altered behavior (should refuse)"; goto done; }
    }

    ok = 1;

done:
    VirtualFree(stub, 0, MEM_RELEASE);
    if (ok) {
        backend_log("B2: patch-layer self-test PASS (apply/restore ok; refuse-on-mismatch ok)");
    } else {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: patch-layer self-test FAIL (%s)", why);
        backend_log(line);
    }
    return ok;
}
