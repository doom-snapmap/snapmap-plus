/* postfx_sigs.c -- the private signatures of postfx.c (the radial blur item's start, SetBlurRadial, the post
 * process and the parm store). Kept apart so sig_test and detour_windows_test link the table without the
 * engine side. Order matches postfx.c. */
#include <stddef.h>
#include "postfx.h"

/* Engine bindings, each recorded in both build 13954591 executables. */
const sig_entry SH_POSTFX_SIGNATURES[] = {
    /* The fx item update's FX_BLUR_RADIAL start: the radius load, its stores and the view call (vtable +0x70). */
    { "FxBlurRadialSite",
      "F3 41 0F 10 86 68 04 00 00 F3 0F 11 44 24 30 89 5C 24 28 89 74 24 20 44 8B CF 44 8B 45 20 48 8D 54 24 78 FF 50 70",
      0x16F40DAu, 0x16F4A5Au },
    /* idView SetBlurRadial(view, const idVec3 *centre, int start, int fadeIn, int fadeOut, int duration, float r). */
    { "SetBlurRadial",
      "48 89 5C 24 10 66 C7 81 5C 11 00 00 01 00 4C 8B D9 8B 02 45 8B D0",
      0xE684B0u, 0xE679A0u },
    /* The post process: Vulkan (frame, viewDef, exposure, async) and OpenGL (frame, viewDef, exposure). */
    { "RenderPostProcessVk",
      "48 8B C4 48 89 58 10 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 88 FD FF FF 48 81 EC 40 03 00 00 0F 29 70 B8 "
      "0F 29 78 A8 44 0F 29 40 98 44 0F 29 48 88 44 0F 29 90 78 FF FF FF 44 0F 29 98 68 FF FF FF 44 0F 29 A0 58 FF FF "
      "FF 44 0F 29 A8 48 FF FF FF 44 0F 29 B0 38 FF FF FF 44 0F 29 B8 28 FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 "
      "85 90 01 00 00 4C 8B 31",
      0x186C970u, 0 },
    { "RenderPostProcessGl",
      "48 8B C4 48 89 58 10 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 48 FE FF FF 48 81 EC 80 02 00 00 0F 29 70 B8 "
      "0F 29 78 A8 44 0F 29 40 98 44 0F 29 48 88 44 0F 29 90 78 FF FF FF 44 0F 29 98 68 FF FF FF 44 0F 29 A0 58 FF FF "
      "FF 44 0F 29 A8 48 FF FF FF 44 0F 29 B0 38 FF FF FF 44 0F 29 B8 28 FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 "
      "85 D0 00 00 00 F2 0F 10 81 C0",
      0, 0x186C7B0u },
    /* The parm state's vec4 store: (state, idDeclRenderParm *, const float[4]). */
    { "RenderParmSetVec4",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 63 42 68 49 8B F0 48 8B FA 48 8B D9 44 8B 8C 81 ?? ?? 00 00 "
      "44 85 89 ?? ?? 00 00 74 07 B2 01 E8",
      0x18F6F90u, 0x18ECF60u },
    { NULL, NULL, 0, 0 }
};
