/* Object Mode entry drop length.
 *
 * Entering Object Mode (TAB in Blueprint) places the FPS camera by tracing a
 * player-sized box straight down the hovered column, starting 100 units above
 * the top of the whole map, in segments of at most 18364 units: each hit
 * restarts the descent just below it, and the descent ends at the first
 * segment that hits nothing, on the last surface it found. A vertical gap
 * taller than 18364 therefore ends it early: inside a Grid Room taller than
 * that the camera stays under the roof, and when the first segment finds
 * nothing (every module in the column lies more than 18364 below the map's
 * top) the camera keeps its previous Object Mode position. Every map built
 * from stock modules fits inside one segment, so a segment as long as the
 * whole coordinate range changes nothing for them and lets tall rooms reach
 * their floor. */
#ifndef SH_OBJECT_ENTRY_H
#define SH_OBJECT_ENTRY_H
#include <stdint.h>
#include "signatures.h"

/* The native segment length, and the length Snapmap+ publishes: from 100
 * above the highest possible module top (+32671) past the lowest possible
 * module bottom (-32671), rounded up. */
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
int sh_object_entry_install(const sig_result *results,size_t count);
int sh_object_entry_live(void);
/* The segment length the entry trace reads now (18364 when not live). */
float sh_object_entry_drop(void);
#endif
#endif
