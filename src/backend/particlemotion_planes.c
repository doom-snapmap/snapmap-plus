/* particlemotion_planes.c -- the scene's surfaces a colliding particle stage bounces on (particlemotion.h).
 *
 * DOOM Eternal's hard collision tests a particle against the screen's depth on the GPU. A CPU particle of DOOM 2016
 * has no depth to test, and the particle jobs run off the game thread, where the engine's trace is not safe to
 * call. So the surfaces are found where it is: the jobs ask for an emitter's place (lock-free, best effort), the
 * game thread traces it once (sh_planes_service, from the script tick) and publishes the planes in a table the jobs
 * read under a sequence count. A place is the emitter's origin quantized to 8 units and its z axis to 1/8.
 *
 * The planes of a place: the surface under it (a trace 2,048 units down the world's z) and, for an emitter that
 * does not face up (an impact on a wall or a ceiling: its z axis is the surface's normal), the surface behind it (a
 * trace 256 units back along its z axis). A trace starts 8 units off the place, so an emitter born on the surface
 * finds that surface. Eternal's depth test sees every surface on the screen and none off it; these are two, on or
 * off the screen, extended as planes: the cost is written in report R-FXD section 1.
 *
 * Pure: no engine here, so tests/particlemotion_test.c links it with a trace of its own. */
#include <intrin.h>
#include <math.h>
#include <string.h>

#include "particlemotion.h"

#define TABLE      512
#define PROBE      8
#define RING       256
#define QUANT      8.0f
#define AXISQ      8.0f
#define DOWN       2048.0f
#define BEHIND     256.0f
#define LIFT       8.0f

typedef struct {
    volatile long long key;   /* 0: empty */
    volatile long seq;        /* even: stable; odd: being written by the game thread */
    long epoch;
    sh_motion_planes planes;
} plane_row;

typedef struct {
    volatile long state;      /* 0 free, 1 being filled, 2 asked */
    float origin[3], zaxis[3];
} ask_row;

static plane_row g_rows[TABLE];
static ask_row g_asks[RING];
static volatile long g_ask_next;
static long g_epoch = 1;

static long long place_key(const float o[3], const float z[3])
{
    unsigned long long h = 1469598103934665603ULL;
    int i;
    for (i = 0; i < 6; i++) {
        float f = i < 3 ? o[i] / QUANT : z[i - 3] * AXISQ;
        long q = (long)floorf(f + 0.5f);
        h = (h ^ (unsigned long long)(unsigned long)q) * 1099511628211ULL;
    }
    return (long long)(h | 1ULL);   /* never 0 */
}

static plane_row *find(long long key)
{
    unsigned i, at = (unsigned)((unsigned long long)key >> 7) % TABLE;
    for (i = 0; i < PROBE; i++) {
        plane_row *r = &g_rows[(at + i) % TABLE];
        if (r->key == key) return r;
    }
    return NULL;
}

static void ask(const float o[3], const float z[3])
{
    long slot = (_InterlockedIncrement(&g_ask_next) - 1) & (RING - 1);
    ask_row *a = &g_asks[slot];
    if (_InterlockedCompareExchange(&a->state, 1, 0) != 0) return;   /* the ring is full: asked again next frame */
    memcpy(a->origin, o, sizeof a->origin);
    memcpy(a->zaxis, z, sizeof a->zaxis);
    _ReadWriteBarrier();
    a->state = 2;
}

int sh_planes_lookup(const float origin[3], const float zaxis[3], sh_motion_planes *out)
{
    long long key = place_key(origin, zaxis);
    plane_row *r = find(key);
    long s1, s2, epoch;
    if (r) {
        s1 = r->seq;
        _ReadWriteBarrier();
        if (!(s1 & 1) && r->key == key) {
            *out = r->planes;
            epoch = r->epoch;
            _ReadWriteBarrier();
            s2 = r->seq;
            if (s1 == s2 && r->key == key) {
                if (g_epoch - epoch > SH_PLANES_STALE) ask(origin, zaxis);   /* kept, and traced again */
                return 1;
            }
        }
    }
    ask(origin, zaxis);
    return 0;
}

