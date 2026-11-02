/* companion_sigs.c -- the private signatures of companion_engine.c.
 * Kept apart so the two-image test (tests/companion_sig_test.c) links the
 * table without the engine side. Order matches companion_engine.c's enum. */
#include <stddef.h>
#include "companion_engine.h"

const sig_entry SH_COMPANION_SIGNATURES[] = {
    { "CompanionCallSpawn",      /* idClass::CallSpawn: push rbx; GetType; walk the type chain */
      "40 53 48 83 EC 40 48 8B 01 48 8B D9 FF 10 48 8B D3 48 8D 4C 24 20 4C 8B C0 E8",
      0x1583A00u, 0x1584A90u },
    { "CompanionEntityBind",     /* idEntity::Bind(master, flags): "Tried to bind an object to itself." */
      "40 53 55 56 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 68 41 8B E8 48 8B F2 "
      "48 8B D9 48 3B D1 0F 84 ?? ?? ?? ??",
      0x818D60u, 0x817EB0u },
    { "CompanionAddTarget",      /* idEntity::AddTarget(entity): grow targets (+0x3F0), set the last idEntityPtr */
      "48 89 5C 24 10 57 48 83 EC 20 48 8D 99 F0 03 00 00 48 8B FA 8B 43 0C 48 89 5C 24 30 "
      "39 43 08 75 ?? 48 8B CB E8 ?? ?? ?? ??",
      0x818210u, 0x817340u },
    { "CompanionSpawnEntityDef", /* game vtable +0x450: worker(gm, decl, num, id, arg5, 1, 4), then the idEntity cast */
      "48 83 EC 48 8B 44 24 70 C7 44 24 30 04 00 00 00 C6 44 24 28 01 89 44 24 20 E8 ?? ?? ?? ?? "
      "48 8B C8 E8 ?? ?? ?? ?? 48 83 C4 48 C3",
      0x3BCFF0u, 0x3BC950u },
    { "CompanionIsClient",       /* idGameLocal::IsClient: "frameParms.isClient %d common->IsClient %d ..." */
      "40 55 48 83 EC 40 83 3D ?? ?? ?? ?? 00 48 8B E9 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
      "48 89 5C 24 50 48 89 74 24 58 48 89 7C 24 60 48 8B 01 4C 89 74 24 38 4C 89 7C 24 30 "
      "FF 90 A0 01 00 00 45 33 F6",
      0x398370u, 0x397C80u },
    { NULL, NULL, 0, 0 }
};
