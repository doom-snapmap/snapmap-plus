/* Bootstrap checks for live signature resolution and scratch-memory detours.
 * The bootstrap polls until SteamStub exposes enough code, then records a smoke
 * result through backend_log. See dllmain.c for timing and partial-binding behavior. */
#ifndef BACKEND_PB0_SMOKE_H
#define BACKEND_PB0_SMOKE_H

#include <stdint.h>
#include <stddef.h>

/* Count signatures resolving now, including verified hooked fallbacks; no logging. */
size_t sh_resolve_count(const uint8_t *doom_base);

/* Test resolution and a scratch detour; return 1 if both pass. deferred_ms is
 * elapsed startup time for the result log, not a delay performed by this call. */
int sh_smoke_run(const uint8_t *doom_base, unsigned long deferred_ms);

#endif /* BACKEND_PB0_SMOKE_H */
