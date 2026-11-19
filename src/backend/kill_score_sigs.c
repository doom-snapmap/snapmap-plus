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
    { NULL, NULL, 0, 0 }
};
