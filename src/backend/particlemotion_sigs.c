/* particlemotion_sigs.c -- the signatures of particlemotion.c's engine functions, in a file of their own so tests
 * link the table without the engine side. */
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
    /* The stage's context: (job, sorted, the model's origin, its axes, the fx item's scale, ..., the context). A
     * stage built in its model's axes leaves the scale out of the context (VK 0x1419BA6F0, read). */
    { "ParticleContext",
      "48 83 EC 18 41 8B 01 4C 8B D9 4C 8B 54 24 58 48 8B 4C 24 48 41 89 42 24 41 8B 41 04 41 89 42 28 41 8B 41 08 "
      "41 89 42 2C 41 8B 41 0C 41 89 42 30",
      0x19BA6F0u, 0x19AE9C0u },
    { NULL, NULL, 0, 0 }
};
