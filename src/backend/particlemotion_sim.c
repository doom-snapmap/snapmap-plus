/* particlemotion_sim.c -- the particle simulation and quad placement of particlemotion.h; pure, no engine calls.
 * The forces follow DOOM Eternal's `gpuparticlesimulate` commands (VORTEX, ACCELERATION, BROWNIAN, ATTRACTOR_*,
 * EMITTER_DRAG, INTEGRATE); Doomforge's doom2016/effects/particle_motion.py is the same arithmetic, so change both. */
#include <math.h>
#include <string.h>

#include "particlemotion.h"

static float rd(const uint8_t *stage, size_t off)
{
    float v;
    memcpy(&v, stage + off, sizeof v);
    return v;
}

int sh_motion_asked(const uint8_t *stage)
{
    return stage && stage[SH_MOTION_FIELD_REPEAT] && !(stage[SH_MOTION_STAGE_FLAGS] & SH_MOTION_STAGE_GPU);
}

void sh_motion_read(const uint8_t *stage, sh_motion *m)
{
    int i;
    for (i = 0; i < 3; i++) {
        m->accel[i] = rd(stage, SH_MOTION_ACCELERATION + 4 * i);
        m->wind[i] = rd(stage, SH_MOTION_VELOCITY_OFF + 4 * i);
        m->vortex_axis[i] = rd(stage, SH_MOTION_FIELD_OFFSET + 4 * i);
        m->attract_at[i] = rd(stage, SH_MOTION_DISTRIBUTION_OFF + 4 * i);
    }
    m->wind_drag = rd(stage, SH_MOTION_FIELD_TIGHTNESS);
    m->vortex_falloff = rd(stage, SH_MOTION_FIELD_ROTATION);
    m->vortex_speed = rd(stage, SH_MOTION_FIELD_ROTATION + 4);
    m->vortex_drag = rd(stage, SH_MOTION_FIELD_ROTATION + 8);
    m->noise_speed = rd(stage, SH_MOTION_FIELD_ROTATIONVAR);
    m->noise_scale = rd(stage, SH_MOTION_FIELD_ROTATIONVAR + 4);
    m->noise_drag = rd(stage, SH_MOTION_FIELD_ROTATIONVAR + 8);
    m->noise_strength = rd(stage, SH_MOTION_FIELD_SPEED);
    m->brownian = rd(stage, SH_MOTION_FIELD_SPEED + 4);
    m->stretch = rd(stage, SH_MOTION_FIELD_SPEED + 8);
    m->attract_kind = rd(stage, SH_MOTION_FIELD_SPEEDVAR);
    m->attract_strength = rd(stage, SH_MOTION_FIELD_SPEEDVAR + 4);
    m->attract_radius = rd(stage, SH_MOTION_FIELD_SPEEDVAR + 8);
    m->emitter_drag0 = rd(stage, SH_MOTION_GEOMETRY_SIZEVAR);
    m->emitter_drag1 = rd(stage, SH_MOTION_GEOMETRY_SIZEVAR + 4);
    m->emitter_drag_radius = rd(stage, SH_MOTION_PHYSICS_MASSVAR);
    m->launch = rd(stage, SH_MOTION_SYSTEM_BOUNDS);
    m->launch_lo = rd(stage, SH_MOTION_SYSTEM_BOUNDS + 4);
    m->launch_hi = rd(stage, SH_MOTION_SYSTEM_BOUNDS + 8);
    m->uniform_volume = rd(stage, SH_MOTION_FIELD_STRENGTH) == (1.0f / 3.0f);
    m->oriented_cone = m->uniform_volume && rd(stage, SH_MOTION_VELOCITY_SCALE + 8) == -1.0f;
    if (m->oriented_cone) {
        for (i = 0; i < 3; ++i) m->cone_rotation[i] = rd(stage, SH_MOTION_FIELD_SCALE + 4*i);
        m->cone_rotation[3] = rd(stage, SH_MOTION_DISTRIBUTION_SCALE + 8);
        for (i = 0; i < 2; ++i) m->cone_angle_span[i] = rd(stage, SH_MOTION_DISTRIBUTION_SCALE + 4*i);
    }
    for (i = 0; i < 2; i++) {
        m->cone_angles[i] = m->uniform_volume ? rd(stage, SH_MOTION_GEOMETRY_INITIAL + 4*i) : 0.0f;
        m->cone_speeds[i] = m->uniform_volume ? rd(stage, SH_MOTION_GEOMETRY_FINAL + 4*i) : 0.0f;
        m->ball_scale[i] = m->uniform_volume ? rd(stage, SH_MOTION_VELOCITY_SCALE + 4*i) : 1.0f;
    }
    m->collide_thickness = rd(stage, SH_MOTION_COLOR_FADE);
    m->collide_min_life = rd(stage, SH_MOTION_COLOR_FADE + 4);
    m->collide_penetration = rd(stage, SH_MOTION_COLOR_FADE + 8);
    m->collide_speed = rd(stage, SH_MOTION_COLOR_FADE + 12);
    m->collide_restitution = rd(stage, SH_MOTION_PHYSICS_RESTITUTION);
    m->collide_elasticity = rd(stage, SH_MOTION_PHYSICS_ELASTICITY);
    m->collide_offset = rd(stage, SH_MOTION_SYSTEM_TIMEOFFSET);
}

