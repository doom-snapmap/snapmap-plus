/* shield_diag.h -- DIAGNOSTIC build hooks (compiled only when SNAPHAK_DIAG is defined).
 *
 * A catch-all crash + environment logger for diagnosing a remote end-user's crash that the recovery
 * shield's VEH does NOT trap (a fault outside DOOM's code, or a __fastfail/heap-corruption). LOG-ONLY:
 * it never alters control flow, so it cannot change crash behavior -- safe to ship to an end-user.
 * Writes snaphak_diag.log next to the DLL. See shield_diag.c.
 */
#ifndef SNAPHAK_SHIELD_DIAG_H
#define SNAPHAK_SHIELD_DIAG_H
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call from DllMain DLL_PROCESS_ATTACH (self = the DLL HINSTANCE). Sets the log path, installs the
 * first-chance VEH + unhandled-exception filter immediately (loader-lock-safe), and spawns a thread
 * for the heavier environment dump (loaded modules / folder state). No-op if called twice. */
void shield_diag_install(HINSTANCE self);

/* Call from DllMain DLL_PROCESS_DETACH -- records whether the process is exiting cleanly (lets us tell
 * a crash apart from a normal quit in the log). */
void shield_diag_detach(void);

#ifdef __cplusplus
}
#endif
#endif /* SNAPHAK_SHIELD_DIAG_H */
