/* script_ai_sigs.c -- the private signatures of the AI calls of mod scripts
 * (script_api_ai.h). Kept apart so the two-image test
 * (tests/script_ai_sigs_test.c) links the table without the engine side.
 * Order matches enum sh_ai_sig.
 *
 * Build 13954591 (shared Ghidra and offline scans, snaphak-re
 * logs/shared-ghidra/SC5; every pattern is the only match in each
 * executable):
 *   AiIsRunningScript  VK 0x14042FBC0  GL 0x14042EE60  idAI2::IsRunningScript(ai): true while one of the
 *                      SuperScript ids at ai+0x23BB8 (count +0x23BC0) is running. The condition
 *                      Shared_ExecutingScript enters the alert cycle's ExecuteDoomscript state through it.
 *                      Detoured; 15-byte window.
 *   AiGetAwareness     VK 0x1404A9E30  GL 0x1404A85B0  perception (ai+0x29A88) GetAwareness(perception,
 *                      entity) -> the AIAWARE_ word of the entity's perception record (+4 of a 0x2B40 record).
 *   AiEntityPtrGet     VK 0x1403935E0  GL 0x140392E10  idEntityPtr resolve: the entity an idEntityPtr names, or
 *                      NULL (the current enemy is the one at ai+0x25050).
 *   AiPathSite         VK 0x1405F6A00  GL 0x1405F5C20  inside Shared_TargetDistanceFilter's evaluate (VK
 *                      0x1405F6780, vtable 0x1423D6C90 slot +0x60): the path-key call (VK 0x1403D54C0 /
 *                      GL 0x1403D49C0), the load of the query's first word (VK 0x145B64F00 / GL
 *                      0x14446A580) and the move FSM's path query call (VK 0x140515300 / GL 0x140514030).
 *                      The query's own bytes repeat in a sibling (VK 0x140515660 / GL 0x140514390), so it
 *                      is reached through this unique call site. */
#include <stddef.h>
#include "script_api_ai.h"

const sig_entry SH_AI_SIGNATURES[] = {
    { "AiIsRunningScript",
      "40 56 48 83 EC 20 8B 81 C0 3B 02 00 48 8B F1 85 C0 75 06 48 83 C4 20 5E C3 48 89 5C 24 30 33 DB "
      "48 89 7C 24 38 85 C0 7E 36",
      0x42FBC0u, 0x42EE60u },
    { "AiGetAwareness",
      "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 85 C0 78 27 48 8B 93 70 0B 00 00 48 85 D2 74 23 48 98 "
      "48 69 C8 40 2B 00 00 48 8D 42 10 48 03 C1 74 09 8B 40 04 48 83 C4 20 5B C3 33 C0 48 83 C4 20 5B C3",
      0x4A9E30u, 0x4A85B0u },
    { "AiEntityPtrGet",
      "40 53 48 83 EC 20 8B 05 ?? ?? ?? ?? 48 8B D9 39 41 08 75 0A 48 8B 41 10 48 83 C4 20 5B C3 48 63 41 04 "
      "85 C0 78 4C 48 8B C8",
      0x3935E0u, 0x392E10u },
    { "AiPathSite",
      "33 D2 48 8B CB E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 89 4C 24 50 48 8B 00 48 89 44 24 58 44 89 7C 24 60 "
      "48 8B 8F 08 08 00 00 48 8B 9B 98 31 02 00 48 8B 01 33 D2 FF 90 80 00 00 00 4C 8B C0 44 88 6C 24 30 "
      "48 8D 85 50 01 00 00 48 89 44 24 28 48 8D 44 24 40 48 89 44 24 20 F3 41 0F 10 9F F8 00 00 00 "
      "48 8D 54 24 50 48 8B CB E8 ?? ?? ?? ?? 83 F8 02 75 25",
      0x5F6A00u, 0x5F5C20u },
    { NULL, NULL, 0, 0 }
};
