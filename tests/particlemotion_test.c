/* particlemotion_test.c -- a CPU particle stage's simulated motion (particlemotion_sim.c): which stage asks, the
 * decl fields it is read from, the integrator against closed forms and against Doomforge's Python arithmetic
 * (doom2016/effects/particle_motion.py: the expected numbers below are its answers), and where the quad goes. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "particlemotion.h"

static int g_checks, g_failed;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static int near3(const float a[3], const float b[3], float tol)
{
    int i;
    for (i = 0; i < 3; i++)
        if (fabsf(a[i] - b[i]) > tol * (1.0f + fabsf(b[i]))) return 0;
    return 1;
}

static void put(uint8_t *stage, size_t off, float v) { memcpy(stage + off, &v, sizeof v); }

typedef struct { const char *name; float p[3], v[3]; } expect_t;

/* A room for the planes: the floor at z = 0, a wall at x = 320 facing -x. */
static int g_traces;
static int room_trace(const float from[3], const float to[3], float end[3], float normal[3])
{
    float t;
    g_traces++;
    if (to[2] < from[2] && from[2] > 0.0f && to[2] <= 0.0f) {        /* down onto the floor */
        t = from[2] / (from[2] - to[2]);
        end[0] = from[0] + (to[0] - from[0]) * t; end[1] = from[1] + (to[1] - from[1]) * t; end[2] = 0.0f;
        normal[0] = 0.0f; normal[1] = 0.0f; normal[2] = 1.0f;
        return 1;
    }
    if (to[0] > from[0] && from[0] < 320.0f && to[0] >= 320.0f) {     /* into the wall */
        t = (320.0f - from[0]) / (to[0] - from[0]);
        end[0] = 320.0f; end[1] = from[1] + (to[1] - from[1]) * t; end[2] = from[2] + (to[2] - from[2]) * t;
        normal[0] = -1.0f; normal[1] = 0.0f; normal[2] = 0.0f;
        return 1;
    }
    return 0;
}

