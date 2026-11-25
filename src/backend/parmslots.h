/* parmslots.h -- a render parm always gets a slot (parmslots.c, parmslots_rule.c).
 *
 * The engine keeps each render parm in one of 0x93F slots, a fixed range per kind (idParmState::AllocParmState,
 * VK 0x199D000, GL 0x19918D0; ranges in parmslots_rule.c); a full range is a fatal engine error. AllocParmState
 * is detoured so a parm is never refused: the engine places it unless its range is full, a vector parm without a
 * slot shares the last one given to a non-Snapmap+ parm (modprog.h's reserved slots stay free), and another kind
 * shares the last slot of its own kind. */
#ifndef SNAPMAP_PLUS_PARMSLOTS_H
#define SNAPMAP_PLUS_PARMSLOTS_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#define SH_PARMSLOTS_COUNT  0x93F     /* slots; also the engine's "no slot" value */
#define SH_PARMSLOTS_ENGINE (-1)      /* sh_parmslots_decide: the engine places the parm */
#define SH_PARMSLOTS_VECTOR 0         /* the vector kind (a render parm decl's kind, +0x6C) */

/* The slot range [*lo, *hi) of a render parm kind, as AllocParmState's switch gives it. 0 for an unknown kind. */
int sh_parmslots_range(int kind, int *lo, int *hi);

/* One slot's holder: 1 with its name and kind when the slot is held, 0 when it is empty. */
typedef int (*sh_parmslots_read)(void *context, int slot, const char **name, int *kind);

/* Where a parm asking for a slot goes: SH_PARMSLOTS_ENGINE, or the slot it shares. Pure: reads the table only
 * through `read`. kind and name are the asking parm (name compared by pointer for "already holds a slot", as the
 * engine does, and by text against `reserved`, the n_reserved vector parm names that keep their slots); spill is the
 * shared vector slot or -1; *becomes_spill is set to 1 when the slot the engine is about to give becomes it. */
int sh_parmslots_decide(int kind, const char *name, sh_parmslots_read read, void *context,
                        const char *const *reserved, unsigned n_reserved, int spill, int *becomes_spill);

/* Detour AllocParmState (both renderers). 1 when the guard is in place. */
int sh_parmslots_install(const sig_result *results, size_t count, const uint8_t *module_base);

#endif /* SNAPMAP_PLUS_PARMSLOTS_H */
