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
     * maxFallHeight 0 -- the monster-class shape. */
    putf(p + SETW(0), -24.0f); putf(p + SETW(1), -24.0f); putf(p + SETW(2), 0.0f);
    putf(p + SETW(3),  24.0f); putf(p + SETW(4),  24.0f); putf(p + SETW(5), 80.0f);
    putf(p + SETW(12), 18.0f);          /* maxStepHeight */
    putf(p + SETW(15), 0.0f);           /* maxFallHeight */
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
    mkplat(&p, -500.0f, -500.0f, 500.0f, 500.0f, 128.0f, "roof");

    CHECK(sh_aas_augment(a, &p, 1, &o, &rep) == 1);
    CHECK_MSG(rep.platforms[0].emitted == 1, "the platform must be emitted");
    CHECK_MSG(rep.areas_after == rep.areas_before + 1, "exactly one area added");
    CHECK_MSG(rep.platforms[0].carrier == 1, "the floor slab is the carrier");
    CHECK_MSG(rep.platforms[0].leaf_slots_carved > 0,
              "the area must be spliced into the BSP or it is unfindable");
    CHECK_MSG(rep.platforms[0].island == 1,
              "128 is past maxStepHeight and no traversal is emitted in this build");
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

int main(void)
{
    printf("aas_augment_test\n");
    test_fixture_resolves();
    test_traversal_anchor_pattern();
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
