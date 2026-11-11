/* Engine function signatures for the one supported build (host_image.h).
 * Every entry records its RVA in both build 13954591 executables; at run time a
 * signature resolves only at that RVA for the host's renderer, and only when
 * its whole masked pattern holds there. sig_scan_one, the whole-image scan,
 * is the tool that derives the recorded RVAs and proves them unique offline. */
#ifndef BACKEND_SIGNATURES_H
#define BACKEND_SIGNATURES_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* IDA-style pattern with ?/?? wildcards. rva_vk / rva_gl are the function's
 * RVAs in the two build 13954591 executables; 0 = not on that renderer. An
 * entry with both zero is a code-site anchor of the globals table
 * (engine_globals.c), whose recorded RVAs are those of the global itself; it is
 * still located by scanning. */
typedef struct sig_entry {
    const char *name;
    const char *pattern;
    uint32_t    rva_vk;      /* DOOMx64vk.exe build 13954591 */
    uint32_t    rva_gl;      /* DOOMx64.exe build 13954591 */
} sig_entry;

typedef enum sig_status {
    SIG_OK = 0,
    SIG_NOT_FOUND,      /* no recorded RVA for this host, or (scan) zero matches */
    SIG_AMBIGUOUS,      /* (scan) more than one match -- not an identification */
    SIG_BAD_PATTERN,    /* empty / malformed pattern, or pattern longer than the scan buffer */
    SIG_BAD_MODULE,     /* the module base is not a parseable PE32+ image */
    SIG_OK_HOOKED,      /* never produced; kept until the remaining comparisons drop it */
    SIG_CHANGED         /* the bytes at the recorded RVA differ from the pattern */
} sig_status;

typedef struct sig_result {
    const char *name;
    sig_status  status;
    uintptr_t   addr;   /* module_base + rva when SIG_OK, else 0 */
    uint32_t    rva;    /* the RVA resolved or checked, or 0 */
} sig_result;

/* Shared results capacity. Check database growth rather than silently omitting entries. */
#define SIG_RESULTS_MAX 320

/* The shipped engine signature database. NULL-terminated
 * (the final entry has name==NULL). */
extern const sig_entry BACKEND_ENGINE_SIGNATURES[];
/* OpenGL-only helper; resolved only after identifying the OpenGL post job. */
extern const sig_entry NAV_RENDER_TARGET_GL_SIGNATURE;

/* Count of real entries in BACKEND_ENGINE_SIGNATURES (excluding the NULL terminator). */
size_t sig_db_count(void);

/* Check one signature at its recorded RVA for the host's renderer: SIG_OK,
 * SIG_CHANGED (bytes differ), or SIG_NOT_FOUND (no RVA for this host). */
sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out);

/* Resolve up to cap database entries; return the number with SIG_OK.
 * Callers should provide at least sig_db_count() slots. */
size_t sig_resolve_all(const uint8_t *module_base, sig_result *results, size_t cap);

/* Scan every executable section for the pattern: SIG_OK for exactly one
 * match, SIG_AMBIGUOUS, or SIG_NOT_FOUND. For deriving and auditing RVAs. */
sig_status sig_scan_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out);

/* The result named name, or NULL. */
const sig_result *sig_lookup(const sig_result *results, size_t n, const char *name);

/* The address of the SIG_OK result named name, or 0. */
uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name);

/* A short reason for a status, for logs and the feature-health table. */
const char *sig_status_reason(sig_status status);

#endif /* BACKEND_SIGNATURES_H */
