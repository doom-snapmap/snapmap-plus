/* smoke.c -- see smoke.h. The backend's end-to-end foundation proof.
 *
 * Two proofs, one line:
 *
 *  (A) RESOLVER: sig_resolve_all over the live DOOM module. Every signature in the DB must resolve
 *      UNIQUELY (the whole point -- a sig that matches 0 or >1 times can't identify a function). We
 *      also cross-check each resolved RVA against known_rva: on the PINNED build they match (proves
 *      the scanner re-finds the SAME function the sig was extracted from); on a shifted build they
 *      differ and that is EXPECTED (the resolver earning its keep) -- reported, not failed. The
 *      pass/fail bar is uniqueness, not RVA equality.
 *
 *  (B) INSTALLER: a scratch self-test (no engine side effects). We detour a hand-laid scratch stub to
 *      a hand-laid detour stub, call it through the patched entry, confirm the detour ran, call the
 *      trampoline to confirm the ORIGINAL bytes still execute, then hook_unpatch and confirm the
 *      original runs un-detoured again. This exercises the exact VirtualProtect->patch->trampoline->
 *      un-patch path the real engine detours will use, with zero risk to the live game.
 *
 * The scratch target is HAND-LAID machine code (not a C function) so the optimizer can't shrink it
 * below the 16-byte stolen window or emit a RIP-relative prologue (that combination -- a tiny /O2 fn
 * whose 6-byte body was RIP-relative AND < the steal window -- corrupted memory in an earlier draft).
 * Its first 16 bytes are whole, register-only, position-independent instructions.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "smoke.h"
#include "signatures.h"
#include "hook.h"
#include "backend_log.h"

/* ---- (B) scratch self-test target (hand-laid PI machine code) ---------------------------------- */
#define TAG_ORIG   1
#define TAG_DETOUR 2

/* int orig(int x_ecx, int* tag_rdx): *tag = 1; return x*2;  (16-byte PI prologue + pad + ret)
 *   C7 02 01 00 00 00     mov  dword [rdx], 1        ; *tag = TAG_ORIG               (6)
 *   8B C1                 mov  eax, ecx              ; eax = x                        (2)
 *   03 C0                 add  eax, eax              ; eax = x*2                      (2)
 *   48 87 C0              xchg rax, rax              ; PI no-op pad (REX, 3B)         (3)
 *   48 87 C0              xchg rax, rax              ; PI no-op pad                   (3)  ->16 bytes
 *   90 90 C3              nop; nop; ret                                              (3) */
static const uint8_t SCRATCH_ORIG_CODE[] = {
    0xC7, 0x02, 0x01, 0x00, 0x00, 0x00,
    0x8B, 0xC1,
    0x03, 0xC0,
    0x48, 0x87, 0xC0,
    0x48, 0x87, 0xC0,
    0x90, 0x90, 0xC3
};
/* int detour(int x_ecx, int* tag_rdx): *tag = 2; return x+1000; */
static const uint8_t SCRATCH_DETOUR_CODE[] = {
    0xC7, 0x02, 0x02, 0x00, 0x00, 0x00,   /* mov dword [rdx], 2 (TAG_DETOUR) */
    0x8B, 0xC1,                           /* mov eax, ecx */
    0x05, 0xE8, 0x03, 0x00, 0x00,         /* add eax, 1000 */
    0xC3                                  /* ret */
};
#define SCRATCH_STOLEN 16

static const char *sig_status_str(sig_status s)
{
    switch (s) {
        case SIG_OK:          return "OK";
        case SIG_NOT_FOUND:   return "NOT_FOUND";
        case SIG_AMBIGUOUS:   return "AMBIGUOUS";
        case SIG_BAD_PATTERN: return "BAD_PATTERN";
        case SIG_BAD_MODULE:  return "BAD_MODULE";
        default:              return "?";
    }
}

/* SEH-guarded peek of `n` bytes from `src` into `dst`. Returns 1 if all bytes read, 0 if an access
 * violation hit (e.g. an uncommitted .text page) -- used by the failure diagnostic to distinguish a
 * runtime-patched prologue (bytes differ) from an unreadable page (read AV). */
