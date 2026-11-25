/* level_templates.h -- a level load sees only the mods its map declares. idGameLocal::LoadMap (Vulkan 0x39B790;
 * template walk Vulkan 0x39CD59, OpenGL 0x39C669) templates every entityDef in the resource manager's list
 * (list copy Vulkan 0x1572330, OpenGL 0x15734C0), so an undeclared mod's unparsable entityDef stops the load.
 * The product detours the list copy for its two calls from the template walk only (told apart by return
 * address) and drops every entry sh_modserve_level_hides names; other callers are unchanged. */
#ifndef BACKEND_LEVEL_TEMPLATES_H
#define BACKEND_LEVEL_TEMPLATES_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* NULL-terminated: [0] the template walk's first list copy, [1] the copy again after a template loaded more
 * entityDefs. Each pattern holds the E8 call to the list copy at the offset below. */
extern const sig_entry SH_LEVEL_TEMPLATES_SIGNATURES[];
#define SH_LT_WALK_CALL    0x1D
#define SH_LT_REWALK_CALL  0x29

/* The list copy's detoured prologue: 22 bytes with no relative operand. */
#define SH_LT_STOLEN 22

/* The target of the E8 rel32 call at `call` (0 when the byte is not E8). */
uintptr_t sh_level_templates_call_target(const uint8_t *call);

/* Sets to NULL every entry of items[0..count) whose name `hides` (a NULL entry or unreadable name stays). The
 * count is kept because the walk compares it with the manager's to notice more entityDefs; a shorter list
 * would warn and recopy after every template. Returns how many were cleared; *first names the first. */
int sh_level_templates_filter(void **items, int count, const char *(*name_of)(const void *item),
                              int (*hides)(const char *name), const char **first);

/* Resolve both walk sites, check that both call the same list copy, detour it.
 * Returns 1 when the detour is in place. */
int sh_level_templates_install(const uint8_t *module_base);

#endif /* BACKEND_LEVEL_TEMPLATES_H */