int sh_motion_collides(const sh_motion *m)
{
    return m && m->collide_thickness > 0.0f;
}

/* Tests the first plane the next step crosses (simulate commands 10 to 12: test, response, kill), changing p and
 * v. Returns 1 when the particle dies. */
static int collide(const sh_motion *m, const sh_motion_planes *pl, float t, float dt, float scale, float p[3],
                   float v[3])
{
    int j, i;
    if (!pl || !sh_motion_collides(m) || t < m->collide_min_life) return 0;
    for (j = 0; j < pl->count && j < SH_MOTION_PLANES; j++) {
        const float *n = pl->n[j];
        float q[3], s, k, r[3];
        for (i = 0; i < 3; i++) q[i] = p[i] + v[i] * dt * scale;
        s = n[0] * q[0] + n[1] * q[1] + n[2] * q[2] - pl->d[j] + m->collide_offset;
        if (!(s < 0.0f && s > -m->collide_thickness)) continue;
        for (i = 0; i < 3; i++) p[i] -= v[i] * dt * scale;
        k = 2.0f * (v[0] * n[0] + v[1] * n[1] + v[2] * n[2]);
        for (i = 0; i < 3; i++) r[i] = (v[i] - k * n[i]) * m->collide_elasticity;
        k = (1.0f - m->collide_restitution) * (r[0] * n[0] + r[1] * n[1] + r[2] * n[2]);
        for (i = 0; i < 3; i++) v[i] = r[i] - n[i] * k;
        return m->collide_speed > 0.0f && -s < m->collide_penetration &&
               v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < m->collide_speed * m->collide_speed;
    }
    return 0;
}

int sh_family_role(const uint8_t *stage)
{
    float e;
    if (!stage) return 0;
    e = rd(stage, SH_MOTION_SYSTEM_EMISSION);
    if (e < 0.0f) return 1;
    if (e >= 900.0f && e <= 1100.0f) return 2;
    return 0;
}

const uint8_t *sh_family_parent(const uint8_t *child)
{
    float e = rd(child, SH_MOTION_SYSTEM_EMISSION) - 1000.0f;
    int d = (int)floorf(e + 0.5f);
    if (d == 0 || d < -64 || d > 64) return NULL;
    return child + (ptrdiff_t)d * SH_MOTION_STAGE_SIZE;
}

static int count_of(const uint8_t *stage)
{
    short n;
    memcpy(&n, stage + SH_MOTION_STAGE_COUNT, sizeof n);
    return n;
}

