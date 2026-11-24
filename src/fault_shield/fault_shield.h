/* fault_shield.h -- the merged fault-shield's install entry (called from the backend's bootstrap). */
#ifndef FAULT_SHIELD_H
#define FAULT_SHIELD_H

#include <stdint.h>
#include <stddef.h>
#include "../backend/signatures.h"

/* Install recovery and crash capture from the host module range and signature results; a NULL base is a
 * no-op, and the call must be outside the loader lock. Returns 1 when the fault handler and main-frame hook
 * are installed. */
int shield_install(uint8_t *doom_base, size_t doom_size, const sig_result *results, size_t count);

#endif /* FAULT_SHIELD_H */
