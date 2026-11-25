/* particlemotion.c -- the detour of the particle vertex writer (particlemotion.h). The per-stage job (VK 0x19B1400)
 * calls writer(context, generation, cursor) per live particle; for a stage that asks, the game writes the quad
 * first and the detour moves it (a trail or a mesh stays where the game put it). */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "particlemotion.h"
#include "script/script_api_common.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"

#define WRITE_STOLEN   19      /* mov rax,rsp; mov [rax+18h],r8; push rbp, rbx, rsi, rdi, r12, r13, r14, r15 */
#define CTX_AXIS       0x48    /* x axis; y at +0x54, z at +0x60: world = x * l.x + y * l.y + z * l.z + origin */
#define CTX_ORIGIN     0x6C
#define CTX_EYE        0xB4
#define GEN_BYTES      28      /* int index; float age, life, fraction, fraction; uint seed, seed */
#define VERT_STRIDE    0x30
#define LAUNCH_SPAN    (1.0f / 64.0f)   /* seconds between the two origins the launch velocity is taken from */
#define CTX_COPY       0x100   /* the context bytes a parent's origin is asked with (the stage pointer first) */
#define CONTEXT_STOLEN 15      /* sub rsp,18h; mov eax,[r9]; mov r11,rcx; mov r10,[rsp+58h] */

typedef uint64_t (*write_fn)(void **context, void *generation, void **cursor);
typedef float *(*origin_fn)(float *velocity, void **context, void *generation, float *origin);
typedef void (*context_fn)(void *job, char sorted, const float *origin, const float *axes, const float *scale,
                           const void *a6, const float *a7, void *context);

static write_fn g_write;
static origin_fn g_origin;
static context_fn g_context;

/* The fx item's scale of the stage this thread last built in its model's axes. The job builds the context and then
 * writes the particles on the same thread (VK 0x1419B1400), and the builder multiplies the axes by the scale only
 * for a sorted stage. */
static __declspec(thread) void *t_model_context;
static __declspec(thread) float t_model_scale;
static __declspec(thread) float t_model_origin[3], t_model_axes[9];   /* the model in the world: o + A p */

static void context_detour(void *job, char sorted, const float *origin, const float *axes, const float *scale,
                           const void *a6, const float *a7, void *context)
{
    g_context(job, sorted, origin, axes, scale, a6, a7, context);
    t_model_context = NULL;
    if (sorted || !scale || !context) return;
    __try {
        float s = sh_motion_scale(scale);
        memcpy(t_model_origin, origin, sizeof t_model_origin);
        memcpy(t_model_axes, axes, sizeof t_model_axes);
        t_model_scale = s;
        t_model_context = context;
    } __except (EXCEPTION_EXECUTE_HANDLER) { t_model_context = NULL; }
}
static volatile LONG g_moved, g_collided, g_killed, g_children;
static volatile LONG g_said_local, g_say_local, g_said_planes;
static volatile LONG g_trace;            /* particles still to be written to the log (sh_particles trace N) */
static volatile LONG g_trace_ctx;        /* contexts still to be dumped with them (the first few of a trace) */

static void pm_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

/* The game's origin for this particle at another age. */
static void origin_at(void **context, const uint32_t *born, float age, float life, float out[3])
{
    uint32_t gen[8];
    float vel[4], org[4], x = life > 1e-6f ? age / life : 1.0f;
    memcpy(gen, born, GEN_BYTES);
    gen[7] = 0;
    memcpy(&gen[1], &age, sizeof age);
    memcpy(&gen[3], &x, sizeof x);
    memcpy(&gen[4], &x, sizeof x);
    g_origin(vel, context, gen, org);
    out[0] = org[0]; out[1] = org[1]; out[2] = org[2];
}

/* Reads the emitter's frame from the context: unit axes, origin and eye; returns the scale. For a stage built in its
 * model's axes the scale comes from context_detour and *model (when given) is set, since the game's places are then
 * unscaled. */
static float read_frame_ex(void **context, sh_motion_frame *fr, float eye[3], int *model);

static float read_frame(void **context, sh_motion_frame *fr, float eye[3])
{
    return read_frame_ex(context, fr, eye, NULL);
}