/* Particle i's birth after its stage's cycle starts, ms, as the game's per-stage job has it (VK 0x1419B1400:
 * i x (int)(spawnBunching x bunchTime x 1000) / count, integer). */
static long long birth_ms(const uint8_t *stage, int i, int n)
{
    long long window = (long long)(rd(stage, SH_MOTION_STAGE_BUNCHING) * rd(stage, SH_MOTION_STAGE_BUNCHTIME) * 1000.0f);
    return n > 0 ? ((long long)i * window) / n : 0;
}

int sh_family_link(const uint8_t *child, const uint8_t *parent, int k, int *p, float *delta)
{
    int nc = count_of(child), np = count_of(parent);
    long long ms;
    if (nc <= 0 || np <= 0 || k < 0) return 0;
    k %= nc;
    *p = k % np;
    ms = birth_ms(child, k, nc) - birth_ms(parent, *p, np) +
         (long long)(rd(child, SH_MOTION_STAGE_TIMEOFFSET) * 1000.0f) -
         (long long)(rd(parent, SH_MOTION_STAGE_TIMEOFFSET) * 1000.0f);
    *delta = (float)ms * 0.001f;
    return 1;
}

float sh_family_life(const uint8_t *parent)
{
    return rd(parent, SH_MOTION_STAGE_MAXLIFE);
}

