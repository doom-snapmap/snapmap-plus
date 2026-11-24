/* Unrestricted entity placement in the editor. The editor refuses an element with no accepted surface unless its
 * placementFlags include AIR or AIR_UNALIGNED, so FLOOR-only entities cannot be dropped in the air; Snapmap+ patches
 * the per-element test and the group test to accept it, leaving every other placement refusal native. */
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