static float read_frame_ex(void **context, sh_motion_frame *fr, float eye[3], int *model)
{
    const float *c = (const float *)context;
    float len[3], scale;
    int i;
    for (i = 0; i < 3; i++) {
        fr->x[i] = c[(CTX_AXIS + 4 * i) / 4];
        fr->y[i] = c[(CTX_AXIS + 12 + 4 * i) / 4];
        fr->z[i] = c[(CTX_AXIS + 24 + 4 * i) / 4];
        fr->origin[i] = c[(CTX_ORIGIN + 4 * i) / 4];
        if (eye) eye[i] = c[(CTX_EYE + 4 * i) / 4];
    }
    /* the axes carry the emitter's scale: take it out of them, and out of the launch velocity */
    len[0] = sqrtf(fr->x[0] * fr->x[0] + fr->x[1] * fr->x[1] + fr->x[2] * fr->x[2]);
    len[1] = sqrtf(fr->y[0] * fr->y[0] + fr->y[1] * fr->y[1] + fr->y[2] * fr->y[2]);
    len[2] = sqrtf(fr->z[0] * fr->z[0] + fr->z[1] * fr->z[1] + fr->z[2] * fr->z[2]);
    scale = sh_motion_scale(len);
    if (model) *model = 0;
    if (scale == 1.0f && t_model_context == (void *)context) {
        scale = t_model_scale;
        if (model) *model = 1;
    }
    for (i = 0; i < 3; i++) {
        if (len[0] > 1e-6f) fr->x[i] /= len[0];
        if (len[1] > 1e-6f) fr->y[i] /= len[1];
        if (len[2] > 1e-6f) fr->z[i] /= len[2];
    }
    return scale;
}

/* What a traced particle's flight started from (sh_particlemotion_trace). */
typedef struct pm_start {
    float p0[3], v0[3], scale;
    sh_motion_frame fr;
} pm_start;

/* A particle's flight to `age`: birth place and launch from the game's origin function (from `from` when given: a
 * child born at its parent's place), then the stage's simulated motion against the scene's planes. Returns 1 when
 * it died on a surface; *have says whether planes were used. */
static int flight(void **context, const uint8_t *stage, const uint32_t *born, float age, float life,
                  const float *from, float p[3], float v[3], int *have, pm_start *start)
{
    sh_motion m;
    sh_motion_frame fr;
    sh_motion_planes planes;
    float span, scale, p0[3], p1[3], v0[3];
    int i, model;
    *have = 0;
    sh_motion_read(stage, &m);
    scale = read_frame_ex(context, &fr, NULL, &model);
    span = life < LAUNCH_SPAN ? life : LAUNCH_SPAN;
    origin_at(context, born, 0.0f, life, p0);
    origin_at(context, born, span, life, p1);
    /* The game's places carry the scale in world axes but not in model axes, where the birth place is scaled here
     * (the displacement times the scale, the velocity not). */
    for (i = 0; i < 3; i++) v0[i] = (p1[i] - p0[i]) / (model ? span : span * scale);
    if (model) for (i = 0; i < 3; i++) p0[i] = fr.origin[i] + (p0[i] - fr.origin[i]) * scale;
    sh_motion_launch(&m, born[5], v0);
    if (from) for (i = 0; i < 3; i++) p0[i] = from[i] + (p0[i] - fr.origin[i]);
    if (start) {
        memcpy(start->p0, p0, sizeof start->p0);
        memcpy(start->v0, v0, sizeof start->v0);
        start->scale = scale;
        start->fr = fr;
    }
    if (sh_motion_collides(&m) && (stage[SH_MOTION_STAGE_FLAGS] & SH_MOTION_STAGE_SORTED)) {
        /* world axes: the planes of the emitter's place (asked for, and found on the game thread, when new) */
        if (sh_planes_lookup(fr.origin, fr.z, &planes) && planes.count > 0) *have = 1;
    } else if (sh_motion_collides(&m) && model) {
        /* the model's axes: the planes of the model's place in the world, brought into the model's frame (the
         * builder's origin o and unit axes A: a world plane n.x = d is A'n . p = d - n.o) */
        const float *A = t_model_axes, *o = t_model_origin;
        float zw[3] = { A[6], A[7], A[8] };
        if (sh_planes_lookup(o, zw, &planes) && planes.count > 0) {
            int j;
            for (j = 0; j < planes.count && j < SH_MOTION_PLANES; j++) {
                float n[3] = { planes.n[j][0], planes.n[j][1], planes.n[j][2] };
                planes.d[j] -= n[0] * o[0] + n[1] * o[1] + n[2] * o[2];
                for (i = 0; i < 3; i++) planes.n[j][i] = n[0] * A[3 * i] + n[1] * A[3 * i + 1] + n[2] * A[3 * i + 2];
            }
            *have = 1;
        }
    } else if (sh_motion_collides(&m) && !InterlockedCompareExchange(&g_said_local, 1, 0)) {
        g_say_local = 1;
    }
    if (!sh_motion_asked(stage)) {       /* a stage without simulated motion: the game's own closed form */
        float pa[3], pb[3];
        float a2 = age + span;
        origin_at(context, born, age, life, pa);
        origin_at(context, born, a2, life, pb);
        for (i = 0; i < 3; i++) { p[i] = pa[i]; v[i] = (pb[i] - pa[i]) / (model ? span : span * scale); }
        return 0;
    }
    return sh_motion_run_planes(&m, &fr, born[5], age, life, scale, p0, v0, *have ? &planes : NULL, p, v);
}

