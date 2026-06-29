/* algo.c -- see algo.h. The B2 snaphak_algo reimplementation: the 4 engine-math overrides the
 * [18] cs_dontuse toggle installs, plus the sh_alginfo report. Clean-room from our own RE
 * (the engine decompiles 0x141a82f10 / 828f0 / 19470 / 5eb40 re-
 * confirmed value-precise 2026-06-21). Zero OG SnapHak bytes.
 *
 * FIDELITY (user decision): matmul/inverse/curveEval are reimplemented in f64 ("more precise than the
 * engine's native f32" -- satisfies OG's cs_dontuse contract, portable + deterministic). color-pack is
 * reproduced BIT-EXACT to the OG hook (round-half-up in double). The detours FULL-REPLACE the engine fns
 * (OG's hooks don't chain, so neither do ours -- each op computes the COMPLETE operation, engine ABI
 * exactly). OFF BY DEFAULT; the ON state diverges from OG's x87-80-bit in the last ULPs by design
 * (the 2nd sanctioned divergence).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "algo.h"
#include "commands.h"   /* idCmdArgs / sh_printf */
#include "patch.h"      /* sh_install_detour_sig / sh_uninstall_detour */
#include "signatures.h"
#include "backend_log.h"

/* ----------------------------------------------------------------------------- the 4 ops -------- */

/* 1. matmul: out = A*B, ROW-MAJOR (out[r*4+c] = sum_k A[r*4+k] * B[k*4+c]). DIRECT layout from the
 * FUN_141a82f10 decompile: A read straight, B read column-wise, out[0]=A0*B0+A1*B4+A2*B8+A3*B12 = A's
 * row 0 dotted with B's column 0. Each element accumulated in DOUBLE, stored f32. */
void sh_algo_matmul(const float *A, const float *B, float *out)
{
    if (!A || !B || !out) return;
    /* out may alias A or B in principle; the engine writes straight to param_3 after loading both
     * operands into registers/locals first. Mirror that: snapshot A,B into doubles, then write out. */
    double a[16], b[16];
    for (int i = 0; i < 16; i++) { a[i] = (double)A[i]; b[i] = (double)B[i]; }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double s = a[r*4 + 0] * b[0*4 + c]
                     + a[r*4 + 1] * b[1*4 + c]
                     + a[r*4 + 2] * b[2*4 + c]
                     + a[r*4 + 3] * b[3*4 + c];
            out[r*4 + c] = (float)s;
        }
    }
}

/* The engine's singular epsilon (DAT_14279b060 = 1.0000000168623835e-16f, read DIRECT). The engine tests
 * ABS(det) < epsilon on the f32 determinant; we test the f64 determinant against the same threshold. */
#define B2_ALGO_INV_EPSILON 1.0000000168623835e-16

/* 2. inverse: 4x4 inverse via adjugate/determinant in DOUBLE. Engine contract (FUN_141a828f0): compute
 * det; if |det| < epsilon -> return 0 (singular) and leave out UNTOUCHED; else write the 16 cofactor/det
 * entries and return 1. (Safer than the OG hook FUN_18001ccd0, which wrote 16 unconditionally; we keep the
 * engine flag-and-skip contract -- noted in algo.h.) */
int sh_algo_inverse(const float *M, float *out)
{
    if (!M || !out) return 0;
    double m[16];
    for (int i = 0; i < 16; i++) m[i] = (double)M[i];

    /* Cofactor expansion. inv = adjugate / det. adj[col*4+row] = cofactor(row,col) (transpose of the
     * cofactor matrix). Standard 4x4 closed form (each cofactor a 3x3 determinant). */
    double inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
             + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
             - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
             + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
             - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
             - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
             + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
             - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
             + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
             + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
             - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
             + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
             - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];

    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
             - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
             + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
             - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
             + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    /* det = m row 0 dotted with column 0 of the cofactor matrix (== inv[0],inv[4],inv[8],inv[12]
     * are adj entries = cofactor transposed, so det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] +
     * m[3]*inv[12]). This mirrors the engine's "4 cofactors dotted with row 0". */
    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];

    if (fabs(det) < B2_ALGO_INV_EPSILON)
        return 0;   /* singular -- leave out UNTOUCHED (engine contract) */

    double idet = 1.0 / det;
    for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] * idet);
    return 1;
}

/* 3. packRGBA: float4 -> packed RGBA8, BIT-EXACT to the OG cs_dontuse hook FUN_18001cdc0 (DIRECT capstone):
 * per channel cvtps2pd; mulsd 255.0 (DAT_180038740 f64); addsd 0.5 (DAT_180038728 f64); call floor;
 * cvttsd2si; clamp <0->0, >0xff->0xff. => i = (int)floor((double)f*255.0 + 0.5), clamp [0,255]. Pack the
 * 4 channels R | G<<8 | B<<16 | A<<24 (the engine + hook agree on the byte order). */
