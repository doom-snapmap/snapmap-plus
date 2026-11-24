/* sync_clock_sigs.c -- the signature of the engine's animation re-anchor, apart from sync_clock.c so tests link it.
 * Build 13954591: the 49-byte pattern is the only match in each executable (callers: the adaptive-tick loop,
 * VK 0x14038E920, GL 0x14038E1C0), so no call-target check is needed. */
#include <stddef.h>
#include "sync_clock.h"

const sig_entry SH_SYNC_CLOCK_SIGNATURES[] = {
    { "AnimReanchor",
      "48 85 C9 0F 84 ?? ?? ?? ?? 48 8B C4 57 41 54 41 55 41 56 41 57 48 81 EC 70 04 00 00 48 C7 44 24 30 FE FF FF "
      "FF 48 89 58 08 48 89 68 10 48 89 70 18",
      0x15D1940u, 0x15D2D60u },
    { NULL, NULL, 0, 0 }
};
