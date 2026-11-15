/* postfx_blurs.c -- the radial blurs postfx.c keeps and the parameters the post programs read (postfx.h).
 * Up to SH_POSTFX_KEPT blurs are kept, the oldest replaced; SH_POSTFX_DRAWN at a time are drawn (the most recent).
 * Kept apart from the engine side so tests/postfx_test.c links it alone. */
#include <windows.h>
#include <string.h>

#include "postfx.h"

#define POSTFX_SLOTS  SH_POSTFX_KEPT
#define POSTFX_DRAWN  SH_POSTFX_DRAWN

typedef struct pf_blur {
    float centre[3];
    int start, fade_in, fade_out, end;       /* game ms */
    float radius;
    float color[4], extents[3], size;
} pf_blur;

static SRWLOCK g_lock = SRWLOCK_INIT;
static pf_blur g_blurs[POSTFX_SLOTS];
static int g_next, g_count;

int sh_postfx_shape_drawn(const float color[4], const float extents[3], float size, float radius)
{
    (void)color; (void)size;
    return extents && extents[0] > 0.0f && radius > 0.0f;
}

void sh_postfx_keep(const float centre[3], int start, int fade_in, int fade_out, int duration, float radius,
                    const float color[4], const float extents[3], float size)
{
    pf_blur b;
    int i;
    if (!sh_postfx_shape_drawn(color, extents, size, radius)) return;
    memset(&b, 0, sizeof b);
    memcpy(b.centre, centre, sizeof b.centre);
    b.start = start; b.fade_in = fade_in; b.fade_out = fade_out;
    b.end = start + (fade_in + fade_out > duration ? fade_in + fade_out : duration);
    b.radius = radius;
    memcpy(b.color, color, sizeof b.color);
    memcpy(b.extents, extents, sizeof b.extents);
    b.size = size;
    AcquireSRWLockExclusive(&g_lock);
    /* one item plays to every player view: one blur */
    for (i = 0; i < g_count; i++)
        if (g_blurs[i].start == start && !memcmp(g_blurs[i].centre, centre, sizeof b.centre) && g_blurs[i].radius == radius) {
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    g_blurs[g_next] = b;
    g_next = (g_next + 1) % POSTFX_SLOTS;
    if (g_count < POSTFX_SLOTS) g_count++;
    ReleaseSRWLockExclusive(&g_lock);
}

/* The item's strength at `now`: 0 -> 1 over the fade in, 1, then -> 0 over the fade out before its end (the
 * engine's relay for the same item). */
float sh_postfx_envelope(int start, int fade_in, int fade_out, int end, int now)
{
    int t = now - start;
    if (t < 0 || now >= end) return 0.0f;
    if (fade_in > 0 && t < fade_in) return (float)t / (float)fade_in;
    if (fade_out > 0 && now > end - fade_out) return (float)(end - now) / (float)fade_out;
    return 1.0f;
}

int sh_postfx_parms(int now, float ysign, float out[SH_POSTFX_PARMS][4])
{
    pf_blur cur[POSTFX_SLOTS];
    int n, i, k, drawn = 0;
    float len = 0.0f;
    memset(out, 0, SH_POSTFX_PARMS * 4 * sizeof(float));
    AcquireSRWLockShared(&g_lock);
    n = g_count;
    for (i = 0; i < n; i++) cur[i] = g_blurs[(g_next - 1 - i + POSTFX_SLOTS * 2) % POSTFX_SLOTS];   /* newest first */
    ReleaseSRWLockShared(&g_lock);
    for (k = 0; k < n && drawn < POSTFX_DRAWN; k++) {
        const pf_blur *b = &cur[k];
        float s = sh_postfx_envelope(b->start, b->fade_in, b->fade_out, b->end, now) * b->color[1];
        float *a, *q, *e = out[SH_POSTFX_PARMS - 1];
        if (s <= 0.0f) continue;
        a = out[1 + 2 * drawn]; q = out[2 + 2 * drawn];
        a[0] = b->centre[0]; a[1] = b->centre[1]; a[2] = b->centre[2]; a[3] = s;
        q[0] = b->radius; q[1] = b->color[0]; q[2] = b->color[2]; q[3] = b->extents[0];
        if (!drawn) { e[0] = b->extents[1]; e[1] = b->extents[2]; e[2] = b->size; }     /* the newest blur's shape */
        if (b->color[3] > len) len = b->color[3];
        drawn++;
    }
    out[0][0] = ysign;
    out[0][1] = (float)drawn;
    out[0][2] = len;
    return drawn;
}

void sh_postfx_forget(void)
{
    AcquireSRWLockExclusive(&g_lock);
    memset(g_blurs, 0, sizeof g_blurs);
    g_next = g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
}
