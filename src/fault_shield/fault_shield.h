/* fault_shield.h -- the merged fault-shield's install entry (called from the backend's bootstrap). */
#ifndef FAULT_SHIELD_H
#define FAULT_SHIELD_H

#include <stdint.h>
#include <stddef.h>
#include "../backend/signatures.h"

/* Install recovery and crash capture with the host module range and the
 * bootstrap's signature results (resolved before any detour). A NULL base is
 * a no-op. Call outside the loader lock. */
void shield_install(uint8_t *doom_base, size_t doom_size, const sig_result *results, size_t count);

#endif /* FAULT_SHIELD_H */
