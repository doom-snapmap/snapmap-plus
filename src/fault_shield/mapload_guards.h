/* Independently installable map-load and spawn guards. Each repairs an invalid
 * precondition before calling the engine. Install once with a resolved module
 * base; repeat calls are harmless. Return 1 if armed, 0 on failure. */
#ifndef SHIELD_MAPLOAD_GUARDS_H
#define SHIELD_MAPLOAD_GUARDS_H

#include <stdint.h>

/* Reset unreadable or inconsistent event-link lists before their next walk. */
int sh_evwire_guard_install(const uint8_t *module_base);

/* Clear tags before Spawn when their required subsystem is unreadable. */
int sh_interactable_guard_install(const uint8_t *module_base);

#endif /* SHIELD_MAPLOAD_GUARDS_H */
