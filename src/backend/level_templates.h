/* level_templates.h -- a level load sees only the mods its map declares.
 *
 * Every SnapMap level load (idGameLocal::LoadMap, build 13954591 Vulkan 0x39B790;
 * its walk at Vulkan 0x39CD59, OpenGL 0x39C669) builds a snapshot template of every entityDef the resource
 * manager holds: it copies the manager's whole list (the resource list copy,
 * Vulkan 0x1572330, OpenGL 0x15734C0), and for each entry not yet templated
 * creates one, which parses the entity's edit state (SetEntityEditState). It
 * copies the list again when a template loaded further entityDefs. The engine
 * keeps every decl the editor's passes registered for the session (the rows'
 * closure of every installed mod, modserve_engine.c), so without this module a
 * stock map's Play parsed the entityDefs of mods it does not declare, and one
 * that does not parse stopped the level load (R-ED1).
 *
 * The product detours the list copy and, for its two calls from the template
 * walk only (identified by their return addresses), leaves out every entry the
 * scope says a level load must not see (sh_modserve_level_hides). Both walk
 * loops read the filtered list, so neither creates nor demands a template for
 * an undeclared mod's entityDef. Every other caller of the list copy is
 * unchanged. */
#ifndef BACKEND_LEVEL_TEMPLATES_H
#define BACKEND_LEVEL_TEMPLATES_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* NULL-terminated: [0] the template walk's first list copy, [1] the copy again
 * after a template loaded more entityDefs. Each pattern starts at its own
 * sequence and holds the E8 call to the list copy at the offset below. */
extern const sig_entry SH_LEVEL_TEMPLATES_SIGNATURES[];
#define SH_LT_WALK_CALL    0x1D
#define SH_LT_REWALK_CALL  0x29

/* The list copy's prologue, detoured: push rsi; push rdi; push r12; push r14;
 * push r15; sub rsp,0x40; mov qword [rsp+0x20],-2 (22 bytes, no relative operand). */
#define SH_LT_STOLEN 22

/* The target of the E8 rel32 call at `call` (0 when the byte is not E8). */
uintptr_t sh_level_templates_call_target(const uint8_t *call);

/* Remove from items[0..count) every entry whose name `hides` (name_of reads an
 * entry's name; NULL or unreadable keeps the entry), keeping the order of the
 * rest. Returns the new count; *hidden counts the removed entries and *first
 * names the first one (left unchanged when none). */
int sh_level_templates_filter(void **items, int count, const char *(*name_of)(const void *item),
                              int (*hides)(const char *name), int *hidden, const char **first);

/* Resolve both walk sites, check that both call the same list copy, detour it.
 * Returns 1 when the detour is in place. */
int sh_level_templates_install(const uint8_t *module_base);

#endif /* BACKEND_LEVEL_TEMPLATES_H */
