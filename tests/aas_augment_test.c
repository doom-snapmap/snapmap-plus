/* Tests AAS augmentation, BSP splicing, link selection and refusals using synthetic
 * geometry. A 16-unit rise admits walk links; a 128-unit rise without a traversal
 * table remains a reported island. See README.md for fixture provenance. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/backend/aas_edit.h"
#include "../src/backend/aas_augment.h"
#include "../src/backend/nav_traversal.h"
#include "../src/backend/navmesh.h"
#include "../src/backend/nav_geometry.h"

static int g_checks = 0, g_fail = 0;

/* navmesh.c supplies the structural validator; its service dependencies are stubbed. */
void backend_log(const char *message) { (void)message; }

int sh_config_get_bool(const char *key, int *out_value, unsigned int *out_flags)
{
    (void)key;
    if (out_flags) *out_flags = 0;
    if (!out_value) return 0;
    *out_value = 1;
    return 1;
}

#define CHECK(cond) do {                                                      \
    g_checks++;                                                               \
    if (!(cond)) {                                                            \
        g_fail++;                                                             \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                         \
} while (0)

#define CHECK_MSG(cond, msg) do {                                             \
    g_checks++;                                                               \
    if (!(cond)) {                                                            \
        g_fail++;                                                             \
        printf("  FAIL %s:%d  %s -- %s\n", __FILE__, __LINE__, #cond, msg);   \
    }                                                                         \
} while (0)

/* ==================================================================== */
/* a synthetic module: one flat floor slab, findable through the BSP     */
/* ==================================================================== */

static const unsigned REC[22] = {
    16, 12, 12, 4, 40, 44, 16, 12, 4, 16, 1, 132, 128, 128, 128, 56, 4, 4, 60, 24, 24, 12
};

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v;
}
static void putf(unsigned char *p, float f)
{
    unsigned u; memcpy(&u, &f, 4); put32(p, u);
}

#define SET_WORDS 208
#define SETW(n)   (30 + SET_WORDS + (n) * 4)

/* A floor slab spanning +/-2000 at z = 0, in a BSP band -9.6 < z < 200 so a
 * point standing on the slab resolves to it. Three nodes: 0 is the engine's
 * dummy, 1 is the root, 2 is the ceiling test. */
static unsigned char *build_module(size_t *out_len)
{
    unsigned counts[22];
    size_t total = 394, off;
    unsigned i;
    unsigned char *p;
    size_t o[22];

    memset(counts, 0, sizeof counts);
    counts[SH_AAS_L_PLANES] = 2;
    counts[SH_AAS_L_VERTICES] = 4;
    counts[SH_AAS_L_EDGES] = 4;
    counts[SH_AAS_L_EDGEINDEX] = 4;
    counts[SH_AAS_L_AREAS] = 2;
    counts[SH_AAS_L_NODES] = 3;
    counts[SH_AAS_L_CLUSTERS] = 1;
    counts[SH_AAS_L_OBSTACLEPVS] = 2;
    counts[SH_AAS_L_TREES] = 1;
    counts[SH_AAS_L_AREABOUNDS] = 2;

    for (i = 0; i < 22; i++) total += 4 + (size_t)counts[i] * REC[i];
    p = (unsigned char *)calloc(1, total);
    if (!p) return NULL;

    memcpy(p, "2SAA", 4);
    p[4] = 3;
    p[5] = 29;

    /* settings: agent box 48x48x80 (radius 24), maxStepHeight 18,
     * maxFallHeight 0, minFloorCos 0.7 -- the monster-class shape. */
    putf(p + SETW(0), -24.0f); putf(p + SETW(1), -24.0f); putf(p + SETW(2), 0.0f);
    putf(p + SETW(3),  24.0f); putf(p + SETW(4),  24.0f); putf(p + SETW(5), 80.0f);
    putf(p + SETW(12), 18.0f);          /* maxStepHeight */
    putf(p + SETW(15), 0.0f);           /* maxFallHeight */
    /* minFloorCos = 0.7 admits slopes up to about 45.57 degrees.
     * Missing or invalid settings must refuse augmentation. */
    putf(p + SETW(16), 0.7f);
    put32(p + SETW(36), 100);           /* tt_startWalkOffLedge */

    off = 394;
    for (i = 0; i < 22; i++) {
        put32(p + off, counts[i]);
        off += 4;
        o[i] = off;
        off += (size_t)counts[i] * REC[i];
    }

    /* plane 0: (0 0 1) d=9.6  -> front is z > -9.6
     * plane 1: (0 0 -1) d=200 -> front is z < 200 */
    putf(p + o[SH_AAS_L_PLANES] + 8, 1.0f);
    putf(p + o[SH_AAS_L_PLANES] + 12, 9.6f);
    putf(p + o[SH_AAS_L_PLANES] + 16 + 8, -1.0f);
    putf(p + o[SH_AAS_L_PLANES] + 16 + 12, 200.0f);

    for (i = 0; i < 4; i++) {
        unsigned char *e = p + o[SH_AAS_L_EDGES] + i * 12;
        put32(e, i);
        put32(e + 4, (i + 1) % 4);
        put32(p + o[SH_AAS_L_EDGEINDEX] + i * 4, i);
    }

    /* area 0 is the dummy; area 1 is the floor. */
    put32(p + o[SH_AAS_L_AREAS] + 0x14, 0xFFFFFFFFu);
    put32(p + o[SH_AAS_L_AREAS] + 0x18, 0xFFFFFFFFu);
    {
        unsigned char *a1 = p + o[SH_AAS_L_AREAS] + 44;
        put32(a1 + 0x00, 1);            /* flags: floor */
        put16(a1 + 0x06, 4);            /* numEdges */
        put32(a1 + 0x08, 0);            /* firstEdgeIndex */
        put16(a1 + 0x0C, 0);            /* cluster 0 */
        put32(a1 + 0x10, 0);            /* firstObstaclePVS */
        put32(a1 + 0x14, 0xFFFFFFFFu);
        put32(a1 + 0x18, 0xFFFFFFFFu);
    }
    /* areaBounds: area 1 is flat at z = 0, +/-2000 in x and y. */
    {
        unsigned char *b = p + o[SH_AAS_L_AREABOUNDS] + 12;
        put16(b + 0, (unsigned)(short)-2000);
        put16(b + 2, (unsigned)(short)-2000);
        put16(b + 4, 0);
        put16(b + 6, 2000);
        put16(b + 8, 2000);
        put16(b + 10, 0);
    }
    /* the BSP: root is node 1. */
    {
        unsigned char *n = p + o[SH_AAS_L_NODES];
        put32(n + 16 + 0, 0);                       /* node 1 uses plane 0 */
        put32(n + 16 + 8, 2);                       /* front -> node 2 */
        put32(n + 16 + 12, 0);                      /* back  -> void */
        put32(n + 32 + 0, 1);                       /* node 2 uses plane 1 */
        put32(n + 32 + 8, 0xFFFFFFFFu);             /* front -> area 1 */
        put32(n + 32 + 12, 0);                      /* back  -> void */
    }
    /* Tree records contain three floats followed by three ints. The root is at
     * +12 and the area count at +20; independent offsets catch layout drift. */
    putf(p + o[SH_AAS_L_TREES] + 8, 1.0f);          /* ( 0 0 1 ) */
    put32(p + o[SH_AAS_L_TREES] + 12, 1);           /* a = root node index */
    put32(p + o[SH_AAS_L_TREES] + 16, 1);           /* b */
    put32(p + o[SH_AAS_L_TREES] + 20, 2);           /* c = area count */

    *out_len = total;
    return p;
}

static sh_aas *load_module(void)
{
    size_t len = 0;
    char err[192];
    unsigned char *bytes = build_module(&len);
    sh_aas *a;
    if (!bytes) return NULL;
    err[0] = 0;
    a = sh_aas_parse(bytes, len, err, sizeof err);
    if (!a) printf("  (fixture did not parse: %s)\n", err);
    free(bytes);
    return a;
}

static void mkplat(sh_aug_platform *p, float x0, float y0, float x1, float y1,
                   float z, const char *name)
{
    memset(p, 0, sizeof *p);
    /* Clockwise seen from +Z, the winding every Grid Room floor area uses. */
    p->c[0][0] = x0; p->c[0][1] = y1;
    p->c[1][0] = x1; p->c[1][1] = y1;
    p->c[2][0] = x1; p->c[2][1] = y0;
    p->c[3][0] = x0; p->c[3][1] = y0;
    p->c[0][2] = p->c[1][2] = p->c[2][2] = p->c[3][2] = z;
    p->n[2] = 1.0f;
    p->face = 4;                    /* an upright box's top */
    _snprintf_s(p->name, sizeof p->name, _TRUNCATE, "%s", name);
}

/* ==================================================================== */

static void test_fixture_resolves(void)
{
    sh_aas *a = load_module();
    printf("fixture\n");
    CHECK_MSG(a != NULL, "the synthetic module must parse");
    if (!a) return;
    CHECK_MSG(sh_aas_point_area(a, 0.0f, 0.0f, 2.0f) == 1,
              "a point standing on the floor slab resolves to area 1");
    CHECK_MSG(sh_aas_point_area(a, 0.0f, 0.0f, 500.0f) == 0,
              "a point above the band is void");
    /* A nonzero depth confirms the root was read after the direction vector. */
    CHECK_MSG(sh_aas_tree_depth(a) == 2, "the tree root must come from trees[0].a at +12");
    sh_aas_free(a);
}

static void test_island_at_128(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = {0};
    printf("a platform past maxStepHeight is an island\n");
    if (!a) { CHECK(0); return; }
    memset(&o, 0, sizeof o);
    o.fall = SH_AUG_FALL_AUTO;
    o.inset = 1;
    /* Disable traversal explicitly: the process-wide table cache would make AUTO
     * depend on which earlier test loaded a table. */
    o.traversal = SH_AUG_TRAVERSAL_NEVER;
    mkplat(&p, -500.0f, -500.0f, 500.0f, 500.0f, 128.0f, "roof");

    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK_MSG(rep.platforms[0].emitted == 1, "the platform must be emitted");
    CHECK_MSG(rep.areas_after == rep.areas_before + 1, "exactly one area added");
    CHECK_MSG(rep.platforms[0].carrier == 1, "the floor slab is the carrier");
    CHECK_MSG(rep.platforms[0].leaf_slots_carved > 0,
              "the area must be spliced into the BSP or it is unfindable");
    CHECK_MSG(rep.platforms[0].island == 1,
              "128 is past maxStepHeight and no climb was offered");
    CHECK_MSG(rep.depth_exceeded == 0, "the tree must stay inside the loader limit");

    /* The whole point of the splice: the engine can now find the area. */
    CHECK_MSG(sh_aas_point_area(a, 0.0f, 0.0f, 130.0f) == rep.platforms[0].area,
              "a point standing on the platform resolves to the new area");

    /* Independent literal floor flags catch format drift that structural
     * validation alone cannot detect. */
    {
        const unsigned char *ar = sh_aas_rec_const(a, SH_AAS_L_AREAS,
                                                   (unsigned)rep.platforms[0].area);
        unsigned i, n = sh_aas_count(a, SH_AAS_L_EDGES);
        int boundary = 0;
        CHECK(ar != NULL);
        if (ar) {
            CHECK_MSG(sh_aas_get_u32(ar, 0) == 0x8u, "area.flags must be 0x8");
            CHECK_MSG(sh_aas_get_u16(ar, 4) == 0x000Au, "area.travel_flags must be 0x000A");
        }
        for (i = 0; i < n; i++) {
            const unsigned char *e = sh_aas_rec_const(a, SH_AAS_L_EDGES, i);
            if (e && sh_aas_get_i32(e, 8) == 0x0C01) boundary++;
        }
        CHECK_MSG(boundary >= 4, "the platform's own edges carry the 0x0C01 boundary flag");
    }
    sh_aas_free(a);
}

static void test_step_regime_at_16(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = {0};
    printf("a platform inside maxStepHeight is walked, not climbed\n");
    if (!a) { CHECK(0); return; }
    memset(&o, 0, sizeof o);
    o.fall = SH_AUG_FALL_AUTO;
    o.inset = 1;
    mkplat(&p, -500.0f, -500.0f, 500.0f, 500.0f, 16.0f, "kerb");

    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK(rep.platforms[0].emitted == 1);
    CHECK_MSG(rep.platforms[0].island == 0,
              "16 is inside maxStepHeight so plain walk links must join it");
    CHECK_MSG(rep.reach_after > rep.reach_before,
              "reachabilities must have been added");
    CHECK_MSG(rep.platforms[0].links > 0, "the area must be linked");
    sh_aas_free(a);
}

static void test_refusals(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[3];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    printf("refusals are reported, never fudged\n");
    if (!a) { CHECK(0); return; }
    memset(&o, 0, sizeof o);
    o.inset = 1;

    /* Too small once inset by the 24-unit agent radius: 60x60 leaves 12x12,
     * under the 48x48 footprint. */
    mkplat(&p[0], -30.0f, -30.0f, 30.0f, 30.0f, 100.0f, "pebble");
    /* Fine. */
    mkplat(&p[1], -500.0f, -500.0f, 500.0f, 500.0f, 100.0f, "roof");
    /* Directly under `roof` with only 20 units of clearance, less than the
     * 80-unit agent height. */
    mkplat(&p[2], -400.0f, -400.0f, 400.0f, 400.0f, 80.0f, "crawlspace");

    CHECK(sh_aas_augment(a, p, 3, &o, &rep) == 1);
    CHECK_MSG(rep.platforms[0].emitted == 0, "the too-small platform is refused");
    CHECK_MSG(rep.platforms[0].reason[0] != 0, "and the refusal is explained");
    CHECK_MSG(rep.platforms[1].emitted == 1, "one bad box must not cost the others");
    CHECK_MSG(rep.platforms[2].emitted == 0, "the low-headroom platform is refused");
    CHECK_MSG(rep.platforms[2].reason[0] != 0, "and that refusal is explained too");
    CHECK_MSG(rep.areas_after == rep.areas_before + 1, "only the good one landed");
    sh_aas_free(a);
}

static void test_zero_platforms_is_a_no_op(void)
{
    sh_aas *a = load_module();
    sh_aug_report rep;
    sh_aug_opts o = {0};
    unsigned char *before, *after;
    size_t blen = 0, alen = 0;
    printf("augmenting with nothing changes nothing\n");
    if (!a) { CHECK(0); return; }
    memset(&o, 0, sizeof o);
    o.inset = 1;

    before = sh_aas_write(a, &blen);
    CHECK(before != NULL);
    CHECK(sh_aas_augment(a, NULL, 0, &o, &rep) == 1);
    after = sh_aas_write(a, &alen);
    CHECK(after != NULL);
    if (before && after) {
        CHECK_MSG(blen == alen && memcmp(before, after, blen) == 0,
                  "a bake with no platforms must be byte-identical");
    }
    if (before) HeapFree(GetProcessHeap(), 0, before);
    if (after) HeapFree(GetProcessHeap(), 0, after);
    sh_aas_free(a);
}

static void test_result_still_validates(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = {0};
    unsigned char *bytes;
    size_t len = 0;
    char err[192];
    printf("the augmented payload passes the same gate the serving path uses\n");
    if (!a) { CHECK(0); return; }
    memset(&o, 0, sizeof o);
    o.inset = 1;
    mkplat(&p, -500.0f, -500.0f, 500.0f, 500.0f, 128.0f, "roof");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);

    bytes = sh_aas_write(a, &len);
    CHECK(bytes != NULL);
    if (bytes) {
        err[0] = 0;
        CHECK_MSG(sh_navmesh_validate_aas(bytes, len, err, sizeof err) == 1,
                  err[0] ? err : "the structural gate refused our own output");
        HeapFree(GetProcessHeap(), 0, bytes);
    }
    sh_aas_free(a);
}

static void test_many_platforms_stay_within_depth(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[24];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int i;
    printf("a cityscape of separated roofs stays inside the loader depth limit\n");
    if (!a) { CHECK(0); return; }
    memset(&o, 0, sizeof o);
    o.inset = 1;
    for (i = 0; i < 24; i++) {
        float x = -1800.0f + (float)(i % 6) * 600.0f;
        float y = -1800.0f + (float)(i / 6) * 600.0f;
        char nm[SH_AUG_NAME_CAP];
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "roof%02d", i);
        mkplat(&p[i], x, y, x + 400.0f, y + 400.0f,
               64.0f + (float)(i % 4) * 32.0f, nm);
    }
    CHECK(sh_aas_augment(a, p, 24, &o, &rep) == 1);
    CHECK_MSG(rep.areas_after > rep.areas_before, "roofs landed");
    CHECK_MSG(rep.depth_exceeded == 0,
              "BSP depth grows logarithmically with separated platforms");
    CHECK(rep.depth_after < (unsigned)SH_AAS_MAX_DEPTH);
    sh_aas_free(a);
}

/* Climb start offsets differ by demon, so test more than one anchor on each
 * ledge. Keep the midpoint first to preserve placement when it already fits. */
static void test_traversal_anchor_pattern(void)
{
    double a[24];
    int n, i, j;
    printf("climb anchors are sampled along the edge, midpoint first\n");

    n = sh_aug_test_trav_anchors(-1000.0, 1000.0, a, 24);
    CHECK_MSG(n >= 2, "a wide edge must offer an alternative to the midpoint");
    CHECK_MSG(a[0] == 0.0, "the midpoint is tried first, so existing links do not move");
    for (i = 0; i < n; i++)
        CHECK_MSG(a[i] >= -1000.0 && a[i] <= 1000.0, "an anchor must stay on the edge");
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            CHECK_MSG(a[i] != a[j], "a repeated anchor is a wasted BSP query");

    /* Reversed spans are the same edge. */
    {
        double b[24];
        int m = sh_aug_test_trav_anchors(1000.0, -1000.0, b, 24);
        CHECK(m == n);
        if (m == n) for (i = 0; i < n; i++) CHECK(a[i] == b[i]);
    }

    /* A degenerate edge still offers the midpoint rather than nothing. */
    n = sh_aug_test_trav_anchors(50.0, 50.0, a, 24);
    CHECK_MSG(n >= 1, "even a zero-length span offers its midpoint");
    CHECK(a[0] == 50.0);

    /* The cap is honoured: this runs per demon per direction per edge. */
    n = sh_aug_test_trav_anchors(-100000.0, 100000.0, a, 4);
    CHECK_MSG(n <= 4, "the anchor list must never overrun the caller buffer");
    CHECK(n >= 1);

    /* Spacing follows the edge's length in world units, not a unit interval:
     * a 300-unit edge gets its ends as well as its middle, and a 2000-unit
     * edge is sampled about every 192 units. */
    n = sh_aug_test_trav_anchors(0.0, 300.0, a, 24);
    CHECK_MSG(n == 3, "a 300-unit edge offers its middle and both inset ends");
    CHECK(a[0] == 150.0);
    n = sh_aug_test_trav_anchors(-1000.0, 1000.0, a, 24);
    CHECK_MSG(n >= 11, "a 2000-unit edge is sampled every 192 units or closer");
    for (i = 2; i < n; i++)
        CHECK_MSG(fabs(a[i]) >= fabs(a[i - 1]), "anchors run outward from the midpoint");
    {
        double s[24];
        int m = n, k;
        memcpy(s, a, sizeof s);
        for (i = 1; i < m; i++) for (k = i; k > 0 && s[k] < s[k - 1]; k--) {
            double t = s[k]; s[k] = s[k - 1]; s[k - 1] = t;
        }
        for (i = 1; i < m; i++)
            CHECK_MSG(s[i] - s[i - 1] <= 192.0 + 1e-6, "no stretch of edge goes unsampled");
    }
}


