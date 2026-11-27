/* script_ai_sigs.c -- the private signatures of the AI calls of mod scripts (script_api_ai.h), in the order of
 * enum sh_ai_sig. Kept apart so tests/script_ai_sigs_test.c links the table without the engine side.
 *
 * Build 13954591; every pattern is the only match in each executable (VK / GL addresses):
 *   AiIsRunningScript  0x14042FBC0 / 0x14042EE60  idAI2::IsRunningScript(ai); detoured, 15-byte window
 *   AiGetAwareness     0x1404A9E30 / 0x1404A85B0  GetAwareness(perception, entity) -> the AIAWARE_ word
 *   AiEntityPtrGet     0x1403935E0 / 0x140392E10  idEntityPtr resolve: the entity it names, or NULL
 *   AiPathSite         0x1405F6A00 / 0x1405F5C20  in Shared_TargetDistanceFilter's evaluate: path query call
 *   AiRecoilPlay       0x1403D9520 / 0x1403D8A40  in the recoil play: the recoil animator's weight
 *   AiNextPull         0x1404447D0 / 0x1404436F0  the weapon holder next-pull writer
 *   AiAimFocusSite     0x140438D71 / 0x140437CE1  in ae_selectAimFocusPoint: the set-focus-to-position call
 *   AiPainStringSite   0x1403F5EDD / 0x1403F53BD  in the pain request: where the pain string is final
 *   AiGoreInitSite     0x14042A7AA / 0x1404299FA  in the demon's spawn: the gore component's init gate
 *   AiShowWound        0x1409021C0 / 0x140901270  idGoreComponent::ShowWound (component, row); detoured
 *   AiMeshHideSite     0x140830F00 / 0x140830090  in the armour pop-off: the per-name mesh hide
 *   AiPainStart        0x140402FA0 / 0x140402480  the start of a pain reaction; detoured, 18-byte window
 *   AiThreatRow        0x1406E08D0 / 0x1406DFD00  the threat table's check of one row; detoured
 *   AiPathLength       0x14050EA60 / 0x14050D790  the length of a computed path; detoured, 14-byte window
 *   AiAnimOfProp       0x141712C60 / 0x141713650  an animWeb node's animation (prop, model, 0)
 *   AiAnimDelta        0x1415C5290 / 0x1415C63F0  an animation's total root motion (anim, idVec3*, NULL)
 *   AiWeaponInSlot     0x14074AB30 / 0x14074A440  the weapon in an equip slot, NULL for none or another item
 *   AiInventoryItem    0x140EEDC50 / 0x140EED200  the inventory list's item `index`
 *   AiMuzzleTest       0x140436460 / 0x1404353D0  the muzzle test (ai, mode, weapon)
 *   AiClearMove        0x1404EA6F0 / 0x1404E9260  ClearMove(animFSM, result code)
 *   AiFireThink        0x1404456E0 / 0x140444600  the fire control's per-frame think; modes +0x1C, +0x20
 *   AiIdleAction       0x1405DCDD0 / 0x1405DC0A0  the ssaction_Idle handler; turnParms +0x528
 *   AiCheckPull        0x1404439D0 / 0x1404428F0  CheckPullTrigger, the fire gate; byte +0x27
 *   AiLaserAdd         0x140551940 / 0x1405509D0  the laser targeter component's add-laser (component, atom*)
 *   AiLaserRemove      0x1405517A0 / 0x140550830  the laser targeter component's remove (component, atom*)
 *   AiLaserState       0x1403ED2B0 / 0x1403EC780  the ae_setLaserState handler; component at ai +0x2CD90
 *   AiFootTable        0x14074B790 / 0x14074B0A0  the footstep table of a foot (actor, foot index); detoured
 *   AiCmdSetWait       0x1405223F0 / 0x140521120  the animWeb command context's set-active-wait-handle
 *   AiMoveStage        0x14051A660 / 0x140519390  the animFSM's move update; stage at animFSM +0x2A9C
 *   AiRigEntry         0x141A20CD0 / 0x141A3CC00  the rig's entry (runtime, pose context, align); detoured
 *   AiRigRun           0x141A2C1F0 / 0x141A480D0  the rig engine's run (engine, context); blocks at engine +0
 *   AiRigBlock         0x141A2BDA0 / 0x141A47C80  a block's run: evaluates each component via vtable +0x58
 *   AiRigEval          0x141A29310 / 0x141A451F0  the two-bone leg component's evaluate (object, legs, ...)
 *   AiRigSolve         0x141A26940 / 0x141A428B0  the two-bone leg solve; reads only
 *   AiRigSite          0x140426377 / 0x140425437  in the demon setup: model animation object
 *   AiFkReference      0x140418BF0 / 0x140417D20  an fk effector's reference pose; detoured, 15-byte window
 *   AiWebLeaf          0x141705850 / 0x1417062C0  the anim web's current leaf; detoured, 18-byte window
 *   AiWebForceState    0x141704AA0 / 0x141705510  the anim web's ForceState; flags +0x56C, +0x570
 *   AiWebUpdate        0x14170B890 / 0x14170C300  the anim web's update; interruptBlend +0x575
 *   AiBreakableGore    0x1407A06E0 / 0x14079FED0  a gore breakable gore; detoured, 19 bytes
 *   AiBreakableBreak   0x14079E6D0 / 0x14079DEC0  idBreakable's break (breakable, 0); brokenStartTime +0xE6C
 *   AiWeaponUse        0x140435430 / 0x1404343A0  the demon's weapon-use behaviour; index at ai +0x250C8
 *   AiFxPhysPlace      0x141650390 / 0x141650F30  an effect rigid body's broad phase entry moved to a place
 *   AiDamageWound      0x1408FC8E0 / 0x1408FB910  DamageActivatesWound (component, ...); detoured, 16 bytes
 *   AiWebScalarSet     0x14170A950 / 0x14170B3C0  the anim web set-variable-by-name
 *   AiUpdateRelations  0x1404AA490 / 0x1404A8C10  the senses' UpdateRelations; relation table rows +0x28
 *   AiBestRelation     0x1404AAD10 / 0x1404A9490  the engine's pick from a relation list (senses, list)
 *   AiFsmMove          0x14064D330 / 0x14064C5B0  the entrance move of an AI FSM into a state
 *   AiGameTime         0x14038DE60 / 0x14038D700  the game time record a request is given (game, out[4]) */
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
    { "AiCmdSetWait",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 79 18 48 8B F2 48 8B D9 48 85 FF 74 5E 48 3B D7 74 59 "
      "48 8B 09 48 85 C9 74 24 48 8B 97 E0 00 00 00 48 85 D2 74 18 F6 87 E9 00 00 00 01 75 0F 45 33 C0 "
      "E8 ?? ?? ?? ?? 80 8F E9 00 00 00 01 48 8B 4B 18 BA 09 00 00 00 E8 ?? ?? ?? ?? 48 8B 43 18 "
      "C7 40 18 09 00 00 00 48 89 73 18",
      0x5223F0u, 0x521120u },
    { "AiMoveStage",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 83 3D ?? ?? ?? ?? 00 49 8B D8 48 8B F2 48 8B F9 0F 85 ?? ?? ?? ?? "
      "41 83 48 08 04 4C 8B C2 48 8B D3 E8 ?? ?? ?? ?? 8B 87 9C 2A 00 00 FF C8 83 F8 12",
      0x51A660u, 0x519390u },
    { "AiGoreInitSite",
      "83 B8 20 01 00 00 01 74 44 83 3D ?? ?? ?? ?? 00 74 3B 49 8B 06 49 8B CE FF 90 E0 06 00 00 48 8B 90 30 06 00 00 "
      "4D 8D 86 08 4E 00 00 49 8D 8E D8 B3 02 00 4C 89 7C 24 30 4C 89 7C 24 28 48 89 54 24 20 45 33 C9 49 8B D6 "
      "E8 ?? ?? ?? ??",
      0x42A7AAu, 0x4299FAu },
    { "AiShowWound",
      "48 89 4C 24 08 53 57 48 81 EC 88 00 00 00 48 83 B9 30 06 00 00 00 48 8B FA 48 8B D9 0F 84 ?? ?? ?? ?? "
      "48 83 3A 00 0F 84 ?? ?? ?? ?? 4C 89 6C 24 70 4C 89 7C 24 60 4C 8D B9 B0 06 00 00",
      0x9021C0u, 0x901270u },
    { "AiMeshHideSite",
      "49 8B 56 48 48 8B 14 17 49 8B CD E8 ?? ?? ?? ?? 44 8B F8 48 8D 56 04 85 C0 78 61 8B 06 3B 02 75 1B "
      "3D FE FF FF 01 74 1C 48 83 7E 08 00 75 15 48 83 3D ?? ?? ?? ?? 00 74 0B 48 8B D6 48 8B CE E8 ?? ?? ?? ?? "
      "4C 8B 46 08 41 8B D7 48 8D 4D B8 E8 ?? ?? ?? ?? 90 48 8D 55 B8 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
      "41 8B D7 49 8B CD E8 ?? ?? ?? ??",
      0x830F00u, 0x830090u },
    { "AiPainStart",
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 58 F9 FF FF 48 81 EC A8 07 00 00 48 C7 45 C8 FE FF FF FF "
      "0F 29 B4 24 90 07 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 80 06 00 00 4D 8B E9 4C 89 45 A8 4C 8B E2 "
      "4C 8B F9 80 B9 1D AA 02 00 00 0F 85 ?? ?? ?? ?? 80 B9 EC A9 02 00 00 74 28 80 B9 E5 C0 00 00 00 74 1F "
      "48 81 C1 B8 23 01 00 48 8B 01 FF 50 78 BA E8 00 00 00 48 8B C8 E8 ?? ?? ?? ?? E9",
      0x402FA0u, 0x402480u },
    { "AiRigEntry",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 45 84 C0 74 17 48 8B 49 18 48 85 C9 74 0E 83 3D ?? ?? ?? ?? 00 "
      "75 05 E8 ?? ?? ?? ?? 48 8B 4B 08",
      0x1A20CD0u, 0x1A3CC00u },
    { "AiRigRun",
      "48 89 74 24 18 41 56 48 83 EC 20 4C 8B F1 48 8B F2 48 8B 4A 50 E8 ?? ?? ?? ?? 41 8B 46 08 85 C0 7E 30 "
      "48 89 5C 24 30 33 DB 48 89 7C 24 38 8B F8 49 8B 0E 48 8B D6 48 03 CB E8 ?? ?? ?? ?? 48 83 C3 70",
      0x1A2C1F0u, 0x1A480D0u },
    { "AiRigBlock",
      "40 53 55 56 57 41 56 48 81 EC F0 00 00 00 33 FF 4C 8B F2 F6 41 60 01 48 8B F1 75 39 48 63 69 20 48 85 ED "
      "7E 30 8B DF 66 0F 1F 84 00 00 00 00 00 48 8B 56 18 4D 8B CE 48 8B 0C 13 44 8B 44 13 10 48 8B 54 13 08 "
      "48 8B 01 FF 50 58 48 8D 5B 18",
      0x1A2BDA0u, 0x1A47C80u },
    { "AiRigEval",
      "45 85 C0 0F 8E ?? ?? ?? ?? 48 8B C4 48 89 58 20 55 56 41 56 48 8D 68 A8 48 81 EC 40 01 00 00 49 8B 59 50 "
      "4D 8B F1 F3 0F 10 15 ?? ?? ?? ?? 48 89 78 08 48 8D 7A 70",
      0x1A29310u, 0x1A451F0u },
    { "AiRigSolve",
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 81 EC D0 00 00 00 0F 29 70 E8 49 8B F0 0F 29 78 D8 "
      "48 8B E9 44 0F 29 40 C8 49 83 C0 48 44 0F 29 48 B8",
      0x1A26940u, 0x1A428B0u },
    { "AiRigSite",
      "49 8B 8C 24 B0 3D 00 00 F3 0F 11 44 24 20 4C 8B CB 48 8B D7 48 8B 89 98 0E 00 00 E8 ?? ?? ?? ?? "
      "49 8B 8C 24 B0 3D 00 00",
      0x426377u, 0x425437u },
    { "AiFkReference",
      "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 58 FA FF FF 48 81 EC 68 06 00 00 48 C7 45 80 FE FF FF "
      "FF 0F 29 70 A8 0F 29 78 98 44 0F 29 40 88",
      0x418BF0u, 0x417D20u },
    { "AiWebLeaf",
      "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 90 80 00 00 00 48 8B D0 48 83 78 10 00 75 39 48 83 78 18 00 75 32 "
      "48 0F BF 93 2C 03 00 00 48 8B 83 48 03 00 00",
      0x1705850u, 0x17062C0u },
    { "AiWebForceState",
      "66 44 89 44 24 18 66 89 54 24 10 57 41 55 48 83 EC 68 48 83 B9 10 03 00 00 00 49 8B F9 4C 8B E9 75 0A 33 C0 "
      "48 83 C4 68 41 5D 5F C3 48 89 5C 24 60",
      0x1704AA0u, 0x1705510u },
    { "AiWebUpdate",
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 98 48 81 EC 68 01 00 00 48 C7 45 D8 FE FF FF FF 0F 29 B4 24 "
      "50 01 00 00 48 8B 05",
      0x170B890u, 0x170C300u },
    { "AiBreakableGore",
      "4C 8B DC 55 41 54 41 55 41 56 41 57 49 8D AB 48 FF FF FF 48 81 EC 90 01 00 00 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 45 D8 4C 8B B5 F8 00 00 00 4D 8B E0",
      0x7A06E0u, 0x79FED0u },
    { "AiBreakableBreak",
      "41 56 41 57 48 83 EC 38 4C 8B FA 41 B8 10 27 00 00 33 D2 4C 8B F1 E8 ?? ?? ?? ?? 41 83 BE 6C 0E 00 00 FF "
      "0F 8F ?? ?? ?? ?? 49 8B 8E 08 08 00 00",
      0x79E6D0u, 0x79DEC0u },
    { "AiFxPhysPlace",
      "40 53 56 57 41 56 48 83 EC 48 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 38 48 8B DA 49 8B F1 48 8B 94 24 "
      "90 00 00 00 4D 8B F0 48 8B F9 48 85 D2",
      0x1650390u, 0x1650F30u },
    { "AiWeaponUse",
      "48 63 81 C8 50 02 00 85 C0 78 1E 48 8B 91 20 C1 00 00 48 8D 0C 80 48 C1 E1 04 48 8B 82 98 03 00 00 "
      "48 83 C0 08 48 03 C1 C3 33 C0 C3",
      0x435430u, 0x4343A0u },
    { "AiDamageWound",
      "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 F1 48 81 EC C0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
      "48 89 45 BF 48 83 B9 30 06 00 00 00 4D 8B E1 4C 63 7D 6F 4D 8B F0 48 8B F2 48 8B D9 0F 84 ?? ?? ?? ?? "
      "48 8D B9 08 06 00 00",
      0x8FC8E0u, 0x8FB910u },
    { "AiWebScalarSet",
      "48 83 EC 28 48 63 81 88 05 00 00 45 33 C0 85 C0 7E 27 4C 8B 99 80 05 00 00 4C 8B D0 4C 8B 0A 49 8B C3 "
      "41 8B D0 4C 39 08 74 16 41 FF C0 48 FF C2 48 83 C0 18 49 3B D2 7C EC 32 C0 48 83 C4 28 C3 "
      "48 8D 91 98 05 00 00 49 63 C0 48 8D 0C 40 49 8D 0C CB E8 ?? ?? ?? ?? B0 01 48 83 C4 28 C3",
      0x170A950u, 0x170B3C0u },
    { "AiUpdateRelations",
      "40 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 70 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 C8 00 00 00 "
      "48 8B F1 48 8D 59 28 C7 43 08 00 00 00 00 83 79 18 00 7D 21 48 8B 51 08 48 81 C2 C0 4E 00 00",
      0x4AA490u, 0x4A8C10u },
    { "AiBestRelation",
      "48 89 6C 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 48 8B 41 08 4C 8B F2 33 D2 0F 29 74 24 20 "
      "48 8B 88 08 08 00 00 48 8B 01 FF 90 80 00 00 00 F3 0F 10 35 ?? ?? ?? ?? 33 ED 4C 8B E0 8B FD 44 8D 7D 0E "
      "41 39 6E 08",
      0x4AAD10u, 0x4A9490u },
    { "AiFsmMove",
      "48 8B C4 57 41 54 41 55 41 56 41 57 48 81 EC A0 00 00 00 48 C7 44 24 30 FE FF FF FF 48 89 58 08 48 89 68 10 "
      "48 89 70 18 4C 8B FA 4C 8B E9 33 ED 8B F5 8B FD 89 6C 24 38 0F 57 C0 F3 0F 11 44 24 3C",
      0x64D330u, 0x64C5B0u },
    { "AiGameTime",
      "48 89 5C 24 10 57 48 83 EC 20 48 8D 99 10 D2 49 00 48 8B FA 48 8B 03 48 8D 54 24 30 41 B8 01 00 00 00 "
      "48 8B CB FF 90 D0",
      0x38DE60u, 0x38D700u },
    { "AiResetMotionCorrection",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B 01 48 8D 54 24 20 F3 0F 10 05 ?? ?? ?? ?? "
      "33 F6 80 64 24 4A FE 48 8B F9 F3 0F 11 44 24 20 48 8B 98 B0 3D 00 00 80 61 44 FC",
      0x50C800u, 0x50B530u },
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

