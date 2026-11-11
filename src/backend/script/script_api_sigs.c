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
 *                           runs the trace at once (VK 0x141638340 / GL 0x1416391C0). */
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
    { NULL, NULL, 0, 0 }
};
