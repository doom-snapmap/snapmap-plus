/* Independently installable map-load and spawn guards. Each repairs an invalid
 * precondition before calling the engine. Install once with a resolved module
 * base; repeat calls are harmless. Return 1 if armed, 0 on failure. */
#ifndef SHIELD_MAPLOAD_GUARDS_H
#define SHIELD_MAPLOAD_GUARDS_H

#include <stdint.h>
#include "../backend/signatures.h"

/* results/count are the startup signature results (NULL: none); a target they
 * do not hold is resolved alone, never by rescanning the whole database. */

/* Reset unreadable or inconsistent event-link lists before their next walk. */
int sh_evwire_guard_install(const uint8_t *module_base, const sig_result *results, size_t count);

/* Clear tags before Spawn when their required subsystem is unreadable. */
int sh_interactable_guard_install(const uint8_t *module_base, const sig_result *results, size_t count);

#endif /* SHIELD_MAPLOAD_GUARDS_H */
