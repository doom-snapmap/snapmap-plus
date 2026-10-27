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

/* Built-in light policy (one function for resize and for map load):
 * - scale per room axis: X and Y by size/stock; Z by interior height
 *   (modern: height-48 over 3344; classic: height over 2048);
 * - origin scales about the room pivot (modern Y=1280, otherwise 0);
 * - radius and center are on the light's own axes (spawnOrientation rows,
 *   NULL = identity) and follow the room scale along each light axis;
 * - roof_anchor keeps the effective point (origin+center) at its stock
 *   clearance below the ceiling, at most a quarter of the interior height;
 * - intensity, color and every other field are retained.
 * The resulting origin must stay inside the room, or the call fails. */
int sh_grid_light_scale(const sh_grid_size *size, float scale[3], float pivot[3],
                        float *roof, float *stock_roof);
int sh_grid_light_axes(const sh_grid_warp *warp, const float origin[3],
                       const float radius[3], const float center[3], const float axis[9],
                       int roof_anchor, float new_origin[3], float new_radius[3], float new_center[3]);
int sh_grid_light(const sh_grid_warp *warp, const float origin[3],
                   const float radius[3], const float center[3], int roof_anchor,
                   float new_origin[3], float new_radius[3], float new_center[3]);
/* Distance cutoffs (maxVisibleRange, maxShadowVisibleRange) grow with the
 * largest size ratio and never fall below their stock values. */
float sh_grid_light_range_scale(const sh_grid_size *size);

#endif
