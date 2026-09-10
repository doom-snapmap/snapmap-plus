/* Optional higher-precision matrix, color, and curve overrides for cs_dontuse.
 * Matrix and curve operations use f64 rather than the original x87 precision;
 * color packing keeps its round-half-up behavior. Hooks replace whole functions
 * and are disabled until the command enables them. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "algo.h"
#include "commands.h"
#include "patch.h"
#include "signatures.h"
#include "backend_log.h"

/* Engine math overrides. */

/* Row-major out = A * B, accumulated in double and stored as float. */
void sh_algo_matmul(const float *A, const float *B, float *out)
{
    if (!A || !B || !out) return;
    /* Snapshot both operands so out may alias either input. */
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

/* Preserve the engine singularity threshold while computing the determinant in f64. */
#define B2_ALGO_INV_EPSILON 1.0000000168623835e-16

/* Return 0 without touching out when the matrix is singular. */
int sh_algo_inverse(const float *M, float *out)
{
    if (!M || !out) return 0;
    double m[16];
    for (int i = 0; i < 16; i++) m[i] = (double)M[i];

    /* Compute the adjugate, the transpose of the cofactor matrix. */
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


    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];

    if (fabs(det) < B2_ALGO_INV_EPSILON)
        return 0;

    double idet = 1.0 / det;
    for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] * idet);
    return 1;
}

/* Round each channel half-up in double, clamp to [0,255], then pack RGBA bytes. */
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

/* Curve mode bytes select endpoint clamping, step interpolation, or Catmull-Rom.
 * Only the linear path flushes small values to zero; the spline keeps its raw sum. */
#define B2_ALGO_CURVE_FZ_THRESH 1.0e-18   /* DAT_141fd5940 = 1.000000045813705e-18f, in double */
#define CURVE_OFF_MODE0    0x00
#define CURVE_OFF_MODE1    0x01
#define CURVE_OFF_MODE2    0x02    /* alt-mode select: uniform Catmull-Rom (cubic spline) */
#define CURVE_OFF_TIMES    0x0c
#define CURVE_OFF_VALUES   0x10c
#define CURVE_OFF_COUNT    0x20c
#define CURVE_OFF_EDGEMODE 0x218   /* int 0=extrapolate, 1=clamp, 2=wrap */
#define CURVE_OFF_PERIOD   0x21c   /* f32, used by edgeMode==2 (wrap) */

/* Curve objects belong to the engine; guard reads against invalid pointers. */
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

/* Spline control points can fall outside [0,n-1]. Edge mode 0 extrapolates,
 * 1 clamps, and 2 wraps. Wrapped times also advance by period to stay monotonic.
 * The caller has already handled curves with fewer than two keys. */
static double curve_read_value(const uint8_t *values, int n, int idx, int edgeMode)
{
    if (idx >= 0 && idx <= n - 1) {
        float v = 0.0f; curve_rd_f32(values, idx, &v); return (double)v;
    }
    if (idx < 0) {
        if (edgeMode == 0) {
            float v0 = 0.0f, v1 = 0.0f;
            curve_rd_f32(values, 0, &v0); curve_rd_f32(values, 1, &v1);
            return ((double)v1 - (double)v0) * (double)idx + (double)v0;
        }
        if (edgeMode == 2) {
            int w = ((idx % n) + n) % n;
            float v = 0.0f; curve_rd_f32(values, w, &v); return (double)v;
        }
        float v0 = 0.0f; curve_rd_f32(values, 0, &v0); return (double)v0;
    }

    if (edgeMode == 0) {
        float vl = 0.0f, vp = 0.0f;
        curve_rd_f32(values, n - 1, &vl); curve_rd_f32(values, n - 2, &vp);
        return ((double)vl - (double)vp) * (double)(idx - (n - 1)) + (double)vl;
    }
    if (edgeMode == 2) {
        int w = idx % n;
        float v = 0.0f; curve_rd_f32(values, w, &v); return (double)v;
    }
    float vl = 0.0f; curve_rd_f32(values, n - 1, &vl); return (double)vl;
}

