/* cachewrites_sigs.c -- the signature of the engine's atomic write, kept apart from the engine side. */
#include <stddef.h>
#include "signatures.h"
#include "cachewrites.h"

const sig_entry SH_CACHEWRITES_SIGNATURES[] = {
    /* AtomicWrite(path relative to the base path or absolute, path type, buffer, length): writes a temp file beside
     * the target and renames it into place. */
    { "AtomicWrite",
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 58 BE FF FF B8 A8 42 00 00 E8",
      0x2A03E0u, 0x2A05A0u },
    { NULL, NULL, 0, 0 }
};
