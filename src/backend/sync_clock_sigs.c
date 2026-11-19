/* sync_clock_sigs.c -- the private signature of sync_clock.c (the engine's animation re-anchor), kept apart so the
 * two-image test (tests/sync_clock_sigs_test.c) and detour_windows_test link it without the engine side.
 *
 * Build 13954591 (paired with snaphak-re tools/re/site_pair.py): the re-anchor the adaptive-tick loop calls for
 * every animator, (animator, const int *now, int hz, int old_hz, bool precise, bool is_ai). Its only callers are the
 * eight calls of that loop (VK 0x14038E920, GL 0x14038E1C0). The 49-byte pattern (the null test, its jz with the
 * displacement left open, the five pushes, the frame and the three register saves) is the only match in each
 * executable. A single site, so no call-target check is needed. */
#include <stddef.h>
#include "sync_clock.h"

const sig_entry SH_SYNC_CLOCK_SIGNATURES[] = {
    { "AnimReanchor",
      "48 85 C9 0F 84 ?? ?? ?? ?? 48 8B C4 57 41 54 41 55 41 56 41 57 48 81 EC 70 04 00 00 48 C7 44 24 30 FE FF FF "
      "FF 48 89 58 08 48 89 68 10 48 89 70 18",
      0x15D1940u, 0x15D2D60u },
    { NULL, NULL, 0, 0 }
};