/* ==================================================================== */
/* oriented geometry                                                     */
/* ==================================================================== */

static int near_d(double a, double b) { double v = a - b; return v < 0.001 && v > -0.001; }
static int near_f(float a, float b) { float v = a - b; return v < 0.001f && v > -0.001f; }

/* A yawed rect: a w-by-h rectangle rotated `deg` about its own centre, level at
 * `z`, wound clockwise seen from +Z. */
static void mkplat_yawed(sh_aug_platform *p, double deg, double w, double h,
                         double z, const char *name)
{
    double r = deg * 3.14159265358979323846 / 180.0;
    double cs = cos(r), sn = sin(r), hw = w / 2.0, hh = h / 2.0;
    double lx[4], ly[4];
    int i;
    lx[0] = -hw; lx[1] = +hw; lx[2] = +hw; lx[3] = -hw;
    ly[0] = +hh; ly[1] = +hh; ly[2] = -hh; ly[3] = -hh;
    memset(p, 0, sizeof *p);
    for (i = 0; i < 4; i++) {
        p->c[i][0] = (float)(lx[i] * cs - ly[i] * sn);
        p->c[i][1] = (float)(lx[i] * sn + ly[i] * cs);
        p->c[i][2] = (float)z;
    }
    p->n[2] = 1.0f;
    p->face = 4;
    _snprintf_s(p->name, sizeof p->name, _TRUNCATE, "%s", name);
}

/* A rect tilted `deg` about the y axis: the surface rises across x, so its
 * normal leans by exactly `deg` and n[2] is cos(deg). */
static void mkplat_tilted(sh_aug_platform *p, double deg, double w, double h,
                          double z, const char *name)
{
    double r = deg * 3.14159265358979323846 / 180.0;
    double cs = cos(r), sn = sin(r), hw = w / 2.0, hh = h / 2.0;
    double lx[4], ly[4];
    int i;
    lx[0] = -hw; lx[1] = +hw; lx[2] = +hw; lx[3] = -hw;
    ly[0] = +hh; ly[1] = +hh; ly[2] = -hh; ly[3] = -hh;
    memset(p, 0, sizeof *p);
    for (i = 0; i < 4; i++) {
        p->c[i][0] = (float)(lx[i] * cs);
        p->c[i][1] = (float)ly[i];
        p->c[i][2] = (float)(z + lx[i] * sn);
    }
    p->n[0] = (float)(-sn); p->n[1] = 0.0f; p->n[2] = (float)cs;
    p->face = 4;
    _snprintf_s(p->name, sizeof p->name, _TRUNCATE, "%s", name);
}

/* On a sloped quad the surface height is a FUNCTION of position. Every link
 * endpoint depends on getting this right. */
static void test_z_at_interpolates_across_a_slope(void)
{
    unsigned char q[2048];
    double corners[4][3];
    printf("the surface height varies across a sloped quad\n");
    corners[0][0] = 0;   corners[0][1] = 100; corners[0][2] = 0;
    corners[1][0] = 100; corners[1][1] = 100; corners[1][2] = 50;
    corners[2][0] = 100; corners[2][1] = 0;   corners[2][2] = 50;
    corners[3][0] = 0;   corners[3][1] = 0;   corners[3][2] = 0;
    CHECK(sh_aug_test_quad_size() <= sizeof q);
    CHECK(sh_aug_test_quad_init(q, corners) == 1);
    CHECK(near_d(sh_aug_test_z_at(q,   0.0, 50.0),  0.0));
    CHECK(near_d(sh_aug_test_z_at(q,  50.0, 50.0), 25.0));
    CHECK(near_d(sh_aug_test_z_at(q, 100.0, 50.0), 50.0));
    /* A 50-in-100 rise is 26.57 degrees, cos 0.894427 -- and that number is
     * exactly what the minFloorCos gate reads. */
    CHECK(near_d(sh_aug_test_quad_normal_z(q), 0.894427));
}

/* Inside points must pass containment; reversed normals invert carving and insets. */
static void test_contains_is_true_inside(void)
{
    unsigned char q[2048];
    double corners[4][3];
    printf("a point inside the quad is inside it\n");
    corners[0][0] = 0;   corners[0][1] = 100; corners[0][2] = 0;
    corners[1][0] = 100; corners[1][1] = 100; corners[1][2] = 0;
    corners[2][0] = 100; corners[2][1] = 0;   corners[2][2] = 0;
    corners[3][0] = 0;   corners[3][1] = 0;   corners[3][2] = 0;
    CHECK(sh_aug_test_quad_init(q, corners) == 1);
    CHECK(sh_aug_test_quad_contains(q, 50.0, 50.0) == 1);
    CHECK(sh_aug_test_quad_contains(q, 150.0, 50.0) == 0);
    CHECK(sh_aug_test_quad_contains(q, 50.0, -50.0) == 0);
}

/* The inset of a ROTATED quad is the rotated inset. The axis-wise inset this
 * replaced would eat the corners and refuse platforms that are large enough. */
static void test_inset_of_a_rotated_quad_stays_rotated(void)
{
    unsigned char q[2048], in[2048];
    double corners[4][3], c0[3];
    printf("the inset of a rotated quad is rotated too\n");
    /* A 200x200 square yawed 45 degrees: corners on the axes at r = 141.421,
     * wound clockwise seen from +Z. */
    corners[0][0] = 0;        corners[0][1] = 141.421;  corners[0][2] = 0;
    corners[1][0] = 141.421;  corners[1][1] = 0;        corners[1][2] = 0;
    corners[2][0] = 0;        corners[2][1] = -141.421; corners[2][2] = 0;
    corners[3][0] = -141.421; corners[3][1] = 0;        corners[3][2] = 0;
    CHECK(sh_aug_test_quad_init(q, corners) == 1);
    CHECK(sh_aug_test_quad_inset(q, 24.0, in) == 1);
    sh_aug_test_quad_corner(in, 0, c0);
    /* Each edge moves in by 24 along its own normal, so a corner sitting on an
     * axis moves in by 24*sqrt(2) = 33.941. */
    CHECK(near_d(c0[0], 0.0));
    CHECK(near_d(c0[1], 141.421 - 33.9411));
}

/* Smaller than twice the inset: refused, never inverted. If the edge normal
 * pointed outward this quad would GROW and this check would pass a lie. */
static void test_inset_refuses_a_collapsing_quad(void)
{
    unsigned char q[2048], in[2048];
    double corners[4][3];
    printf("a quad too small to inset is refused\n");
    corners[0][0] = 0;  corners[0][1] = 30; corners[0][2] = 0;
    corners[1][0] = 30; corners[1][1] = 30; corners[1][2] = 0;
    corners[2][0] = 30; corners[2][1] = 0;  corners[2][2] = 0;
    corners[3][0] = 0;  corners[3][1] = 0;  corners[3][2] = 0;
    CHECK(sh_aug_test_quad_init(q, corners) == 1);
    CHECK(sh_aug_test_quad_inset(q, 24.0, in) == 0);
}

/* node.field4 records any z component, including on yawed vertical planes. */
static void test_field4_follows_the_plane_z_component(void)
{
    printf("node.field4 follows the plane's z component\n");
    CHECK(sh_aug_test_node_field4(0.0)   == 0);   /* vertical, any yaw */
    CHECK(sh_aug_test_node_field4(1.0)   == 1);   /* axis-aligned Z */
    CHECK(sh_aug_test_node_field4(0.894) == 1);   /* oblique */
    CHECK(sh_aug_test_node_field4(-0.5)  == 1);   /* oblique, leaning down */
}

/* The engine's own bake walks floors up to minFloorCos -- 0.7, 45.57 degrees.
 * Steeper is refused WITH the angle, not silently dropped. */
static void test_minfloorcos_gate_at_the_boundary(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("a face steeper than minFloorCos is refused with its angle\n");
    mkplat_tilted(&p[0], 45.0, 512.0, 512.0, 16.0, "shallow");   /* cos 0.7071 */
    mkplat_tilted(&p[1], 46.0, 512.0, 512.0, 400.0, "steep");    /* cos 0.6947 */
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK_MSG(rep.platforms[0].emitted == 1, "45 degrees is inside the limit");
    CHECK_MSG(rep.platforms[1].emitted == 0, "46 degrees is past it");
    CHECK(near_f(rep.platforms[1].tilt_degrees, 46.0f));
    CHECK_MSG(strstr(rep.platforms[1].reason, "46") != NULL,
              "the refusal names the measured angle");
    sh_aas_free(a);
}

/* Reject an invalid minFloorCos; a zero fallback could admit vertical walls.
 * Settings offsets are bytes: word 16 is at 208 + 16*4 = 272. */
static void test_unreadable_minfloorcos_rejects_the_payload(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("a payload with no readable slope limit is refused\n");
    sh_aas_set_setting_f32(a, 272u, 0.0f);
    mkplat(&p, -256.0f, -256.0f, 256.0f, 256.0f, 16.0f, "shelf");
    CHECK_MSG(sh_aas_augment(a, &p, 1, &o, &rep) == 0,
              "fail closed rather than treat a wall as floor");
    sh_aas_free(a);
}

/* The rotated area must resolve through the BSP without claiming its whole
 * axis-aligned bounding box. */
static void test_a_yawed_area_resolves_through_the_bsp(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("a yawed area is findable, and only where it actually is\n");
    mkplat_yawed(&p, 45.0, 400.0, 400.0, 16.0, "diamond");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK(rep.platforms[0].emitted == 1);
    if (rep.platforms[0].emitted) {
        CHECK_MSG(sh_aas_point_area(a, 0.0f, 0.0f, 18.0f) == rep.platforms[0].area,
                  "the centre of the diamond is on it");
        /* Inside the bounding box, outside the rotated quad. An axis-aligned
         * carve would claim this point; an oriented one must not. */
        CHECK_MSG(sh_aas_point_area(a, 270.0f, 270.0f, 18.0f) != rep.platforms[0].area,
                  "a corner of the bounding square is NOT on the diamond");
    }
    sh_aas_free(a);
}

/* ==================================================================== */
/* chaining: volumes standing together link to EACH OTHER                */
/* ==================================================================== */

/* Does any reachability join these two areas, and how many? */
static int reach_count(sh_aas *a, int from, int to)
{
    unsigned i, n = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    int found = 0;
    for (i = 0; i < n; i++) {
        const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
        if (!r) continue;
        /* reachability: travel_flags u32 at 0, from u16 at 6, to u16 at 8. */
        if ((int)sh_aas_get_u16(r, 6) == from && (int)sh_aas_get_u16(r, 8) == to) found++;
    }
    return found;
}

static int reach_exists(sh_aas *a, int from, int to) { return reach_count(a, from, to) > 0; }

/* Symmetric discovery must not duplicate records with identical areas and
 * endpoints. Link counts alone cannot distinguish duplicates from distinct samples. */
static int no_duplicate_reach_endpoints(sh_aas *a)
{
    unsigned i, j, n = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    for (i = 0; i < n; i++) {
        const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
        if (!r) continue;
        for (j = i + 1; j < n; j++) {
            const unsigned char *q = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, j);
            if (!q) continue;
            if (sh_aas_get_u16(r, 6) != sh_aas_get_u16(q, 6)) continue;   /* from */
            if (sh_aas_get_u16(r, 8) != sh_aas_get_u16(q, 8)) continue;   /* to   */
            if (memcmp(r + 12, q + 12, 12) == 0) return 0;   /* start[3]+end[3] i16 */
        }
    }
    return 1;
}

/* How many of them are of one travel type -- 0x20 is a plain walk. */
static int reach_count_of_type(sh_aas *a, int from, int to, unsigned type)
{
    unsigned i, n = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    int found = 0;
    for (i = 0; i < n; i++) {
        const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
        if (!r) continue;
        if ((int)sh_aas_get_u16(r, 6) == from && (int)sh_aas_get_u16(r, 8) == to &&
            sh_aas_get_u32(r, 0) == type) found++;
    }
    return found;
}

/* Adjacent volumes 12 units apart in height must link directly by stepping,
 * without routing down to the module floor. */
static void test_two_abutting_platforms_link_to_each_other(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("two volumes standing together link to each other\n");
    mkplat(&p[0], -512.0f, -512.0f,    0.0f, 512.0f, 16.0f, "lower");
    mkplat(&p[1],    0.0f, -512.0f,  512.0f, 512.0f, 28.0f, "upper");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK(rep.platforms[0].emitted == 1);
    CHECK(rep.platforms[1].emitted == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted) {
        CHECK_MSG(reach_exists(a, rep.platforms[0].area, rep.platforms[1].area),
                  "the lower one reaches the upper one directly");
        CHECK_MSG(reach_exists(a, rep.platforms[1].area, rep.platforms[0].area),
                  "and back again");
        CHECK_MSG(rep.platforms[0].neighbours >= 1, "the report says so");
    }
    sh_aas_free(a);
}

/* A neighbour abutting only PART of an edge. The single midpoint probe this
 * replaced landed past it and answered with the floor. */
static void test_a_partially_abutting_neighbour_is_found(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("a neighbour touching only part of an edge is still found\n");
    mkplat(&p[0], -512.0f, -512.0f,   0.0f, 512.0f, 16.0f, "wide");
    /* Covers only the top third of the shared edge. */
    mkplat(&p[1],    0.0f,  200.0f, 512.0f, 512.0f, 28.0f, "corner");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted)
        CHECK_MSG(reach_exists(a, rep.platforms[0].area, rep.platforms[1].area),
                  "the partial overlap is enough to link them");
    sh_aas_free(a);
}

/* One owner emits both directions for each segment. Compare direction counts
 * because a segment may contain several samples. */
static void test_a_pair_is_not_emitted_twice(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int fwd, back;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("each pair of volumes is linked once, not twice\n");
    mkplat(&p[0], -512.0f, -512.0f,    0.0f, 512.0f, 16.0f, "a");
    mkplat(&p[1],    0.0f, -512.0f,  512.0f, 512.0f, 28.0f, "b");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted) {
        fwd  = reach_count(a, rep.platforms[0].area, rep.platforms[1].area);
        back = reach_count(a, rep.platforms[1].area, rep.platforms[0].area);
        CHECK(fwd > 0);
        CHECK_MSG(fwd == back, "the two directions must agree");
    }
    sh_aas_free(a);
}

/* ==================================================================== */
/* leaps: crossing a gap                                                 */
/* ==================================================================== */

/* Load a synthetic LEDGE/LEAP table through the public loader. */
static const char *TRAV_TABLE_TEXT =
"{\n"
"\tedit = {\n"
"\t\ttable = {\n"
"\t\t\ttable[0] = {\n"
"\t\t\t\tmonster = \"Imp\";\n"
"\t\t\t\ttraversal = {\n"
"\t\t\t\t\ttraversal[0] = {\n"
"\t\t\t\t\t\ttype = \"LEDGE_UP_64\";\n"
"\t\t\t\t\t\tpath = \"m/imp/traversal/jump_ledge_up_64\";\n"
"\t\t\t\t\t\toffset = {\n\t\t\t\t\t\t\tx = -32;\n\t\t\t\t\t\t}\n"
"\t\t\t\t\t}\n"
"\t\t\t\t\ttraversal[1] = {\n"
"\t\t\t\t\t\ttype = \"LEDGE_UP_128\";\n"
"\t\t\t\t\t\tpath = \"m/imp/traversal/jump_ledge_up_128\";\n"
"\t\t\t\t\t\toffset = {\n\t\t\t\t\t\t\tx = -32;\n\t\t\t\t\t\t}\n"
"\t\t\t\t\t}\n"
"\t\t\t\t\ttraversal[2] = {\n"
"\t\t\t\t\t\ttype = \"LEDGE_UP_512\";\n"
"\t\t\t\t\t\tpath = \"m/imp/traversal/jump_ledge_up_512\";\n"
"\t\t\t\t\t\toffset = {\n\t\t\t\t\t\t\tx = -32;\n\t\t\t\t\t\t}\n"
"\t\t\t\t\t}\n"
"\t\t\t\t\ttraversal[3] = {\n"
"\t\t\t\t\t\ttype = \"LEDGE_DOWN_64\";\n"
"\t\t\t\t\t\tpath = \"m/imp/traversal/jump_ledge_down_64\";\n"
"\t\t\t\t\t\toffset = {\n\t\t\t\t\t\t\tx = -20;\n\t\t\t\t\t\t}\n"
"\t\t\t\t\t}\n"
"\t\t\t\t\ttraversal[4] = {\n"
"\t\t\t\t\t\ttype = \"LEDGE_DOWN_128\";\n"
"\t\t\t\t\t\tpath = \"m/imp/traversal/jump_ledge_down_128\";\n"
"\t\t\t\t\t\toffset = {\n\t\t\t\t\t\t\tx = -20;\n\t\t\t\t\t\t}\n"
"\t\t\t\t\t}\n"
"\t\t\t\t\ttraversal[5] = {\n"
"\t\t\t\t\t\ttype = \"LEDGE_DOWN_512\";\n"
"\t\t\t\t\t\tpath = \"m/imp/traversal/jump_ledge_down_512\";\n"
"\t\t\t\t\t\toffset = {\n\t\t\t\t\t\t\tx = -20;\n\t\t\t\t\t\t}\n"
"\t\t\t\t\t}\n"
"\t\t\t\t\ttraversal[6] = {\n"
"\t\t\t\t\t\ttype = \"LEAP_ACROSS_256\";\n"
"\t\t\t\t\t\tpath = \"m/imp/traversal/jump_forward_256\";\n"
"\t\t\t\t\t\toffset = {\n\t\t\t\t\t\t\tx = -60;\n\t\t\t\t\t\t}\n"
"\t\t\t\t\t}\n"
"\t\t\t\t\ttraversal[7] = {\n"
"\t\t\t\t\t\ttype = \"LEAP_ACROSS_512\";\n"
"\t\t\t\t\t\tpath = \"m/imp/traversal/jump_forward_512\";\n"
"\t\t\t\t\t\toffset = {\n\t\t\t\t\t\t\tx = -60;\n\t\t\t\t\t\t}\n"
"\t\t\t\t\t}\n"
"\t\t\t\t}\n"
"\t\t\t}\n"
"\t\t}\n"
"\t}\n"
"}\n";

