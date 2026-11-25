/* The startup report of live signature resolution, through backend_log. The
 * diagnostic build (SH_DIAG) also tests a detour on scratch code. See
 * dllmain.c for timing and partial-binding behavior. */
#ifndef BACKEND_SMOKE_H
#define BACKEND_SMOKE_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Reports the pre-install resolution snapshot (and, in the diagnostic build, tests a scratch detour); returns 0 only
 * when that detour failed, since then no engine code may be patched. deferred_ms is elapsed startup time for the
 * log, not a delay. */
int sh_smoke_run(const uint8_t *doom_base, const sig_result *results, size_t count,
                 unsigned long deferred_ms);

#endif /* BACKEND_SMOKE_H */
