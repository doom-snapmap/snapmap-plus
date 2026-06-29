/* algo.h -- B2 snaphak_algo: the clean-room reimplementation of OG SnapHak's 4 engine-math
 * overrides (the [18] cs_dontuse "Overrides some calculations in the engine to be more precise"
 * feature) + the sh_alginfo report.
 *
 * OG SnapHak vendored a closed SIMD lib (snaphak_algo.dll) that, when cs_dontuse is toggled ON,
 * FULL-REPLACES four engine math functions with higher-precision versions (it ran the engine's
 * IDENTICAL algorithm in x87 80-bit long double, then rounded back to f32). The OG hooks do NOT
 * chain/trampoline (they `mov rax,fn; push rax; ret` over the engine prologue), so each replacement
 * computes the COMPLETE operation and matches the engine ABI exactly. RE'd DIRECT from the engine
 * decompiles, re-confirmed value-precise 2026-06-21.
 *
 * The 4 pairs (engine fn <- our hook FULL-replaces it):
 *   1. matmul    engine 0x1a82f10  void f(const float*A rcx[16], const float*B rdx[16], float*out r8[16])
 *                4x4 * 4x4, ROW-MAJOR out = A*B (out[r*4+c] = sum_k A[r*4+k]*B[k*4+c]).
 *   2. inverse   engine 0x1a828f0  bool f(const float*M rcx[16], float*out rdx[16])
 *                4x4 inverse; AL=invertible; singular (|det|<epsilon) -> AL=0, out UNTOUCHED.
 *   3. packRGBA  engine 0x1a19470  uint32 f(const float*rgba rcx[4]) -> R|G<<8|B<<16|A<<24.
 *   4. curveEval engine 0x1a5eb40  float f(const void*c rcx, float t xmm1, uint8_t mode r8b) -> xmm0.
 *
 * CLONE FIDELITY (user decision): implement matmul/inverse/curveEval in f64 (NOT OG's x87 80-bit) --
 * "more precise than the engine's native f32" satisfies OG's stated contract, is portable + deterministic,
 * and avoids reproducing the engine's x87 80-bit precision. color-pack is reproduced BIT-EXACT to the OG hook
 * (round-half-up in double). cs_dontuse is OFF BY DEFAULT (installs nothing at startup); when ON, the
 * matmul/inverse/curve ops diverge from OG's last ULPs BY DESIGN (the 2nd sanctioned divergence after the fault-shield).
 *
 * Clean-room: ported from our own RE (the engine decompiles). Zero OG SnapHak bytes.
 */
#ifndef BACKEND_B2_ALGO_H
#define BACKEND_B2_ALGO_H

#include <stdint.h>

/* ---- the 4 ops (the math, callable independently of the live engine -- the self-test exercises them).
 * These are the EXACT hook bodies the detours install (the detours are plain typedef casts of these). */

/* 1. matmul: out = A*B, row-major. Each out element accumulated in DOUBLE, stored f32. */
void     sh_algo_matmul(const float *A, const float *B, float *out);

/* 2. inverse: 4x4 inverse in DOUBLE (adjugate/det). Returns 1 (invertible) + writes 16, or 0 (singular)
 * + leaves out UNTOUCHED -- the engine contract (|det| < epsilon = singular). */
int      sh_algo_inverse(const float *M, float *out);

/* 3. packRGBA: float4 -> packed RGBA8. BIT-EXACT to the OG cs_dontuse hook: per channel
 * i = (int)floor((double)f*255.0 + 0.5) (round-half-up in double), clamp [0,255]; R|G<<8|B<<16|A<<24. */
uint32_t sh_algo_packrgba(const float *rgba);

/* 4. curveEval: keyframed-curve eval, MAIN path in DOUBLE. The c+0x02 ALT (cubic-spline) mode is a
 * documented limitation -- not faithfully reproducible from the decompile; we fall back to the main
 * linear/hold path + log it once. mode is the engine's r8b arg (the bracket-find resume hint). */
float    sh_algo_curveeval(const void *c, float t, uint8_t mode);

/* ---- the cs_dontuse toggle + sh_alginfo (the two console handlers; registered by sh_commands' CMD_TABLE,
 * their deps cached here). sh_algo_install caches the DOOM module base so the toggle can resolve the 4
 * AlgoMatMul/AlgoInverse/AlgoPackRGBA/AlgoCurveEval sigs at FIRE. Installs NOTHING (off by default). */
void sh_algo_install(const uint8_t *module_base);

/* The two handlers (idCmdArgs* ABI; extern so sh_commands' CMD_TABLE references them without drift). */
struct idCmdArgs;
void h_cs_dontuse(struct idCmdArgs *a);    /* [18] toggle: 1st call install 4, 2nd uninstall 4 */
void h_alginfo(struct idCmdArgs *a);       /* sh_alginfo: report our reimpl PRESENT */

/* ---- in-DLL self-test (install-time, like sh_smoke / sh_patch_selftest; NO engine state). Runs the 4
 * ops on known inputs + checks results (matmul I*M==M + a known product; inverse(M)*M ~= I + a singular
 * M -> flag 0; packRGBA known colors == hardcoded u32 BIT-EXACT; curveEval a 2-key curve == the lerp).
 * Logs "B2: snaphak_algo self-test PASS ..." or a specific FAIL. Returns 1 PASS / 0 FAIL. */
int sh_algo_selftest(void);

#endif /* BACKEND_B2_ALGO_H */
