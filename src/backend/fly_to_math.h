/* fly_to_math.h -- the arithmetic behind "fly to entity", kept free of engine
 * reads so tests can drive it: an entity's world bounds centre, the eye that
 * frames it, and the pitch/yaw that aim at it. idAngles convention: degrees,
 * Z up, forward = (cos p cos y, cos p sin y, -sin p), pitch positive looks down. */
#ifndef SH_FLY_TO_MATH_H
#define SH_FLY_TO_MATH_H

#include <math.h>

#define SH_FLY_PI 3.14159265358979323846f
#define SH_FLY_PITCH_DEG   20.0f   /* looking slightly down keeps the pitch far from the +/-89 clamp */
#define SH_FLY_MIN_DIST   128.0f
#define SH_FLY_MAX_DIST   512.0f
#define SH_FLY_PAD         96.0f

typedef struct sh_fly_vec3 { float x, y, z; } sh_fly_vec3;

static sh_fly_vec3 sh_fly_v3(float x, float y, float z) { sh_fly_vec3 v; v.x = x; v.y = y; v.z = z; return v; }

/* The entity's bounds centre in its module frame: origin plus the local
 * centre carried through the three axis rows, exactly as the engine's own
 * world-point helper does. */
static sh_fly_vec3 sh_fly_local_centre(const float origin[3], const float axis[9],
                                       const float bmin[3], const float bmax[3])
{
    float c[3], i;
    sh_fly_vec3 out;
    for (i = 0; i < 3; i++) c[(int)i] = (bmin[(int)i] + bmax[(int)i]) * 0.5f;
    out.x = origin[0] + c[0] * axis[0] + c[1] * axis[3] + c[2] * axis[6];
    out.y = origin[1] + c[0] * axis[1] + c[1] * axis[4] + c[2] * axis[7];
    out.z = origin[2] + c[0] * axis[2] + c[1] * axis[5] + c[2] * axis[8];
    return out;
}

/* Half the local bounds diagonal; 0 for empty bounds. */
static float sh_fly_radius(const float bmin[3], const float bmax[3])
{
    float dx = bmax[0] - bmin[0], dy = bmax[1] - bmin[1], dz = bmax[2] - bmin[2];
    if (dx < 0 || dy < 0 || dz < 0) return 0.0f;
    return 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
}

/* A module frame: one of eight orientation codes (four quarter turns, four
 * reflections) then a translation. Code order follows the engine's table. */
static sh_fly_vec3 sh_fly_module_to_world(sh_fly_vec3 p, int rotation, const float translation[3])
{
    float x = p.x, y = p.y, rx, ry;
    switch (rotation & 7) {
    default:
    case 0: rx = x;  ry = y;  break;
    case 1: rx = -y; ry = x;  break;
    case 2: rx = -x; ry = -y; break;
    case 3: rx = y;  ry = -x; break;
    case 4: rx = -x; ry = y;  break;
    case 5: rx = y;  ry = x;  break;
    case 6: rx = x;  ry = -y; break;
    case 7: rx = -y; ry = -x; break;
    }
    return sh_fly_v3(rx + translation[0], ry + translation[1], p.z + translation[2]);
}

static sh_fly_vec3 sh_fly_forward(float pitch_deg, float yaw_deg)
{
    float p = pitch_deg * (SH_FLY_PI / 180.0f), y = yaw_deg * (SH_FLY_PI / 180.0f);
    return sh_fly_v3(cosf(p) * cosf(y), cosf(p) * sinf(y), -sinf(p));
}

/* The viewing distance for an entity of the given bounding radius. */
static float sh_fly_distance(float radius)
{
    float d = 2.0f * radius + SH_FLY_PAD;
    if (d < SH_FLY_MIN_DIST) d = SH_FLY_MIN_DIST;
    if (d > SH_FLY_MAX_DIST) d = SH_FLY_MAX_DIST;
    return d;
}

/* The eye that looks at the centre from `dist` away with the given angles. */
static sh_fly_vec3 sh_fly_eye(sh_fly_vec3 centre, float dist, float pitch_deg, float yaw_deg)
{
    sh_fly_vec3 f = sh_fly_forward(pitch_deg, yaw_deg);
    return sh_fly_v3(centre.x - dist * f.x, centre.y - dist * f.y, centre.z - dist * f.z);
}

/* Pitch and yaw that aim from `eye` at `target`; 0 for both when they coincide.
 * Pitch is clamped to +/-89 like the engine's mover, yaw wrapped to [0, 360). */
static void sh_fly_aim(sh_fly_vec3 eye, sh_fly_vec3 target, float *pitch_deg, float *yaw_deg)
{
    float dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
    float flat = sqrtf(dx * dx + dy * dy);
    float yaw, pitch;
    if (flat < 1e-6f && fabsf(dz) < 1e-6f) { *pitch_deg = 0.0f; *yaw_deg = 0.0f; return; }
    yaw = atan2f(dy, dx) * (180.0f / SH_FLY_PI);
    pitch = -atan2f(dz, flat) * (180.0f / SH_FLY_PI);
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    while (yaw < 0.0f) yaw += 360.0f;
    while (yaw >= 360.0f) yaw -= 360.0f;
    *pitch_deg = pitch; *yaw_deg = yaw;
}

#endif
