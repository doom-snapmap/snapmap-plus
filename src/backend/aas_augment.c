/* aas_augment.c -- see aas_augment.h for what this is and why.
 *
 * Every constant below was measured against shipped data. Where a value is
 * copied without being understood, the comment says so rather than inventing a
 * justification for it.
 */
#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "aas_augment.h"
#include "navmesh.h"
#include "nav_traversal.h"

/* ---- measured constants ------------------------------------------------ */

/* reachability.travel_flags for a plain walk. 0x20 on all 100 reachabilities of
 * the Grid Room and on 85,422 of 90,929 corpus records. */
#define REACH_WALK              0x00000020u
/* travel_time for a plain walk: 5, on 73,135 records. The other value, 105, is
 * 5 + settings.tt_barrierJump and means a barrier was crossed. */
#define REACH_WALK_TIME         5

/* walk-off-ledge. dz strictly negative in 1559 of 1559 corpus records, and they
 * occur ONLY in files whose settings.maxFallHeight > 0 -- which is never a
 * monster-class module, where it is 0. */
#define REACH_FALL              0x00000040u
/* fall time = tt_startWalkOffLedge + round(drop / 22.2). The divisor is fitted
 * over those 1559 records; the median extra time is within +-1 everywhere
 * across the whole 32..896 unit range. */
#define REACH_FALL_PER_UNIT     (1.0 / 22.2)

/* The BSP split plane for a walkable surface at height Z sits at Z - 9.6.
 * Measured as areaBounds[leaf].minz - planeZ over every (Z-plane, area-leaf)
 * node in the corpus: 9.6 is the modal value at 1984 nodes, and the Grid Room
 * uses it exclusively. */
#define BSP_FLOOR_EPS           9.6f

/* node.field4 by split-plane orientation. 1 on 100% of Z-plane nodes
 * (19279 + 43071 + 4895 records); 0 on 61405/61563 (XY, node, area) and
 * 150254/150292 (XY, area, area). Its MEANING is unknown -- copied, not
 * understood. */
#define NODE_F4_Z               1
#define NODE_F4_XY              0

/* Spacing between successive walk reachabilities along a shared edge, and the
 * inset from that edge's ends. Bracketed by the Grid Room itself: a 4880-unit
 * edge gets 9 intervals and a 336-unit edge gets 1, so the divisor lies in
 * (541.2, 608.75]; any value in the bracket reproduces both. */
#define WALK_SPACING            576.0f
#define WALK_END_INSET          5.0f
/* Each endpoint is displaced one unit perpendicular, into its own area. */
#define REACH_SIDE_OFFSET       1.0f

/* Area flags for a floor area, copied from shipped data. 0x8 is what EVERY real
 * area carries -- 883 of the 947 in the largest sampled module, and all of them
 * in four others. */
#define AREA_FLAGS_FLOOR        0x00000008u

/* area.travel_flags for a floor area. 0x000A on the seven floor slabs of the
 * Grid Room and the dominant value everywhere (710 of 947). NOT 0x0002, which
 * marks a module-seam connector, and not 0x0020 -- that is a REACHABILITY travel
 * flag and means nothing in the area's own flag space. */
#define AREA_TRAVEL_FLAGS_FLOOR 0x000Au

/* edge.flags. An edge used by exactly one area is a ledge/outer boundary and
 * carries 0x0C01 (17421 of 17442 shipped instances); one shared by two areas
 * carries 0x0C00. Writing 0 leaves an edge that belongs to nothing shipped data
 * recognises. */
#define EDGE_FLAGS_BOUNDARY     0x00000C01
#define EDGE_FLAGS_SHARED       0x00000C00

#define AUG_INT16_LO            (-32768)
#define AUG_INT16_HI            (32767)

/* Settings-block word offsets. The block is a u32 type, three length-prefixed
 * fixed-64-byte strings (3 * 68 = 204), then 39 big-endian words -- which is
 * where 4 + 204 + 156 = 364, SH_AAS_SETTINGS_BYTES, comes from. */
#define SET_WORDS               208
#define SET_W(n)                (SET_WORDS + (n) * 4)
#define SET_MAX_STEP_HEIGHT     SET_W(12)
#define SET_MAX_FALL_HEIGHT     SET_W(15)
#define SET_TT_WALK_OFF_LEDGE   SET_W(36)

/* Record field offsets, from the on-disk record layouts. */
#define PL_A                    0       /* plane: 4 floats, a b c dist */
#define PL_DIST                 12
#define AR_FLAGS                0       /* area: 44 bytes */
#define AR_TRAVEL_FLAGS         4
#define AR_NUM_EDGES            6
#define AR_FIRST_EDGE_INDEX     8
#define AR_CLUSTER              12
#define AR_CLUSTER_AREA_NUM     14
#define AR_FIRST_OBSTACLE_PVS   16
#define AR_FIRST_REACH_FROM     20
#define AR_FIRST_REACH_TO       24
#define AR_FIRST_AREA_COVER     32
#define AR_FIRST_TRAV_POINT     36      /* -> traversalPoints, partitioned per area */
#define AR_NUM_TRAV_POINT       38
#define ND_PLANE                0       /* node: 4 ints */
#define ND_FIELD4               4
#define ND_CHILD0               8
#define ND_CHILD1               12
#define CL_W0                   0       /* cluster: 4 ints */
#define CL_W1                   4
#define RE_TRAVEL_FLAGS         0       /* reachability: 40 bytes */
#define RE_TRAVEL_TIME          4
#define RE_FROM_AREA            6
#define RE_TO_AREA              8
#define RE_START                12      /* int16[3] */
#define RE_END                  18      /* int16[3] */
#define RE_NEXT_FROM            32
#define RE_NEXT_TO              36
#define AB_MINX                 0       /* areaBounds: 6 int16 */
#define AB_MINY                 2
#define AB_MINZ                 4
#define AB_MAXX                 6
#define AB_MAXY                 8
#define AB_MAXZ                 10
/* trees: 24 bytes, THREE FLOATS then three ints -- ( dx dy dz ) a b c, with the
 * integers sitting outside the parentheses in the text form. Shipped files carry
 * ( 0 0 1 ) 1 1 <numAreas>, so `a` (the root node) is at +12 and `c` (the area
 * count) at +20. Reading the root from +0 instead yields the float 0.0's bits,
 * i.e. root 0, which makes the tree walk terminate immediately: every point
 * resolves to void, the splice carves nothing, and the bake silently produces
 * navigation no demon can find. */
#define TR_A                    12
#define TR_C                    20

/* ---- small helpers ----------------------------------------------------- */

/* The engine's float->short conversion (FUN_14175E920) is cvtss2si, i.e. round
 * to nearest EVEN. The Grid Room's vertices are not exact integers
 * (z = 3359.999755859375 stores as 3360), which is what proves it rounds at
 * all rather than truncating. */
static int aug_round(double v)
{
    double f = floor(v);
    double d = v - f;
    if (d > 0.5) return (int)(f + 1.0);
    if (d < 0.5) return (int)f;
    return (int)(fmod(f, 2.0) == 0.0 ? f : f + 1.0);
}

static int aug_clamp16(int v)
{
    if (v < AUG_INT16_LO) return AUG_INT16_LO;
    if (v > AUG_INT16_HI) return AUG_INT16_HI;
    return v;
}

/* C truncation toward zero -- how the shipped reachability endpoints are
 * quantised (Grid Room edge y = -1223.99987 -> endpoints -1224 and -1222). */
static int aug_trunc(double v) { return (int)v; }

static float aug_agent_radius(const sh_aas *a)
{
    float mins[3], maxs[3], r;
    sh_aas_agent_bounds(a, mins, maxs);
    r = maxs[0];
    if (maxs[1] > r) r = maxs[1];
    if (-mins[0] > r) r = -mins[0];
    if (-mins[1] > r) r = -mins[1];
    return r;
}

static float aug_agent_height(const sh_aas *a)
{
    float mins[3], maxs[3];
    sh_aas_agent_bounds(a, mins, maxs);
    return maxs[2] - mins[2];
}

/* ---- BSP queries ------------------------------------------------------- */

/* Every shipped tree record is ( 0 0 1 ) 1 1 N: node 0 is a dummy with both
 * children 0, so the root is field `a`, which is 1. */
static int aug_tree_root(const sh_aas *a)
{
    const unsigned char *t;
    if (sh_aas_count(a, SH_AAS_L_TREES) == 0) return 1;
    t = sh_aas_rec_const(a, SH_AAS_L_TREES, 0);
    return t ? (int)sh_aas_get_i32(t, TR_A) : 1;
}

/* Walk the BSP the way the engine does: n.p + dist > 0 takes child0. Returns
 * the area number, or 0 for "no area". */
