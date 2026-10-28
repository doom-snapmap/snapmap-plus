/* Prefab Create grab-frame guard: hit detection, the synthesised frame, and
 * that a prefab stored against it pastes as an exact rigid copy while the
 * engine's missed frame does not. Engine-free; drives src/backend/prefab_grab.c. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "prefab_grab.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "[FAIL] %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static const float IDENTITY[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

static void box_make(sh_prefab_grab_box *b, float ox, float oy, float oz, const float axis[9],
                     float x0, float y0, float z0, float x1, float y1, float z1)
{
    b->origin[0] = ox; b->origin[1] = oy; b->origin[2] = oz;
    memcpy(b->axis, axis, sizeof b->axis);
    b->bmin[0] = x0; b->bmin[1] = y0; b->bmin[2] = z0;
    b->bmax[0] = x1; b->bmax[1] = y1; b->bmax[2] = z1;
}

/* The helper's own output for a given line: midpoint of the line-vs-OBB
 * clip, hit or not (FUN_14028a390 starts at +-1e30 and the caller ignores its
 * return). Only the in-plane slabs are replicated, which is enough to build a
 * native hit or a native miss for the tests below. */
static void native_frame(const float view[3], const float fwd[3], const sh_prefab_grab_box *b,
                         float frame[12], float *distance)
{
    float lo = -1e30f, hi = 1e30f, c[3], h[3];
    int i;
    sh_prefab_grab_box_centre(b, c);
    for (i = 0; i < 3; i++) h[i] = (b->bmax[i] - b->bmin[i]) * 0.5f;
    for (i = 0; i < 3; i++) {
        const float *a = b->axis + 3 * i;
        float p = fwd[0] * a[0] + fwd[1] * a[1] + fwd[2] * a[2];
        float q = (view[0] - c[0]) * a[0] + (view[1] - c[1]) * a[1] + (view[2] - c[2]) * a[2];
        if (p > 0) { float t0 = (-h[i] - q) / p, t1 = (h[i] - q) / p; if (t0 > lo) lo = t0; if (t1 < hi) hi = t1; }
        else if (p < 0) { float t0 = (h[i] - q) / p, t1 = (-h[i] - q) / p; if (t0 > lo) lo = t0; if (t1 < hi) hi = t1; }
    }
    *distance = (lo + hi) * 0.5f;
    for (i = 0; i < 3; i++) frame[i] = view[i] + fwd[i] * *distance;
    memcpy(frame + 3, b->axis, 9 * sizeof(float));
}

/* Row-vector rigid transforms as the engine composes them (FUN_1411afe40,
 * FUN_1411af890): x' = x A + o. */
typedef struct rigid { float o[3]; float a[9]; } rigid;

static void rigid_inverse(const rigid *g, rigid *out)
{
    int r, c;
    for (r = 0; r < 3; r++) for (c = 0; c < 3; c++) out->a[r * 3 + c] = g->a[c * 3 + r];
    for (c = 0; c < 3; c++)
        out->o[c] = -(g->o[0] * out->a[c] + g->o[1] * out->a[3 + c] + g->o[2] * out->a[6 + c]);
}

static void rigid_point(const rigid *t, const float p[3], float out[3])
{
    int c;
    float r[3];
    for (c = 0; c < 3; c++) r[c] = p[0] * t->a[c] + p[1] * t->a[3 + c] + p[2] * t->a[6 + c] + t->o[c];
    memcpy(out, r, sizeof r);
}

static void rigid_from_frame(const float frame[12], rigid *t)
{
    memcpy(t->o, frame, 3 * sizeof(float));
    memcpy(t->a, frame + 3, 9 * sizeof(float));
}

