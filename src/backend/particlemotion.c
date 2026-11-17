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

typedef uint64_t (*write_fn)(void **context, void *generation, void **cursor);
typedef float *(*origin_fn)(float *velocity, void **context, void *generation, float *origin);

static write_fn g_write;
static origin_fn g_origin;
static volatile LONG g_moved, g_collided, g_killed, g_children;
static volatile LONG g_said_local, g_say_local, g_said_planes;

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

/* The emitter's frame from the context: its axes without the scale, the scale, its origin and the eye. */
static float read_frame(void **context, sh_motion_frame *fr, float eye[3])
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
    for (i = 0; i < 3; i++) {
        if (len[0] > 1e-6f) fr->x[i] /= len[0];
        if (len[1] > 1e-6f) fr->y[i] /= len[1];
        if (len[2] > 1e-6f) fr->z[i] /= len[2];
    }
    return scale;
}

/* A particle's flight to `age`: its birth place and launch by the game's origin function (from `from` when given:
 * a child born at its parent's place), then the stage's simulated motion against the scene's planes. 1 when it died
 * on a surface. `have` says whether planes were used. */
static int flight(void **context, const uint8_t *stage, const uint32_t *born, float age, float life,
                  const float *from, const float *inherit, float p[3], float v[3], int *have)
{
    sh_motion m;
    sh_motion_frame fr;
    sh_motion_planes planes;
    float span, scale, p0[3], p1[3], v0[3];
    int i;
    *have = 0;
    sh_motion_read(stage, &m);
    scale = read_frame(context, &fr, NULL);
    span = life < LAUNCH_SPAN ? life : LAUNCH_SPAN;
    origin_at(context, born, 0.0f, life, p0);
    origin_at(context, born, span, life, p1);
    for (i = 0; i < 3; i++) v0[i] = (p1[i] - p0[i]) / (span * scale);
    sh_motion_launch(&m, born[5], v0);
    if (from) for (i = 0; i < 3; i++) p0[i] = from[i] + (p0[i] - fr.origin[i]);
    if (inherit) for (i = 0; i < 3; i++) v0[i] += inherit[i];
    if (sh_motion_collides(&m) && (stage[SH_MOTION_STAGE_FLAGS] & SH_MOTION_STAGE_SORTED)) {
        /* world axes: the planes of the emitter's place (asked for, and found on the game thread, when new) */
        if (sh_planes_lookup(fr.origin, fr.z, &planes) && planes.count > 0) *have = 1;
    } else if (sh_motion_collides(&m) && !InterlockedCompareExchange(&g_said_local, 1, 0)) {
        g_say_local = 1;
    }
    if (!sh_motion_asked(stage)) {       /* a stage without simulated motion: the game's own closed form */
        float pa[3], pb[3];
        float a2 = age + span;
        origin_at(context, born, age, life, pa);
        origin_at(context, born, a2, life, pb);
        for (i = 0; i < 3; i++) { p[i] = pa[i]; v[i] = (pb[i] - pa[i]) / (span * scale); }
        return 0;
    }
    return sh_motion_run_planes(&m, &fr, born[5], age, life, scale, p0, v0, *have ? &planes : NULL, p, v);
}

static void fold(float *verts)          /* a particle that is not there: its quad has no area */
{
    int i, k;
    for (i = 1; i < 4; i++)
        for (k = 0; k < 3; k++) ((float *)((uint8_t *)verts + VERT_STRIDE * (size_t)i))[k] = verts[k];
}

/* A child particle (sh_family_role 2): where its parent was when it was born (particlemotion.h). 1 and the place
 * (and the parent's velocity there) when the parent existed then; 0 when it did not (the child is not drawn). */
static int parent_place(void **context, const uint8_t *child, const uint32_t *born, float at[3], float vel[3])
{
    const uint8_t *parent = sh_family_parent(child);
    uint8_t pctx[CTX_COPY];
    uint32_t prec[8];
    sh_motion_frame fr;
    float delta, plife, x;
    int p, have;
    if (!parent || sh_family_role(parent) != 1) return -1;          /* not a family: drawn as its own */
    if (!sh_family_link(child, parent, (int)born[0], &p, &delta)) return -1;
    plife = sh_family_life(parent);
    if (!(delta >= 0.0f) || delta > plife) return 0;
    memcpy(pctx, context, sizeof pctx);
    *(const uint8_t **)pctx = parent;
    read_frame(context, &fr, NULL);
    memset(prec, 0, sizeof prec);
    prec[0] = (uint32_t)p;
    memcpy(&prec[1], &delta, sizeof delta);
    memcpy(&prec[2], &plife, sizeof plife);
    x = plife > 1e-6f ? delta / plife : 1.0f;
    memcpy(&prec[3], &x, sizeof x);
    memcpy(&prec[4], &x, sizeof x);
    prec[5] = prec[6] = sh_family_seed(sh_family_salt(fr.origin, parent), p);
    return flight((void **)pctx, parent, prec, delta, plife, NULL, NULL, at, vel, &have) ? 0 : 1;
}

static void move_quad(void **context, const uint8_t *stage, const uint32_t *born, float *verts)
{
    sh_motion m;
    sh_motion_frame fr;
    float age, life, pe[3], p[3], v[3], eye[3], from[3], pvel[3], inherit[3];
    int i, dead, have, role = sh_family_role(stage), parent = -1;
    memcpy(&age, &born[1], sizeof age);
    memcpy(&life, &born[2], sizeof life);
    if (!(age > 0.0f) || !(life > 0.0f)) return;
    if (role == 2) {
        parent = parent_place(context, stage, born, from, pvel);
        if (parent == 0) { fold(verts); return; }
        for (i = 0; i < 3; i++) inherit[i] = pvel[i] * sh_family_inherit(stage);
    }
    sh_motion_read(stage, &m);
    read_frame(context, &fr, eye);
    origin_at(context, born, age, life, pe);
    dead = flight(context, stage, born, age, life, parent == 1 ? from : NULL, parent == 1 ? inherit : NULL, p, v,
                  &have);
    sh_motion_place(verts, VERT_STRIDE, *(const int *)(stage + SH_MOTION_STAGE_ORIENTATION) == 3, pe, p, v, eye,
                    m.stretch);
    if (dead) {                          /* Eternal's COLLISION_KILL: the particle is gone */
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
        /* a parent: its random numbers are the product's, so its children can retrace it */
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

/* The game thread, once a frame with the script tick: trace the emitter places the particle jobs asked for. */
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
    sh_script_api_on_frame(planes_frame);
    pm_log("PARTICLES: simulated motion ready for stages that ask (vectorFieldGPU.repeat on a CPU stage); "
           "collision with the scene in maps a mod script runs in");
    return 1;
}
