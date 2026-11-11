/* The startup report of live signature resolution, through backend_log. The
 * diagnostic build (SH_DIAG) also tests a detour on scratch code. See
 * dllmain.c for timing and partial-binding behavior. */
#ifndef BACKEND_SMOKE_H
#define BACKEND_SMOKE_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Report the bootstrap's pre-install resolution snapshot (and, in the
 * diagnostic build, test a scratch detour); return 1 if every check passes.
 * deferred_ms is elapsed startup time for the result log, not a delay
 * performed by this call. */
int sh_smoke_run(const uint8_t *doom_base, const sig_result *results, size_t count,
                 unsigned long deferred_ms);

#endif /* BACKEND_SMOKE_H */