/* aiFireMode_t and aiIdleTurn_t names in the engine's order; the sigs test compares them with the executables'
 * enum tables. */
const char *const SH_AI_FIREMODE_NAMES[SH_AI_FIREMODE_MAX + 1] = {
    "AIFIREMODE_HOLDFIRE", "AIFIREMODE_WEAPON_EXPLICIT_RELEASE", "AIFIREMODE_WEAPON", "AIFIREMODE_FIREWHENREADY",
    "AIFIREMODE_SPRAYWHENREADY", "AIFIREMODE_MAX"
};
const char *const SH_AI_IDLETURN_NAMES[SH_AI_IDLETURN_MAX + 1] = {
    "AIIDLETURN_RELAXED", "AIIDLETURN_COMBAT", "AIIDLETURN_INCOVER", "AIIDLETURN_MAX"
};

/* The pain-string stub (script_api_ai.h), entered by the 14-byte jump over the two loads at the site (rbx the
 * demon, rbp the request frame): saves the volatile state, stores fetch(demon)'s string at [rbp+0x88] when it is
 * non-NULL, runs the two loads and jumps to resume. Flags are not live there. */
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

/* The off-limits getter (script_api_ai.h): movzx eax, byte [rcx + SH_AI_OFF_OFF_LIMITS]; ret. */
int sh_ai_off_limits_getter(const uint8_t code[8])
{
    uint32_t disp;
    if (!code || code[0] != 0x0F || code[1] != 0xB6 || code[2] != 0x81 || code[7] != 0xC3) return 0;
    memcpy(&disp, code + 3, 4);
    return disp == SH_AI_OFF_OFF_LIMITS;
}
