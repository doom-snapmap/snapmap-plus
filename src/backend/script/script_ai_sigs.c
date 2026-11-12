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
 *                      is reached through this unique call site.
 *   AiRecoilPlay       VK 0x1403D9520  GL 0x1403D8A40  inside the recoil play (VK 0x1403D9300): the weapon's
 *                      distance alpha (recoilAnimAlphas over recoilAlphaRanges) times the float at ai+0x24EB0
 *                      becomes the recoil animator's weight. The FSM states write their recoilStrength there
 *                      on enter and 1.0 on exit (VK 0x14062B630, 0x14062C3F0).
 *   AiNextPull         VK 0x1404447D0  GL 0x1404436F0  the weapon holder's next-pull writer: game clock
 *                      (vtable +0x810, group 1) + interval into holder +(slot+4)*0x10, slot = table[fire decl
 *                      +0x1B4]; the fire decl call (VK 0x140F137B0 / GL 0x140F12C10) and the slot table.
 *                      The can-fire check (VK 0x1404439D0) refuses with "cooldown" while the clock is below it.
 *   AiAimFocusSite     VK 0x140438D71  GL 0x140437CE1  ae_selectAimFocusPoint (VK 0x140438CD0): its call of
 *                      the set-focus-to-position (VK 0x14041BBA0 / GL 0x14041AD60; ai, focus 0/2/3, idVec3*,
 *                      true, -1, 14).
 *   AiPainStringSite   VK 0x1403F5EDD  GL 0x1403F53BD  inside the pain request (VK 0x1403F5440 / GL
 *                      0x1403F4920): the second accessor branch of the painString, the atomic-string set
 *                      (VK 0x14032B3D0 / GL 0x14032B040) into [rbp+0x88], then the 14 bytes where the string
 *                      is final; the request later compares each pain anim's +0x48 string with [rbp+0x88].
 *   AiThreatRow        VK 0x1406E08D0  GL 0x1406DFD00  the threat table's check of one SDPSReaction_t row
 *                      (threat, actor, row, ...), called for every row by the evaluator (VK 0x1406DFE70);
 *                      the row's reaction is at +0x1C. Detoured; 15-byte window.
 *   AiPathLength       VK 0x14050EA60  GL 0x14050D790  the length of a computed path (move FSM, path): the
 *                      path query (VK 0x140515300) calls it with the path it just built (segments +0x90,
 *                      count +0x70, stride 0x1C, from +4, to +0x10). Detoured; 14-byte window.
 *   AiAnimOfProp       VK 0x141712C60  GL 0x141713650  an animWeb node's animation (prop, model, 0) -> idMD6Anim.
 *   AiAnimDelta        VK 0x1415C5290  GL 0x1415C63F0  an animation's total root motion (anim, idVec3*, NULL).
 *   AiWeaponInSlot     VK 0x14074AB30  GL 0x14074A440  the weapon in an equip slot: the demon's vtable +0x678 item
 *                      of that equipSlot_t, cast to a weapon (NULL for none or another item). The ae pull and
 *                      equip handlers and the fire control's per-hand loop read hands through it (evidence
 *                      snaphak-re logs/shared-ghidra/SCF2-*).
 *   AiInventoryItem    VK 0x140EEDC50  GL 0x140EED200  the inventory list's item `index` (the list the demon's
 *                      vtable +0x660 returns, ai +0x9900), as EquipInventoryItem (VK 0x14040CB00) reads it.
 *   AiMuzzleTest       VK 0x140436460  GL 0x1404353D0  the muzzle test (ai, mode, weapon): false while the aim
 *                      focus is not valid (ai +0xEE9B), else a ray from the muzzle tag (the weapon's muzzle when
 *                      the model has none) against a sphere at the focus point (ai +0xE020) of radius
 *                      ai +0x11AE4, times 3 unless ai +0x11AE8 is PRECISE, times 0.5 for mode 0 and 1.5 for
 *                      mode 1. CheckPullTrigger asks it with mode 0; called directly it is the engine's answer
 *                      (shared Ghidra m5-fire FUN_140436460; GL by the same bytes).
 *   AiClearMove        VK 0x1404EA6F0  GL 0x1404E9260  ClearMove(animFSM, result code): the call the scripted
 *                      actions start with (animFSM = *(ai+0x23198), code 0); it ends the move request, the path
 *                      state and the flight path. The GL address is the one place those bytes match.
 *   AiFireThink        VK 0x1404456E0  GL 0x140444600  the fire control's per-frame think: its two fire modes at
 *                      the fire control +0x1C (animation events) and +0x20 (script events), the control at
 *                      ai +0x22278. Present for the offsets the test reads from it.
 *   AiIdleAction       VK 0x1405DCDD0  GL 0x1405DC0A0  the ssaction_Idle handler: stores the idle turn type in
 *                      the animFSM when the aibehavior decl (ai +0xC120) has a turnParms entry (list +0x528,
 *                      count +0x530) for it. Present for those offsets.
 *   AiCheckPull        VK 0x1404439D0  GL 0x1404428F0  CheckPullTrigger, the fire gate: asks the muzzle test only
 *                      when the fire control's requireMuzzleOnTarget byte (+0x27) is set (cmp at +0x25A).
 *   AiLaserAdd         VK 0x140551940  GL 0x1405509D0  the laser targeter component's add-laser (component,
 *                      idAtomicString*): an entry of that name is reset, else one is appended, drawn with the
 *                      ribbon the component's decl (+0x10) names in laserPinpoint_ribbonDecl (+0x108; read at
 *                      +0x193 and +0x1B2 of the function), and left off. The engine calls it only from the
 *                      enter of two stock state classes (the Revenant's hover attack, the laser soldier's beam);
 *                      ae_setLaserState only switches entries that exist.
 *   AiLaserRemove      VK 0x1405517A0  GL 0x140550830  the component's remove (component, idAtomicString*).
 *   AiLaserState       VK 0x1403ED2B0  GL 0x1403EC780  the ae_setLaserState handler. Present for the offset of
 *                      the component in an idAI2 (+0x2CD90).
 *   AiFootTable        VK 0x14074B790  GL 0x14074B0A0  the footstep table of a foot (actor, foot index): the
 *                      actor's footstepEffectTable by gait (walk, slow walk, crouch, sprint, landing), never by
 *                      foot. Detoured; 15-byte window (snaphak-re reports/R-M7-engine-wave-laser-foot.md). */
