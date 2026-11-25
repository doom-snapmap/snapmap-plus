/* Add the declared world velocity while the native ribbon creates a node. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "ribbonmotion.h"
#include "patch.h"

typedef void (*birth_fn)(void *, int, const float *, const float *, const float *, const float *, const float *);
static birth_fn original_birth;

static void ribbon_birth(void *ribbon, int time, const float *a, const float *b,
                         const float *axis, const float *velocity, const float *colour)
{
    const char *instance = (const char *)ribbon;
    const char *decl = *(const char *const *)(instance + 0x48);
    const sh_ribbon_noise *motion;
    float centre[3], offset[3], drift[3], first[3], second[3], speed[3];
    uint32_t spawn, random;
    int i;
    if (!decl) goto native;
    motion = (const sh_ribbon_noise *)(decl + 0xf4);
    if (motion->marker != SH_RIBBON_NOISE && motion->marker != SH_RIBBON_NOISE_AT_BIRTH) goto native;
    if (!sh_ribbon_motion_owned(*(const char *const *)(decl + 8))) goto native;
    for (i = 0; i < 3; ++i) centre[i] = a[i] * .5f + b[i] * .5f;
    memcpy(&spawn, instance + 0x10, sizeof spawn);
    memcpy(&random, instance + 0x18, sizeof random);
    if (!sh_ribbon_motion_sample(motion, centre, spawn ^ random, time, *(const int *)(instance + 0x1c) == 0,
                                 offset, drift)) goto native;
    for (i = 0; i < 3; ++i) {
        first[i] = a[i] + offset[i];
        second[i] = b[i] + offset[i];
        speed[i] = velocity[i] + drift[i];
        if (!isfinite(first[i]) || !isfinite(second[i]) || !isfinite(speed[i])) goto native;
    }
    /* The engine uses pointer equality to distinguish a centre line from two explicit edges. */
    original_birth(ribbon, time, first, a == b ? first : second, axis, speed, colour);
    return;
native:
    original_birth(ribbon, time, a, b, axis, velocity, colour);
}

#ifndef SH_RIBBON_MOTION_TEST
int sh_ribbon_motion_install(const sig_result *results, size_t count)
{
    const sig_result *site = sig_lookup(results, count, "RibbonNodeBirth");
    if (original_birth) return 1;
    original_birth = (birth_fn)sh_prepare_detour_sig(site, (void *)ribbon_birth, 21);
    if (!original_birth) return 0;
    if (sh_commit_detour((void *)original_birth) == B2_PATCH_OK) return 1;
    sh_uninstall_detour((void *)original_birth);
    original_birth = NULL;
    return 0;
}
#endif