static void plane_of(const float end[3], const float n[3], sh_motion_planes *out)
{
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    int k = out->count, i;
    if (k >= SH_MOTION_PLANES || !(len > 0.5f && len < 2.0f)) return;
    for (i = 0; i < 3; i++) out->n[k][i] = n[i] / len;
    out->d[k] = out->n[k][0] * end[0] + out->n[k][1] * end[1] + out->n[k][2] * end[2];
    out->count = k + 1;
}

void sh_planes_find(sh_planes_trace_fn trace, const float o[3], const float z[3], sh_motion_planes *out)
{
    float from[3], to[3], end[3], n[3], len;
    int i;
    memset(out, 0, sizeof *out);
    if (!trace) return;
    from[0] = o[0]; from[1] = o[1]; from[2] = o[2] + LIFT;
    to[0] = o[0]; to[1] = o[1]; to[2] = o[2] - DOWN;
    if (trace(from, to, end, n)) plane_of(end, n, out);
    len = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (len > 1e-6f && z[2] / len < 0.7f) {          /* an emitter that does not face up: the surface behind it */
        for (i = 0; i < 3; i++) {
            from[i] = o[i] + z[i] / len * LIFT;
            to[i] = o[i] - z[i] / len * BEHIND;
        }
        if (trace(from, to, end, n)) {
            /* the same surface as the floor's (an emitter facing down under nothing but the floor) is kept once */
            if (!(out->count == 1 && fabsf(out->n[0][0] * n[0] + out->n[0][1] * n[1] + out->n[0][2] * n[2] - 1.0f) < 1e-3f &&
                  fabsf(out->n[0][0] * end[0] + out->n[0][1] * end[1] + out->n[0][2] * end[2] - out->d[0]) < 1.0f))
                plane_of(end, n, out);
        }
    }
}

static void publish(long long key, const sh_motion_planes *p)
{
    unsigned i, at = (unsigned)((unsigned long long)key >> 7) % TABLE;
    plane_row *r = NULL, *oldest = NULL;
    for (i = 0; i < PROBE; i++) {
        plane_row *c = &g_rows[(at + i) % TABLE];
        if (c->key == key || c->key == 0) { r = c; break; }
        if (!oldest || c->epoch < oldest->epoch) oldest = c;
    }
    if (!r) r = oldest;
    _InterlockedIncrement(&r->seq);                  /* odd: readers retry */
    _ReadWriteBarrier();
    r->key = key;
    r->planes = *p;
    r->epoch = g_epoch;
    _ReadWriteBarrier();
    _InterlockedIncrement(&r->seq);
}

int sh_planes_service(sh_planes_trace_fn trace, int budget)
{
    int done = 0, i;
    g_epoch++;
    for (i = 0; i < RING && done < budget; i++) {
        ask_row *a = &g_asks[i];
        float o[3], z[3];
        long long key;
        plane_row *r;
        if (a->state != 2) continue;
        memcpy(o, a->origin, sizeof o);
        memcpy(z, a->zaxis, sizeof z);
        _ReadWriteBarrier();
        a->state = 0;
        key = place_key(o, z);
        r = find(key);
        if (r && g_epoch - r->epoch <= SH_PLANES_STALE) continue;   /* asked by many particles: traced once */
        {
            sh_motion_planes p;
            sh_planes_find(trace, o, z, &p);
            publish(key, &p);
        }
        done++;
    }
    return done;
}

void sh_planes_clear(void)
{
    int i;
    for (i = 0; i < TABLE; i++) {
        _InterlockedIncrement(&g_rows[i].seq);
        g_rows[i].key = 0;
        g_rows[i].epoch = 0;
        _InterlockedIncrement(&g_rows[i].seq);
    }
}
