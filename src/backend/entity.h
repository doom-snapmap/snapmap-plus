/* Engine dependencies for entity, spawn-position, map-export, and cheat commands.
 * Vtable slots and object offsets require validation when porting engine builds. */
#ifndef BACKEND_B2_ENTITY_H
#define BACKEND_B2_ENTITY_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Resolve the game-manager pointer slot, or NULL. Read the object lazily because
 * it may not exist at startup. Prefer GameMgrLea and an independent global anchor;
 * the pinned Vulkan RVA is a last resort behind the host filename gate. */
const uint8_t *sh_resolve_gamemgr_slot(const sig_result *results, size_t n, const uint8_t *module_base);

/* Cache handler dependencies after sh_commands_install registers the commands.
 * cmdsys may be NULL; affected handlers then refuse dispatch. Return 1 after the
 * first pass, including partial binding, or 0 if already installed or base is NULL. */
int sh_entity_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys);

#endif /* BACKEND_B2_ENTITY_H */