static unsigned char *trav_table_reader(const char *name, size_t *out_len)
{
    size_t n = strlen(TRAV_TABLE_TEXT);
    unsigned char *buf;
    (void)name;
    buf = (unsigned char *)malloc(n);
    if (!buf) return NULL;
    memcpy(buf, TRAV_TABLE_TEXT, n);
    if (out_len) *out_len = n;
    return buf;
}

static void load_synthetic_traversal_table(void)
{
    CHECK_MSG(sh_trav_load(trav_table_reader) == 1, "the synthetic table parses");
    CHECK_MSG(sh_trav_ready() == 1, "and leaves the module ready");
}

/* Two platforms with a 600-unit gap: inside the range shipped leaps cover, and
 * level, so a demon with a LEAP_ACROSS row can cross it. */
static void test_a_gap_within_range_becomes_a_leap(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a gap the shipped animations cover becomes a leap\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -1200.0f, -400.0f, -600.0f, 400.0f, 16.0f, "near");
    mkplat(&p[1],     0.0f, -400.0f,  600.0f, 400.0f, 16.0f, "far");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted)
        CHECK_MSG(rep.platforms[0].leaps > 0, "the 600-unit gap is crossed");
    sh_aas_free(a);
}

/* Beyond the range shipped leaps cover: not crossed. An uncrossed gap is a
 * reported island edge, never an error. */
static void test_a_gap_beyond_range_is_not_crossed(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a gap wider than any shipped leap is left uncrossed\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -2000.0f, -400.0f, -1500.0f, 400.0f, 16.0f, "near");
    mkplat(&p[1],  1500.0f, -400.0f,  2000.0f, 400.0f, 16.0f, "far");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK_MSG(rep.platforms[0].leaps == 0, "3000 units is past every animation");
    sh_aas_free(a);
}

/* Nominal leap animations do not justify links across steep vertical gaps. */
static void test_a_steep_gap_is_refused(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a gap between surfaces at very different heights is refused\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -1200.0f, -400.0f, -600.0f, 400.0f,  16.0f, "near");
    mkplat(&p[1],     0.0f, -400.0f,  600.0f, 400.0f, 400.0f, "far");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK_MSG(rep.platforms[0].leaps == 0, "a 0.64 grade is past the envelope");
    sh_aas_free(a);
}

/* ==================================================================== */
/* direction: a neighbour ABOVE is a climb, never a step                 */
/* ==================================================================== */

/* Gate steps by absolute height change: a negative drop for a tall rise must
 * not pass a positive maxStepHeight limit and become a plain walk link. */
static void test_a_tall_neighbour_gets_a_climb_not_a_walk(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a neighbour standing above is climbed, not stepped onto\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -512.0f, -512.0f,   0.0f, 512.0f,  16.0f, "low");
    mkplat(&p[1],    0.0f, -512.0f, 512.0f, 512.0f, 128.0f, "high");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted) {
        CHECK_MSG(reach_count_of_type(a, rep.platforms[0].area,
                                      rep.platforms[1].area, 0x20u) == 0,
                  "a 112-unit rise must NEVER be a plain walk link");
        CHECK_MSG(reach_count_of_type(a, rep.platforms[1].area,
                                      rep.platforms[0].area, 0x20u) == 0,
                  "nor the reverse");
        CHECK_MSG(rep.platforms[0].climbs > 0 || rep.platforms[1].climbs > 0,
                  "it is carried by a traversal instead");
    }
    sh_aas_free(a);
}

/* The lower area index owns the pair and must emit both climb and descent links. */
static void test_the_climb_is_reciprocated(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a climb between two volumes goes both ways\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -512.0f, -512.0f,   0.0f, 512.0f,  16.0f, "low");
    mkplat(&p[1],    0.0f, -512.0f, 512.0f, 512.0f, 128.0f, "high");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted) {
        CHECK_MSG(reach_exists(a, rep.platforms[0].area, rep.platforms[1].area),
                  "up");
        CHECK_MSG(reach_exists(a, rep.platforms[1].area, rep.platforms[0].area),
                  "and back down");
    }
    sh_aas_free(a);
}

/* Each adjacent pair in a three-platform climb must link without using the floor. */
static void test_a_three_tower_chain_links_end_to_end(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[3];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a three-volume chain links end to end\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -900.0f, -512.0f, -300.0f, 512.0f,  16.0f, "a");
    mkplat(&p[1], -300.0f, -512.0f,  300.0f, 512.0f,  80.0f, "b");
    mkplat(&p[2],  300.0f, -512.0f,  900.0f, 512.0f, 144.0f, "c");
    CHECK(sh_aas_augment(a, p, 3, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted && rep.platforms[2].emitted) {
        CHECK_MSG(reach_exists(a, rep.platforms[0].area, rep.platforms[1].area), "a to b");
        CHECK_MSG(reach_exists(a, rep.platforms[1].area, rep.platforms[2].area), "b to c");
        CHECK_MSG(reach_exists(a, rep.platforms[2].area, rep.platforms[1].area), "c back to b");
        CHECK_MSG(rep.platforms[1].neighbours >= 2,
                  "the middle one reaches both of its neighbours");
    }
    sh_aas_free(a);
}

/* A gap beyond stepping range but below the shortest available leap gets no link. */
static void test_a_gap_below_the_leap_minimum_is_not_crossed(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a gap shorter than any shipped leap is not crossed\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -600.0f, -400.0f, -100.0f, 400.0f, 16.0f, "near");
    /* 80 units apart: past the touching epsilon, under SH_TRAV_LEAP_MIN_SPAN. */
    mkplat(&p[1],  -20.0f, -400.0f,  480.0f, 400.0f, 16.0f, "far");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted)
        CHECK_MSG(rep.platforms[0].leaps == 0, "80 units is below every nominal");
    sh_aas_free(a);
}

/* Reject a leap whose swept path intersects a solid between the platforms. */
static void test_a_leap_through_a_third_volume_is_dropped(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[3];
    sh_aug_report rep;
    sh_aug_report bare;
    sh_aug_platform two[2];
    sh_aas *b = load_module();
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a leap whose path runs through another volume is dropped\n");
    load_synthetic_traversal_table();

    /* Control: the same two platforms with nothing between them DO get a leap,
     * so the refusal below is the wall and not the geometry. */
    mkplat(&two[0], -1200.0f, -400.0f, -600.0f, 400.0f, 16.0f, "near");
    mkplat(&two[1],     0.0f, -400.0f,  600.0f, 400.0f, 16.0f, "far");
    CHECK(sh_aas_augment(b, two, 2, &o, &bare) == 1);
    CHECK_MSG(bare.platforms[0].leaps > 0, "the control leap is emitted");
    sh_aas_free(b);

    mkplat(&p[0], -1200.0f, -400.0f, -600.0f, 400.0f,  16.0f, "near");
    mkplat(&p[1],     0.0f, -400.0f,  600.0f, 400.0f,  16.0f, "far");
    mkplat(&p[2],  -400.0f, -400.0f, -200.0f, 400.0f, 190.0f, "wall");
    CHECK(sh_aas_augment(a, p, 3, &o, &rep) == 1);
    CHECK_MSG(!reach_exists(a, rep.platforms[0].area, rep.platforms[1].area),
              "the wall between them refuses the leap");
    sh_aas_free(a);
}

/* An ordinary upright platform is NOT a side face. face 4 is an upright box's
 * top, so a `face != 0` test would flag every platform in every map. */
static void test_an_upright_platform_is_not_a_side_face(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("an ordinary upright platform is not reported as a side face\n");
    mkplat(&p, -500.0f, -500.0f, 500.0f, 500.0f, 16.0f, "flat");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK(rep.platforms[0].side_face == 0);
    CHECK(near_f(rep.platforms[0].tilt_degrees, 0.0f));
    sh_aas_free(a);
}

/* Unclipped oblique planes increase BSP depth. Measure the resulting depth
 * because exceeding the limit rejects the entire module bake. */
static void test_a_dense_yawed_chain_stays_within_the_depth_limit(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[16];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int i;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("a dense chain of yawed platforms stays inside the loader depth limit\n");
    for (i = 0; i < 16; i++) {
        mkplat_yawed(&p[i], 30.0, 220.0, 220.0, 16.0 + i * 8.0, "chain");
        /* Spread them along x so they are a chain, not a stack. */
        p[i].c[0][0] += (float)(i * 240 - 1800); p[i].c[1][0] += (float)(i * 240 - 1800);
        p[i].c[2][0] += (float)(i * 240 - 1800); p[i].c[3][0] += (float)(i * 240 - 1800);
    }
    CHECK(sh_aas_augment(a, p, 16, &o, &rep) == 1);
    CHECK_MSG(rep.depth_exceeded == 0,
              "16 yawed platforms must not blow the 0x80 tree-depth limit");
    sh_aas_free(a);
}

/* Exercise the platform cap; the peer bounding-box filter must keep gap
 * discovery tractable across every edge sample. */
static void test_a_bake_at_the_platform_cap_completes(void)
{
    sh_aas *a = load_module();
    static sh_aug_platform p[200];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int i, emitted = 0;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a bake with two hundred platforms completes and stays coherent\n");
    load_synthetic_traversal_table();
    /* A 20x10 grid of small platforms inside the fixture's +/-2000 floor. */
    for (i = 0; i < 200; i++) {
        float x = -1900.0f + (float)(i % 20) * 190.0f;
        float y = -950.0f  + (float)(i / 20) * 190.0f;
        mkplat(&p[i], x, y, x + 150.0f, y + 150.0f, 16.0f + (float)(i % 3) * 6.0f, "cell");
    }
    CHECK_MSG(sh_aas_augment(a, p, 200, &o, &rep) == 1,
              "candidate storage grows beyond the old 2048-entry ceiling");
    CHECK(!rep.links_truncated);
    CHECK(sh_aas_count(a, SH_AAS_L_REACHABILITIES) > 2048);
    for (i = 0; i < rep.platform_count; i++) if (rep.platforms[i].emitted) emitted++;
    CHECK_MSG(emitted > 100, "most of them should land");
    CHECK_MSG(rep.depth_exceeded == 0, "and the tree stays inside the loader limit");
    sh_aas_free(a);
}

/* Add climbs from a floor that already owns traversal points. Appending must
 * regroup points by from-area so each first_trav_point/num_trav_point range
 * stays contiguous. Index 0 is the dummy; index 1 is an existing floor point. */
static void test_existing_traversals_on_our_floor_are_regrouped_not_declined(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = {0};
    unsigned char *floor_area, *tp;
    unsigned first, np, na, i, j, claimed;
    size_t rs;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a module that already owns climbs on our floor is regrouped, not declined\n");
    load_synthetic_traversal_table();

    /* Give the floor (area 1) a genuine pre-existing traversal point: a dummy at
     * index 0 that the engine skips, then one owned point at index 1. */
    CHECK(sh_aas_append(a, SH_AAS_L_TRAVERSALPOINTS, 2, &first) == 1);
    CHECK(first == 0);
    rs = sh_aas_record_size(SH_AAS_L_TRAVERSALPOINTS);
    tp = sh_aas_rec(a, SH_AAS_L_TRAVERSALPOINTS, 1);
    CHECK(tp != NULL);
    if (tp) sh_aas_put_u16(tp, 52u, 1u);            /* TP_W34 = owning area 1 */
    floor_area = sh_aas_rec(a, SH_AAS_L_AREAS, 1);
    CHECK(floor_area != NULL);
    if (floor_area) {
        sh_aas_put_u16(floor_area, 36u, 1u);        /* AR_FIRST_TRAV_POINT */
        sh_aas_put_u16(floor_area, 38u, 1u);        /* AR_NUM_TRAV_POINT   */
    }

    mkplat(&p, -500.0f, -500.0f, 500.0f, 500.0f, 128.0f, "roof");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK_MSG(rep.climbs_declined == 0, "the set is regrouped, not declined");
    CHECK_MSG(rep.platforms[0].climbs > 0, "and the climbs are there");

    /* Every real point belongs to exactly one in-bounds area range; index 0 is unowned. */
    np = sh_aas_count(a, SH_AAS_L_TRAVERSALPOINTS);
    na = sh_aas_count(a, SH_AAS_L_AREAS);
    CHECK(np > 2);
    claimed = 0;
    for (i = 0; i < na; i++) {
        const unsigned char *ar = sh_aas_rec_const(a, SH_AAS_L_AREAS, i);
        unsigned f, num;
        if (!ar) continue;
        f = sh_aas_get_u16(ar, 36u);
        num = sh_aas_get_u16(ar, 38u);
        if (!num) continue;
        CHECK_MSG(f >= 1 && f + num <= np, "the range is in bounds and skips the dummy");
        for (j = f; j < f + num; j++) {
            const unsigned char *q = sh_aas_rec_const(a, SH_AAS_L_TRAVERSALPOINTS, j);
            CHECK_MSG(q && sh_aas_get_u16(q, 52u) == i,
                      "every point in an area's range is a traversal out of that area");
        }
        claimed += num;
    }
    CHECK_MSG(claimed == np - 1, "the ranges partition exactly the real points");
    (void)rs;
    sh_aas_free(a);
}

/* The ordinary case must not regress: a module with no traversal points of its
 * own still gets its climbs. */
static void test_a_module_without_traversals_still_gets_climbs(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a module with no climbs of its own still gets ours\n");
    load_synthetic_traversal_table();
    mkplat(&p, -500.0f, -500.0f, 500.0f, 500.0f, 128.0f, "roof");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK_MSG(rep.climbs_declined == 0, "nothing to decline");
    CHECK_MSG(rep.platforms[0].climbs > 0, "and the climbs are there");
    sh_aas_free(a);
}

/* ==================================================================== */
/* the geometry matrix: every way two marked volumes can meet            */
/* ==================================================================== */
/* In these connected overlap arrangements, each emitted platform must have
 * a route. A refused platform is acceptable; an unexpected island is not. */

/* A pillar inside a slab is discovered only from the pillar side. Pair
 * ownership must not assign emission to the slab, which cannot see that edge. */
static void test_a_pillar_through_a_slab_is_linked(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int slab, pil;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a pillar standing through a slab is linked, not stranded\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -384.0f, -384.0f, 384.0f, 384.0f,  16.0f, "slab");
    mkplat(&p[1],  -96.0f,  -96.0f,  96.0f,  96.0f, 128.0f, "pillar");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    slab = 0; pil = 1;
    if (rep.platforms[pil].emitted) {
        CHECK_MSG(rep.platforms[pil].island == 0,
                  "the contained platform must not be an island -- an emitted "
                  "area nothing reaches is what freezes every demon on the map");
        CHECK_MSG(rep.platforms[pil].links > 0, "and it must carry links");
    }
    (void)slab;
    sh_aas_free(a);
}

/* The mirror: the CONTAINING platform is created second (higher centroid), so
 * the index order flips. Ownership must still land on the side that can see it. */
static void test_containment_works_with_the_index_order_reversed(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("containment is linked whichever platform is created first\n");
    load_synthetic_traversal_table();
    /* The small one is LOWER here, so it is created first and takes the lower
     * index -- the opposite of the pillar case. */
    mkplat(&p[0],  -96.0f,  -96.0f,  96.0f,  96.0f,  16.0f, "inner");
    mkplat(&p[1], -384.0f, -384.0f, 384.0f, 384.0f, 128.0f, "outer");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted) {
        CHECK_MSG(!(rep.platforms[0].island && rep.platforms[1].island),
                  "both cannot be islands");
    }
    sh_aas_free(a);
}

/* Identical footprints contain each other; break the tie by area index
 * instead of letting both sides emit. */
static void test_identical_footprints_are_not_double_linked(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int fwd, back;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("two volumes with the same footprint are linked once, not twice\n");
    mkplat(&p[0], -256.0f, -256.0f, 256.0f, 256.0f, 16.0f, "lower");
    mkplat(&p[1], -256.0f, -256.0f, 256.0f, 256.0f, 28.0f, "upper");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted) {
        fwd  = reach_count(a, rep.platforms[0].area, rep.platforms[1].area);
        back = reach_count(a, rep.platforms[1].area, rep.platforms[0].area);
        CHECK_MSG(fwd == back, "the two directions must agree");
        CHECK(no_duplicate_reach_endpoints(a));
    }
    sh_aas_free(a);
}

/* Partial overlap is discovered from both sides and must keep a single owner. */
static void test_partially_overlapping_volumes_still_link_once(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("partially overlapping volumes link exactly once\n");
    mkplat(&p[0], -400.0f, -400.0f,  100.0f, 100.0f, 16.0f, "a");
    mkplat(&p[1], -100.0f, -100.0f,  400.0f, 400.0f, 28.0f, "b");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    if (rep.platforms[0].emitted && rep.platforms[1].emitted) {
        CHECK(reach_count(a, rep.platforms[0].area, rep.platforms[1].area) ==
              reach_count(a, rep.platforms[1].area, rep.platforms[0].area));
        CHECK(no_duplicate_reach_endpoints(a));
    }
    sh_aas_free(a);
}

/* An intersecting floating volume must be connected if it is emitted. */
static void test_a_floating_intersecting_volume_is_never_a_stranded_area(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int i;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a floating volume intersecting a solid one is linked or refused, "
           "never stranded\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -384.0f, -384.0f, 384.0f, 384.0f,  16.0f, "solid");
    mkplat(&p[1],  -96.0f,  -96.0f,  96.0f,  96.0f, 120.0f, "floater");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    for (i = 0; i < rep.platform_count; i++) {
        if (!rep.platforms[i].emitted) {
            CHECK_MSG(rep.platforms[i].reason[0] != 0,
                      "a refusal must say why");
            continue;
        }
        CHECK_MSG(rep.platforms[i].island == 0,
                  "an emitted platform must be reachable");
    }
    sh_aas_free(a);
}

/* Without an underlying leaf to splice, refuse the platform instead of
 * counting an area that cannot be reached through the BSP. */
static void test_a_platform_with_no_carrier_is_refused_not_phantom(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("a platform the tree cannot hold is refused, not left a phantom\n");
    /* Far outside the fixture's +/-2000 floor slab and high above it. */
    mkplat(&p, 6000.0f, 6000.0f, 6600.0f, 6600.0f, 900.0f, "orphan");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    if (!rep.platforms[0].emitted)
        CHECK_MSG(rep.platforms[0].reason[0] != 0, "and it says why");
    else
        CHECK_MSG(rep.platforms[0].leaf_slots_carved > 0,
                  "if it claims to be emitted it must be in the tree");
    sh_aas_free(a);
}