int sh_aas_point_area(const sh_aas *a, float x, float y, float z)
{
    int n = aug_tree_root(a);
    unsigned guard = 0;
    while (n > 0) {
        const unsigned char *nd = sh_aas_rec_const(a, SH_AAS_L_NODES, (unsigned)n);
        const unsigned char *p;
        double d;
        if (!nd) return 0;
        p = sh_aas_rec_const(a, SH_AAS_L_PLANES,
                             (unsigned)sh_aas_get_i32(nd, ND_PLANE));
        if (!p) return 0;
        d = (double)sh_aas_get_f32(p, PL_A) * (double)x
          + (double)sh_aas_get_f32(p, PL_A + 4) * (double)y
          + (double)sh_aas_get_f32(p, PL_A + 8) * (double)z
          + (double)sh_aas_get_f32(p, PL_DIST);
        n = d > 0.0 ? sh_aas_get_i32(nd, ND_CHILD0)
                    : sh_aas_get_i32(nd, ND_CHILD1);
        /* The validator has already proved this tree terminates, so a guard trip
         * means the model was mutated into a cycle -- refuse, do not spin. */
        if (++guard > 65536u) return 0;
    }
    return -n;
}

unsigned sh_aas_tree_depth(const sh_aas *a)
{
    typedef struct { int node; unsigned depth; } aug_frame;
    aug_frame *stack;
    unsigned cap = 256, top = 0, best = 0;
    int root = aug_tree_root(a);
    if (root <= 0) return 0;
    stack = (aug_frame *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(aug_frame));
    if (!stack) return 0;
    stack[top].node = root; stack[top].depth = 1; top++;
    while (top > 0) {
        aug_frame fr = stack[--top];
        const unsigned char *nd;
        int k;
        if (fr.depth > best) best = fr.depth;
        if (fr.depth > (unsigned)SH_AAS_MAX_DEPTH) break;
        nd = sh_aas_rec_const(a, SH_AAS_L_NODES, (unsigned)fr.node);
        if (!nd) continue;
        for (k = 0; k < 2; k++) {
            int child = sh_aas_get_i32(nd, k == 0 ? ND_CHILD0 : ND_CHILD1);
            if (child <= 0) continue;
            if (top + 1 >= cap) {
                aug_frame *bigger;
                unsigned ncap = cap * 2;
                bigger = (aug_frame *)HeapReAlloc(GetProcessHeap(), 0, stack,
                                                  ncap * sizeof(aug_frame));
                if (!bigger) { HeapFree(GetProcessHeap(), 0, stack); return best; }
                stack = bigger; cap = ncap;
            }
            stack[top].node = child; stack[top].depth = fr.depth + 1; top++;
        }
    }
    HeapFree(GetProcessHeap(), 0, stack);
    return best;
}

/* ---- the augmenter state ----------------------------------------------- */

typedef struct aug_rect {
    float x0, y0, x1, y1, z;
} aug_rect;

typedef struct aug_ctx {
    sh_aas         *a;
    const sh_aug_opts *o;
    sh_aug_report  *rep;
    float           radius;
    float           height;
    float           step;
    int             failed;
} aug_ctx;

/* An AABB, as (minx, miny, minz, maxx, maxy, maxz). */
typedef struct aug_box { double v[6]; } aug_box;

/* ---- interning --------------------------------------------------------- */

/* Appending a vertex/edge/plane that already exists would bloat the payload and
 * -- worse for the plane lump -- give the tree two numerically identical split
 * planes, which makes a carve chain impossible to reason about. So each of the
 * three is deduplicated by value against the whole existing lump. The lumps are
 * small enough (the Grid Room has 12 planes) that a linear scan is right. */
static int aug_vertex(aug_ctx *c, float x, float y, float z)
{
    unsigned i, n = sh_aas_count(c->a, SH_AAS_L_VERTICES), first;
    unsigned char *r;
    for (i = 0; i < n; i++) {
        const unsigned char *v = sh_aas_rec_const(c->a, SH_AAS_L_VERTICES, i);
        if (v && sh_aas_get_f32(v, 0) == x && sh_aas_get_f32(v, 4) == y
              && sh_aas_get_f32(v, 8) == z)
            return (int)i;
    }
    if (!sh_aas_append(c->a, SH_AAS_L_VERTICES, 1, &first)) return -1;
    r = sh_aas_rec(c->a, SH_AAS_L_VERTICES, first);
    if (!r) return -1;
    sh_aas_put_f32(r, 0, x); sh_aas_put_f32(r, 4, y); sh_aas_put_f32(r, 8, z);
    return (int)first;
}

/* Return a SIGNED edge index: +i when the edge runs v0->v1, -i when it already
 * exists as v1->v0. A negative edgeIndex entry is the shipped convention for
 * "this edge is traversed reversed", and an edge that turns out to be shared by
 * two areas is demoted from boundary to shared. */
static int aug_edge(aug_ctx *c, int v0, int v1)
{
    unsigned i, n = sh_aas_count(c->a, SH_AAS_L_EDGES), first;
    unsigned char *r;
    for (i = 0; i < n; i++) {
        unsigned char *e = sh_aas_rec(c->a, SH_AAS_L_EDGES, i);
        if (!e) continue;
        if (sh_aas_get_i32(e, 0) == v0 && sh_aas_get_i32(e, 4) == v1) {
            if (sh_aas_get_i32(e, 8) == EDGE_FLAGS_BOUNDARY)
                sh_aas_put_i32(e, 8, EDGE_FLAGS_SHARED);
            return (int)i;
        }
        if (sh_aas_get_i32(e, 0) == v1 && sh_aas_get_i32(e, 4) == v0) {
            if (sh_aas_get_i32(e, 8) == EDGE_FLAGS_BOUNDARY)
                sh_aas_put_i32(e, 8, EDGE_FLAGS_SHARED);
            return -(int)i;
        }
    }
    if (!sh_aas_append(c->a, SH_AAS_L_EDGES, 1, &first)) return 0;
    r = sh_aas_rec(c->a, SH_AAS_L_EDGES, first);
    if (!r) return 0;
    sh_aas_put_i32(r, 0, v0);
    sh_aas_put_i32(r, 4, v1);
    sh_aas_put_i32(r, 8, EDGE_FLAGS_BOUNDARY);
    return (int)first;
}

static int aug_plane(aug_ctx *c, float pa, float pb, float pc, float dist)
{
    unsigned i, n = sh_aas_count(c->a, SH_AAS_L_PLANES), first;
    unsigned char *r;
    for (i = 0; i < n; i++) {
        const unsigned char *p = sh_aas_rec_const(c->a, SH_AAS_L_PLANES, i);
        if (p && sh_aas_get_f32(p, 0) == pa && sh_aas_get_f32(p, 4) == pb
              && sh_aas_get_f32(p, 8) == pc && sh_aas_get_f32(p, 12) == dist)
            return (int)i;
    }
    if (!sh_aas_append(c->a, SH_AAS_L_PLANES, 1, &first)) return -1;
    r = sh_aas_rec(c->a, SH_AAS_L_PLANES, first);
    if (!r) return -1;
    sh_aas_put_f32(r, 0, pa); sh_aas_put_f32(r, 4, pb);
    sh_aas_put_f32(r, 8, pc); sh_aas_put_f32(r, 12, dist);
    return (int)first;
}

static int aug_node(aug_ctx *c, int plane_num, int field4, int child0, int child1)
{
    unsigned first;
    unsigned char *r;
    if (!sh_aas_append(c->a, SH_AAS_L_NODES, 1, &first)) return 0;
    r = sh_aas_rec(c->a, SH_AAS_L_NODES, first);
    if (!r) return 0;
    sh_aas_put_i32(r, ND_PLANE, plane_num);
    sh_aas_put_i32(r, ND_FIELD4, field4);
    sh_aas_put_i32(r, ND_CHILD0, child0);
    sh_aas_put_i32(r, ND_CHILD1, child1);
    return (int)first;
}

/* ---- area geometry ----------------------------------------------------- */

/* An area whose bounds are flat in z, as (minx, miny, maxx, maxy, z). A
 * generated platform and every Grid Room floor slab are flat; anything else is
 * not something these linkers know how to join. */
static int aug_flat_box(const sh_aas *a, unsigned area, float out[5])
{
    const unsigned char *b = sh_aas_rec_const(a, SH_AAS_L_AREABOUNDS, area);
    if (!b) return 0;
    if (sh_aas_get_i16(b, AB_MINZ) != sh_aas_get_i16(b, AB_MAXZ)) return 0;
    out[0] = (float)sh_aas_get_i16(b, AB_MINX);
    out[1] = (float)sh_aas_get_i16(b, AB_MINY);
    out[2] = (float)sh_aas_get_i16(b, AB_MAXX);
    out[3] = (float)sh_aas_get_i16(b, AB_MAXY);
    out[4] = (float)sh_aas_get_i16(b, AB_MINZ);
    return 1;
}

/* Which cluster a platform joins: the carrier's.
 *
 * Two facts decide it. portalIndex is empty in 20/20 shipped payloads and the
 * single portals record is all-zero, so clusters cannot be linked to each other
 * in the file at all. And the one area the Grid Room does put in its own cluster
 * -- the ceiling -- is also the one area with no reachabilities. Giving a
 * platform its own cluster would reproduce exactly that unreachable shape. */
