/* postfx.h -- DOOM 2016's radial blur fx item drawn in the game's post process (postfx.c, modprog.h).
 *
 * An fx item of type FX_BLUR_RADIAL is drawn when it sets a strength cap (extents.x above 0); the game's own items
 * set none and stay as the game plays them (undrawn). Its fields, as Snapmap+ reads them:
 *   blurRadial2Parms.radius   the radius where the blur is strongest, in world units
 *   fadeInTime, fadeOutTime, duration   its strength 0 -> 1 -> 0 over its life (seconds, as every fx item)
 *   color.x   falloff: how fast the blur rises from the edge of its ring to its peak
 *   color.y   strength at full envelope
 *   color.z   the ring's half width, as a fraction of the radius (the blur reaches from (1 - z) r to (1 + z) r)
 *   color.w   the blur's length at full kernel, in screen heights
 *   extents.x the strength cap after the screen and distance terms (0: not drawn)
 *   extents.y the screen radius (screen heights) below which the strength ramps down
 *   extents.z the distance in front of the near plane over which it fades in (world units)
 *   size      the widest fade zone (screen heights); a wider one is narrowed and a falloff below 1 weakens it
 * The blur is hidden by the share of eight depth samples around its centre where the scene is in front of it. */
#ifndef SNAPMAP_PLUS_POSTFX_H
#define SNAPMAP_PLUS_POSTFX_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

extern const sig_entry SH_POSTFX_SIGNATURES[];   /* NULL-terminated */

/* Install the item relay, the SetBlurRadial and post process detours (both renderers). 1 when the blur draws. */
int sh_postfx_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Pure parts (unit tested): whether an item's shape is drawn; keep one item's blur; an item's envelope at `now`
 * (game ms); the 13 parameter vectors of the blurs on at `now` (radialblur0..12), returning how many are drawn. */
int sh_postfx_shape_drawn(const float color[4], const float extents[3], float size, float radius);
void sh_postfx_keep(const float centre[3], int start, int fade_in, int fade_out, int duration, float radius,
                    const float color[4], const float extents[3], float size);
float sh_postfx_envelope(int start, int fade_in, int fade_out, int end, int now);
int sh_postfx_parms(int now, float ysign, float out[13][4]);
void sh_postfx_forget(void);   /* every blur kept (tests) */

#endif /* SNAPMAP_PLUS_POSTFX_H */
