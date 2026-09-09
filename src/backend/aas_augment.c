/* aas_augment.c -- see aas_augment.h for what this is and why.
 *
 * Every constant below was measured against shipped data. Where a value is
 * copied without being understood, the comment says so rather than inventing a
 * justification for it.
 *
 * THE SHAPE OF THIS FILE
 * ----------------------
 * It is long, and deliberately one file. The obvious split -- lifting the edge
 * and link machinery out -- was measured and declined: it moves 891 lines but
 * has to export `aug_ctx` and `aug_quad`, the whole data model, so the two
 * halves would still change together. That is a header to maintain, not a
 * seam. This map is the answer to the real cost instead.
 *
 *   measured constants        the numbers, each with the corpus behind it
 *   small helpers             rounding, clamping, the agent's box
 *   BSP queries               tree root, depth, and the point-to-area walk
 *   the quad                  a walkable surface: four corners and a plane.
 *                             aug_z_at, the inward edge normal, the inset.
 *                             THE EDGE NORMAL'S SIGN IS LOAD-BEARING -- read
 *                             the comment on aug_edge_normal_in before touching
 *                             anything geometric.
 *   interning                 vertices, edges, planes, nodes, deduplicated
 *   area geometry             bounds, clusters, obstaclePVS, aug_add_area
 *   the BSP splice            aug_carve: five planes per platform
 *   reachabilities            aug_reach, and walk links between flat boxes
 *   edges and neighbours      the segment model: who is across each part of
 *                             each edge, how far, and how much higher. Two
 *                             neighbour sources, and they are not symmetric.
 *   link regimes              step, fall, climb and leap, chosen by magnitude
 *                             and direction rather than by a signed drop
 *   baked traversals          the five records a climb or a leap writes
 *   admission                 headroom and the platform bounds
 *   the driver                sh_aas_augment: two passes, areas then links
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
/* node.field4 is 1 exactly when the split plane HAS a z component, and 0
 * otherwise -- including for a YAWED VERTICAL plane, which is still 0. Measured
 * over 12,450 classified nodes in ten shipped payloads: axis-aligned XY 5,623 at
 * 0, yawed vertical 5,120 at 0, axis-aligned Z 1,579 at 1, oblique 128 at 1.
 * 100% consistent. A "Z versus XY" rule gets the yawed-vertical case wrong. */
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
/* An edge only one area uses, and one two areas share. The demotion below fires
 * when a second area asks for an edge that already exists.
 *
 * For GENERATED geometry it now essentially never fires: aug_vertex interns by
 * exact float equality, and two abutting ORIENTED quads -- inset by the agent
 * radius, each on its own plane -- will not produce bit-identical corners. That
 * costs nothing here, because nothing in the augmenter routes on shared edges;
 * chained platforms are joined by reachabilities, not by shared geometry. */
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
/* minFloorCos -- the walkable-slope threshold, 0.7 (45.57 degrees) in every
 * shipped payload and identical across all three monster classes.
 *
 * Word 16, NOT 17. The disk settings record is not a packed image of
 * idAAS2Settings: it drops maxLedgeGrabHeight among others and reorders after
 * word 23, so extrapolating the struct layout lands one word late on a value
 * (minHighCeiling, 80) that is not a cosine at all. The offset comes from the
 * decoded corpus. */
#define SET_MIN_FLOOR_COS       SET_W(16)
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

/* A walkable surface: a planar convex quad wound CLOCKWISE seen from +Z.
 *
 * Not a rect. A Blocking Box can be yawed, tilted, or lying on its side, so the
 * surface has four corners each with their own z and there is no single `z` any
 * more -- `n`,`d` are its supporting plane (n.p + d = 0, n[2] > 0) and the
 * height at a point is aug_z_at.
 *
 * `x0..y1` is the XY BOUNDING BOX, kept for cheap rejection and for the record
 * fields that genuinely want an AABB. It is NOT the footprint: for a yawed quad
 * it is strictly larger, and testing containment against it is the bug that
 * makes a rotated platform swallow its own bounding square. */
typedef struct aug_quad {
    double c[4][3];
    double n[3];
    double d;
    double x0, y0, x1, y1;
} aug_quad;

static int aug_quad_init(aug_quad *q, const double c[4][3])
{
    double u[3], v[3], len;
    int i, k;

    for (i = 0; i < 4; i++)
        for (k = 0; k < 3; k++) q->c[i][k] = c[i][k];
    for (k = 0; k < 3; k++) { u[k] = c[1][k] - c[0][k]; v[k] = c[3][k] - c[0][k]; }
    q->n[0] = u[1]*v[2] - u[2]*v[1];
    q->n[1] = u[2]*v[0] - u[0]*v[2];
    q->n[2] = u[0]*v[1] - u[1]*v[0];
    len = sqrt(q->n[0]*q->n[0] + q->n[1]*q->n[1] + q->n[2]*q->n[2]);
    if (len < 1e-6) return 0;
    for (k = 0; k < 3; k++) q->n[k] /= len;
    if (q->n[2] < 0.0) for (k = 0; k < 3; k++) q->n[k] = -q->n[k];
    if (q->n[2] < 1e-6) return 0;            /* vertical: nothing to stand on */
    q->d = -(q->n[0]*c[0][0] + q->n[1]*c[0][1] + q->n[2]*c[0][2]);
    q->x0 = q->x1 = c[0][0];
    q->y0 = q->y1 = c[0][1];
    for (i = 1; i < 4; i++) {
        if (c[i][0] < q->x0) q->x0 = c[i][0];
        if (c[i][0] > q->x1) q->x1 = c[i][0];
        if (c[i][1] < q->y0) q->y0 = c[i][1];
        if (c[i][1] > q->y1) q->y1 = c[i][1];
    }
    return 1;
}

static int aug_quad_from_platform(aug_quad *q, const sh_aug_platform *p)
{
    double c[4][3];
    int i, k;
    for (i = 0; i < 4; i++)
        for (k = 0; k < 3; k++) c[i][k] = p->c[i][k];
    return aug_quad_init(q, c);
}

/* The surface height at (x,y). Defined across the WHOLE plane, not only inside
 * the quad, because probes and link endpoints sit just outside an edge. */
static double aug_z_at(const aug_quad *q, double x, double y)
{
    return -(q->n[0]*x + q->n[1]*y + q->d) / q->n[2];
}

static double aug_quad_min_z(const aug_quad *q)
{
    double v = q->c[0][2];
    int i;
    for (i = 1; i < 4; i++) if (q->c[i][2] < v) v = q->c[i][2];
    return v;
}

static double aug_quad_max_z(const aug_quad *q)
{
    double v = q->c[0][2];
    int i;
    for (i = 1; i < 4; i++) if (q->c[i][2] > v) v = q->c[i][2];
    return v;
}

/* The INWARD XY normal of edge i, which runs c[i] -> c[i+1].
 *
 * The winding is CLOCKWISE seen from +Z, so inward is the edge direction rotated
 * MINUS 90 degrees: (ey, -ex). Check it on the edge (x0,y1) -> (x1,y1):
 * e = (+1,0), inward = (0,-1), and the interior is indeed at y < y1.
 *
 * The +90 rotation (-ey, ex) points OUTWARD, and using it here would invert the
 * inset, containment, the carve's lateral planes and every neighbour probe at
 * once -- one sign, four silent failures. */
static void aug_edge_normal_in(const aug_quad *q, int i, double out[2])
{
    int j = (i + 1) & 3;
    double ex = q->c[j][0] - q->c[i][0];
    double ey = q->c[j][1] - q->c[i][1];
    double len = sqrt(ex*ex + ey*ey);
    if (len < 1e-9) { out[0] = out[1] = 0.0; return; }
    out[0] =  ey / len;
    out[1] = -ex / len;
}

/* Offset every edge inward by r and re-intersect.
 *
 * Exact for a convex quad, where the axis-wise +/- radius inset this replaces is
 * correct only for an axis-aligned rectangle -- inset a 45-degree quad by its
 * AABB and the corners are eaten, refusing platforms that are actually large
 * enough for the agent.
 *
 * Corner i of the result is the intersection of edges i-1 and i, because edge
 * i-1 ends at c[i] and edge i starts there. Returns 0 if the quad collapses or
 * turns itself inside out. */
