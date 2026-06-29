/* signatures.h -- version-portable masked-byte engine-function resolver for the SnapHak BACKEND
 * (our clean-room XINPUT1_3.dll). C port of the prototype's signature table.
 *
 * SnapHak hardcodes `engineBase + rva` (build-locked to its 2021 DOOM). We instead carry a masked
 * byte SIGNATURE per engine function and scan the live DOOM module's executable bytes for it, so the
 * backend ports to an RVA-shifted DOOM with no Ghidra and no hardcoded RVAs (the build-mismatch
 * lesson, memory `reference-entity-layout-offsets-build-specific`). A signature pins exactly one
 * location or it is rejected -- a resolve that is not UNIQUE is a failure, never a guess.
 *
 * This runs against the ALREADY-MAPPED DOOM module in our own address space (GetModuleHandle gives the
 * runtime base; the PE headers + section table are present in-memory), not a file on disk -- so the
 * matcher reads each section's mapped virtual bytes directly, no RVA<->file-offset translation.
 *
 * (Self-contained: no dependency on the fault-shield DLL. The fault-shield is a SEPARATE proxy with a
 *  DIFFERENT signature; this resolver lives in and ships with the backend only.)
 */
#ifndef BACKEND_SIGNATURES_H
#define BACKEND_SIGNATURES_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* One masked byte signature for one engine function. `pattern` is an IDA-style hex string with `??`
 * (or `?`) wildcards for the volatile operand bytes; `known_rva` is the RVA the signature was
 * extracted at on the pinned build (validation / documentation only -- never used to locate). */
typedef struct sig_entry {
    const char *name;
    const char *pattern;
    uint32_t    known_rva;   /* the RVA on the extraction build (0 = none) */
} sig_entry;

typedef enum sig_status {
    SIG_OK = 0,
    SIG_NOT_FOUND,      /* zero matches in the executable sections */
    SIG_AMBIGUOUS,      /* more than one match -- not unique enough to identify a function */
    SIG_BAD_PATTERN,    /* empty / malformed pattern, or pattern longer than the scan buffer */
    SIG_BAD_MODULE,     /* the module base is not a parseable PE32+ image */
    SIG_OK_HOOKED       /* scan missed (prologue inline-hooked) but the known_rva fallback found the fn:
                         * an E9/EB/FF-25/FF-/4 detour at known_rva + the sig's fixed TAIL still matches.
                         * Resolved at known_rva (build-specific fallback). Calling it is transparent --
                         * the detour trampolines to the original. See sig_resolve_one. */
} sig_status;

typedef struct sig_result {
    const char *name;
    sig_status  status;
    uintptr_t   addr;   /* module_base + rva, or 0 */
    uint32_t    rva;    /* recovered RVA, or 0 */
} sig_result;

/* The shipped engine signature database (port of signatures.py ENGINE_SIGNATURES). NULL-terminated
 * (the final entry has name==NULL). */
extern const sig_entry BACKEND_ENGINE_SIGNATURES[];

/* Count of real entries in BACKEND_ENGINE_SIGNATURES (excluding the NULL terminator). */
size_t sig_db_count(void);

/* Resolve one signature over `module_base`'s executable sections. Returns the status; fills *out. */
sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out);

/* Resolve the whole DB into the caller's `results` array (cap >= sig_db_count()). Returns the number
 * that resolved (status==SIG_OK from the scan, OR status==SIG_OK_HOOKED from the known_rva hook-tolerant
 * fallback -- both are present + callable). */
size_t sig_resolve_all(const uint8_t *module_base, sig_result *results, size_t cap);

/* Look up a single resolved address by name from a results array (returns 0 if not OK / not found). */
uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name);

#endif /* BACKEND_SIGNATURES_H */
