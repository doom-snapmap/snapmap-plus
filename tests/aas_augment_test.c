/* aas_augment_test.c -- the augmenter: admission, the BSP splice, the link
 * regimes, and the refusals.
 *
 * NO GAME BYTES. The fixture below is synthesized: a single flat floor area in
 * a small BSP, with a settings block carrying a plausible monster-class agent
 * box. It is not derived from any shipped file -- see README.md.
 *
 * The two cases that matter most are the two link regimes, because they are the
 * whole behavioural claim:
 *
 *   a platform 16 units up  -> inside maxStepHeight, joined by plain walk links
 *   a platform 128 units up -> outside it, and with no traversal emitted in this
 *                              build it is an ISLAND, which is legal and must be
 *                              reported as such rather than silently shipped
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/backend/aas_edit.h"
#include "../src/backend/aas_augment.h"
#include "../src/backend/nav_traversal.h"
#include "../src/backend/navmesh.h"

static int g_checks = 0, g_fail = 0;

/* navmesh.c is linked in only for sh_navmesh_validate_aas -- the gate the
 * serving path applies -- so its two service dependencies are stubbed, exactly
 * as navmesh_test.c does. */
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
    /* minFloorCos -- 0.7 in every shipped payload, so a floor up to 45.57
     * degrees from horizontal is walkable. The augmenter fails closed without
     * it, exactly as it would on a payload it could not read. */
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
    /* The trees record is ( dx dy dz ) a b c -- THREE FLOATS then three ints, so
     * the root is at +12 and the area count at +20. Shipped files carry
     * ( 0 0 1 ) 1 1 <numAreas>, and this fixture reproduces that exactly: an
     * earlier version wrote the root at +0, which made the augmenter's own
     * off-by-a-vector read agree with it and hid the bug from this suite until
     * a live map exposed it. */
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
    /* A non-zero depth is the assertion that the root was read from the right
     * field. Reading it from the record's first float yields 0, and then EVERY
     * query below answers void and every splice carves nothing -- which looks
     * like a working bake right up until no demon can use it. */
    CHECK_MSG(sh_aas_tree_depth(a) == 2, "the tree root must come from trees[0].a at +12");
    sh_aas_free(a);
}