/* Logs one traced particle: the stage, the particle, what its flight started from (birth place, launch velocity,
 * emitter scale, origin and axes) and where it is (the game's place, the simulated place and velocity). */
static void trace_line(const uint8_t *stage, const uint32_t *born, float age, float life, const pm_start *s,
                       const float pe[3], const float p[3], const float v[3], int dead)
{
    short count;
    float maxlife;
    memcpy(&count, stage + SH_MOTION_STAGE_COUNT, sizeof count);
    memcpy(&maxlife, stage + SH_MOTION_STAGE_MAXLIFE, sizeof maxlife);
    pm_log("PARTICLES: trace stage=%p count=%d maxlife=%.4f sorted=%d index=%u age=%.5f life=%.5f seed=%u "
           "scale=%.5f origin=%.3f,%.3f,%.3f x=%.4f,%.4f,%.4f y=%.4f,%.4f,%.4f z=%.4f,%.4f,%.4f "
           "p0=%.3f,%.3f,%.3f v0=%.3f,%.3f,%.3f engine=%.3f,%.3f,%.3f p=%.3f,%.3f,%.3f v=%.3f,%.3f,%.3f dead=%d",
           (const void *)stage, (int)count, maxlife, (stage[SH_MOTION_STAGE_FLAGS] & SH_MOTION_STAGE_SORTED) ? 1 : 0,
           born[0], age, life, born[5], s->scale, s->fr.origin[0], s->fr.origin[1], s->fr.origin[2],
           s->fr.x[0], s->fr.x[1], s->fr.x[2], s->fr.y[0], s->fr.y[1], s->fr.y[2], s->fr.z[0], s->fr.z[1], s->fr.z[2],
           s->p0[0], s->p0[1], s->p0[2], s->v0[0], s->v0[1], s->v0[2], pe[0], pe[1], pe[2], p[0], p[1], p[2],
           v[0], v[1], v[2], dead);
}

/* Logs the job's raw context for the stage: its first 0x100 bytes as words, and for each of its first three pointer
 * slots the offsets (below 0x800) of floats between 1.05 and 4, which is where an fx scale shows up. */
