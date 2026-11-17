/* particlemotion.h -- simulated motion for a CPU particle stage (particlemotion.c, particlemotion_sim.c).
 *
 * DOOM 2016 places a CPU particle in closed form: where it was born, a straight flight along its direction at
 * its speed, a uniform acceleration, gravity, and a custom path added on top (the stage's origin function, VK
 * 0x19B8080, GL 0x19AC350). Nothing in it turns a particle around an axis, slows it by its own velocity or
 * shakes it, so a stage cannot swirl, feel air drag or jitter. Snapmap+ adds that for a stage that asks: the
 * particle's flight is integrated from its birth, in fixed steps, under the forces the stage names. A particle's
 * place stays a function of its seed and its age alone (the steps are replayed from birth each frame), so it
 * needs no state, survives a save or a pause and draws the same in every view.
 *
 * A stage asks through its own decl. A CPU stage (`gpuStage` unset) never reads its GPU blocks; with
 * `vectorFieldGPU.repeat = true` Snapmap+ reads them as the stage's motion (lengths in world units, times in
 * seconds; a term whose strength or drag is 0 is off, and every field below is 0 unless the decl writes it):
 *
 *   decl key                                          meaning
 *   vectorFieldGPU.repeat                             true: this stage's motion is simulated
 *   accelerationGPU.acceleration  x y z               acceleration, world axes (units / s^2); gravity belongs here
 *   velocityGPU.offset            x y z               wind: the velocity of the air, world axes (units / s)
 *   vectorFieldGPU.tightness                          wind drag (1 / s): dv/dt += (wind - v) * drag
 *   vectorFieldGPU.offset         x y z               vortex axis, in the emitter's axes (not normalised)
 *   vectorFieldGPU.initialRotation  pitch yaw roll    vortex falloff (units), angular speed (1 / s), drag (1 / s):
 *                                                     dv/dt += (speed * axis x r / (1 + |r|^2 / falloff^2) - v) * drag,
 *                                                     r from the emitter's origin
 *   vectorFieldGPU.initialRotationVariation  p y r    noise field: its speed in time, its scale (1 / units), its
 *                                                     drag (1 / s): dv/dt += (strength * N(p * scale + t * speed) - v) * drag
 *   vectorFieldGPU.rotationSpeed  pitch yaw roll      noise field strength (units / s); brownian factor
 *                                                     (units / s^1.5): each step adds (u - 0.5) * 2 * factor / sqrt(dt)
 *                                                     per axis; velocity stretch (s / unit): a quad aligned to its
 *                                                     velocity is drawn its own length x speed x this (0: its own length)
 *   vectorFieldGPU.rotationSpeedVariation  p y r      attractor kind (0 none, 1 point, 2 axis, 3 plane), strength
 *                                                     (units / s^2), radius (units): dv/dt += strength * dir / (1 + d^2 / radius^2)
 *   distributionGPU.offset        x y z               the attractor's point, or its axis or plane normal, in the
 *                                                     emitter's axes
 *   geometryGPU.sizeVariation     x y                 drag toward the emitter at the particle's birth and at its
 *                                                     death (1 / s), by 1 / (1 + |r|^2 / radius^2)
 *   physicsGPU.massVariation                          that drag's radius (units)
 *   systemPropertiesGPU.boundsExpansion  x            launch ball (units / s): a velocity in a random direction,
 *                                                     this size x sqrt(u), added to the particle's own at birth
 *   systemPropertiesGPU.boundsExpansion  y z          launch speed factor: the velocity the game launches the
 *                                                     particle with is multiplied by a number drawn between y and
 *                                                     z, once per particle (both 0: as launched). A stage's three
 *                                                     speed parms draw apart, so a speed per particle along one
 *                                                     direction is the stage's speed on all three times this
 *
 * Collision with the scene. A stage that asks and whose `colorGPU.fadeColor.x` is over 0 collides with the
 * surfaces under and behind its emitter (DOOM Eternal's hard collision, which tests the screen's depth on the GPU;
 * a CPU particle has no depth, so the planes are found by the engine's own trace, on the game thread, once per
 * emitter place: particlemotion_planes.c):
 *
 *   colorGPU.fadeColor            x y z w             thickness (units; over 0: the stage collides), minimal life
 *                                                     time (s), penetration threshold (units), velocity threshold
 *                                                     (units / s; 0: a particle at rest is never killed)
 *   physicsGPU.elasticity, .restitution               the reflected velocity's factor; the share of its part along
 *                                                     the normal kept (the stage reset's 0.5 each, Eternal's too)
 *   systemPropertiesGPU.timeOffset                    the depth offset (units)
 *
 * Each step, after the forces are summed and before the integrate, Eternal's COLLISION_TEST, RESPONSE and KILL
 * (its simulate commands 10, 11, 12): the place one step ahead is behind a plane by less than the thickness and the
 * particle is older than the minimal life time: it steps back by v dt, its velocity is reflected and times the
 * elasticity, then loses (1 - restitution) of its normal part; it dies (its quad is folded to a point) when it is
 * less than the penetration threshold behind the plane and slower than the velocity threshold. The test needs the
 * particle's place in the world: a stage that collides writes `isTransparencySorted = true` (stage flag 0x02),
 * which makes the game build it in world axes; one that does not is drawn without collision, and the backend log
 * says so once. Planes come from a mod script's map (the game-thread service runs with the script tick); until an
 * emitter's planes are found, its particles fly without them.
 *
 * The emitter's scale (an fx item's `size`). The game scales a particle's whole place by it: for a stage whose
 * material is transparency sorted the origin function answers in the world and the scale is in the axes it applies;
 * for any other stage it answers in the emitter's own axes, unscaled, and the scale is applied when the model is
 * drawn. The simulation is DOOM Eternal's: the forces see the scaled places, the velocity stays in the stage's own
 * units and each step moves the particle by velocity x step x scale. So the detour takes the scale out of the launch
 * velocity it measures and gives it to the steps; where the origin function answers unscaled the scale is 1 and the
 * forces that depend on a place (the vortex, the noise field, the attractors, the emitter drag) see unscaled
 * places.
 *
 * The forces are DOOM Eternal's particle simulation commands (its `gpuparticlesimulate` program: vortex, vortex
 * grid, velocity field, brownian, attractor, emitter drag, acceleration, integrate), with its formulas and its
 * integrator: all forces are summed at the step's start, then v += a dt and p += (v - a dt / 2) dt. The step is the
 * particle's life / 96, held between 1 / 60 s and 0.05 s. Eternal's collision with the scene's depth is not
 * here: a CPU particle has no depth to test.
 *
 * No stage the game ships asks: none of its 9,834 CPU stages writes `vectorFieldGPU` at all. The detour sits on
 * the function that writes one particle's vertices (VK 0x19AFCE0, GL 0x19A3FB0) and returns to the game's own
 * code for every stage that does not ask; for one that does, the game writes the quad as always and Snapmap+
 * then moves it to the simulated place (a quad aligned to its velocity is rebuilt along the simulated velocity). */
