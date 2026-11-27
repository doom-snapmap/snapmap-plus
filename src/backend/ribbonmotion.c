/* Apply mod-authored motion at node birth and width at draw time. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "ribbonmotion.h"
#include "patch.h"

typedef void (*birth_fn)(void *, int, const float *, const float *, const float *, const float *, const float *);
static birth_fn original_birth;
typedef void (*draw_fn)(void *, int, void *, const float *, const float *);
static draw_fn original_draw;

typedef struct {
    char *data;
    int count, capacity;
} render_list;

static void ribbon_draw(void *ribbon, int time, void *model, const float *axis, const float *origin)
{
    const char *instance = (const char *)ribbon;
    const char *decl = *(const char *const *)(instance + 0x48);
    const sh_ribbon_noise *params = decl ? (const sh_ribbon_noise *)(decl + 0xf4) : NULL;
    render_list *beams = NULL;
    int before = -1;
    float factor = 1;
    if (model && params && params->marker == SH_RIBBON_WIDTH_ENVELOPE &&
        sh_ribbon_motion_owned(*(const char *const *)(decl + 8))) {
        const float *v = params->magic;
        if (isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) && isfinite(v[3]) &&
            v[0] >= 0 && v[1] > v[0] && v[2] >= 0 && v[3] >= 0) {
            float age = (float)((double)time - *(const int *)(instance + 0x10));
            float t = fmaxf(0, fminf(1, (age - v[0]) / (v[1] - v[0])));
            factor = (1 - t) * v[2] + t * v[3];
            beams = (render_list *)((char *)model + 0xe90);
            before = beams->count;
        }
    }
    original_draw(ribbon, time, model, axis, origin);
    /* Only the new beam belongs to this draw. Its nodes already carry the native width and taper. */
    if (beams && before >= 0 && before < 64 && beams->count == before + 1 &&
        beams->count <= beams->capacity && beams->data) {
        render_list *nodes = (render_list *)(beams->data + (size_t)before * 0x3a28);
        if (nodes->data && nodes->count <= nodes->capacity && nodes->count <= 128) {
            for (int i = 0; i < nodes->count; ++i) {
                float *half_width = (float *)(nodes->data + (size_t)i * 0x74 + 0x4c);
                float width = *half_width * factor;
                if (isfinite(width)) *half_width = width;
            }
        }
    }
}

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
    const sig_result *draw = sig_lookup(results, count, "RibbonDrawBeam");
    if (!original_birth) {
        original_birth = (birth_fn)sh_prepare_detour_sig(site, (void *)ribbon_birth, 21);
        if (original_birth && sh_commit_detour((void *)original_birth) != B2_PATCH_OK) {
            sh_uninstall_detour((void *)original_birth);
            original_birth = NULL;
        }
    }
    if (!original_draw) {
        original_draw = (draw_fn)sh_prepare_detour_sig(draw, (void *)ribbon_draw, 17);
        if (original_draw && sh_commit_detour((void *)original_draw) != B2_PATCH_OK) {
            sh_uninstall_detour((void *)original_draw);
            original_draw = NULL;
        }
    }
    return original_birth && original_draw;
}
#endif
