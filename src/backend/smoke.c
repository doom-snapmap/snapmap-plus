/* Check live signature resolution and exercise detours on scratch memory.
 * RVA drift is diagnostic; resolution requires uniqueness or a verified hooked
 * fallback. Handwritten scratch code guarantees a 16-byte relocatable prologue. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "smoke.h"
#include "signatures.h"
#include "hook.h"
#include "backend_log.h"

/* Scratch detour target. */
#define TAG_ORIG   1
#define TAG_DETOUR 2

/* orig(x,tag) sets *tag=1 and returns x*2. The first 16 bytes are whole,
 * position-independent instructions: mov [rdx],1; mov eax,ecx; add eax,eax;
 * two three-byte xchg rax,rax pads. Two NOPs and ret follow the stolen window. */
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

/* Distinguish an unreadable page from differing bytes in failure diagnostics. */
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

/* Compare expected bytes with live bytes at the recorded RVA for diagnostics.
 * On other builds this location need not be the function. */
#define DIAG_BYTES 16
static void log_sig_failure_diag(const uint8_t *doom_base, const sig_entry *e, sig_status st)
{
    char line[256], exp_hex[64] = {0}, got_hex[64] = {0};


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


    uint8_t got[DIAG_BYTES];
    int readable = safe_read(doom_base + e->known_rva, got, en > 0 ? en : DIAG_BYTES);
    if (readable) hexdump(got, en > 0 ? en : DIAG_BYTES, got_hex, sizeof got_hex);

    _snprintf_s(line, sizeof line, _TRUNCATE,
        "PB0: sig FAIL %s status=%s @known_rva=0x%x exp=[%s] got=[%s]",
        e->name, sig_status_str(st), e->known_rva, exp_hex,
        readable ? got_hex : "<unreadable page>");
    backend_log(line);
}

/* Bootstrap resolution probe. */
size_t sh_resolve_count(const uint8_t *doom_base)
{
    sig_result results[SIG_RESULTS_MAX];
    return sig_resolve_all(doom_base, results, SIG_RESULTS_MAX);
}

/* Full smoke check. */
int sh_smoke_run(const uint8_t *doom_base, unsigned long deferred_ms)
{
    char line[256];


    size_t total = sig_db_count();
    sig_result results[SIG_RESULTS_MAX];
    /* Refuse an undersized results array instead of silently omitting signatures. */
    if (total > SIG_RESULTS_MAX) {
        char over[160];
        _snprintf_s(over, sizeof over, _TRUNCATE,
            "PB0: SIGNATURE DB OVERFLOW -- %zu entries but only %d result slots; raise "
            "SIG_RESULTS_MAX. The last %zu signature(s) were NOT resolved.",
            total, SIG_RESULTS_MAX, total - (size_t)SIG_RESULTS_MAX);
        backend_log(over);
        total = SIG_RESULTS_MAX;
    }
    size_t ok = sig_resolve_all(doom_base, results, SIG_RESULTS_MAX);

    int rva_match = 0, rva_diff = 0, hooked = 0;
    char hooked_names[160] = {0};   /* comma-list of the hook-tolerant sigs for the success line */
    const char *first_bad = NULL;
    sig_status  first_bad_status = SIG_OK;
    for (size_t i = 0; i < total; i++) {
        if (results[i].status == SIG_OK) {
            if (results[i].rva == BACKEND_ENGINE_SIGNATURES[i].known_rva) rva_match++;
            else rva_diff++;
        } else if (results[i].status == SIG_OK_HOOKED) {
            /* A matching tail identifies an entry whose prologue is already detoured. */
            hooked++;
            size_t l = strlen(hooked_names);
            _snprintf_s(hooked_names + l, sizeof hooked_names - l, _TRUNCATE,
                        "%s%s", l ? "/" : "", results[i].name);
        } else {
            if (!first_bad) {
                first_bad = results[i].name;
                first_bad_status = results[i].status;
            }
            /* Log bytes for every unresolved entry. */
            log_sig_failure_diag(doom_base, &BACKEND_ENGINE_SIGNATURES[i], results[i].status);
        }
    }
    int resolver_ok = (ok == total);


    typedef int (*scratch_fn)(int, volatile int *);
    int detour_ok = 0;
    int scratch_retained = 0;
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
        void *tramp = hook_prepare((void *)orig_fn, (void *)scratch_detour, SCRATCH_STOLEN);
        scratch_fn tramp_fn = (scratch_fn)tramp;
        if (!tramp || hook_commit(tramp) != B2_PATCH_OK) {
            detour_why = "install failed";
            if (tramp && !hook_unpatch(tramp)) scratch_retained = 1;
        } else {
            tag = 0;
            int patched = orig_fn(7, &tag);
            int detour_hit = (tag == TAG_DETOUR && patched == 1007);
            tag = 0;
            int via_tramp = tramp_fn(9, &tag);
            int tramp_ok = (tag == TAG_ORIG && via_tramp == 18);
            int reverted = hook_unpatch(tramp);
            if (!reverted) scratch_retained = 1;
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
    /* A pending scratch detour still owns its target and destination pages. */
    if (!scratch_retained) {
        if (orig_code) VirtualFree(orig_code, 0, MEM_RELEASE);
        if (det_code) VirtualFree(det_code, 0, MEM_RELEASE);
    }


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
        /* The remaining signatures may be encrypted, patched, or incompatible. */
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
