/* Engine constants of the one supported DOOM build: the April 2024 update,
 * Steam build 13954591 (host_image.h identifies it by executable SHA-256).
 * Every value here comes from the build 13954591 address table (snaphak-re
 * R-008): verified on both renderers, or verified unchanged from 2018.
 *
 * Raw RVAs come as Vulkan/OpenGL pairs because the two renderer executables are
 * separate links; SH_RVA picks the host's renderer and yields 0 on any other
 * executable, so a raw address is never applied to an unrecognized image. */
#ifndef BACKEND_ENGINE_BUILD_H
#define BACKEND_ENGINE_BUILD_H

#include <stdint.h>
#include "host_image.h"

#define SH_RVA(vk, gl) sh_host_rva((uint32_t)(vk), (uint32_t)(gl))

/* idCVar: valueGameTime/valueMicroseconds were inserted at +0x40 on this build,
 * so name, flags and every later field sit 0x10 later than in 2018. */
#define SH_IDCVAR_VALUE_INT          0x30u
#define SH_IDCVAR_NAME               0x50u
#define SH_IDCVAR_FLAGS              0x68u

/* idCVarSystemLocal list/hash layout (registration, lookup, developer-table
 * unlock): verified unchanged from 2018. */
#define SH_CVARSYS_LAYOUT_ESTABLISHED 1

/* The idCVar name offset for registry lookups, or 0 while the registry layout
 * those lookups walk is not established (engine_cvar_read.h then refuses). */
#define SH_IDCVAR_NAME_FOR_LOOKUP (SH_CVARSYS_LAYOUT_ESTABLISHED ? SH_IDCVAR_NAME : 0u)

/* idFile has 34 slots: three were inserted at +0x18, +0x50 and +0xF0, so the
 * idStr helpers sit at +0xF8/+0x100/+0x108. The file-system provider's
 * open-by-name slot moved from +0xF8 to +0x148. */
#define SH_IDFILE_SLOTS              34u
#define SH_IDFILE_READ_STRING_SLOT   0xF8u
#define SH_IDFILE_COMPARE_SLOT       0x100u
#define SH_IDFILE_WRITE_STRING_SLOT  0x108u
#define SH_PROVIDER_OPEN_SLOT        0x148u

/* Session byte read by SessionDevModeGetter (movzx eax, byte [rcx+disp32]). */
#define SH_SESSION_DEVMODE_FIELD     0x349BBu

/* Validator-manager reflection getter slot. */
#define SH_VALIDATOR_REFLECT_SLOT    0x258u

/* AllocateGameResources / UnloadGameResources / FinalizeMapChange interior
 * layout that map_transition.c verifies and calls into (TR_* there). */
#define SH_MAP_TRANSITION_LAYOUT_ESTABLISHED 1

/* GetGameSystemInterface is no longer exported. The idGameSystemLocal object is
 * reached through a statically initialised, relocated pointer at these RVAs. */
#define SH_GAME_SYSTEM_SLOT_RVA_VK   0x2F48600u
#define SH_GAME_SYSTEM_SLOT_RVA_GL   0x2F905D8u

/* Address of the game-system pointer slot in a supported image, or 0. */
static __inline uintptr_t sh_engine_game_system_slot(const uint8_t *base)
{
    uint32_t rva = sh_host_image_rva(base, SH_GAME_SYSTEM_SLOT_RVA_VK, SH_GAME_SYSTEM_SLOT_RVA_GL);
    return rva ? (uintptr_t)base + rva : 0;
}

/* Features that use engine offsets the build 13954591 address table leaves to a
 * live check (snaphak-re live-checks-13954591.json, results in
 * work/daemon/live-checks-results.json). Such a feature is off by default and
 * logs which values it waits for. It is not a permanent switch: starting DOOM
 * with SNAPMAP_PLUS_LIVE_VERIFY=1 in its environment runs every pending feature
 * (logged as VERIFY), so the live lane can exercise and confirm it without a
 * rebuild. Each feature keeps its own runtime byte or shape checks either way.
 * When a check is confirmed, set its flag to 0. The _WHY string names the
 * unconfirmed values. */
static __inline int sh_live_verify(void)
{
    char value[4];
    DWORD n = GetEnvironmentVariableA("SNAPMAP_PLUS_LIVE_VERIFY", value, (DWORD)sizeof value);
    return n == 1 && value[0] == '1';
}
#define SH_PENDING_ON(flag) ((flag) && !sh_live_verify())

#define SH_PENDING_APPLY_PASTE        1
#define SH_PENDING_APPLY_PASTE_WHY    "substate paste bit +0x41 / +0x42, paste action id 0x5c (apply_engine.c); staged-only placement remains"
#define SH_PENDING_DECL_SERVER        1
#define SH_PENDING_DECL_SERVER_WHY    "decl outputs count +0x448, decl field +0xac (decl_server.c)"
#define SH_PENDING_ENGINE_DIALOG      1
#define SH_PENDING_ENGINE_DIALOG_WHY  "dialog parameter +0x26, actions 0x4a/0x4b match 2018 in code; the prompt reaching the screen is unconfirmed (engine_dialog.c)"
#define SH_PENDING_GRID_PROPERTIES    1
#define SH_PENDING_GRID_PROPERTIES_WHY "properties action 0x50, editor field +0xac (grid_room_editor.c)"
#define SH_PENDING_IFACE_MODULES      1
#define SH_PENDING_IFACE_MODULES_WHY  "module table stride 0x98, name +0x48 (iface_engine.c)"
#define SH_PENDING_MAP_PUBLISHED      1
#define SH_PENDING_MAP_PUBLISHED_WHY  "launch parameters +0x600 (map_published.c)"
#define SH_PENDING_RENDER_EDITOR      1
#define SH_PENDING_RENDER_EDITOR_WHY  "settings panel slider +0x120 (map_render_editor.c)"
#define SH_PENDING_NAV_MENU           1
#define SH_PENDING_NAV_MENU_WHY       "menu clone +0x74/+0x78, entry stride 0x58, matrix +0xc (nav_menu.c)"
#define SH_PENDING_RESIDENT           1
#define SH_PENDING_RESIDENT_WHY       "renderer count +0x14, world-text fonts +0x688 (resource_resident.c)"
#define SH_PENDING_SWF_TEXTEDIT       1
#define SH_PENDING_SWF_TEXTEDIT_WHY   "text input max chars +0x280 (swf_textedit.c)"
#define SH_PENDING_TARGET_ANY         1
#define SH_PENDING_TARGET_ANY_WHY     "decl list +0x20/+0x28 (target_any.c)"
#define SH_PENDING_FAULT_SHIELD       1
#define SH_PENDING_FAULT_SHIELD_WHY   "editor exit flag +0x23294, shell dialog-visible +0xa8 while a dialog is up (engine_layout.h)"

#endif /* BACKEND_ENGINE_BUILD_H */
