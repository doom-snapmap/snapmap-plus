/* particlemotion.c -- simulated motion for a CPU particle stage: the detour of the particle vertex writer
 * (particlemotion.h).
 *
 * The engine's per-stage job (VK 0x19B1400) walks a stage's particles and calls the writer for each one that is
 * alive: writer(context, generation, cursor). `context` is the job's own record of the stage being drawn (its
 * stage pointer first; the emitter's axes at +0x48, times its scale, and origin at +0x6C as the origin function
 * applies them, the identity and zero for a stage that is not transparency sorted; the view's origin at +0xB4),
 * `generation` the particle (index, age in seconds, life in seconds, two fractions of
 * its life, its seed), `cursor` where the vertices go (48 bytes each, position first). Both records live on the
 * job's stack, so nothing here is shared between threads.
 *
 * For a stage that asks (sh_motion_asked) the game's writer runs first and writes the quad at the game's own
 * place. The detour then asks the game's origin function where the particle was born and where it is a moment
 * later (its launch velocity, times the stage's launch speed factor, plus its launch ball), where the game put it now, integrates the stage's
 * forces from the birth
 * (sh_motion_run) and moves the quad (sh_motion_place). A trail or a mesh (more or fewer than four vertices) is
 * left where the game put it. */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "particlemotion.h"
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

typedef uint64_t (*write_fn)(void **context, void *generation, void **cursor);
typedef float *(*origin_fn)(float *velocity, void **context, void *generation, float *origin);

static write_fn g_write;
static origin_fn g_origin;
static volatile LONG g_moved;

static void pm_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void *clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (void *)r[i].addr;
    return NULL;
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

static void move_quad(void **context, const uint8_t *stage, const uint32_t *born, float *verts)
{
    const float *c = (const float *)context;
    sh_motion m;
    sh_motion_frame fr;
    float age, life, span, scale, len[3], p0[3], p1[3], pe[3], v0[3], p[3], v[3], eye[3];
    int i;
    memcpy(&age, &born[1], sizeof age);
    memcpy(&life, &born[2], sizeof life);
    if (!(age > 0.0f) || !(life > 0.0f)) return;
    sh_motion_read(stage, &m);
    for (i = 0; i < 3; i++) {
        fr.x[i] = c[(CTX_AXIS + 4 * i) / 4];
        fr.y[i] = c[(CTX_AXIS + 12 + 4 * i) / 4];
        fr.z[i] = c[(CTX_AXIS + 24 + 4 * i) / 4];
        fr.origin[i] = c[(CTX_ORIGIN + 4 * i) / 4];
        eye[i] = c[(CTX_EYE + 4 * i) / 4];
    }
    /* the axes carry the emitter's scale: take it out of them, and below out of the launch velocity */
    len[0] = sqrtf(fr.x[0] * fr.x[0] + fr.x[1] * fr.x[1] + fr.x[2] * fr.x[2]);
    len[1] = sqrtf(fr.y[0] * fr.y[0] + fr.y[1] * fr.y[1] + fr.y[2] * fr.y[2]);
    len[2] = sqrtf(fr.z[0] * fr.z[0] + fr.z[1] * fr.z[1] + fr.z[2] * fr.z[2]);
    scale = sh_motion_scale(len);
    for (i = 0; i < 3; i++) {
        if (len[0] > 1e-6f) fr.x[i] /= len[0];
        if (len[1] > 1e-6f) fr.y[i] /= len[1];
        if (len[2] > 1e-6f) fr.z[i] /= len[2];
    }
    span = life < LAUNCH_SPAN ? life : LAUNCH_SPAN;
    origin_at(context, born, 0.0f, life, p0);
    origin_at(context, born, span, life, p1);
    origin_at(context, born, age, life, pe);
    for (i = 0; i < 3; i++) v0[i] = (p1[i] - p0[i]) / (span * scale);
    sh_motion_launch(&m, born[5], v0);
    sh_motion_run(&m, &fr, born[5], age, life, scale, p0, v0, p, v);
    sh_motion_place(verts, VERT_STRIDE, *(const int *)(stage + SH_MOTION_STAGE_ORIENTATION) == 3, pe, p, v, eye,
                    m.stretch);
    InterlockedIncrement(&g_moved);
}

static uint64_t write_detour(void **context, void *generation, void **cursor)
{
    const uint8_t *stage = context ? (const uint8_t *)context[0] : NULL;
    uint32_t born[8];
    float *verts;
    uint64_t n;
    if (!sh_motion_asked(stage) || !generation || !cursor) return g_write(context, generation, cursor);
    memcpy(born, generation, GEN_BYTES);
    verts = (float *)cursor[0];
    n = g_write(context, generation, cursor);
    if ((int)n == 4 && verts) {
        __try { move_quad(context, stage, born, verts); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return n;
}

long sh_particlemotion_moved(void)
{
    return InterlockedCompareExchange(&g_moved, 0, 0);
}

int sh_particlemotion_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    sig_result own[2];
    void *tramp;
    size_t i;
    (void)results; (void)count;
    if (g_write) return 1;
    for (i = 0; i < 2; i++) {
        if (sig_resolve_one(module_base, &SH_PARTICLEMOTION_SIGNATURES[i], &own[i]) != SIG_OK) {
            pm_log("PARTICLES: %s did not resolve on this executable; a stage's simulated motion is not drawn",
                   SH_PARTICLEMOTION_SIGNATURES[i].name);
            return 0;
        }
    }
    g_origin = (origin_fn)clean(own, 2, "ParticleOrigin");
    tramp = hook_prepare(clean(own, 2, "ParticleWriteVerts"), (void *)write_detour, WRITE_STOLEN);
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
    pm_log("PARTICLES: simulated motion ready for stages that ask (vectorFieldGPU.repeat on a CPU stage)");
    return 1;
}
