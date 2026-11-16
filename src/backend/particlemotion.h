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
} sh_motion;

/* The emitter a particle belongs to: its origin and its three axes in the world. */
typedef struct sh_motion_frame {
    float origin[3];
    float x[3], y[3], z[3];
} sh_motion_frame;

/* idParticleStage offsets of build 13954591 (the engine's own reflection; the same in both executables). */
#define SH_MOTION_STAGE_FLAGS        0x44     /* bit 0x20: gpuStage */
#define SH_MOTION_STAGE_GPU          0x20
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

/* Add the stage's launch ball to a particle's launch velocity (the same for the same seed). */
void sh_motion_launch(const sh_motion *m, uint32_t seed, float v0[3]);

/* The step a particle of this life is integrated with (seconds). */
float sh_motion_step(float life);

/* Where a particle born at p0 with velocity v0 is after `age` seconds, and its velocity there. `seed` is the
 * particle's own; the same arguments always give the same answer. Pure. */
void sh_motion_run(const sh_motion *m, const sh_motion_frame *frame, uint32_t seed, float age, float life,
                   const float p0[3], const float v0[3], float p[3], float v[3]);

/* Move the four vertices of one particle's quad (stride bytes apart, position first) from the game's place
 * `engine_at` to `p`; `aimed` rebuilds the quad along `v` as the game's aimed builder lays it out (far - side,
 * far + side, near - side, near + side), facing `eye`, its length scaled by |v| x stretch when stretch is set. */
void sh_motion_place(float *verts, size_t stride, int aimed, const float engine_at[3], const float p[3],
                     const float v[3], const float eye[3], float stretch);

extern const sig_entry SH_PARTICLEMOTION_SIGNATURES[];   /* NULL-terminated */

/* Detour the particle vertex writer (both renderers). 1 when simulated motion is available. */
int sh_particlemotion_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Particles moved since the start (for the test bench). */
long sh_particlemotion_moved(void);

#endif /* SNAPMAP_PLUS_PARTICLEMOTION_H */
