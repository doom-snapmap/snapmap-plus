/* particlemotion_sigs.c -- the private signatures of particlemotion.c (the particle vertex writer and the
 * stage's origin function). Kept apart so sig_test and detour_windows_test link the table without the engine
 * side. Order matches particlemotion.c. */
#include <stddef.h>
#include "particlemotion.h"

/* Engine bindings, each recorded in both build 13954591 executables. */
const sig_entry SH_PARTICLEMOTION_SIGNATURES[] = {
    /* One CPU particle's vertices: (stage context, the particle's generation record, the output cursor) -> the
     * vertices written. Calls the origin function, the colour and the quad builder of the stage's orientation. */
    { "ParticleWriteVerts",
      "48 8B C4 4C 89 40 18 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 88 48 81 EC 38 01 00 00 4C 8B 79 08",
      0x19AFCE0u, 0x19A3FB0u },
    /* A particle's origin at its age: (out velocity, stage context, generation record, out origin). */
    { "ParticleOrigin",
      "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 88 48 81 EC 78 01 00 00 0F 29 70 A8 0F 29 78 98 "
      "44 0F 29 40 88",
      0x19B8080u, 0x19AC350u },
    { NULL, NULL, 0, 0 }
};
