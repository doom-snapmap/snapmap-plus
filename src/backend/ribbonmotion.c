/* Apply mod-authored motion at node birth and width at draw time. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "ribbonmotion.h"
#include "patch.h"
#include "entity.h"

typedef void (*birth_fn)(void *, int, const float *, const float *, const float *, const float *, const float *);
static birth_fn original_birth;
typedef void (*draw_fn)(void *, int, void *, const float *, const float *);
static draw_fn original_draw;
static double (*view_distance)(const float *);

typedef struct { void **callbacks; int count, capacity; } fx_views;
typedef unsigned char (*game_ready_fn)(void *);
typedef const fx_views *(*game_views_fn)(void *);
typedef void (*view_pose_fn)(void *, float *, float *);

static double distance_to_views(void *game, const float *point)
{
    if (!game) return 0;
    void **methods = *(void ***)game;
    game_ready_fn ready = (game_ready_fn)methods[0x38 / sizeof(void *)];
    game_views_fn get = (game_views_fn)methods[0x348 / sizeof(void *)];
    if (!ready || !get || !ready(game)) return 0;
    const fx_views *views = get(game);
    if (!views || !views->callbacks || views->count > views->capacity) return 0;
    double nearest = HUGE_VAL;
    for (int i = 0; i < views->count && i < 12; ++i) {
        void *view = views->callbacks[i];
        if (!view) continue;
        float origin[3], axis[9];
        view_pose_fn pose = (view_pose_fn)(*(void ***)view)[0x20 / sizeof(void *)];
        if (!pose) continue;
        pose(view, origin, axis);
        double x = (double)origin[0] - point[0], y = (double)origin[1] - point[1], z = (double)origin[2] - point[2];
        double squared = x*x + y*y + z*z;
        if (squared < nearest) nearest = squared;
    }
    return isfinite(nearest) ? sqrt(nearest) : 0;
}

#ifndef SH_RIBBON_MOTION_TEST
static double active_view_distance(const float *point)
{
    /* World FX use the nearest active view. Its callback supplies the final camera including view offsets. */
    __try { return distance_to_views(sh_entity_gamemgr(), point); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}
#endif

/* The native add keeps the preceding input pose even when this frame contributes no knots. */
static void ribbon_node(void *ribbon, int time, const float *a, const float *b,
                        const float *axis, const float *velocity, const float *colour);

static void sample_node(void *ribbon, int previous, int now, const float *from, const float *to,
                        double fraction, const float *axis, const float *velocity, const float *colour)
{
    float p[3];
    for (int i = 0; i < 3; ++i) p[i] = (float)(from[i] + ((double)to[i] - from[i]) * fraction);
    int time = (int)round(previous + ((double)now - previous) * fraction);
    ribbon_node(ribbon, time, p, p, axis, velocity, colour);
}

