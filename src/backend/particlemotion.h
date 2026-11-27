/* particlemotion.h -- simulated motion for a CPU particle stage (particlemotion.c, particlemotion_sim.c).
 * The game places a CPU particle in closed form (birth, straight flight, acceleration, gravity, origin function
 * VK 0x19B8080 / GL 0x19AC350). A stage with `vectorFieldGPU.repeat = true` is instead integrated from birth in
 * fixed steps under the forces its GPU-block decl fields name (sh_motion), so its place depends on seed and age
 * alone. The detour on the vertex writer (VK 0x19AFCE0, GL 0x19A3FB0) lets the game write the quad, then moves it. */
#ifndef SNAPMAP_PLUS_PARTICLEMOTION_H
#define SNAPMAP_PLUS_PARTICLEMOTION_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* One stage's motion; each field names the decl key it is read from (lengths in world units, times in seconds). */
typedef struct sh_motion {
    float accel[3];                  /* accelerationGPU.acceleration, units / s^2 */
    float wind[3];                   /* velocityGPU.offset: air velocity, units / s */
    float wind_drag;                 /* vectorFieldGPU.tightness, 1 / s */
    float vortex_axis[3];            /* vectorFieldGPU.offset, in the emitter's axes */
    float vortex_falloff, vortex_speed, vortex_drag;   /* vectorFieldGPU.initialRotation */
    float noise_speed, noise_scale, noise_drag, noise_strength;   /* initialRotationVariation; rotationSpeed.x */
    float brownian;                  /* vectorFieldGPU.rotationSpeed.y, units / s^1.5 */
    float stretch;                   /* vectorFieldGPU.rotationSpeed.z: aligned quad length x speed x this */
    float attract_kind, attract_strength, attract_radius;   /* rotationSpeedVariation; kind 0-3 */
    float attract_at[3];             /* distributionGPU.offset: the point, or the axis or plane normal */
    float emitter_drag0, emitter_drag1, emitter_drag_radius;   /* geometryGPU.sizeVariation; massVariation */
    float launch;                    /* systemPropertiesGPU.boundsExpansion.x: random launch ball, units / s */
    float launch_lo, launch_hi;      /* boundsExpansion y z: launch speed factor range (both 0: as launched) */
    int uniform_volume;              /* vectorFieldGPU.strength = 1/3: choose a point uniformly inside the ball */
    float cone_angles[2], cone_speeds[2]; /* geometryGPU.sizeInitial/sizeFinal: half angles (radians), units/s */
    int oriented_cone;               /* velocityGPU.scale.z = -1: use the emitter frame */
    float cone_rotation[4], cone_angle_span[2]; /* unit quaternion XYZW and angle variation, radians */
    float ball_scale[2];             /* velocityGPU.scale.x/y: independent radius multipliers */
    float collide_thickness, collide_min_life, collide_penetration, collide_speed;   /* colorGPU.fadeColor x y z w */
    float collide_elasticity, collide_restitution, collide_offset;   /* physicsGPU.elasticity, .restitution; timeOffset */
} sh_motion;

/* The scene's surfaces a colliding particle bounces on: n . x >= d is the open side, n of unit length. */
#define SH_MOTION_PLANES 2
typedef struct sh_motion_planes {
    int count;
    float n[SH_MOTION_PLANES][3];
    float d[SH_MOTION_PLANES];
} sh_motion_planes;

/* The emitter a particle belongs to: its origin and its three axes in the world. */
typedef struct sh_motion_frame {
    float origin[3];
    float x[3], y[3], z[3];
} sh_motion_frame;

