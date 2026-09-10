/* fault_shield.h -- the merged fault-shield's install entry (called from the backend's bootstrap). */
#ifndef FAULT_SHIELD_H
#define FAULT_SHIELD_H

#include <stdint.h>
#include <stddef.h>

/* Install recovery and crash capture after the backend's decryption wait, using
 * its resolved module range. May wait up to 10 seconds for instrumentation.
 * A NULL base is a no-op. Call outside the loader lock. */
void shield_install(uint8_t *doom_base, size_t doom_size);

#endif /* FAULT_SHIELD_H */
