/* Prefab Create grab-frame guard. PrefabPopulate (VK 0x1411adb30 / GL 0x1411acd30) stores entities relative to
 * the grab frame the helper (VK 0x1412164d0 / GL 0x141215920) clips from the view-centre line (editor+0x170,
 * angles +0x17c) and the hovered body's box; when the line misses, grabDistance is a huge negative and float32
 * rounds the layout coarse. PasteInstantiate (VK 0x1411af070 / GL 0x1411ae270) reads only grabAxis (prefab+0x00),
 * grabDistance (+0x24) and cameraYaw (+0x28), so any grab point works; the guard moves a bad one to the hovered
 * box's centre. The math is engine-free for native tests; apply_engine.c owns the detour and the engine reads. */
#ifndef BACKEND_PREFAB_GRAB_H
#define BACKEND_PREFAB_GRAB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_PREFAB_GRAB_UNCHECKED 0   /* the guard did not run (not installed, not armed) */
#define SH_PREFAB_GRAB_OK        1   /* the native frame hit the hovered box */
#define SH_PREFAB_GRAB_CORRECTED 2   /* missed or absurd; a frame was built from the hovered box */
#define SH_PREFAB_GRAB_REFUSED   3   /* no finite frame could be built */

/* Every world coordinate lies inside +/-SH_BOUNDS_CEILING (32671); twice the cube's diagonal, 2 * sqrt(3) *
 * 32671, bounds any distance from a camera that can still see the world to a point in it. */
#define SH_PREFAB_GRAB_MAX_DISTANCE 113175.0f
/* A paste closer than this would land on the camera. */
#define SH_PREFAB_GRAB_MIN_DISTANCE 32.0f
/* Positions this large in a saved prefab mean its grab point was far from the selection (float32 resolution
 * there is 1/256 unit). */
#define SH_PREFAB_GRAB_MAX_POSITION 65536.0f

/* The hovered body as the helper reads it: origin body+0x288, axis
 * body+0x294 (rows are the local X, Y, Z axes in world, row-vector
 * convention), and the local clip box. */
typedef struct sh_prefab_grab_box {
    float origin[3];
    float axis[9];
    float bmin[3];
    float bmax[3];
} sh_prefab_grab_box;

typedef struct sh_prefab_grab_result {
    int   status;
    float native_distance;
    float distance;
    float native_point[3];
    float point[3];
    /* How far the native grab point lay outside the hovered box (0 inside;
     * negative when it could not be measured). */
    float outside;
} sh_prefab_grab_result;

/* The helper's forward vector (FUN_1402d91d0 VK / FUN_1402d9030 GL): angles
 * are pitch, yaw, roll in degrees; forward = (cp*cy, cp*sy, -sp). */
void  sh_prefab_grab_forward(const float angles[3], float forward[3]);

/* The clip box the helper uses when decl+0x3ce bit 8 is set: centred on the
 * origin, +/-0.5 * (body+0x400, +0x404, +0x434). */
void  sh_prefab_grab_centred_box(const float size[3], float bmin[3], float bmax[3]);

/* World centre of the box: ((bmin+bmax)/2) x axis + origin. */
void  sh_prefab_grab_box_centre(const sh_prefab_grab_box *box, float centre[3]);

/* Distance of p outside the oriented box along its worst axis; 0 inside,
 * negative when an input is not finite. */
float sh_prefab_grab_outside(const sh_prefab_grab_box *box, const float p[3]);

/* Check and, when needed, correct the helper's output in place: frame is its 12-float transform (origin, 3x3
 * axis) and distance the value it wrote to prefab+0x24. A NULL box (body unreadable) keeps a bounded, finite
 * frame and refuses anything else. Returns the status, also stored in *result when not NULL. */
int   sh_prefab_grab_guard(const float view[3], const float forward[3],
                           const sh_prefab_grab_box *box, float frame[12],
                           float *distance, sh_prefab_grab_result *result);

/* Fallback for a backend without the guard: reads a serialized prefab. Returns 1 when grabDistance is finite and
 * bounded and no spawnPosition component exceeds SH_PREFAB_GRAB_MAX_POSITION, 0 for a missed frame, -1 with no
 * grabDistance; *distance receives the value read. */
int   sh_prefab_grab_json_check(const char *json, size_t len, double *distance);

#ifdef __cplusplus
}
#endif
#endif
