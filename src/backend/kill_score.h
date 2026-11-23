/* kill_score.h -- a ported demon's kill scores in the map's Player Score as
 * the stock demon it stands in for.
 *
 * SnapMap scores a kill twice by the victim's conductor spawn type (R-SM1
 * finding 2): its snapAIScore decl, which a mod supplies as data, and the
 * map's Gameplay Settings Player Score field of that demon, which a switch
 * over the 18 stock spawn-type bits returns (build 13954591 Vulkan 0x140890390,
 * OpenGL 0x14088F2C0: (rules, type) -> int; "Unsupported AI Type, no score
 * will be returned" and 0 for any other bit). A ported demon has a spawn type
 * of its own (modregistry.h), so without this module its kill always scored 0
 * in the Player Score whatever the author set.
 *
 * The product detours the switch: a type the current listing gave a ported
 * demon is answered as the stock type its conductor row says it stands in for
 * (sh_modserve_spawn_type_standin), so the author's field for that class
 * applies to it, scaled as the game scales every field. Every stock type, and
 * a type no declared mod was given, reaches the switch unchanged.
 *
 * The kill's challenge score (the game challenge record's `score`) takes the
 * victim's snapAIScore decl, which the rules find in their list by the same
 * spawn type (Vulkan 0x1408904D0, OpenGL 0x14088F400). A ported type is
 * answered there as its stand-in too, so a port's kill scores the stand-in's
 * baseScore and falloff, as its Player Score field does (R-LV8 measured 0 for
 * a port kill, 825 for its stand-in's); the contract therefore writes no
 * snapAIScore decl for a port. */
#ifndef BACKEND_KILL_SCORE_H
#define BACKEND_KILL_SCORE_H

#include <stdint.h>
#include "signatures.h"

/* NULL-terminated: the Player Score switch. Its window (SH_KS_STOLEN bytes:
 * sub rsp,0x28; bt edx,10; jnc short; mov eax,[rcx+0x78BC]) holds one short
 * conditional jump at SH_KS_JCC_OFFSET, relocated by the hook. */
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
