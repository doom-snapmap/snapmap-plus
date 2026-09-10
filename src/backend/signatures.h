/* Masked-byte resolver for functions in the mapped engine executable.
 * Scan executable sections independent of section names; reject ambiguous matches.
 * A recorded RVA can also recognize a detoured entry after a signature miss. */
#ifndef BACKEND_SIGNATURES_H
#define BACKEND_SIGNATURES_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* IDA-style pattern with ?/?? wildcards. known_rva is an extraction-build
 * reference and hook-tolerant fallback location; 0 disables that fallback. */
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
    SIG_OK_HOOKED       /* scan missed, but a detour at known_rva has a matching fixed tail */
} sig_status;

typedef struct sig_result {
    const char *name;
    sig_status  status;
    uintptr_t   addr;   /* module_base + rva, or 0 */
    uint32_t    rva;    /* recovered RVA, or 0 */
} sig_result;

/* Shared results capacity. Check database growth rather than silently omitting entries. */
#define SIG_RESULTS_MAX 128

/* The shipped engine signature database. NULL-terminated
 * (the final entry has name==NULL). */
extern const sig_entry BACKEND_ENGINE_SIGNATURES[];
/* OpenGL-only helper; resolved only after identifying the OpenGL post job. */
extern const sig_entry NAV_RENDER_TARGET_GL_SIGNATURE;

/* Count of real entries in BACKEND_ENGINE_SIGNATURES (excluding the NULL terminator). */
size_t sig_db_count(void);

/* Resolve one signature over `module_base`'s executable sections. Returns the status; fills *out. */
sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out);

/* Resolve up to cap database entries. Return the number with SIG_OK or
 * SIG_OK_HOOKED. Callers should provide at least sig_db_count() slots. */
size_t sig_resolve_all(const uint8_t *module_base, sig_result *results, size_t cap);

/* Look up a single resolved address by name from a results array (returns 0 if not OK / not found). */
uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name);

#endif /* BACKEND_SIGNATURES_H */