/* Largest pairwise-distance error between a layout and its stored-then-pasted copy. */
static float paste_error(const float frame[12], float distance, const float view[3], const float fwd[3],
                         const float world[][3], int n)
{
    rigid g, inv, paste;
    float local[8][3], out[8][3], worst = 0.0f;
    int i, j, k;
    rigid_from_frame(frame, &g);
    rigid_inverse(&g, &inv);
    for (i = 0; i < n; i++) rigid_point(&inv, world[i], local[i]);   /* PrefabPopulate */
    memcpy(paste.a, frame + 3, sizeof paste.a);                        /* grabAxis x Yaw(0) */
    for (k = 0; k < 3; k++) paste.o[k] = view[k] + fwd[k] * distance;  /* PasteInstantiate */
    for (i = 0; i < n; i++) rigid_point(&paste, local[i], out[i]);
    for (i = 0; i < n; i++) for (j = i + 1; j < n; j++)
        for (k = 0; k < 3; k++) {
            float e = fabsf((out[i][k] - out[j][k]) - (world[i][k] - world[j][k]));
            if (!(e <= worst)) worst = e;   /* NaN counts as the worst */
        }
    return worst;
}

static void test_forward(void)
{
    float a0[3] = { 0, 0, 0 }, a1[3] = { 0, 90, 0 }, a2[3] = { 30, 0, 0 }, f[3];
    sh_prefab_grab_forward(a0, f);
    CHECK(near(f[0], 1, 1e-6f) && near(f[1], 0, 1e-6f) && near(f[2], 0, 1e-6f), "yaw 0 looks along +X");
    sh_prefab_grab_forward(a1, f);
    CHECK(near(f[0], 0, 1e-6f) && near(f[1], 1, 1e-6f), "yaw 90 looks along +Y");
    sh_prefab_grab_forward(a2, f);
    CHECK(near(f[2], -0.5f, 1e-6f) && near(f[0], 0.8660254f, 1e-6f), "positive pitch looks down");
}

static void test_centred_box(void)
{
    float size[3] = { 16, 128, 64 }, lo[3], hi[3];
    sh_prefab_grab_centred_box(size, lo, hi);
    CHECK(lo[0] == -8 && hi[0] == 8 && lo[1] == -64 && hi[1] == 64 && lo[2] == -32 && hi[2] == 32,
          "decl+0x3ce bit 8 box is centred on the origin");
}

static void test_hit_kept(void)
{
    sh_prefab_grab_box b;
    sh_prefab_grab_result r;
    float view[3] = { 0, 0, 8 }, fwd[3] = { 1, 0, 0 }, frame[12], d, before[12];
    box_make(&b, 100, 0, 0, IDENTITY, -8, -64, 0, 8, 64, 64);
    native_frame(view, fwd, &b, frame, &d);
    memcpy(before, frame, sizeof before);
    CHECK(near(d, 100, 1e-3f), "replicated helper hits the box at its centre depth");
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_OK, "a hit is kept");
    CHECK(!memcmp(before, frame, sizeof before) && near(d, 100, 1e-3f), "a kept frame is untouched");
    CHECK(r.outside == 0.0f, "a hit grab point lies inside the hovered box");
}

static void test_huge_miss_corrected(void)
{
    /* M114's live value on the 2018 build: grabDistance -2.95e10. */
    sh_prefab_grab_box b;
    sh_prefab_grab_result r;
    float view[3] = { -2552.5f, 4053.8f, 238.3f }, fwd[3] = { 1, 0, 0 }, frame[12], d = -2.95e10f;
    int i;
    box_make(&b, -2000, 4300, 200, IDENTITY, -3, -12, 0, 3, 12, 24);
    for (i = 0; i < 3; i++) frame[i] = view[i] + fwd[i] * d;
    memcpy(frame + 3, b.axis, sizeof b.axis);
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_CORRECTED, "an absurd distance is corrected");
    CHECK(near(frame[0], -2000, 1e-3f) && near(frame[1], 4300, 1e-3f) && near(frame[2], 212, 1e-3f),
          "the corrected grab point is the hovered box centre");
    CHECK(near(d, 552.5f, 1e-2f), "the corrected distance is the centre's depth along the view");
    CHECK(r.native_distance == -2.95e10f && r.status == SH_PREFAB_GRAB_CORRECTED, "the report keeps the native value");
    CHECK(!memcmp(frame + 3, IDENTITY, sizeof IDENTITY), "the hovered body's axis is kept");
}

