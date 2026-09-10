/* Optional cs_dontuse math overrides and the sh_alginfo status command.
 * Matrix and curve calculations use f64; color packing preserves round-half-up
 * behavior. See docs/fidelity.md for the precision differences from SnapHak. */
#ifndef BACKEND_B2_ALGO_H
#define BACKEND_B2_ALGO_H

#include <stdint.h>

/* Hook bodies can also be called by the in-process math checks. */

/* Row-major 4x4 product, accumulated in double and stored as float. out may alias an input. */
void     sh_algo_matmul(const float *A, const float *B, float *out);

/* Write the 4x4 inverse and return 1, or leave out untouched and return 0 if singular. */
int      sh_algo_inverse(const float *M, float *out);

/* Round channels with floor(f*255+0.5), clamp to [0,255], and pack R|G<<8|B<<16|A<<24. */
uint32_t sh_algo_packrgba(const float *rgba);

/* Evaluate linear, step, or uniform Catmull-Rom curves in double.
 * mode is the engine bracket-search hint; this implementation recomputes the bracket. */
float    sh_algo_curveeval(const void *c, float t, uint8_t mode);

/* Cache the host image for signature resolution. Overrides remain off until toggled. */
void sh_algo_install(const uint8_t *module_base);

/* Console handlers registered by commands.c. */
struct idCmdArgs;
void h_cs_dontuse(struct idCmdArgs *a);    /* Toggle all four overrides. */
void h_alginfo(struct idCmdArgs *a);       /* Report available overrides. */

/* Run known-vector math checks without engine state; return 1 on success, 0 on failure. */
int sh_algo_selftest(void);

#endif /* BACKEND_B2_ALGO_H */