#include <stddef.h>
#include <string.h>
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
    { "AiRecoilPlay",
      "74 0C FF C3 83 FB 04 7C DC E9 ?? ?? ?? ?? 48 8D 4C 24 50 E8 ?? ?? ?? ?? 8B 94 24 F8 00 00 00 "
      "48 8D 4C 24 50 E8 ?? ?? ?? ?? F3 0F 59 B6 B0 4E 02 00 48 63 C3 48 69 C8 F8 00 00 00",
      0x3D9520u, 0x3D8A40u },
    { "AiNextPull",
      "48 89 5C 24 10 44 89 44 24 18 55 56 57 48 83 EC 20 48 8B F1 48 8B FA 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 40 "
      "41 B8 01 00 00 00 48 8B 01 FF 90 10 08 00 00 8B 5C 24 50 48 8B 4E 08 03 18 48 85 C9 75 0B 8B 97 D4 08 00 00 "
      "48 8B CF EB 06 8B 91 D4 08 00 00 E8 ?? ?? ?? ?? 48 8D 2D ?? ?? ?? ?? 48 63 80 B4 01 00 00",
      0x4447D0u, 0x4436F0u },
    { "AiAimFocusSite",
      "C7 44 24 28 0E 00 00 00 4C 8D 44 24 48 41 B1 01 C7 44 24 20 FF FF FF FF 33 D2 48 8B CB F3 0F 10 00 "
      "F3 0F 11 44 24 48 F3 0F 10 48 04 F3 0F 11 4C 24 4C F3 0F 10 40 08 F3 0F 11 44 24 50 E8 ?? ?? ?? ??",
      0x438D71u, 0x437CE1u },
    { "AiPainStringSite",
      "48 8B 8B 80 CD 02 00 48 85 C9 74 2A 48 8D 95 60 01 00 00 E8 ?? ?? ?? ?? 90 48 8B 50 10 48 8D 8D 88 00 00 00 "
      "E8 ?? ?? ?? ?? 90 48 8D 8D 60 01 00 00 E8 ?? ?? ?? ?? 48 8B 83 20 C1 00 00 48 8B 88 90 00 00 00 48 89 4D 50",
      0x3F5EDDu, 0x3F53BDu },
    { "AiThreatRow",
      "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 40 41 0F B6 40 46 41 8B E9 49 8B D8 48 8B F2 38 84 24 A0 00 00 00 "
      "74 12 32 C0",
      0x6E08D0u, 0x6DFD00u },
    { "AiPathLength",
      "48 83 EC 28 48 63 4A 70 0F 57 E4 48 85 C9 0F 8E ?? ?? ?? ?? 48 8B 82 90 00 00 00 0F 28 2D ?? ?? ?? ?? "
      "48 83 C0 08 0F 29 74 24 10",
      0x50EA60u, 0x50D790u },
    { "AiAnimOfProp",
      "40 53 48 83 EC 20 48 8B 01 4C 8B CA 49 63 D8 48 85 C0 75 41 48 8D 51 08 B8 FF FF 00 00 66 39 02 74 31 "
      "4D 85 C9 74 2C 41 B0 01 49 8B C9",
      0x1712C60u, 0x1713650u },
    { "AiAnimDelta",
      "40 53 56 57 48 81 EC D0 00 00 00 48 8B 41 58 33 FF 48 89 AC 24 F0 00 00 00 49 8B E8 48 8B F2 8B DF "
      "48 85 C0 74 08 48 8D 58 28 48 8D 78 58 0F 29 B4 24 C0 00 00 00 0F 29 BC 24 B0 00 00 00",
      0x15C5290u, 0x15C63F0u },
    { "AiWeaponInSlot",
      "48 83 EC 28 48 8B 01 FF 90 78 06 00 00 48 85 C0 75 05 48 83 C4 28 C3 48 8B C8 48 83 C4 28 E9",
      0x74AB30u, 0x74A440u },
    { "AiInventoryItem",
      "48 83 EC 28 85 D2 78 4F 3B 51 08 7D 4A 48 89 5C 24 20 48 63 DA 48 C1 E3 05 48 03 19 8B 03 48 8D 53 04 "
      "3B 02 75 1B 3D FE FF FF 01 74 1C 48 83 7B 08 00 75 15 48 83 3D",
      0xEEDC50u, 0xEED200u },
    { "AiMoveTransition",    /* the move system's transition query for a request: (move, request, out, kind);
                             * kind 3 is the turn on the spot, whose clip query rejects root travel unless the
                             * request's bit 0x102.0 allows it */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C0 E9 FF FF B8 40 17 00 00 E8 ?? ?? ?? ?? 48 2B E0 "
      "48 C7 45 08 FE FF FF FF",
      0x4FC0B0u, 0x4FADE0u },
    { "AiMuzzleTest",
      "48 81 EC A8 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 80 00 00 00 80 B9 9B EE 00 00 00 75 1A 32 C0 "
      "48 8B 8C 24 80 00 00 00 48 33 CC E8 ?? ?? ?? ?? 48 81 C4 A8 00 00 00 C3 83 B9 E8 1A 01 00 01 F2 0F 10 81 "
      "20 E0 00 00 8B 81 28 E0 00 00",
      0x436460u, 0x4353D0u },
    { "AiClearMove",
      "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC E0 01 00 00 48 C7 44 24 60 FE FF FF FF 48 89 9C 24 30 02 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 D0 01 00 00 44 8B E2 4C 8B F9 48 8B 09 48 83 B9 40 CD 02 00 00 "
      "74 11",
      0x4EA6F0u, 0x4E9260u },
    { "AiFireThink",
      "48 89 5C 24 08 48 89 74 24 10 44 89 44 24 18 57 48 83 EC 20 48 8B FA 48 8B D9 8B 51 10 85 D2 74 11 48 8B CF "
      "E8 ?? ?? ?? ?? 44 8B 44 24 40 48 89 43 08 48 8B D7 48 8B CB E8 ?? ?? ?? ?? 8B 43 20 85 C0 74 58 8B 4B 1C "
      "85 C9 74 51 83 F9 05 0F 45 C1 85 C0 74 47 7E 55 83 F8 02 7E 2E 83 F8 03 74",
      0x4456E0u, 0x444600u },
    { "AiIdleAction",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B F1 45 8B F1 49 8B C8 "
      "49 8B F8 48 8B DA E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 63 54 24 50 48 8B E8 89 96 08 01 00 00 85 D2 "
      "78 7E 48 8B 8F 20 C1 00 00 3B 91 30 05 00 00 7D 6F 48 8B 89 28 05 00 00 48 83",
      0x5DCDD0u, 0x5DC0A0u },
    { "AiCheckPull",
      "40 56 57 41 55 41 56 48 83 EC 48 4D 8B F1 49 8B F0 48 8B FA 4C 8B E9 4D 85 C0 75 0D 32 C0 48 83 C4 48 41 5E "
      "41 5D 5F 5E C3 48 8B CE E8 ?? ?? ?? ?? 84 C0 74 E7 4D 85 F6 74 10 49 8B 06 49 8B CE FF 90 80 05 00 00 84 C0 "
      "75 D2 4C 89 7C 24 40 4C 8B BC 24 98 00 00 00 41 8B 07",
      0x4439D0u, 0x4428F0u },
    { "AiLaserAdd",
      "48 89 74 24 20 41 56 48 83 EC 20 4C 8B F2 48 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 44 8B 86 08 01 00 00 "
      "33 C0 48 89 5C 24 30 45 85 C0 7E 23",
      0x551940u, 0x5509D0u },
    { "AiLaserRemove",
      "48 89 74 24 20 41 57 48 83 EC 20 4C 8B FA 48 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
      "48 89 5C 24 30",
      0x5517A0u, 0x550830u },
    { "AiLaserState",
      "48 89 5C 24 10 57 48 83 EC 20 48 83 B9 90 CD 02 00 00 48 8B DA 48 8B F9 74 50 48 8B 54 24 50 48 8D 4C 24 30 "
      "45 84 C9 41 B1 01",
      0x3ED2B0u, 0x3EC780u },
    { "AiFootTable",
      "48 89 5C 24 18 48 89 6C 24 20 56 41 56 41 57 48 83 EC 40 33 F6 8B EA 4C 8B F1 40 38 B1 F8 43 00 00 74 45 "
      "39 35 ?? ?? ?? ??",
      0x74B790u, 0x74B0A0u },
    { NULL, NULL, 0, 0 }
};