static void test_moderate_miss_corrected(void)
{
    /* The line passes beside the box: finite, bounded, and still a miss. */
    sh_prefab_grab_box b;
    sh_prefab_grab_result r;
    float view[3] = { 0, 0, 8 }, fwd[3] = { 1, 0, 0 }, frame[12], d;
    box_make(&b, 300, 400, 0, IDENTITY, -8, -8, 0, 8, 8, 16);
    native_frame(view, fwd, &b, frame, &d);
    CHECK(fabsf(d) < SH_PREFAB_GRAB_MAX_DISTANCE, "this miss is within the distance bound");
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_CORRECTED,
          "a bounded grab point outside the hovered box is still corrected");
    CHECK(r.outside > 100.0f, "the report says how far outside the box the native point was");
    CHECK(near(d, 300, 1e-3f) && near(frame[1], 400, 1e-3f), "the paste lands at the box centre's depth");
}

static void test_rotated_and_mirrored(void)
{
    /* A 512x512x16 ramp tilted 30 degrees about X, then the same mirrored. */
    const float c = 0.8660254f, s = 0.5f;
    float tilt[9] = { 1, 0, 0, 0, c, s, 0, -s, c };
    float mirror[9] = { -1, 0, 0, 0, c, s, 0, -s, c };
    float view[3] = { 0, -900, 300 }, fwd[3], ang[3] = { 15, 90, 0 }, frame[12], d, ctr[3];
    sh_prefab_grab_box b;
    sh_prefab_grab_result r;
    int i;
    sh_prefab_grab_forward(ang, fwd);
    box_make(&b, 0, 0, 0, tilt, -256, -256, 0, 256, 256, 16);
    sh_prefab_grab_box_centre(&b, ctr);
    CHECK(near(ctr[0], 0, 1e-4f) && near(ctr[1], -4, 1e-4f) && near(ctr[2], 8 * c, 1e-4f),
          "the box centre follows the tilted local Z axis");
    for (i = 0; i < 3; i++) view[i] = ctr[i] - fwd[i] * 500.0f;   /* view centre on the ramp */
    native_frame(view, fwd, &b, frame, &d);
    CHECK(near(d, 500, 0.05f), "the view-centre line crosses the ramp at its centre");
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_OK, "a hit on a tilted box is kept");
    box_make(&b, 0, 0, 0, mirror, -256, -256, 0, 256, 256, 16);
    native_frame(view, fwd, &b, frame, &d);
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_OK,
          "a hit on a mirrored (det -1) box is kept");
    d = 9.0e9f; frame[0] = 1e9f;
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_CORRECTED &&
          !memcmp(frame + 3, mirror, sizeof mirror), "a corrected mirrored frame keeps the mirrored axis");
}

static void test_behind_camera(void)
{
    sh_prefab_grab_box b;
    sh_prefab_grab_result r;
    float view[3] = { 0, 0, 0 }, fwd[3] = { 1, 0, 0 }, frame[12] = { 0 }, d = 1e12f;
    box_make(&b, -200, 0, 0, IDENTITY, -8, -8, 0, 8, 8, 16);
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_CORRECTED, "corrected");
    CHECK(d == SH_PREFAB_GRAB_MIN_DISTANCE, "a box behind the camera pastes just in front of it");
}

static void test_unmeasurable(void)
{
    sh_prefab_grab_box b;
    sh_prefab_grab_result r;
    float view[3] = { 0, 0, 0 }, fwd[3] = { 1, 0, 0 }, frame[12], d = 250;
    memset(frame, 0, sizeof frame);
    memcpy(frame + 3, IDENTITY, sizeof IDENTITY);
    frame[0] = 250;
    box_make(&b, NAN, 0, 0, IDENTITY, -8, -8, 0, 8, 8, 16);
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_OK,
          "an unreadable body keeps a bounded native frame");
    CHECK(sh_prefab_grab_guard(view, fwd, NULL, frame, &d, &r) == SH_PREFAB_GRAB_OK, "so does a missing body");
    d = -2.95e10f;
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_REFUSED,
          "an absurd frame with nothing to rebuild it from is refused");
    d = NAN;
    CHECK(sh_prefab_grab_guard(view, fwd, NULL, frame, &d, &r) == SH_PREFAB_GRAB_REFUSED, "a NaN frame is refused");
    box_make(&b, 0, 0, 0, IDENTITY, 8, -8, 0, -8, 8, 16);   /* inverted bounds */
    d = 1e20f;
    CHECK(sh_prefab_grab_guard(view, fwd, &b, frame, &d, &r) == SH_PREFAB_GRAB_REFUSED, "inverted bounds are not a box");
}