uint32_t sh_family_seed(uint32_t salt, int p)
{
    uint32_t x = (uint32_t)p * 0x9E3779B9u ^ salt ^ 0x5BD1E995u;
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint32_t sh_family_salt(const float o[3], const uint8_t *stage)
{
    uint32_t h = 2166136261u;
    int i;
    if (!stage || !(stage[SH_MOTION_STAGE_FLAGS] & SH_MOTION_STAGE_SORTED)) return 0;
    for (i = 0; i < 3; i++) {
        long q = (long)floorf(o[i] / 8.0f + 0.5f);
        h = (h ^ (uint32_t)q) * 16777619u;
    }
    return h;
}

float sh_motion_scale(const float f[3])
{
    float s = f[0] > f[1] ? f[0] : f[1];
    if (f[2] > s) s = f[2];
    return s > 0.001f && s < 1000.0f ? s : 1.0f;
}

float sh_motion_step(float life)
{
    float h = life / 96.0f;
    if (!(h >= 1.0f / 60.0f)) h = 1.0f / 60.0f;
    if (h > 0.05f) h = 0.05f;
    return h;
}

/* A uniform number in [0, 1) for a particle's seed, a step and one of its axes. */
static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static float uniform(uint32_t seed, uint32_t step, uint32_t axis)
{
    return (float)(mix(seed ^ mix(step * 3u + axis + 0x9E3779B9u)) >> 8) * (1.0f / 16777216.0f);
}

static float fract(float x) { return x - floorf(x); }

/* A uniform direction, with a radius drawn independently once at birth. */
static void to_world(const sh_motion_frame *fr, const float local[3], float world[3]);

void sh_motion_launch(const sh_motion *m, uint32_t seed, float v0[3], const sh_motion_frame *frame)
{
    float a, z, k, s;
    if (m->launch_lo != 0.0f || m->launch_hi != 0.0f) {
        s = m->launch_lo + (m->launch_hi - m->launch_lo) * uniform(seed, 0x7FFFFFFEu, 0u);
        v0[0] *= s; v0[1] *= s; v0[2] *= s;
    }
    if (m->uniform_volume && (m->cone_speeds[0] || m->cone_speeds[1])) {
        float pitch = m->cone_angles[0], yaw = m->cone_angles[1];
        if (m->oriented_cone) {
            pitch += m->cone_angle_span[0] * uniform(seed, 0x7FFFFFFBu, 0);
            yaw += m->cone_angle_span[1] * uniform(seed, 0x7FFFFFFBu, 1);
        }
        float theta = fmaxf(0.001f, 0.5f * (pitch + yaw));
        float azimuth = 6.2831855f * uniform(seed, 0x7FFFFFFDu, 0);
        float disc = sqrtf(uniform(seed, 0x7FFFFFFDu, 1)) * sinf(theta);
        float x = disc * cosf(azimuth) * pitch, y = disc * sinf(azimuth) * yaw;
        float up = theta * cosf(theta);
        if (m->oriented_cone) {
            const float *q = m->cone_rotation;
            float u = 2.0f*(q[1]*up - q[2]*y), v = 2.0f*(q[2]*x - q[0]*up), w = 2.0f*(q[0]*y - q[1]*x);
            float local[3] = {x+q[3]*u+q[1]*w-q[2]*v, y+q[3]*v+q[2]*u-q[0]*w, up+q[3]*w+q[0]*v-q[1]*u};
            float world[3];
            if (frame) { to_world(frame, local, world); x = world[0]; y = world[1]; up = world[2]; }
            else { x = local[0]; y = local[1]; up = local[2]; }
        }
        float norm = sqrtf(x*x + y*y + up*up);
        if (norm > 0) {
            float speed = m->cone_speeds[0] + (m->cone_speeds[1] - m->cone_speeds[0]) * uniform(seed, 0x7FFFFFFDu, 2);
            v0[0] += speed * x / norm; v0[1] += speed * y / norm; v0[2] += speed * up / norm;
        }
    }
    if (m->launch == 0.0f) return;
    a = 6.2831855f * uniform(seed, 0x7FFFFFFFu, 0u);
    z = 2.0f * uniform(seed, 0x7FFFFFFFu, 1u) - 1.0f;
    k = sqrtf(1.0f - z * z);
    s = uniform(seed, 0x7FFFFFFFu, 2u);
    s = m->launch * (m->uniform_volume ? cbrtf(s) : sqrtf(s));
    if (m->uniform_volume && (m->ball_scale[0] || m->ball_scale[1]))
        s *= m->ball_scale[0] + (m->ball_scale[1] - m->ball_scale[0]) * uniform(seed, 0x7FFFFFFCu, 0);
    v0[0] += k * cosf(a) * s;
    v0[1] += k * sinf(a) * s;
    v0[2] += z * s;
}

/* The vortex noise field: per unit cell a random axis, the field its cross product with the offset from the cell's
 * centre under a smooth kernel. */
static void noise(const float q[3], float out[3])
{
    float c[3], f[3], p[3], g[3], d, len2, w;
    int i;
    for (i = 0; i < 3; i++) { c[i] = floorf(q[i]); f[i] = q[i] - c[i] - 0.5f; }
    p[0] = fract(c[0] * 0.1031f); p[1] = fract(c[1] * 0.1030f); p[2] = fract(c[2] * 0.0973f);
    d = p[0] * (p[1] + 19.19f) + p[1] * (p[0] + 19.19f) + p[2] * (p[2] + 19.19f);
    p[0] += d; p[1] += d; p[2] += d;
    g[0] = (fract((p[0] + p[1]) * p[2]) - 0.5f) * 2.0f;
    g[1] = (fract((p[0] + p[2]) * p[1]) - 0.5f) * 2.0f;
    g[2] = (fract((p[1] + p[2]) * p[0]) - 0.5f) * 2.0f;
    len2 = f[0] * f[0] + f[1] * f[1] + f[2] * f[2];
    w = 1.0f - len2;
    w = len2 <= 1.0f ? 1.56668f * w * w * w : 0.0f;
    out[0] = (g[1] * f[2] - g[2] * f[1]) * w;
    out[1] = (g[2] * f[0] - g[0] * f[2]) * w;
    out[2] = (g[0] * f[1] - g[1] * f[0]) * w;
}

static void to_world(const sh_motion_frame *fr, const float l[3], float w[3])
{
    int i;
    for (i = 0; i < 3; i++) w[i] = fr->x[i] * l[0] + fr->y[i] * l[1] + fr->z[i] * l[2];
}

static int one_step(const sh_motion *m, const sh_motion_frame *fr, uint32_t seed, uint32_t index, float t,
                    float dt, float life, float scale, const sh_motion_planes *pl, float p[3], float v[3])
{
    float a[3], r[3], w[3], n[3], q[3], k, d2;
    int i, dead;
    for (i = 0; i < 3; i++) { a[i] = m->accel[i]; r[i] = p[i] - fr->origin[i]; }
    if (m->noise_drag != 0.0f) {
        for (i = 0; i < 3; i++) q[i] = p[i] * m->noise_scale + t * m->noise_speed;
        noise(q, n);
        for (i = 0; i < 3; i++) a[i] += (n[i] * m->noise_strength - v[i]) * m->noise_drag;
    }
    if (m->wind_drag != 0.0f)
        for (i = 0; i < 3; i++) a[i] += (m->wind[i] - v[i]) * m->wind_drag;
    if (m->attract_kind >= 0.5f && m->attract_strength != 0.0f && m->attract_radius > 0.0f) {
        float dir[3], len;
        to_world(fr, m->attract_at, w);
        if (m->attract_kind < 1.5f) {                       /* a point: toward origin + at */
            for (i = 0; i < 3; i++) dir[i] = fr->origin[i] + w[i] - p[i];
        } else {
            k = r[0] * w[0] + r[1] * w[1] + r[2] * w[2];
            if (m->attract_kind < 2.5f) for (i = 0; i < 3; i++) dir[i] = w[i] * k - r[i];   /* an axis */
            else for (i = 0; i < 3; i++) dir[i] = -w[i] * k;                                 /* a plane */
        }
        d2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
        len = sqrtf(d2) + 1e-5f;
        k = m->attract_strength / (1.0f + d2 / (m->attract_radius * m->attract_radius)) / len;
        for (i = 0; i < 3; i++) a[i] += dir[i] * k;
    }
    if (m->brownian != 0.0f) {
        k = m->brownian / sqrtf(dt > 0.0001f ? dt : 0.0001f) * 2.0f;
        for (i = 0; i < 3; i++) a[i] += (uniform(seed, index, (uint32_t)i) - 0.5f) * k;
    }
    if (m->vortex_drag != 0.0f) {
        to_world(fr, m->vortex_axis, w);
        d2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        k = m->vortex_falloff != 0.0f ? m->vortex_speed / (1.0f + d2 / (m->vortex_falloff * m->vortex_falloff)) : 0.0f;
        n[0] = (w[1] * r[2] - w[2] * r[1]) * k;
        n[1] = (w[2] * r[0] - w[0] * r[2]) * k;
        n[2] = (w[0] * r[1] - w[1] * r[0]) * k;
        for (i = 0; i < 3; i++) a[i] += (n[i] - v[i]) * m->vortex_drag;
    }
    if ((m->emitter_drag0 != 0.0f || m->emitter_drag1 != 0.0f) && m->emitter_drag_radius > 0.0f) {
        float x = life > 0.0f ? t / life : 1.0f;
        if (x > 1.0f) x = 1.0f;
        d2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        k = (m->emitter_drag0 + (m->emitter_drag1 - m->emitter_drag0) * x) /
            (1.0f + d2 / (m->emitter_drag_radius * m->emitter_drag_radius));
        for (i = 0; i < 3; i++) a[i] -= v[i] * k;
    }
    dead = collide(m, pl, t, dt, scale, p, v);
    for (i = 0; i < 3; i++) {
        v[i] += a[i] * dt;
        p[i] += (v[i] - a[i] * dt * 0.5f) * dt * scale;
    }
    return dead;
}

/* Whether any force depends on the particle's place or velocity (without one the flight is a closed form). */
static int has_force(const sh_motion *m)
{
    return m->noise_drag != 0.0f || m->wind_drag != 0.0f || m->brownian != 0.0f || m->vortex_drag != 0.0f ||
           (m->attract_kind >= 0.5f && m->attract_strength != 0.0f && m->attract_radius > 0.0f) ||
           ((m->emitter_drag0 != 0.0f || m->emitter_drag1 != 0.0f) && m->emitter_drag_radius > 0.0f);
}

int sh_motion_run_planes(const sh_motion *m, const sh_motion_frame *frame, uint32_t seed, float age, float life,
                         float scale, const float p0[3], const float v0[3], const sh_motion_planes *planes,
                         float p[3], float v[3])
{
    float h = sh_motion_step(life), t = 0.0f;
    uint32_t i, n;
    memcpy(p, p0, 3 * sizeof(float));
    memcpy(v, v0, 3 * sizeof(float));
    if (!(age > 0.0f)) return 0;
    if (planes && (planes->count <= 0 || !sh_motion_collides(m))) planes = NULL;
    if (!has_force(m) && !planes) {      /* a uniform acceleration at most: what the steps would add up to */
        for (i = 0; i < 3; i++) {
            p[i] += (v0[i] * age + m->accel[i] * age * age * 0.5f) * scale;
            v[i] += m->accel[i] * age;
        }
        return 0;
    }
    n = (uint32_t)(age / h);
    if (n > 4096u) n = 4096u;
    for (i = 0; i < n; i++, t += h)
        if (one_step(m, frame, seed, i, t, h, life, scale, planes, p, v)) return 1;
    if (age - t > 1e-6f && one_step(m, frame, seed, n, t, age - t, life, scale, planes, p, v)) return 1;
    return 0;
}

void sh_motion_run(const sh_motion *m, const sh_motion_frame *frame, uint32_t seed, float age, float life,
                   float scale, const float p0[3], const float v0[3], float p[3], float v[3])
{
    (void)sh_motion_run_planes(m, frame, seed, age, life, scale, p0, v0, NULL, p, v);
}

void sh_motion_place(float *verts, size_t stride, int aimed, const float engine_at[3], const float p[3],
                     const float v[3], const float eye[3], float stretch)
{
    float *q[4];
    int i, k;
    for (i = 0; i < 4; i++) q[i] = (float *)((uint8_t *)verts + stride * (size_t)i);
    if (aimed) {
        /* the game's aimed quad: q0 = far - side, q1 = far + side, q2 = near - side, q3 = near + side */
        float side[3], dir[3], view[3], far_[3], half, length, speed, len;
        for (k = 0; k < 3; k++) {
            side[k] = (q[3][k] - q[2][k]) * 0.5f;
            far_[k] = (q[0][k] + q[1][k]) * 0.5f - (q[2][k] + q[3][k]) * 0.5f;
        }
        half = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        length = sqrtf(far_[0] * far_[0] + far_[1] * far_[1] + far_[2] * far_[2]);
        speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (speed > 1e-6f) {
            for (k = 0; k < 3; k++) { dir[k] = v[k] / speed; view[k] = p[k] - eye[k]; }
            if (stretch > 0.0f) length *= speed * stretch;
            side[0] = dir[1] * view[2] - dir[2] * view[1];
            side[1] = dir[2] * view[0] - dir[0] * view[2];
            side[2] = dir[0] * view[1] - dir[1] * view[0];
            len = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
            if (len > 1e-9f) {
                for (k = 0; k < 3; k++) {
                    float s = side[k] / len * half, f = p[k] + dir[k] * length;
                    q[0][k] = f - s; q[1][k] = f + s;
                    q[2][k] = p[k] - s; q[3][k] = p[k] + s;
                }
                return;
            }
        }
    }
    for (i = 0; i < 4; i++)
        for (k = 0; k < 3; k++) q[i][k] += p[k] - engine_at[k];
}
