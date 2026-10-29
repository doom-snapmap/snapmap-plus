/* fly_to_test.c -- the fly-to arithmetic: bounds centre, module frames, eye and aim. */
#include <stdio.h>
#include <math.h>
#include "../src/backend/fly_to_math.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
static int near3(sh_fly_vec3 v, float x, float y, float z) { return fabsf(v.x - x) < 0.01f && fabsf(v.y - y) < 0.01f && fabsf(v.z - z) < 0.01f; }

int main(void)
{
    static const float identity[9] = {1,0,0, 0,1,0, 0,0,1};
    static const float yaw90[9] = {0,1,0, -1,0,0, 0,0,1};   /* rows: x axis -> +y, y axis -> -x */
    float origin[3] = {100, 200, 0};
    float bmin[3] = {-16, -16, 0}, bmax[3] = {16, 16, 64};
    float translation[3] = {1000, -500, 192};
    sh_fly_vec3 c, w, eye, f;
    float pitch, yaw;

    /* A floor-resting box: the centre sits half the height above the origin. */
    c = sh_fly_local_centre(origin, identity, bmin, bmax);
    CHECK(near3(c, 100, 200, 32));
    CHECK(fabsf(sh_fly_radius(bmin, bmax) - 0.5f * sqrtf(32 * 32 + 32 * 32 + 64 * 64)) < 0.01f);
    CHECK(sh_fly_radius(bmax, bmin) == 0.0f);

    /* An off-centre box rotated by the entity axis: the local offset follows the rows. */
    { float omin[3] = {0, 0, 0}, omax[3] = {20, 0, 0};
      c = sh_fly_local_centre(origin, yaw90, omin, omax);
      CHECK(near3(c, 100, 210, 0)); }

    /* Module frames: the root keeps the point; code 1 is a quarter turn; code 4 mirrors x. */
    w = sh_fly_module_to_world(sh_fly_v3(10, 20, 5), 0, translation);
    CHECK(near3(w, 1010, -480, 197));
    w = sh_fly_module_to_world(sh_fly_v3(10, 20, 5), 1, translation);
    CHECK(near3(w, 980, -490, 197));
    w = sh_fly_module_to_world(sh_fly_v3(10, 20, 5), 2, translation);
    CHECK(near3(w, 990, -520, 197));
    w = sh_fly_module_to_world(sh_fly_v3(10, 20, 5), 3, translation);
    CHECK(near3(w, 1020, -510, 197));
    w = sh_fly_module_to_world(sh_fly_v3(10, 20, 5), 4, translation);
    CHECK(near3(w, 990, -480, 197));
    w = sh_fly_module_to_world(sh_fly_v3(10, 20, 5), 7, translation);
    CHECK(near3(w, 980, -510, 197));

    /* Distance grows with the entity and is clamped. */
    CHECK(sh_fly_distance(0) == SH_FLY_MIN_DIST);
    CHECK(fabsf(sh_fly_distance(50) - 196.0f) < 0.01f);
    CHECK(sh_fly_distance(10000) == SH_FLY_MAX_DIST);

    /* Forward follows idAngles: yaw 0 looks down +x, pitch down lowers z. */
    f = sh_fly_forward(0, 0);
    CHECK(near3(f, 1, 0, 0));
    f = sh_fly_forward(0, 90);
    CHECK(near3(f, 0, 1, 0));
    f = sh_fly_forward(90, 0);
    CHECK(near3(f, 0, 0, -1));

    /* The eye sits behind and above the centre and aiming back at it recovers the angles. */
    eye = sh_fly_eye(sh_fly_v3(0, 0, 0), 200, SH_FLY_PITCH_DEG, 30);
    CHECK(eye.z > 0 && eye.x < 0 && eye.y < 0);
    CHECK(fabsf(sqrtf(eye.x * eye.x + eye.y * eye.y + eye.z * eye.z) - 200) < 0.01f);
    sh_fly_aim(eye, sh_fly_v3(0, 0, 0), &pitch, &yaw);
    CHECK(fabsf(pitch - SH_FLY_PITCH_DEG) < 0.01f);
    CHECK(fabsf(yaw - 30) < 0.01f);

    /* Aim from below wraps yaw into [0, 360) and clamps a straight-up look. */
    sh_fly_aim(sh_fly_v3(10, 10, 0), sh_fly_v3(0, 0, 0), &pitch, &yaw);
    CHECK(fabsf(yaw - 225) < 0.01f && fabsf(pitch) < 0.01f);
    sh_fly_aim(sh_fly_v3(0, 0, 0), sh_fly_v3(0, 0, 100), &pitch, &yaw);
    CHECK(pitch == -89.0f);
    sh_fly_aim(sh_fly_v3(5, 5, 5), sh_fly_v3(5, 5, 5), &pitch, &yaw);
    CHECK(pitch == 0.0f && yaw == 0.0f);

    printf("fly_to_test: %d failures\n", failures);
    return failures != 0;
}