int main(void)
{
    static uint8_t stage[0x6E0];
    static const sh_motion_frame FR = { { 100.0f, 200.0f, 50.0f }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    static const float P0[3] = { 163.0f, 200.0f, 76.0f }, V0[3] = { 100.0f, 40.0f, 200.0f };
    /* run(m, frame, seed 12345, age 0.8, life 1.2, P0, V0) by particle_motion.py */
    static const expect_t EXPECT[] = {
        { "drag", { 207.8104f, 211.8917f, 47.4842f }, { 35.9680f, 3.4103f, -143.0284f } },
        { "vortex", { 186.4942f, 320.4793f, 185.8152f }, { -88.1071f, 244.5332f, 89.2620f } },
        { "brownian", { 255.6057f, 227.5038f, 200.3065f }, { 149.3993f, 29.4894f, 122.9720f } },
        { "noise", { 239.2062f, 168.8899f, 363.7298f }, { -22.3358f, -79.0408f, 493.7647f } },
        { "point", { 43.0659f, 202.1145f, 139.2448f }, { -159.9304f, -42.4805f, -189.8022f } },
        { "axis", { 252.6713f, 232.8837f, 236.0000f }, { 121.0894f, 42.5561f, 200.0000f } },
        { "plane", { 243.0000f, 232.0000f, 166.6184f }, { 100.0000f, 40.0000f, 46.7679f } },
        { "edrag", { 198.6844f, 214.2737f, 147.3687f }, { 24.6156f, 9.8462f, 49.2312f } },
        { "ember", { 199.6179f, 321.3162f, 159.0416f }, { -44.1007f, 249.1868f, 38.5853f } },
    };
    sh_motion cases[9];
    sh_motion m;
    float p[3], v[3], want[3];
    int i;
    printf("particlemotion_test\n");

    printf("-- which stage asks: vectorFieldGPU.repeat on a CPU stage\n");
    CHECK(!sh_motion_asked(NULL));
    CHECK(!sh_motion_asked(stage));
    stage[SH_MOTION_FIELD_REPEAT] = 1;
    CHECK(sh_motion_asked(stage));
    stage[SH_MOTION_STAGE_FLAGS] = SH_MOTION_STAGE_GPU;          /* a GPU stage reads the block as the game does */
    CHECK(!sh_motion_asked(stage));
    stage[SH_MOTION_STAGE_FLAGS] = 0x1F;                         /* the other flag bits do not matter */
    CHECK(sh_motion_asked(stage));

    printf("-- the fields a stage's motion is read from (idParticleStage, build 13954591)\n");
    for (i = 0; i < 3; i++) {
        put(stage, 0x65C + 4 * i, 1.0f + i);        /* accelerationGPU.acceleration */
        put(stage, 0x650 + 4 * i, 4.0f + i);        /* velocityGPU.offset */
        put(stage, 0x694 + 4 * i, 7.0f + i);        /* vectorFieldGPU.offset */
        put(stage, 0x6A0 + 4 * i, 10.0f + i);       /* vectorFieldGPU.initialRotation */
        put(stage, 0x6AC + 4 * i, 13.0f + i);       /* vectorFieldGPU.initialRotationVariation */
        put(stage, 0x6B8 + 4 * i, 16.0f + i);       /* vectorFieldGPU.rotationSpeed */
        put(stage, 0x6C4 + 4 * i, 19.0f + i);       /* vectorFieldGPU.rotationSpeedVariation */
        put(stage, 0x634 + 4 * i, 22.0f + i);       /* distributionGPU.offset */
    }
    put(stage, 0x6D4, 25.0f);                       /* vectorFieldGPU.tightness */
    put(stage, 0x61C, 26.0f); put(stage, 0x620, 27.0f);   /* geometryGPU.sizeVariation */
    put(stage, 0x678, 28.0f);                       /* physicsGPU.massVariation */
    put(stage, 0x59C, 29.0f);                       /* systemPropertiesGPU.boundsExpansion.x */
    put(stage, 0x5A0, 30.0f); put(stage, 0x5A4, 31.0f);   /* .y .z */
    sh_motion_read(stage, &m);
    CHECK(m.accel[0] == 1.0f && m.accel[2] == 3.0f && m.wind[0] == 4.0f && m.wind[2] == 6.0f);
    CHECK(m.vortex_axis[0] == 7.0f && m.vortex_axis[2] == 9.0f);
    CHECK(m.vortex_falloff == 10.0f && m.vortex_speed == 11.0f && m.vortex_drag == 12.0f);
    CHECK(m.noise_speed == 13.0f && m.noise_scale == 14.0f && m.noise_drag == 15.0f && m.noise_strength == 16.0f);
    CHECK(m.brownian == 17.0f && m.stretch == 18.0f);
    CHECK(m.attract_kind == 19.0f && m.attract_strength == 20.0f && m.attract_radius == 21.0f);
    CHECK(m.attract_at[0] == 22.0f && m.attract_at[2] == 24.0f);
    CHECK(m.wind_drag == 25.0f && m.emitter_drag0 == 26.0f && m.emitter_drag1 == 27.0f && m.emitter_drag_radius == 28.0f);
    CHECK(m.launch == 29.0f && m.launch_lo == 30.0f && m.launch_hi == 31.0f);

    printf("-- the launch ball: a velocity in a random direction, up to its size, the same for the same seed\n");
    {
        float a[3] = { 1.0f, 2.0f, 3.0f }, b[3] = { 1.0f, 2.0f, 3.0f }, c[3] = { 1.0f, 2.0f, 3.0f };
        static const float WANT[3] = { -97.3907f, 24.3933f, 9.6262f };       /* particle_motion.launch_velocity */
        sh_motion ball;
        memset(&ball, 0, sizeof ball);
        sh_motion_launch(&ball, 12345u, a);
        CHECK(a[0] == 1.0f && a[1] == 2.0f && a[2] == 3.0f);                  /* no ball: untouched */
        ball.launch = 157.48f;
        sh_motion_launch(&ball, 12345u, a);
        sh_motion_launch(&ball, 12345u, b);
        sh_motion_launch(&ball, 54321u, c);
        CHECK(near3(a, b, 0.0f) && !near3(a, c, 1e-3f) && near3(a, WANT, 1e-4f));
        CHECK(sqrtf((a[0] - 1) * (a[0] - 1) + (a[1] - 2) * (a[1] - 2) + (a[2] - 3) * (a[2] - 3)) <= 157.48f);
    }

    printf("-- the launch speed factor: one number per particle between its two ends, before the ball\n");
    {
        float a[3] = { 1.0f, 2.0f, 3.0f }, b[3] = { 1.0f, 2.0f, 3.0f };
        static const float WANT[3] = { -0.62273f, -1.24546f, -1.86819f };            /* particle_motion.launch_velocity */
        sh_motion f;
        memset(&f, 0, sizeof f);
        f.launch_lo = -0.8f; f.launch_hi = 2.4f;
        sh_motion_launch(&f, 12345u, a);
        CHECK(near3(a, WANT, 1e-4f));
        CHECK(fabsf(a[1] / a[0] - 2.0f) < 1e-4f && fabsf(a[2] / a[0] - 3.0f) < 1e-4f);   /* the direction is kept */
        f.launch_lo = f.launch_hi = 0.5f;
        sh_motion_launch(&f, 777u, b);
        CHECK(b[0] == 0.5f && b[1] == 1.0f && b[2] == 1.5f);
    }

    printf("-- the emitter's scale: the largest of the three factors, 1 when they are not a scale\n");
    {
        static const float S[3] = { 1.7f, 1.7f, 1.7f }, T[3] = { 1.0f, 2.5f, 0.5f }, Z0[3] = { 0, 0, 0 }, H[3] = { 1e9f, 1, 1 };
        CHECK(sh_motion_scale(S) == 1.7f && sh_motion_scale(T) == 2.5f && sh_motion_scale(Z0) == 1.0f && sh_motion_scale(H) == 1.0f);
    }

    printf("-- the step: life / 96 between 1/60 s and 0.05 s\n");
    CHECK(fabsf(sh_motion_step(0.4f) - 1.0f / 60.0f) < 1e-7f);
    CHECK(fabsf(sh_motion_step(3.0f) - 0.03125f) < 1e-7f);
    CHECK(fabsf(sh_motion_step(9.0f) - 0.05f) < 1e-7f);

    printf("-- no force: a straight flight; a uniform acceleration: the closed form\n");
    memset(&m, 0, sizeof m);
    sh_motion_run(&m, &FR, 1u, 0.8f, 1.2f, 1.0f, P0, V0, p, v);
    for (i = 0; i < 3; i++) want[i] = P0[i] + V0[i] * 0.8f;
    CHECK(near3(p, want, 1e-5f) && near3(v, V0, 1e-6f));
    m.accel[2] = -800.0f;
    sh_motion_run(&m, &FR, 1u, 0.8f, 1.2f, 1.0f, P0, V0, p, v);
    want[2] = P0[2] + V0[2] * 0.8f - 800.0f * 0.32f;
    CHECK(near3(p, want, 1e-4f) && fabsf(v[2] - (V0[2] - 640.0f)) < 0.01f);
    sh_motion_run(&m, &FR, 1u, 0.0f, 1.2f, 1.0f, P0, V0, p, v);          /* at its birth: where it was born */
    CHECK(near3(p, P0, 0.0f) && near3(v, V0, 0.0f));

    printf("-- a scaled emitter: a step moves the particle by velocity x step x scale; the forces see the scaled places\n");
    sh_motion_run(&m, &FR, 1u, 0.8f, 1.2f, 1.7f, P0, V0, p, v);
    for (i = 0; i < 3; i++) want[i] = P0[i] + V0[i] * 0.8f * 1.7f;
    want[2] -= 800.0f * 0.32f * 1.7f;
    CHECK(near3(p, want, 1e-4f) && fabsf(v[2] - (V0[2] - 640.0f)) < 0.01f);      /* the velocity is not scaled */
    {
        static const float WANT_P[3] = { 190.8733f, 423.9781f, 217.1707f }, WANT_V[3] = { -130.5954f, 262.6765f, 38.5853f };
        sh_motion e;
        memset(&e, 0, sizeof e);
        e.vortex_axis[2] = 1.0f; e.vortex_falloff = 524.934f; e.vortex_speed = 5.0f; e.vortex_drag = 1.0f; e.brownian = 104.9868f;
        sh_motion_run(&e, &FR, 12345u, 0.8f, 1.2f, 1.7f, P0, V0, p, v);
        CHECK(near3(p, WANT_P, 2e-3f) && near3(v, WANT_V, 5e-3f));
    }

    printf("-- each force against Doomforge's arithmetic\n");
    memset(cases, 0, sizeof cases);
    cases[0].wind[0] = 30.0f; cases[0].wind_drag = 3.0f; cases[0].accel[2] = -525.0f;
    cases[1].vortex_axis[2] = 1.0f; cases[1].vortex_falloff = 524.934f; cases[1].vortex_speed = 5.0f; cases[1].vortex_drag = 1.0f;
    cases[2].brownian = 104.9868f;
    cases[3].noise_speed = 0.8f; cases[3].noise_scale = 0.019f; cases[3].noise_drag = 2.0f; cases[3].noise_strength = 2600.0f;
    cases[3].accel[2] = 1050.0f;
    cases[4].attract_kind = 1.0f; cases[4].attract_strength = 1050.0f; cases[4].attract_radius = 157.5f; cases[4].attract_at[2] = 52.5f;
    cases[5].attract_kind = 2.0f; cases[5].attract_strength = -52.5f; cases[5].attract_radius = 105.0f; cases[5].attract_at[2] = 1.0f;
    cases[6].attract_kind = 3.0f; cases[6].attract_strength = 300.0f; cases[6].attract_radius = 105.0f; cases[6].attract_at[2] = 1.0f;
    cases[7].emitter_drag0 = 5.0f; cases[7].emitter_drag_radius = 105.0f;
    cases[8] = cases[1]; cases[8].brownian = 104.9868f;
    for (i = 0; i < 9; i++) {
        sh_motion_run(&cases[i], &FR, 12345u, 0.8f, 1.2f, 1.0f, P0, V0, p, v);
        g_checks++;
        if (!near3(p, EXPECT[i].p, 2e-3f) || !near3(v, EXPECT[i].v, 5e-3f)) {
            g_failed++;
            printf("FAIL %s: at %.4f %.4f %.4f moving %.4f %.4f %.4f\n", EXPECT[i].name, p[0], p[1], p[2], v[0], v[1], v[2]);
        }
    }

    printf("-- the same particle is in the same place whenever it is asked; a later age continues the path\n");
    {
        float a[3], b[3], va[3], vb[3], c[3], vc[3];
        sh_motion_run(&cases[8], &FR, 777u, 0.5f, 1.2f, 1.0f, P0, V0, a, va);
        sh_motion_run(&cases[8], &FR, 777u, 0.5f, 1.2f, 1.0f, P0, V0, b, vb);
        CHECK(near3(a, b, 0.0f) && near3(va, vb, 0.0f));
        sh_motion_run(&cases[8], &FR, 777u, 0.5001f, 1.2f, 1.0f, P0, V0, c, vc);     /* no jump across a part step */
        CHECK(near3(a, c, 1e-3f));
        sh_motion_run(&cases[8], &FR, 778u, 0.5f, 1.2f, 1.0f, P0, V0, c, vc);        /* another seed, another path */
        CHECK(!near3(a, c, 1e-3f));
    }

    printf("-- a vortex turns the particle about the emitter's axis\n");
    {
        static const float B[3] = { 163.0f, 200.0f, 50.0f }, Z[3] = { 0, 0, 0 };
        float ang;
        sh_motion_run(&cases[1], &FR, 1u, 1.0f, 1.2f, 1.0f, B, Z, p, v);
        ang = atan2f(p[1] - 200.0f, p[0] - 100.0f);
        CHECK(ang > 0.5f && ang < 2.6f && fabsf(p[2] - 50.0f) < 1e-3f);        /* counter-clockwise, in its plane */
    }

    printf("-- the quad: moved whole; an aimed quad rebuilt along its velocity\n");
    {
        float q[4][12];
        static const float AT[3] = { 10.0f, 20.0f, 30.0f }, TO[3] = { 15.0f, 18.0f, 31.0f }, EYE[3] = { 15.0f, -82.0f, 31.0f };
        static const float VEL[3] = { 200.0f, 0.0f, 0.0f };
        memset(q, 0, sizeof q);
        q[0][0] = 9.0f;  q[0][1] = 20.0f; q[0][2] = 31.0f;
        q[1][0] = 11.0f; q[1][1] = 20.0f; q[1][2] = 31.0f;
        q[2][0] = 9.0f;  q[2][1] = 20.0f; q[2][2] = 29.0f;
        q[3][0] = 11.0f; q[3][1] = 20.0f; q[3][2] = 29.0f;
        q[0][5] = 42.0f;                                         /* the rest of a vertex is not touched */
        sh_motion_place(&q[0][0], sizeof q[0], 0, AT, TO, VEL, EYE, 0.0f);
        CHECK(q[0][0] == 14.0f && q[0][1] == 18.0f && q[0][2] == 32.0f && q[3][0] == 16.0f && q[3][2] == 30.0f);
        CHECK(q[0][5] == 42.0f);
        /* aimed: near edge at the particle, 2 wide (side 1), 4 long; rebuilt along +x seen from -y */
        q[0][0] = 9.0f;  q[0][1] = 24.0f; q[0][2] = 30.0f;      /* far - side */
        q[1][0] = 11.0f; q[1][1] = 24.0f; q[1][2] = 30.0f;      /* far + side */
        q[2][0] = 9.0f;  q[2][1] = 20.0f; q[2][2] = 30.0f;      /* near - side */
        q[3][0] = 11.0f; q[3][1] = 20.0f; q[3][2] = 30.0f;      /* near + side */
        sh_motion_place(&q[0][0], sizeof q[0], 1, AT, TO, VEL, EYE, 0.0f);
        CHECK(fabsf((q[2][0] + q[3][0]) * 0.5f - 15.0f) < 1e-4f && fabsf((q[2][1] + q[3][1]) * 0.5f - 18.0f) < 1e-4f);
        CHECK(fabsf((q[0][0] + q[1][0]) * 0.5f - 19.0f) < 1e-4f);            /* far end 4 along +x */
        CHECK(fabsf(fabsf(q[3][2] - q[2][2]) - 2.0f) < 1e-4f);               /* still 2 wide, across the view */
        sh_motion_place(&q[0][0], sizeof q[0], 1, TO, TO, VEL, EYE, 0.01f);  /* stretched: 4 x 200 x 0.01 = 8 long */
        CHECK(fabsf((q[0][0] + q[1][0]) * 0.5f - 23.0f) < 1e-3f);
    }

    printf("-- collision with the scene's planes: the same numbers as particle_motion.py's run_ex\n");
    {
        static const float G[3] = { 0.0f, 0.0f, -1050.0f };
        sh_motion_planes floor_ = { 1, { { 0, 0, 1 } }, { 40.0f } };
        sh_motion_planes both = { 2, { { 0, 0, 1 }, { -1, 0, 0 } }, { 40.0f, -180.0f } };
        float want_v[3];
        int dead;
        memset(&m, 0, sizeof m);
        memcpy(m.accel, G, sizeof G);
        m.collide_thickness = 104.987f; m.collide_elasticity = 0.5f; m.collide_restitution = 0.5f;
        dead = sh_motion_run_planes(&m, &FR, 12345u, 0.8f, 1.2f, 1.0f, P0, V0, &floor_, p, v);
        want[0] = 223.8333f; want[1] = 224.3333f; want[2] = 43.6667f;
        want_v[0] = 25.0f; want_v[1] = 10.0f; want_v[2] = -29.0625f;
        CHECK(!dead && near3(p, want, 2e-4f) && near3(v, want_v, 2e-4f));
        dead = sh_motion_run_planes(&m, &FR, 12345u, 0.8f, 1.2f, 1.0f, P0, V0, &both, p, v);
        want[0] = 167.1667f; want[1] = 214.6667f; want[2] = 41.6510f;
        want_v[0] = -6.25f; want_v[1] = 5.0f; want_v[2] = -73.5938f;
        CHECK(!dead && near3(p, want, 2e-4f) && near3(v, want_v, 2e-4f));
        /* with drag, a quarter of the speed kept and the whole normal part */
        m.wind_drag = 3.0f; m.collide_elasticity = 0.25f; m.collide_restitution = 1.0f;
        dead = sh_motion_run_planes(&m, &FR, 12345u, 0.8f, 1.2f, 1.0f, P0, V0, &floor_, p, v);
        want[0] = 188.8063f; want[1] = 210.3225f; want[2] = 41.4194f;
        want_v[0] = 0.0796f; want_v[1] = 0.0318f; want_v[2] = 2.7067f;
        CHECK(!dead && near3(p, want, 5e-4f) && near3(v, want_v, 5e-3f));
        /* COLLISION_KILL: at rest on the floor, the particle dies where it lies */
        m.wind_drag = 6.0f; m.collide_restitution = 0.5f; m.collide_penetration = 1.05f; m.collide_speed = 7.39f;
        dead = sh_motion_run_planes(&m, &FR, 12345u, 0.8f, 1.2f, 1.0f, P0, V0, &floor_, p, v);
        want[0] = 178.2798f; want[1] = 206.1119f; want[2] = 40.6895f;
        CHECK(dead == 1 && near3(p, want, 5e-4f));
        /* no planes, or a stage that does not collide: the flight without collision */
        {
            float p2[3], v2[3];
            sh_motion plain = m;
            plain.collide_thickness = 0.0f;
            sh_motion_run(&m, &FR, 12345u, 0.8f, 1.2f, 1.0f, P0, V0, p, v);
            CHECK(sh_motion_run_planes(&plain, &FR, 12345u, 0.8f, 1.2f, 1.0f, P0, V0, &floor_, p2, v2) == 0);
            CHECK(near3(p, p2, 1e-6f) && near3(v, v2, 1e-6f));
        }
        /* the keys */
        memset(stage, 0, sizeof stage);
        put(stage, 0x5D0, 104.987f); put(stage, 0x5D4, 0.2f); put(stage, 0x5D8, 1.05f); put(stage, 0x5DC, 7.39f);
        put(stage, 0x66C, 1.0f); put(stage, 0x670, 0.25f); put(stage, 0x594, 2.0f);
        sh_motion_read(stage, &m);
        CHECK(sh_motion_collides(&m) && m.collide_min_life == 0.2f && m.collide_penetration == 1.05f);
        CHECK(m.collide_speed == 7.39f && m.collide_restitution == 1.0f && m.collide_elasticity == 0.25f);
        CHECK(m.collide_offset == 2.0f);
        stage[SH_MOTION_STAGE_FLAGS] = SH_MOTION_STAGE_SORTED;
        CHECK(stage[SH_MOTION_STAGE_FLAGS] & 0x02);
    }

    printf("-- the planes of an emitter place: asked from any thread, traced once on the game thread\n");
    {
        static const float ON_FLOOR[3] = { 100.0f, 200.0f, 16.0f }, UP[3] = { 0, 0, 1 };
        static const float ON_WALL[3] = { 300.0f, 200.0f, 80.0f }, FACING[3] = { -1, 0, 0 };
        sh_motion_planes got;
        g_traces = 0;
        sh_planes_clear();
        CHECK(!sh_planes_lookup(ON_FLOOR, UP, &got));              /* new: asked for */
        CHECK(!sh_planes_lookup(ON_FLOOR, UP, &got));              /* asked again by another particle */
        CHECK(sh_planes_service(room_trace, 24) == 1 && g_traces == 1);   /* traced once: one floor, facing up */
        CHECK(sh_planes_lookup(ON_FLOOR, UP, &got) && got.count == 1);
        CHECK(got.n[0][2] == 1.0f && got.d[0] == 0.0f);
        { static const float NEAR_[3] = { 102.0f, 199.0f, 17.0f }; CHECK(sh_planes_lookup(NEAR_, UP, &got)); }   /* the same 8-unit place */
        CHECK(sh_planes_service(room_trace, 24) == 0);
        /* an impact on the wall at x = 320, facing -x: the floor under it and the wall behind it */
        CHECK(!sh_planes_lookup(ON_WALL, FACING, &got));
        CHECK(sh_planes_service(room_trace, 24) == 1);
        CHECK(sh_planes_lookup(ON_WALL, FACING, &got) && got.count == 2);
        CHECK(got.n[1][0] == -1.0f && got.d[1] == -320.0f);
        /* the budget: at most this many traced a frame, the rest next frame */
        {
            int k, asked = 0;
            for (k = 0; k < 10; k++) {
                float at[3] = { 1000.0f + 64.0f * k, 0.0f, 16.0f };
                asked += !sh_planes_lookup(at, UP, &got);
            }
            CHECK(asked == 10);
            CHECK(sh_planes_service(room_trace, 4) == 4 && sh_planes_service(room_trace, 24) == 6);
        }
        sh_planes_clear();
        CHECK(!sh_planes_lookup(ON_FLOOR, UP, &got));
        CHECK(sh_planes_service(NULL, 24) == 1);                    /* no trace: the place is known to have none */
        CHECK(sh_planes_lookup(ON_FLOOR, UP, &got) && got.count == 0);
    }

    printf("-- children born at their parent's place: the link between two stages of one decl\n");
    {
        static uint8_t stages[3][SH_MOTION_STAGE_SIZE];
        uint8_t *parent = stages[0], *child = stages[2];
        short n;
        int pidx;
        float delta;
        static const float AT[3] = { 100.0f, 200.0f, 16.0f }, NEARBY[3] = { 101.0f, 201.0f, 17.0f };
        memset(stages, 0, sizeof stages);
        CHECK(sh_family_role(parent) == 0 && sh_family_role(NULL) == 0);
        put(parent, SH_MOTION_SYSTEM_EMISSION, -1.0f);
        put(child, SH_MOTION_SYSTEM_EMISSION, 998.0f);          /* the parent is two stages before */
        CHECK(sh_family_role(parent) == 1 && sh_family_role(child) == 2);
        CHECK(sh_family_parent(child) == parent);
        put(stages[1], SH_MOTION_SYSTEM_EMISSION, 1000.0f);      /* a link to itself is none */
        CHECK(sh_family_parent(stages[1]) == NULL);
        /* a burst of 7 parents (INSTANT: no spawn window); 700 children over 1 s after a delay of 0.25 s */
        n = 7; memcpy(parent + SH_MOTION_STAGE_COUNT, &n, sizeof n);
        put(parent, SH_MOTION_STAGE_BUNCHTIME, 1.5f); put(parent, SH_MOTION_STAGE_MAXLIFE, 1.5f);
        n = 700; memcpy(child + SH_MOTION_STAGE_COUNT, &n, sizeof n);
        put(child, SH_MOTION_STAGE_BUNCHING, 2.0f); put(child, SH_MOTION_STAGE_BUNCHTIME, 0.5f);   /* 1 s window */
        put(child, SH_MOTION_STAGE_TIMEOFFSET, 0.25f);
        CHECK(sh_family_link(child, parent, 0, &pidx, &delta) && pidx == 0 && fabsf(delta - 0.25f) < 1e-4f);
        CHECK(sh_family_link(child, parent, 9, &pidx, &delta) && pidx == 2 && fabsf(delta - 0.262f) < 1e-4f);
        CHECK(sh_family_link(child, parent, 699, &pidx, &delta) && pidx == 6 && fabsf(delta - 1.248f) < 1e-4f);
        /* each parent's children follow it through the window: child 7 j + p is parent p's j-th, about j / 100 s apart */
        CHECK(sh_family_link(child, parent, 7 * 50 + 3, &pidx, &delta) && pidx == 3 && fabsf(delta - 0.754f) < 1e-4f);
        CHECK(sh_family_life(parent) == 1.5f);
        /* a parent with a spawn window of its own: parent 3 is born 3 x 700 / 7 = 300 ms into it */
        put(parent, SH_MOTION_STAGE_BUNCHING, 0.4666667f);     /* 0.7 s */
        CHECK(sh_family_link(child, parent, 3, &pidx, &delta) && pidx == 3 && fabsf(delta - (0.25f + 0.004f - 0.3f)) < 2e-3f);
        n = 0; memcpy(parent + SH_MOTION_STAGE_COUNT, &n, sizeof n);
        CHECK(!sh_family_link(child, parent, 3, &pidx, &delta));
        /* the seed: the same for a parent's particle and its children's retrace, different per particle; the salt
         * is the emitter's place for a stage in world axes, 0 in its model's */
        CHECK(sh_family_seed(77u, 3) == sh_family_seed(77u, 3) && sh_family_seed(77u, 3) != sh_family_seed(77u, 4));
        CHECK(sh_family_salt(AT, parent) == 0u);
        parent[SH_MOTION_STAGE_FLAGS] |= SH_MOTION_STAGE_SORTED;
        CHECK(sh_family_salt(AT, parent) != 0u && sh_family_salt(AT, parent) == sh_family_salt(NEARBY, parent));
        CHECK(sh_family_inherit(child) == 0.0f);
    }

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
