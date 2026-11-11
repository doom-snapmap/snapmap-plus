/* script_api_sigs.c -- the private signatures of the mod-script API calls
 * (script_api_common.c). Kept apart so the two-image test
 * (tests/script_api_sigs_test.c) links the table without the engine side. */
#include <stddef.h>
#include "script_api_common.h"

/* Build 13954591 (shared Ghidra, snaphak-re logs/shared-ghidra/SC4-*; each
 * pattern is the only match in each executable, at the recorded RVA):
 *   ScriptProcessEventArgs  VK 0x140B47350  GL 0x140B46940  (obj, ret[0x18], def, n, a0..a7)
 *   ScriptPostEventArgs     VK 0x140B45700  GL 0x140B44CF0  (obj, def, ms, n, a0...)
 *   ScriptEventArgClear     VK 0x140B44640  GL 0x140B43A50  releases an idEventArg
 *   ScriptEventArgEntity    VK 0x140B44300  GL 0x140B438F0  (arg, idEntity*) -> an 'e' argument
 *   ScriptTraceQuery        VK 0x141637CE0  GL 0x141638B60  (traces, handle*, result*, start*, end*, clip,
 *                           axis*, contents, pass spawn id, keep, name, pass spawn id 2); a result pointer
 *                           runs the trace at once (VK 0x141638340 / GL 0x1416391C0).
 *   ScriptDifficulty        VK 0x140391A80  GL 0x140391350  idGameLocal vtable +0x670 (game) -> int: the
 *                           g_gameDifficulty cvar when it is 0 or more, else the game's currentGameDifficulty
 *                           (+0x2C3010); 0 easy, 1 medium, 2 hard, 3 ultra-violent, 4 nightmare. Optional: the
 *                           calls work without it and difficulty() then answers nil.
 *   ScriptTagFind           VK 0x1416CE1E0  GL 0x1416CEE90  (model decl +0x210 prop list, prop name, tag name) ->
 *                           the tag row: +0 offset idVec3, +0xC rotation quaternion, +0x1C short parent joint
 *                           (-1 for a default row when there is none). The engine reads aim and focus tags with
 *                           it from the "_info" prop (VK 0x1407720A0, "can't find focus tracking tag").
 *   ScriptTagWorld          VK 0x1415EFAA0  GL 0x1415F0CA0  (animator, 1, tag row, idVec3 *pos, idMat3 *axis) ->
 *                           bool: the tag placed on its parent joint in world space, as idAI2 finds a muzzle
 *                           tag (VK 0x140434E20). Both optional: without them a tag name answers as a joint.
 *                           Evidence snaphak-re logs/shared-ghidra/SCF2-*. */
const sig_entry SH_SCRIPT_API_SIGNATURES[] = {
    { "ScriptProcessEventArgs",
      "44 89 4C 24 20 53 55 56 57 41 56 41 57 48 81 EC 28 01 00 00", 0xB47350u, 0xB46940u },
    { "ScriptPostEventArgs",
      "44 89 4C 24 20 53 56 57 48 83 EC 20 48 8B 01 41 8B F0", 0xB45700u, 0xB44CF0u },
    { "ScriptEventArgClear",
      "40 53 48 83 EC 20 80 39 37 48 8B D9 75 24 48 8B 49 08", 0xB44640u, 0xB43A50u },
    { "ScriptEventArgEntity",
      "40 53 48 83 EC 20 48 8B D9 C6 01 65 48 8B 0D", 0xB44300u, 0xB438F0u },
    { "ScriptTraceQuery",
      "48 89 5C 24 18 48 89 74 24 20 41 56 48 83 EC 60 4D 8B F1 4D 8B D0 48 8B F2 48 8B D9 4D 85 C0 "
      "0F 85 ?? ?? ?? ?? 48 8B 89 98 82 18 00 48 8B C1 48 2B 83 80 82 18 00 48 FF C0 48 3D 00 20 00 00 "
      "0F 87 ?? ?? ?? ?? 8B 93 28 01 12 00 81 FA 00 10 00 00 0F 83 ?? ?? ?? ?? 48 8D 41 01 48 89 6C 24 70 "
      "48 89 83 98 82 18 00 33 C9 25 FF 1F 00 00 48 89 7C 24 78 48 05 70 E5 00 00 48 8D BB 30 01 12 00",
      0x1637CE0u, 0x1638B60u },
    { "ScriptDifficulty",
      "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C8 48 8B 10 FF 92 B8 00 00 00 80 BB 88 0B 30 00 00 "
      "48 8B 50 38 8B 42 30 74 10 83 F8 01 7E 0B B8 01 00 00 00",
      0x391A80u, 0x391350u },
    { "ScriptTagFind",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 33 FF "
      "4D 8B E0 4C 8B FA 4C 8B F1 8B EF 39 79 08 7E 29",
      0x16CE1E0u, 0x16CEE90u },
    { "ScriptTagWorld",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 40 49 8B F8 B8 FF FF 00 00 49 83 C0 1C "
      "49 8B F1 8B EA 48 8B D9 66 41 39 00 75 04 32 C0",
      0x15EFAA0u, 0x15F0CA0u },
    { NULL, NULL, 0, 0 }
};
