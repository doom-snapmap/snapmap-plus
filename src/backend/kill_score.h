/* kill_score.h -- a ported demon's kill scores in the map's Player Score as the stock demon it stands in for.
 * The Player Score switch over the 18 stock spawn-type bits (Vulkan 0x140890390, OpenGL 0x14088F2C0:
 * (rules, type) -> int) scores a ported spawn type as 0, so the product detours it and answers a ported type
 * as its stand-in (sh_modserve_spawn_type_standin); the snapAIScore lookup (Vulkan 0x1408904D0, OpenGL
 * 0x14088F400) is answered the same way, so no snapAIScore decl is written for a port. */
#ifndef BACKEND_KILL_SCORE_H
#define BACKEND_KILL_SCORE_H

#include <stdint.h>
#include "signatures.h"

/* NULL-terminated: the Player Score switch. Its stolen window (SH_KS_STOLEN bytes) holds one short
 * conditional jump at SH_KS_JCC_OFFSET, which the hook relocates. */
extern const sig_entry SH_KILL_SCORE_SIGNATURES[];
#define SH_KS_STOLEN     16
#define SH_KS_JCC_OFFSET 8
/* [1]: the snapAIScore lookup. Its window (SH_KS_LOOKUP_STOLEN bytes: two
 * register saves, push rdi, sub rsp,0x20) has no relative operand. */
#define SH_KS_LOOKUP          1
#define SH_KS_LOOKUP_STOLEN  15

/* The type the switch should read for an AI of spawn type `type`: the
 * stand-in's stock bits when `standin` answers for it, else `type`. Pure. */
unsigned sh_kill_score_type(unsigned type, unsigned (*standin)(unsigned type));

/* Resolve the switch on this executable and detour it. 1 when in place. */
int sh_kill_score_install(const uint8_t *module_base);

#endif /* BACKEND_KILL_SCORE_H */