static int safe_read(const uint8_t *src, uint8_t *dst, int n)
{
    __try {
        for (int i = 0; i < n; i++) dst[i] = src[i];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Append up to `n` bytes of `buf` as hex into `out` (out must hold 3*n+1). */
static void hexdump(const uint8_t *buf, int n, char *out, size_t outcap)
{
    size_t p = 0;
    for (int i = 0; i < n && p + 3 < outcap; i++)
        p += (size_t)_snprintf_s(out + p, outcap - p, _TRUNCATE, "%02X ", buf[i]);
    if (p && out[p - 1] == ' ') out[p - 1] = '\0';
}

/* For a FAILING sig, log a diagnostic: the EXPECTED first fixed bytes (from the DB pattern, which the
 * extractor took from the on-disk unpacked exe at known_rva) vs the ACTUAL live bytes at
 * doom_base+known_rva. This is the DIRECT evidence that tells whether the live prologue is patched,
 * shifted, or on an unreadable page -- without needing a Ghidra/manual live read. */
#define DIAG_BYTES 16
static void log_sig_failure_diag(const uint8_t *doom_base, const sig_entry *e, sig_status st)
{
    char line[256], exp_hex[64] = {0}, got_hex[64] = {0};

    /* Expected: the leading fixed (non-wildcard) bytes of the pattern, parsed straight from the string. */
    uint8_t exp[DIAG_BYTES];
    int en = 0;
    const char *p = e->pattern;
    while (*p && en < DIAG_BYTES) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (p[0] == '?') { exp[en++] = 0; /* wildcard -> shown as 00 below, marked */ }
        else {
            int hi = (p[0] <= '9') ? p[0] - '0' : (p[0] | 0x20) - 'a' + 10;
            int lo = (p[1] <= '9') ? p[1] - '0' : (p[1] | 0x20) - 'a' + 10;
            exp[en++] = (uint8_t)((hi << 4) | lo);
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    hexdump(exp, en, exp_hex, sizeof exp_hex);

    /* Actual: live bytes at the OFFLINE known_rva (where the fn sits in the unpacked exe). */
    uint8_t got[DIAG_BYTES];
    int readable = safe_read(doom_base + e->known_rva, got, en > 0 ? en : DIAG_BYTES);
    if (readable) hexdump(got, en > 0 ? en : DIAG_BYTES, got_hex, sizeof got_hex);

    _snprintf_s(line, sizeof line, _TRUNCATE,
        "PB0: sig FAIL %s status=%s @known_rva=0x%x exp=[%s] got=[%s]",
        e->name, sig_status_str(st), e->known_rva, exp_hex,
        readable ? got_hex : "<unreadable page>");
    backend_log(line);
}

/* ---- lightweight resolve pass (the bootstrap poll uses this) ----------------------------------- */
size_t sh_resolve_count(const uint8_t *doom_base)
{
    sig_result results[64];
    return sig_resolve_all(doom_base, results, 64);
}

/* ---- the proof --------------------------------------------------------------------------------- */
int sh_smoke_run(const uint8_t *doom_base, unsigned long deferred_ms)
{
    char line[256];

    /* (A) resolver -------------------------------------------------------------------------------- */
    size_t total = sig_db_count();
    sig_result results[64];
    if (total > 64) total = 64;
    size_t ok = sig_resolve_all(doom_base, results, 64);

    int rva_match = 0, rva_diff = 0, hooked = 0;
    char hooked_names[160] = {0};   /* comma-list of the hook-tolerant sigs for the success line */
    const char *first_bad = NULL;
    sig_status  first_bad_status = SIG_OK;
    for (size_t i = 0; i < total; i++) {
        if (results[i].status == SIG_OK) {
            if (results[i].rva == BACKEND_ENGINE_SIGNATURES[i].known_rva) rva_match++;
            else rva_diff++;
        } else if (results[i].status == SIG_OK_HOOKED) {
            /* Present-but-inline-hooked (the scan missed the overwritten prologue; the known_rva
             * fallback confirmed it via the matching fixed tail). Resolved + callable via trampoline. */
            hooked++;
            size_t l = strlen(hooked_names);
            _snprintf_s(hooked_names + l, sizeof hooked_names - l, _TRUNCATE,
                        "%s%s", l ? "/" : "", results[i].name);
        } else {
            if (!first_bad) {
                first_bad = results[i].name;
                first_bad_status = results[i].status;
            }
            /* Log EVERY genuinely-failing sig with a live-vs-expected byte diagnostic, so one re-smoke
             * shows the exact set AND why each fails (patched-but-tail-mismatch / shifted / unreadable). */
            log_sig_failure_diag(doom_base, &BACKEND_ENGINE_SIGNATURES[i], results[i].status);
        }
    }
    int resolver_ok = (ok == total);

    /* (B) installer self-test -------------------------------------------------------------------- */
    typedef int (*scratch_fn)(int, volatile int *);
    int detour_ok = 0;
    const char *detour_why = "not run";
    volatile int tag;

    uint8_t *orig_code = (uint8_t *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    uint8_t *det_code  = (uint8_t *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    scratch_fn orig_fn = NULL, scratch_detour = NULL;
    if (orig_code && det_code) {
        memcpy(orig_code, SCRATCH_ORIG_CODE, sizeof SCRATCH_ORIG_CODE);
        memcpy(det_code,  SCRATCH_DETOUR_CODE, sizeof SCRATCH_DETOUR_CODE);
        FlushInstructionCache(GetCurrentProcess(), orig_code, 64);
        FlushInstructionCache(GetCurrentProcess(), det_code, 64);
        orig_fn = (scratch_fn)orig_code;
        scratch_detour = (scratch_fn)det_code;
    }

    tag = 0;
    int base = orig_fn ? orig_fn(21, &tag) : -1;
    if (!orig_fn) {
        detour_why = "stub alloc failed";
    } else if (base != 42 || tag != TAG_ORIG) {
        detour_why = "baseline scratch wrong";
    } else {
        int before = hook_installed_count();
        void *tramp = install_inline_hook((void *)orig_fn, (void *)scratch_detour, SCRATCH_STOLEN);
        if (!tramp || hook_installed_count() != before + 1) {
            detour_why = "install failed";
        } else {
            tag = 0;
            int patched = orig_fn(7, &tag);
            int detour_hit = (tag == TAG_DETOUR && patched == 1007);
            tag = 0;
            scratch_fn tramp_fn = (scratch_fn)tramp;
            int via_tramp = tramp_fn(9, &tag);
            int tramp_ok = (tag == TAG_ORIG && via_tramp == 18);
            int reverted = hook_unpatch(tramp);
            tag = 0;
            int after = orig_fn(5, &tag);
            int revert_ok = (reverted && hook_installed_count() == before &&
                             tag == TAG_ORIG && after == 10);
            if (!detour_hit)       detour_why = "detour not taken";
            else if (!tramp_ok)    detour_why = "trampoline wrong";
            else if (!revert_ok)   detour_why = "un-patch failed";
            else { detour_ok = 1;  detour_why = "OK"; }
        }
    }
    if (orig_code) VirtualFree(orig_code, 0, MEM_RELEASE);
    if (det_code)  VirtualFree(det_code, 0, MEM_RELEASE);

    /* ---- emit the single foundation-proof line -------------------------------------------------------------- */
    if (resolver_ok && detour_ok) {
        if (hooked > 0) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "PB0: resolved %zu/%zu sigs (deferred %lums; rva-pinned %d, hook-tolerant %d: %s); "
                "test detour OK",
                ok, total, deferred_ms, rva_match, hooked, hooked_names);
        } else {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "PB0: resolved %zu/%zu sigs (deferred %lums past load; rva-pinned %d, rva-shifted %d); "
                "test detour OK",
                ok, total, deferred_ms, rva_match, rva_diff);
        }
    } else if (!resolver_ok) {
        /* After the bootstrap poll, a still-incomplete resolve means the SteamStub decrypt never landed
         * within the budget (or the build shifted so a sig no longer matches). */
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "PB0: FAIL resolver still %zu/%zu sigs after %lums (SteamStub not decrypted?; "
            "first bad: %s status=%d); detour %s",
            ok, total, deferred_ms, first_bad ? first_bad : "?", (int)first_bad_status, detour_why);
    } else {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "PB0: resolved %zu/%zu sigs (deferred %lums past load); FAIL test detour (%s)",
            ok, total, deferred_ms, detour_why);
    }
    backend_log(line);

    return (resolver_ok && detour_ok) ? 1 : 0;
}
