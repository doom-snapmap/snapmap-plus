#ifndef SNAPMAP_PLUS_RIBBONMOTION_H
#define SNAPMAP_PLUS_RIBBONMOTION_H

#include <stdint.h>
#include "signatures.h"

/* A negative magnitude leaves the ordinary ribbon wave dormant. These two values select birth noise on mod ribbons. */
#define SH_RIBBON_NOISE (-84001.0f)
#define SH_RIBBON_NOISE_AT_BIRTH (-84002.0f)
#define SH_RIBBON_WIDTH_ENVELOPE (-84003.0f)

typedef struct {
    float frequency, marker, velocity;
    float magic[4]; /* World displacement XYZ, and time-phase increments per second. */
} sh_ribbon_noise;

int sh_ribbon_motion_owned(const char *name);
int sh_ribbon_motion_sample(const sh_ribbon_noise *motion, const float centre[3], uint32_t seed,
                            int time_ms, int first, float offset[3], float drift[3]);
int sh_ribbon_motion_install(const sig_result *results, size_t count);
#endif