/* aiMonsterType_t (the engine's {name, value} table, VK 0x143557480 / GL
 * 0x14358E090: one bit per type, NONE 0). */
const char *const SH_AI_MONSTER_NAMES[SH_AI_MONSTER_TYPES] = {
    "AI_MONSTER_NONE", "AI_MONSTER_GENERIC", "AI_MONSTER_ZOMBIE", "AI_MONSTER_IMP", "AI_MONSTER_HELLIFIED_SOLDIER",
    "AI_MONSTER_LASER_SOLDIER", "AI_MONSTER_HELLIFIED_SHOTGUNNER", "AI_MONSTER_LOSTSOUL", "AI_MONSTER_MAX_COMMON",
    "AI_MONSTER_ARCHVILE", "AI_MONSTER_HELLKNIGHT", "AI_MONSTER_MANCUBUS", "AI_MONSTER_PINKY",
    "AI_MONSTER_PINKY_SPECTRE", "AI_MONSTER_NPC", "AI_MONSTER_CACODEMON", "AI_MONSTER_REVENANT", "AI_MONSTER_BARON",
    "AI_MONSTER_MAX_HEAVY", "AI_MONSTER_CYBERDEMON", "AI_MONSTER_OLIVIASGUARD", "AI_MONSTER_TALISMANGUARD",
    "AI_MONSTER_CYBERDEMON_HELL", "AI_MONSTER_SPIDER_MASTERMIND"
};