/* Add a period per wrap so the interpolation interval remains monotonic. */
static double curve_read_time(const uint8_t *times, int n, int idx, int edgeMode, double period)
{
    if (idx >= 0 && idx <= n - 1) {
        float v = 0.0f; curve_rd_f32(times, idx, &v); return (double)v;
    }
    if (idx < 0) {
        if (edgeMode == 0) {
            float t0 = 0.0f, t1 = 0.0f;
            curve_rd_f32(times, 0, &t0); curve_rd_f32(times, 1, &t1);
            return ((double)t1 - (double)t0) * (double)idx + (double)t0;
        }
        if (edgeMode == 2) {
            int w = ((idx % n) + n) % n;
            int wraps = (n - 1 - idx) / n;
            float v = 0.0f; curve_rd_f32(times, w, &v);
            return (double)v - (double)wraps * period;
        }
        float t0 = 0.0f; curve_rd_f32(times, 0, &t0); return (double)t0;
    }

    if (edgeMode == 0) {
        float tl = 0.0f, tp = 0.0f;
        curve_rd_f32(times, n - 1, &tl); curve_rd_f32(times, n - 2, &tp);
        return ((double)tl - (double)tp) * (double)(idx - (n - 1)) + (double)tl;
    }
    if (edgeMode == 2) {
        int w = idx % n;
        int wraps = idx / n;
        float v = 0.0f; curve_rd_f32(times, w, &v);
        return (double)v + (double)wraps * period;
    }
    float tl = 0.0f; curve_rd_f32(times, n - 1, &tl); return (double)tl;
}

float sh_algo_curveeval(const void *c, float t, uint8_t mode)
{
    (void)mode;   /* The engine resume hint is unused; bracket search starts from scratch. */
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

    /* Uniform Catmull-Rom uses four edge-aware control points and no zero flush. */
    if (mode2 != 0) {

        int   edgeMode = 0;
        float period_f = 0.0f;
        curve_rd_i32(base + CURVE_OFF_EDGEMODE, &edgeMode);
        curve_rd_f32(base + CURVE_OFF_PERIOD, 0, &period_f);
        double period = (double)period_f;

        float tfirst = 0.0f, tlast2 = 0.0f;
        if (!curve_rd_f32(times, 0, &tfirst))             return 0.0f;
        if (!curve_rd_f32(times, count - 1, &tlast2))     return 0.0f;

        /* Wrap uses last-key time plus period as the span, matching the engine. */
        double adjt = (double)t;
        if (edgeMode == 1) {
            if (adjt < (double)tfirst) adjt = (double)tfirst;
            if (adjt > (double)tlast2) adjt = (double)tlast2;
        } else if (edgeMode == 2) {
            double span = (double)tlast2 + period;
            if (span != 0.0) adjt = (double)t - floor((double)t / span) * span;
        }

        /* Find the first time strictly above adjt; edge readers handle outer controls. */
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

        /* Uniform Catmull-Rom basis, tension 0.5. */
        double w0 = 0.5 * u * ((2.0 - u) * u - 1.0);
        double w1 = 0.5 * ((3.0 * u - 5.0) * u * u + 2.0);
        double w2 = 0.5 * u * ((4.0 - 3.0 * u) * u + 1.0);
        double w3 = 0.5 * u * u * (u - 1.0);

        double result = w0 * curve_read_value(values, count, i - 2, edgeMode)
                      + w1 * curve_read_value(values, count, i - 1, edgeMode)
                      + w2 * curve_read_value(values, count, i,     edgeMode)
                      + w3 * curve_read_value(values, count, i + 1, edgeMode);
        return (float)result;
    }

    float t0 = 0.0f, tlast = 0.0f;
    if (!curve_rd_f32(times, 0, &t0))             return 0.0f;
    if (!curve_rd_f32(times, count - 1, &tlast))  return 0.0f;


    if (mode0 != 0) {
        if (t <= t0) {
            float v0 = 0.0f; curve_rd_f32(values, 0, &v0); return v0;
        }
        if (t >= tlast) {
            float vl = 0.0f; curve_rd_f32(values, count - 1, &vl); return vl;
        }
    }

    /* Find the first key at or above t, retaining a valid previous key. */
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


    if (mode1 != 0)
        return v_prev;

    /* Equal key times return zero, matching the engine. */
    double dcur = (double)t_cur, dprev = (double)t_prev;
    if (dcur == dprev)
        return 0.0f;

    double frac = ((double)t - dprev) / (dcur - dprev);

    /* The linear path flushes both small endpoint values before interpolation. */
    double dv_cur  = (fabsf(v_cur)  <= (float)B2_ALGO_CURVE_FZ_THRESH) ? 0.0 : (double)v_cur;
    double dv_prev = (fabsf(v_prev) <= (float)B2_ALGO_CURVE_FZ_THRESH) ? 0.0 : (double)v_prev;

    double res = (1.0 - frac) * dv_prev + frac * dv_cur;
    return (float)res;
}

