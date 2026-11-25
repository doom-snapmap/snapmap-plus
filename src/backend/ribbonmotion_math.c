#include <math.h>
#include <stdint.h>
#include <string.h>
#include "ribbonmotion.h"
#include "modsalt.h"

/* The standard 256-entry permutation for improved gradient noise. */
static const unsigned char permutation[256] = {
    151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225,
    140, 36, 103, 30, 69, 142, 8, 99, 37, 240, 21, 10, 23, 190, 6, 148,
    247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32,
    57, 177, 33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175,
    74, 165, 71, 134, 139, 48, 27, 166, 77, 146, 158, 231, 83, 111, 229, 122,
    60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54,
    65, 25, 63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169,
    200, 196, 135, 130, 116, 188, 159, 86, 164, 100, 109, 198, 173, 186, 3, 64,
    52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212,
    207, 206, 59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213,
    119, 248, 152, 2, 44, 154, 163, 70, 221, 153, 101, 155, 167, 43, 172, 9,
    129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104,
    218, 246, 97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241,
    81, 51, 145, 235, 249, 14, 239, 107, 49, 192, 214, 31, 181, 199, 106, 157,
    184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93,
    222, 114, 67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180,
};

static float fade(float x)
{
    return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

static float gradient(int hash, float x, float y, float z)
{
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : h == 12 || h == 14 ? x : z;
    return (h & 1 ? -u : u) + (h & 2 ? -v : v);
}

static int perm(int i)
{
    return permutation[(unsigned)i & 255];
}

static float noise(float x, float y, float z)
{
    float low[3], u[3], planes[2], rows[2];
    float coords[3] = {x, y, z};
    int cells[3], dx, dy, i;
    for (i = 0; i < 3; ++i) {
        float wrapped = fmodf(coords[i], 256.0f);
        float base = floorf(wrapped);
        cells[i] = (int)base;
        low[i] = wrapped - base;
        u[i] = fade(low[i]);
    }
    for (dx = 0; dx < 2; ++dx) {
        for (dy = 0; dy < 2; ++dy) {
            int xy = cells[0] + dx, yy = cells[1] + dy, zz = cells[2];
            float a = gradient(perm(perm(perm(zz) + yy) + xy), low[0]-dx, low[1]-dy, low[2]);
            float b = gradient(perm(perm(perm(zz+1) + yy) + xy), low[0]-dx, low[1]-dy, low[2]-1);
            rows[dy] = lerp(a, b, u[2]);
        }
        planes[dx] = lerp(rows[0], rows[1], u[1]);
    }
    return .936f * lerp(planes[0], planes[1], u[0]);
}

int sh_ribbon_motion_owned(const char *name)
{
    const char *end;
    if (!name || strncmp(name, "mods/", 5) != 0) return 0;
    end = strchr(name + 5, '/');
    return end && end[1] && sh_salt_valid(name + 5, (size_t)(end - name - 5));
}

int sh_ribbon_motion_sample(const sh_ribbon_noise *motion, const float centre[3], uint32_t seed,
                            int time_ms, int first, float offset[3], float drift[3])
{
    float phase[3], p[3], v[3];
    int i;
    if (!motion || (motion->marker != SH_RIBBON_NOISE && motion->marker != SH_RIBBON_NOISE_AT_BIRTH) ||
        !isfinite(motion->frequency) || motion->frequency <= 0.0f ||
        !isfinite(motion->velocity) || !isfinite(motion->magic[3])) return 0;
    for (i = 0; i < 2; ++i) {
        seed = 1664525u * seed + 1013904223u;
        phase[i] = ((seed >> 10) & 32767) * (256.0f / 32767.0f);
    }
    phase[2] = motion->magic[3] * (first ? 1.0f : time_ms * .001f);
    for (i = 0; i < 3; ++i) {
        p[i] = centre[i] * motion->frequency + phase[i];
        if (!isfinite(p[i]) || !isfinite(motion->magic[i])) return 0;
    }
    v[0] = noise(p[0], p[1], p[2] + 128.0f) * motion->magic[0];
    v[1] = noise(p[0] + 128.0f, p[1], p[2]) * motion->magic[1];
    v[2] = noise(p[0], p[1] + 128.0f, p[2]) * motion->magic[2];
    for (i = 0; i < 3; ++i) {
        drift[i] = v[i] * motion->velocity;
        offset[i] = motion->marker == SH_RIBBON_NOISE_AT_BIRTH ? v[i] : 0.0f;
        if (!isfinite(drift[i]) || !isfinite(offset[i])) return 0;
    }
    return 1;
}