/* Across this connected fixture matrix, no emitted platform may be an island. */
static void test_no_arrangement_emits_a_stranded_area(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[8];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int i, emitted = 0;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("no arrangement in the matrix emits a stranded area\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -1500.0f, -400.0f, -740.0f, 360.0f,  16.0f, "slab");
    mkplat(&p[1], -1210.0f, -110.0f, -1030.0f, 70.0f, 128.0f, "pillar");   /* contained */
    mkplat(&p[2],  -600.0f, -400.0f,  160.0f, 360.0f,  16.0f, "solid");
    mkplat(&p[3],  -310.0f, -110.0f, -130.0f,  70.0f, 120.0f, "floater");  /* contained, floating */
    mkplat(&p[4],   400.0f, -400.0f,  900.0f, 100.0f,  16.0f, "ov_a");
    mkplat(&p[5],   700.0f, -100.0f, 1200.0f, 360.0f,  28.0f, "ov_b");     /* partial */
    mkplat(&p[6],  1400.0f, -400.0f, 1900.0f, 360.0f,  16.0f, "apart_a");
    mkplat(&p[7],  1400.0f,  500.0f, 1900.0f, 900.0f,  16.0f, "apart_b");  /* separate */
    CHECK(sh_aas_augment(a, p, 8, &o, &rep) == 1);
    for (i = 0; i < rep.platform_count; i++) {
        if (!rep.platforms[i].emitted) continue;
        emitted++;
        CHECK_MSG(rep.platforms[i].island == 0, rep.platforms[i].name);
    }
    CHECK_MSG(emitted >= 4, "most of the matrix should still be emitted");
    CHECK(rep.depth_exceeded == 0);
    sh_aas_free(a);
}

/* The area-count increase must equal the reported emitted-platform count,
 * including refusal cases. A late refusal must not leave an unowned area behind. */
static void test_areas_added_equals_platforms_emitted(void)
{
    static const struct { float x0, y0, x1, y1, z; const char *name; } CASE[] = {
        { -400.0f, -400.0f,  400.0f,  400.0f,   16.0f, "ordinary" },
        { 6000.0f, 6000.0f, 6600.0f, 6600.0f,  900.0f, "over nothing" },
        { 1900.0f, -400.0f, 2600.0f,  400.0f,   16.0f, "overhanging the edge" },
        {  -60.0f,  -60.0f,   60.0f,   60.0f,   16.0f, "small" },
    };
    int ci;
    printf("every area added belongs to a platform the report claims\n");
    for (ci = 0; ci < (int)(sizeof CASE / sizeof CASE[0]); ci++) {
        sh_aas *a = load_module();
        sh_aug_platform p;
        sh_aug_report rep;
        sh_aug_opts o = {0};
        int i, emitted = 0, added;
        o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
        mkplat(&p, CASE[ci].x0, CASE[ci].y0, CASE[ci].x1, CASE[ci].y1,
               CASE[ci].z, CASE[ci].name);
        CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
        for (i = 0; i < rep.platform_count; i++)
            if (rep.platforms[i].emitted) emitted++;
        added = (int)rep.areas_after - (int)rep.areas_before;
        CHECK_MSG(added == emitted, CASE[ci].name);
        /* And a refusal always explains itself. */
        for (i = 0; i < rep.platform_count; i++)
            if (!rep.platforms[i].emitted)
                CHECK_MSG(rep.platforms[i].reason[0] != 0, CASE[ci].name);
        sh_aas_free(a);
    }
}

/* An emitted platform must resolve to its own area at its centre, including
 * when it overhangs the module floor and could receive only a partial splice. */
static void test_an_emitted_platform_resolves_at_its_own_centre(void)
{
    static const struct { float x0, y0, x1, y1, z; const char *name; } CASE[] = {
        { -400.0f, -400.0f,  400.0f,  400.0f,  16.0f, "ordinary" },
        { 1900.0f, -400.0f, 2600.0f,  400.0f,  16.0f, "overhanging the edge" },
        { 1990.0f, -400.0f, 2800.0f,  400.0f,  16.0f, "mostly off the edge" },
    };
    int ci;
    printf("an emitted platform resolves to its own area at its centre\n");
    for (ci = 0; ci < (int)(sizeof CASE / sizeof CASE[0]); ci++) {
        sh_aas *a = load_module();
        sh_aug_platform p;
        sh_aug_report rep;
        sh_aug_opts o = {0};
        float cx, cy, cz;
        o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
        mkplat(&p, CASE[ci].x0, CASE[ci].y0, CASE[ci].x1, CASE[ci].y1,
               CASE[ci].z, CASE[ci].name);
        CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
        if (rep.platforms[0].emitted) {
            cx = (CASE[ci].x0 + CASE[ci].x1) / 2.0f;
            cy = (CASE[ci].y0 + CASE[ci].y1) / 2.0f;
            cz = CASE[ci].z + 2.0f;
            CHECK_MSG(sh_aas_point_area(a, cx, cy, cz) == rep.platforms[0].area,
                      CASE[ci].name);
        } else {
            CHECK_MSG(rep.platforms[0].reason[0] != 0, CASE[ci].name);
        }
        sh_aas_free(a);
    }
}

/* Intersecting solids must subtract occupied ground. Set depth explicitly;
 * zero-depth fixtures exercise faces without solid-volume subtraction. */

static void mkbox(sh_aug_platform *p, float x0, float y0, float x1, float y1,
                  float top, float bottom, const char *name)
{
    mkplat(p, x0, y0, x1, y1, top, name);
    p->depth = top - bottom;            /* swept back along -n, which is -z here */
}

/* The pieces of `rep` that came from requested volume `src`. */
static int pieces_of(const sh_aug_report *rep, int src, int *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < rep->platform_count && n < cap; i++)
        if (rep->platforms[i].source == src) out[n++] = i;
    return n;
}

static void test_a_pillar_through_a_slab_is_cut_out_of_it(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int idx[8], n, i, at_centre, claimed = 0;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a pillar standing through a slab is cut out of the slab's top\n");
    load_synthetic_traversal_table();
    /* A pillar rising through a floor-standing slab must remove its footprint
     * from the slab's walkable ground. */
    mkbox(&p[0], -384.0f, -384.0f, 384.0f, 384.0f,  16.0f, 0.0f, "slab");
    mkbox(&p[1],  -96.0f,  -96.0f,  96.0f,  96.0f, 128.0f, 0.0f, "pillar");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK_MSG(rep.source_count == 2, "two volumes were asked about");

    n = pieces_of(&rep, 0, idx, 8);
    CHECK_MSG(n == 4, "the slab becomes the four strips around the pillar");
    for (i = 0; i < n; i++) {
        CHECK_MSG(rep.platforms[idx[i]].pieces == 4,
                  "and every strip says so, so the author is told");
        /* Not vacuous: a cut that emitted nothing would satisfy every
         * "no strip claims that ground" check below for the wrong reason. */
        CHECK_MSG(rep.platforms[idx[i]].emitted,
                  "and every strip is actually emitted -- reason: %s");
    }

    /* Standing room inside the pillar must not resolve to a slab area. */
    at_centre = sh_aas_point_area(a, 0.0f, 0.0f, 17.0f);
    CHECK_MSG(at_centre > 0, "that point still resolves to the module's own floor");
    for (i = 0; i < n; i++)
        if (rep.platforms[idx[i]].emitted && rep.platforms[idx[i]].area == at_centre)
            claimed = 1;
    CHECK_MSG(!claimed, "no strip of the slab claims the ground inside the pillar");
    /* The other half of the same claim: a point on a strip DOES resolve to that
     * strip, so the subtraction removed the pillar and nothing else. */
    CHECK_MSG(sh_aas_point_area(a, -300.0f, 0.0f, 17.0f) != at_centre,
              "and ground on a strip resolves to the strip, not to the floor");

    /* And the strips are still one surface: they touch, so they link. */
    for (i = 0; i < n; i++)
        if (rep.platforms[idx[i]].emitted)
            CHECK_MSG(rep.platforms[idx[i]].links > 0,
                      "each strip is still reachable, not cut into islands");
    sh_aas_free(a);
}

static void test_a_face_buried_in_another_volume_is_refused_by_name(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int idx[8], n;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a face entirely inside another volume is refused for that reason\n");
    load_synthetic_traversal_table();
    /* A small box swallowed by a big one. Its top is not a surface at all: it is
     * a plane in the middle of somebody else's solid. */
    mkbox(&p[0], -384.0f, -384.0f, 384.0f, 384.0f, 128.0f, 0.0f, "block");
    mkbox(&p[1],  -96.0f,  -96.0f,  96.0f,  96.0f,  64.0f, 0.0f, "swallowed");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);

    n = pieces_of(&rep, 1, idx, 8);
    CHECK_MSG(n == 1, "a buried face is still reported once");
    if (n == 1) {
        CHECK_MSG(rep.platforms[idx[0]].emitted == 0, "and it is not emitted");
        CHECK_MSG(rep.platforms[idx[0]].pieces == 0, "with no pieces left of it");
        CHECK_MSG(strstr(rep.platforms[idx[0]].reason, "standing room") != NULL,
                  "and the reason names the burial, not some later gate");
    }
    /* The big box is UNDER the small one's face, never over it, so it keeps its
     * whole top. Subtraction must not run backwards. */
    n = pieces_of(&rep, 0, idx, 8);
    CHECK_MSG(n == 1 && rep.platforms[idx[0]].pieces == 1,
              "the containing box's own top is untouched");
    sh_aas_free(a);
}

static void test_a_floating_volume_clipping_into_a_platform_is_cut_out(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int idx[8], n, i, at_centre, claimed = 0;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a floating volume clipping into a platform is cut out of it\n");
    load_synthetic_traversal_table();
    /* A floating box intrudes into the slab; agent height controls the required headroom. */
    mkbox(&p[0], -384.0f, -384.0f, 384.0f, 384.0f, 16.0f,  0.0f, "slab");
    mkbox(&p[1],  -96.0f,  -96.0f,  96.0f,  96.0f, 72.0f,  8.0f, "floater");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);

    n = pieces_of(&rep, 0, idx, 8);
    CHECK_MSG(n == 4, "the slab is cut around the floater's footprint");
    for (i = 0; i < n; i++)
        CHECK_MSG(rep.platforms[idx[i]].emitted, "and every strip is emitted");
    at_centre = sh_aas_point_area(a, 0.0f, 0.0f, 17.0f);
    for (i = 0; i < n; i++)
        if (rep.platforms[idx[i]].emitted && rep.platforms[idx[i]].area == at_centre)
            claimed = 1;
    CHECK_MSG(!claimed, "no strip claims the ground the floater hangs in");
    sh_aas_free(a);
}

static void test_the_separated_control_arrangement_is_left_alone(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("two volumes that share nothing are not cut at all\n");
    load_synthetic_traversal_table();
    /* Caliban's RIGHT case, the control -- and the arrangement every earlier
     * fixture used, which is why none of them caught any of this. */
    mkbox(&p[0], -384.0f, -384.0f, -128.0f, 384.0f, 16.0f, 0.0f, "left");
    mkbox(&p[1],  128.0f, -384.0f,  384.0f, 384.0f, 16.0f, 0.0f, "right");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK_MSG(rep.platform_count == 2, "two volumes stay two platforms");
    CHECK_MSG(rep.platforms[0].pieces == 1 && rep.platforms[1].pieces == 1,
              "and neither is split");
    CHECK_MSG(rep.platforms[0].source == 0 && rep.platforms[1].source == 1,
              "each still names the volume it came from");
    sh_aas_free(a);
}

static void test_a_box_parked_on_a_platform_is_cut_out_of_it(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int idx[8], n;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a box standing on a platform is cut out of the platform's top\n");
    load_synthetic_traversal_table();
    /* Stacked volumes must subtract the upper box from the lower walkable face. */
    mkbox(&p[0], -384.0f, -384.0f, 384.0f, 384.0f,  16.0f,  0.0f, "floor slab");
    mkbox(&p[1],  -96.0f,  -96.0f,  96.0f,  96.0f, 112.0f, 16.0f, "crate");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    n = pieces_of(&rep, 0, idx, 8);
    CHECK_MSG(n == 4, "the slab is cut around the crate standing on it");
    n = pieces_of(&rep, 1, idx, 8);
    CHECK_MSG(n == 1 && rep.platforms[idx[0]].pieces == 1,
              "and the crate's own top is whole");
    sh_aas_free(a);
}

static void test_a_low_ceiling_over_part_of_a_platform_keeps_the_rest(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    int idx[8], n, i, any = 0;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a low ceiling over half a platform costs it that half, not all of it\n");
    load_synthetic_traversal_table();
    /* A solid 44 units above an 80-unit agent blocks only the affected portion
     * of the slab, not the entire face. */
    mkbox(&p[0], -384.0f, -384.0f, 384.0f, 384.0f, 16.0f,  0.0f, "slab");
    mkbox(&p[1],    0.0f, -384.0f, 384.0f, 384.0f, 60.0f, 16.0f, "overhang");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    n = pieces_of(&rep, 0, idx, 8);
    CHECK_MSG(n == 1, "the covered half is subtracted, leaving one piece");
    for (i = 0; i < n; i++) if (rep.platforms[idx[i]].emitted) any = 1;
    CHECK_MSG(any, "and the uncovered half is emitted rather than refused whole");
    sh_aas_free(a);
}

static void test_a_cut_platform_never_strands_an_area(void)
{
    static const struct { float px0, py0, px1, py1, ptop, pbot; } CASES[] = {
        {  -96.0f,  -96.0f,   96.0f,   96.0f, 128.0f,   0.0f },  /* centred pillar */
        { -384.0f, -384.0f,  -96.0f,   96.0f, 128.0f,   0.0f },  /* against one edge */
        { -384.0f, -384.0f,   96.0f,   96.0f, 128.0f,   0.0f },  /* into one corner */
        { -500.0f, -500.0f,  500.0f,  500.0f, 128.0f,   0.0f },  /* swallows it */
        {  -96.0f,  -96.0f,   96.0f,   96.0f,  72.0f,   8.0f },  /* floating */
        {  -96.0f,  -96.0f,   96.0f,   96.0f, 112.0f,  16.0f },  /* parked on top */
        { -384.0f,  -20.0f,  384.0f,   20.0f, 128.0f,   0.0f },  /* a thin wall across */
    };
    int ci;
    printf("no cut arrangement leaves an area nothing can reach\n");
    load_synthetic_traversal_table();
    for (ci = 0; ci < (int)(sizeof CASES / sizeof CASES[0]); ci++) {
        sh_aas *a = load_module();
        sh_aug_platform p[2];
        sh_aug_report rep;
        sh_aug_opts o = {0};
        int i;
        o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
        mkbox(&p[0], -384.0f, -384.0f, 384.0f, 384.0f, 16.0f, 0.0f, "slab");
        mkbox(&p[1], CASES[ci].px0, CASES[ci].py0, CASES[ci].px1, CASES[ci].py1,
              CASES[ci].ptop, CASES[ci].pbot, "intruder");
        CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
        /* Only the fully swallowed slab may emit nothing; partial overlap must
         * preserve the remaining walkable ground. */
        if (ci != 3) {
            int emitted = 0;
            for (i = 0; i < rep.platform_count; i++)
                if (rep.platforms[i].emitted) emitted++;
            CHECK_MSG(emitted > 0, "the cut left something to walk on");
        }
        for (i = 0; i < rep.platform_count; i++) {
            if (!rep.platforms[i].emitted) continue;
            CHECK_MSG(rep.platforms[i].island == 0,
                      "an emitted piece nothing links is the stand-still failure");
            /* Each emitted piece must resolve through the BSP at its own centre. */
            CHECK_MSG(rep.platforms[i].area > 0, "with a real area index");
        }
        sh_aas_free(a);
    }
}


static void test_a_gap_in_the_dead_zone_is_counted_and_named(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a gap nothing can step or jump across is counted, not passed over\n");
    load_synthetic_traversal_table();
    /* An 80-unit gap exceeds stepping range but is shorter than the 149-unit
     * minimum leap. Both platforms reach the floor; test their missing direct link. */
    mkbox(&p[0], -400.0f, -200.0f,  -40.0f, 200.0f, 16.0f, 0.0f, "west");
    mkbox(&p[1],   40.0f, -200.0f,  400.0f, 200.0f, 16.0f, 0.0f, "east");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK_MSG(rep.platforms[0].emitted && rep.platforms[1].emitted,
              "both volumes are emitted -- the count below means nothing if not");
    CHECK_MSG(rep.dead_gaps == 1, "the unlinkable pair is counted exactly once");
    CHECK_MSG(rep.platforms[0].island == 0 && rep.platforms[1].island == 0,
              "and neither reads as an island, which is why it needs saying");
    sh_aas_free(a);
}

static void test_a_flush_pair_is_not_a_dead_gap(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = {0};
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("two volumes standing flush are not reported as a dead gap\n");
    load_synthetic_traversal_table();
    mkbox(&p[0], -400.0f, -200.0f,    0.0f, 200.0f, 16.0f, 0.0f, "west");
    mkbox(&p[1],    0.0f, -200.0f,  400.0f, 200.0f, 16.0f, 0.0f, "east");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK_MSG(rep.dead_gaps == 0, "touching volumes step between each other");
    sh_aas_free(a);
}

static void test_suspended_bridge_is_continuous(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[3];
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_NEVER, 1, SH_AUG_TRAVERSAL_NEVER };
    int left, deck, right, x;
    printf("a suspended bridge has continuous standing space and walk links\n");
    mkbox(&p[0], 0, 0, 128, 256, 128, 0, "west support");
    mkbox(&p[1], 128, 0, 384, 256, 128, 96, "floating deck");
    mkbox(&p[2], 384, 0, 512, 256, 128, 0, "east support");
    CHECK(sh_aas_augment(a, p, 3, &o, &rep));
    left = sh_aas_point_area(a, 64, 128, 130);
    deck = sh_aas_point_area(a, 256, 128, 130);
    right = sh_aas_point_area(a, 448, 128, 130);
    CHECK(left > 1 && deck > 1 && right > 1);
    for (x = 25; x < 487; x += 7) CHECK(sh_aas_point_area(a, (float)x, 128, 130) > 1);
    CHECK(reach_exists(a, left, deck)); CHECK(reach_exists(a, deck, left));
    CHECK(reach_exists(a, deck, right)); CHECK(reach_exists(a, right, deck));
    CHECK(rep.areas_after == rep.areas_before + 3);
    sh_aas_free(a);
}