/* idParticleStage offsets of build 13954591 (the engine's own reflection; the same in both executables). */
#define SH_MOTION_STAGE_FLAGS        0x44     /* bit 0x20: gpuStage; bit 0x02: isTransparencySorted */
#define SH_MOTION_STAGE_GPU          0x20
#define SH_MOTION_STAGE_SORTED       0x02
#define SH_MOTION_SYSTEM_TIMEOFFSET  0x594
#define SH_MOTION_COLOR_FADE         0x5D0
#define SH_MOTION_PHYSICS_RESTITUTION 0x66C
#define SH_MOTION_PHYSICS_ELASTICITY 0x670
#define SH_MOTION_STAGE_ORIENTATION  0x12C    /* prtOrientationType_t; 3 = POR_AIMED */
#define SH_MOTION_SYSTEM_BOUNDS      0x59C
#define SH_MOTION_GEOMETRY_SIZEVAR   0x61C
#define SH_MOTION_GEOMETRY_INITIAL   0x60C
#define SH_MOTION_GEOMETRY_FINAL     0x614
#define SH_MOTION_VELOCITY_SCALE     0x644
#define SH_MOTION_DISTRIBUTION_SCALE 0x628
#define SH_MOTION_FIELD_SCALE        0x688
#define SH_MOTION_DISTRIBUTION_OFF   0x634
#define SH_MOTION_VELOCITY_OFF       0x650
#define SH_MOTION_ACCELERATION       0x65C
#define SH_MOTION_PHYSICS_MASSVAR    0x678
#define SH_MOTION_FIELD_OFFSET       0x694
#define SH_MOTION_FIELD_ROTATION     0x6A0
#define SH_MOTION_FIELD_ROTATIONVAR  0x6AC
#define SH_MOTION_FIELD_SPEED        0x6B8
#define SH_MOTION_FIELD_SPEEDVAR     0x6C4
#define SH_MOTION_FIELD_STRENGTH     0x6D0
#define SH_MOTION_FIELD_TIGHTNESS    0x6D4
#define SH_MOTION_FIELD_REPEAT       0x6D8

/* 1 when the stage at `stage` asks for simulated motion. */
int sh_motion_asked(const uint8_t *stage);

/* Read a stage's motion from its fields. */
void sh_motion_read(const uint8_t *stage, sh_motion *out);

/* A particle's launch velocity by the stage: times its launch speed factor, plus its launch ball (the same for
 * the same seed). */
void sh_motion_launch(const sh_motion *m, uint32_t seed, float v0[3], const sh_motion_frame *frame);

/* The step a particle of this life is integrated with (seconds). */
float sh_motion_step(float life);

/* The emitter's scale (an fx item's `size`) from the lengths of the three axes the origin function applies: the
 * largest, 1 when they are not a scale. The forces see scaled places and the steps move by velocity x step x scale. */
float sh_motion_scale(const float factors[3]);

/* Where a particle born at p0 with velocity v0 is after `age` seconds, and its velocity there. `seed` is the
 * particle's own, `scale` the emitter's; the same arguments always give the same answer. Pure. */
void sh_motion_run(const sh_motion *m, const sh_motion_frame *frame, uint32_t seed, float age, float life,
                   float scale, const float p0[3], const float v0[3], float p[3], float v[3]);

/* sh_motion_run against the scene's planes (NULL: none); returns 1 when the particle has died on a surface (p and
 * v are then where and how it died). */
int sh_motion_run_planes(const sh_motion *m, const sh_motion_frame *frame, uint32_t seed, float age, float life,
                         float scale, const float p0[3], const float v0[3], const sh_motion_planes *planes,
                         float p[3], float v[3]);

/* 1 when the stage collides (`colorGPU.fadeColor.x` over 0); it also needs `isTransparencySorted` so the game
 * builds it in world axes. */
int sh_motion_collides(const sh_motion *m);

/* The planes of an emitter place (particlemotion_planes.c). The particle jobs read them (any thread); the game
 * thread finds them with the engine's trace. Places are quantized to 8 units and the emitter's z axis to 1/8, so an
 * emitter that stays put is traced once; a table entry older than SH_PLANES_STALE service frames is traced again. */