static void test_island_at_128(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o;
    printf("a platform past maxStepHeight is an island\n");
    if (!a) { CHECK(0); return; }
    memset(&o, 0, sizeof o);
    o.fall = SH_AUG_FALL_AUTO;
    o.inset = 1;
    /* Say NEVER rather than relying on no traversal table having been loaded.
     * sh_trav_load caches for the process, so once any test in this binary
     * loads one the AUTO default stops meaning "no climbs available" -- an
     * order dependency, not a property of the code under test. */
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

    /* The generated area must LOOK like a shipped floor area. These three
     * constants were transcribed wrongly once (flags 0x1, travel 0x20, edge
     * flags 0) and the result was a payload that validated, served and played
     * while the AI quietly refused to route over it -- a failure no structural
     * check can catch, so it is pinned here as literals. */
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
    sh_aug_opts o;
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
    sh_aug_opts o;
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
    sh_aug_opts o;
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
    sh_aug_opts o;
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
    sh_aug_opts o;
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

/* THE ANCHOR PATTERN A CLIMB LINK IS PLACED ON.
 *
 * The outer endpoint of a climb sits the animation's own offset.x out from the
 * ledge, and that offset is PER DEMON -- a clip that starts further out needs
 * more clear floor than one that starts close in. Sampling a single position on
 * the edge therefore decides for every demon at once: if that one spot does not
 * work for a demon with a long start offset, it loses the edge entirely even
 * though it can make the climb, which is what "the big demons ignore my
 * platform" looks like from the outside.
 *
 * The midpoint stays FIRST so every link the single-anchor build produced is
 * still produced in the same place; the rest are only reached by a demon that
 * would otherwise have been dropped. */
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
    unsigned char q[512];
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

/* Containment must be TRUE inside. If the edge normal's sign were inverted this
 * would answer exactly the opposite, and the inset, the carve and neighbour
 * discovery would all invert with it. */
static void test_contains_is_true_inside(void)
{
    unsigned char q[512];
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
    unsigned char q[512], in[512];
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
    unsigned char q[512], in[512];
    double corners[4][3];
    printf("a quad too small to inset is refused\n");
    corners[0][0] = 0;  corners[0][1] = 30; corners[0][2] = 0;
    corners[1][0] = 30; corners[1][1] = 30; corners[1][2] = 0;
    corners[2][0] = 30; corners[2][1] = 0;  corners[2][2] = 0;
    corners[3][0] = 0;  corners[3][1] = 0;  corners[3][2] = 0;
    CHECK(sh_aug_test_quad_init(q, corners) == 1);
    CHECK(sh_aug_test_quad_inset(q, 24.0, in) == 0);
}

/* node.field4 is "has a z component", not "is a Z plane". The yawed-vertical
 * case is the one a Z-versus-XY rule gets wrong, and this build writes a yawed
 * vertical plane on every edge of every rotated platform. */
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
    sh_aug_opts o;
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

/* A payload whose minFloorCos does not read as a cosine is rejected outright.
 * sh_aas_setting_f32 answers 0.0f for a model it cannot read, and a gate of
 * "normal.z < 0" would accept a vertical wall as floor. The setter takes a BYTE
 * offset, and word 16 is 208 + 16*4 = 272. */
static void test_unreadable_minfloorcos_rejects_the_payload(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("a payload with no readable slope limit is refused\n");
    sh_aas_set_setting_f32(a, 272u, 0.0f);
    mkplat(&p, -256.0f, -256.0f, 256.0f, 256.0f, 16.0f, "shelf");
    CHECK_MSG(sh_aas_augment(a, &p, 1, &o, &rep) == 0,
              "fail closed rather than treat a wall as floor");
    sh_aas_free(a);
}

/* A rotated area must be findable through the BSP -- the property the serving
 * path checks before trusting a payload -- and must NOT swallow its own
 * bounding box, which is what an axis-aligned carve would do. */
static void test_a_yawed_area_resolves_through_the_bsp(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o;
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

/* No two reachability records share the same areas AND the same endpoints.
 *
 * This is the shape a broken dedup rule produces: with symmetric discovery both
 * quads emit the pair, and the duplicates are identical record-for-record rather
 * than merely numerous. Counting links alone cannot see it, because a segment
 * legitimately emits one record per sample along it. */
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

/* THE REPORTED BUG. Two volumes standing side by side, 12 apart in z so the step
 * regime carries it and no traversal table is needed. Before this change each
 * one linked only to the module floor, so a demon walked DOWN off one, across
 * the floor, and back UP the other -- "they climb down first then climb the
 * other bv". */
static void test_two_abutting_platforms_link_to_each_other(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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
    sh_aug_opts o;
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

/* Symmetric discovery means A sees B and B sees A, and the regimes already write
 * both directions per segment. Without the ownership rule every record would be
 * written twice and AUG_MAX_TRAVERSALS would fill with duplicates. Step links
 * are emitted per SAMPLE, so the count is not one -- what must hold is that the
 * two directions agree. */
static void test_a_pair_is_not_emitted_twice(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* A synthetic table with LEDGE and LEAP rows for two demons, served through the
 * public loader -- aas_augment_test builds without SH_TRAV_TESTING, so
 * sh_trav_test_parse is not available here. NO GAME BYTES: this is written by
 * the test. */
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
    sh_aug_opts o;
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
    sh_aug_opts o;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a gap wider than any shipped leap is left uncrossed\n");
    load_synthetic_traversal_table();
    mkplat(&p[0], -2000.0f, -400.0f, -1500.0f, 400.0f, 16.0f, "near");
    mkplat(&p[1],  1500.0f, -400.0f,  2000.0f, 400.0f, 16.0f, "far");
    CHECK(sh_aas_augment(a, p, 2, &o, &rep) == 1);
    CHECK_MSG(rep.platforms[0].leaps == 0, "3000 units is past every animation");
    sh_aas_free(a);
}

/* A table-nominal leap is LEVEL -- the median vertical change across 1,754
 * shipped records is one unit, and |dz|/span p90 is 0.29. A steeply graded gap
 * is not what these animations do. */
static void test_a_steep_gap_is_refused(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* THE regression guard on the most dangerous edge of this change.
 *
 * Edge discovery used to refuse a neighbour above, and the regimes gated on the
 * SIGNED height change. Removing the refusal without restating the gates would
 * make aug_step_links' `drop > step` test true for every rise -- a negative drop
 * is always inside any positive step -- and a 112-unit RISE would be written as
 * a plain 0x20 walk link. Across 94,327 shipped walk records joining two flat
 * areas, not one spans more than maxStepHeight. */
static void test_a_tall_neighbour_gets_a_climb_not_a_walk(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* With the ownership rule only the lower area index emits for a pair, so it has
 * to write BOTH directions or the taller platform is a roach motel: demons climb
 * up and can never come down. */
static void test_the_climb_is_reciprocated(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* Three volumes in a row, each a climb above the last. A-B and B-C must both
 * link without either routing through the module floor -- which is the whole
 * shape of the reported bug at one more step. */
static void test_a_three_tower_chain_links_end_to_end(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[3];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* A gap too wide to step and too narrow for the shortest shipped leap gets no
 * link at all. That is honest -- there is no animation for it -- but it must not
 * be mistaken for a leap and written with one that does not fit. */
static void test_a_gap_below_the_leap_minimum_is_not_crossed(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* The swept-path check. Without it a leap is written straight through whatever
 * stands between the two platforms -- "they can glitch through the bv during the
 * traversal and get lost". This is only meaningful now that leaps exist; before
 * them no link across this gap was attempted and the test would have passed
 * without testing anything. */
static void test_a_leap_through_a_third_volume_is_dropped(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[3];
    sh_aug_report rep;
    sh_aug_report bare;
    sh_aug_platform two[2];
    sh_aas *b = load_module();
    sh_aug_opts o;
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
    sh_aug_opts o;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_NEVER;
    printf("an ordinary upright platform is not reported as a side face\n");
    mkplat(&p, -500.0f, -500.0f, 500.0f, 500.0f, 16.0f, "flat");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK(rep.platforms[0].side_face == 0);
    CHECK(near_f(rep.platforms[0].tilt_degrees, 0.0f));
    sh_aas_free(a);
}

/* Oblique and yawed split planes are not clipped by aug_clip_cell, so the
 * running cell stays a superset, aug_box_side straddles more often, and more
 * leaves keep all five split tests. nav_bake discards the WHOLE module's bake on
 * depth_exceeded, so that cost has to be measured rather than assumed. */
static void test_a_dense_yawed_chain_stays_within_the_depth_limit(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[16];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* A bake at the platform cap has to finish, and finish coherently. Gap discovery
 * casts outward as far as the longest shipped leap from every sample on every
 * edge, so without a bounding-box prefilter over the peer set this is four edges
 * by thirty-two samples by sixty ray steps by five hundred peers, per platform,
 * for five hundred platforms. That is not slow, it is never. */
static void test_a_bake_at_the_platform_cap_completes(void)
{
    sh_aas *a = load_module();
    static sh_aug_platform p[200];
    sh_aug_report rep;
    sh_aug_opts o;
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
              "the model must still be coherent");
    for (i = 0; i < rep.platform_count; i++) if (rep.platforms[i].emitted) emitted++;
    CHECK_MSG(emitted > 100, "most of them should land");
    CHECK_MSG(rep.depth_exceeded == 0, "and the tree stays inside the loader limit");
    sh_aas_free(a);
}

/* A module that already owns traversal points on the area we would climb FROM.
 *
 * The predecessor refused whenever a payload had ANY traversal points, on the
 * stated grounds that no SnapMap module ships one. classic_90_climb -- this
 * project's own donor -- ships seven, and 312 of 696 extracted payloads carry
 * traversal animation names, so that refusal silently produced zero climbs on
 * about half of all modules while blaming the geometry.
 *
 * The condition that actually matters is narrower: each area's points must stay
 * contiguous, and ours are appended at the end. This pokes the floor area's
 * ownership count to make the unsafe case, and checks it is refused AND said
 * out loud. */
static void test_existing_traversals_on_our_floor_are_declined_out_loud(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o;
    unsigned char *floor_area;
    o.fall = SH_AUG_FALL_AUTO; o.inset = 1; o.traversal = SH_AUG_TRAVERSAL_AUTO;
    printf("a module that already owns climbs on our floor says so\n");
    load_synthetic_traversal_table();

    /* Area 1 is the floor slab; claim it already owns a traversal point. */
    floor_area = sh_aas_rec(a, SH_AAS_L_AREAS, 1);
    CHECK(floor_area != NULL);
    if (floor_area) sh_aas_put_u16(floor_area, 38u, 1u);   /* AR_NUM_TRAV_POINT */

    mkplat(&p, -500.0f, -500.0f, 500.0f, 500.0f, 128.0f, "roof");
    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK_MSG(rep.climbs_declined == 1,
              "the refusal is reported, not blamed on the geometry");
    CHECK_MSG(rep.platforms[0].climbs == 0, "and no climb was written");
    sh_aas_free(a);
}

/* The ordinary case must not regress: a module with no traversal points of its
 * own still gets its climbs. */
static void test_a_module_without_traversals_still_gets_climbs(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o;
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
/*
 * The reported failure was a tall pillar standing THROUGH a wide slab: all AI
 * froze, shooting but not moving. Nothing here caught it because every fixture
 * above uses volumes that are separate or merely abutting -- which is exactly
 * the arrangement that always worked.
 *
 * These pin the rest of the matrix. The property that matters for every one of
 * them is the same: a platform that is EMITTED must not be an ISLAND, because an
 * emitted-but-unroutable area is what makes a demon stand still. A platform we
 * decline to emit is fine; silence is not.
 */

/* CONTAINMENT -- the reported case. The pillar's footprint is wholly inside the
 * slab's, so no slab edge can ever face the pillar. Discovery is one-sided, and
 * an ownership rule based on area index alone hands the pair to the side that
 * cannot see it. */
static void test_a_pillar_through_a_slab_is_linked(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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
    sh_aug_opts o;
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

/* IDENTICAL footprints at different heights. Each contains the other, so the
 * containment rule must NOT fire for both -- that would emit every record twice.
 * It has to fall through to the index rule. */
static void test_identical_footprints_are_not_double_linked(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* PARTIAL overlap -- corners crossing. Both sides discover it, so this is the
 * symmetric case the index rule was written for. Guard against the fix breaking
 * it. */
static void test_partially_overlapping_volumes_still_link_once(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* A FLOATING volume that intersects a floor-standing one without reaching the
 * floor. Whatever the bake decides, it must not emit an area and then strand
 * it. */
static void test_a_floating_intersecting_volume_is_never_a_stranded_area(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[2];
    sh_aug_report rep;
    sh_aug_opts o;
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

/* A platform with NOTHING under it to splice into. The carve takes no leaf
 * slots, and the area must not be reported as emitted -- a phantom counted into
 * a cluster but absent from the tree is exactly what corrupts routing. */
static void test_a_platform_with_no_carrier_is_refused_not_phantom(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p;
    sh_aug_report rep;
    sh_aug_opts o;
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

/* THE INVARIANT, over the whole matrix at once: nothing the bake emits may be
 * an island. This is the property that actually maps onto "demons stand still
 * and shoot", so it is asserted across every arrangement together rather than
 * only one at a time. */
static void test_no_arrangement_emits_a_stranded_area(void)
{
    sh_aas *a = load_module();
    sh_aug_platform p[8];
    sh_aug_report rep;
    sh_aug_opts o;
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

int main(void)
{
    printf("aas_augment_test\n");
    test_fixture_resolves();
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
    test_existing_traversals_on_our_floor_are_declined_out_loud();
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