static void test_paste_is_exact(void)
{
    /* Four boxes like the live fixture, hovered box first. */
    static const float world[4][3] = {
        { -2000.0f, 4300.0f, 200.0f }, { -1744.25f, 4410.5f, 216.0f },
        { -2130.75f, 4012.0f, 192.0f }, { -1888.0f, 4555.5f, 264.0f } };
    sh_prefab_grab_box b;
    sh_prefab_grab_result r;
    float view[3] = { -2552.5f, 4053.8f, 238.3f }, fwd[3] = { 1, 0, 0 };
    float native[12], fixed[12], d_native = -2.95e10f, d_fixed;
    float e_native, e_fixed;
    int i;
    box_make(&b, world[0][0], world[0][1], world[0][2], IDENTITY, -3, -12, 0, 3, 12, 24);
    for (i = 0; i < 3; i++) native[i] = view[i] + fwd[i] * d_native;
    memcpy(native + 3, IDENTITY, sizeof IDENTITY);
    memcpy(fixed, native, sizeof fixed);
    d_fixed = d_native;
    sh_prefab_grab_guard(view, fwd, &b, fixed, &d_fixed, &r);
    e_native = paste_error(native, d_native, view, fwd, world, 4);
    e_fixed = paste_error(fixed, d_fixed, view, fwd, world, 4);
    CHECK(!(e_native < 256.0f), "the engine's missed frame destroys the relative layout");
    CHECK(e_fixed < 0.01f, "the corrected frame stores and pastes the layout exactly");
    printf("prefab_grab_test: pairwise error native %g, corrected %g\n", (double)e_native, (double)e_fixed);
}

static void test_json_check(void)
{
    static const char good[] =
        "{\"cameraYaw\":12.5,\"entities\":[{\"entityDef\":{\"state\":{\"edit\":{\"spawnPosition\":{\"x\":-206.5,\"z\":332.0}}}}}],"
        "\"grabAxis\":{\"mat\":[]},\"grabDistance\":85.58,\"~type\":\"idSnapEntityPrefab\"}";
    static const char pretty[] =
        "{\n   \"grabDistance\" : 194.06,\n   \"entities\" : [ { \"spawnPosition\" : { \"x\" : 1.0, \"~type\" : \"idVec3\" } } ]\n}";
    static const char far_dist[] = "{\"grabDistance\":-2.95e+10,\"entities\":[]}";
    static const char far_pos[] =
        "{\"grabDistance\":12.0,\"entities\":[{\"spawnPosition\":{\"x\":2.95e10,\"y\":3.0}}]}";
    static const char words[] = "{\"grabDistance\":1.#INF,\"entities\":[]}";
    static const char none[] = "{\"entities\":[]}";
    double d = 0;
    CHECK(sh_prefab_grab_json_check(good, sizeof good - 1, &d) == 1 && near((float)d, 85.58f, 1e-3f), "a normal file passes");
    CHECK(sh_prefab_grab_json_check(pretty, sizeof pretty - 1, &d) == 1, "pretty JSON parses");
    CHECK(sh_prefab_grab_json_check(far_dist, sizeof far_dist - 1, &d) == 0, "an absurd grabDistance is caught");
    CHECK(sh_prefab_grab_json_check(far_pos, sizeof far_pos - 1, &d) == 0, "absurd stored positions are caught");
    CHECK(sh_prefab_grab_json_check(words, sizeof words - 1, &d) == 0, "a non-finite word is caught");
    CHECK(sh_prefab_grab_json_check(none, sizeof none - 1, &d) == -1, "a file without grabDistance is unknown");
}

int main(void)
{
    test_forward();
    test_centred_box();
    test_hit_kept();
    test_huge_miss_corrected();
    test_moderate_miss_corrected();
    test_rotated_and_mirrored();
    test_behind_camera();
    test_unmeasurable();
    test_paste_is_exact();
    test_json_check();
    if (failures) { fprintf(stderr, "prefab_grab_test: %d failure(s)\n", failures); return 1; }
    puts("prefab_grab_test: OK");
    return 0;
}