#ifndef SNAPMAP_PLUS_PARTICLEMOTION_H
#define SNAPMAP_PLUS_PARTICLEMOTION_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* One stage's motion, as the decl keys above give it. */
typedef struct sh_motion {
    float accel[3];
    float wind[3];
    float wind_drag;
    float vortex_axis[3];
    float vortex_falloff, vortex_speed, vortex_drag;
    float noise_speed, noise_scale, noise_drag, noise_strength;
    float brownian;
    float stretch;
    float attract_kind, attract_strength, attract_radius;
    float attract_at[3];
    float emitter_drag0, emitter_drag1, emitter_drag_radius;
    float launch;
    float launch_lo, launch_hi;
    float collide_thickness, collide_min_life, collide_penetration, collide_speed;
    float collide_elasticity, collide_restitution, collide_offset;
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
#define SH_MOTION_DISTRIBUTION_OFF   0x634
#define SH_MOTION_VELOCITY_OFF       0x650
#define SH_MOTION_ACCELERATION       0x65C
#define SH_MOTION_PHYSICS_MASSVAR    0x678
#define SH_MOTION_FIELD_OFFSET       0x694
#define SH_MOTION_FIELD_ROTATION     0x6A0
#define SH_MOTION_FIELD_ROTATIONVAR  0x6AC
#define SH_MOTION_FIELD_SPEED        0x6B8
#define SH_MOTION_FIELD_SPEEDVAR     0x6C4
#define SH_MOTION_FIELD_TIGHTNESS    0x6D4
#define SH_MOTION_FIELD_REPEAT       0x6D8

/* 1 when the stage at `stage` asks for simulated motion. */
int sh_motion_asked(const uint8_t *stage);

/* Read a stage's motion from its fields. */
void sh_motion_read(const uint8_t *stage, sh_motion *out);

/* A particle's launch velocity by the stage: times its launch speed factor, plus its launch ball (the same for
 * the same seed). */
void sh_motion_launch(const sh_motion *m, uint32_t seed, float v0[3]);

/* The step a particle of this life is integrated with (seconds). */
float sh_motion_step(float life);

/* The emitter's scale from the lengths of the three axes the origin function applies: the largest, 1 when they
 * are not a scale. */
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

/* 1 when the stage collides with the scene. */
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
void sh_planes_clear(void);
/* The planes of one place by the trace (the service's own rule, for the test): the surface under it and, for an
 * emitter that does not face up, the one behind it. */
void sh_planes_find(sh_planes_trace_fn trace, const float origin[3], const float zaxis[3], sh_motion_planes *out);

/* Move the four vertices of one particle's quad (stride bytes apart, position first) from the game's place
 * `engine_at` to `p`; `aimed` rebuilds the quad along `v` as the game's aimed builder lays it out (far - side,
 * far + side, near - side, near + side), facing `eye`, its length scaled by |v| x stretch when stretch is set. */
void sh_motion_place(float *verts, size_t stride, int aimed, const float engine_at[3], const float p[3],
                     const float v[3], const float eye[3], float stretch);

extern const sig_entry SH_PARTICLEMOTION_SIGNATURES[];   /* NULL-terminated */

/* Detour the particle vertex writer (both renderers). 1 when simulated motion is available. */
int sh_particlemotion_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Children born at their parent's place (DOOM Eternal's spawnChildrenNEW, condition IN_TIMESPAN: a parent particle
 * spawns children at a rate over a window of its life, each born where the parent is; Eternal does it on the GPU).
 * A CPU stage's particles cannot be born at another stage's particle, so the product retraces the parent: a parent
 * stage writes `systemPropertiesGPU.emissionTime = -1` and its particles take the product's random numbers (a seed
 * from the particle's index and its emitter's place); a child stage writes `emissionTime = 1000 + d`, d the parent's
 * stage index less its own (the decl's stages are one array of idParticleStage, 0x6E0 bytes each: the allocation
 * count x 0x6E0 and its constructor loop, VK 0x1417C9D39). Child k's parent is p = k mod the parent's count; the
 * game gives particle i of a stage its birth i x (spawnBunching x bunchTime) / count after its cycle starts, plus
 * the stage's timeOffset (the per-stage job, VK 0x1419B1400), so the parent's age at the child's birth is known
 * from the two stages alone. The child is drawn at its own birth offset from the emitter added to the parent's
 * place then (the parent's flight retraced with its seed), not drawn when the parent was not yet born or already
 * dead. The converter gives the child stage the parent's window (timeOffset = delay, spawn window = duration) and
 * count x rate x duration particles. One cycle is assumed (the children of a burst). */
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
/* The share of the parent's velocity a child takes at birth (Eternal's SPAWN command is not read: 0). */
float sh_family_inherit(const uint8_t *child);

/* Particles moved since the start (for the test bench). */
/* Write the next n particles the detour moves to the backend log, one "PARTICLES: trace" line each (at most 4,096;
 * 0 stops): for comparing the live flight with Doomforge's copy of the arithmetic. */
void sh_particlemotion_trace(long n);
long sh_particlemotion_moved(void);
/* Particles moved against the scene's planes, and particles that died on them, since the start. */
long sh_particlemotion_collided(void);
long sh_particlemotion_killed(void);
/* Child particles drawn at their parent's place since the start. */
long sh_particlemotion_children(void);

#endif /* SNAPMAP_PLUS_PARTICLEMOTION_H */
