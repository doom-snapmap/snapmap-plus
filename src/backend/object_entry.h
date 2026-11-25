/* Object Mode entry drop length. Entering Object Mode traces a player-sized box down the hovered column in
 * segments of at most 18364 units, ending at the first segment that hits nothing; a segment spanning the whole
 * coordinate range lets tall rooms reach their floor. */
#ifndef SH_OBJECT_ENTRY_H
#define SH_OBJECT_ENTRY_H
#include <stdint.h>
#include "signatures.h"

/* The native segment length, and the length Snapmap+ uses: from 100 above the highest possible module top
 * (+32671) past the lowest possible module bottom (-32671), rounded up. */
#define SH_OBJECT_ENTRY_NATIVE_DROP 18364.0f
#define SH_OBJECT_ENTRY_DROP 66048.0f
/* MOVSS xmm9,[rip+disp32]; XORPS xmm7,xmm7; MOVSS xmm8,[rip+disp32]. */
#define SH_OBJECT_ENTRY_SITE_LEN 17

/* Check the site's bytes; on success *constant is the address its disp32
 * reads. Does not read the constant. */
int sh_object_entry_site(const unsigned char *site,uintptr_t *constant);
/* The disp32 that makes the 9-byte load at site read dest; 0 when out of reach. */
int sh_object_entry_disp(uintptr_t site,uintptr_t dest,int32_t *disp);

#ifndef SH_OBJECT_ENTRY_PURE
/* Boot thread. Logs either way; on any failure the engine is untouched. */
/* The install's byte checks (load decoded, constant 18364); NULL when they hold, else the reason. */
const char *sh_object_entry_check(const unsigned char *site,uintptr_t *constant);
int sh_object_entry_install(const sig_result *results,size_t count);
int sh_object_entry_live(void);
/* The segment length the entry trace reads (18364 when not live). */
float sh_object_entry_drop(void);
#endif
#endif
