/* kill_score_sigs.c -- the private signature of kill_score.c, kept apart so the two-image test
 * (tests/kill_score_sigs_test.c) links it without the engine side.
 *
 * Build 13954591, read in Ghidra (R-SM2, work/SM2/evidence/vk-dc-0x140890390.json, gl-dc-0x14088f2c0.json,
 * vk-by-0x140890390.json, gl-by-0x14088f2c0.json): the Player Score switch over the 18 stock spawn-type bits,
 * called by the two co-op kill callers (Vulkan 0x140895763, 0x140895922). Its first five tests (Archvile,
 * Baron, Beam Demon, Cacodemon, Hell Soldier fields at +0x78BC .. +0x78CC of the rules object) are
 * byte-identical in the two executables and carry no relative operand but their own short branches. */
#include <stddef.h>
#include "kill_score.h"

const sig_entry SH_KILL_SCORE_SIGNATURES[] = {
    { "SnapPlayerScoreByType",
      "48 83 EC 28 0F BA E2 0A 73 0B 8B 81 BC 78 00 00 48 83 C4 28 C3 "
      "84 D2 79 0B 8B 81 C0 78 00 00 48 83 C4 28 C3 "
      "F6 C2 20 74 0B 8B 81 C4 78 00 00 48 83 C4 28 C3 "
      "0F BA E2 0C 73 0B 8B 81 C8 78 00 00",
      0x890390u, 0x88F2C0u },
    /* The kill's snapAIScore lookup (R-SO1: vk 0x1408904D0, gl 0x14088F400, read in Ghidra, logs/shared-ghidra/so1
     * vk_d_8904d0, gl_d_88f400): (rules, type, kill index, player, killer) walks the rules' snapAIScore list (+0x7930,
     * count +0x7938) for the first decl whose aiType (+0x70) meets the type and answers baseScore / (kills x
     * fallOffRate + 1), "Unable to lookup score decl for ai type %d" and 0 when none does; called by the two co-op
     * kill callers (vk 0x1408956FA, 0x1408958D1) with the victim's spawn type. Its first 15 bytes (two register saves,
     * push rdi, sub rsp) carry no relative operand; the pattern runs through the list loop, byte-identical in the two
     * executables. */
    { "SnapAIScoreLookup",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 63 81 38 79 00 00 45 33 D2 49 63 F0 48 8B F9 85 C0 7E 26 "
      "48 8B 99 30 79 00 00 4C 8B D8 41 8B C2 48 8B 0C C3 48 85 C9 74 05 85 51 70 75 29 41 FF C2 48 FF C0 49 3B C3 "
      "7C E7",
      0x8904D0u, 0x88F400u },
    { NULL, NULL, 0, 0 }
};