/* aiFireMode_t and aiIdleTurn_t, in the engine's order (13954591; the sigs test compares them
 * with the executables' own enum tables). */
const char *const SH_AI_FIREMODE_NAMES[SH_AI_FIREMODE_MAX + 1] = {
    "AIFIREMODE_HOLDFIRE", "AIFIREMODE_WEAPON_EXPLICIT_RELEASE", "AIFIREMODE_WEAPON", "AIFIREMODE_FIREWHENREADY",
    "AIFIREMODE_SPRAYWHENREADY", "AIFIREMODE_MAX"
};
const char *const SH_AI_IDLETURN_NAMES[SH_AI_IDLETURN_MAX + 1] = {
    "AIIDLETURN_RELAXED", "AIIDLETURN_COMBAT", "AIIDLETURN_INCOVER", "AIIDLETURN_MAX"
};

/* The pain-string stub (script_api_ai.h). Entered by the 14-byte jump that
 * replaces the two loads at the site, with rbx the demon and rbp the pain
 * request's frame:
 *   push rax, rcx, rdx, r8, r9, r10, r11      the volatile registers
 *   mov r11,rsp; and rsp,-16; sub rsp,0x90    an aligned frame
 *   mov [rsp+0x88],r11                         (the stack before it)
 *   movdqu [rsp+0x20..0x70],xmm0..xmm5         the volatile vector registers
 *   mov rcx,rbx; mov rax,fetch; call rax       fetch(demon) -> string or NULL
 *   movdqu xmm0..xmm5,[rsp+0x20..0x70]
 *   mov rsp,[rsp+0x88]
 *   test rax,rax; jz +7; mov [rbp+0x88],rax     the request's string, when the script set one
 *   pop r11, r10, r9, r8, rdx, rcx, rax
 *   jmp [rip+0]; dq resume                     the trampoline: the two loads, then back
 * Flags are not live at the site (the next instruction that reads them is a
 * cmp after three moves). */
