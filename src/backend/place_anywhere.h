/* Unrestricted entity placement in the editor.
 *
 * Every snapEditorEntityDef carries placementFlags, a bit set of
 * snapEntityPlaceType_t: FLOOR 0x01, WALL 0x02, CEILING 0x04,
 * SURFACE_ALIGNED 0x08, AIR 0x10, AIR_UNALIGNED 0x20 (DEFAULT is 0). While an
 * entity is held or moved, the editor classifies the surface its box touches
 * (none, floor, ceiling, wall, sloped) and clears that classification when
 * the flags do not accept the surface. An element with no accepted surface is
 * then invalid unless its flags include AIR or AIR_UNALIGNED: the ghost turns
 * red and Place is refused. So demons, the player start, encounter spawners,
 * AI path and combat points and the item hints, all FLOOR only, cannot be
 * dropped in the air.
 *
 * Snapmap+ removes only that refusal. The two places that apply it (the
 * per-element test that colours the ghost, and the group test that admits a
 * Place) accept an element with no accepted surface as if its flags allowed
 * air, so it stays where the cursor holds it. Everything else is native: the
 * surface classification, floor contact, alignment, Drop to Floor, the
 * overlap ("blocked") and outside-every-module refusals. Nothing new is
 * written into the map: the entity's position is its only record. */
#ifndef SH_PLACE_ANYWHERE_H
#define SH_PLACE_ANYWHERE_H
#include <stddef.h>
#include "signatures.h"

/* The two gate sites and the byte each patch changes: the JNZ taken when the
 * element has a surface becomes a JMP, so the AIR flags test that follows it
 * is never reached. */
enum {SH_PLACE_GATE_COMMIT=0,SH_PLACE_GATE_ELEMENT=1,SH_PLACE_GATE_COUNT=2};
#define SH_PLACE_GATE_JNZ 0x75
#define SH_PLACE_GATE_JMP 0xEB

/* Offset of the JNZ opcode from the site address for the given gate, or -1
 * when the bytes at site are not that gate. */
int sh_place_gate_branch(const unsigned char *site,int gate);

#ifndef SH_PLACE_ANYWHERE_PURE
/* Boot thread. Patches both gates or neither, and logs either way. */
int sh_place_anywhere_install(const sig_result *results,size_t count);
int sh_place_anywhere_live(void);
#endif
#endif