static int aug_quad_inset(const aug_quad *in, double r, aug_quad *out)
{
    double nx[4], ny[4], off[4], c[4][3], sh = 0.0;
    int i;

    if (r <= 0.0) { *out = *in; return 1; }
    for (i = 0; i < 4; i++) {
        double n2[2];
        aug_edge_normal_in(in, i, n2);
        if (n2[0] == 0.0 && n2[1] == 0.0) return 0;
        nx[i] = n2[0]; ny[i] = n2[1];
        off[i] = nx[i]*in->c[i][0] + ny[i]*in->c[i][1] + r;
    }
    for (i = 0; i < 4; i++) {
        int pv = (i + 3) & 3;
        double det = nx[pv]*ny[i] - nx[i]*ny[pv];
        if (det > -1e-9 && det < 1e-9) return 0;
        c[i][0] = (off[pv]*ny[i] - off[i]*ny[pv]) / det;
        c[i][1] = (nx[pv]*off[i] - nx[i]*off[pv]) / det;
        c[i][2] = aug_z_at(in, c[i][0], c[i][1]);
    }
    for (i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        sh += c[i][0]*c[j][1] - c[j][0]*c[i][1];
    }
    if (sh > -1.0) return 0;             /* collapsed, or wound the other way */
    /* Winding is NOT enough. Over-inset a small quad and it turns inside out
     * through itself while STAYING clockwise: a 30x30 square inset by 24 lands
     * on (24,6),(6,6),(6,24),(24,24), whose shoelace is -648 -- still negative,
     * still "valid", and completely wrong.
     *
     * The test that does hold is convexity against the inset half-planes: for a
     * genuine inset every corner sits on the inward side of every edge. In the
     * flipped case corner 0 is 18 units on the WRONG side of edge 1. */
    for (i = 0; i < 4; i++) {
        int j;
        for (j = 0; j < 4; j++)
            if (nx[j]*c[i][0] + ny[j]*c[i][1] < off[j] - 1e-6) return 0;
    }
    return aug_quad_init(out, c);
}

/* Inside the quad in XY. The AABB is only the cheap reject; the real test is the
 * sign of every edge's inward normal. */
static int aug_quad_contains_xy(const aug_quad *q, double x, double y)
{
    int i;
    if (x < q->x0 - 1e-6 || x > q->x1 + 1e-6 ||
        y < q->y0 - 1e-6 || y > q->y1 + 1e-6) return 0;
    for (i = 0; i < 4; i++) {
        double n2[2];
        aug_edge_normal_in(q, i, n2);
        if (n2[0]*(x - q->c[i][0]) + n2[1]*(y - q->c[i][1]) < -1e-6) return 0;
    }
    return 1;
}

/* The narrowest the quad gets, edge to opposite corners. For a rotated quad the
 * AABB overstates usable size, so this is what the agent footprint is compared
 * against. */
static double aug_quad_min_width(const aug_quad *q)
{
    double best = 1e30;
    int i, k;
    for (i = 0; i < 4; i++) {
        double n2[2];
        aug_edge_normal_in(q, i, n2);
        for (k = 0; k < 4; k++) {
            double dist = n2[0]*(q->c[k][0] - q->c[i][0]) + n2[1]*(q->c[k][1] - q->c[i][1]);
            if (dist > 1e-6 && dist < best) best = dist;
        }
    }
    return best;
}

/* node.field4 from a split plane's z component. See the NODE_F4_* comment: the
 * rule is "has a z component", not "is a Z plane". */
static int aug_node_field4(double pc)
{
    return pc != 0.0 ? 1 : 0;
}

static double aug_degrees_from_horizontal(double nz)
{
    if (nz >  1.0) nz =  1.0;
    if (nz < -1.0) nz = -1.0;
    return acos(nz) * 180.0 / 3.14159265358979323846;
}

