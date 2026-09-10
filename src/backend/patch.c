/* Guarded code patches with restore handles, plus wrappers for hook.c detours.
 * Signature wrappers require a clean match; optional expected bytes are checked
 * before writing. Failed writes roll back; failed rollback keeps its handle. */
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
        case B2_PATCH_FAIL_ROLLBACK:   return "FAIL_ROLLBACK";
        default:                       return "?";
    }
}

/* Guarded memory access. */

/* Return 1 if both ranges were readable, with their equality in match. */
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

/* Return 0 on a memory fault; a partial copy may already have occurred. */
static int safe_memcpy(uint8_t *dst, const uint8_t *src, size_t n)
{
    __try {
        for (size_t i = 0; i < n; i++) dst[i] = src[i];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

#ifdef SH_PATCH_TESTING
static uint64_t g_fail_protect, g_fail_write;
static unsigned g_protect_calls, g_write_calls;
static size_t g_write_prefix;
static void (*g_write_observer)(void *target);
void sh_patch_test_observe_write(void (*observer)(void *target)) { g_write_observer = observer; }
void sh_patch_test_faults(uint64_t protect_calls, uint64_t write_calls, size_t write_prefix)
{
    g_fail_protect = protect_calls; g_fail_write = write_calls;
    g_protect_calls = g_write_calls = 0; g_write_prefix = write_prefix;
}
#endif

static int patch_protect(void *target, size_t size, DWORD protection, DWORD *old)
{
#ifdef SH_PATCH_TESTING
    unsigned call = g_protect_calls++;
    if (call < 64 && (g_fail_protect & (UINT64_C(1) << call))) return 0;
#endif
    return VirtualProtect(target, size, protection, old) != 0;
}

static int patch_write(uint8_t *target, const uint8_t *bytes, size_t size)
{
#ifdef SH_PATCH_TESTING
    if (g_write_observer) g_write_observer(target);
    unsigned call = g_write_calls++;
    if (call < 64 && (g_fail_write & (UINT64_C(1) << call))) {
        safe_memcpy(target, bytes, g_write_prefix < size ? g_write_prefix : size);
        return 0;
    }
#endif
    return safe_memcpy(target, bytes, size);
}

static sh_patch_status patch_failed(sh_patch_handle *handle, sh_patch_status reason)
{
    if (code_unpatch(handle) == B2_PATCH_OK) return reason;
    backend_log("B2: patch rollback failed; restore handle retained for retry");
    return B2_PATCH_FAIL_ROLLBACK;
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

/* Byte patches and restoration. */

sh_patch_status code_patch(void *target, const uint8_t *expect, const uint8_t *new_bytes,
                           size_t len, sh_patch_handle *out_handle)
{
    if (out_handle) { out_handle->live = 0; out_handle->atomic_rel32 = 0; out_handle->protection_only = 0; }
    if (!target || !new_bytes || !out_handle || len == 0 || len > B2_PATCH_MAX_BYTES)
        return B2_PATCH_REFUSED_BADARG;

    uint8_t *t = (uint8_t *)target;

    /* Refuse before writing when expected bytes do not match. */
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

    /* Save original bytes before changing protection or writing the patch. */
    if (!safe_memcpy(out_handle->orig, t, len)) {
        backend_log("B2: code_patch FAIL_SEH (target unreadable during orig-record) -- no write");
        return B2_PATCH_FAIL_SEH;
    }


    DWORD old = 0;
    if (!patch_protect(t, len, PAGE_EXECUTE_READWRITE, &old)) {
        char line[96];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: code_patch FAIL_PROTECT @%p (err %lu) -- no write", target, GetLastError());
        backend_log(line);
        return B2_PATCH_FAIL_PROTECT;
    }

    out_handle->target      = target;
    out_handle->len         = len;
    out_handle->old_protect = (uint32_t)old;
    out_handle->live        = 1;
    if (!patch_write(t, new_bytes, len))
        return patch_failed(out_handle, B2_PATCH_FAIL_SEH);
    FlushInstructionCache(GetCurrentProcess(), t, len);
    {
        DWORD ignored;
        if (!patch_protect(t, len, old, &ignored))
            return patch_failed(out_handle, B2_PATCH_FAIL_PROTECT);
    }
    return B2_PATCH_OK;
}

sh_patch_status code_unpatch(sh_patch_handle *handle)
{
    if (!handle || !handle->live) return B2_PATCH_FAIL_NOTLIVE;
    if (!handle->target || handle->len == 0 || handle->len > B2_PATCH_MAX_BYTES)
        return B2_PATCH_REFUSED_BADARG;
    if (handle->atomic_rel32 && (handle->len != 5 ||
        (((uintptr_t)handle->target + 1) & 3u))) return B2_PATCH_REFUSED_BADARG;

    uint8_t *t = (uint8_t *)handle->target;

    DWORD old = 0;
    if (!patch_protect(t, handle->len, PAGE_EXECUTE_READWRITE, &old)) {
        backend_log("B2: code_unpatch FAIL_PROTECT -- original NOT restored");
        return B2_PATCH_FAIL_PROTECT;
    }

    int restored;
    if (handle->protection_only) restored = 1;
    else if (handle->atomic_rel32) {
        __try {
            LONG original;
            memcpy(&original, handle->orig + 1, sizeof original);
            InterlockedExchange((volatile LONG *)(t + 1), original);
            restored = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { restored = 0; }
    } else restored = patch_write(t, handle->orig, handle->len);
    FlushInstructionCache(GetCurrentProcess(), t, handle->len);


    DWORD ignored;
    int protected = patch_protect(t, handle->len, handle->old_protect, &ignored);

    if (!restored) {
        backend_log("B2: code_unpatch FAIL_SEH (restore write faulted)");
        return B2_PATCH_FAIL_SEH;
    }
    if (!protected) {
        backend_log("B2: code_unpatch FAIL_PROTECT -- original bytes restored; protection retry needed");
        return B2_PATCH_FAIL_PROTECT;
    }
    handle->live = 0;
    return B2_PATCH_OK;
}

/* Signature-gated patch operations. */

sh_patch_status code_patch_call_sig(const sig_result *r, const uint8_t expect[5],
                                    const uint8_t replacement[5], sh_patch_handle *handle)
{
    DWORD old, ignored;
    LONG before, after;
    sh_patch_status status = B2_PATCH_REFUSED_VERIFY;
    if (!handle) return B2_PATCH_REFUSED_BADARG;
    memset(handle, 0, sizeof *handle);
    if (!r || r->status != SIG_OK || !r->addr) return B2_PATCH_REFUSED_SIG;
    if (!expect || !replacement || expect[0] != 0xe8 || replacement[0] != 0xe8 ||
        ((r->addr + 1) & 3u)) return B2_PATCH_REFUSED_BADARG;
    uint8_t *target = (uint8_t *)r->addr;
    int match = 0;
    if (!safe_memcmp(target, expect, 5, &match) || !match) return B2_PATCH_REFUSED_VERIFY;
    memcpy(&before, expect + 1, sizeof before);
    memcpy(&after, replacement + 1, sizeof after);
    if (!patch_protect(target, 5, PAGE_EXECUTE_READWRITE, &old)) return B2_PATCH_FAIL_PROTECT;
    memcpy(handle->orig, expect, 5);
    handle->target = target; handle->len = 5; handle->old_protect = old;
    handle->atomic_rel32 = 1;
    handle->protection_only = 1;
    __try {
        if (target[0] == 0xe8 &&
            InterlockedCompareExchange((volatile LONG *)(target + 1), after, before) == before) {
            memcpy(handle->orig, expect, 5);
            handle->target = target; handle->len = 5; handle->old_protect = old;
            handle->live = 1; handle->atomic_rel32 = 1;
            handle->protection_only = 0;
            status = B2_PATCH_OK;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { status = B2_PATCH_FAIL_SEH; }
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    if (!patch_protect(target, 5, old, &ignored)) {
        /* Even an unchanged call owns the outstanding page-protection restore. */
        handle->live = 1;
        return patch_failed(handle, B2_PATCH_FAIL_PROTECT);
    }
    return status;
}

sh_patch_status code_patch_sig(const sig_result *r, const uint8_t *expect, const uint8_t *new_bytes,
                               size_t len, sh_patch_handle *out_handle)
{
    if (out_handle) out_handle->live = 0;
    if (!r || r->addr == 0) {
        backend_log("B2: code_patch_sig REFUSED_SIG (target not resolved) -- no write");
        return B2_PATCH_REFUSED_SIG;
    }
    /* Already-hooked results do not establish intact patch bytes. */
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

/* Inline detour wrappers. */

int sh_uninstall_detour(void *tramp)
{
    return hook_unpatch(tramp);
}

sh_patch_status sh_commit_detour(void *tramp) { return hook_commit(tramp); }
int sh_detour_is_installed(void *tramp) { return hook_is_installed(tramp); }

void *sh_prepare_detour_sig(const sig_result *r, void *detour, size_t stolen)
{
    if (!r || r->addr == 0) {
        backend_log("B2: prepare_detour_sig REFUSED_SIG (target not resolved)");
        return NULL;
    }
    if (r->status != SIG_OK) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: prepare_detour_sig REFUSED_SIG %s status=%d (not a clean unique hit)",
            r->name ? r->name : "?", (int)r->status);
        backend_log(line);
        return NULL;
    }
    void *tramp = hook_prepare((void *)r->addr, detour, stolen);
    if (!tramp) {
        char line[128];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: prepare_detour_sig %s FAILED", r->name ? r->name : "?");
        backend_log(line);
    }
    return tramp;
}

/* Scratch-memory patch checks. */

/* This stub returns 0x11. Change its immediate to 0x22 and restore it, checking
 * calls as well as bytes. Handwritten code keeps the patch window deterministic. */
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
    char whybuf[160];             /* Keep diagnostic scratch separate from the destination log buffer. */
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


    if (fn() != 0x11) {
        why = "baseline scratch wrong";
        goto done;
    }

    /* Matching expected bytes permit the patch. */
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

        if (fn() != 0x22) { why = "patch did not take (call-through != 0x22)"; goto done; }

        if (stub[SCRATCH_PATCH_OFF] != 0x22) { why = "patch readback wrong"; goto done; }


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

    /* Mismatched expected bytes must refuse the patch without modifying the stub. */
    {
        const uint8_t wrong_expect[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
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