static int sampled_birth(char *ribbon, const char *decl, int now, const float *p, const float *axis,
                         const float *velocity, const float *colour)
{
    const float mode = *(const float *)(decl + 0xd4);
    if (mode != SH_RIBBON_TIMED_KNOTS && mode != SH_RIBBON_DISTANCE_KNOTS) return 0;
    const double period = *(const float *)(decl + 0xdc);
    const float first = *(const float *)(decl + 0xe0), second = *(const float *)(decl + 0xe4);
    if (!isfinite(period) || period <= 0 || !isfinite(first) || !isfinite(second) ||
        *(const char *)(decl + 0x118) || !sh_ribbon_motion_owned(*(const char *const *)(decl + 8))) return 0;
    if (mode == SH_RIBBON_TIMED_KNOTS && (first < 2 || first > 65535 || first != floorf(first))) return 0;
    if (mode == SH_RIBBON_DISTANCE_KNOTS && (first < 0 || second < 0)) return 0;

    const int start = *(const int *)(ribbon + 0x10);
    int previous = *(const int *)(ribbon + 0x2c);
    const float *from = (const float *)(ribbon + 0x30);
    /* A centre-line input has no second edge. Its unused previous edge carries the distance remainder. */
    float *remainder = (float *)(ribbon + 0x3c);
    if (previous < start || previous > now ||
        (mode == SH_RIBBON_DISTANCE_KNOTS && !*(const int *)(ribbon + 0x1c))) {
        previous = mode == SH_RIBBON_TIMED_KNOTS && start <= now ? start : now;
        from = previous < now ? (const float *)(ribbon + 4) : p;
        *remainder = 0;
        ribbon_node(ribbon, previous, from, from, axis, velocity, colour);
    }
    if (now <= previous) return 1;
    if (mode == SH_RIBBON_TIMED_KNOTS) {
        double last = first - 1;
        int begin = (int)fmin(last + 1, floor(((double)previous - start) * last / period) + 1);
        int end = (int)fmin(last, floor(((double)now - start) * last / period));
        /* Only the newest 128 points survive the native ring after a long frame. */
        if (begin < end - 127) begin = end - 127;
        for (int i = begin; i <= end; ++i) {
            double fraction = ((double)start - previous + period * i / last) / ((double)now - previous);
            sample_node(ribbon, previous, now, from, p, fraction, axis, velocity, colour);
        }
    } else {
        int head = *(const int *)(ribbon + 0x20);
        if (head < 0 || head >= 128) return 1;
        const char *last = ribbon + 0x60 + (size_t)head * 0x78;
        from = (const float *)last;
        previous = *(const int *)(last + 0x4c);
        if (previous < start || previous > now) return 1;
        double x = (double)p[0] - from[0], y = (double)p[1] - from[1], z = (double)p[2] - from[2];
        double length = sqrt(x*x + y*y + z*z);
        if (isfinite(length) && length > 0) {
            double spacing = period;
            if (view_distance && first > 0 && second > 0 && period != first) {
                double distance = view_distance(p);
                if (distance > 0) spacing += (first-period) * fmin(1, distance/second);
            }
            double carried = isfinite(*remainder) && *remainder >= 0 ? *remainder : 0;
            double total = carried + length, knots = floor(total / spacing);
            int count = (int)fmin(knots, 16);
            if (length <= .0001 / .01905) count = 0;
            /* A distance trigger fills the interval to the current pose and starts a new interval there. */
            for (int i = 1; i <= count; ++i) {
                sample_node(ribbon, previous, now, from, p, (double)i/count, axis, velocity, colour);
            }
            *remainder = count ? 0 : (float)total;
        }
    }
    return 1;
}

typedef struct {
    char *data;
    int count, capacity;
} render_list;

typedef struct {
    float start[3], end[3], start2[3], end2[3], orientation[3], u[2], v[2], width;
    uint8_t colour[4], tangent[4][4], normal[4][4];
} beam_segment;
C_ASSERT(sizeof(beam_segment) == 0x74);

static float mix(float a, float b, float t)
{
    return a + t * (b - a);
}

static float catmull(float p, float a, float b, float q, float t)
{
    return a + .5f*t*((b-p) + t*((2*p-5*a+4*b-q) + t*(3*a-3*b+q-p)));
}

static void smooth_beam(render_list *nodes, int subdivisions)
{
    beam_segment *segments = (beam_segment *)nodes->data;
    const int count = nodes->count;
    int expanded = count * subdivisions;
    if (expanded > nodes->capacity) expanded = nodes->capacity;
    if (expanded > 128) expanded = 128;
    if (expanded <= count) return;

    /* Expand backwards so the native rows before this one are still available. */
    beam_segment next = segments[count-1];
    for (int i = count - 1; i >= 0; --i) {
        beam_segment current = segments[i];
        const float *previous = i ? segments[i-1].start : current.start;
        const float *following = next.end;
        int begin = i*expanded/count, end = (i+1)*expanded/count;
        for (int j = begin; j < end; ++j) {
            float t = (float)(j-begin)/(end-begin), s = (float)(j+1-begin)/(end-begin);
            beam_segment *part = &segments[j];
            *part = current;
            for (int k = 0; k < 3; ++k) {
                part->start[k] = catmull(previous[k], current.start[k], current.end[k], following[k], t);
                part->end[k] = catmull(previous[k], current.start[k], current.end[k], following[k], s);
            }
            part->u[0] = mix(current.u[0], current.u[1], t);
            part->u[1] = mix(current.u[0], current.u[1], s);
            part->width = mix(current.width, next.width, t);
            for (int k = 0; k < 4; ++k) {
                part->colour[k] = (uint8_t)lroundf(mix(current.colour[k], next.colour[k], t));
                for (int side = 0; side < 2; ++side) {
                    part->tangent[side][k] = (uint8_t)lroundf(mix(current.tangent[side][k], current.tangent[side+2][k], t));
                    part->tangent[side+2][k] = (uint8_t)lroundf(mix(current.tangent[side][k], current.tangent[side+2][k], s));
                }
            }
        }
        next = current;
    }
    nodes->count = expanded;
}

