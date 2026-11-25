/* script_fsm_sigs.c -- the game function the state-machine class bridge detours (script_fsm.h).
 *
 * Build 13954591: the type lookup by name, VK 0x141583B20 / GL 0x141584BB0, matched uniquely in each
 * executable; its first 15 bytes are three whole position-independent register saves. */
#include "../signatures.h"
#include "script_fsm.h"

const sig_entry SH_SCRIPT_FSM_SIGNATURE = {
    "TypeInfoByName",
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 80 3D ?? ?? ?? ?? 00",
    0x1583B20u, 0x1584BB0u
};