static void test_rotated_ridge_has_reciprocal_walk_links(void)
{
    sh_aas *a=load_module();sh_aug_platform p;sh_aug_report rep;
    sh_aug_opts o={SH_AUG_FALL_NEVER,1,SH_AUG_TRAVERSAL_NEVER};
    int i,left,right;double s=sqrt(0.5);
    mkbox(&p,-128,-256,128,256,128,-128,"rotated ridge");
    for(i=0;i<4;i++) {double x=p.c[i][0],z=p.c[i][2];
        p.c[i][0]=(float)((x+z)*s);p.c[i][2]=(float)((z-x)*s);}
    p.n[0]=p.n[2]=(float)s;
    CHECK(sh_aas_augment(a,&p,1,&o,&rep));
    left=sh_aas_point_area(a,-32,0,(float)(256*s-32+2));
    right=sh_aas_point_area(a,32,0,(float)(256*s-32+2));
    CHECK(left>1&&right>1&&left!=right);
    CHECK(reach_exists(a,left,right));CHECK(reach_exists(a,right,left));
    sh_aas_free(a);
}

static void test_level_floor_joins_ramp(void)
{
    sh_aas *a=load_module();sh_aug_platform p[2];sh_aug_report rep;
    sh_aug_opts o={SH_AUG_FALL_NEVER,1,SH_AUG_TRAVERSAL_NEVER};int flat,ramp;
    mkbox(&p[0],-256,-128,0,128,64,0,"level support");
    mkbox(&p[1],0,-128,256,128,64,32,"ramp");
    p[1].c[1][2]=p[1].c[2][2]=192;
    p[1].n[0]=(float)(-1/sqrt(5.0));p[1].n[2]=(float)(2/sqrt(5.0));
    CHECK(sh_aas_augment(a,p,2,&o,&rep));
    flat=sh_aas_point_area(a,-32,0,66);ramp=sh_aas_point_area(a,32,0,82);
    CHECK(flat>1&&ramp>1&&flat!=ramp);
    CHECK(reach_exists(a,flat,ramp));CHECK(reach_exists(a,ramp,flat));
    sh_aas_free(a);
}

static int generated_walk_route(const sh_aas *a,int start,int goal)
{
    unsigned char seen[SH_AUG_MAX_PLATFORMS+2]={0};
    unsigned pass,r,na=sh_aas_count(a,SH_AAS_L_AREAS),nr=sh_aas_count(a,SH_AAS_L_REACHABILITIES);
    if(start<2||goal<2||na>sizeof seen)return 0;
    seen[start]=1;
    for(pass=0;pass<na;pass++)for(r=0;r<nr;r++) {
        const unsigned char *p=sh_aas_rec_const(a,SH_AAS_L_REACHABILITIES,r);
        unsigned from=sh_aas_get_u16(p,6),to=sh_aas_get_u16(p,8);
        if(from>=2&&to>=2&&from<na&&to<na&&seen[from]&&sh_aas_get_u32(p,0)==0x20)seen[to]=1;
    }
    return seen[goal];
}

/* Independent line/convex-floor clipping. Native walking follows the area
 * polygons and cannot cross an XY gap merely because a walk record exists. */
static int floor_line_interval(const sh_aas *a,unsigned area,const double s[2],
                               const double d[2],double *lo,double *hi)
{
    const unsigned char *ar=sh_aas_rec_const(a,SH_AAS_L_AREAS,area);
    unsigned i,count=sh_aas_get_u16(ar,6),first=sh_aas_get_u32(ar,8);
    *lo=0;*hi=1;
    for(i=0;i<count;i++) {
        const unsigned char *ix=sh_aas_rec_const(a,SH_AAS_L_EDGEINDEX,first+i);
        int ei=sh_aas_get_i32(ix,0);
        const unsigned char *ed=sh_aas_rec_const(a,SH_AAS_L_EDGES,(unsigned)abs(ei));
        const unsigned char *v=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(ed,ei<0?4:0));
        const unsigned char *w=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(ed,ei<0?0:4));
        double vx=sh_aas_get_f32(v,0),vy=sh_aas_get_f32(v,4);
        double nx=sh_aas_get_f32(w,4)-vy,ny=vx-sh_aas_get_f32(w,0);
        double at=nx*(s[0]-vx)+ny*(s[1]-vy),slope=nx*d[0]+ny*d[1];
        if(fabs(slope)<1e-10){if(at < -1e-5)return 0;continue;}
        if(slope>0){double t=-at/slope;if(t>*lo)*lo=t;}
        else {double t=-at/slope;if(t<*hi)*hi=t;}
        if(*lo>*hi)return 0;
    }
    return 1;
}

static int wall_at_floor_point(const sh_aas *a,unsigned area,double x,double y)
{
    const unsigned char *ar=sh_aas_rec_const(a,SH_AAS_L_AREAS,area);unsigned i;
    for(i=0;i<sh_aas_get_u16(ar,6);i++) {
        const unsigned char *ix=sh_aas_rec_const(a,SH_AAS_L_EDGEINDEX,sh_aas_get_u32(ar,8)+i);
        const unsigned char *ed=sh_aas_rec_const(a,SH_AAS_L_EDGES,abs(sh_aas_get_i32(ix,0)));
        const unsigned char *v=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(ed,0));
        const unsigned char *w=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(ed,4));
        double vx=sh_aas_get_f32(v,0),vy=sh_aas_get_f32(v,4);
        double dx=sh_aas_get_f32(w,0)-vx,dy=sh_aas_get_f32(w,4)-vy,len=hypot(dx,dy);
        double along=len>0?((x-vx)*dx+(y-vy)*dy)/len:0;
        /* At a corner the other incident edge can legitimately remain a wall. */
        if(along>0.04&&along<len-0.04&&fabs(dx*(y-vy)-dy*(x-vx))<0.005*len&&
           (sh_aas_get_u32(ed,8)&1)) {
            printf("wall area %u point %.6f %.6f edge %.6f %.6f -> %.6f %.6f\n",area,x,y,vx,vy,vx+dx,vy+dy);return 1;
        }
    }
    return 0;
}

static int bridge_links_valid(const sh_aas *a)
{
    unsigned i;
    for(i=0;i<sh_aas_count(a,SH_AAS_L_REACHABILITIES);i++) {
        const unsigned char *p=sh_aas_rec_const(a,SH_AAS_L_REACHABILITIES,i);
        unsigned from=sh_aas_get_u16(p,6),to=sh_aas_get_u16(p,8);
        if(from<2||to<2)continue;
        if(sh_aas_get_u32(p,0)==0x20) {
            double s[2]={sh_aas_get_i16(p,12),sh_aas_get_i16(p,14)};
            double d[2]={sh_aas_get_i16(p,18)-s[0],sh_aas_get_i16(p,20)-s[1]};
            double a0,a1,b0,b1;
            if(!floor_line_interval(a,from,s,d,&a0,&a1)||!floor_line_interval(a,to,s,d,&b0,&b1)||
                (b0-a1)*sqrt(d[0]*d[0]+d[1]*d[1])>0.2) {
                printf("walk reach %u crosses disconnected floor polygons %u -> %u; s %.8f %.8f d %.8f %.8f spans %.9f %.9f %.9f %.9f\n",i,from,to,s[0],s[1],d[0],d[1],a0,a1,b0,b1);
                return 0;
            }
            if(wall_at_floor_point(a,from,s[0]+a1*d[0],s[1]+a1*d[1])||
               wall_at_floor_point(a,to,s[0]+b0*d[0],s[1]+b0*d[1])) {
                printf("walk reach %u crosses a wall flag %u -> %u\n",i,from,to);return 0;
            }
        }
        if(sh_aas_point_area(a,(float)sh_aas_get_i16(p,12),(float)sh_aas_get_i16(p,14),
            (float)sh_aas_get_i16(p,16)+2)!=from ||
           sh_aas_point_area(a,(float)sh_aas_get_i16(p,18),(float)sh_aas_get_i16(p,20),
            (float)sh_aas_get_i16(p,22)+2)!=to) {
            printf("stored reach %u misses its areas %u -> %u\n",i,from,to);return 0;
        }
    }
    for(i=1;i<sh_aas_count(a,SH_AAS_L_TRAVERSALPOINTS);i++) {
        const unsigned char *p=sh_aas_rec_const(a,SH_AAS_L_TRAVERSALPOINTS,i);
        const char *anim=(const char*)sh_aas_rec_const(a,SH_AAS_L_TRAVERSALANIMNAMES,sh_aas_get_u16(p,0x24));
        float dz=sh_aas_get_f32(p,20)-sh_aas_get_f32(p,8);
        if((strstr(anim,"ledge_up")&&dz<=0)||(strstr(anim,"ledge_down")&&dz>=0)) {
            printf("traversal %u uses %s for dz %g\n",i,anim,dz);return 0;
        }
    }
    return 1;
}

static void test_exposed_edges_remain_walls(void)
{
    sh_aas *a=load_module();sh_aug_platform p;sh_aug_report rep;
    sh_aug_opts o={SH_AUG_FALL_NEVER,1,SH_AUG_TRAVERSAL_NEVER};
    int area;unsigned i;const unsigned char *ar;
    mkbox(&p,-256,-256,256,256,128,0,"isolated roof");
    CHECK(sh_aas_augment(a,&p,1,&o,&rep));
    area=sh_aas_point_area(a,0,0,130);CHECK(area>1);
    ar=sh_aas_rec_const(a,SH_AAS_L_AREAS,(unsigned)area);
    CHECK(sh_aas_get_u16(ar,6)==4);
    for(i=0;i<sh_aas_get_u16(ar,6);i++) {
        const unsigned char *ix=sh_aas_rec_const(a,SH_AAS_L_EDGEINDEX,sh_aas_get_u32(ar,8)+i);
        const unsigned char *ed=sh_aas_rec_const(a,SH_AAS_L_EDGES,abs(sh_aas_get_i32(ix,0)));
        CHECK(sh_aas_get_u32(ed,8)==0xc01);
    }
    sh_aas_free(a);
}

static void test_unmarked_wall_keeps_routes_around_both_ends(void)
{
    static const float radii[3]={24,48,64};
    int r,rot,i,k;
    for(r=0;r<3;r++)for(rot=0;rot<2;rot++) {
        sh_aas *a=load_module();sh_aug_platform p[2];sh_aug_report rep;
        sh_aug_opts o={SH_AUG_FALL_NEVER,1,SH_AUG_TRAVERSAL_NEVER};
        double angle=rot*0.6108652382,c=cos(angle),s=sin(angle);
        int west,east,north,south;
        sh_aas_set_setting_f32(a,208,-radii[r]);sh_aas_set_setting_f32(a,212,-radii[r]);
        sh_aas_set_setting_f32(a,220,radii[r]);sh_aas_set_setting_f32(a,224,radii[r]);
        mkbox(&p[0],-1024,-1024,1024,1024,128,0,"support");
        mkbox(&p[1],-64,-384,64,384,1152,128,"unmarked wall");
        p[1].obstacle_only=1;
        for(k=0;k<4;k++) {
            double x=p[1].c[k][0],y=p[1].c[k][1];
            p[1].c[k][0]=(float)(x*c-y*s);p[1].c[k][1]=(float)(x*s+y*c);
        }
        CHECK(sh_aas_augment(a,p,2,&o,&rep));
        CHECK(!rep.pieces_truncated&&!rep.links_truncated&&!rep.reach_limit_exceeded);
        west=sh_aas_point_area(a,-700,0,130);east=sh_aas_point_area(a,700,0,130);
        north=sh_aas_point_area(a,0,700,130);south=sh_aas_point_area(a,0,-700,130);
        CHECK(west>1&&east>1&&north>1&&south>1);
        CHECK(generated_walk_route(a,west,north)&&generated_walk_route(a,north,east));
        CHECK(generated_walk_route(a,east,south)&&generated_walk_route(a,south,west));
        CHECK(bridge_links_valid(a));
        for(i=0;i<rep.platform_count;i++)if(rep.platforms[i].emitted)
            CHECK_MSG(rep.platforms[i].source==0,"an unmarked wall must not emit a walkable roof");
        sh_aas_free(a);
    }
}

static void test_intersecting_bridge_routes(void)
{
    static const int permutations[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    static const float radii[3]={24,48,64},drops[8]={0,1,8,16,18,24,64,96};
    static const double angles[5]={0,0.317,0.7853981633974483,1.917,3.941};
    int r,d,rotation,order,skew,i,k,left,deck,right;
    printf("intersecting bridge walk routes across sizes, rotations, order and step heights\n");
    load_synthetic_traversal_table();
    for(r=0;r<3;r++)for(d=0;d<8;d++)for(rotation=0;rotation<5;rotation++)
    for(order=0;order<6;order++)for(skew=0;skew<2;skew++) {
        sh_aas *a=load_module();sh_aug_platform original[3],p[3];sh_aug_report rep;
        sh_aug_opts o={SH_AUG_FALL_NEVER,1,SH_AUG_TRAVERSAL_NEVER};
        double angle=angles[rotation],c=cos(angle),s=sin(angle);
        if(drops[d]>=64)o.traversal=SH_AUG_TRAVERSAL_AUTO;
        sh_aas_set_setting_f32(a,208,-radii[r]);sh_aas_set_setting_f32(a,212,-radii[r]);
        sh_aas_set_setting_f32(a,220,radii[r]);sh_aas_set_setting_f32(a,224,radii[r]);
        mkbox(&original[0],-768,-384,-256,384,128,0,"west support");
        mkbox(&original[1],-384,-96,384,96,128-drops[d],16,"overlapping deck");
        mkbox(&original[2],256,-384,768,384,128,0,"east support");
        for(i=0;i<3;i++) {
            double box_angle=angle+(permutations[order][i]==1?skew*0.173:0);
            double bc=cos(box_angle),bs=sin(box_angle);
            p[i]=original[permutations[order][i]];
            for(k=0;k<4;k++) {
                double x=p[i].c[k][0],y=p[i].c[k][1];
                p[i].c[k][0]=(float)(x*bc-y*bs+0.375);p[i].c[k][1]=(float)(x*bs+y*bc-0.625);
            }
        }
        CHECK(sh_aas_augment(a,p,3,&o,&rep));
        left=sh_aas_point_area(a,(float)(-512*c+0.375),(float)(-512*s-0.625),130);
        deck=sh_aas_point_area(a,0.375f,-0.625f,130-drops[d]);
        right=sh_aas_point_area(a,(float)(512*c+0.375),(float)(512*s-0.625),130);
        if(!bridge_links_valid(a)) {
            printf("bridge links failed: radius %.0f drop %.0f rotation %d order %d skew %d\n",radii[r],drops[d],rotation,order,skew);
            CHECK(0);sh_aas_free(a);return;
        }
        if(drops[d]<=18) {
            if(!generated_walk_route(a,left,deck)||!generated_walk_route(a,deck,right)||
               !generated_walk_route(a,right,deck)||!generated_walk_route(a,deck,left)) {
                printf("bridge route failed: radius %.0f drop %.0f rotation %d order %d skew %d areas %d %d %d\n",
                    radii[r],drops[d],rotation,order,skew,left,deck,right);CHECK(0);sh_aas_free(a);return;
            }
        } else {
            CHECK(!generated_walk_route(a,left,deck));
            if(drops[d]>=64) {
                CHECK(reach_exists(a,left,deck));CHECK(reach_exists(a,deck,left));
                CHECK(reach_exists(a,deck,right));CHECK(reach_exists(a,right,deck));
            }
        }
        sh_aas_free(a);
    }
}

static void test_narrow_partial_contacts(void)
{
    sh_aas *a=load_module();sh_aug_platform p[9];sh_aug_report rep;
    sh_aug_opts o={SH_AUG_FALL_NEVER,1,SH_AUG_TRAVERSAL_NEVER};int i,main_area;
    mkbox(&p[0],-400,-1900,0,1900,128,0,"long support");
    for(i=0;i<8;i++) {
        float y=-1733.0f+443.0f*i;
        mkbox(&p[i+1],0,y,400,y+64,128,96,"narrow abutment");
    }
    CHECK(sh_aas_augment(a,p,9,&o,&rep));
    main_area=sh_aas_point_area(a,-200,0,130);CHECK(main_area>1);
    for(i=0;i<8;i++) {
        int peer=sh_aas_point_area(a,200,-1701.0f+443.0f*i,130);
        CHECK(peer>1&&peer!=main_area);
        unsigned r,pass,nr=sh_aas_count(a,SH_AAS_L_REACHABILITIES);
        unsigned char seen[SH_AUG_MAX_PLATFORMS+2]={0};
        CHECK(sh_aas_count(a,SH_AAS_L_AREAS)<=sizeof seen);
        seen[main_area]=1;
        for(pass=0;pass<sh_aas_count(a,SH_AAS_L_AREAS);pass++)for(r=0;r<nr;r++) {
            const unsigned char *rr=sh_aas_rec_const(a,SH_AAS_L_REACHABILITIES,r);
            unsigned from=sh_aas_get_u16(rr,6),to=sh_aas_get_u16(rr,8);
            if(from<sizeof seen&&to<sizeof seen&&seen[from]&&sh_aas_get_u32(rr,0)==0x20)seen[to]=1;
        }
        CHECK(seen[peer]);
    }
    sh_aas_free(a);
}

static void test_dense_climbs_fit_native_routing(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[36];
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO };
    unsigned degree[128] = {0}, i, nr;
    int j;
    unsigned char *bytes;
    size_t len;
    char err[256];
    load_synthetic_traversal_table();
    for (j = 0; j < 36; j++) {
        float x = -1800.0f + (j % 6) * 600.0f;
        float y = -1800.0f + (j / 6) * 600.0f;
        mkplat(&p[j], x, y, x + 300, y + 300, 128, "raised block");
    }
    CHECK(sh_aas_augment(a, p, 36, &o, &rep));
    CHECK(rep.anchors_reduced > 0);
    nr = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    for (i = 0; i < nr; i++) {
        const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
        unsigned from = sh_aas_get_u16(r, 6);
        CHECK(from < 128);
        if (from < 128) CHECK(++degree[from] <= 256);
    }
    for (j = 0; j < rep.platform_count; j++) {
        CHECK(rep.platforms[j].emitted);
        CHECK(rep.platforms[j].demons == 1);
        CHECK(reach_exists(a, 1, rep.platforms[j].area));
        CHECK(reach_exists(a, rep.platforms[j].area, 1));
    }
    bytes = sh_aas_write(a, &len);
    CHECK(bytes != NULL);
    if (bytes) {
        CHECK(sh_navmesh_validate_aas(bytes, len, err, sizeof err));
        HeapFree(GetProcessHeap(), 0, bytes);
    }
    sh_aas_free(a);
}

