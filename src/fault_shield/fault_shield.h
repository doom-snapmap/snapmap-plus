/* fault_shield.h -- the merged fault-shield's install entry (called from the backend's bootstrap). */
#ifndef FAULT_SHIELD_H
#define FAULT_SHIELD_H

#include <stdint.h>
#include <stddef.h>
#include "../backend/signatures.h"

extern uint8_t *g_doom_base;
extern size_t g_doom_size;

/* Resolve an address without acquiring a module reference. The supplied path buffer keeps crash-handler stacks bounded. */
int shield_module_at(const void *address, wchar_t *path, size_t path_cap, char *name, size_t name_cap, uintptr_t *rva);

/* Install recovery and crash capture from the host module range and signature results; a NULL base is a
 * no-op, and the call must be outside the loader lock. Returns 1 when the fault handler and main-frame hook
 * are installed. */
int shield_install(uint8_t *doom_base, size_t doom_size, const sig_result *results, size_t count);

#endif /* FAULT_SHIELD_H */
