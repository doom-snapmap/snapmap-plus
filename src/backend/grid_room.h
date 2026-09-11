/* Grid Room dimensions and door-preserving deformation, in module space. */
#ifndef SH_GRID_ROOM_H
#define SH_GRID_ROOM_H

#include <stddef.h>

typedef enum sh_grid_kind {
    SH_GRID_NONE = 0,
    SH_GRID_CLASSIC = 1,
    SH_GRID_MODERN = 2
} sh_grid_kind;

typedef struct sh_grid_size {
    sh_grid_kind kind;
    unsigned xyz[3];
} sh_grid_size;

typedef struct sh_grid_axis {
    unsigned count;
    float source[6], target[6];
} sh_grid_axis;

typedef struct sh_grid_warp {
    sh_grid_size size;
    sh_grid_axis axis[3];
} sh_grid_warp;

/* Strict identities: ordinary non-grid modules are never admitted. Variant
 * names carry dimensions in the native saved moduleName field, so copying a
 * placement copies its dimensions without a parallel index-keyed sidecar. */
sh_grid_kind sh_grid_stock_kind(const char *module_name);
const char *sh_grid_stock_name(sh_grid_kind kind);
int sh_grid_default(sh_grid_kind kind, sh_grid_size *out);
int sh_grid_minimum(sh_grid_kind kind, sh_grid_size *out);
/* Collision node bounds and AAS bounds use signed 16-bit coordinates.
 * These are format limits in module space; world placement is checked later. */
int sh_grid_maximum(sh_grid_kind kind, sh_grid_size *out);
/* Native numeric entry clamps each finite axis independently, then rounds to
 * whole game units. Unknown room kinds and non-finite input are refused. */
int sh_grid_clamp(sh_grid_kind kind, const float requested[3], sh_grid_size *out);
int sh_grid_name(const sh_grid_size *size, char *out, size_t capacity);
int sh_grid_parse_name(const char *name, sh_grid_size *out);

/* Numerical deformation limits retain at least 16 units between the rigid
 * doorway and wall/ceiling regions. The authoring layer must additionally
 * enforce tested animated-door clearance; these are not gameplay guarantees. */
int sh_grid_warp_init(const sh_grid_size *size, sh_grid_warp *out);
float sh_grid_coordinate(const sh_grid_warp *warp, unsigned axis, float value);
void sh_grid_point(const sh_grid_warp *warp, const float source[3], float target[3]);
void sh_grid_affine(const sh_grid_warp *warp, unsigned axis, float value,
                    float *scale, float *shift);

/* Pure buffer transforms. Caller owns two disjoint buffers. Equal-sized output is
 * unusable on failure. Reads are bounded; unknown formats/layouts are refused. */
int sh_grid_bmodel(const sh_grid_warp *warp, const unsigned char *source,
                    size_t length, unsigned char *target);
int sh_grid_bcm(const sh_grid_warp *warp, const unsigned char *source,
                size_t length, unsigned char *target);

/* Independent light coverage, with effective dynamic-light center anchored
 * below the roof. Intensity is retained: coverage changes do not imply a
 * universal photometric intensity multiplier. */
int sh_grid_light(const sh_grid_warp *warp, const float origin[3],
                   const float radius[3], const float center[3], int roof_anchor,
                   float new_origin[3], float new_radius[3], float new_center[3]);

#endif
