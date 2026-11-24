/* Independently installable map-load and spawn guards. Each repairs an invalid
 * precondition before calling the engine. Install once with a resolved module
 * base and the bootstrap's signature results (no rescan); repeat calls are
 * harmless. Return 1 if armed, 0 on failure. */
#ifndef SHIELD_MAPLOAD_GUARDS_H
#define SHIELD_MAPLOAD_GUARDS_H

#include <stdint.h>
#include <stddef.h>
#include "../backend/signatures.h"

/* Reset unreadable or inconsistent event-link lists before their next walk. */
int sh_evwire_guard_install(const uint8_t *module_base, const sig_result *results, size_t count);

/* Clear tags before Spawn when their required subsystem is unreadable. */
int sh_interactable_guard_install(const uint8_t *module_base, const sig_result *results, size_t count);

/* The installs' own stolen-prologue checks (EventLink, InteractableSpawn), for the build-image test:
 * 1 when the bytes at the function are the expected ones. */
int sh_evlink_prologue_ok(const uint8_t *target);
int sh_interactable_prologue_ok(const uint8_t *target);

#endif /* SHIELD_MAPLOAD_GUARDS_H */
