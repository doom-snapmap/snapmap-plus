/* aas_augment.h -- give an author's own geometry real AI navigation.
 *
 * THE PROBLEM
 * -----------
 * A SnapMap module ships its navigation baked by id's tools, and the engine's
 * AI cannot occupy space that has no AAS area. The Grid Room -- the box most
 * custom maps are built in -- ships nine areas, all flat: seven floor slabs at
 * z=0 and a ceiling plane at z=3360. Everything an author stacks inside it is
 * invisible to demons.
 *
 * This module takes a shipped payload and ADDS to it: one walkable area per
 * marked surface, spliced into the BSP tree so the engine can actually find it,
 * and the reachabilities that join it to what is already there.
 *
 * WHAT IT EMITS, AND WHAT IT DELIBERATELY DOES NOT
 * -----------------------------------------------
 * Three link regimes, chosen by the height difference to the floor outside each
 * platform edge:
 *
 *   drop <= maxStepHeight   a reciprocated pair of plain 0x20 walk links, no
 *                           animation at all. maxStepHeight is 18 in every
 *                           SnapMap monster class, and across 94,327 shipped
 *                           plain-walk records joining two flat areas not one
 *                           spans more than that. This is why short volumes
 *                           have always half-worked.
 *   drop downward           a walk-off-ledge link, gated on maxFallHeight
 *                           (0 in every monster-class module, so `fall=auto`
 *                           emits none -- see sh_aas_aug_opts).
 *   drop upward, > 18       a baked traversal: the animated climb. NOT EMITTED
 *                           BY THIS VERSION. The encoding is understood and the
 *                           seam is `traversal` below, but it needs the game's
 *                           universal traversal table read at runtime and live
 *                           verification per demon; until then a platform out of
 *                           step range is an ISLAND.
 *
 * An island is not an error. An area with no reachability is legal -- the Grid
 * Room's own ceiling is one in the shipped file -- and demons on it hunt and
 * fight across it normally; they simply cannot walk on or off. Authors reach a
 * tall platform today either by spawning demons on it or by chaining ledges no
 * more than maxStepHeight apart, which the step regime links for free.
 *
 * EVERY MAGIC NUMBER HERE WAS MEASURED
 * ------------------------------------
 * The constants in the .c cite the shipped data they came from -- the modal BSP
 * floor epsilon, the node field4 values per split-plane orientation, the walk
 * reachability spacing and end insets, the cluster bookkeeping invariant. None
 * of them is a guess, and where a value is copied without being understood the
 * comment says so.
 *
 * NO GAME BYTES: tests synthesize payloads. See README.md.
 */
#ifndef SNAPMAP_PLUS_AAS_AUGMENT_H
#define SNAPMAP_PLUS_AAS_AUGMENT_H

#include <stddef.h>
#include "aas_edit.h"

#define SH_AUG_MAX_PLATFORMS   512
#define SH_AUG_NAME_CAP        64
#define SH_AUG_REASON_CAP      160

/* One surface to make walkable, in the payload's own module-local space. */
typedef struct sh_aug_platform {
    float x0, y0, x1, y1;               /* rect, normalised */
    float z;                            /* the walkable surface height */
    char  name[SH_AUG_NAME_CAP];        /* for the report only */
} sh_aug_platform;

/* `fall`: emit walk-off-ledge links. AUTO honours settings.maxFallHeight, which
 * is 0 in every monster-class module, so AUTO emits none -- ALWAYS is an
 * off-precedent deviation offered for testing, not for authors. */
enum { SH_AUG_FALL_AUTO = 0, SH_AUG_FALL_NEVER, SH_AUG_FALL_ALWAYS };

typedef struct sh_aug_opts {
    int   fall;             /* SH_AUG_FALL_* */
    int   inset;            /* inset by the agent radius; 1 unless testing */
    int   traversal;        /* reserved: baked climbs. Must be 0 in this build. */
} sh_aug_opts;

/* What one platform became, per class, so the author can be told the truth. */
typedef struct sh_aug_platform_result {
    char     name[SH_AUG_NAME_CAP];
    int      emitted;                   /* 1 if an area was added */
    int      area;                      /* the new area index, or -1 */
    int      carrier;                   /* the area the platform sits over */
    int      leaf_slots_carved;
    int      links;                     /* reachabilities touching this area */
    int      island;                    /* 1 if nothing links it */
    char     reason[SH_AUG_REASON_CAP]; /* why it was not emitted */
} sh_aug_platform_result;

typedef struct sh_aug_report {
    sh_aug_platform_result platforms[SH_AUG_MAX_PLATFORMS];
    int      platform_count;
    unsigned areas_before, areas_after;
    unsigned reach_before, reach_after;
    unsigned depth_before, depth_after;
    int      depth_exceeded;            /* past the loader's 0x80 limit */
} sh_aug_report;

/* Augment `a` in place. Returns 1 if the model is still coherent (even if every
 * platform was refused), 0 only if the model was left unusable -- in which case
 * the caller must discard it and serve the shipped bytes.
 *
 * A platform is refused, never fudged: too small for the agent once inset, too
 * little headroom for the class, or outside the int16 range areaBounds uses.
 * Each refusal is a line in the report, because refusing one box must not cost
 * the author the other forty. */
int sh_aas_augment(sh_aas *a, const sh_aug_platform *plats, int n,
                   const sh_aug_opts *opts, sh_aug_report *out);

/* Which area a point resolves to by walking the BSP -- the same query the
 * engine's own aas_findArea makes. 0 means void. Exposed because the serving
 * path uses it to prove a generated area is actually findable before trusting
 * the payload. */
int sh_aas_point_area(const sh_aas *a, float x, float y, float z);

/* The BSP tree's depth, and the loader's hard limit on it. */
unsigned sh_aas_tree_depth(const sh_aas *a);

#endif /* SNAPMAP_PLUS_AAS_AUGMENT_H */