static int aug_choose_cluster(aug_ctx *c, int carrier)
{
    unsigned i, n = sh_aas_count(c->a, SH_AAS_L_AREAS);
    int best = 0, best_count = -1;
    unsigned counts[64];
    if (carrier > 0 && (unsigned)carrier < n) {
        const unsigned char *ar = sh_aas_rec_const(c->a, SH_AAS_L_AREAS, (unsigned)carrier);
        if (ar) return (int)sh_aas_get_u16(ar, AR_CLUSTER);
    }
    memset(counts, 0, sizeof counts);
    for (i = 1; i < n; i++) {
        const unsigned char *ar = sh_aas_rec_const(c->a, SH_AAS_L_AREAS, i);
        unsigned cl = ar ? sh_aas_get_u16(ar, AR_CLUSTER) : 0;
        if (cl < 64) counts[cl]++;
    }
    for (i = 0; i < 64; i++) {
        if ((int)counts[i] > best_count) { best_count = (int)counts[i]; best = (int)i; }
    }
    return best;
}

/* The carrier's obstaclePVS row, copied verbatim. The lump is a flat byte
 * stream partitioned by area.first_obstacle_pvs, so a row runs from this area's
 * offset to the NEXT area's -- there is no length field. */
static void aug_copy_pvs(aug_ctx *c, int carrier, unsigned *out_first)
{
    unsigned n = sh_aas_count(c->a, SH_AAS_L_AREAS);
    unsigned total = sh_aas_count(c->a, SH_AAS_L_OBSTACLEPVS);
    unsigned lo, hi, i, first;
    const unsigned char *ar;

    *out_first = total;
    if (carrier <= 0 || (unsigned)carrier >= n) return;
    ar = sh_aas_rec_const(c->a, SH_AAS_L_AREAS, (unsigned)carrier);
    if (!ar) return;
    lo = sh_aas_get_u32(ar, AR_FIRST_OBSTACLE_PVS);
    hi = total;
    if ((unsigned)carrier + 1 < n) {
        const unsigned char *nx = sh_aas_rec_const(c->a, SH_AAS_L_AREAS,
                                                   (unsigned)carrier + 1);
        if (nx) hi = sh_aas_get_u32(nx, AR_FIRST_OBSTACLE_PVS);
    }
    if (hi < lo || lo > total || hi > total) return;
    if (hi == lo) return;
    if (!sh_aas_append(c->a, SH_AAS_L_OBSTACLEPVS, hi - lo, &first)) return;
    for (i = 0; i < hi - lo; i++) {
        const unsigned char *src = sh_aas_rec_const(c->a, SH_AAS_L_OBSTACLEPVS, lo + i);
        unsigned char *dst = sh_aas_rec(c->a, SH_AAS_L_OBSTACLEPVS, first + i);
        if (src && dst) *dst = *src;
    }
    *out_first = first;
}

/* Append one walkable area for `eff` and return its index, or -1. */
static int aug_add_area(aug_ctx *c, const aug_rect *eff, int carrier)
{
    /* A closed edge loop wound clockwise seen from +Z -- the winding of every
     * floor area of the Grid Room, verified by shoelace on areas 2, 5 and 8. */
    const float cx[4] = { eff->x0, eff->x1, eff->x1, eff->x0 };
    const float cy[4] = { eff->y1, eff->y1, eff->y0, eff->y0 };
    int vs[4];
    unsigned first_ei = sh_aas_count(c->a, SH_AAS_L_EDGEINDEX);
    unsigned first_pvs = 0, first_area, first_bounds, slot;
    unsigned char *ar, *ab;
    int cluster, can = 0, k;
    unsigned i, n;

    for (k = 0; k < 4; k++) {
        vs[k] = aug_vertex(c, cx[k], cy[k], eff->z);
        if (vs[k] < 0) return -1;
    }
    for (k = 0; k < 4; k++) {
        /* Signed: a negative entry means the edge is traversed reversed, so 0 is
         * the failure value here rather than a negative one. */
        int e = aug_edge(c, vs[k], vs[(k + 1) & 3]);
        unsigned char *ie;
        if (e == 0) return -1;
        if (!sh_aas_append(c->a, SH_AAS_L_EDGEINDEX, 1, &slot)) return -1;
        ie = sh_aas_rec(c->a, SH_AAS_L_EDGEINDEX, slot);
        if (!ie) return -1;
        sh_aas_put_i32(ie, 0, e);
    }

    cluster = aug_choose_cluster(c, carrier);
    n = sh_aas_count(c->a, SH_AAS_L_AREAS);
    for (i = 1; i < n; i++) {
        const unsigned char *o = sh_aas_rec_const(c->a, SH_AAS_L_AREAS, i);
        if (o && (int)sh_aas_get_u16(o, AR_CLUSTER) == cluster) can++;
    }

    aug_copy_pvs(c, carrier, &first_pvs);

    if (!sh_aas_append(c->a, SH_AAS_L_AREAS, 1, &first_area)) return -1;
    if (!sh_aas_append(c->a, SH_AAS_L_AREABOUNDS, 1, &first_bounds)) return -1;
    ar = sh_aas_rec(c->a, SH_AAS_L_AREAS, first_area);
    ab = sh_aas_rec(c->a, SH_AAS_L_AREABOUNDS, first_bounds);
    if (!ar || !ab) return -1;

    sh_aas_put_u32(ar, AR_FLAGS, AREA_FLAGS_FLOOR);
    sh_aas_put_u16(ar, AR_TRAVEL_FLAGS, AREA_TRAVEL_FLAGS_FLOOR);
    sh_aas_put_u16(ar, AR_NUM_EDGES, 4);
    sh_aas_put_u32(ar, AR_FIRST_EDGE_INDEX, first_ei);
    sh_aas_put_u16(ar, AR_CLUSTER, (uint16_t)cluster);
    sh_aas_put_u16(ar, AR_CLUSTER_AREA_NUM, (uint16_t)can);
    sh_aas_put_u32(ar, AR_FIRST_OBSTACLE_PVS, first_pvs);
    sh_aas_put_i32(ar, AR_FIRST_REACH_FROM, -1);
    sh_aas_put_i32(ar, AR_FIRST_REACH_TO, -1);
    sh_aas_put_u16(ar, AR_FIRST_AREA_COVER,
                   (uint16_t)sh_aas_count(c->a, SH_AAS_L_AREACOVERINDEX));

    sh_aas_put_i16(ab, AB_MINX, (int16_t)aug_clamp16(aug_round(eff->x0)));
    sh_aas_put_i16(ab, AB_MINY, (int16_t)aug_clamp16(aug_round(eff->y0)));
    sh_aas_put_i16(ab, AB_MINZ, (int16_t)aug_clamp16(aug_round(eff->z)));
    sh_aas_put_i16(ab, AB_MAXX, (int16_t)aug_clamp16(aug_round(eff->x1)));
    sh_aas_put_i16(ab, AB_MAXY, (int16_t)aug_clamp16(aug_round(eff->y1)));
    sh_aas_put_i16(ab, AB_MAXZ, (int16_t)aug_clamp16(aug_round(eff->z)));

    /* Cluster bookkeeping: w0 == w1 == the number of areas in the cluster, true
     * in 20/20 shipped payloads. */
    if (cluster >= 0 && (unsigned)cluster < sh_aas_count(c->a, SH_AAS_L_CLUSTERS)) {
        unsigned char *cl = sh_aas_rec(c->a, SH_AAS_L_CLUSTERS, (unsigned)cluster);
        if (cl) {
            sh_aas_put_i32(cl, CL_W0, can + 1);
            sh_aas_put_i32(cl, CL_W1, can + 1);
        }
    }
    return (int)first_area;
}

/* ---- the BSP splice ---------------------------------------------------- */

/* Which side(s) of a plane an AABB lies on. The engine takes child0 when
 * n.p + dist > 0, so "front" is that side. */
static int aug_box_side(const aug_box *b, const unsigned char *p, double eps)
{
    double pa = sh_aas_get_f32(p, PL_A);
    double pb = sh_aas_get_f32(p, PL_A + 4);
    double pc = sh_aas_get_f32(p, PL_A + 8);
    double pd = sh_aas_get_f32(p, PL_DIST);
    double lo = pa * (pa > 0 ? b->v[0] : b->v[3])
              + pb * (pb > 0 ? b->v[1] : b->v[4])
              + pc * (pc > 0 ? b->v[2] : b->v[5]) + pd;
    double hi = pa * (pa > 0 ? b->v[3] : b->v[0])
              + pb * (pb > 0 ? b->v[4] : b->v[1])
              + pc * (pc > 0 ? b->v[5] : b->v[2]) + pd;
    if (lo > eps) return 1;                     /* wholly front */
    if (hi <= -eps) return -1;                  /* wholly back */
    return 0;                                   /* straddles */
}

/* Clip an AABB by an axis-aligned split plane. A non-axis-aligned plane leaves
 * the box alone, so the result is always a SUPERSET of the true convex cell --
 * which is exactly what keeps the pruning in aug_chain sound: dropping a split
 * plane the superset already satisfies can never be wrong. */
static void aug_clip_cell(aug_box *cell, const unsigned char *p, int front)
{
    double comps[3];
    int ax = -1, i, nz = 0;
    double t, comp;
    comps[0] = sh_aas_get_f32(p, PL_A);
    comps[1] = sh_aas_get_f32(p, PL_A + 4);
    comps[2] = sh_aas_get_f32(p, PL_A + 8);
    for (i = 0; i < 3; i++) if (comps[i] != 0.0) { nz++; ax = i; }
    if (nz != 1) return;
    comp = comps[ax];
    t = -(double)sh_aas_get_f32(p, PL_DIST) / comp;
    if ((comp > 0.0) == (front != 0)) {
        if (t > cell->v[ax]) cell->v[ax] = t;
    } else {
        if (t < cell->v[ax + 3]) cell->v[ax + 3] = t;
    }
}