/* Runtime toggle. */


#define ALGO_SIG_MATMUL  "AlgoMatMul"
#define ALGO_SIG_INVERSE "AlgoInverse"
#define ALGO_SIG_PACK    "AlgoPackRGBA"
#define ALGO_SIG_CURVE   "AlgoCurveEval"

/* These hooks replace whole functions and never call their trampolines.
 * The 14-byte jump may cover relative instructions because none are replayed. */
#define ALGO_STOLEN 14

static const uint8_t *g_algo_module_base = NULL;
static volatile LONG  g_algo_installed   = 0;
static int            g_algo_on          = 0;


static void *g_tramp_matmul  = NULL;
static void *g_tramp_inverse = NULL;
static void *g_tramp_pack    = NULL;
static void *g_tramp_curve   = NULL;

/* Return a named signature result; the install gate checks its resolution status. */
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

/* Typed install targets must match each engine function ABI. */
typedef void     (*matmul_fn) (const float *, const float *, float *);
typedef int      (*inverse_fn)(const float *, float *);
typedef uint32_t (*pack_fn)   (const float *);
typedef float    (*curve_fn)  (const void *, float, uint8_t);


static void *algo_prepare_one(const char *name, void *hook)
{
    sig_result r;
    if (!algo_resolve_sig(name, &r)) {
        char line[128];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: snaphak_algo %s not in the signature DB -- cannot install", name);
        backend_log(line);
        return NULL;
    }
    void *tr = sh_prepare_detour_sig(&r, hook, ALGO_STOLEN);
    if (!tr) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: snaphak_algo %s detour refused/failed (status=%d)", name, (int)r.status);
        backend_log(line);
    }
    return tr;
}


static void algo_uninstall_all(void)
{
    if (g_tramp_curve && sh_uninstall_detour(g_tramp_curve)) g_tramp_curve = NULL;
    if (g_tramp_pack && sh_uninstall_detour(g_tramp_pack)) g_tramp_pack = NULL;
    if (g_tramp_inverse && sh_uninstall_detour(g_tramp_inverse)) g_tramp_inverse = NULL;
    if (g_tramp_matmul && sh_uninstall_detour(g_tramp_matmul)) g_tramp_matmul = NULL;
    g_algo_on = g_tramp_curve || g_tramp_pack || g_tramp_inverse || g_tramp_matmul;
}

/* Enable all four overrides together; roll back a partial installation. */
void h_cs_dontuse(struct idCmdArgs *a)
{
    (void)a;
    if (!g_algo_on) {

        g_tramp_matmul  = algo_prepare_one(ALGO_SIG_MATMUL,  (void *)(matmul_fn) sh_algo_matmul);
        g_tramp_inverse = algo_prepare_one(ALGO_SIG_INVERSE, (void *)(inverse_fn)sh_algo_inverse);
        g_tramp_pack    = algo_prepare_one(ALGO_SIG_PACK,    (void *)(pack_fn)   sh_algo_packrgba);
        g_tramp_curve   = algo_prepare_one(ALGO_SIG_CURVE,   (void *)(curve_fn)  sh_algo_curveeval);

        if (!g_tramp_matmul || !g_tramp_inverse || !g_tramp_pack || !g_tramp_curve ||
            sh_commit_detour(g_tramp_matmul) != B2_PATCH_OK ||
            sh_commit_detour(g_tramp_inverse) != B2_PATCH_OK ||
            sh_commit_detour(g_tramp_pack) != B2_PATCH_OK ||
            sh_commit_detour(g_tramp_curve) != B2_PATCH_OK) {
            algo_uninstall_all();
            if (g_algo_on) {
                sh_printf("cs_dontuse: install failed and rollback is incomplete; run cs_dontuse again to retry restoration.\n");
                backend_log("B2: snaphak_algo rollback incomplete; trampoline ownership retained");
                return;
            }
            sh_printf("cs_dontuse: snaphak_algo override install FAILED -- rolled back, overrides OFF.\n");
            backend_log("B2: snaphak_algo cs_dontuse ON aborted (a hook refused) -- rolled back");
            return;
        }
        g_algo_on = 1;
        sh_printf("snaphak_algo overrides ON (4 ops, f64; color-pack bit-exact).\n");
        backend_log("B2: snaphak_algo overrides ON (matmul/inverse/curveEval f64, color-pack bit-exact)");
    } else {

        algo_uninstall_all();
        if (g_algo_on) {
            sh_printf("snaphak_algo restoration incomplete; run cs_dontuse again to retry.\n");
            backend_log("B2: snaphak_algo restore failed; trampoline ownership retained");
            return;
        }
        sh_printf("snaphak_algo overrides OFF (engine math restored).\n");
        backend_log("B2: snaphak_algo overrides OFF (4 detours uninstalled)");
    }
}


