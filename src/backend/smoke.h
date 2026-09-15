/* Bootstrap checks for live signature resolution and scratch-memory detours.
 * The bootstrap polls until SteamStub exposes enough code, then records a smoke
 * result through backend_log. See dllmain.c for timing and partial-binding behavior. */
#ifndef BACKEND_PB0_SMOKE_H
#define BACKEND_PB0_SMOKE_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Report the bootstrap's pre-install resolution snapshot and test a scratch
 * detour; return 1 if both pass. deferred_ms is
 * elapsed startup time for the result log, not a delay performed by this call. */
int sh_smoke_run(const uint8_t *doom_base, const sig_result *results, size_t count,
                 unsigned long deferred_ms);

#endif /* BACKEND_PB0_SMOKE_H */