typedef struct aug_ctx {
    sh_aas         *a;
    const sh_aug_opts *o;
    double          min_floor_cos;  /* the payload's own walkable-slope gate */
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

/* An area's bounds, as (minx, miny, maxx, maxy, minz, maxz).
 *
 * There is deliberately NO flatness gate. Its predecessor refused any area whose
 * minz != maxz, which did two kinds of harm: it would make every tilted
 * generated area an unlinkable island, and it ALREADY made the linkers blind to
 * the majority of shipped module areas -- 144 of 159 in d2_map_01.aas_monster48
 * have spanning bounds, 57 of 102 in hell_big_blank_room.
 *
 * Callers take maxz as the surface a demon stands on: exact for a flat area, and
 * the conservative choice for a sloped one. */
static int aug_area_box(const sh_aas *a, unsigned area, float out[6])
{
    const unsigned char *b = sh_aas_rec_const(a, SH_AAS_L_AREABOUNDS, area);
    if (!b) return 0;
    out[0] = (float)sh_aas_get_i16(b, AB_MINX);
    out[1] = (float)sh_aas_get_i16(b, AB_MINY);
    out[2] = (float)sh_aas_get_i16(b, AB_MAXX);
    out[3] = (float)sh_aas_get_i16(b, AB_MAXY);
    out[4] = (float)sh_aas_get_i16(b, AB_MINZ);
    out[5] = (float)sh_aas_get_i16(b, AB_MAXZ);
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
static int aug_add_area(aug_ctx *c, const aug_quad *eff, int carrier)
{
    /* A closed edge loop wound clockwise seen from +Z -- the winding of every
     * floor area of the Grid Room, verified by shoelace on areas 2, 5 and 8. */
    int vs[4];
    unsigned first_ei = sh_aas_count(c->a, SH_AAS_L_EDGEINDEX);
    unsigned first_pvs = 0, first_area, first_bounds, slot;
    unsigned char *ar, *ab;
    int cluster, can = 0, k;
    unsigned i, n;

    for (k = 0; k < 4; k++) {
        vs[k] = aug_vertex(c, (float)eff->c[k][0], (float)eff->c[k][1],
                              (float)eff->c[k][2]);
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

    sh_aas_put_i16(ab, AB_MINX, (int16_t)aug_clamp16(aug_round((float)eff->x0)));
    sh_aas_put_i16(ab, AB_MINY, (int16_t)aug_clamp16(aug_round((float)eff->y0)));
    sh_aas_put_i16(ab, AB_MINZ, (int16_t)aug_clamp16(aug_round((float)aug_quad_min_z(eff))));
    sh_aas_put_i16(ab, AB_MAXX, (int16_t)aug_clamp16(aug_round((float)eff->x1)));
    sh_aas_put_i16(ab, AB_MAXY, (int16_t)aug_clamp16(aug_round((float)eff->y1)));
    sh_aas_put_i16(ab, AB_MAXZ, (int16_t)aug_clamp16(aug_round((float)aug_quad_max_z(eff))));

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
static int aug_carve(aug_ctx *c, int area, const aug_quad *eff)
{
    typedef struct { int node; aug_box cell; } aug_frame;
    struct { int plane; int f4; } split[5];
    aug_frame *stack;
    unsigned cap = 256, top = 0;
    int carved = 0, root, i;
    aug_box box;
    const double BIG = 1e9;
    const double eps = 1e-3;

    /* The face plane, offset VERTICALLY. BSP_FLOOR_EPS was measured as
     * areaBounds[leaf].minz - planeZ, a vertical quantity, so displacing the
     * plane 9.6 along an oblique normal would make the vertical drop 9.6/n.z --
     * 13.7 at the 0.7 slope gate, outside anything the corpus shows. Shifting
     * `dist` by eps*n.z keeps the vertical drop at exactly eps for any tilt. */
    split[0].plane = aug_plane(c, (float)eff->n[0], (float)eff->n[1], (float)eff->n[2],
                                  (float)(eff->d + BSP_FLOOR_EPS * eff->n[2]));
    split[0].f4 = NODE_F4_Z;
    /* One vertical plane per quad edge, on the INWARD normal: the engine takes
     * child0 when n.p + dist > 0, so the area child must be on the interior
     * side. For an axis-aligned quad these are exactly the four planes this
     * replaced; for a yawed one they are the yawed edges, which is what stops a
     * rotated platform swallowing its own bounding square. */
    for (i = 0; i < 4; i++) {
        double n2[2];
        aug_edge_normal_in(eff, i, n2);
        split[1 + i].plane = aug_plane(c, (float)n2[0], (float)n2[1], 0.0f,
            (float)(-(n2[0]*eff->c[i][0] + n2[1]*eff->c[i][1])));
        split[1 + i].f4 = aug_node_field4(0.0);
    }
    for (i = 0; i < 5; i++) if (split[i].plane < 0) return 0;

    box.v[0] = eff->x0; box.v[1] = eff->y0;
    box.v[2] = aug_quad_min_z(eff) - BSP_FLOOR_EPS;
    box.v[3] = eff->x1; box.v[4] = eff->y1;
    box.v[5] = aug_quad_max_z(eff) + c->height;

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
    float A[6], B[6];
    double ox0, ox1, oy0, oy1, ts[64];
    int added = 0, n, k;
    if (!aug_area_box(c->a, (unsigned)ai, A)) return 0;
    if (!aug_area_box(c->a, (unsigned)bi, B)) return 0;
    if (fabs((double)A[5] - (double)B[5]) > (double)c->step) return 0;
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
            s[0] = ox0 - sgn * REACH_SIDE_OFFSET; s[1] = ts[k]; s[2] = A[5];
            e[0] = ox0 + sgn * REACH_SIDE_OFFSET; e[1] = ts[k]; e[2] = B[5];
            if (aug_reach(c, REACH_WALK, REACH_WALK_TIME, ai, bi, s, e)) added++;
        }
    } else if (oy1 == oy0 && ox1 > ox0) {
        double sgn = (((double)A[1] + A[3]) / 2.0 < oy0) ? 1.0 : -1.0;
        n = aug_samples((float)ox0, (float)ox1, ts, 64);
        for (k = 0; k < n; k++) {
            double s[3], e[3];
            s[0] = ts[k]; s[1] = oy0 - sgn * REACH_SIDE_OFFSET; s[2] = A[5];
            e[0] = ts[k]; e[1] = oy0 + sgn * REACH_SIDE_OFFSET; e[2] = B[5];
            if (aug_reach(c, REACH_WALK, REACH_WALK_TIME, ai, bi, s, e)) added++;
        }
    }
    return added;
}

/* One SEGMENT of one platform edge, and what lies across it. Shared by every
 * link regime so they all see exactly the same geometry.
 *
 * DIRECTIONAL, not axis-aligned: a rotated platform's edge has no axis. `out` is
 * the outward unit XY normal, which for an axis-aligned quad is exactly the
 * (axis, sgn) pair this replaced.
 *
 * A SEGMENT, not the whole edge: its predecessor probed one point per side, at
 * the side's midpoint, so a neighbour abutting only part of an edge was invisible
 * and the probe answered with the module floor far below. That is what made
 * demons climb down and back up between two volumes standing together.
 *
 * `drop` is SIGNED -- near_z minus far_z -- because discovery can now see a
 * neighbour ABOVE. Every regime gates on its MAGNITUDE plus a direction; see
 * aug_regime. Gating on the signed value was safe only while a neighbour above
 * could not occur, and would now classify a 500-unit rise as a step. */
typedef struct aug_side {
    double p0[2], p1[2];    /* the segment on the INSET quad, in edge order */
    double out[2];          /* outward unit normal in XY */
    int    floor_area;      /* the area across it */
    double near_z, far_z;   /* surface height each side, at the segment midpoint */
    double drop;            /* near_z - far_z */
    double gap;             /* XY clearance ALONG THIS SEGMENT'S ray; 0 when touching */
    double land[2];         /* where a link actually arrives on the far side */
    int    generated;       /* 1 if the neighbour is another generated quad */
    int    peer;            /* index into the peer table, or -1 */
} aug_side;

/* A quad this bake already created: its uninset footprint, its inset footprint
 * and the area it became. Adjacency is decided on `req` and endpoints are placed
 * inside `eff`; see aug_edge_segments. */
typedef struct aug_peer {
    const aug_quad *req;
    const aug_quad *eff;
    int             area;
} aug_peer;

/* How close two UNINSET footprints must be to count as touching. Volumes an
 * author snapped together are flush, but exact zero is the wrong test against
 * float noise, and a gap below the shortest shipped leap cannot be crossed by
 * anything -- so anything within this is dispatched to the step or climb
 * regime rather than falling into a hole between the two. */
#define AUG_TOUCH_EPS   4.0

/* The XY clearance between two convex quads: 0 if they touch or overlap.
 * Measured on the UNINSET footprints, because that is where the walls actually
 * are. */
static double aug_quad_gap(const aug_quad *a, const aug_quad *b)
{
    double best = 1e30;
    int i, k;
    /* Cheap separating-box reject first. */
    if (a->x1 < b->x0 - 4096.0 || b->x1 < a->x0 - 4096.0 ||
        a->y1 < b->y0 - 4096.0 || b->y1 < a->y0 - 4096.0) return 1e30;
    /* Point-to-edge over both directions is enough for two convex quads at the
     * distances that matter here. */
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 4; k++) {
            double ex = b->c[(k + 1) & 3][0] - b->c[k][0];
            double ey = b->c[(k + 1) & 3][1] - b->c[k][1];
            double len2 = ex * ex + ey * ey, t, dx, dy, d;
            if (len2 < 1e-9) continue;
            t = ((a->c[i][0] - b->c[k][0]) * ex + (a->c[i][1] - b->c[k][1]) * ey) / len2;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            dx = a->c[i][0] - (b->c[k][0] + t * ex);
            dy = a->c[i][1] - (b->c[k][1] + t * ey);
            d = sqrt(dx * dx + dy * dy);
            if (d < best) best = d;
        }
        if (aug_quad_contains_xy(b, a->c[i][0], a->c[i][1])) return 0.0;
        if (aug_quad_contains_xy(a, b->c[i][0], b->c[i][1])) return 0.0;
    }
    return best;
}

/* Parameters along an edge, in 0..1, for segmenting it. Denser than the single
 * midpoint the predecessor used, which is the whole point. */
static int aug_samples_unit(double *out, int cap)
{
    int i, n = cap < 17 ? cap : 17;
    if (n < 2) { if (cap > 0) out[0] = 0.5; return cap > 0 ? 1 : 0; }
    for (i = 0; i < n; i++) out[i] = (double)i / (double)(n - 1);
    return n;
}

/* How far along a ray the quad is first entered, and where to stand once inside.
 *
 * Measured ALONG THE RAY, not as the minimum clearance between two whole quads.
 * Those differ whenever the neighbour is off to one side, and using the whole-
 * quad minimum picks an animation for one distance and then writes the link at
 * another -- outside the stretch envelope the animation was chosen for.
 *
 * Returns 0 if the ray never reaches it inside `maxd`. */
static int aug_ray_entry(const aug_quad *target, double ox, double oy,
                         double dx, double dy, double maxd,
                         double *out_dist, double out_land[2])
{
    double lo = -1.0, hi = -1.0, d;
    int i;

    for (d = 0.0; d <= maxd; d += 16.0) {
        if (aug_quad_contains_xy(target, ox + dx * d, oy + dy * d)) { hi = d; break; }
        lo = d;
    }
    if (hi < 0.0) return 0;
    /* Bisect down to a unit, so the reported distance is the wall and not the
     * step size that found it. */
    if (lo >= 0.0) {
        for (i = 0; i < 8; i++) {
            double mid = (lo + hi) / 2.0;
            if (aug_quad_contains_xy(target, ox + dx * mid, oy + dy * mid)) hi = mid;
            else lo = mid;
        }
    }
    *out_dist = hi;
    /* Stand a little way past the lip rather than exactly on it, and fall back
     * to the quad's centre if that overshoots a narrow target. */
    out_land[0] = ox + dx * (hi + REACH_SIDE_OFFSET * 2.0);
    out_land[1] = oy + dy * (hi + REACH_SIDE_OFFSET * 2.0);
    if (!aug_quad_contains_xy(target, out_land[0], out_land[1])) {
        out_land[0] = (target->x0 + target->x1) / 2.0;
        out_land[1] = (target->y0 + target->y1) / 2.0;
    }
    return 1;
}