/* Fill area 1 with `count` outgoing shipped links (to itself), relinked. */
static void fill_area_one(sh_aas *a, unsigned count)
{
    unsigned original = sh_aas_count(a, SH_AAS_L_REACHABILITIES), first, i;
    CHECK(original <= count);
    CHECK(sh_aas_append(a, SH_AAS_L_REACHABILITIES, count - original, &first));
    for (i = 0; i < count; i++) {
        unsigned char *r = sh_aas_rec(a, SH_AAS_L_REACHABILITIES, i);
        sh_aas_put_u16(r, 6, 1); sh_aas_put_u16(r, 8, 1);
        sh_aas_put_i32(r, 32, i == count - 1 ? -1 : (int)i + 1);
        sh_aas_put_i32(r, 36, i == count - 1 ? -1 : (int)i + 1);
    }
    sh_aas_put_i32(sh_aas_rec(a, SH_AAS_L_AREAS, 1), 20, 0);
    sh_aas_put_i32(sh_aas_rec(a, SH_AAS_L_AREAS, 1), 24, 0);
}

static unsigned out_degree(sh_aas *a, unsigned area)
{
    unsigned i, n = sh_aas_count(a, SH_AAS_L_REACHABILITIES), d = 0;
    for (i = 0; i < n; i++)
        if (sh_aas_get_u16(sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i), 6) == area) d++;
    return d;
}

static int payload_validates(sh_aas *a)
{
    size_t len = 0;
    char err[256];
    unsigned char *bytes = sh_aas_write(a, &len);
    int ok;
    if (!bytes) return 0;
    err[0] = 0;
    ok = sh_navmesh_validate_aas(bytes, len, err, sizeof err);
    if (!ok) printf("  (validation: %s)\n", err);
    HeapFree(GetProcessHeap(), 0, bytes);
    return ok;
}

/* The floor the box stands on already has its 256 routes. The engine would
 * refuse the map over a 257th, so the box gives up its routes out of that
 * area and keeps the rest of its navigation, instead of the module losing
 * all of it. */
static void test_required_routes_over_native_limit_drop_the_box_routes(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO };
    printf("routes past 256 from one area are dropped from the box causing them\n");
    fill_area_one(a, 256);
    load_synthetic_traversal_table();
    mkplat(&p, -150, -150, 150, 150, 128, "raised block");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep));
    CHECK(!rep.reach_limit_exceeded);
    CHECK(rep.routes_dropped > 0 && rep.route_areas == 1);
    CHECK(rep.dropped_count == 1 && rep.dropped[0].source == 0 && rep.dropped[0].area == 1);
    CHECK(rep.dropped[0].routes == rep.routes_dropped);
    CHECK(out_degree(a, 1) == 256);
    CHECK(rep.platform_count == 1 && rep.platforms[0].emitted);
    CHECK(payload_validates(a));
    sh_aug_report_free(&rep);
    CHECK(rep.platforms == NULL);
    sh_aas_free(a);
}

/* With room for one more route, two boxes that each want one share it: just
 * enough is dropped, from one box, and the other keeps its route. */
static void test_route_drop_takes_just_enough(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO };
    int kept;
    printf("a route overflow drops just enough, one box at a time\n");
    fill_area_one(a, 255);
    load_synthetic_traversal_table();
    mkplat(&p[0], -900, -150, -600, 150, 128, "west block");
    mkplat(&p[1], 600, -150, 900, 150, 128, "east block");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep));
    CHECK(!rep.reach_limit_exceeded);
    CHECK(out_degree(a, 1) == 256);
    CHECK(rep.routes_dropped >= 1 && rep.dropped_count >= 1);
    kept = reach_exists(a, 1, rep.platforms[0].area) + reach_exists(a, 1, rep.platforms[1].area);
    CHECK(kept >= 1);
    CHECK(payload_validates(a));
    sh_aug_report_free(&rep);
    sh_aas_free(a);
}

/* The module's own data already passes 256 from one area: nothing generated
 * can make it fit, so that is the one route refusal left, with its area. */
static void test_shipped_routes_over_limit_refuse(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO };
    printf("a module whose own area passes 256 routes is refused by name\n");
    fill_area_one(a, 257);
    load_synthetic_traversal_table();
    mkplat(&p, -150, -150, 150, 150, 128, "raised block");
    CHECK(!sh_aas_augment(a, &p, 1, &o, &rep));
    CHECK(rep.reach_limit_exceeded);
    CHECK(rep.reach_limit_area == 1);
    CHECK(rep.routes_dropped == 0);
    sh_aug_report_free(&rep);
    sh_aas_free(a);
}

/* More boxes than the former 512 ceiling, all on one floor: the module bakes,
 * the floor sheds the routes it cannot hold, and the payload is valid. */
static void test_more_than_512_boxes_bake_and_shed_routes(void)
{
    static sh_aug_platform p[600];
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO };
    sh_aas *a = load_module();
    LARGE_INTEGER t0, t1, f;
    int i, emitted = 0;
    printf("six hundred boxes in one module bake; the shared floor sheds routes\n");
    load_synthetic_traversal_table();
    for (i = 0; i < 600; i++) {
        float x = -1900.0f + (float)(i % 25) * 150.0f;
        float y = -1900.0f + (float)(i / 25) * 150.0f;
        mkbox(&p[i], x, y, x + 100.0f, y + 100.0f, 16.0f + (float)(i % 2) * 6.0f, 0.0f, "cell");
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    CHECK(sh_aas_augment(a, p, 600, &o, &rep));
    QueryPerformanceCounter(&t1);
    printf("  600 boxes: %d pieces, %u areas, %u links, depth %u, %d routes dropped, %.0f ms\n",
           rep.platform_count, rep.areas_after, rep.reach_after, rep.depth_after,
           rep.routes_dropped, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
    CHECK(rep.platform_count > 512);
    for (i = 0; i < rep.platform_count; i++) if (rep.platforms[i].emitted) emitted++;
    CHECK(emitted > 512);
    CHECK(!rep.reach_limit_exceeded && rep.routes_dropped > 0);
    CHECK(out_degree(a, 1) <= 256);
    CHECK(rep.depth_exceeded == 0);
    CHECK(payload_validates(a));
    sh_aug_report_free(&rep);
    sh_aas_free(a);
}

/* A payload route budget below 256 thins every generated area to it, keeps
 * the shortest moves (each top still reaches its nearest neighbour), and the
 * budget sh_aug_route_budget_for picks brings the file under the cap. */
static void build_tower_grid(sh_aug_platform *p, int side)
{
    int i;
    for (i = 0; i < side * side; i++) {
        float x = -800.0f + (float)(i % side) * 160.0f;
        float y = -800.0f + (float)(i / side) * 160.0f;
        mkbox(&p[i], x, y, x + 96.0f, y + 96.0f, 128.0f, 0.0f, "tower");
    }
}

static void test_route_budget_thins_longest_routes_to_fit_the_file(void)
{
    static sh_aug_platform p[64];
    sh_aug_report full, thin, again;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO, 0 };
    sh_aas *a = load_module(), *b = load_module(), *c = load_module();
    unsigned na, i, worst = 0, shipped_max = 0;
    size_t size_full, size_thin, cap;
    int budget, next, areas_linked = 0, generated = 0, down = 0;
    printf("a route budget thins the longest routes and fits the file\n");
    load_synthetic_traversal_table();
    for (i = 0; i < sh_aas_count(a, SH_AAS_L_AREAS); i++)
        if (out_degree(a, i) > shipped_max) shipped_max = out_degree(a, i);
    build_tower_grid(p, 8);
    CHECK(sh_aas_augment(a, p, 64, &o, &full));
    na = sh_aas_count(a, SH_AAS_L_AREAS);
    for (i = full.areas_before; i < na; i++) if (out_degree(a, i) > worst) worst = out_degree(a, i);
    printf("  64 towers: %u links, busiest generated area %u routes\n", full.reach_after, worst);
    CHECK(worst > 8);
    size_full = sh_aas_serialized_size(a);

    /* Half the busiest area's routes, everywhere. */
    budget = (int)worst / 2;
    o.route_budget = budget;
    CHECK(sh_aas_augment(b, p, 64, &o, &thin));
    CHECK(!thin.reach_limit_exceeded);
    CHECK(thin.routes_dropped > full.routes_dropped);
    for (i = thin.areas_before; i < sh_aas_count(b, SH_AAS_L_AREAS); i++) {
        unsigned d = out_degree(b, i);
        CHECK_MSG(d <= (unsigned)budget, "every generated area within the budget");
        if (d) areas_linked++;
        generated++;
    }
    CHECK_MSG(areas_linked == generated, "no tower lost every route");
    /* The shortest move off a tower, its climb down to the floor it stands
     * on, outlasts the long leaps. */
    for (i = 0; i < 64; i++) {
        const sh_aug_platform_result *r = &thin.platforms[i];
        if (!r->emitted || !full.platforms[i].emitted) continue;
        if (!reach_exists(a, full.platforms[i].area, full.platforms[i].carrier)) continue;
        CHECK_MSG(reach_exists(b, r->area, r->carrier), "every tower keeps its way down");
        down++;
    }
    CHECK_MSG(down > 0, "the towers had a way down to keep");
    size_thin = sh_aas_serialized_size(b);
    CHECK(size_thin < size_full);
    CHECK(payload_validates(b));

    /* The chooser: a cap between the two sizes asks for a budget below 256
     * that, baked, fits. */
    cap = size_full - (size_full - size_thin) / 2;
    next = sh_aug_route_budget_for(a, &full, size_full, cap, 0);
    CHECK(next > 0 && next < (int)SH_AAS_MAX_AREA_REACHABILITIES);
    o.route_budget = next;
    CHECK(sh_aas_augment(c, p, 64, &o, &again));
    CHECK_MSG(sh_aas_serialized_size(c) <= cap, "the chosen budget fits the cap");
    /* Nothing to gain below a budget of one, nor under the cap. */
    CHECK(sh_aug_route_budget_for(a, &full, size_full, cap, 1) == 0);
    CHECK(sh_aug_route_budget_for(a, &full, size_full, size_full, 0) == 0);
    (void)shipped_max;
    sh_aug_report_free(&full); sh_aug_report_free(&thin); sh_aug_report_free(&again);
    sh_aas_free(a); sh_aas_free(b); sh_aas_free(c);
}

/* At the piece ceiling, faces are left out whole and named; the pieces that
 * fit are built. A face is never emitted partly cut. */
static void test_piece_ceiling_drops_whole_faces(void)
{
    static sh_aug_platform boxes[SH_AUG_MAX_PLATFORMS], out[8];
    int source[8], pieces[8];
    unsigned char buried[8], lost[SH_AUG_MAX_PLATFORMS];
    sh_nav_geometry_stats stats;
    int n, i;
    printf("the piece ceiling leaves whole faces out and counts them\n");
    for (i = 0; i < 5; i++)
        mkbox(&boxes[i], -1900.0f + i * 400.0f, 0, -1700.0f + i * 400.0f, 200, 64, 0, "cell");
    memset(&stats, 0, sizeof stats);
    memset(lost, 0, sizeof lost);
    stats.source_dropped = lost;
    n = sh_nav_geometry_build_budget(boxes, 5, NULL, 0, 24, 80, 0.7, 18,
                                     out, source, pieces, buried, 3, &stats);
    CHECK(n == 3);
    CHECK(stats.faces_dropped == 2);
    CHECK(lost[3] && lost[4] && !lost[0] && !lost[1] && !lost[2]);
    for (i = 0; i < n; i++) CHECK(!buried[i] && source[i] == i && pieces[i] == 1);
    /* The strict form still refuses rather than build an incomplete mesh. */
    CHECK(sh_nav_geometry_build(boxes, 5, 24, 80, 0.7, 18, out, source, pieces, buried, 3) == -1);
}

/* Decode independently into a flat membership set, bounded by the row's
 * encoded extent rather than by subsequent rows in the same lump. */
static int read_visibility(const sh_aas *a,unsigned area,unsigned char *bits,unsigned cap)
{
    unsigned n=sh_aas_count(a,SH_AAS_L_AREAS),at,end,pos=0,i;
    const unsigned char *ar=sh_aas_rec_const(a,SH_AAS_L_AREAS,area);
    if(n>cap)return 0;
    memset(bits,0,cap);at=sh_aas_get_u32(ar,16);
    end=sh_aas_count(a,SH_AAS_L_OBSTACLEPVS);
    for(i=0;i<n;i++) {
        unsigned other=sh_aas_get_u32(sh_aas_rec_const(a,SH_AAS_L_AREAS,i),16);
        if(other>at&&other<end)end=other;
    }
    while(pos<n) {
        unsigned b,k,skip;
        if(at>=end)return 0;
        b=*sh_aas_rec_const(a,SH_AAS_L_OBSTACLEPVS,at++);
        if(b<128) {
            for(k=0;k<7;k++,pos++) {
                if(pos<n)bits[pos]=(unsigned char)((b>>k)&1);
                else if(b&(1<<k))return 0;
            }
        } else {
            skip=(b&63)+1;
            if(b&64){if(at>=end)return 0;skip+=64u**sh_aas_rec_const(a,SH_AAS_L_OBSTACLEPVS,at++);}
            pos+=skip;
        }
    }
    return 1;
}

static void test_visibility_grows_with_connected_geometry(void)
{
    const int sizes[]={3,6,13};unsigned c;
    for(c=0;c<sizeof sizes/sizeof sizes[0];c++) {
        sh_aas *a=load_module();sh_aug_platform p[13];sh_aug_report rep;
        sh_aug_opts o={SH_AUG_FALL_NEVER,0,SH_AUG_TRAVERSAL_NEVER};
        unsigned char bits[16];unsigned i,j,n;
        for(i=0;i<(unsigned)sizes[c];i++)
            mkplat(&p[i],-1800+(float)i*48,-100,-1752+(float)i*48,100,
                   16.0f,"connected deck");
        CHECK(sh_aas_augment(a,p,sizes[c],&o,&rep));
        n=sh_aas_count(a,SH_AAS_L_AREAS);CHECK(n==(unsigned)sizes[c]+2);
        for(i=1;i<n;i++) {
            CHECK(read_visibility(a,i,bits,sizeof bits));
            for(j=1;j<n;j++)CHECK(bits[j]);
        }
        sh_aas_free(a);
    }
}

static void test_visibility_extends_sparse_native_rows(void)
{
    sh_aas *a=load_module();sh_aug_platform p;sh_aug_report rep;
    sh_aug_opts o={SH_AUG_FALL_NEVER,0,SH_AUG_TRAVERSAL_NEVER};
    unsigned char bits[72];unsigned first,i;
    CHECK(sh_aas_append(a,SH_AAS_L_AREAS,68,&first));
    CHECK(sh_aas_append(a,SH_AAS_L_AREABOUNDS,68,&first));
    /* Seventy original areas: literal bits0..6 followed by 63 zero bits. */
    *sh_aas_rec(a,SH_AAS_L_OBSTACLEPVS,0)=2;
    *sh_aas_rec(a,SH_AAS_L_OBSTACLEPVS,1)=0xbe;
    mkplat(&p,-100,-100,100,100,16,"deck");
    CHECK(sh_aas_augment(a,&p,1,&o,&rep));
    CHECK(sh_aas_count(a,SH_AAS_L_AREAS)==71);
    CHECK(read_visibility(a,1,bits,sizeof bits));CHECK(bits[1]&&bits[70]);
    for(i=2;i<70;i++)CHECK(!bits[i]);
    CHECK(read_visibility(a,70,bits,sizeof bits));CHECK(bits[1]&&bits[70]);
    sh_aas_free(a);
}

static void test_truncated_visibility_refuses_bake(void)
{
    sh_aas *a=load_module();sh_aug_platform p;sh_aug_report rep;
    sh_aug_opts o={SH_AUG_FALL_NEVER,0,SH_AUG_TRAVERSAL_NEVER};
    CHECK(sh_aas_truncate(a,SH_AAS_L_OBSTACLEPVS,1));
    *sh_aas_rec(a,SH_AAS_L_OBSTACLEPVS,0)=0xc0; /* missing extended run byte */
    mkplat(&p,-100,-100,100,100,16,"deck");
    CHECK(!sh_aas_augment(a,&p,1,&o,&rep));
    sh_aas_free(a);
}

static void test_rotated_ramps_join_native_floor(void)
{
    const float radii[]={24,48,64};const double angles[]={-40,-25,25,40};
    const double yaws[]={0,37,90,179};unsigned r,g,y;
    for(r=0;r<3;r++)for(g=0;g<4;g++)for(y=0;y<4;y++) {
        sh_aas *a=load_module();sh_aug_platform p;sh_aug_report rep;
        sh_aug_opts o={SH_AUG_FALL_NEVER,1,SH_AUG_TRAVERSAL_NEVER};
        const float xy[4][2]={{-2000,2000},{2000,2000},{2000,-2000},{-2000,-2000}};
        double angle=angles[g]*3.14159265358979323846/180.0;
        double yaw=yaws[y]*3.14159265358979323846/180.0,cs=cos(yaw),sn=sin(yaw);
        unsigned i,nr;int down=0,up=0;
        sh_aas_put_u32(sh_aas_rec(a,SH_AAS_L_AREAS,1),0,8);
        sh_aas_put_u16(sh_aas_rec(a,SH_AAS_L_AREAS,1),4,10);
        for(i=0;i<4;i++) {
            unsigned char *v=sh_aas_rec(a,SH_AAS_L_VERTICES,i);
            sh_aas_put_f32(v,0,xy[i][0]);sh_aas_put_f32(v,4,xy[i][1]);
        }
        sh_aas_put_f32(sh_aas_rec(a,SH_AAS_L_PLANES,1),12,1000);
        sh_aas_set_setting_f32(a,SET_WORDS+0,-radii[r]);
        sh_aas_set_setting_f32(a,SET_WORDS+4,-radii[r]);
        sh_aas_set_setting_f32(a,SET_WORDS+12,radii[r]);
        sh_aas_set_setting_f32(a,SET_WORDS+16,radii[r]);
        mkplat_tilted(&p,angles[g],320,512,fabs(sin(angle))*160,"floor ramp");p.depth=32;
        for(i=0;i<4;i++){float x=p.c[i][0],yy=p.c[i][1];p.c[i][0]=(float)(cs*x-sn*yy);p.c[i][1]=(float)(sn*x+cs*yy);}
        {float x=p.n[0],yy=p.n[1];p.n[0]=(float)(cs*x-sn*yy);p.n[1]=(float)(sn*x+cs*yy);}
        CHECK(sh_aas_augment(a,&p,1,&o,&rep));
        nr=sh_aas_count(a,SH_AAS_L_REACHABILITIES);
        for(i=0;i<nr;i++) {
            const unsigned char *reach=sh_aas_rec_const(a,SH_AAS_L_REACHABILITIES,i);
            unsigned from=sh_aas_get_u16(reach,6),to=sh_aas_get_u16(reach,8);
            unsigned floor_end=rep.areas_before+(unsigned)rep.floor_pieces_added;
            if(sh_aas_get_u32(reach,0)!=0x20)continue;
            /* The floor under the ramp is cut into pieces: any of them is the
             * native floor the ramp joins. */
            if(from>=floor_end&&to>0&&to<floor_end) {
                double start[2]={sh_aas_get_i16(reach,12),sh_aas_get_i16(reach,14)};
                double d[2]={sh_aas_get_i16(reach,18)-start[0],sh_aas_get_i16(reach,20)-start[1]},lo,hi;
                CHECK(floor_line_interval(a,from,start,d,&lo,&hi));
                CHECK(!wall_at_floor_point(a,from,start[0]+hi*d[0],start[1]+hi*d[1]));
                down++;
            }
            if(from>0&&from<floor_end&&to>=floor_end)up++;
        }
        if(!down||!up)printf("native floor join missing: radius %g slope %g yaw %g pieces %d\n",radii[r],angles[g],yaws[y],rep.platform_count);
        CHECK(down>0);CHECK(up>0);
        sh_aas_free(a);
    }
}