/* Splice `area` into the BSP for the volume standing on `eff`.
 *
 * Descends from the root with the agent's standing box. Wherever that box
 * reaches a leaf slot holding a real area, the slot is replaced by a chain of at
 * most five new nodes:
 *
 *     z > eff.z - 9.6  ->  x > x0  ->  x < x1  ->  y > y0  ->  y < y1  ->  -area
 *     (any test failing)  ->  the original leaf
 *
 * Leaf slots holding 0 ("no area") are left alone. That keeps every node we
 * write in one of the child-kind combinations whose field4 value is unambiguous
 * in shipped data; the combinations with a void child carry only unexplained
 * 0x8000xxxx / 0x7fffxxxx values, so authoring one would be a guess. The
 * consequence is that a platform overhanging the module's navigable space is not
 * navigable over the overhang -- which is reported, not hidden.
 *
 * Returns the number of leaf slots carved. */
static int aug_carve(aug_ctx *c, int area, const aug_rect *eff)
{
    typedef struct { int node; aug_box cell; } aug_frame;
    struct { int plane; int f4; } split[5];
    aug_frame *stack;
    unsigned cap = 256, top = 0;
    int carved = 0, root, i;
    aug_box box;
    const double BIG = 1e9;
    const double eps = 1e-3;

    split[0].plane = aug_plane(c, 0.0f, 0.0f, 1.0f, -(eff->z - BSP_FLOOR_EPS));
    split[0].f4 = NODE_F4_Z;
    split[1].plane = aug_plane(c, 1.0f, 0.0f, 0.0f, -eff->x0);
    split[1].f4 = NODE_F4_XY;
    split[2].plane = aug_plane(c, -1.0f, 0.0f, 0.0f, eff->x1);
    split[2].f4 = NODE_F4_XY;
    split[3].plane = aug_plane(c, 0.0f, 1.0f, 0.0f, -eff->y0);
    split[3].f4 = NODE_F4_XY;
    split[4].plane = aug_plane(c, 0.0f, -1.0f, 0.0f, eff->y1);
    split[4].f4 = NODE_F4_XY;
    for (i = 0; i < 5; i++) if (split[i].plane < 0) return 0;

    box.v[0] = eff->x0; box.v[1] = eff->y0; box.v[2] = eff->z - BSP_FLOOR_EPS;
    box.v[3] = eff->x1; box.v[4] = eff->y1; box.v[5] = eff->z + c->height;

    root = aug_tree_root(c->a);
    if (root <= 0) return 0;

    stack = (aug_frame *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(aug_frame));
    if (!stack) return 0;
    stack[top].node = root;
    stack[top].cell.v[0] = -BIG; stack[top].cell.v[1] = -BIG; stack[top].cell.v[2] = -BIG;
    stack[top].cell.v[3] =  BIG; stack[top].cell.v[4] =  BIG; stack[top].cell.v[5] =  BIG;
    top++;

    while (top > 0) {
        aug_frame fr = stack[--top];
        const unsigned char *nd = sh_aas_rec_const(c->a, SH_AAS_L_NODES, (unsigned)fr.node);
        const unsigned char *p;
        int side, slot;
        if (!nd) continue;
        p = sh_aas_rec_const(c->a, SH_AAS_L_PLANES, (unsigned)sh_aas_get_i32(nd, ND_PLANE));
        if (!p) continue;
        side = aug_box_side(&box, p, eps);
        for (slot = 0; slot < 2; slot++) {
            int child;
            aug_box sub = fr.cell;
            if (side == 1 && slot == 1) continue;
            if (side == -1 && slot == 0) continue;
            /* Re-read the node each pass: appending a node may have moved the lump. */
            nd = sh_aas_rec_const(c->a, SH_AAS_L_NODES, (unsigned)fr.node);
            if (!nd) break;
            p = sh_aas_rec_const(c->a, SH_AAS_L_PLANES, (unsigned)sh_aas_get_i32(nd, ND_PLANE));
            if (!p) break;
            child = sh_aas_get_i32(nd, slot == 0 ? ND_CHILD0 : ND_CHILD1);
            aug_clip_cell(&sub, p, slot == 0);
            if (child > 0) {
                if (top + 1 >= cap) {
                    aug_frame *bigger;
                    unsigned ncap = cap * 2;
                    bigger = (aug_frame *)HeapReAlloc(GetProcessHeap(), 0, stack,
                                                      ncap * sizeof(aug_frame));
                    if (!bigger) { HeapFree(GetProcessHeap(), 0, stack); return carved; }
                    stack = bigger; cap = ncap;
                }
                stack[top].node = child; stack[top].cell = sub; top++;
                continue;
            }
            if (child == 0) continue;           /* void: never carved, see above */
            {
                /* Build the chain, dropping any split plane this cell already
                 * satisfies -- that is what keeps the tree shallow. */
                int keep[5], nkeep = 0, head, outside = 0, k;
                for (k = 0; k < 5; k++) {
                    const unsigned char *sp = sh_aas_rec_const(c->a, SH_AAS_L_PLANES,
                                                               (unsigned)split[k].plane);
                    int s;
                    if (!sp) { outside = 1; break; }
                    s = aug_box_side(&sub, sp, 0.0);
                    if (s == 1) continue;       /* cell wholly inside: drop the test */
                    if (s == -1) { outside = 1; break; }  /* wholly outside */
                    keep[nkeep++] = k;
                }
                if (outside) continue;
                if (nkeep == 0) {
                    /* The whole cell is the platform: replace the leaf outright. */
                    if (child == -area) continue;
                    head = -area;
                } else {
                    head = -area;
                    for (k = nkeep - 1; k >= 0; k--) {
                        head = aug_node(c, split[keep[k]].plane, split[keep[k]].f4,
                                        head, child);
                        if (head == 0) { HeapFree(GetProcessHeap(), 0, stack); return carved; }
                    }
                }
                {
                    unsigned char *w = sh_aas_rec(c->a, SH_AAS_L_NODES, (unsigned)fr.node);
                    if (!w) continue;
                    sh_aas_put_i32(w, slot == 0 ? ND_CHILD0 : ND_CHILD1, head);
                }
                carved++;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, stack);
    return carved;
}

/* ---- reachabilities ---------------------------------------------------- */

static int aug_reach(aug_ctx *c, unsigned flags, int time, int from, int to,
                     const double s[3], const double e[3])
{
    unsigned first;
    unsigned char *r;
    if (!sh_aas_append(c->a, SH_AAS_L_REACHABILITIES, 1, &first)) return 0;
    r = sh_aas_rec(c->a, SH_AAS_L_REACHABILITIES, first);
    if (!r) return 0;
    sh_aas_put_u32(r, RE_TRAVEL_FLAGS, flags);
    sh_aas_put_u16(r, RE_TRAVEL_TIME, (uint16_t)time);
    sh_aas_put_u16(r, RE_FROM_AREA, (uint16_t)from);
    sh_aas_put_u16(r, RE_TO_AREA, (uint16_t)to);
    sh_aas_put_i16(r, RE_START + 0, (int16_t)aug_clamp16(aug_trunc(s[0])));
    sh_aas_put_i16(r, RE_START + 2, (int16_t)aug_clamp16(aug_trunc(s[1])));
    sh_aas_put_i16(r, RE_START + 4, (int16_t)aug_clamp16(aug_trunc(s[2])));
    sh_aas_put_i16(r, RE_END + 0, (int16_t)aug_clamp16(aug_trunc(e[0])));
    sh_aas_put_i16(r, RE_END + 2, (int16_t)aug_clamp16(aug_trunc(e[1])));
    sh_aas_put_i16(r, RE_END + 4, (int16_t)aug_clamp16(aug_trunc(e[2])));
    return 1;
}

/* The Grid Room's own sample pattern along a shared edge: ends inset 5 units,
 * spaced by WALK_SPACING, plus one at the exact midpoint, deduplicated after
 * truncation to integers. */
static int aug_samples(float lo, float hi, double *out, int cap)
{
    double a = (double)lo + WALK_END_INSET;
    double b = (double)hi - WALK_END_INSET;
    double mid = ((double)lo + (double)hi) / 2.0;
    int n, k, count = 0, i;
    if (b < a) { a = b = mid; }
    n = (int)ceil((b - a) / (double)WALK_SPACING);
    if (n < 1) n = 1;
    for (k = 0; k <= n && count < cap; k++) {
        double v = (double)aug_trunc(a + (b - a) * (double)k / (double)n);
        int dup = 0;
        for (i = 0; i < count; i++) if (out[i] == v) { dup = 1; break; }
        if (!dup) out[count++] = v;
    }
    if (count < cap) {
        double v = (double)aug_trunc(mid);
        int dup = 0;
        for (i = 0; i < count; i++) if (out[i] == v) { dup = 1; break; }
        if (!dup) out[count++] = v;
    }
    return count;
}

/* Walk reachabilities across the shared edge of two flat areas.
 *
 * Shape copied from the Grid Room's own 100 records: endpoints one unit either
 * side of the shared edge (start inside `from`, end inside `to`), each at its
 * own area's z, sampled along the edge. */
static int aug_walk_links(aug_ctx *c, int ai, int bi)
{
    float A[5], B[5];
    double ox0, ox1, oy0, oy1, ts[64];
    int added = 0, n, k;
    if (!aug_flat_box(c->a, (unsigned)ai, A)) return 0;
    if (!aug_flat_box(c->a, (unsigned)bi, B)) return 0;
    if (fabs((double)A[4] - (double)B[4]) > (double)c->step) return 0;
    ox0 = A[0] > B[0] ? A[0] : B[0];
    ox1 = A[2] < B[2] ? A[2] : B[2];
    oy0 = A[1] > B[1] ? A[1] : B[1];
    oy1 = A[3] < B[3] ? A[3] : B[3];
    if (ox1 < ox0 || oy1 < oy0) return 0;
    if (ox1 == ox0 && oy1 > oy0) {
        double sgn = (((double)A[0] + A[2]) / 2.0 < ox0) ? 1.0 : -1.0;
        n = aug_samples((float)oy0, (float)oy1, ts, 64);
        for (k = 0; k < n; k++) {
            double s[3], e[3];
            s[0] = ox0 - sgn * REACH_SIDE_OFFSET; s[1] = ts[k]; s[2] = A[4];
            e[0] = ox0 + sgn * REACH_SIDE_OFFSET; e[1] = ts[k]; e[2] = B[4];
            if (aug_reach(c, REACH_WALK, REACH_WALK_TIME, ai, bi, s, e)) added++;
        }
    } else if (oy1 == oy0 && ox1 > ox0) {
        double sgn = (((double)A[1] + A[3]) / 2.0 < oy0) ? 1.0 : -1.0;
        n = aug_samples((float)ox0, (float)ox1, ts, 64);
        for (k = 0; k < n; k++) {
            double s[3], e[3];
            s[0] = ts[k]; s[1] = oy0 - sgn * REACH_SIDE_OFFSET; s[2] = A[4];
            e[0] = ts[k]; e[1] = oy0 + sgn * REACH_SIDE_OFFSET; e[2] = B[4];
            if (aug_reach(c, REACH_WALK, REACH_WALK_TIME, ai, bi, s, e)) added++;
        }
    }
    return added;
}

/* (axis, outward sign, area edge, wall, floor area, floor z, drop) for each
 * platform edge that has a flat floor area outside it and below it. Shared by
 * the step and fall linkers so both see exactly the same geometry. */
typedef struct aug_side {
    int   axis;         /* 0 = x, 1 = y */
    double sgn;         /* outward */
    double area_edge;
    double wall;
    int   floor_area;
    double floor_z;
    double drop;
} aug_side;

static int aug_platform_edges(aug_ctx *c, int ai, const aug_rect *eff,
                              const aug_rect *req, aug_side *out)
{
    double mx = ((double)eff->x0 + eff->x1) / 2.0;
    double my = ((double)eff->y0 + eff->y1) / 2.0;
    const int    axes[4] = { 1, 1, 0, 0 };
    const double sgns[4] = { +1.0, -1.0, -1.0, +1.0 };
    double edges[4], walls[4];
    int i, n = 0;

    edges[0] = eff->y1; walls[0] = req->y1;
    edges[1] = eff->y0; walls[1] = req->y0;
    edges[2] = eff->x0; walls[2] = req->x0;
    edges[3] = eff->x1; walls[3] = req->x1;

    for (i = 0; i < 4; i++) {
        double along = edges[i] + sgns[i] * REACH_SIDE_OFFSET;
        double px = axes[i] == 0 ? along : mx;
        double py = axes[i] == 0 ? my : along;
        int bi = sh_aas_point_area(c->a, (float)px, (float)py, eff->z);
        float tb[5];
        double drop;
        if (bi <= 0 || bi == ai) continue;
        if (!aug_flat_box(c->a, (unsigned)bi, tb)) continue;
        drop = (double)eff->z - (double)tb[4];
        if (drop <= 0.0) continue;
        out[n].axis = axes[i];
        out[n].sgn = sgns[i];
        out[n].area_edge = edges[i];
        out[n].wall = walls[i];
        out[n].floor_area = bi;
        out[n].floor_z = tb[4];
        out[n].drop = drop;
        n++;
    }
    return n;
}

/* Plain walk links for a platform inside the STEP regime.
 *
 * maxStepHeight is 18 for every monster class. Across the 26-payload donor
 * corpus, 94,327 plain-walk records join two flat areas and NOT ONE spans more
 * than that -- the observed |dz| distribution is 0 (99.09%), 1, 6, 8, 12 and 16.
 * A height change at or below it therefore needs no traversal animation at all;
 * the demon simply walks it. This is why players have always found short volumes
 * "sort of working".
 *
 * aug_walk_links cannot do this job: it fires only when two flat boxes share a
 * degenerate edge segment, and a generated platform is carved INSIDE a floor
 * slab, so their footprints overlap in 2D instead. */
static int aug_step_links(aug_ctx *c, int ai, const aug_rect *eff,
                          const aug_rect *req)
{
    aug_side sides[4];
    int n = aug_platform_edges(c, ai, eff, req, sides);
    int added = 0, i, k;
    for (i = 0; i < n; i++) {
        double lo, hi, inner, outer, ts[64];
        int cnt;
        if (sides[i].drop > (double)c->step) continue;
        lo = sides[i].axis == 0 ? eff->y0 : eff->x0;
        hi = sides[i].axis == 0 ? eff->y1 : eff->x1;
        inner = sides[i].area_edge - sides[i].sgn * REACH_SIDE_OFFSET;
        outer = sides[i].area_edge + sides[i].sgn * REACH_SIDE_OFFSET;
        cnt = aug_samples((float)lo, (float)hi, ts, 64);
        for (k = 0; k < cnt; k++) {
            double up[3], dn[3];
            if (sides[i].axis == 0) {
                up[0] = outer; up[1] = ts[k]; up[2] = sides[i].floor_z;
                dn[0] = inner; dn[1] = ts[k]; dn[2] = eff->z;
            } else {
                up[0] = ts[k]; up[1] = outer; up[2] = sides[i].floor_z;
                dn[0] = ts[k]; dn[1] = inner; dn[2] = eff->z;
            }
            /* Both endpoints must resolve to the area they claim, or the link is
             * a lie the router will act on. */
            if (sh_aas_point_area(c->a, (float)dn[0], (float)dn[1],
                                  (float)(eff->z + 2.0)) != ai) continue;
            if (sh_aas_point_area(c->a, (float)up[0], (float)up[1],
                                  (float)(sides[i].floor_z + 2.0)) != sides[i].floor_area)
                continue;
            if (aug_reach(c, REACH_WALK, REACH_WALK_TIME, sides[i].floor_area, ai, up, dn)) added++;
            if (aug_reach(c, REACH_WALK, REACH_WALK_TIME, ai, sides[i].floor_area, dn, up)) added++;
        }
    }
    return added;
}

/* One walk-off-ledge link per platform edge. See the header: AUTO honours
 * maxFallHeight, which is 0 in every monster-class module, so AUTO emits none.
 * That is deliberate -- no shipped monster-class payload contains a 0x40
 * record, and inventing one is off-precedent. */
static int aug_fall_links(aug_ctx *c, int ai, const aug_rect *eff,
                          const aug_rect *req)
{
    aug_side sides[4];
    int n, i, added = 0;
    double max_fall = sh_aas_setting_f32(c->a, SET_MAX_FALL_HEIGHT);
    double tt = sh_aas_setting_f32(c->a, SET_TT_WALK_OFF_LEDGE);
    if (c->o->fall == SH_AUG_FALL_NEVER) return 0;
    n = aug_platform_edges(c, ai, eff, req, sides);
    for (i = 0; i < n; i++) {
        double s[3], e[3], mx, my;
        int time;
        if (sides[i].drop <= (double)c->step) continue;    /* the step regime walks it */
        if (c->o->fall == SH_AUG_FALL_AUTO && sides[i].drop > max_fall) continue;
        mx = ((double)eff->x0 + eff->x1) / 2.0;
        my = ((double)eff->y0 + eff->y1) / 2.0;
        if (sides[i].axis == 0) {
            s[0] = sides[i].area_edge - sides[i].sgn * REACH_SIDE_OFFSET; s[1] = my;
            e[0] = sides[i].area_edge + sides[i].sgn * REACH_SIDE_OFFSET; e[1] = my;
        } else {
            s[0] = mx; s[1] = sides[i].area_edge - sides[i].sgn * REACH_SIDE_OFFSET;
            e[0] = mx; e[1] = sides[i].area_edge + sides[i].sgn * REACH_SIDE_OFFSET;
        }
        s[2] = eff->z;
        e[2] = sides[i].floor_z;
        time = (int)tt + aug_round(sides[i].drop * REACH_FALL_PER_UNIT);
        if (aug_reach(c, REACH_FALL, time, ai, sides[i].floor_area, s, e)) added++;
    }
    return added;
}

/* ---- baked traversals: the animated climb ------------------------------ */

/* traversalPoint is 60 bytes: six floats, eight u16, a u32, two u16, a u32,
 * then four u16 -- so the field names below ARE their offsets. */
#define TP_F0    0      /* start x/y/z */
#define TP_FC    12     /* end x/y/z */
#define TP_W18   24     /* facing, int16 fixed point */
#define TP_W1A   26
#define TP_W24   36     /* -> traversalAnimNames */
#define TP_D28   40     /* -> the paired reachability */
#define TP_W2C   44
#define TP_W2E   46
#define TP_D30   48     /* the demon, arithmetically */
#define TP_W34   52     /* from area */
#define TP_W36   54     /* to area */
#define TP_W38   56
#define TP_W3A   58

/* Copied from classic_90_climb.aas_monster48 and not understood; the comments
 * in the reference implementation say so too rather than inventing a meaning. */
#define TP_W2C_VALUE  0xFFFF
#define TP_W2E_VALUE  0xFFFF
#define TP_W38_VALUE  128

/* The int16 fixed-point unit of the facing vector. 0x7FFE on every record of
 * our donor and on 214 of 258 in the larger sample. */
#define TP_DIR_UNIT   32766

/* How far inside the platform area the platform-side endpoint sits. OUR
 * convention, not a donor value: the animation's root-motion distance is
 * carried by neither the AAS nor the traversal table, so nothing fixes it. One
 * unit in is what the module's own walk reachabilities use, and it is what
 * guarantees the point resolves to the platform area. */
#define TRAVERSAL_INNER_INSET  REACH_SIDE_OFFSET

typedef struct aug_trav_spec {
    unsigned travel_flags;
    unsigned d30;
    int      travel_time;
    int      from_area, to_area;
    double   start[3], end[3];
    double   dir[2];
    char     anim[SH_TRAV_PATH_CAP];
    int      anim_index;    /* resolved into traversalAnimNames before writing */
} aug_trav_spec;

#define AUG_MAX_TRAVERSALS 2048

/* Candidate anchor positions along one platform edge, MIDPOINT FIRST.
 *
 * A climb link needs two points to land where they claim: an inner point on the
 * platform and an outer point on the floor below, the latter sitting the chosen
 * animation's own offset.x out from the wall. That offset is PER DEMON -- a demon
 * whose clip starts further out needs more clear floor than one whose clip starts
 * close in -- so a single anchor makes the two demons' endpoints land in different
 * places, and one of them can miss the floor area (past its far edge, inside a
 * wall, or on top of a neighbouring platform) while the other is fine.
 *
 * Sampling only the midpoint therefore dropped whole demons from an edge for a
 * reason that had nothing to do with whether they can make the climb, which is
 * what "the big demons cannot use my platform" looks like from the outside.
 * Trying the midpoint first keeps every link the single-anchor build already
 * produced exactly where it was; the rest are only reached by a demon the
 * midpoint would have lost entirely. */
static int aug_trav_anchors(double lo, double hi, double *out, int cap)
{
    double mid;
    double ts[24];
    int n, i, count = 0;

    if (cap <= 0) return 0;
    if (hi < lo) { double t = lo; lo = hi; hi = t; }

    mid = (lo + hi) / 2.0;
    out[count++] = mid;

    n = aug_samples((float)lo, (float)hi, ts, (int)(sizeof ts / sizeof ts[0]));
    for (i = 0; i < n && count < cap; i++) {
        int dup = 0, k;
        for (k = 0; k < count; k++) if (out[k] == ts[i]) { dup = 1; break; }
        if (!dup) out[count++] = ts[i];
    }
    return count;
}

/* Collect up- and down-climb specs for one platform: one per demon per usable
 * edge, in both directions.
 *
 * The ledge lip is the REQUESTED rectangle, not the inset one -- the wall face
 * is where the geometry actually is, and the area is inset by the agent radius
 * exactly as the module's own floor areas are. The floor-side endpoint then sits
 * the animation's own offset.x outside that lip, which is what reproduces the
 * start positions of all fourteen traversals in our donor module.
 *
 * Every endpoint is checked with the BSP. A point that does not land in the area
 * it claims is dropped rather than written: the router would act on the lie. */
static int aug_traversal_specs(aug_ctx *c, int ai, const aug_rect *eff,
                               const aug_rect *req, aug_trav_spec *out, int cap)
{
    aug_side sides[4];
    int n, i, k, d, count = 0;

    if (c->o->traversal == SH_AUG_TRAVERSAL_NEVER) return 0;
    if (!sh_trav_ready()) return 0;

    n = aug_platform_edges(c, ai, eff, req, sides);
    for (i = 0; i < n; i++) {
        double anchors[24];
        double inner = sides[i].area_edge - sides[i].sgn * TRAVERSAL_INNER_INSET;
        int na;
        if (sides[i].drop <= (double)c->step) continue;   /* the step regime walks it */

        /* The edge runs along the axis the side does NOT face. */
        na = aug_trav_anchors(sides[i].axis == 0 ? (double)eff->y0 : (double)eff->x0,
                              sides[i].axis == 0 ? (double)eff->y1 : (double)eff->x1,
                              anchors, (int)(sizeof anchors / sizeof anchors[0]));

        for (d = 0; d < 2; d++) {                          /* UP then DOWN */
            int up = (d == SH_TRAV_UP);
            for (k = 0; k < sh_trav_monster_count() && count < cap; k++) {
                const sh_trav_monster *m = sh_trav_monster_at(k);
                char path[SH_TRAV_PATH_CAP];
                float off = 0.0f;
                int dist = 0, time = 0, a, placed = 0;
                double outer, inner_pt[3], outer_pt[3], dir;
                aug_trav_spec *s;

                if (!m) continue;
                if (!sh_trav_select(m, d, (float)sides[i].drop, path, sizeof path,
                                    &off, &dist, &time))
                    continue;                               /* this demon cannot */

                if (off < 0.0f) off = -off;
                outer = sides[i].wall + sides[i].sgn * (double)off;

                /* This demon's own pair of endpoints, tried along the edge until
                 * both land where they claim. The offset is the demon's, so the
                 * answer is too: an anchor that fails for one can serve another. */
                for (a = 0; a < na; a++) {
                    if (sides[i].axis == 0) {
                        inner_pt[0] = inner; inner_pt[1] = anchors[a];
                        outer_pt[0] = outer; outer_pt[1] = anchors[a];
                    } else {
                        inner_pt[0] = anchors[a]; inner_pt[1] = inner;
                        outer_pt[0] = anchors[a]; outer_pt[1] = outer;
                    }
                    inner_pt[2] = eff->z;
                    outer_pt[2] = sides[i].floor_z;
                    if (sh_aas_point_area(c->a, (float)inner_pt[0], (float)inner_pt[1],
                                          (float)(eff->z + 2.0)) != ai) continue;
                    if (sh_aas_point_area(c->a, (float)outer_pt[0], (float)outer_pt[1],
                                          (float)(sides[i].floor_z + 2.0)) != sides[i].floor_area)
                        continue;
                    placed = 1;
                    break;
                }
                if (!placed) continue;      /* nowhere on this edge works for it */

                s = &out[count++];
                memset(s, 0, sizeof *s);
                s->travel_flags = m->travel_flags;
                s->d30 = m->d30;
                s->travel_time = time;
                s->from_area = up ? sides[i].floor_area : ai;
                s->to_area   = up ? ai : sides[i].floor_area;
                memcpy(s->start, up ? outer_pt : inner_pt, sizeof s->start);
                memcpy(s->end,   up ? inner_pt : outer_pt, sizeof s->end);
                /* Facing is the XY travel direction: inward climbing up, outward
                 * dropping back down. */
                dir = up ? -sides[i].sgn : sides[i].sgn;
                s->dir[0] = sides[i].axis == 0 ? dir : 0.0;
                s->dir[1] = sides[i].axis == 0 ? 0.0 : dir;
                _snprintf_s(s->anim, sizeof s->anim, _TRUNCATE, "%s", path);
            }
        }
    }
    return count;
}

/* Write the specs as reachabilities, traversalPoints and animation names.
 *
 * The layout is copied from a shipped donor: the traversal reachabilities are a
 * contiguous TAIL of the reachability array, traversalPoints[0] is a dummy the
 * engine's own validation deliberately skips ("traversal point %d has an invalid
 * start area" only fires for index > 0), and the real points are grouped by
 * from_area so that each area's first_trav_point / num_trav_point partition
 * them.
 *
 * Returns the number written. */
static int aug_emit_traversals(aug_ctx *c, aug_trav_spec *specs, int n)
{
    unsigned base, first, i;
    int k, j, written = 0;
    unsigned na;

    if (n <= 0) return 0;
    /* Regrouping an existing traversal set would move indices other records
     * already point at. No SnapMap module ships one, so refuse rather than
     * guess. */
    if (sh_aas_count(c->a, SH_AAS_L_TRAVERSALPOINTS) > 1) return 0;

    /* Animation names, deduplicated by path. */
    for (k = 0; k < n; k++) {
        unsigned cnt = sh_aas_count(c->a, SH_AAS_L_TRAVERSALANIMNAMES);
        int found = -1;
        for (i = 0; i < cnt; i++) {
            const unsigned char *r = sh_aas_rec_const(c->a, SH_AAS_L_TRAVERSALANIMNAMES, i);
            if (r && strncmp((const char *)r, specs[k].anim, SH_TRAV_PATH_CAP) == 0) {
                found = (int)i;
                break;
            }
        }
        if (found < 0) {
            unsigned char *r;
            if (!sh_aas_append(c->a, SH_AAS_L_TRAVERSALANIMNAMES, 1, &first)) return written;
            r = sh_aas_rec(c->a, SH_AAS_L_TRAVERSALANIMNAMES, first);
            if (!r) return written;
            memset(r, 0, SH_TRAV_PATH_CAP);
            _snprintf_s((char *)r, SH_TRAV_PATH_CAP, _TRUNCATE, "%s", specs[k].anim);
            found = (int)first;
        }
        specs[k].anim_index = found;
    }

    /* The dummy record every shipped file starts with. */
    if (sh_aas_count(c->a, SH_AAS_L_TRAVERSALPOINTS) == 0) {
        unsigned char *tp;
        if (!sh_aas_append(c->a, SH_AAS_L_TRAVERSALPOINTS, 1, &first)) return written;
        tp = sh_aas_rec(c->a, SH_AAS_L_TRAVERSALPOINTS, first);
        if (!tp) return written;
        memset(tp, 0, sh_aas_record_size(SH_AAS_L_TRAVERSALPOINTS));
        sh_aas_put_u16(tp, TP_W24, 0xFFFF);
        sh_aas_put_u16(tp, TP_W2C, TP_W2C_VALUE);
        sh_aas_put_u16(tp, TP_W2E, TP_W2E_VALUE);
        sh_aas_put_u16(tp, TP_W38, TP_W38_VALUE);
    }

    /* Group by from_area, then to_area, so the per-area partition below is a
     * contiguous run for each area. Insertion sort: n is small and stability
     * keeps the per-demon order deterministic. */
    for (k = 1; k < n; k++) {
        aug_trav_spec key = specs[k];
        j = k - 1;
        while (j >= 0 && (specs[j].from_area > key.from_area ||
                          (specs[j].from_area == key.from_area &&
                           specs[j].to_area > key.to_area))) {
            specs[j + 1] = specs[j];
            j--;
        }
        specs[j + 1] = key;
    }

    base = sh_aas_count(c->a, SH_AAS_L_REACHABILITIES);
    for (k = 0; k < n; k++) {
        unsigned char *tp;
        if (!aug_reach(c, specs[k].travel_flags, specs[k].travel_time,
                       specs[k].from_area, specs[k].to_area,
                       specs[k].start, specs[k].end)) break;
        if (!sh_aas_append(c->a, SH_AAS_L_TRAVERSALPOINTS, 1, &first)) break;
        tp = sh_aas_rec(c->a, SH_AAS_L_TRAVERSALPOINTS, first);
        if (!tp) break;
        memset(tp, 0, sh_aas_record_size(SH_AAS_L_TRAVERSALPOINTS));
        sh_aas_put_f32(tp, TP_F0 + 0, (float)specs[k].start[0]);
        sh_aas_put_f32(tp, TP_F0 + 4, (float)specs[k].start[1]);
        sh_aas_put_f32(tp, TP_F0 + 8, (float)specs[k].start[2]);
        sh_aas_put_f32(tp, TP_FC + 0, (float)specs[k].end[0]);
        sh_aas_put_f32(tp, TP_FC + 4, (float)specs[k].end[1]);
        sh_aas_put_f32(tp, TP_FC + 8, (float)specs[k].end[2]);
        sh_aas_put_u16(tp, TP_W18, (uint16_t)(int16_t)aug_round(specs[k].dir[0] * TP_DIR_UNIT));
        sh_aas_put_u16(tp, TP_W1A, (uint16_t)(int16_t)aug_round(specs[k].dir[1] * TP_DIR_UNIT));
        sh_aas_put_u16(tp, TP_W24, (uint16_t)specs[k].anim_index);
        sh_aas_put_u32(tp, TP_D28, base + (unsigned)k);
        sh_aas_put_u16(tp, TP_W2C, TP_W2C_VALUE);
        sh_aas_put_u16(tp, TP_W2E, TP_W2E_VALUE);
        sh_aas_put_u32(tp, TP_D30, specs[k].d30);
        sh_aas_put_u16(tp, TP_W34, (uint16_t)specs[k].from_area);
        sh_aas_put_u16(tp, TP_W36, (uint16_t)specs[k].to_area);
        sh_aas_put_u16(tp, TP_W38, TP_W38_VALUE);
        sh_aas_put_u16(tp, TP_W3A, 0);
        written++;
    }

    /* Per-area ownership of the point range. Record 0 is the dummy and belongs
     * to nobody. */
    na = sh_aas_count(c->a, SH_AAS_L_AREAS);
    for (i = 0; i < na; i++) {
        unsigned char *ar = sh_aas_rec(c->a, SH_AAS_L_AREAS, i);
        if (!ar) continue;
        sh_aas_put_u16(ar, AR_FIRST_TRAV_POINT, 0);
        sh_aas_put_u16(ar, AR_NUM_TRAV_POINT, 0);
    }
    {
        unsigned np = sh_aas_count(c->a, SH_AAS_L_TRAVERSALPOINTS);
        for (i = 1; i < np; i++) {
            const unsigned char *tp = sh_aas_rec_const(c->a, SH_AAS_L_TRAVERSALPOINTS, i);
            unsigned char *ar;
            unsigned owner;
            if (!tp) continue;
            owner = sh_aas_get_u16(tp, TP_W34);
            if (owner >= na) continue;
            ar = sh_aas_rec(c->a, SH_AAS_L_AREAS, owner);
            if (!ar) continue;
            if (sh_aas_get_u16(ar, AR_NUM_TRAV_POINT) == 0)
                sh_aas_put_u16(ar, AR_FIRST_TRAV_POINT, (uint16_t)i);
            sh_aas_put_u16(ar, AR_NUM_TRAV_POINT,
                           (uint16_t)(sh_aas_get_u16(ar, AR_NUM_TRAV_POINT) + 1));
        }
    }
    return written;
}

/* Reproduce the fix-up idAAS2File::Load runs after parsing: for each
 * reachability in order, push it onto the front of its from-area's and
 * to-area's chains. Without this the engine's own per-area lists are empty and
 * every link we wrote is invisible. */
static void aug_relink(aug_ctx *c)
{
    unsigned na = sh_aas_count(c->a, SH_AAS_L_AREAS);
    unsigned nr = sh_aas_count(c->a, SH_AAS_L_REACHABILITIES);
    unsigned i;
    for (i = 0; i < na; i++) {
        unsigned char *ar = sh_aas_rec(c->a, SH_AAS_L_AREAS, i);
        if (!ar) continue;
        sh_aas_put_i32(ar, AR_FIRST_REACH_FROM, -1);
        sh_aas_put_i32(ar, AR_FIRST_REACH_TO, -1);
    }
    for (i = 0; i < nr; i++) {
        unsigned char *r = sh_aas_rec(c->a, SH_AAS_L_REACHABILITIES, i);
        unsigned char *ar;
        unsigned from, to;
        if (!r) continue;
        from = sh_aas_get_u16(r, RE_FROM_AREA);
        to = sh_aas_get_u16(r, RE_TO_AREA);
        if (from < na) {
            ar = sh_aas_rec(c->a, SH_AAS_L_AREAS, from);
            if (ar) {
                sh_aas_put_i32(r, RE_NEXT_FROM, sh_aas_get_i32(ar, AR_FIRST_REACH_FROM));
                sh_aas_put_i32(ar, AR_FIRST_REACH_FROM, (int32_t)i);
            }
        }
        if (to < na) {
            ar = sh_aas_rec(c->a, SH_AAS_L_AREAS, to);
            if (ar) {
                sh_aas_put_i32(r, RE_NEXT_TO, sh_aas_get_i32(ar, AR_FIRST_REACH_TO));
                sh_aas_put_i32(ar, AR_FIRST_REACH_TO, (int32_t)i);
            }
        }
    }
}

/* ---- admission --------------------------------------------------------- */

/* Clearance above a platform: the distance to the lowest thing that overlaps it
 * in XY and sits above it, counting both the module's own areas and the other
 * platforms in this bake. */
static double aug_headroom(aug_ctx *c, const aug_rect *p,
                           const sh_aug_platform *all, int n, int self)
{
    double best = 1e30;
    unsigned i, na = sh_aas_count(c->a, SH_AAS_L_AREAS);
    int k;
    for (i = 1; i < na; i++) {
        const unsigned char *b = sh_aas_rec_const(c->a, SH_AAS_L_AREABOUNDS, i);
        double minz;
        if (!b) continue;
        minz = sh_aas_get_i16(b, AB_MINZ);
        if (minz <= p->z) continue;
        if (sh_aas_get_i16(b, AB_MAXX) <= p->x0 || sh_aas_get_i16(b, AB_MINX) >= p->x1) continue;
        if (sh_aas_get_i16(b, AB_MAXY) <= p->y0 || sh_aas_get_i16(b, AB_MINY) >= p->y1) continue;
        if (minz - p->z < best) best = minz - p->z;
    }
    for (k = 0; k < n; k++) {
        if (k == self) continue;
        if (all[k].z <= p->z) continue;
        if (all[k].x1 <= p->x0 || all[k].x0 >= p->x1) continue;
        if (all[k].y1 <= p->y0 || all[k].y0 >= p->y1) continue;
        if (all[k].z - p->z < best) best = all[k].z - p->z;
    }
    return best;
}

/* ---- the driver -------------------------------------------------------- */

int sh_aas_augment(sh_aas *a, const sh_aug_platform *plats, int n,
                   const sh_aug_opts *opts, sh_aug_report *out)
{
    static const sh_aug_opts defaults = { SH_AUG_FALL_AUTO, 1, 0 };
    aug_ctx c;
    int order[SH_AUG_MAX_PLATFORMS];
    int made[SH_AUG_MAX_PLATFORMS];
    aug_rect effs[SH_AUG_MAX_PLATFORMS];
    aug_rect reqs[SH_AUG_MAX_PLATFORMS];
    float mins[3], maxs[3], fw, fd;
    int i, j, k, nmade = 0;

    if (!a || !out) return 0;
    memset(out, 0, sizeof *out);
    if (!opts) opts = &defaults;
    if (n < 0) n = 0;
    if (n > SH_AUG_MAX_PLATFORMS) n = SH_AUG_MAX_PLATFORMS;

    c.a = a; c.o = opts; c.rep = out; c.failed = 0;
    c.radius = opts->inset ? aug_agent_radius(a) : 0.0f;
    c.height = aug_agent_height(a);
    c.step = sh_aas_setting_f32(a, SET_MAX_STEP_HEIGHT);
    sh_aas_agent_bounds(a, mins, maxs);
    fw = maxs[0] - mins[0];
    fd = maxs[1] - mins[1];

    out->areas_before = sh_aas_count(a, SH_AAS_L_AREAS);
    out->reach_before = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    out->depth_before = sh_aas_tree_depth(a);
    out->platform_count = n;

    /* Lowest first, so a platform that carries another is already in the tree
     * when the one above it looks for its carrier. */
    for (i = 0; i < n; i++) order[i] = i;
    for (i = 1; i < n; i++) {
        int key = order[i];
        for (j = i - 1; j >= 0 && plats[order[j]].z > plats[key].z; j--)
            order[j + 1] = order[j];
        order[j + 1] = key;
    }

    for (k = 0; k < n; k++) {
        int idx = order[k];
        sh_aug_platform_result *pr = &out->platforms[idx];
        const sh_aug_platform *p = &plats[idx];
        aug_rect req, eff;
        int carrier, area;
        double head;

        memcpy(pr->name, p->name, sizeof pr->name);
        pr->name[sizeof pr->name - 1] = 0;
        pr->area = -1;
        pr->carrier = -1;

        req.x0 = p->x0 < p->x1 ? p->x0 : p->x1;
        req.x1 = p->x0 < p->x1 ? p->x1 : p->x0;
        req.y0 = p->y0 < p->y1 ? p->y0 : p->y1;
        req.y1 = p->y0 < p->y1 ? p->y1 : p->y0;
        req.z = p->z;
        eff.x0 = req.x0 + c.radius; eff.x1 = req.x1 - c.radius;
        eff.y0 = req.y0 + c.radius; eff.y1 = req.y1 - c.radius;
        eff.z = req.z;

        if (eff.x1 - eff.x0 < fw || eff.y1 - eff.y0 < fd) {
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "too small: %.0fx%.0f after the %.0f-unit agent-radius inset, "
                        "this demon size needs %.0fx%.0f",
                        eff.x1 - eff.x0, eff.y1 - eff.y0, c.radius, fw, fd);
            continue;
        }
        head = aug_headroom(&c, &req, plats, n, idx);
        if (head < (double)c.height) {
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "not enough headroom: %.0f units of clearance, this demon "
                        "size is %.0f tall", head, c.height);
            continue;
        }
        if (eff.x0 <= AUG_INT16_LO || eff.x1 >= AUG_INT16_HI ||
            eff.y0 <= AUG_INT16_LO || eff.y1 >= AUG_INT16_HI ||
            eff.z  <= AUG_INT16_LO || eff.z  >= AUG_INT16_HI) {
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "outside the coordinate range navigation bounds can hold "
                        "(+/-32767)");
            continue;
        }

        carrier = sh_aas_point_area(a, (eff.x0 + eff.x1) / 2.0f,
                                    (eff.y0 + eff.y1) / 2.0f, eff.z);
        area = aug_add_area(&c, &eff, carrier);
        if (area < 0) {
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "the navigation file is full");
            c.failed = 1;
            break;
        }
        pr->emitted = 1;
        pr->area = area;
        pr->carrier = carrier;
        pr->leaf_slots_carved = aug_carve(&c, area, &eff);
        effs[nmade] = eff;
        reqs[nmade] = req;
        made[nmade] = area;
        nmade++;
    }

    if (!c.failed) {
        /* Walk links first, so the step linker knows which pairs are covered.
         * A generated area is carved inside a floor slab, so in practice the
         * walk linker rarely fires and the step linker does the work -- both are
         * run because a platform butting exactly against another area is the
         * case walk links exist for. */
        for (i = 0; i < nmade; i++) {
            unsigned na = sh_aas_count(a, SH_AAS_L_AREAS);
            unsigned b;
            for (b = 1; b < na; b++) {
                if ((int)b == made[i]) continue;
                aug_walk_links(&c, made[i], (int)b);
                aug_walk_links(&c, (int)b, made[i]);
            }
        }
        for (i = 0; i < nmade; i++) aug_step_links(&c, made[i], &effs[i], &reqs[i]);
        for (i = 0; i < nmade; i++) aug_fall_links(&c, made[i], &effs[i], &reqs[i]);

        /* Climbs LAST. The traversal reachabilities must be a contiguous tail of
         * the reachability array -- that is how every shipped donor lays them
         * out, and traversalPoint.d28 indexes into it. */
        {
            aug_trav_spec *specs = (aug_trav_spec *)HeapAlloc(
                GetProcessHeap(), 0, AUG_MAX_TRAVERSALS * sizeof(aug_trav_spec));
            if (specs) {
                int total = 0;
                for (i = 0; i < nmade; i++) {
                    int got = aug_traversal_specs(&c, made[i], &effs[i], &reqs[i],
                                                  specs + total,
                                                  AUG_MAX_TRAVERSALS - total);
                    total += got;
                }
                if (total > 0) aug_emit_traversals(&c, specs, total);
                HeapFree(GetProcessHeap(), 0, specs);
            }
        }
        aug_relink(&c);
    }

    /* trees[0].c is the number of distinct areas the tree references plus one,
     * true in 20/20 shipped payloads. Areas were added, so it must move. */
    if (sh_aas_count(a, SH_AAS_L_TREES) > 0) {
        unsigned char *t = sh_aas_rec(a, SH_AAS_L_TREES, 0);
        if (t) sh_aas_put_i32(t, TR_C, (int32_t)sh_aas_count(a, SH_AAS_L_AREAS));
    }

    /* Report what each platform actually got, including whether it ended up an
     * island. An island is legal and sometimes wanted -- the Grid Room's own
     * ceiling is one -- but the author has to be told, because a platform they
     * expected demons to climb onto is a different thing from one they meant to
     * spawn demons on. */
    for (i = 0; i < nmade; i++) {
        int links = 0, climbs = 0;
        unsigned seen[SH_TRAV_MAX_MONSTERS];
        int nseen = 0;
        unsigned r, nr = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
        for (r = 0; r < nr; r++) {
            const unsigned char *rr = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, r);
            unsigned flags;
            if (!rr) continue;
            if ((int)sh_aas_get_u16(rr, RE_FROM_AREA) != made[i] &&
                (int)sh_aas_get_u16(rr, RE_TO_AREA) != made[i]) continue;
            links++;
            /* A traversal names its demon in the high half of travel_flags; a
             * walk or fall link is 0x20 / 0x40 and names nobody. */
            flags = sh_aas_get_u32(rr, RE_TRAVEL_FLAGS);
            if ((flags >> 16) != 0) {
                int k, dup = 0;
                climbs++;
                for (k = 0; k < nseen; k++) if (seen[k] == flags) { dup = 1; break; }
                if (!dup && nseen < SH_TRAV_MAX_MONSTERS) seen[nseen++] = flags;
            }
        }
        for (j = 0; j < n; j++) {
            if (out->platforms[j].area == made[i]) {
                out->platforms[j].links = links;
                out->platforms[j].climbs = climbs;
                out->platforms[j].demons = nseen;
                out->platforms[j].island = (links == 0);
                break;
            }
        }
    }

    out->areas_after = sh_aas_count(a, SH_AAS_L_AREAS);
    out->reach_after = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
    out->depth_after = sh_aas_tree_depth(a);
    out->depth_exceeded = out->depth_after > (unsigned)SH_AAS_MAX_DEPTH;

    return c.failed ? 0 : 1;
}

#ifdef SH_AUG_TESTING
int sh_aug_test_trav_anchors(double lo, double hi, double *out, int cap)
{
    return aug_trav_anchors(lo, hi, out, cap);
}
#endif
