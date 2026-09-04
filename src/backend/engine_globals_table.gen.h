/* engine_globals_table.gen.h -- GENERATED. Do not edit by hand.
 *
 * Each entry signs the CODE SITE that computes a data global's address and records
 * where the RIP-relative displacement sits inside that pattern. Resolving reads the
 * displacement from the live image, so the same table serves any DOOM build whose
 * code shape matches -- no per-build address list exists or is wanted.
 *
 * Every anchor below was verified to match EXACTLY ONCE on both shipped executables
 * (DOOMx64vk.exe and DOOMx64.exe), and each address was cross-checked against
 * independent reference sites. Regenerate with the project derivation tooling; never
 * hand-edit a pattern.
 */
#ifndef BACKEND_ENGINE_GLOBALS_TABLE_GEN_H
#define BACKEND_ENGINE_GLOBALS_TABLE_GEN_H

#include "engine_globals.h"

const global_entry BACKEND_ENGINE_GLOBALS[] = {
    { "cmd_system_slot",
      /* idCmdSystem singleton pointer.
       * Anchor: mov rcx, qword ptr [rip + 0x5290195] at Vulkan 0x3270e4 (unique on both images).
       * Resolves to Vulkan 0x55B7280 / OpenGL 0x3eb98a0; 6 of 6 reference sites agreed. */
      "48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 50 50 48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 01 48 83 C4 28",
      3, 0, 0x55B7280u },

    { "cvar_system_slot",
      /* idCvarSystem singleton pointer (cmdSystem + 0x10).
       * Anchor: mov rcx, qword ptr [rip + 0x529c5e4] at Vulkan 0x31aca5 (unique on both images).
       * Resolves to Vulkan 0x55B7290 / OpenGL 0x3eb98b0; 6 of 6 reference sites agreed. */
      "48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 45 33 C0 0F 28 F0 48 8B 01 FF 50 60 33 C9 F3 0F 11 73 08 0F 28 74 24 40 88 43 1C 48 8B C3 F3 0F 11 7B 0C 0F 28 7C 24 30 F3 44 0F 11 43 10 44 0F 28 44 24 20 89 0B 48 89 4B 14",
      3, 0, 0x55B7290u },

    { "decl_resource_head",
      /* head of the engine decl resource list-of-lists.
       * Anchor: mov qword ptr [rip + 0x4a17968], rsi at Vulkan 0x1800621 (unique on both images).
       * Resolves to Vulkan 0x6217F90 / OpenGL 0x4b162a0; 6 of 6 reference sites agreed. */
      "48 89 35 ?? ?? ?? ?? 48 89 6E 08 4C 89 76 10 48 8B C6 48 8B 8C 24 40 08 00 00 48 33 CC",
      3, 0, 0x6217F90u },

    { "decl_visibility_manager",
      /* decl visibility manager pointer.
       * Anchor: mov rcx, qword ptr [rip + 0x52d099d] at Vulkan 0x2866ec (unique on both images).
       * Resolves to Vulkan 0x5557090 / OpenGL 0x3e59350; 6 of 6 reference sites agreed. */
      "48 8B 0D ?? ?? ?? ?? 48 8B 01 BA 0F 00 00 00 FF 90 B0 00 00 00 44 89 6C 24 30 48 8B 96 90 01 00 00",
      3, 0, 0x5557090u },

    { "editor_singleton",
      /* idSnapEditorLocal, constructed in place.
       * Anchor: lea rcx, [rip + 0x10cf6f6] at Vulkan 0x1f8704b (unique on both images).
       * Resolves to Vulkan 0x3056748 / OpenGL 0x309b588; 2 of 2 reference sites agreed. */
      "48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 83 C4 28",
      3, 0, 0x3056748u },

    { "error_state",
      /* error state, cleared during shield recovery.
       * Anchor: movzx eax, byte ptr [rip + 0x53e18a5] at Vulkan 0x19fc8f0 (unique on both images).
       * Resolves to Vulkan 0x6DDE19C / OpenGL 0x56e4a0c; 3 of 3 reference sites agreed. */
      "0F B6 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 83 EC 28 48 83 3D ?? ?? ?? ?? 00 74 15",
      3, 0, 0x6DDE19Cu },

    { "game_manager_slot",
      /* game manager singleton pointer.
       * Anchor: mov rax, qword ptr [rip + 0x541d5b7] at Vulkan 0x2e25d2 (unique on both images).
       * Resolves to Vulkan 0x56FFB90 / OpenGL 0x4002188; 6 of 6 reference sites agreed. */
      "48 8B 05 ?? ?? ?? ?? 48 63 CB 48 8D 14 49 48 8B CE 48 C1 E2 04 48 03 90 E0 53 04 00",
      3, 0, 0x56FFB90u },

    { "last_error_msg",
      /* last error message buffer (0x800 bytes).
       * Anchor: lea rcx, [rip + 0x5fd163d] at Vulkan 0xe0c34c (unique on both images).
       * Resolves to Vulkan 0x6DDD990 / OpenGL 0x56e4200; 6 of 6 reference sites agreed. */
      "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D A1 E8 ?? ?? ?? ?? CC 41 B8 00 08 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D A1 E8 ?? ?? ?? ?? 90 48 8B 4D F0 48 33 CC E8 ?? ?? ?? ??",
      3, 0, 0x6DDD990u },

    { "load_state",
      /* map load state; the Play detector.
       * Anchor: mov dword ptr [rip + 0x53e1862], ecx at Vulkan 0x19fc930 (unique on both images).
       * Resolves to Vulkan 0x6DDE198 / OpenGL 0x56e4a08; 1 of 1 reference sites agreed. */
      "89 0D ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC E9 ?? ?? ?? ?? CC",
      2, 0, 0x6DDE198u },

    { "main_thread_id",
      /* engine main-thread id (DWORD).
       * Anchor: cmp qword ptr [rip + 0x53e1876], rax at Vulkan 0x19fc913 (unique on both images).
       * Resolves to Vulkan 0x6DDE190 / OpenGL 0x56e4a00; 3 of 3 reference sites agreed. */
      "48 39 05 ?? ?? ?? ?? 74 07 32 C0 48 83 C4 28 C3 B0 01",
      3, 0, 0x6DDE190u },

    { "material_manager_ctx",
      /* material type-manager context.
       * Anchor: lea rcx, [rip + 0x5737654] at Vulkan 0x286375 (unique on both images).
       * Resolves to Vulkan 0x59BD9D0 / OpenGL 0x42bbce0; 6 of 6 reference sites agreed. */
      "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 89 86 F0 1B 00 00 48 8B 86 E8 1B 00 00 48 89 86 E0 1B 00 00 41 B0 01",
      3, 0, 0x59BD9D0u },

    { "palette_vtable",
      /* editor palette vtable (.rdata).
       * Anchor: lea rcx, [rip + 0x1b2eea1] at Vulkan 0x51aaf8 (unique on both images).
       * Resolves to Vulkan 0x20499A0 / OpenGL 0x20615a0; 1 of 1 reference sites agreed. */
      "48 8D 0D ?? ?? ?? ?? 48 89 08 C7 40 20 00 00 05 00 4C 89 70 10 4C 89 70 18 C7 40 40 00 00 05 00 4C 89 70 30 4C 89 70 38 C7 40 58 00 00 05 00 4C 89 70 48 4C 89 70 50 48 8D 87 C0 06 02 00 48 89 44 24 68",
      3, 0, 0x20499A0u },

    { "provider_vtable",
      /* resource provider vtable, 31 slots (.rdata).
       * Anchor: lea rax, [rip + 0xd4740e] at Vulkan 0x1a5108b (unique on both images).
       * Resolves to Vulkan 0x27984A0 / OpenGL 0x27ae6c0; 1 of 1 reference sites agreed. */
      "48 8D 05 ?? ?? ?? ?? 48 89 01 33 FF C7 41 18 00 00 33 00 48 89 79 08 48 89 79 10",
      3, 0, 0x27984A0u },

    { "render_world_slot",
      /* idRenderWorld singleton pointer.
       * Anchor: lea r8, [rip + 0x49ef91a] at Vulkan 0xd31dcf (unique on both images).
       * Resolves to Vulkan 0x57216F0 / OpenGL 0x4023cf0; 6 of 6 reference sites agreed. */
      "4C 8D 05 ?? ?? ?? ?? 48 8B D7 48 8B CB F2 48 0F 2A C0 48 8B 45 70 48 89 44 24 28",
      3, 0, 0x57216F0u },

    { "resource_manager_ctx",
      /* resource type-manager context.
       * Anchor: lea rcx, [rip + 0x56cd397] at Vulkan 0x2f0552 (unique on both images).
       * Resolves to Vulkan 0x59BD8F0 / OpenGL 0x42bbc00; 6 of 6 reference sites agreed. */
      "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 89 86 B8 A0 29 00 4C 89 B6 08 5C 28 00 C7 86 10 5C 28 00 00 00 80 BF 4C 89 B6 CC A1 29 00",
      3, 0, 0x59BD8F0u },

    { "shell_ptr_slot",
      /* menu shell pointer slot.
       * Anchor: mov rcx, qword ptr [rip + 0x4b231c2] at Vulkan 0x2d4dff (unique on both images).
       * Resolves to Vulkan 0x4DF7FC8 / OpenGL 0x36fa4c8; 6 of 6 reference sites agreed. */
      "48 8B 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 84 24 C8 00 00 00 48 8D 54 24 30 C7 84 24 D0 00 00 00 D8 01 00 00 C6 84 24 D4 00 00 00 00",
      3, 0, 0x4DF7FC8u },

    { "throw_suppressor_a",
      /* level-6 dispatcher throw gate A.
       * Anchor: cmp dword ptr [rip + 0x5e8af2f], r12d at Vulkan 0x11248ea (unique on both images).
       * Resolves to Vulkan 0x6FAF820 / OpenGL 0x58b6090; 6 of 6 reference sites agreed. */
      "44 39 25 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 4C 89 7C 3B 10 83 CA FF 49 8B 8E 08 08 00 00 48 8B 01",
      3, 0, 0x6FAF820u },

    { "throwinfo_fatal",
      /* ThrowInfo for the fatal level-7 throw (.rdata).
       * Anchor: lea rdx, [rip + 0x13e40a2] at Vulkan 0x1a098e7 (unique on both images).
       * Resolves to Vulkan 0x2DED990 / OpenGL 0x2e17df0; 1 of 1 reference sites agreed. */
      "48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 30 E8 ?? ?? ?? ?? CC 41 B8 00 08 00 00 48 8D 95 B0 05 00 00",
      3, 0, 0x2DED990u },

    { "throwinfo_recoverable",
      /* ThrowInfo for the recoverable level-6 throw (.rdata).
       * Anchor: lea rdx, [rip + 0x1fe1331] at Vulkan 0xe0c358 (unique on both images).
       * Resolves to Vulkan 0x2DED690 / OpenGL 0x2e17af0; 6 of 6 reference sites agreed. */
      "48 8D 15 ?? ?? ?? ?? 48 8D 4D A1 E8 ?? ?? ?? ?? CC 41 B8 00 08 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D A1 E8 ?? ?? ?? ?? 90 48 8B 4D F0",
      3, 0, 0x2DED690u },

    { "type_container",
      /* reflection type container.
       * Anchor: lea rdx, [rip + 0x26bb106] at Vulkan 0x9c7a03 (unique on both images).
       * Resolves to Vulkan 0x3082B10 / OpenGL 0x30c78e0; 1 of 1 reference sites agreed. */
      "48 8D 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 3D ?? ?? ?? ?? 48 89 7C 24 30 48 8D 1D ?? ?? ?? ?? 48 89 5C 24 28",
      3, 0, 0x3082B10u },

    { "validator_manager",
      /* reflection validator manager pointer.
       * Anchor: mov rcx, qword ptr [rip + 0x4b8de9e] at Vulkan 0x26b7a3 (unique on both images).
       * Resolves to Vulkan 0x4DF9648 / OpenGL 0x36fb9d0; 6 of 6 reference sites agreed. */
      "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 40 02 00 00 48 8B C8 48 89 5C 24 28 48 8D 1D ?? ?? ?? ??",
      3, 0, 0x4DF9648u },

    { NULL, NULL, 0, 0, 0 }
};

#endif /* BACKEND_ENGINE_GLOBALS_TABLE_GEN_H */
