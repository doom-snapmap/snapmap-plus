/* smoke.h -- the foundation smoke proof for the SnapHak backend.
 *
 * Proves the two foundation pieces work end-to-end against the LIVE DOOM module, with a visible
 * signal the daemon console ring captures (OutputDebugStringA via backend_log):
 *   1. the signature resolver re-finds the engine fns by SIGNATURE (no hardcoded RVA), and
 *   2. the inline-detour installer patches + invokes + reverses a detour.
 *
 * Emits one line:  "PB0: resolved N/M sigs (deferred Tms past load) ...; test detour OK"  (or a
 * specific failure form, e.g. the SteamStub-not-decrypted timeout).
 *
 * DEFERRAL: DOOMx64vk.exe is SteamStub-wrapped -- its `.text` is encrypted at DLL-init and only
 * decrypted later, so the bootstrap thread POLLS sh_resolve_count until the whole DB resolves (the
 * decrypt has landed) before calling sh_smoke_run. See dllmain.c + steamstub-drm-unpack.md.
 */
#ifndef BACKEND_PB0_SMOKE_H
#define BACKEND_PB0_SMOKE_H

#include <stdint.h>
#include <stddef.h>

/* One lightweight resolve pass: returns how many signatures resolve UNIQUELY over `doom_base` right
 * now (== sig_db_count() once the SteamStub has decrypted .text). Used by the bootstrap thread's
 * poll-until-decrypted loop; no side effects, no logging. */
size_t sh_resolve_count(const uint8_t *doom_base);

/* Run the smoke proof against `doom_base` (the DOOM module base). `deferred_ms` = how long past load
 * the resolver took to succeed (for the log line). Emits the PB0 result line via backend_log. Returns 1
 * if BOTH the resolver (all sigs unique) and the detour self-test passed. */
int sh_smoke_run(const uint8_t *doom_base, unsigned long deferred_ms);

#endif /* BACKEND_PB0_SMOKE_H */