/* Fill one segment record from a run of samples facing one neighbour. */
static void aug_close_segment(aug_ctx *c, aug_side *sd, const aug_quad *eff,
                              int e, const double out2[2], double t0, double t1,
                              int who, const aug_peer *peers, int npeers,
                              const aug_quad *req)
{
    int j = (e + 1) & 3, q;
    double mt = (t0 + t1) / 2.0, mx, my;

    sd->p0[0] = eff->c[e][0] + t0 * (eff->c[j][0] - eff->c[e][0]);
    sd->p0[1] = eff->c[e][1] + t0 * (eff->c[j][1] - eff->c[e][1]);
    sd->p1[0] = eff->c[e][0] + t1 * (eff->c[j][0] - eff->c[e][0]);
    sd->p1[1] = eff->c[e][1] + t1 * (eff->c[j][1] - eff->c[e][1]);
    sd->out[0] = out2[0];
    sd->out[1] = out2[1];
    mx = eff->c[e][0] + mt * (eff->c[j][0] - eff->c[e][0]);
    my = eff->c[e][1] + mt * (eff->c[j][1] - eff->c[e][1]);
    sd->near_z = aug_z_at(eff, mx, my);
    sd->floor_area = who;
    sd->generated = 0;
    sd->peer = -1;
    sd->gap = 0.0;

    sd->land[0] = mx + out2[0] * REACH_SIDE_OFFSET;
    sd->land[1] = my + out2[1] * REACH_SIDE_OFFSET;

    for (q = 0; q < npeers; q++) {
        double dist, wx, wy, junk[2];
        if (peers[q].area != who) continue;
        sd->generated = 1;
        sd->peer = q;

        /* TWO casts along the same ray, because the two questions differ.
         *
         * The GAP is wall to wall, so it is cast from OUR uninset edge against
         * the peer's uninset footprint. Casting from the inset edge instead
         * would report the agent radius -- 24 to 64 units -- as a gap between
         * two volumes an author placed flush, and the segment would be
         * classified as a leap across a gap that does not exist.
         *
         * The LANDING point has to be inside the peer's AREA, which is carved at
         * its inset quad, so that one is cast against `eff`. */
        wx = req->c[e][0] + mt * (req->c[j][0] - req->c[e][0]);
        wy = req->c[e][1] + mt * (req->c[j][1] - req->c[e][1]);
        if (aug_ray_entry(peers[q].req, wx, wy, out2[0], out2[1],
                          SH_TRAV_LEAP_MAX_SPAN, &dist, junk))
            sd->gap = dist < AUG_TOUCH_EPS ? 0.0 : dist;
        else {
            /* The ray misses it even though a sample found it, which happens at
             * a corner. The whole-quad clearance is right, just less precise. */
            sd->gap = aug_quad_gap(req, peers[q].req);
            if (sd->gap < AUG_TOUCH_EPS) sd->gap = 0.0;
        }
        if (!aug_ray_entry(peers[q].eff, wx, wy, out2[0], out2[1],
                           SH_TRAV_LEAP_MAX_SPAN + 256.0, &dist, sd->land)) {
            sd->land[0] = (peers[q].eff->x0 + peers[q].eff->x1) / 2.0;
            sd->land[1] = (peers[q].eff->y0 + peers[q].eff->y1) / 2.0;
        }
        /* The height is read where the link ACTUALLY arrives. Reading it a few
         * units outside our own edge is mid-air once there is a gap, and on a
         * tilted neighbour it extrapolates the plane to a height no surface
         * has. */
        sd->far_z = aug_z_at(peers[q].eff, sd->land[0], sd->land[1]);
        break;
    }
    if (!sd->generated) {
        float tb[6];
        sd->far_z = aug_area_box(c->a, (unsigned)who, tb) ? (double)tb[5] : sd->near_z;
    }
    sd->drop = sd->near_z - sd->far_z;
}

/* The peers close enough to this quad to be worth testing, by bounding box.
 *
 * Without this, gap discovery is four edges by thirty-two samples by sixty
 * ray steps by EVERY peer, and the platform cap is 512 -- billions of
 * containment tests for one bake. The AABB overlap test is exact enough as a
 * filter because it can only ever admit too many, never too few.
 *
 * `reach` is how far out the caller intends to look: the touching epsilon for
 * adjacency, the longest shipped leap for gaps. */
static int aug_candidates(const aug_quad *req, const aug_peer *peers, int npeers,
                          int ai, double reach, const aug_peer **out, int cap)
{
    int q, n = 0;
    for (q = 0; q < npeers && n < cap; q++) {
        if (peers[q].area == ai) continue;
        if (peers[q].req->x0 > req->x1 + reach) continue;
        if (peers[q].req->x1 < req->x0 - reach) continue;
        if (peers[q].req->y0 > req->y1 + reach) continue;
        if (peers[q].req->y1 < req->y0 - reach) continue;
        out[n++] = &peers[q];
    }
    return n;
}

/* Per edge of the quad, the SEGMENTS of that edge and the neighbour each faces.
 *
 * Two neighbour sources, and they are deliberately not symmetric:
 *
 * 1. OTHER GENERATED QUADS, compared directly in XY against their UNINSET
 *    footprints. Exact and symmetric, so partial abutment, neighbours ABOVE and
 *    the lowest-first creation order all stop mattering -- the augmenter already
 *    holds every quad it made and does not have to rediscover them through the
 *    BSP.
 *
 *    On `req`, NOT `eff`. `eff` is inset by the agent radius -- 24, 48 or 64 by
 *    nav class -- so two FLUSH volumes are up to 128 units apart in `eff`, and a
 *    proximity test there finds nothing at all for exactly the case this exists
 *    to fix.
 *
 * 2. THE MODULE'S OWN SHIPPED AREAS, sampled and BSP-probed. Downward only: an
 *    author cannot place a shipped module area above their own platform, so the
 *    restriction is right for this source and wrong for the other.
 *
 * A contiguous run of samples facing one neighbour becomes one segment. */
static int aug_edge_segments(aug_ctx *c, int ai, const aug_quad *eff,
                             const aug_quad *req, const aug_peer *peers,
                             int npeers, aug_side *out, int cap)
{
    const aug_peer *cand[SH_AUG_MAX_PLATFORMS];
    int e, n = 0, ncand;

    ncand = aug_candidates(req, peers, npeers, ai, AUG_TOUCH_EPS * 2.0,
                           cand, SH_AUG_MAX_PLATFORMS);

    for (e = 0; e < 4 && n < cap; e++) {
        double in2[2], out2[2], ts[32];
        int cnt, k, run_start = 0, run_peer = -2, j = (e + 1) & 3;

        aug_edge_normal_in(eff, e, in2);
        out2[0] = -in2[0];
        out2[1] = -in2[1];
        cnt = aug_samples_unit(ts, 32);

        for (k = 0; k <= cnt; k++) {
            int who = -1, q;
            if (k < cnt) {
                double t = ts[k];
                double ex = req->c[e][0] + t * (req->c[j][0] - req->c[e][0]);
                double ey = req->c[e][1] + t * (req->c[j][1] - req->c[e][1]);
                double px = ex + out2[0] * AUG_TOUCH_EPS;
                double py = ey + out2[1] * AUG_TOUCH_EPS;
                for (q = 0; q < ncand; q++) {
                    if (aug_quad_contains_xy(cand[q]->req, px, py)) { who = cand[q]->area; break; }
                }
                if (who < 0) {
                    float tb[6];
                    double nz = aug_z_at(eff, ex, ey);
                    double bx = ex + out2[0] * REACH_SIDE_OFFSET;
                    double by = ey + out2[1] * REACH_SIDE_OFFSET;
                    int bi = sh_aas_point_area(c->a, (float)bx, (float)by, (float)nz);
                    if (bi > 0 && bi != ai && aug_area_box(c->a, (unsigned)bi, tb) &&
                        (double)tb[5] < nz) who = bi;
                }
            }
            if (who != run_peer) {
                if (run_peer >= 0 && n < cap)
                    aug_close_segment(c, &out[n++], eff, e, out2,
                                      ts[run_start], ts[k - 1], run_peer,
                                      peers, npeers, req);
                run_peer = who;
                run_start = k;
            }
        }
    }
    return n;
}

/* Segments of an edge that face another generated quad ACROSS OPEN SPACE.
 *
 * This has to be its own query. aug_edge_segments asks whether a point a few
 * units outside the edge lies inside a peer, and a quad across a 600-unit gap
 * never does -- so leaps found by the touching probe would be exactly none, and
 * a REGIME_LEAP segment could never occur.
 *
 * So this casts outward from the edge, as far as the longest leap shipped data
 * shows (982 units), and records the first peer it meets with the clearance to
 * it. Runs of samples facing the same peer at a similar distance become one
 * segment. Anything already touching is left to aug_edge_segments. */