static void ribbon_draw(void *ribbon, int time, void *model, const float *axis, const float *origin)
{
    const char *instance = (const char *)ribbon;
    const char *decl = *(const char *const *)(instance + 0x48);
    const sh_ribbon_noise *params = decl ? (const sh_ribbon_noise *)(decl + 0xf4) : NULL;
    render_list *beams = NULL;
    int before = -1;
    int subdivisions = 1;
    int mask_alpha = -1;
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
    if (model && decl && *(const float *)(decl + 0xd4) == SH_RIBBON_TIMED_KNOTS &&
        !instance[0x3c60] && sh_ribbon_motion_owned(*(const char *const *)(decl + 8))) {
        float steps = *(const float *)(decl + 0xe4);
        double fade_start = *(const float *)(decl + 0xe8), deadline = -*(const float *)(decl + 0xec);
        if (isfinite(deadline) && fade_start >= 0 && deadline > 0 && fade_start <= deadline) {
            double age = (double)time - *(const int *)(instance + 0x10);
            if (age >= deadline || *(const int *)instance == 1) mask_alpha = 0;
            else if (age <= fade_start) mask_alpha = 255;
            else {
                double remaining = (deadline - age) / (deadline - fade_start);
                if (*(const float *)(decl + 0xd8) == 11) remaining = 1 - (1-remaining)*(1-remaining);
                mask_alpha = (int)lround(255 * remaining);
            }
        }
        if (steps >= 2 && steps <= 65535 && steps == floorf(steps)) {
            subdivisions = (int)steps;
        }
        if (subdivisions > 1 || mask_alpha >= 0) {
            beams = (render_list *)((char *)model + 0xe90);
            before = beams->count;
        }
    }
    original_draw(ribbon, time, model, axis, origin);
    /* Only the new beam belongs to this draw. Its nodes already carry the native width and taper. */
    if (beams && before >= 0 && before < 64 && beams->count == before + 1 &&
        beams->count <= beams->capacity && beams->data) {
        render_list *nodes = (render_list *)(beams->data + (size_t)before * 0x3a28);
        if (nodes->data && nodes->count > 0 && nodes->count <= nodes->capacity && nodes->count <= 128) {
            if (subdivisions > 1) smooth_beam(nodes, subdivisions);
            if (mask_alpha >= 0) {
                beam_segment *segments = (beam_segment *)nodes->data;
                for (int i = 0; i < nodes->count; ++i) {
                    for (int edge = 0; edge < 4; ++edge) segments[i].tangent[edge][0] = (uint8_t)mask_alpha;
                }
            }
            for (int i = 0; factor != 1 && i < nodes->count; ++i) {
                float *half_width = (float *)(nodes->data + (size_t)i * 0x74 + 0x4c);
                float width = *half_width * factor;
                if (isfinite(width)) *half_width = width;
            }
        }
    }
}

static void ribbon_node(void *ribbon, int time, const float *a, const float *b,
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

static void ribbon_birth(void *ribbon, int time, const float *a, const float *b,
                         const float *axis, const float *velocity, const float *colour)
{
    const char *decl = *(const char *const *)((const char *)ribbon + 0x48);
    if (a == b && decl && sampled_birth((char *)ribbon, decl, time, a, axis, velocity, colour)) return;
    ribbon_node(ribbon, time, a, b, axis, velocity, colour);
}

#ifndef SH_RIBBON_MOTION_TEST
int sh_ribbon_motion_install(const sig_result *results, size_t count)
{
    view_distance = active_view_distance;
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