static void trace_context(void **context)
{
    char line[4096];
    size_t n = 0;
    int i, k;
    const uint32_t *w = (const uint32_t *)context;
    n += (size_t)_snprintf_s(line + n, sizeof line - n, _TRUNCATE, "PARTICLES: trace context");
    for (i = 0; i < CTX_COPY / 4 && n < sizeof line - 16; i++)
        n += (size_t)_snprintf_s(line + n, sizeof line - n, _TRUNCATE, " %08x", w[i]);
    for (i = 0; i < 3 && n < sizeof line - 64; i++) {
        n += (size_t)_snprintf_s(line + n, sizeof line - n, _TRUNCATE, " [%d]", i);
        __try {
            const uint8_t *q = (const uint8_t *)context[i];
            for (k = 0; q && k < 0x800 && n < sizeof line - 32; k += 4) {
                float f;
                memcpy(&f, q + k, 4);
                if (f > 1.05f && f < 4.0f)
                    n += (size_t)_snprintf_s(line + n, sizeof line - n, _TRUNCATE, " +%x=%g", k, (double)f);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n += (size_t)_snprintf_s(line + n, sizeof line - n, _TRUNCATE, " -");
        }
    }
    backend_log(line);
}

static void fold(float *verts)          /* a particle that is not there: its quad has no area */
{
    int i, k;
    for (i = 1; i < 4; i++)
        for (k = 0; k < 3; k++) ((float *)((uint8_t *)verts + VERT_STRIDE * (size_t)i))[k] = verts[k];
}

/* Finds where a child particle's parent was when the child was born (particlemotion.h). Returns 1 with the place,
 * 0 when the parent did not yet exist (the child is not drawn), -1 for a non-family. */
static int parent_place(void **context, const uint8_t *child, const uint32_t *born, float at[3])
{
    const uint8_t *parent = sh_family_parent(child);
    uint8_t pctx[CTX_COPY];
    uint32_t prec[8];
    sh_motion_frame fr;
    float delta, plife, x, vel[3];
    int p, have;
    if (!parent || sh_family_role(parent) != 1) return -1;          /* not a family: drawn as its own */
    if (!sh_family_link(child, parent, (int)born[0], &p, &delta)) return -1;
    plife = sh_family_life(parent);
    if (!(delta >= 0.0f) || delta > plife) return 0;
    memcpy(pctx, context, sizeof pctx);
    *(const uint8_t **)pctx = parent;
    read_frame(context, &fr, NULL);
    if (t_model_context == (void *)context) t_model_context = pctx;      /* the parent: the same model's axes */
    memset(prec, 0, sizeof prec);
    prec[0] = (uint32_t)p;
    memcpy(&prec[1], &delta, sizeof delta);
    memcpy(&prec[2], &plife, sizeof plife);
    x = plife > 1e-6f ? delta / plife : 1.0f;
    memcpy(&prec[3], &x, sizeof x);
    memcpy(&prec[4], &x, sizeof x);
    prec[5] = prec[6] = sh_family_seed(sh_family_salt(fr.origin, parent), p);
    p = flight((void **)pctx, parent, prec, delta, plife, NULL, at, vel, &have, NULL) ? 0 : 1;
    if (t_model_context == (void *)pctx) t_model_context = context;
    return p;
}

static void move_quad(void **context, const uint8_t *stage, const uint32_t *born, float *verts)
{
    sh_motion m;
    sh_motion_frame fr;
    pm_start start;
    float age, life, pe[3], p[3], v[3], eye[3], from[3];
    int dead, have, role = sh_family_role(stage), parent = -1;
    memcpy(&age, &born[1], sizeof age);
    memcpy(&life, &born[2], sizeof life);
    if (!(age > 0.0f) || !(life > 0.0f)) return;
    if (role == 2) {
        parent = parent_place(context, stage, born, from);
        if (parent == 0) { fold(verts); return; }
    }
    sh_motion_read(stage, &m);
    read_frame(context, &fr, eye);
    origin_at(context, born, age, life, pe);
    dead = flight(context, stage, born, age, life, parent == 1 ? from : NULL, p, v, &have, &start);
    if (g_trace > 0 && InterlockedDecrement(&g_trace) >= 0) {
        trace_line(stage, born, age, life, &start, pe, p, v, dead);
        if (g_trace_ctx > 0 && InterlockedDecrement(&g_trace_ctx) >= 0) trace_context(context);
    }
    sh_motion_place(verts, VERT_STRIDE, *(const int *)(stage + SH_MOTION_STAGE_ORIENTATION) == 3, pe, p, v, eye,
                    m.stretch);
    if (dead) {                          /* killed on a surface: the particle is gone */
        fold(verts);
        InterlockedIncrement(&g_killed);
    }
    if (have) InterlockedIncrement(&g_collided);
    if (parent == 1) InterlockedIncrement(&g_children);
    InterlockedIncrement(&g_moved);
}

static uint64_t write_detour(void **context, void *generation, void **cursor)
{
    const uint8_t *stage = context ? (const uint8_t *)context[0] : NULL;
    uint32_t born[8];
    float *verts;
    uint64_t n;
    if (!sh_motion_asked(stage) || !generation || !cursor) return g_write(context, generation, cursor);
    if (sh_family_role(stage) == 1) {
        /* a parent: its seed is derived from its place, so its children can retrace it */
        __try {
            sh_motion_frame fr;
            uint32_t s;
            read_frame(context, &fr, NULL);
            s = sh_family_seed(sh_family_salt(fr.origin, stage), (int)((uint32_t *)generation)[0]);
            ((uint32_t *)generation)[5] = s;
            ((uint32_t *)generation)[6] = s;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    memcpy(born, generation, GEN_BYTES);
    verts = (float *)cursor[0];
    n = g_write(context, generation, cursor);
    if ((int)n == 4 && verts) {
        __try { move_quad(context, stage, born, verts); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return n;
}

void sh_particlemotion_trace(long n)
{
    InterlockedExchange(&g_trace, n < 0 ? 0 : (n > 4096 ? 4096 : n));
    InterlockedExchange(&g_trace_ctx, n > 0 ? 4 : 0);
}

long sh_particlemotion_moved(void)
{
    return InterlockedCompareExchange(&g_moved, 0, 0);
}

long sh_particlemotion_collided(void)
{
    return InterlockedCompareExchange(&g_collided, 0, 0);
}

long sh_particlemotion_killed(void)
{
    return InterlockedCompareExchange(&g_killed, 0, 0);
}

long sh_particlemotion_children(void)
{
    return InterlockedCompareExchange(&g_children, 0, 0);
}

/* Runs on the game thread once a frame with the script tick: traces the emitter places the particle jobs asked for. */
static void planes_frame(void)
{
    int n = sh_planes_service(sh_script_api_world_trace, 24);
    if (n > 0 && !g_said_planes) {
        g_said_planes = 1;
        pm_log("PARTICLES: the scene's surfaces found for %d emitter place(s); colliding stages bounce on them", n);
    }
    if (g_say_local == 1) {
        g_say_local = 2;
        pm_log("PARTICLES: a stage asks for collision but is built in its model's axes (not isTransparencySorted); "
               "its particles are drawn without collision");
    }
}

int sh_particlemotion_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    sig_result own[3];
    void *tramp;
    size_t i;
    (void)results; (void)count;
    if (g_write) return 1;
    for (i = 0; i < 3; i++) {
        if (sig_resolve_one(module_base, &SH_PARTICLEMOTION_SIGNATURES[i], &own[i]) != SIG_OK) {
            pm_log("PARTICLES: %s did not resolve on this executable; a stage's simulated motion is not drawn",
                   SH_PARTICLEMOTION_SIGNATURES[i].name);
            return 0;
        }
    }
    g_origin = (origin_fn)sig_pointer(own, 3, "ParticleOrigin");
    /* The context detour supplies the fx scale of a model-axes stage; without it such a stage draws at scale 1. */
    tramp = hook_prepare(sig_pointer(own, 3, "ParticleContext"), (void *)context_detour, CONTEXT_STOLEN);
    if (tramp && hook_commit(tramp) == B2_PATCH_OK) {
        g_context = (context_fn)tramp;
    } else {
        if (tramp) hook_unpatch(tramp);
        pm_log("PARTICLES: the stage context builder could not be detoured (%s); a stage in its model's axes is "
               "drawn at scale 1", hook_last_failure());
    }
    tramp = hook_prepare(sig_pointer(own, 3, "ParticleWriteVerts"), (void *)write_detour, WRITE_STOLEN);
    if (!tramp) {
        pm_log("PARTICLES: the particle vertex writer could not be detoured (%s); a stage's simulated motion is "
               "not drawn", hook_last_failure());
        return 0;
    }
    g_write = (write_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_write = NULL;
        pm_log("PARTICLES: the particle vertex writer's detour could not be committed; a stage's simulated motion "
               "is not drawn");
        return 0;
    }
    sh_script_api_on_frame(planes_frame);
    pm_log("PARTICLES: simulated motion ready for stages that ask (vectorFieldGPU.repeat on a CPU stage); "
           "collision with the scene in maps a mod script runs in");
    return 1;
}
