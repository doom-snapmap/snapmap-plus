/* modscene_sigs.c -- the private signatures of modscene_place.c's scene spawns
 * and the sync-start detour that times them.
 * Kept apart so the two-image test (tests/modscene_decl_test.c) links the
 * table without the engine side. Order matches modscene_place.c. */
#include <stddef.h>
#include "modscene_decl.h"

const sig_entry SH_MODSCENE_SPAWN_SIGNATURES[] = {
    { "SceneCreateEntity",   /* idGameLocal vtable +0x448: CreateManagedClassFromDecl, +0x440 edit state; not spawned */
      "40 56 57 41 56 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 58 48 89 6C 24 60 49 8B E8 "
      "48 8B F2 4C 8B F1 48 8B 1D ?? ?? ?? ?? 48 89 5C 24 50 48 89 0D ?? ?? ?? ?? 48 85 D2 74 55 "
      "48 8B CA E8 ?? ?? ?? ?? F6 80 90 00 00 00 20 75 12",
      0x384DF0u, 0x384690u },
    { "SceneFinishSpawn",    /* idGameLocal vtable +0x458: register the object, then idClass::CallSpawn */
      "44 89 4C 24 20 41 56 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 50 48 89 6C 24 58 "
      "48 89 7C 24 60 45 8B F0 48 8B FA 48 8B E9 48 83 7C 24 78 00 74 2D 48 8B CA E8 ?? ?? ?? ?? "
      "48 8B D8 4C 8B 0F 48 8B CF 41 FF 91 E0 00 00 00",
      0x3BCF10u, 0x3BC870u },
    { "SceneSyncStart",      /* the interaction link's sync-start helper: spawn the sync, place it at the
                                owner, start it, then store it at this + 0x8D0 (detoured, 17 bytes) */
      "48 89 5C 24 10 55 56 41 54 41 56 41 57 48 83 EC 50 48 8B B4 24 A0 00 00 00 4C 8B F2 49 8B 51 10 "
      "4C 8B F9 49 8B D9 4D 8B E0 48 8D 8E 58 37 04 00 E8 ?? ?? ?? ?? 48 8B E8 48 85 C0 0F 84 8E 01 00 00 "
      "83 3D ?? ?? ?? ?? 00 48 89 BC 24 80 00 00 00 74 57 48 8B 53 10 48 8D 0D ?? ?? ?? ?? 45 33 C0 "
      "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B D8 48 8B 39 E8 ?? ?? ?? ?? 44 8B 0D ?? ?? ?? ?? F6 D8 "
      "48 8B D3 1B C9 41 83 C8 FF F7 D9 FF C9 89 4C 24 20 48 8B 0D ?? ?? ?? ?? FF 97 50 04 00 00 "
      "48 8B C8 E8 ?? ?? ?? ?? 48 8B E8",
      0x6BC7B0u, 0x6BBA80u },
    { NULL, NULL, 0, 0 }
};