static int aug_edge_gap_segments(aug_ctx *c, int ai, const aug_quad *eff,
                                 const aug_quad *req, const aug_peer *peers,
                                 int npeers, aug_side *out, int cap)
{
    const aug_peer *cand[SH_AUG_MAX_PLATFORMS];
    int e, n = 0, ncand;

    ncand = aug_candidates(req, peers, npeers, ai, SH_TRAV_LEAP_MAX_SPAN,
                           cand, SH_AUG_MAX_PLATFORMS);
    if (ncand == 0) return 0;

    for (e = 0; e < 4 && n < cap; e++) {
        double in2[2], out2[2], ts[32];
        int cnt, k, run_start = 0, run_peer = -2, j = (e + 1) & 3;

        aug_edge_normal_in(eff, e, in2);
        out2[0] = -in2[0];
        out2[1] = -in2[1];
        cnt = aug_samples_unit(ts, 32);

        for (k = 0; k <= cnt; k++) {
            int who = -1, q;
            if (k < cnt) {
                double t = ts[k];
                double ex = req->c[e][0] + t * (req->c[j][0] - req->c[e][0]);
                double ey = req->c[e][1] + t * (req->c[j][1] - req->c[e][1]);
                double d;
                for (d = AUG_TOUCH_EPS; d <= SH_TRAV_LEAP_MAX_SPAN && who < 0; d += 16.0) {
                    double px = ex + out2[0] * d, py = ey + out2[1] * d;
                    for (q = 0; q < ncand; q++) {
                        if (aug_quad_contains_xy(cand[q]->req, px, py)) { who = cand[q]->area; break; }
                    }
                }
            }
            if (who != run_peer) {
                if (run_peer >= 0 && n < cap) {
                    aug_close_segment(c, &out[n], eff, e, out2,
                                      ts[run_start], ts[k - 1], run_peer,
                                      peers, npeers, req);
                    /* Only a genuine gap belongs here; anything the touching
                     * probe already owns is not a leap. */
                    if (out[n].gap > AUG_TOUCH_EPS) n++;
                }
                run_peer = who;
                run_start = k;
            }
        }
    }
    return n;
}

/* Which regime carries a segment.
 *
 * On the MAGNITUDE of the height change plus an explicit direction. The
 * predecessors gated on the signed drop, which was safe only because discovery
 * refused neighbours above; with that gone, `drop > step` would call a 500-unit
 * RISE a step and emit a plain 0x20 walk link for it -- and across 94,327
 * shipped walk records joining two flat areas, not one spans more than
 * maxStepHeight. */
enum { REGIME_NONE = 0, REGIME_STEP, REGIME_UP, REGIME_DOWN, REGIME_LEAP };

static int aug_regime(const aug_side *s, double step)
{
    if (s->gap > AUG_TOUCH_EPS) return REGIME_LEAP;
    if (fabs(s->drop) <= step)  return REGIME_STEP;
    return s->drop > 0.0 ? REGIME_DOWN : REGIME_UP;
}

/* Is every corner of `inner` inside `outer`, in XY? */
static int aug_quad_contains_quad(const aug_quad *outer, const aug_quad *inner)
{
    int i;
    for (i = 0; i < 4; i++)
        if (!aug_quad_contains_xy(outer, inner->c[i][0], inner->c[i][1])) return 0;
    return 1;
}

/* Emit for this segment at all?
 *
 * Two generated quads that face each other are discovered from BOTH sides, and
 * each regime writes both directions per segment, so without a rule every record
 * between them is written twice. "The lower area index owns the pair" settles
 * that -- but only while discovery really is symmetric, and it is not.
 *
 * A quad is discovered by sampling points just outside the INSPECTING quad's own
 * edges. So when one footprint sits wholly INSIDE another -- a tall pillar
 * standing through a wide slab -- every pillar edge faces the slab, and no slab
 * edge ever faces the pillar. Platforms are created lowest-first, so the slab
 * takes the lower index and would own a pair it can never see: the pillar found
 * the segments, declined to emit, and the slab never looked. The pillar came out
 * emitted, carved, passing the serving gate, and with ZERO reachabilities -- and
 * a demon that cannot be routed anywhere just stands still and shoots.
 *
 * So containment decides first, and only then the index. Mutual containment
 * (identical footprints) means both sides do discover it, and falls through to
 * the index rule rather than being emitted twice. */