static uint32_t pack_channel(float f)
{
    int i = (int)floor((double)f * 255.0 + 0.5);
    if (i < 0)   i = 0;
    if (i > 255) i = 255;
    return (uint32_t)i;
}
uint32_t sh_algo_packrgba(const float *rgba)
{
    if (!rgba) return 0;
    uint32_t r = pack_channel(rgba[0]);
    uint32_t g = pack_channel(rgba[1]);
    uint32_t b = pack_channel(rgba[2]);
    uint32_t a = pack_channel(rgba[3]);
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* 4. curveEval -- keyframed curve, MAIN path in DOUBLE. struct (DIRECT, FUN_141a5eb40): mode bytes
 * c+0x00/01/02, times[count] f32 @c+0xc, values[count] f32 @c+0x10c, count(int) @c+0x20c, edgeMode(int)
 * @c+0x218, period(f32) @c+0x21c. count<1->0; count==1->values[0].
 * c+0x02 set -> ALT mode = uniform Catmull-Rom (cubic spline, tension 0.5) -- FAITHFULLY REIMPLEMENTED in
 * f64 (RATIFIED RE: canonical CR weights + 6 verified constants). The
 * alt path edge-pre-adjusts by edgeMode, brackets (first key with times[i] > adjt), reads 4 control points
 * v[i-2..i+1] edge-aware (extrapolate/clamp/wrap), and returns the raw weighted sum -- NO flush-to-zero.
 * Main path: if c+0x00 set, clamp the ends (t<=times[0]->v0, t>=times[last]->v_last); bracket-find the
 * interval; if c+0x01 set HOLD v_prev; else LINEAR lerp (1-frac)*v_prev + frac*v_cur, with TWO flush-to-zero
 * guards (|v| <= 1e-18f -> 0) on v_prev + v_cur (engine abs-mask 0x7fffffff, thresh DAT_141fd5940=1e-18f). */
#define B2_ALGO_CURVE_FZ_THRESH 1.0e-18   /* DAT_141fd5940 = 1.000000045813705e-18f, in double */
#define CURVE_OFF_MODE0    0x00
#define CURVE_OFF_MODE1    0x01
#define CURVE_OFF_MODE2    0x02    /* alt-mode select: uniform Catmull-Rom (cubic spline) */
#define CURVE_OFF_TIMES    0x0c
#define CURVE_OFF_VALUES   0x10c
#define CURVE_OFF_COUNT    0x20c
#define CURVE_OFF_EDGEMODE 0x218   /* int 0=extrapolate, 1=clamp, 2=wrap */
#define CURVE_OFF_PERIOD   0x21c   /* f32, used by edgeMode==2 (wrap) */

/* SEH-guarded struct reads (the engine hands us the curve object; never trust its shape). */
static int curve_rd_i32(const uint8_t *p, int *out)
{
    __try { *out = *(const int *)p; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int curve_rd_u8(const uint8_t *p, uint8_t *out)
{
    __try { *out = *p; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int curve_rd_f32(const uint8_t *p, int idx, float *out)
{
    __try { *out = ((const float *)p)[idx]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* --- edge-aware readers for the c+0x02 uniform Catmull-Rom alt path (FUN_141a5edb0 / FUN_141a5ecd0) ----
 * The spline reads 4 control points v[i-2..i+1] (and the times[i-1]/times[i] interval), so indices fall
 * OUT of [0,n-1] at the curve ends. The engine handles that by edgeMode (c+0x218):
 *   in-range (0<=idx<=n-1): the array value;
 *   idx<0:    E0 extrapolate (slope*(idx)+v0) | E2 wrap ((idx%n)+n)%n | else clamp to the first;
 *   idx>n-1:  E0 extrapolate (slope*(idx-(n-1))+v_last) | E2 wrap idx%n | else clamp to the last.
 * The TIME reader is the same structure, except wrap adds a whole `period` (c+0x21c) per wrap so the times
 * it returns stay MONOTONIC across the seam (needed so the u = (adjt-t0)/(t1-t0) interval stays positive).
 * E = edgeMode; n = count (>=2 here -- count==1 short-circuits upstream). All reads SEH-guarded.
 *
 * read_value(c, idx): edge-aware value at a (possibly out-of-range) signed index. */
static double curve_read_value(const uint8_t *values, int n, int idx, int edgeMode)
{
    if (idx >= 0 && idx <= n - 1) {
        float v = 0.0f; curve_rd_f32(values, idx, &v); return (double)v;
    }
    if (idx < 0) {
        if (edgeMode == 0) {                 /* extrapolate: (v1-v0)*idx + v0 */
            float v0 = 0.0f, v1 = 0.0f;
            curve_rd_f32(values, 0, &v0); curve_rd_f32(values, 1, &v1);
            return ((double)v1 - (double)v0) * (double)idx + (double)v0;
        }
        if (edgeMode == 2) {                 /* wrap: values[((idx%n)+n)%n] */
            int w = ((idx % n) + n) % n;
            float v = 0.0f; curve_rd_f32(values, w, &v); return (double)v;
        }
        float v0 = 0.0f; curve_rd_f32(values, 0, &v0); return (double)v0;   /* clamp */
    }
    /* idx > n-1 */
    if (edgeMode == 0) {                      /* extrapolate: (v_last-v_prev)*(idx-(n-1)) + v_last */
        float vl = 0.0f, vp = 0.0f;
        curve_rd_f32(values, n - 1, &vl); curve_rd_f32(values, n - 2, &vp);
        return ((double)vl - (double)vp) * (double)(idx - (n - 1)) + (double)vl;
    }
    if (edgeMode == 2) {                      /* wrap: values[idx%n] */
        int w = idx % n;
        float v = 0.0f; curve_rd_f32(values, w, &v); return (double)v;
    }
    float vl = 0.0f; curve_rd_f32(values, n - 1, &vl); return (double)vl;   /* clamp */
}

/* read_time(c, idx): same structure as read_value; wrap adds `period` per wrap so times stay monotonic. */
static double curve_read_time(const uint8_t *times, int n, int idx, int edgeMode, double period)
{
    if (idx >= 0 && idx <= n - 1) {
        float v = 0.0f; curve_rd_f32(times, idx, &v); return (double)v;
    }
    if (idx < 0) {
        if (edgeMode == 0) {                 /* extrapolate: (t1-t0)*idx + t0 */
            float t0 = 0.0f, t1 = 0.0f;
            curve_rd_f32(times, 0, &t0); curve_rd_f32(times, 1, &t1);
            return ((double)t1 - (double)t0) * (double)idx + (double)t0;
        }
        if (edgeMode == 2) {                 /* wrap: times[((idx%n)+n)%n] - (wraps)*period (stay monotonic) */
            int w = ((idx % n) + n) % n;
            int wraps = (n - 1 - idx) / n;   /* whole spans below 0 */
            float v = 0.0f; curve_rd_f32(times, w, &v);
            return (double)v - (double)wraps * period;
        }
        float t0 = 0.0f; curve_rd_f32(times, 0, &t0); return (double)t0;    /* clamp */
    }
    /* idx > n-1 */
    if (edgeMode == 0) {                      /* extrapolate: (t_last-t_prev)*(idx-(n-1)) + t_last */
        float tl = 0.0f, tp = 0.0f;
        curve_rd_f32(times, n - 1, &tl); curve_rd_f32(times, n - 2, &tp);
        return ((double)tl - (double)tp) * (double)(idx - (n - 1)) + (double)tl;
    }
    if (edgeMode == 2) {                      /* wrap: times[idx%n] + (wraps)*period (stay monotonic) */
        int w = idx % n;
        int wraps = idx / n;                  /* whole spans above n-1 */
        float v = 0.0f; curve_rd_f32(times, w, &v);
        return (double)v + (double)wraps * period;
    }
    float tl = 0.0f; curve_rd_f32(times, n - 1, &tl); return (double)tl;    /* clamp */
}

float sh_algo_curveeval(const void *c, float t, uint8_t mode)
{
    (void)mode;   /* the engine r8b is the bracket-find resume hint; our clean bracket recomputes it */
    if (!c) return 0.0f;
    const uint8_t *base = (const uint8_t *)c;

    int count = 0;
    if (!curve_rd_i32(base + CURVE_OFF_COUNT, &count)) return 0.0f;
    if (count < 1)  return 0.0f;
    if (count == 1) {
        float v0 = 0.0f;
        curve_rd_f32(base + CURVE_OFF_VALUES, 0, &v0);
        return v0;
    }

    uint8_t mode0 = 0, mode1 = 0, mode2 = 0;
    curve_rd_u8(base + CURVE_OFF_MODE0, &mode0);
    curve_rd_u8(base + CURVE_OFF_MODE1, &mode1);
    curve_rd_u8(base + CURVE_OFF_MODE2, &mode2);

    const uint8_t *times  = base + CURVE_OFF_TIMES;
    const uint8_t *values = base + CURVE_OFF_VALUES;

    /* c+0x02 ALT MODE: uniform Catmull-Rom (cubic spline, tension 0.5), the engine tail-calls FUN_141a5e6e0.
     * RATIFIED RE: the 4 weights are the canonical CR basis, the 6
     * float constants verified @ their RVAs. Faithful f64 reimpl below; takes its OWN return path (NOT the
     * main lerp). NO flush-to-zero guard on this path -- it returns the raw weighted sum (the main lerp
     * keeps its guards; the spline does not). count<1 / count==1 already short-circuited upstream. */
    if (mode2 != 0) {
        /* edgeMode (c+0x218) + period (c+0x21c) drive the pre-adjust + the edge-aware readers. */
        int   edgeMode = 0;
        float period_f = 0.0f;
        curve_rd_i32(base + CURVE_OFF_EDGEMODE, &edgeMode);
        curve_rd_f32(base + CURVE_OFF_PERIOD, 0, &period_f);
        double period = (double)period_f;

        float tfirst = 0.0f, tlast2 = 0.0f;
        if (!curve_rd_f32(times, 0, &tfirst))             return 0.0f;
        if (!curve_rd_f32(times, count - 1, &tlast2))     return 0.0f;

        /* time_preadjust by edgeMode: 0=extrapolate(adjt=t); 1=clamp t into [t0,t_last];
         * 2=wrap(span=t_last + period; adjt = t - floor(t/span)*span). */
        double adjt = (double)t;
        if (edgeMode == 1) {
            if (adjt < (double)tfirst) adjt = (double)tfirst;
            if (adjt > (double)tlast2) adjt = (double)tlast2;
        } else if (edgeMode == 2) {
            double span = (double)tlast2 + period;
            if (span != 0.0) adjt = (double)t - floor((double)t / span) * span;
        }

        /* bracket_upper = first key with times[i] > adjt (strict). Clamp i into [1, count-1] so i-1 is a
         * valid lower key; the edge-aware readers handle i-2 / i+1 falling out of range. */
        int i = count - 1;
        for (int k = 0; k < count; k++) {
            float tk = 0.0f;
            if (!curve_rd_f32(times, k, &tk)) break;
            if ((double)tk > adjt) { i = k; break; }
        }
        if (i < 1)         i = 1;
        if (i > count - 1) i = count - 1;

        double tt0 = curve_read_time(times, count, i - 1, edgeMode, period);
        double tt1 = curve_read_time(times, count, i,     edgeMode, period);
        double u   = (tt1 != tt0) ? (adjt - tt0) / (tt1 - tt0) : 0.0;

        /* canonical uniform Catmull-Rom basis (tension 0.5), u in [0,1]. */
        double w0 = 0.5 * u * ((2.0 - u) * u - 1.0);
        double w1 = 0.5 * ((3.0 * u - 5.0) * u * u + 2.0);
        double w2 = 0.5 * u * ((4.0 - 3.0 * u) * u + 1.0);
        double w3 = 0.5 * u * u * (u - 1.0);

        double result = w0 * curve_read_value(values, count, i - 2, edgeMode)
                      + w1 * curve_read_value(values, count, i - 1, edgeMode)
                      + w2 * curve_read_value(values, count, i,     edgeMode)
                      + w3 * curve_read_value(values, count, i + 1, edgeMode);
        return (float)result;   /* NO flush-to-zero guard on the alt path */
    }

    float t0 = 0.0f, tlast = 0.0f;
    if (!curve_rd_f32(times, 0, &t0))             return 0.0f;
    if (!curve_rd_f32(times, count - 1, &tlast))  return 0.0f;

    /* c+0x00 set: clamp/hold at the ends (engine: t<=times[0]->values[0]; t>=times[last]->values[last]). */
    if (mode0 != 0) {
        if (t <= t0) {
            float v0 = 0.0f; curve_rd_f32(values, 0, &v0); return v0;
        }
        if (t >= tlast) {
            float vl = 0.0f; curve_rd_f32(values, count - 1, &vl); return vl;
        }
    }

    /* Bracket-find: the smallest idx in [1, count-1] with times[idx] >= t (binary search, mirroring the
     * engine's FUN_141a5e790 interval selection). idx-1 = the previous key. Clamp idx into [1, count-1]. */
    int lo = 0, hi = count - 1, idx = count - 1;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        float tm = 0.0f;
        if (!curve_rd_f32(times, mid, &tm)) break;
        if (t <= tm) { idx = mid; hi = mid - 1; }
        else         { lo = mid + 1; }
    }
    if (idx < 1)         idx = 1;
    if (idx > count - 1) idx = count - 1;

    float t_cur = 0.0f, t_prev = 0.0f, v_cur = 0.0f, v_prev = 0.0f;
    curve_rd_f32(times,  idx,     &t_cur);
    curve_rd_f32(times,  idx - 1, &t_prev);
    curve_rd_f32(values, idx,     &v_cur);
    curve_rd_f32(values, idx - 1, &v_prev);

    /* c+0x01 set: HOLD the previous value (step interpolation -- engine returns values[idx-1]). */
    if (mode1 != 0)
        return v_prev;

    /* LINEAR lerp. frac = (t - t_prev) / (t_cur - t_prev) (degenerate equal-times -> 0, like the engine's
     * fVar3==fVar4 guard). Compute in DOUBLE. */
    double dcur = (double)t_cur, dprev = (double)t_prev;
    if (dcur == dprev)
        return 0.0f;   /* engine: if (times[idx]==times[idx-1]) result = 0.0 */

    double frac = ((double)t - dprev) / (dcur - dprev);

    /* TWO flush-to-zero guards (engine: if (fabsf(v) <= 1e-18f) v = 0) on v_cur and v_prev. */
    double dv_cur  = (fabsf(v_cur)  <= (float)B2_ALGO_CURVE_FZ_THRESH) ? 0.0 : (double)v_cur;
    double dv_prev = (fabsf(v_prev) <= (float)B2_ALGO_CURVE_FZ_THRESH) ? 0.0 : (double)v_prev;

    double res = (1.0 - frac) * dv_prev + frac * dv_cur;
    return (float)res;
}

/* ------------------------------------------------------------------- the cs_dontuse toggle ------- */

/* The 4 sig NAMES (must match signatures.c / signatures.py exactly). */
#define ALGO_SIG_MATMUL  "AlgoMatMul"
#define ALGO_SIG_INVERSE "AlgoInverse"
#define ALGO_SIG_PACK    "AlgoPackRGBA"
#define ALGO_SIG_CURVE   "AlgoCurveEval"

/* FULL-replace detours: OG hooks don't chain, so neither do ours. hook.c requires stolen >= 14; the 14-
 * byte abs-jmp clobber is safe for all 4 (packRGBA's 3rd instr is RIP-rel at off 10, but we never execute
 * the original or the unused trampoline hook.c builds, so the clobber is harmless). */
#define ALGO_STOLEN 14

static const uint8_t *g_algo_module_base = NULL;
static volatile LONG  g_algo_installed   = 0;   /* one-shot install latch for sh_algo_install */
static int            g_algo_on          = 0;   /* the toggle state (0 = off; off by default) */

/* The trampoline/restore handles for the 4 installed detours (NULL = not installed). */
static void *g_tramp_matmul  = NULL;
static void *g_tramp_inverse = NULL;
static void *g_tramp_pack    = NULL;
static void *g_tramp_curve   = NULL;

/* Resolve a named engine site from the shipped sig DB off the cached module base (mirrors sh_commands'
 * resolve_sig_by_name). Fills *out; returns 1 if the name was in the DB (then *out carries the resolve
 * status the install_detour_sig gate checks), 0 if absent / no module base. */
static int algo_resolve_sig(const char *name, sig_result *out)
{
    if (g_algo_module_base == NULL || name == NULL) return 0;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) != 0) continue;
        sig_resolve_one(g_algo_module_base, &BACKEND_ENGINE_SIGNATURES[i], out);
        return 1;
    }
    return 0;
}

/* The 4 detour entry points: thin typedef-cast wrappers so the installed function pointer has the EXACT
 * engine ABI. (sh_algo_* already match, but the explicit hook fns document the install targets + keep the
 * detour-target type local to the install.) */
typedef void     (*matmul_fn) (const float *, const float *, float *);
typedef int      (*inverse_fn)(const float *, float *);
typedef uint32_t (*pack_fn)   (const float *);
typedef float    (*curve_fn)  (const void *, float, uint8_t);

/* Install ONE algo detour by sig name; returns the trampoline (non-NULL) or NULL (refused/failed). */
static void *algo_install_one(const char *name, void *hook)
{
    sig_result r;
    if (!algo_resolve_sig(name, &r)) {
        char line[128];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: snaphak_algo %s not in the signature DB -- cannot install", name);
        backend_log(line);
        return NULL;
    }
    void *tr = sh_install_detour_sig(&r, hook, ALGO_STOLEN);
    if (!tr) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: snaphak_algo %s detour refused/failed (status=%d)", name, (int)r.status);
        backend_log(line);
    }
    return tr;
}

/* Uninstall all currently-installed algo detours (LIFO-ish; each independent). */
static void algo_uninstall_all(void)
{
    if (g_tramp_curve)   { sh_uninstall_detour(g_tramp_curve);   g_tramp_curve   = NULL; }
    if (g_tramp_pack)    { sh_uninstall_detour(g_tramp_pack);    g_tramp_pack    = NULL; }
    if (g_tramp_inverse) { sh_uninstall_detour(g_tramp_inverse); g_tramp_inverse = NULL; }
    if (g_tramp_matmul)  { sh_uninstall_detour(g_tramp_matmul);  g_tramp_matmul  = NULL; }
}

/* [18] cs_dontuse -- the TOGGLE. First call installs all 4 (resolve each + install_detour_sig), sets
 * g_algo_on=1. Next call uninstalls all 4, g_algo_on=0. On ANY per-hook install failure during the ON
 * transition, ROLL BACK the ones already installed + report (don't leave a partial override set live). */
void h_cs_dontuse(struct idCmdArgs *a)
{
    (void)a;
    if (!g_algo_on) {
        /* ON: install all 4. */
        g_tramp_matmul  = algo_install_one(ALGO_SIG_MATMUL,  (void *)(matmul_fn) sh_algo_matmul);
        g_tramp_inverse = algo_install_one(ALGO_SIG_INVERSE, (void *)(inverse_fn)sh_algo_inverse);
        g_tramp_pack    = algo_install_one(ALGO_SIG_PACK,    (void *)(pack_fn)   sh_algo_packrgba);
        g_tramp_curve   = algo_install_one(ALGO_SIG_CURVE,   (void *)(curve_fn)  sh_algo_curveeval);

        if (!g_tramp_matmul || !g_tramp_inverse || !g_tramp_pack || !g_tramp_curve) {
            algo_uninstall_all();   /* roll back any that took */
            sh_printf("cs_dontuse: snaphak_algo override install FAILED -- rolled back, overrides OFF.\n");
            backend_log("B2: snaphak_algo cs_dontuse ON aborted (a hook refused) -- rolled back");
            return;
        }
        g_algo_on = 1;
        sh_printf("snaphak_algo overrides ON (4 ops, f64; color-pack bit-exact).\n");
        backend_log("B2: snaphak_algo overrides ON (matmul/inverse/curveEval f64, color-pack bit-exact)");
    } else {
        /* OFF: uninstall all 4. */
        algo_uninstall_all();
        g_algo_on = 0;
        sh_printf("snaphak_algo overrides OFF (engine math restored).\n");
        backend_log("B2: snaphak_algo overrides OFF (4 detours uninstalled)");
    }
}

/* sh_alginfo -- report our reimpl PRESENT (replaces the cosmetic stub). */
void h_alginfo(struct idCmdArgs *a)
{
    (void)a;
    sh_printf("snaphak_algo: clone reimpl present -- matmul/inverse/curveEval f64 (more-precise than "
              "engine f32), color-pack bit-exact; NOT bit-identical to OG x87-80-bit "
              "(currently %s; off-by-default; see divergence note).\n",
              g_algo_on ? "ON" : "OFF");
}

void sh_algo_install(const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_algo_installed, 1, 0) != 0) return;   /* one-shot */
    g_algo_module_base = module_base;   /* cs_dontuse resolves the 4 algo sigs at FIRE off this base */
    /* OFF BY DEFAULT: install NOTHING here. The handlers are registered by sh_commands' CMD_TABLE. */
    backend_log("B2: snaphak_algo ready (cs_dontuse off-by-default; 4 ops f64 + color-pack bit-exact)");
}

