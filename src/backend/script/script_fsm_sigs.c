/* script_fsm_sigs.c -- the game function the state-machine class bridge detours (script_fsm.h).
 *
 * Build 13954591 (shared Ghidra, snaphak-re reports R-M6-engine-fsm section 2.6; the pattern is
 * the only match in each executable, tests/script_combat_sigs_test.c): the type lookup by name,
 * VK 0x141583B20 / GL 0x141584BB0. Its first 15 bytes are three whole register saves
 * (mov [rsp+8],rbx; mov [rsp+0x10],rbp; mov [rsp+0x18],rsi), position-independent. */
#include "../signatures.h"
#include "script_fsm.h"

const sig_entry SH_SCRIPT_FSM_SIGNATURE = {
    "TypeInfoByName",
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 80 3D ?? ?? ?? ?? 00",
    0x1583B20u, 0x1584BB0u
};