#define SH_PLANES_STALE 600
/* 1 and `out` when the planes of this place are known (count may be 0); 0 when not yet, and the place is asked for. */
int sh_planes_lookup(const float origin[3], const float zaxis[3], sh_motion_planes *out);
/* One trace of the world: 1 and the hit's end and normal, 0 for no hit. */
typedef int (*sh_planes_trace_fn)(const float from[3], const float to[3], float end[3], float normal[3]);
/* Game thread, once a frame: trace up to `budget` asked places. Returns how many were traced. */
int sh_planes_service(sh_planes_trace_fn trace, int budget);
/* Forget every place (a new map). Game thread. */
#ifdef SH_PARTICLE_FIXTURE
void sh_planes_clear(void);
#endif

/* Move the four vertices of one particle's quad (stride bytes apart, position first) from the game's place
 * `engine_at` to `p`; `aimed` rebuilds the quad along `v` as the game's aimed builder lays it out (far - side,
 * far + side, near - side, near + side), facing `eye`, its length scaled by |v| x stretch when stretch is set. */
void sh_motion_place(float *verts, size_t stride, int aimed, const float engine_at[3], const float p[3],
                     const float v[3], const float eye[3], float stretch);

extern const sig_entry SH_PARTICLEMOTION_SIGNATURES[];   /* NULL-terminated */

/* Detour the particle vertex writer (both renderers). 1 when simulated motion is available. */
int sh_particlemotion_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Children born at their parent's place: a CPU stage cannot birth a particle at another stage's particle, so the
 * product retraces the parent. A parent stage writes `systemPropertiesGPU.emissionTime = -1`; a child writes
 * `1000 + d`, d the parent's stage index less its own (stage array of 0x6E0-byte idParticleStage, constructor loop
 * VK 0x1417C9D39). Child k's parent is k mod the parent's count, and the parent's age at the child's birth follows
 * from both stages' birth offsets (per-stage job, VK 0x1419B1400); one cycle is assumed. */
#define SH_MOTION_STAGE_MAXLIFE      0x10
#define SH_MOTION_STAGE_BUNCHTIME    0x18
#define SH_MOTION_STAGE_COUNT        0x70     /* short: systemProperties.totalParticles */
#define SH_MOTION_STAGE_TIMEOFFSET   0x8C     /* systemProperties.timeOffset */
#define SH_MOTION_STAGE_BUNCHING     0xA4     /* systemProperties.spawnBunching */
#define SH_MOTION_SYSTEM_EMISSION    0x598    /* systemPropertiesGPU.emissionTime: the family link */
#define SH_MOTION_STAGE_SIZE         0x6E0
/* 0 none, 1 a parent, 2 a child. */
int sh_family_role(const uint8_t *stage);
/* A child's parent stage (NULL when the link is out of range). */
const uint8_t *sh_family_parent(const uint8_t *child);
/* Child particle k: its parent's index and the parent's age at the child's birth (s). 0 when a count is 0. */
int sh_family_link(const uint8_t *child, const uint8_t *parent, int k, int *p, float *delta);
/* The life a parent is retraced with: the stage's longest. */
float sh_family_life(const uint8_t *parent);
/* The seed a family's particle p takes, and the place's salt (0 for a stage built in its model's axes). */
uint32_t sh_family_seed(uint32_t salt, int p);
uint32_t sh_family_salt(const float origin[3], const uint8_t *stage);
/* Writes the next n particles the detour moves to the backend log, one "PARTICLES: trace" line each (at most 4,096;
 * 0 stops). */
void sh_particlemotion_trace(long n);
/* Particles moved since the start. */
long sh_particlemotion_moved(void);
/* Particles moved against the scene's planes, and particles that died on them, since the start. */
long sh_particlemotion_collided(void);
long sh_particlemotion_killed(void);
/* Child particles drawn at their parent's place since the start. */
long sh_particlemotion_children(void);

#endif /* SNAPMAP_PLUS_PARTICLEMOTION_H */
