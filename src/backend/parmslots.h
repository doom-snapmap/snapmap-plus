/* parmslots.h -- a render parm always gets a slot (parmslots.c, parmslots_rule.c).
 *
 * The engine keeps every render parm in one of 0x93F parm state slots, a fixed range per kind
 * (idParmState::AllocParmState, VK 0x199D000, GL 0x19918D0): vectors 0x000-0x3FF, textures 0x400-0x5FF, samplers
 * 0x600-0x67F, programs 0x680-0x7FF, strings 0x800-0x87F, buffers 0x880-0x8BF, uniform buffers 0x8C0-0x8FF, image
 * buffers 0x900-0x93E. A parm whose range is full is a fatal engine error, and under the fault shield a parm that
 * never loads: every render program naming it then draws the engine's default program. The vector range is the
 * tight one: the game's own decls and its 128 expression temporaries hold 973 of the 1,024, so everything that is
 * not the game's has 51.
 *
 * Snapmap+ makes the refusal impossible. AllocParmState is detoured and asked first:
 *   - a parm whose name already holds a slot, or whose range has room, is the engine's to place, with one
 *     reservation: the vector parms of Snapmap+'s own render programs (modprog.h: the multilayer blend, the Eternal
 *     particle shading, the radial blur) keep as many free vector slots as they still need, whatever asks first, so
 *     a mod's own parms can never take the slots a ported demon's programs draw with;
 *   - a vector parm that finds no slot of its own shares one: the last vector slot handed out to a parm that is
 *     not Snapmap+'s becomes the shared slot, and every later one is given the same slot. Parms sharing a slot
 *     read each other's value, so a program naming one draws with a wrong constant instead of not drawing at all;
 *   - a parm of another kind whose range is full shares the slot of the last parm of its own kind.
 * Nothing is reported to the author or the player; the backend log names each parm that shares a slot (the first
 * 32), for the test bench. */
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

/* Where a parm asking for a slot goes: SH_PARMSLOTS_ENGINE, or the slot it shares.
 *   kind, name    the asking parm (name compared by pointer for "already holds a slot", as the engine does, and
 *                 by text against `reserved`)
 *   reserved      the vector parm names that keep their slots (Snapmap+'s programs'); n_reserved of them
 *   spill         the shared vector slot, or -1 when none has been made yet
 *   *becomes_spill set to 1 when the slot the engine is about to give this parm is to become the shared slot
 * Pure: reads the table only through `read`. */
int sh_parmslots_decide(int kind, const char *name, sh_parmslots_read read, void *context,
                        const char *const *reserved, unsigned n_reserved, int spill, int *becomes_spill);

/* Detour AllocParmState (both renderers). 1 when the guard is in place. */
int sh_parmslots_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Parms given a shared slot since the start, and the shared vector slot (-1 none). */
long sh_parmslots_shared(int *spill_slot);

#endif /* SNAPMAP_PLUS_PARMSLOTS_H */