size_t sh_ai_pain_stub_build(uint8_t *code, size_t cap, const void *fetch, const void *resume)
{
    static const uint8_t head[] = {
        0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
        0x4C, 0x8B, 0xDC,                                     /* mov r11,rsp */
        0x48, 0x83, 0xE4, 0xF0,                               /* and rsp,-16 */
        0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00,             /* sub rsp,0x90 */
        0x4C, 0x89, 0x9C, 0x24, 0x88, 0x00, 0x00, 0x00,       /* mov [rsp+0x88],r11 */
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,                   /* movdqu [rsp+0x20],xmm0 */
        0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,
        0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60,
        0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,                   /* movdqu [rsp+0x70],xmm5 */
        0x48, 0x8B, 0xCB,                                     /* mov rcx,rbx */
        0x48, 0xB8                                            /* mov rax, imm64 (fetch) */
    };
    static const uint8_t mid[] = {
        0xFF, 0xD0,                                           /* call rax */
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,                   /* movdqu xmm0,[rsp+0x20] */
        0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
        0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60,
        0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
        0x48, 0x8B, 0xA4, 0x24, 0x88, 0x00, 0x00, 0x00,       /* mov rsp,[rsp+0x88] */
        0x48, 0x85, 0xC0,                                     /* test rax,rax */
        0x74, 0x07,                                           /* jz +7 */
        0x48, 0x89, 0x85, 0x88, 0x00, 0x00, 0x00,             /* mov [rbp+0x88],rax */
        0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58,
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00                    /* jmp [rip+0] */
    };
    size_t n = 0;
    uint64_t f = (uint64_t)(uintptr_t)fetch, r = (uint64_t)(uintptr_t)resume;
    if (!code || cap < sizeof head + 8 + sizeof mid + 8 || !fetch || !resume) return 0;
    memcpy(code + n, head, sizeof head); n += sizeof head;
    memcpy(code + n, &f, 8); n += 8;
    memcpy(code + n, mid, sizeof mid); n += sizeof mid;
    memcpy(code + n, &r, 8); n += 8;
    return n;
}
