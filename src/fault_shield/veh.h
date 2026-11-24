/* Engine-fault classification, recovery, and stack capture. */
#ifndef SHIELD_VEH_H
#define SHIELD_VEH_H

#include <windows.h>
#include <stddef.h>

int veh_install(void);   /* AddVectoredExceptionHandler(first=1); returns 1 on success. */

/* For the unhandled-exception filter: redirect a non-AV hardware fault in DOOM code that no engine handler
 * took, as Class B does a wild access violation. Returns EXCEPTION_CONTINUE_EXECUTION when redirected, else
 * EXCEPTION_CONTINUE_SEARCH. */
LONG shield_second_chance(PEXCEPTION_POINTERS ep);

/* Walk a copy of `ctx` with the OS unwinder into newline-separated "MOD+0xRVA" lines. SEH-guarded per frame,
 * so a wild frame ends the walk. */
void shield_capture_stack(const CONTEXT *ctx, char *out, size_t cap, int maxframes);

#endif /* SHIELD_VEH_H */