/* Independent synthetic declarations exercise the full class multiplicity.
 * No installed paths or animation data are required. */
static void load_all_test_monsters(void)
{
    char text[16384];size_t used=0;int m,d;
    const char *families[]={"LEDGE_UP_128","LEDGE_DOWN_128","LEAP_ACROSS_256"};
    used+=(size_t)sprintf(text+used,"{\nedit = {\ntable = {\n");
    for(m=0;m<sh_trav_monster_count();m++) {
        used+=(size_t)sprintf(text+used,"table[%d] = {\nmonster = \"%s\";\ntraversal = {\n",
                             m,sh_trav_monster_at(m)->decl_name);
        for(d=0;d<3;d++)used+=(size_t)sprintf(text+used,
            "traversal[%d] = {\ntype = \"%s\";\npath = \"test/monster%d/move%d\";\noffset = {\nx = -32;\n}\n}\n",
            d,families[d],m,d);
        used+=(size_t)sprintf(text+used,"}\n}\n");
    }
    used+=(size_t)sprintf(text+used,"}\n}\n}\n");
    CHECK(sh_trav_test_parse(text,used));
}

static void test_traversal_collection_grows_without_losing_demon_routes(void)
{
    sh_aas *a=load_module();sh_aug_platform p[24];sh_aug_report rep;
    sh_aug_opts opts={SH_AUG_FALL_AUTO,1,SH_AUG_TRAVERSAL_AUTO};
    unsigned i,nr;int j,m;unsigned degree[128]={0};
    load_all_test_monsters();
    for(j=0;j<24;j++)mkbox(&p[j],-1800+(j%5)*700,-1800+(j/5)*700,
        -1544+(j%5)*700,-1544+(j/5)*700,128,0,"dense box");
    CHECK(sh_aas_augment(a,p,24,&opts,&rep));
    nr=sh_aas_count(a,SH_AAS_L_REACHABILITIES);
    CHECK(nr>1024);CHECK(!rep.links_truncated);
    for(i=0;i<nr;i++) {
        const unsigned char *r=sh_aas_rec_const(a,SH_AAS_L_REACHABILITIES,i);
        unsigned from=sh_aas_get_u16(r,6);
        CHECK(from<128);if(from<128)CHECK(++degree[from]<=256);
    }
    for(j=0;j<24;j++) {
        unsigned incoming=0,outgoing=0;
        CHECK(rep.platforms[j].emitted);
        for(i=0;i<nr;i++) {
            const unsigned char *r=sh_aas_rec_const(a,SH_AAS_L_REACHABILITIES,i);
            for(m=0;m<sh_trav_monster_count();m++)if(sh_aas_get_u32(r,0)==sh_trav_monster_at(m)->travel_flags) {
                if(sh_aas_get_u16(r,6)==1&&sh_aas_get_u16(r,8)==rep.platforms[j].area)incoming|=1u<<m;
                if(sh_aas_get_u16(r,8)==1&&sh_aas_get_u16(r,6)==rep.platforms[j].area)outgoing|=1u<<m;
            }
        }
        CHECK(incoming==511);CHECK(outgoing==511);
    }
    {
        size_t len;char err[192];unsigned char *bytes=sh_aas_write(a,&len);
        CHECK(bytes!=NULL);
        if(bytes){CHECK_MSG(sh_navmesh_validate_aas(bytes,len,err,sizeof err),err);HeapFree(GetProcessHeap(),0,bytes);}
    }
    sh_aas_free(a);sh_trav_test_reset();
}

static void test_climb_floor_endpoints_clear_rotated_solids(void)
{
    const double slopes[]={0,25,40,90};const double yaws[]={0,37,90};
    unsigned s,y;
    load_all_test_monsters();
    for(s=0;s<4;s++)for(y=0;y<3;y++) {
        sh_aas *a=load_module();sh_aug_platform p;sh_aug_report rep;
        sh_aug_opts opts={SH_AUG_FALL_AUTO,1,SH_AUG_TRAVERSAL_AUTO};
        double angle=slopes[s]*3.14159265358979323846/180.0;
        double yaw=yaws[y]*3.14159265358979323846/180.0,cs=cos(yaw),sn=sin(yaw);
        int i,up=0,down=0;unsigned r,nr;
        sh_aas_put_f32(sh_aas_rec(a,SH_AAS_L_PLANES,1),12,1000);
        sh_aas_set_setting_f32(a,SET_WORDS+0,-64);sh_aas_set_setting_f32(a,SET_WORDS+4,-64);
        sh_aas_set_setting_f32(a,SET_WORDS+12,64);sh_aas_set_setting_f32(a,SET_WORDS+16,64);
        if(slopes[s]==90) {
            mkbox(&p,-128,-256,128,256,512,0,"sideways solid");
            for(i=0;i<4;i++){float x=p.c[i][0];p.c[i][0]=p.c[i][2]-256;p.c[i][2]=-x+128;}
            p.n[0]=1;p.n[2]=0;p.face=0;
        } else {
            mkplat_tilted(&p,slopes[s],512,512,slopes[s]?256*sin(angle):128,"ramp");p.depth=128;
        }
        for(i=0;i<4;i++){float x=p.c[i][0],yy=p.c[i][1];p.c[i][0]=(float)(cs*x-sn*yy);p.c[i][1]=(float)(sn*x+cs*yy);}
        {float x=p.n[0],yy=p.n[1];p.n[0]=(float)(cs*x-sn*yy);p.n[1]=(float)(sn*x+cs*yy);}
        CHECK(sh_aas_augment(a,&p,1,&opts,&rep));
        nr=sh_aas_count(a,SH_AAS_L_REACHABILITIES);
        for(r=0;r<nr;r++) {
            const unsigned char *rr=sh_aas_rec_const(a,SH_AAS_L_REACHABILITIES,r);
            unsigned from=sh_aas_get_u16(rr,6),to=sh_aas_get_u16(rr,8),off;
            double point[3];
            if(!(sh_aas_get_u32(rr,0)&0xffff0000)||(from!=1&&to!=1))continue;
            off=from==1?12:18;
            for(i=0;i<3;i++)point[i]=sh_aas_get_i16(rr,off+2*i);
            CHECK(sh_nav_geometry_path_clear(&p,1,-1,-1,point,point,64,80));
            /* The level controls allow an independent square-body check. */
            if(slopes[s]==0&&yaws[y]==0)CHECK(fabs(point[0])>=320||fabs(point[1])>=320);
            if(sh_aas_get_u32(rr,0)==sh_trav_monster_at(5)->travel_flags) {
                if(from==1)up++;else down++;
            }
        }
        CHECK(up>0);CHECK(down>0);
        sh_aas_free(a);
    }
    sh_trav_test_reset();
}

/* ==================================================================== */
/* a module floor split into slabs, as the Grid Room's is                */
/* ==================================================================== */

/* Floor slabs spanning +/-2000 in y at z = 0, side by side in x, split at
 * `seams`. Areas 1..n carry the shipped floor flag 0x8 and a clockwise edge
 * loop, so the augmenter reads their polygons the way it reads a real module.
 * The BSP is the band -9.6 < z < 200, then one x-plane per seam. */
static sh_aas *load_slabs(const float *seams, int nseams)
{
    unsigned counts[22];
    size_t total = 394, off, len;
    unsigned i, k, ns = (unsigned)nseams, na = ns + 1;
    unsigned char *p;
    size_t o[22];
    char err[192];
    sh_aas *a;
    float xs[16];

    if (ns > 14) return NULL;
    xs[0] = -2000.0f;
    for (i = 0; i < ns; i++) xs[i + 1] = seams[i];
    xs[ns + 1] = 2000.0f;
    memset(counts, 0, sizeof counts);
    counts[SH_AAS_L_PLANES] = 2 + ns;
    counts[SH_AAS_L_VERTICES] = 4 * na;
    counts[SH_AAS_L_EDGES] = 4 * na;
    counts[SH_AAS_L_EDGEINDEX] = 4 * na;
    counts[SH_AAS_L_AREAS] = 1 + na;
    counts[SH_AAS_L_NODES] = 3 + ns;
    counts[SH_AAS_L_CLUSTERS] = 1;
    counts[SH_AAS_L_OBSTACLEPVS] = 2 * (1 + na);
    counts[SH_AAS_L_TREES] = 1;
    counts[SH_AAS_L_AREABOUNDS] = 1 + na;
    for (i = 0; i < 22; i++) total += 4 + (size_t)counts[i] * REC[i];
    p = (unsigned char *)calloc(1, total);
    if (!p) return NULL;
    memcpy(p, "2SAA", 4);
    p[4] = 3; p[5] = 29;
    putf(p + SETW(0), -24.0f); putf(p + SETW(1), -24.0f); putf(p + SETW(2), 0.0f);
    putf(p + SETW(3),  24.0f); putf(p + SETW(4),  24.0f); putf(p + SETW(5), 80.0f);
    putf(p + SETW(12), 18.0f);
    putf(p + SETW(15), 0.0f);
    putf(p + SETW(16), 0.7f);
    put32(p + SETW(35), 100);           /* tt_barrierJump */
    put32(p + SETW(36), 100);
    off = 394;
    for (i = 0; i < 22; i++) {
        put32(p + off, counts[i]);
        off += 4; o[i] = off;
        off += (size_t)counts[i] * REC[i];
    }
    putf(p + o[SH_AAS_L_PLANES] + 8, 1.0f);
    putf(p + o[SH_AAS_L_PLANES] + 12, 9.6f);
    putf(p + o[SH_AAS_L_PLANES] + 16 + 8, -1.0f);
    putf(p + o[SH_AAS_L_PLANES] + 16 + 12, 200.0f);
    for (i = 0; i < ns; i++) {              /* x - seam > 0 is the far slab */
        putf(p + o[SH_AAS_L_PLANES] + 32 + i * 16, 1.0f);
        putf(p + o[SH_AAS_L_PLANES] + 32 + i * 16 + 12, -seams[i]);
    }
    for (k = 0; k < na; k++) {
        float x0 = xs[k], x1 = xs[k + 1];
        float cx[4], cy[4];
        unsigned char *ar = p + o[SH_AAS_L_AREAS] + (k + 1) * 44;
        unsigned char *b = p + o[SH_AAS_L_AREABOUNDS] + (k + 1) * 12;
        cx[0] = x0; cy[0] = 2000.0f; cx[1] = x1; cy[1] = 2000.0f;
        cx[2] = x1; cy[2] = -2000.0f; cx[3] = x0; cy[3] = -2000.0f;
        for (i = 0; i < 4; i++) {
            unsigned v = k * 4 + i;
            putf(p + o[SH_AAS_L_VERTICES] + v * 12, cx[i]);
            putf(p + o[SH_AAS_L_VERTICES] + v * 12 + 4, cy[i]);
            put32(p + o[SH_AAS_L_EDGES] + v * 12, v);
            put32(p + o[SH_AAS_L_EDGES] + v * 12 + 4, k * 4 + (i + 1) % 4);
            put32(p + o[SH_AAS_L_EDGES] + v * 12 + 8, 0x0C01);
            put32(p + o[SH_AAS_L_EDGEINDEX] + v * 4, v);
        }
        put32(ar + 0x00, 8);
        put16(ar + 0x04, 0x000A);
        put16(ar + 0x06, 4);
        put32(ar + 0x08, k * 4);
        put16(ar + 0x0E, (unsigned)k);
        put32(ar + 0x10, 2 * (k + 1));
        put32(ar + 0x14, 0xFFFFFFFFu);
        put32(ar + 0x18, 0xFFFFFFFFu);
        put16(b + 0, (unsigned)(short)x0); put16(b + 2, (unsigned)(short)-2000);
        put16(b + 6, (unsigned)(short)x1); put16(b + 8, 2000);
    }
    put32(p + o[SH_AAS_L_AREAS] + 0x14, 0xFFFFFFFFu);
    put32(p + o[SH_AAS_L_AREAS] + 0x18, 0xFFFFFFFFu);
    put32(p + o[SH_AAS_L_CLUSTERS], na); put32(p + o[SH_AAS_L_CLUSTERS] + 4, na);
    /* every area sees every area: one literal row of seven set bits each */
    for (i = 0; i < 1 + na; i++) {
        p[o[SH_AAS_L_OBSTACLEPVS] + 2 * i] = 0x7F;
        p[o[SH_AAS_L_OBSTACLEPVS] + 2 * i + 1] = 0x7F;
    }
    {
        unsigned char *n = p + o[SH_AAS_L_NODES];
        put32(n + 16 + 0, 0); put32(n + 16 + 8, 2); put32(n + 16 + 12, 0);
        put32(n + 32 + 0, 1); put32(n + 32 + 8, ns ? 3 : 0xFFFFFFFFu); put32(n + 32 + 12, 0);
        for (i = 0; i < ns; i++) {          /* node 3+i tests seam i */
            unsigned char *q = n + (3 + i) * 16;
            put32(q + 0, 2 + i);
            put32(q + 8, i + 1 < ns ? 4 + i : (unsigned)-(int)(i + 2));
            put32(q + 12, (unsigned)-(int)(i + 1));
        }
    }
    putf(p + o[SH_AAS_L_TREES] + 8, 1.0f);
    put32(p + o[SH_AAS_L_TREES] + 12, 1);
    put32(p + o[SH_AAS_L_TREES] + 16, 1);
    put32(p + o[SH_AAS_L_TREES] + 20, 1 + na);
    len = total; err[0] = 0;
    a = sh_aas_parse(p, len, err, sizeof err);
    if (!a) printf("  (slab fixture did not parse: %s)\n", err);
    free(p);
    return a;
}

/* A box of the given size centred at (cx, cy), yawed by deg, top at `top`,
 * standing on z = bottom. */
static void mkbox_yawed(sh_aug_platform *p, double cx, double cy, double w, double d,
                        double deg, float top, float bottom, const char *name)
{
    double r = deg * 3.14159265358979323846 / 180.0, cs = cos(r), sn = sin(r);
    double lx[4] = { -w / 2,  w / 2,  w / 2, -w / 2 };
    double ly[4] = {  d / 2,  d / 2, -d / 2, -d / 2 };
    int i;
    memset(p, 0, sizeof *p);
    for (i = 0; i < 4; i++) {
        p->c[i][0] = (float)(cx + cs * lx[i] - sn * ly[i]);
        p->c[i][1] = (float)(cy + sn * lx[i] + cs * ly[i]);
        p->c[i][2] = top;
    }
    p->n[2] = 1.0f;
    p->face = 4;
    p->depth = top - bottom;
    _snprintf_s(p->name, sizeof p->name, _TRUNCATE, "%s", name);
}

/* Traversal reachabilities that rise from a floor area (1..floor_end-1) onto
 * `top`, and the lowest/highest x their floor start takes. */
static int floor_climbs_onto(sh_aas *a, int top, unsigned floor_end, int *from_area_mask,
                             double *min_x, double *max_x)
{
    unsigned i, n = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    int count = 0;
    if (from_area_mask) *from_area_mask = 0;
    if (min_x) *min_x = 1e9;
    if (max_x) *max_x = -1e9;
    for (i = 0; i < n; i++) {
        const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
        unsigned from = sh_aas_get_u16(r, 6), to = sh_aas_get_u16(r, 8);
        double x;
        if (!(sh_aas_get_u32(r, 0) >> 16) || (int)to != top || from == 0 || from >= floor_end) continue;
        count++;
        if (from_area_mask && from < 31) *from_area_mask |= 1 << from;
        x = sh_aas_get_i16(r, 12);
        if (min_x && x < *min_x) *min_x = x;
        if (max_x && x > *max_x) *max_x = x;
    }
    return count;
}

/* The Grid Room's door-axis strip ends at x = +/-168, three units past the
 * sides of a 330-wide step centred on it. The run-up of an east or west climb
 * crosses that seam; it must land on the neighbouring slab, not be dropped. */
static void test_a_climb_run_up_may_cross_a_floor_seam(void)
{
    static const float seams[2] = { -168.0f, 168.0f };
    sh_aas *a = load_slabs(seams, 2);
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO, 0 };
    int mask = 0, n;
    double lo, hi;
    printf("a climb whose run-up crosses a floor seam lands on the next slab\n");
    if (!a) { CHECK(0); return; }
    CHECK_MSG(sh_aas_point_area(a, 0, 0, 2) == 2 && sh_aas_point_area(a, 200, 0, 2) == 3 &&
              sh_aas_point_area(a, -200, 0, 2) == 1, "the slab fixture resolves by seam");
    load_synthetic_traversal_table();
    mkbox_yawed(&p, 0, 0, 330, 290, 0, 64.0f, 0.0f, "step");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK(rep.platforms[0].emitted);
    /* The floor under the step is cut into pieces, so the slabs' areas are
     * the shipped three plus those pieces. */
    n = floor_climbs_onto(a, rep.platforms[0].area, rep.areas_before + (unsigned)rep.floor_pieces_added,
                          &mask, &lo, &hi);
    CHECK_MSG(n > 0, "the step is climbable from the floor");
    CHECK_MSG(hi > 168.0, "the east face is climbed from the east slab, past the seam");
    CHECK_MSG(lo < -168.0, "the west face is climbed from the west slab, past the seam");
    {
        unsigned i, nr = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
        int bad = 0;
        for (i = 0; i < nr; i++) {
            const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
            unsigned from = sh_aas_get_u16(r, 6), to = sh_aas_get_u16(r, 8);
            if (!(sh_aas_get_u32(r, 0) >> 16)) continue;
            if (sh_aas_point_area(a, sh_aas_get_i16(r, 12), sh_aas_get_i16(r, 14),
                                  (float)(sh_aas_get_i16(r, 16) + 2)) != (int)from) bad++;
            if (sh_aas_point_area(a, sh_aas_get_i16(r, 18), sh_aas_get_i16(r, 20),
                                  (float)(sh_aas_get_i16(r, 22) + 2)) != (int)to) bad++;
        }
        CHECK_MSG(bad == 0, "every climb endpoint resolves to the area it names");
    }
    /* The per-face report: all four faces of the lone step carry a climb. */
    CHECK(rep.faces != NULL);
    if (rep.faces) {
        int e;
        for (e = 0; e < 4; e++) {
            CHECK_MSG(rep.faces[e].climbs > 0 && rep.faces[e].demons & 1u,
                      "every face of a lone step is climbed, and the report says by whom");
        }
    }
    sh_aug_report_free(&rep);
    sh_aas_free(a);
}
/* Climbs along a 1000-unit face come from several places, at least 128 units
 * apart, and a 150-unit face keeps one: a demon beside a long step need not
 * walk to its middle. */
