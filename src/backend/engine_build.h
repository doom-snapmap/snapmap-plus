/* Engine constants of the one supported DOOM build, Steam build 13954591 (host_image.h identifies it by executable
 * SHA-256). Raw RVAs come as Vulkan/OpenGL pairs; SH_RVA picks the host's renderer and yields 0 on any other
 * executable, so a raw address is never applied to an unrecognized image. */
#ifndef BACKEND_ENGINE_BUILD_H
#define BACKEND_ENGINE_BUILD_H

#include <stdint.h>
#include "host_image.h"

#define SH_RVA(vk, gl) sh_host_rva((uint32_t)(vk), (uint32_t)(gl))

/* idCVar field offsets; valueGameTime/valueMicroseconds sit at +0x40, ahead of name and flags. */
#define SH_IDCVAR_VALUE_INT          0x30u
#define SH_IDCVAR_NAME               0x50u
#define SH_IDCVAR_FLAGS              0x68u
#define SH_IDCVAR_NEXT               0x90u   /* static registration chain link */

/* Nonzero when the idCVarSystemLocal list/hash layout (registration, lookup, developer-table unlock) is established. */
#define SH_CVARSYS_LAYOUT_ESTABLISHED 1

/* The idCVar name offset for registry lookups, or 0 while the registry layout
 * those lookups walk is not established (engine_cvar_read.h then refuses). */
#define SH_IDCVAR_NAME_FOR_LOOKUP (SH_CVARSYS_LAYOUT_ESTABLISHED ? SH_IDCVAR_NAME : 0u)

/* idFile has 34 slots; the idStr helpers sit at +0xF8/+0x100/+0x108 and the file-system provider's open-by-name
 * slot is +0x148. */
#define SH_IDFILE_SLOTS              34u
#define SH_IDFILE_READ_STRING_SLOT   0xF8u
#define SH_IDFILE_COMPARE_SLOT       0x100u
#define SH_IDFILE_WRITE_STRING_SLOT  0x108u
#define SH_PROVIDER_OPEN_SLOT        0x148u

/* Session byte read by SessionDevModeGetter (movzx eax, byte [rcx+disp32]). */
#define SH_SESSION_DEVMODE_FIELD     0x349BBu

/* Validator-manager reflection getter slot. */
#define SH_VALIDATOR_REFLECT_SLOT    0x258u

/* GetGameSystemInterface is not exported; the idGameSystemLocal object is reached through a statically
 * initialised, relocated pointer at these RVAs. */
#define SH_GAME_SYSTEM_SLOT_RVA_VK   0x2F48600u
#define SH_GAME_SYSTEM_SLOT_RVA_GL   0x2F905D8u

/* Address of the game-system pointer slot in a supported image, or 0. */
static __inline uintptr_t sh_engine_game_system_slot(const uint8_t *base)
{
    uint32_t rva = sh_host_image_rva(base, SH_GAME_SYSTEM_SLOT_RVA_VK, SH_GAME_SYSTEM_SLOT_RVA_GL);
    return rva ? (uintptr_t)base + rva : 0;
}

/* Layout constants bound without a gate on both renderers: editor paste arming (+0x209E0/+0x209E4, mode +0x420/+0x424
 * = 0x5C), decl server outputs count +0x448, native dialog params +0x98, grid-room properties action 0x50 (+0xAC),
 * module name stride 0x98 (name +0x48), Start menu row stride 0x58, SWF field cap +0x280, fault shield exit flag
 * editor+0x23294, visibility-toggle registry +0x20/+0x28, render settings slider +0x120 and dirty byte +0xc8. */

#endif /* BACKEND_ENGINE_BUILD_H */