void h_alginfo(struct idCmdArgs *a)
{
    (void)a;
    sh_printf("snaphak_algo: clone reimpl present -- matmul/inverse/curveEval f64 (more-precise than "
              "engine f32), color-pack bit-exact; NOT bit-identical to OG x87-80-bit "
              "(currently %s; off-by-default; see divergence note).\n",
              !g_algo_on ? "OFF" :
              (sh_detour_is_installed(g_tramp_matmul) && sh_detour_is_installed(g_tramp_inverse) &&
               sh_detour_is_installed(g_tramp_pack) && sh_detour_is_installed(g_tramp_curve))
                  ? "ON" : "RESTORE PENDING");
}

void sh_algo_install(const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_algo_installed, 1, 0) != 0) return;
    g_algo_module_base = module_base;
    /* Commands are registered separately; startup leaves all overrides disabled. */
    backend_log("B2: snaphak_algo ready (cs_dontuse off-by-default; 4 ops f64 + color-pack bit-exact)");
}

/* In-process math checks. */


static int approx_eq(float x, float y, float eps) { return fabsf(x - y) <= eps; }

int sh_algo_selftest(void)
{
    char line[224];
    char whybuf[160];
    const char *why = "not run";
    int ok = 0;

    /* Identity and nontrivial matrix products. */
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
        /* Row 0 is B row 0 + twice B row 1: [11,14,17,20]. */
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

    /* Inverse round trip and singular-output preservation. */
    {

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
        /* A zero row makes this singular; out must remain untouched. */
        const float S[16] = {
            1.0f, 2.0f, 3.0f, 4.0f,
            0.0f, 0.0f, 0.0f, 0.0f,
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

    /* Verify round-half-up packing and clamping. */
    {

        const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (sh_algo_packrgba(black) != 0x00000000u) { why = "packRGBA black != 0"; goto done; }
        if (sh_algo_packrgba(white) != 0xFFFFFFFFu) { why = "packRGBA white != 0xFFFFFFFF"; goto done; }
        /* Half intensity rounds to 128; the unhooked engine would truncate to 127. */
        const float half[4] = { 0.5f, 0.0f, 0.0f, 1.0f };
        uint32_t got = sh_algo_packrgba(half);
        if (got != 0xFF000080u) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE,
                "packRGBA 0.5 round-half-up wrong: got 0x%08x expected 0xFF000080", got);
            why = whybuf; goto done;
        }

        const float oor[4] = { 2.0f, -1.0f, 2.0f, -1.0f };
        if (sh_algo_packrgba(oor) != 0x00FF00FFu) { why = "packRGBA clamp wrong"; goto done; }
    }

    /* Two-key linear curve: t=2.5 between (0,100) and (10,200) yields 125. */
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

    /* A nonlinear four-key curve distinguishes Catmull-Rom from linear interpolation.
 * At t=1.5, weights [-1,9,9,-1]/16 yield 3.375 rather than the linear 4.5. */
    {
        uint8_t buf[0x220];
        memset(buf, 0, sizeof buf);
        buf[CURVE_OFF_MODE2] = 1;
        *(int *)(buf + CURVE_OFF_COUNT)    = 4;
        *(int *)(buf + CURVE_OFF_EDGEMODE) = 0;
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

        /* Extrapolation gives v[-1]=-1; the edge spline result is 0.125. */
        float ve = sh_algo_curveeval(buf, 0.5f, 0);
        if (!approx_eq(ve, 0.125f, 1e-3f)) {
            _snprintf_s(whybuf, sizeof whybuf, _TRUNCATE,
                "curveEval CR edge(extrap i-2<0) wrong: got %g expected 0.125", ve);
            why = whybuf; goto done;
        }

        /* Clamping gives v[-1]=0; only that control point changes, yielding 0.0625. */
        *(int *)(buf + CURVE_OFF_EDGEMODE) = 1;
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