static void test_a_long_face_offers_climbs_along_its_length(void)
{
    sh_aas *a = load_slabs(NULL, 0);
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO, 0 };
    unsigned i, nr;
    double north[32], east[32];
    int nn = 0, ne = 0, j, k, spaced = 1;
    printf("a long face carries climbs along its length, a short one keeps one\n");
    if (!a) { CHECK(0); return; }
    load_synthetic_traversal_table();
    mkbox_yawed(&p, 0, 0, 1000, 150, 0, 64.0f, 0.0f, "long step");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    nr = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    for (i = 0; i < nr; i++) {
        const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
        double x = sh_aas_get_i16(r, 12), y = sh_aas_get_i16(r, 14);
        if (!(sh_aas_get_u32(r, 0) >> 16) ||
            sh_aas_get_u16(r, 6) >= rep.areas_before + (unsigned)rep.floor_pieces_added) continue;
        if (y > 75.0 && nn < 32) north[nn++] = x;
        if (x > 500.0 && ne < 32) east[ne++] = y;
    }
    CHECK_MSG(nn >= 4, "the 1000-unit north face is climbable from several places");
    CHECK_MSG(ne == 1, "the 150-unit east face keeps a single climb");
    for (j = 0; j < nn; j++) for (k = j + 1; k < nn; k++)
        if (fabs(north[j] - north[k]) < 128.0) spaced = 0;
    CHECK_MSG(spaced, "climbs on one face stand at least 128 units apart");
    {
        size_t len; char err[192]; unsigned char *bytes = sh_aas_write(a, &len);
        CHECK(bytes != NULL);
        if (bytes) { CHECK_MSG(sh_navmesh_validate_aas(bytes, len, err, sizeof err), err);
                     HeapFree(GetProcessHeap(), 0, bytes); }
    }
    sh_aug_report_free(&rep);
    sh_aas_free(a);
}
/* Distance from (x, y) to the nearest edge of a four-corner footprint. */
static double footprint_edge_distance(const sh_aug_platform *p, double x, double y)
{
    double best = 1e30;
    int i;
    for (i = 0; i < 4; i++) {
        const float *a = p->c[i], *b = p->c[(i + 1) % 4];
        double ex = b[0] - a[0], ey = b[1] - a[1], len2 = ex * ex + ey * ey, t, dx, dy;
        t = ((x - a[0]) * ex + (y - a[1]) * ey) / len2;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        dx = x - (a[0] + t * ex); dy = y - (a[1] + t * ey);
        if (sqrt(dx * dx + dy * dy) < best) best = sqrt(dx * dx + dy * dy);
    }
    return best;
}

/* A step whose top is split by the next step standing on it has edges along
 * that cut, deep inside its own box. A floor climb must never arrive on one:
 * its run-up would be pushed out through the box's own face and stretched
 * across its top. Every floor climb lands within the body's clearance of a
 * real face, and no two are identical. */
static void test_no_floor_climb_lands_on_a_cut_inside_its_own_box(void)
{
    sh_aas *a = load_slabs(NULL, 0);
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO, 0 };
    unsigned i, nr;
    int k, floor_climbs = 0, deep = 0;
    printf("no floor climb lands on a cut line inside its own box\n");
    if (!a) { CHECK(0); return; }
    load_synthetic_traversal_table();
    /* Steps 1 and 2 of a user's fanned staircase, module coordinates. */
    {
        static const float s1[4][2] = { { 162.321f, 1581.123f }, { -165.658f, 1584.897f },
                                        { -162.321f, 1874.878f }, { 165.657f, 1871.104f } };
        static const float s2[4][2] = { { -75.754f, 1269.929f }, { -325.081f, 1483.049f },
                                        { -136.652f, 1703.491f }, { 112.675f, 1490.371f } };
        mkbox_yawed(&p[0], 0, 0, 10, 10, 0, 64.25f, 0.25f, "step 1");
        mkbox_yawed(&p[1], 0, 0, 10, 10, 0, 127.024f, 63.024f, "step 2");
        for (k = 0; k < 4; k++) {
            p[0].c[k][0] = s1[k][0]; p[0].c[k][1] = s1[k][1];
            p[1].c[k][0] = s2[k][0]; p[1].c[k][1] = s2[k][1];
        }
    }
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    {
        int idx[8], n = pieces_of(&rep, 0, idx, 8);
        CHECK_MSG(n >= 2, "step 1's top is cut around step 2");
    }
    nr = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    for (i = 0; i < nr; i++) {
        const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
        unsigned from = sh_aas_get_u16(r, 6), to = sh_aas_get_u16(r, 8);
        int on_step1 = 0;
        if (!(sh_aas_get_u32(r, 0) >> 16) || !from ||
            from >= rep.areas_before + (unsigned)rep.floor_pieces_added) continue;
        for (k = 0; k < rep.platform_count; k++)
            if (rep.platforms[k].source == 0 && rep.platforms[k].area == (int)to) on_step1 = 1;
        if (!on_step1) continue;
        floor_climbs++;
        if (footprint_edge_distance(&p[0], sh_aas_get_i16(r, 18), sh_aas_get_i16(r, 20)) >
            sqrt(2.0) * 24.0 + 10.0) deep++;
    }
    CHECK_MSG(floor_climbs > 0, "step 1 is still climbable from the floor");
    CHECK_MSG(deep == 0, "every floor climb onto step 1 arrives at one of its real faces");
    /* Its faces say which carry a climb, and why the rest do not. */
    CHECK(rep.faces != NULL);
    if (rep.faces) {
        int e, climbed = 0, explained = 0;
        for (e = 0; e < 4; e++) {
            if (rep.faces[e].climbs) climbed++;
            else if (rep.faces[e].reasons[SH_AUG_FACE_BLOCKED] || rep.faces[e].reasons[SH_AUG_FACE_INSIDE])
                explained++;
        }
        CHECK_MSG(climbed >= 2, "step 1 keeps climbs on its open faces");
        CHECK_MSG(climbed + explained == 4, "a face without a climb names the step above or the cut");
    }
    {
        int dup = 0;
        unsigned j;
        for (i = 0; i < nr; i++) for (j = i + 1; j < nr; j++) {
            const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
            const unsigned char *q = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, j);
            if (!(sh_aas_get_u32(r, 0) >> 16) || sh_aas_get_u32(r, 0) != sh_aas_get_u32(q, 0)) continue;
            if (!memcmp(r + 6, q + 6, 4) && !memcmp(r + 12, q + 12, 12)) dup++;
        }
        CHECK_MSG(dup == 0, "and no climb is written twice");
    }
    sh_aug_report_free(&rep);
    sh_aas_free(a);
}
/* The area polygon of `area`, as the augmenter wrote it. */
static int area_polygon(sh_aas *a, unsigned area, double out[][2], int cap)
{
    const unsigned char *ar = sh_aas_rec_const(a, SH_AAS_L_AREAS, area);
    unsigned n, first, i;
    if (!ar) return 0;
    n = sh_aas_get_u16(ar, 6); first = sh_aas_get_u32(ar, 8);
    if ((int)n > cap) return 0;
    for (i = 0; i < n; i++) {
        int ei = sh_aas_get_i32(sh_aas_rec_const(a, SH_AAS_L_EDGEINDEX, first + i), 0);
        const unsigned char *e = sh_aas_rec_const(a, SH_AAS_L_EDGES, (unsigned)abs(ei));
        const unsigned char *v = sh_aas_rec_const(a, SH_AAS_L_VERTICES, sh_aas_get_u32(e, ei < 0 ? 4 : 0));
        out[i][0] = sh_aas_get_f32(v, 0); out[i][1] = sh_aas_get_f32(v, 4);
    }
    return (int)n;
}

static int point_in_footprint(const sh_aug_platform *p, double x, double y, double grow)
{
    int i;
    for (i = 0; i < 4; i++) {
        const float *a = p->c[i], *b = p->c[(i + 1) % 4];
        double ex = b[0] - a[0], ey = b[1] - a[1], len = sqrt(ex * ex + ey * ey);
        /* clockwise: inside has (ey, -ex).(p - a) >= 0 */
        if ((ey * (x - a[0]) - ex * (y - a[1])) / len < -grow) return 0;
    }
    return 1;
}

/* The module floor under the first two steps of the user's staircase is cut
 * around them, as a box top is cut around a solid standing on it: no floor
 * area reaches under either step (step 2 hangs at 63, below the 80-unit
 * agent), the ground under them still resolves to a floor area, every floor
 * piece resolves at its centre and walks to the others, and the step keeps
 * its climbs. A box the agent walks under leaves the floor whole. */
static void test_the_floor_is_cut_around_the_boxes_on_it(void)
{
    static const float seams[2] = { -168.0f, 168.0f };
    static const float s1[4][2] = { { 162.321f, 1581.123f }, { -165.658f, 1584.897f },
                                    { -162.321f, 1874.878f }, { 165.657f, 1871.104f } };
    static const float s2[4][2] = { { -75.754f, 1269.929f }, { -325.081f, 1483.049f },
                                    { -136.652f, 1703.491f }, { 112.675f, 1490.371f } };
    sh_aas *a = load_slabs(seams, 2);
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o = { SH_AUG_FALL_AUTO, 1, SH_AUG_TRAVERSAL_AUTO, 0 };
    unsigned floor_end, ar, i, nr;
    int k, inside = 0, unresolved = 0, reached[64] = {0}, grew = 1;
    printf("the module floor is cut around the boxes standing on it\n");
    if (!a) { CHECK(0); return; }
    load_synthetic_traversal_table();
    mkbox_yawed(&p[0], 0, 0, 10, 10, 0, 64.25f, 0.25f, "step 1");
    mkbox_yawed(&p[1], 0, 0, 10, 10, 0, 127.024f, 63.024f, "step 2");
    for (k = 0; k < 4; k++) {
        p[0].c[k][0] = s1[k][0]; p[0].c[k][1] = s1[k][1];
        p[1].c[k][0] = s2[k][0]; p[1].c[k][1] = s2[k][1];
    }
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK_MSG(rep.floors_cut >= 2, "the slabs under the steps are cut");
    CHECK_MSG(rep.floor_pieces_added > 0, "into pieces");
    floor_end = rep.areas_before + (unsigned)rep.floor_pieces_added;
    CHECK(floor_end < 64);
    for (ar = 1; ar < floor_end; ar++) {
        double poly[64][2], cx = 0, cy = 0;
        int n = area_polygon(a, ar, poly, 64), v;
        CHECK(n >= 3);
        for (v = 0; v < n; v++) {
            cx += poly[v][0] / n; cy += poly[v][1] / n;
            /* A vertex may touch the cut, never stand inside either step. */
            if (point_in_footprint(&p[0], poly[v][0], poly[v][1], -1.0) ||
                point_in_footprint(&p[1], poly[v][0], poly[v][1], -1.0)) inside++;
        }
        if (point_in_footprint(&p[0], cx, cy, 0) || point_in_footprint(&p[1], cx, cy, 0)) inside++;
        if (sh_aas_point_area(a, (float)cx, (float)cy, 2.0f) != (int)ar) unresolved++;
    }
    CHECK_MSG(inside == 0, "no floor area reaches under a step");
    CHECK_MSG(unresolved == 0, "every floor piece resolves at its own centre");
    k = sh_aas_point_area(a, 0.0f, 1728.0f, 2.0f);
    CHECK_MSG(k > 0 && k < (int)rep.areas_before, "the ground under a step still resolves to its floor");
    /* Walk connectivity across the cut floor. */
    nr = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    reached[1] = 1;
    while (grew) {
        grew = 0;
        for (i = 0; i < nr; i++) {
            const unsigned char *r = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, i);
            unsigned f = sh_aas_get_u16(r, 6), t = sh_aas_get_u16(r, 8);
            if (sh_aas_get_u32(r, 0) != 0x20u || f >= floor_end || t >= floor_end) continue;
            if (reached[f] && !reached[t]) { reached[t] = 1; grew = 1; }
        }
    }
    for (ar = 1; ar < floor_end; ar++) CHECK_MSG(reached[ar], "every floor piece walks to the rest");
    {
        int mask = 0;
        CHECK_MSG(floor_climbs_onto(a, rep.platforms[0].area, floor_end, &mask, NULL, NULL) > 0 ||
                  floor_climbs_onto(a, rep.platforms[1].area, floor_end, &mask, NULL, NULL) > 0,
                  "step 1 is still climbed from the floor");
    }
    {
        size_t len; char err[192]; unsigned char *bytes = sh_aas_write(a, &len);
        CHECK(bytes != NULL);
        if (bytes) { CHECK_MSG(sh_navmesh_validate_aas(bytes, len, err, sizeof err), err);
                     HeapFree(GetProcessHeap(), 0, bytes); }
    }
    sh_aug_report_free(&rep);
    sh_aas_free(a);

    /* High enough to walk under: the floor stays as shipped. */
    a = load_slabs(seams, 2);
    if (!a) { CHECK(0); return; }
    mkbox_yawed(&p[0], 0, 1700, 330, 290, 0, 164.0f, 100.0f, "canopy");
    CHECK(sh_aas_augment(a, p, 1, &o, &rep) == 1);
    CHECK_MSG(rep.floors_cut == 0 && rep.floor_pieces_added == 0, "a canopy leaves the floor whole");
    sh_aug_report_free(&rep);
    sh_aas_free(a);
}

int main(void)
{
    printf("aas_augment_test\n");
    test_the_floor_is_cut_around_the_boxes_on_it();
    test_no_floor_climb_lands_on_a_cut_inside_its_own_box();
    test_a_climb_run_up_may_cross_a_floor_seam();
    test_a_long_face_offers_climbs_along_its_length();
    test_traversal_collection_grows_without_losing_demon_routes();
    test_climb_floor_endpoints_clear_rotated_solids();
    test_rotated_ramps_join_native_floor();
    test_visibility_grows_with_connected_geometry();
    test_visibility_extends_sparse_native_rows();
    test_truncated_visibility_refuses_bake();
    test_dense_climbs_fit_native_routing();
    test_required_routes_over_native_limit_drop_the_box_routes();
    test_route_drop_takes_just_enough();
    test_shipped_routes_over_limit_refuse();
    test_more_than_512_boxes_bake_and_shed_routes();
    test_route_budget_thins_longest_routes_to_fit_the_file();
    test_piece_ceiling_drops_whole_faces();
    test_fixture_resolves();
    test_intersecting_bridge_routes();
    test_exposed_edges_remain_walls();
    test_unmarked_wall_keeps_routes_around_both_ends();
    test_narrow_partial_contacts();
    test_suspended_bridge_is_continuous();
    test_rotated_ridge_has_reciprocal_walk_links();
    test_level_floor_joins_ramp();
    test_traversal_anchor_pattern();
    test_z_at_interpolates_across_a_slope();
    test_contains_is_true_inside();
    test_inset_of_a_rotated_quad_stays_rotated();
    test_inset_refuses_a_collapsing_quad();
    test_field4_follows_the_plane_z_component();
    test_minfloorcos_gate_at_the_boundary();
    test_unreadable_minfloorcos_rejects_the_payload();
    test_a_yawed_area_resolves_through_the_bsp();
    test_two_abutting_platforms_link_to_each_other();
    test_a_partially_abutting_neighbour_is_found();
    test_a_pair_is_not_emitted_twice();
    test_a_gap_within_range_becomes_a_leap();
    test_a_gap_beyond_range_is_not_crossed();
    test_a_steep_gap_is_refused();
    test_a_tall_neighbour_gets_a_climb_not_a_walk();
    test_the_climb_is_reciprocated();
    test_a_three_tower_chain_links_end_to_end();
    test_a_gap_below_the_leap_minimum_is_not_crossed();
    test_a_leap_through_a_third_volume_is_dropped();
    test_an_upright_platform_is_not_a_side_face();
    test_a_dense_yawed_chain_stays_within_the_depth_limit();
    test_a_bake_at_the_platform_cap_completes();
    test_a_pillar_through_a_slab_is_linked();
    test_containment_works_with_the_index_order_reversed();
    test_identical_footprints_are_not_double_linked();
    test_partially_overlapping_volumes_still_link_once();
    test_a_floating_intersecting_volume_is_never_a_stranded_area();
    test_a_platform_with_no_carrier_is_refused_not_phantom();
    test_no_arrangement_emits_a_stranded_area();
    test_areas_added_equals_platforms_emitted();
    test_an_emitted_platform_resolves_at_its_own_centre();
    test_a_pillar_through_a_slab_is_cut_out_of_it();
    test_a_face_buried_in_another_volume_is_refused_by_name();
    test_a_floating_volume_clipping_into_a_platform_is_cut_out();
    test_the_separated_control_arrangement_is_left_alone();
    test_a_box_parked_on_a_platform_is_cut_out_of_it();
    test_a_low_ceiling_over_part_of_a_platform_keeps_the_rest();
    test_a_cut_platform_never_strands_an_area();
    test_a_gap_in_the_dead_zone_is_counted_and_named();
    test_a_flush_pair_is_not_a_dead_gap();
    test_existing_traversals_on_our_floor_are_regrouped_not_declined();
    test_a_module_without_traversals_still_gets_climbs();
    test_island_at_128();
    test_step_regime_at_16();
    test_refusals();
    test_zero_platforms_is_a_no_op();
    test_result_still_validates();
    test_many_platforms_stay_within_depth();
    printf("%s -- %d checks, %d failed\n", g_fail ? "FAILED" : "ok",
           g_checks, g_fail);
    return g_fail ? 1 : 0;
}
