/* Engine dependencies for entity, spawn-position, map-export, and cheat commands.
 * Vtable slots and object offsets require validation when porting engine builds. */
#ifndef BACKEND_B2_ENTITY_H
#define BACKEND_B2_ENTITY_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Caches handler dependencies after sh_commands_install registers the commands.
 * cmdsys may be NULL; affected handlers then refuse dispatch. Return 1 after the
 * first pass, including partial binding, or 0 if already installed or base is NULL. */
int sh_entity_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys);

/* The live game manager object and FindEntity(name) through it, both NULL
 * before sh_entity_install or when the game has no map. SEH-guarded. */
void *sh_entity_gamemgr(void);
void *sh_entity_find(const char *name);

#endif /* BACKEND_B2_ENTITY_H */