/* ------------------------------------------------------------------- the in-DLL self-test ------- */

/* Helpers for the matmul/inverse checks (all f64-tolerant; the math is computed in f64 then stored f32). */
static int approx_eq(float x, float y, float eps) { return fabsf(x - y) <= eps; }

int sh_algo_selftest(void)
{
    char line[224];
    char whybuf[160];
    const char *why = "not run";
    int ok = 0;

    /* (1) matmul: I * M == M, and a known A*B product. */
    {
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float M[16] = { 1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16 };
        float out[16];
        sh_algo_matmul(I, M, out);
        for (int i = 0; i < 16; i++) {
            if (!approx_eq(out[i], M[i], 1e-4f)) {
                _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE, "matmul I*M != M at [%d] (%g != %g)",
                            i, out[i], M[i]);
                why = whybuf; goto done;
            }
        }
        /* A known product: A = [[1,2,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]], B = M.
         * out row0 = A row0 . B cols = [B0+2*B4, B1+2*B5, B2+2*B6, B3+2*B7] = [1+10,2+12,3+14,4+16]
         *          = [11,14,17,20]; row1 = B row1 = [5,6,7,8]. */
        const float A[16] = { 1,2,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        float p[16];
        sh_algo_matmul(A, M, p);
        const float expect_row0[4] = { 11.0f, 14.0f, 17.0f, 20.0f };
        const float expect_row1[4] = { 5.0f, 6.0f, 7.0f, 8.0f };
        for (int c = 0; c < 4; c++) {
            if (!approx_eq(p[c], expect_row0[c], 1e-4f) || !approx_eq(p[4 + c], expect_row1[c], 1e-4f)) {
                _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE, "matmul A*B wrong at col %d", c);
                why = whybuf; goto done;
            }
        }
    }

    /* (2) inverse: inverse(M)*M ~= I, and a singular M -> flag 0 (out untouched). */
    {
        /* A well-conditioned non-trivial matrix (translation + scale + a shear). */
        const float M[16] = {
            2.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 3.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 4.0f, 0.0f,
            5.0f, 6.0f, 7.0f, 1.0f
        };
        float inv[16], prod[16];
        if (!sh_algo_inverse(M, inv)) { why = "inverse flagged a well-conditioned M singular"; goto done; }
        sh_algo_matmul(inv, M, prod);
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        for (int i = 0; i < 16; i++) {
            if (!approx_eq(prod[i], I[i], 1e-3f)) {
                _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE, "inverse(M)*M != I at [%d] (%g)", i, prod[i]);
                why = whybuf; goto done;
            }
        }
        /* Singular: a zero row (det 0). inverse must return 0 and NOT touch out. */
        const float S[16] = {
            1.0f, 2.0f, 3.0f, 4.0f,
            0.0f, 0.0f, 0.0f, 0.0f,    /* zero row -> singular */
            9.0f, 10.0f, 11.0f, 12.0f,
            13.0f, 14.0f, 15.0f, 16.0f
        };
        float sout[16];
        const float sentinel = -123456.0f;
        for (int i = 0; i < 16; i++) sout[i] = sentinel;
        int rc = sh_algo_inverse(S, sout);
        if (rc != 0) { why = "inverse did not flag a singular M (det 0)"; goto done; }
        for (int i = 0; i < 16; i++) {
            if (sout[i] != sentinel) { why = "inverse touched out on a singular M (should leave untouched)"; goto done; }
        }
    }

    /* (3) packRGBA: BIT-EXACT to the OG hook (round-half-up in double, clamp). Hardcoded expected u32. */
    {
        /* black: all 0 -> 0x00000000. white: all 1 -> 255 each -> 0xFFFFFFFF. */
        const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (sh_algo_packrgba(black) != 0x00000000u) { why = "packRGBA black != 0"; goto done; }
        if (sh_algo_packrgba(white) != 0xFFFFFFFFu) { why = "packRGBA white != 0xFFFFFFFF"; goto done; }
        /* round-half-up: 0.5*255 = 127.5 -> floor(127.5+0.5)=floor(128.0)=128 (0x80). The engine TRUNCATE
         * would give 127 (0x7F) -- this proves we reproduce the HOOK, not the engine. R=0.5 -> 0x80;
         * G=0,B=0,A=1 -> A=0xFF. expected = 0x80 | 0 | 0 | 0xFF<<24 = 0xFF000080. */
        const float half[4] = { 0.5f, 0.0f, 0.0f, 1.0f };
        uint32_t got = sh_algo_packrgba(half);
        if (got != 0xFF000080u) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE,
                "packRGBA 0.5 round-half-up wrong: got 0x%08x expected 0xFF000080", got);
            why = whybuf; goto done;
        }
        /* clamp: f>1 and f<0 saturate. (2.0->255, -1.0->0). expected R=255,G=0,B=255,A=0 = 0x00FF00FF. */
        const float oor[4] = { 2.0f, -1.0f, 2.0f, -1.0f };
        if (sh_algo_packrgba(oor) != 0x00FF00FFu) { why = "packRGBA clamp wrong"; goto done; }
    }

    /* (4) curveEval: a constructed 2-key linear curve -> the expected lerp. Build the struct layout the
     * engine expects (mode bytes c+0x00/01/02 = 0 = linear; times @c+0xc; values @c+0x10c; count @c+0x20c).
     * times = {0,10}, values = {100, 200}. At t=2.5 -> 100 + 0.25*100 = 125. */
    {
        uint8_t buf[0x210];
        memset(buf, 0, sizeof buf);
        *(int *)(buf + CURVE_OFF_COUNT) = 2;
        ((float *)(buf + CURVE_OFF_TIMES))[0]  = 0.0f;
        ((float *)(buf + CURVE_OFF_TIMES))[1]  = 10.0f;
        ((float *)(buf + CURVE_OFF_VALUES))[0] = 100.0f;
        ((float *)(buf + CURVE_OFF_VALUES))[1] = 200.0f;

        float v = sh_algo_curveeval(buf, 2.5f, 0);
        if (!approx_eq(v, 125.0f, 1e-3f)) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE, "curveEval lerp wrong: got %g expected 125", v);
            why = whybuf; goto done;
        }
        /* endpoints + single-key + empty. */
        float ve = sh_algo_curveeval(buf, 0.0f, 0);
        if (!approx_eq(ve, 100.0f, 1e-3f)) { why = "curveEval t=t0 != v0"; goto done; }
        float vf = sh_algo_curveeval(buf, 10.0f, 0);
        if (!approx_eq(vf, 200.0f, 1e-3f)) { why = "curveEval t=tlast != vlast"; goto done; }
        *(int *)(buf + CURVE_OFF_COUNT) = 1;
        float v1 = sh_algo_curveeval(buf, 99.0f, 0);
        if (!approx_eq(v1, 100.0f, 1e-3f)) { why = "curveEval count==1 != values[0]"; goto done; }
        *(int *)(buf + CURVE_OFF_COUNT) = 0;
        float v0 = sh_algo_curveeval(buf, 99.0f, 0);
        if (v0 != 0.0f) { why = "curveEval count<1 != 0"; goto done; }
    }

    /* (5) curveEval c+0x02 ALT mode = uniform Catmull-Rom (tension 0.5). Buffer must span past edgeMode
     * (c+0x218) + period (c+0x21c); use 0x220. mode2=1 selects the spline path.
     * INTERIOR case: a fully-interior segment (i-2 .. i+1 all in range) on a NON-linear key set, so the
     * value would be WRONG if it fell back to the linear lerp. times={0,1,2,3} values={0,1,8,27}, t=1.5.
     *   bracket: first key times[i]>1.5 -> i=2; u=(1.5-1)/(2-1)=0.5.
     *   weights @u=0.5: w0=-0.0625 w1=0.5625 w2=0.5625 w3=-0.0625 (sum 1, computed independently).
     *   result = -0.0625*0 + 0.5625*1 + 0.5625*8 - 0.0625*27 = 0.5625 + 4.5 - 1.6875 = 3.375.
     *   (a pure-linear lerp v[1]..v[2] @0.5 would give 4.5 -- so 3.375 proves the CR branch is live.) */
    {
        uint8_t buf[0x220];
        memset(buf, 0, sizeof buf);
        buf[CURVE_OFF_MODE2] = 1;                          /* select the alt (spline) path */
        *(int *)(buf + CURVE_OFF_COUNT)    = 4;
        *(int *)(buf + CURVE_OFF_EDGEMODE) = 0;            /* extrapolate */
        ((float *)(buf + CURVE_OFF_TIMES))[0]  = 0.0f;
        ((float *)(buf + CURVE_OFF_TIMES))[1]  = 1.0f;
        ((float *)(buf + CURVE_OFF_TIMES))[2]  = 2.0f;
        ((float *)(buf + CURVE_OFF_TIMES))[3]  = 3.0f;
        ((float *)(buf + CURVE_OFF_VALUES))[0] = 0.0f;
        ((float *)(buf + CURVE_OFF_VALUES))[1] = 1.0f;
        ((float *)(buf + CURVE_OFF_VALUES))[2] = 8.0f;
        ((float *)(buf + CURVE_OFF_VALUES))[3] = 27.0f;

        float vs = sh_algo_curveeval(buf, 1.5f, 0);
        if (!approx_eq(vs, 3.375f, 1e-3f)) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE,
                "curveEval CR interior wrong: got %g expected 3.375", vs);
            why = whybuf; goto done;
        }

        /* EDGE case: i-2 < 0 exercises the edge-aware value reader. Same key set, t=0.5.
         *   bracket: first key times[i]>0.5 -> i=1; u=(0.5-0)/(1-0)=0.5; v[i-2]=v[-1] is out of range.
         *   edgeMode=0 (extrapolate): v[-1] = (v1-v0)*(-1)+v0 = (1-0)*(-1)+0 = -1.
         *   result = w0*(-1) + w1*0 + w2*1 + w3*8 = 0.0625 + 0 + 0.5625 - 0.5 = 0.125. */
        float ve = sh_algo_curveeval(buf, 0.5f, 0);
        if (!approx_eq(ve, 0.125f, 1e-3f)) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE,
                "curveEval CR edge(extrap i-2<0) wrong: got %g expected 0.125", ve);
            why = whybuf; goto done;
        }

        /* EDGE case, clamp variant: edgeMode=1 -> v[-1] clamps to v[0]=0 (the `else` reader branch). t=0.5.
         *   result = w0*0 + w1*0 + w2*1 + w3*8 = 0 + 0 + 0.5625 - 0.5 = 0.0625.
         *   (clamp also clamps adjt into [t0,t_last]; here t=0.5 is already in [0,3], so the bracket/u are
         *   unchanged from the extrapolate case -- only the out-of-range v[-1] read differs.) */
        *(int *)(buf + CURVE_OFF_EDGEMODE) = 1;            /* clamp */
        float vc = sh_algo_curveeval(buf, 0.5f, 0);
        if (!approx_eq(vc, 0.0625f, 1e-3f)) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE,
                "curveEval CR edge(clamp i-2<0) wrong: got %g expected 0.0625", vc);
            why = whybuf; goto done;
        }
    }

    ok = 1;

done:
    if (ok) {
        backend_log("B2: snaphak_algo self-test PASS (matmul/inverse/colorpack/curve+spline)");
    } else {
        _snprintf_s(line, sizeof line, _TRUNCATE, "B2: snaphak_algo self-test FAIL (%s)", why);
        backend_log(line);
    }
    return ok;
}