static int aug_side_owns(const aug_side *s, int ai, const aug_quad *req,
                         const aug_peer *peers, int npeers)
{
    if (!s->generated) return 1;
    if (s->peer >= 0 && s->peer < npeers && peers[s->peer].req && req) {
        const aug_quad *theirs = peers[s->peer].req;
        int we_inside_them = aug_quad_contains_quad(theirs, req);
        int they_inside_us = aug_quad_contains_quad(req, theirs);
        if (we_inside_them && !they_inside_us) return 1;   /* only we can see it */
        if (they_inside_us && !we_inside_them) return 0;   /* only they can */
    }
    return !(s->floor_area < ai);
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
static int aug_step_links(aug_ctx *c, int ai, const aug_quad *eff,
                          const aug_quad *req, const aug_peer *peers, int npeers)
{
    aug_side sides[64];
    int n = aug_edge_segments(c, ai, eff, req, peers, npeers, sides,
                              (int)(sizeof sides / sizeof sides[0]));
    int added = 0, i, k;

    for (i = 0; i < n; i++) {
        double ts[64];
        int cnt;
        if (aug_regime(&sides[i], (double)c->step) != REGIME_STEP) continue;
        if (!aug_side_owns(&sides[i], ai, req, peers, npeers)) continue;
        cnt = aug_samples_unit(ts, 64);
        for (k = 0; k < cnt; k++) {
            double t = ts[k], up[3], dn[3];
            /* The near endpoint sits inside THIS quad; the far one inside the
             * neighbour's own inset footprint, not one unit past a shared wall.
             * Generated areas are carved at the INSET quad, so two flush volumes
             * have areas 2*radius apart -- 48 to 128 units -- with the floor slab
             * between them. Probing just outside the wall lands in that dead
             * strip, the BSP answers with the floor, and the link is refused:
             * every step link between two chained volumes, gone. */
            dn[0] = sides[i].p0[0] + t * (sides[i].p1[0] - sides[i].p0[0])
                  - sides[i].out[0] * REACH_SIDE_OFFSET;
            dn[1] = sides[i].p0[1] + t * (sides[i].p1[1] - sides[i].p0[1])
                  - sides[i].out[1] * REACH_SIDE_OFFSET;
            dn[2] = aug_z_at(eff, dn[0], dn[1]);
            if (sides[i].generated && sides[i].peer >= 0) {
                /* The landing point the segment already measured along its own
                 * outward ray. Generated areas are carved at the INSET quad, so
                 * two flush volumes have areas 2*radius apart with floor between
                 * them: a point one unit past the shared wall is in that dead
                 * strip and the BSP answers with the floor, refusing the link. */
                const aug_quad *pe = peers[sides[i].peer].eff;
                up[0] = sides[i].land[0];
                up[1] = sides[i].land[1];
                up[2] = aug_z_at(pe, up[0], up[1]);
            } else {
                up[0] = sides[i].p0[0] + t * (sides[i].p1[0] - sides[i].p0[0])
                      + sides[i].out[0] * REACH_SIDE_OFFSET;
                up[1] = sides[i].p0[1] + t * (sides[i].p1[1] - sides[i].p0[1])
                      + sides[i].out[1] * REACH_SIDE_OFFSET;
                up[2] = sides[i].far_z;
            }
            /* Both endpoints must resolve to the area they claim, or the link is
             * a lie the router will act on. */
            if (sh_aas_point_area(c->a, (float)dn[0], (float)dn[1],
                                  (float)(dn[2] + 2.0)) != ai) continue;
            if (sh_aas_point_area(c->a, (float)up[0], (float)up[1],
                                  (float)(up[2] + 2.0)) != sides[i].floor_area)
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
static int aug_fall_links(aug_ctx *c, int ai, const aug_quad *eff,
                          const aug_quad *req, const aug_peer *peers, int npeers)
{
    aug_side sides[64];
    int n, i, added = 0;
    double max_fall = sh_aas_setting_f32(c->a, SET_MAX_FALL_HEIGHT);
    double tt = sh_aas_setting_f32(c->a, SET_TT_WALK_OFF_LEDGE);

    if (c->o->fall == SH_AUG_FALL_NEVER) return 0;
    n = aug_edge_segments(c, ai, eff, req, peers, npeers, sides,
                          (int)(sizeof sides / sizeof sides[0]));
    for (i = 0; i < n; i++) {
        double s3[3], e3[3], bx, by;
        int time;
        /* Falling is DOWNWARD only, by definition -- a rise is a climb. */
        if (aug_regime(&sides[i], (double)c->step) != REGIME_DOWN) continue;
        if (!aug_side_owns(&sides[i], ai, req, peers, npeers)) continue;
        if (c->o->fall == SH_AUG_FALL_AUTO && sides[i].drop > max_fall) continue;
        bx = (sides[i].p0[0] + sides[i].p1[0]) / 2.0;
        by = (sides[i].p0[1] + sides[i].p1[1]) / 2.0;
        s3[0] = bx - sides[i].out[0] * REACH_SIDE_OFFSET;
        s3[1] = by - sides[i].out[1] * REACH_SIDE_OFFSET;
        s3[2] = aug_z_at(eff, s3[0], s3[1]);
        e3[0] = bx + sides[i].out[0] * REACH_SIDE_OFFSET;
        e3[1] = by + sides[i].out[1] * REACH_SIDE_OFFSET;
        e3[2] = sides[i].far_z;
        time = (int)tt + aug_round((float)(sides[i].drop * REACH_FALL_PER_UNIT));
        if (aug_reach(c, REACH_FALL, time, ai, sides[i].floor_area, s3, e3)) added++;
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
    int      is_leap;       /* a gap crossing rather than a climb, for the report */
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
#ifdef SH_AUG_TESTING
/* Emulate the single-anchor build, so an A/B over real module bytes can show
 * what per-demon anchor placement actually recovers. 0 = no cap. */
static int g_test_anchor_cap;
int sh_aug_test_set_anchor_cap(int n)
{
    int was = g_test_anchor_cap;
    g_test_anchor_cap = n;
    return was;
}
#endif

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
/* A link whose path crosses another generated quad's solid column is dropped.
 *
 * Both endpoints of a traversal were already checked, but the path BETWEEN them
 * never was, so a climb or a leap could send a demon straight through a volume
 * standing in the way -- "they can glitch through the bv during the traversal
 * and get lost".
 *
 * ASSUMPTION, stated because nothing pins it: a generated quad's solid column is
 * that quad extruded downward. nav_regions reports only the walkable face and
 * discards the volume's lower faces, so the true solid is not available at this
 * layer. This errs toward refusing a link that would in fact have been clear,
 * and a refused link is a reported island edge, never a corrupt payload. A quad
 * whose surface is BELOW the sampled path does not block, which is what we
 * want. */
static int aug_path_is_clear(const aug_peer *peers, int npeers,
                             int from_area, int to_area,
                             const double s3[3], const double e3[3])
{
    int q, k;
    for (q = 0; q < npeers; q++) {
        if (peers[q].area == from_area || peers[q].area == to_area) continue;
        for (k = 1; k < 16; k++) {
            double t = (double)k / 16.0;
            double x = s3[0] + t * (e3[0] - s3[0]);
            double y = s3[1] + t * (e3[1] - s3[1]);
            double z = s3[2] + t * (e3[2] - s3[2]);
            if (aug_quad_contains_xy(peers[q].req, x, y) &&
                z < aug_z_at(peers[q].req, x, y)) return 0;
        }
    }
    return 1;
}

/* Collect climb and leap specs for one platform: one per demon per usable
 * segment, in both directions.
 *
 * Every endpoint is checked with the BSP. A point that does not land in the area
 * it claims is dropped rather than written: the router would act on the lie. */
static int aug_traversal_specs(aug_ctx *c, int ai, const aug_quad *eff,
                               const aug_quad *req, const aug_peer *peers,
                               int npeers, aug_trav_spec *out, int cap)
{
    aug_side sides[64];
    int n, i, k, d, count = 0;

    if (c->o->traversal == SH_AUG_TRAVERSAL_NEVER) return 0;
    if (!sh_trav_ready()) return 0;

    n = aug_edge_segments(c, ai, eff, req, peers, npeers, sides,
                          (int)(sizeof sides / sizeof sides[0]));
    /* Climbs come from the touching segments; leaps need their own long-range
     * cast, because a quad across a real gap is invisible to the touching
     * probe. */
    n += aug_edge_gap_segments(c, ai, eff, req, peers, npeers, sides + n,
                               (int)(sizeof sides / sizeof sides[0]) - n);
    for (i = 0; i < n; i++) {
        double anchors[24];
        double seglen, span;
        int na, regime, leap;

        regime = aug_regime(&sides[i], (double)c->step);
        if (regime == REGIME_STEP || regime == REGIME_NONE) continue;
        if (!aug_side_owns(&sides[i], ai, req, peers, npeers)) continue;
        leap = (regime == REGIME_LEAP);
        /* A climb is measured vertically, a leap horizontally. */
        span = leap ? sides[i].gap : fabs(sides[i].drop);
        /* A leap is nearly LEVEL: over 1,754 shipped records on the six table
         * nominals the median vertical change is one unit and |dz|/span p90 is
         * 0.29. Anything steeper is not what these animations do. */
        if (leap && span > 0.0 &&
            fabs(sides[i].drop) / span > SH_TRAV_LEAP_MAX_GRADE) continue;

        seglen = sqrt((sides[i].p1[0] - sides[i].p0[0]) * (sides[i].p1[0] - sides[i].p0[0]) +
                      (sides[i].p1[1] - sides[i].p0[1]) * (sides[i].p1[1] - sides[i].p0[1]));
        (void)seglen;
        /* Anchors run along THIS SEGMENT, not the whole edge, which is what
         * makes a partially-abutting neighbour reachable by a demon whose
         * animation offset is large. */
        na = aug_trav_anchors(0.0, 1.0, anchors,
                              (int)(sizeof anchors / sizeof anchors[0]));
#ifdef SH_AUG_TESTING
        if (g_test_anchor_cap > 0 && na > g_test_anchor_cap) na = g_test_anchor_cap;
#endif

        for (d = 0; d < 2; d++) {                          /* UP then DOWN */
            int up = (d == SH_TRAV_UP);
            for (k = 0; k < sh_trav_monster_count() && count < cap; k++) {
                const sh_trav_monster *m = sh_trav_monster_at(k);
                char path[SH_TRAV_PATH_CAP];
                float off = 0.0f;
                int dist = 0, time = 0, aidx, placed = 0, dirn;
                double inner_pt[3], outer_pt[3], dirs;
                aug_trav_spec *sp;

                if (!m) continue;
                dirn = leap ? SH_TRAV_ACROSS : d;
                if (!sh_trav_select(m, dirn, (float)span, path, sizeof path,
                                    &off, &dist, &time))
                    continue;                               /* this demon cannot */
                if (off < 0.0f) off = -off;

                for (aidx = 0; aidx < na; aidx++) {
                    double t = anchors[aidx];
                    double bx = sides[i].p0[0] + t * (sides[i].p1[0] - sides[i].p0[0]);
                    double by = sides[i].p0[1] + t * (sides[i].p1[1] - sides[i].p0[1]);
                    inner_pt[0] = bx - sides[i].out[0] * TRAVERSAL_INNER_INSET;
                    inner_pt[1] = by - sides[i].out[1] * TRAVERSAL_INNER_INSET;
                    inner_pt[2] = aug_z_at(eff, inner_pt[0], inner_pt[1]);
                    if (sides[i].generated && sides[i].peer >= 0) {
                        /* Land INSIDE the neighbour's own inset footprint, at the
                         * point the segment measured along its outward ray. The
                         * areas are 2*radius apart even when the walls are flush,
                         * so an endpoint measured from the wall lands in the dead
                         * strip between them.
                         *
                         * The animation's own offset is NOT added here. For a
                         * touching neighbour the two areas already stand that far
                         * apart, and for a leap the span was selected from this
                         * same measured distance -- adding the offset on top
                         * would write the link at a distance the chosen clip was
                         * never checked against. */
                        const aug_quad *pe = peers[sides[i].peer].eff;
                        outer_pt[0] = sides[i].land[0];
                        outer_pt[1] = sides[i].land[1];
                        outer_pt[2] = aug_z_at(pe, outer_pt[0], outer_pt[1]);
                    } else {
                        outer_pt[0] = bx + sides[i].out[0] * (double)off;
                        outer_pt[1] = by + sides[i].out[1] * (double)off;
                        outer_pt[2] = sides[i].far_z;
                    }
                    if (sh_aas_point_area(c->a, (float)inner_pt[0], (float)inner_pt[1],
                                          (float)(inner_pt[2] + 2.0)) != ai) continue;
                    if (sh_aas_point_area(c->a, (float)outer_pt[0], (float)outer_pt[1],
                                          (float)(outer_pt[2] + 2.0)) != sides[i].floor_area)
                        continue;
                    if (!aug_path_is_clear(peers, npeers, ai, sides[i].floor_area,
                                           inner_pt, outer_pt)) continue;
                    placed = 1;
                    break;
                }
                if (!placed) continue;      /* nowhere on this segment works for it */

                sp = &out[count++];
                memset(sp, 0, sizeof *sp);
                sp->travel_flags = m->travel_flags;
                sp->d30 = m->d30;
                sp->travel_time = time;
                sp->from_area = up ? sides[i].floor_area : ai;
                sp->to_area   = up ? ai : sides[i].floor_area;
                memcpy(sp->start, up ? outer_pt : inner_pt, sizeof sp->start);
                memcpy(sp->end,   up ? inner_pt : outer_pt, sizeof sp->end);
                /* Facing is the XY travel direction: inward coming onto this
                 * platform, outward leaving it. A non-axis facing is on
                 * precedent -- wc_office_arena.aas_monster48 carries (w18,w1a)
                 * pairs of (23169,23169) and (57468,33778), and 23169 is
                 * 0.707 * TP_DIR_UNIT. */
                dirs = up ? -1.0 : 1.0;
                sp->dir[0] = sides[i].out[0] * dirs;
                sp->dir[1] = sides[i].out[1] * dirs;
                sp->is_leap = leap;
                _snprintf_s(sp->anim, sizeof sp->anim, _TRUNCATE, "%s", path);
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

    /* MODULES THAT ALREADY SHIP TRAVERSALS.
     *
     * The predecessor of this block refused outright whenever the payload had
     * any traversal points, on the stated grounds that no SnapMap module ships
     * one. That is simply false: classic_90_climb -- this project's own donor --
     * ships seven, wc_office_arena ships twenty-seven, and 312 of 696 extracted
     * payloads carry traversal animation names. The blanket refusal therefore
     * silently produced ZERO climbs and ZERO leaps on roughly half of all
     * modules, and the author was told "nothing can climb that high", which was
     * not the reason.
     *
     * What actually has to hold is narrower. The per-area ownership pass at the
     * bottom rebuilds first/num for every area by scanning the whole array, so
     * pre-existing points are fine in themselves -- but each area's points must
     * stay CONTIGUOUS, and we append ours at the end. That is sound exactly when
     * no area we write a point for already owns points somewhere earlier.
     *
     * Our platform areas are brand new and own nothing. The only real hazard is
     * an existing area we climb FROM -- typically the module floor. So test that
     * one condition instead of refusing everything. */
    {
        unsigned na0 = sh_aas_count(c->a, SH_AAS_L_AREAS);
        for (k = 0; k < n; k++) {
            const unsigned char *ar;
            if (specs[k].from_area < 0 || (unsigned)specs[k].from_area >= na0) continue;
            ar = sh_aas_rec_const(c->a, SH_AAS_L_AREAS, (unsigned)specs[k].from_area);
            if (ar && sh_aas_get_u16(ar, AR_NUM_TRAV_POINT) > 0) {
                /* Interleaving with an existing run would need the whole array
                 * regrouped and every index that points into it rewritten. That
                 * is a real change to records whose ordering semantics are not
                 * fully recovered, so it is refused -- but REPORTED, so the
                 * islands are not blamed on the geometry. */
                c->rep->climbs_declined = 1;
                return 0;
            }
        }
    }

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

/* A platform's XY bounds and its centroid height, from its four corners.
 *
 * The corners replaced the old x0/y1/z members, so what used to be a field read
 * is a fold. The centroid is the right single height for ordering and for the
 * headroom comparison: a tilted face has no one z, and its middle is the honest
 * summary of where it sits. */
static void aug_plat_bounds(const sh_aug_platform *p, double b[4])
{
    int i;
    b[0] = b[2] = p->c[0][0];
    b[1] = b[3] = p->c[0][1];
    for (i = 1; i < 4; i++) {
        if (p->c[i][0] < b[0]) b[0] = p->c[i][0];
        if (p->c[i][1] < b[1]) b[1] = p->c[i][1];
        if (p->c[i][0] > b[2]) b[2] = p->c[i][0];
        if (p->c[i][1] > b[3]) b[3] = p->c[i][1];
    }
}

static double aug_plat_centroid_z(const sh_aug_platform *p)
{
    return ((double)p->c[0][2] + p->c[1][2] + p->c[2][2] + p->c[3][2]) / 4.0;
}

/* Clearance above a platform: the distance to the lowest thing that overlaps it
 * in XY and sits above it, counting both the module's own areas and the other
 * platforms in this bake.
 *
 * REFUSED platforms are counted too, and that is deliberate. A volume this bake
 * declined to make walkable -- too small, too steep, out of range -- is still a
 * solid box standing in the world. Skipping it here would let us emit a walkable
 * area in the space underneath one, and demons would spawn into a ceiling. The
 * question this asks is "what is physically above me", not "what did we
 * navigate".
 *
 * The platform overlap test is exact rather than an AABB comparison: for a yawed
 * quad the bounding box is strictly larger, so an AABB test reports overlap
 * between two rotated platforms that do not actually meet and refuses one of
 * them for headroom it really has. */
static double aug_headroom(aug_ctx *c, const aug_quad *p,
                           const sh_aug_platform *all, int n, int self)
{
    double best = 1e30;
    /* A tilted face has no single height; its centroid is the honest summary of
     * where it sits for a clearance comparison. */
    double pz = (p->c[0][2] + p->c[1][2] + p->c[2][2] + p->c[3][2]) / 4.0;
    unsigned i, na = sh_aas_count(c->a, SH_AAS_L_AREAS);
    int k;
    for (i = 1; i < na; i++) {
        const unsigned char *b = sh_aas_rec_const(c->a, SH_AAS_L_AREABOUNDS, i);
        double minz;
        if (!b) continue;
        minz = sh_aas_get_i16(b, AB_MINZ);
        if (minz <= pz) continue;
        if (sh_aas_get_i16(b, AB_MAXX) <= p->x0 || sh_aas_get_i16(b, AB_MINX) >= p->x1) continue;
        if (sh_aas_get_i16(b, AB_MAXY) <= p->y0 || sh_aas_get_i16(b, AB_MINY) >= p->y1) continue;
        if (minz - pz < best) best = minz - pz;
    }
    for (k = 0; k < n; k++) {
        double b[4], z;
        aug_quad other;
        if (k == self) continue;
        z = aug_plat_centroid_z(&all[k]);
        if (z <= pz) continue;
        aug_plat_bounds(&all[k], b);
        /* Cheap AABB reject first; it can only ever admit too many. */
        if (b[2] <= p->x0 || b[0] >= p->x1) continue;
        if (b[3] <= p->y0 || b[1] >= p->y1) continue;
        /* Then the exact test, so a rotated neighbour whose bounding box
         * overlaps but whose footprint does not is not counted. */
        if (aug_quad_from_platform(&other, &all[k]) &&
            aug_quad_gap(p, &other) > 0.0) continue;
        if (z - pz < best) best = z - pz;
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
    aug_quad effs[SH_AUG_MAX_PLATFORMS];
    aug_quad reqs[SH_AUG_MAX_PLATFORMS];
    aug_peer peers[SH_AUG_MAX_PLATFORMS];
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
    c.min_floor_cos = sh_aas_setting_f32(a, SET_MIN_FLOOR_COS);
    /* Fail CLOSED. sh_aas_setting_f32 answers 0.0f for a model it cannot read,
     * and a gate of "normal.z < 0" would cheerfully accept a vertical wall as
     * floor. A payload we cannot read the slope limit out of is one we decline
     * to augment. */
    if (c.min_floor_cos <= 0.0 || c.min_floor_cos > 1.0) return 0;
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
        for (j = i - 1; j >= 0 &&
             aug_plat_centroid_z(&plats[order[j]]) > aug_plat_centroid_z(&plats[key]);
             j--)
            order[j + 1] = order[j];
        order[j + 1] = key;
    }

    for (k = 0; k < n; k++) {
        int idx = order[k];
        sh_aug_platform_result *pr = &out->platforms[idx];
        const sh_aug_platform *p = &plats[idx];
        aug_quad req, eff;
        int carrier, area;
        double head;

        memcpy(pr->name, p->name, sizeof pr->name);
        pr->name[sizeof pr->name - 1] = 0;
        pr->area = -1;
        pr->carrier = -1;

        pr->tilt_degrees = (float)aug_degrees_from_horizontal(p->n[2]);
        /* face 4 is an UPRIGHT box's top. Anything else means the author is
         * standing on what they think of as a side, which is correct for a
         * tipped box and worth telling them. */
        pr->side_face = (p->face != 4);

        if (!aug_quad_from_platform(&req, p)) {
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "this face has no footprint to walk on");
            continue;
        }
        if ((double)p->n[2] < c.min_floor_cos) {
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "too steep: this face is %.0f degrees from horizontal, "
                        "this nav class walks up to %.1f",
                        pr->tilt_degrees,
                        aug_degrees_from_horizontal(c.min_floor_cos));
            continue;
        }
        if (!aug_quad_inset(&req, c.radius, &eff) ||
            aug_quad_min_width(&eff) < (fw < fd ? fw : fd)) {
            /* The AABB would overstate a rotated quad's usable size, so the
             * comparison is against its narrowest edge-to-corner width. */
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "too small: %.0f units across after the %.0f-unit "
                        "agent-radius inset, this demon size needs %.0fx%.0f",
                        aug_quad_min_width(&req) - 2.0 * c.radius, c.radius, fw, fd);
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
            aug_quad_min_z(&eff) <= AUG_INT16_LO ||
            aug_quad_max_z(&eff) >= AUG_INT16_HI) {
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "outside the coordinate range navigation bounds can hold "
                        "(+/-32767)");
            continue;
        }

        {
            double mx = (eff.x0 + eff.x1) / 2.0, my = (eff.y0 + eff.y1) / 2.0;
            carrier = sh_aas_point_area(a, (float)mx, (float)my,
                                        (float)aug_z_at(&eff, mx, my));
        }
        area = aug_add_area(&c, &eff, carrier);
        if (area < 0) {
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "the navigation file is full");
            c.failed = 1;
            break;
        }
        pr->leaf_slots_carved = aug_carve(&c, area, &eff);
        if (pr->leaf_slots_carved <= 0) {
            /* The area exists in the array but the tree cannot reach it. Leaving
             * it would put a phantom in the cluster bookkeeping -- counted, linked
             * and included in trees[0].c, yet unreachable by the BSP walk the
             * router uses. Say so and do not claim it was emitted. */
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "nowhere to splice this into the navigation tree; nothing "
                        "walkable sits under it");
            continue;
        }
        pr->emitted = 1;
        pr->area = area;
        pr->carrier = carrier;
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
        /* The peer table: every quad this bake actually created. Refused
         * platforms are absent -- `nmade` only advances on success -- so the
         * effs[i] <-> plats[i] identity is broken and nothing may rely on it.
         *
         * This is the second neighbour source, and it is what fixes chaining:
         * the augmenter already holds every quad it made, so it does not have to
         * rediscover them through the BSP, where the agent-radius inset hides
         * them behind 48 to 128 units of floor. */
        for (i = 0; i < nmade; i++) {
            peers[i].req = &reqs[i];
            peers[i].eff = &effs[i];
            peers[i].area = made[i];
        }
        for (i = 0; i < nmade; i++)
            aug_step_links(&c, made[i], &effs[i], &reqs[i], peers, nmade);
        for (i = 0; i < nmade; i++)
            aug_fall_links(&c, made[i], &effs[i], &reqs[i], peers, nmade);

        /* Climbs LAST. The traversal reachabilities must be a contiguous tail of
         * the reachability array -- that is how every shipped donor lays them
         * out, and traversalPoint.d28 indexes into it. */
        {
            aug_trav_spec *specs = (aug_trav_spec *)HeapAlloc(
                GetProcessHeap(), 0, AUG_MAX_TRAVERSALS * sizeof(aug_trav_spec));
            if (specs) {
                int total = 0;
                for (i = 0; i < nmade; i++) {
                    int room = AUG_MAX_TRAVERSALS - total;
                    int got = aug_traversal_specs(&c, made[i], &effs[i], &reqs[i],
                                                  peers, nmade, specs + total, room);
                    total += got;
                    /* Filling the budget exactly means the collector stopped
                     * because it ran out of room, not because it ran out of
                     * geometry. Say so: a silently truncated bake reads to an
                     * author exactly like a complete one. */
                    if (got == room) out->links_truncated = 1;
                }
                /* Count leaps only for specs that were actually WRITTEN.
                 * aug_emit_traversals can decline the whole set -- a module that
                 * already owns traversal points on an area we climb from -- and
                 * counting the collected specs beforehand reported leaps the
                 * payload does not contain, which is a report that lies to the
                 * author about what their map got.
                 *
                 * A leap and a climb write the same five records and are
                 * indistinguishable in the payload afterwards, so the count has
                 * to come from the specs; it just has to come from the ones that
                 * survived. */
                if (total > 0 && aug_emit_traversals(&c, specs, total) > 0) {
                    for (i = 0; i < total; i++) {
                        if (!specs[i].is_leap) continue;
                        for (j = 0; j < n; j++)
                            if (out->platforms[j].area == specs[i].from_area ||
                                out->platforms[j].area == specs[i].to_area)
                                out->platforms[j].leaps++;
                    }
                }
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
        int links = 0, climbs = 0, neighbours = 0;
        unsigned seen[SH_TRAV_MAX_MONSTERS];
        int nbr[SH_AUG_MAX_PLATFORMS];
        int nseen = 0;
        unsigned r, nr = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
        for (r = 0; r < nr; r++) {
            const unsigned char *rr = sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, r);
            unsigned flags;
            int other, q, dupn = 0;
            if (!rr) continue;
            if ((int)sh_aas_get_u16(rr, RE_FROM_AREA) != made[i] &&
                (int)sh_aas_get_u16(rr, RE_TO_AREA) != made[i]) continue;
            links++;
            /* How many DISTINCT areas this platform reaches. The author's real
             * question after placing two volumes together is whether they are
             * joined to each other, and "links" alone cannot answer it -- a
             * platform with forty links to the floor looks identical to one
             * chained to its neighbour. */
            other = (int)sh_aas_get_u16(rr, RE_FROM_AREA) == made[i]
                  ? (int)sh_aas_get_u16(rr, RE_TO_AREA)
                  : (int)sh_aas_get_u16(rr, RE_FROM_AREA);
            for (q = 0; q < neighbours; q++) if (nbr[q] == other) { dupn = 1; break; }
            if (!dupn && neighbours < SH_AUG_MAX_PLATFORMS) nbr[neighbours++] = other;
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
                out->platforms[j].neighbours = neighbours;
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

/* The quad geometry, reached through an opaque buffer because aug_quad is
 * internal and the tests are a separate translation unit. */
size_t sh_aug_test_quad_size(void) { return sizeof(aug_quad); }

int sh_aug_test_quad_init(void *quad, const double corners[4][3])
{
    return aug_quad_init((aug_quad *)quad, corners);
}

double sh_aug_test_z_at(const void *quad, double x, double y)
{
    return aug_z_at((const aug_quad *)quad, x, y);
}

int sh_aug_test_quad_inset(const void *quad, double r, void *out)
{
    return aug_quad_inset((const aug_quad *)quad, r, (aug_quad *)out);
}

int sh_aug_test_quad_contains(const void *quad, double x, double y)
{
    return aug_quad_contains_xy((const aug_quad *)quad, x, y);
}

void sh_aug_test_quad_corner(const void *quad, int i, double out[3])
{
    const aug_quad *q = (const aug_quad *)quad;
    out[0] = q->c[i & 3][0]; out[1] = q->c[i & 3][1]; out[2] = q->c[i & 3][2];
}

double sh_aug_test_quad_normal_z(const void *quad)
{
    return ((const aug_quad *)quad)->n[2];
}

int sh_aug_test_node_field4(double plane_c) { return aug_node_field4(plane_c); }
#endif
