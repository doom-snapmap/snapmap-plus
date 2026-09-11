/* Augment AAS in two passes: create areas and BSP leaves, then connect them
 * with walk, fall, climb and leap reachabilities. Geometry, interning and
 * link construction share aug_ctx. Keep the clockwise inward-normal
 * convention consistent across inset, containment, BSP carving and neighbour
 * queries.
 */
#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include "aas_augment.h"
#include "nav_geometry.h"
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

/* node.field4 values are copied from shipped nodes; their meaning is unknown.
 * See the orientation rule below.
 */
#define NODE_F4_Z               1
/* Set node.field4 when the split plane has a Z component, including oblique
 * planes. Yawed vertical planes use 0. This matches all 12,450 classified
 * donor nodes.
 */
#define NODE_F4_XY              0

/* Spacing between successive walk reachabilities along a shared edge, and the
 * inset from that edge's ends. Bracketed by the Grid Room itself: a 4880-unit
 * edge gets 9 intervals and a 336-unit edge gets 1, so the divisor lies in
 * (541.2, 608.75]; any value in the bracket reproduces both. */
#define WALK_SPACING            576.0f
#define WALK_END_INSET          5.0f
/* Each endpoint is displaced one unit perpendicular, into its own area. */
#define REACH_SIDE_OFFSET       1.0f

/* Floor area flags, copied from shipped data. */
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
/* Demote an edge from boundary to shared when a second area reuses it. */
#define EDGE_FLAGS_BOUNDARY     0x00000C01
#define EDGE_FLAGS_SHARED       0x00000C00

#define AUG_INT16_LO            (-32768)
#define AUG_INT16_HI            (32767)

/* Settings word offsets follow type + three length-prefixed 64-byte strings. */
#define SET_WORDS               208
#define SET_W(n)                (SET_WORDS + (n) * 4)
#define SET_MAX_STEP_HEIGHT     SET_W(12)
/* minFloorCos is settings word 16 (0.7 in shipped payloads). The disk layout
 * omits and reorders fields from idAAS2Settings; struct offsets do not apply.
 */
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
/* trees records are 24 bytes: three floats followed by three ints. Root node
 * a is at +12; area count c is at +20. +0 is a direction component, not the
 * root.
 */
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

/* Planar convex surface, clockwise from +Z. n.p+d=0 defines its height and
 * n[2]>0. x0..y1 is an AABB for rejection and bounds records; containment
 * must use the polygon.
 */
typedef struct aug_quad {
    double c[SH_AUG_MAX_CORNERS][3];
    int count;
    double n[3];
    double d;
    double x0, y0, x1, y1;
} aug_quad;

static int aug_poly_init(aug_quad *q, const double c[][3], int count)
{
    double len;
    int i, k;

    if(count<3||count>SH_AUG_MAX_CORNERS)return 0;
    q->count = count;
    for (i = 0; i < count; i++)
        for (k = 0; k < 3; k++) q->c[i][k] = c[i][k];
    /* Clipping can leave collinear vertices at a partition seam. Accumulate
     * the whole polygon normal rather than relying on one corner. */
    q->n[0]=q->n[1]=q->n[2]=0.0;
    for(i=1;i+1<count;i++) {
        double u[3],v[3];
        for(k=0;k<3;k++){u[k]=c[i][k]-c[0][k];v[k]=c[i+1][k]-c[0][k];}
        q->n[0]+=u[1]*v[2]-u[2]*v[1];
        q->n[1]+=u[2]*v[0]-u[0]*v[2];
        q->n[2]+=u[0]*v[1]-u[1]*v[0];
    }
    len = sqrt(q->n[0]*q->n[0] + q->n[1]*q->n[1] + q->n[2]*q->n[2]);
    if (len < 1e-6) return 0;
    for (k = 0; k < 3; k++) q->n[k] /= len;
    if (q->n[2] < 0.0) for (k = 0; k < 3; k++) q->n[k] = -q->n[k];
    if (q->n[2] < 1e-6) return 0;            /* vertical: nothing to stand on */
    q->d = -(q->n[0]*c[0][0] + q->n[1]*c[0][1] + q->n[2]*c[0][2]);
    q->x0 = q->x1 = c[0][0];
    q->y0 = q->y1 = c[0][1];
    for (i = 1; i < count; i++) {
        if (c[i][0] < q->x0) q->x0 = c[i][0];
        if (c[i][0] > q->x1) q->x1 = c[i][0];
        if (c[i][1] < q->y0) q->y0 = c[i][1];
        if (c[i][1] > q->y1) q->y1 = c[i][1];
    }
    return 1;
}

static int aug_quad_init(aug_quad *q, const double c[4][3])
{ return aug_poly_init(q, c, 4); }

static int aug_quad_from_platform(aug_quad *q, const sh_aug_platform *p)
{
    double c[SH_AUG_MAX_CORNERS][3];
    int count = p->corners ? p->corners : 4;
    int i, k;
    if(count<3||count>SH_AUG_MAX_CORNERS)return 0;
    for (i = 0; i < count; i++)
        for (k = 0; k < 3; k++) c[i][k] = p->c[i][k];
    return aug_poly_init(q, c, count);
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
    for (i = 1; i < q->count; i++) if (q->c[i][2] < v) v = q->c[i][2];
    return v;
}

static double aug_quad_max_z(const aug_quad *q)
{
    double v = q->c[0][2];
    int i;
    for (i = 1; i < q->count; i++) if (q->c[i][2] > v) v = q->c[i][2];
    return v;
}

/* Clockwise edges have inward XY normal (ey,-ex), a -90 degree rotation. The
 * opposite sign breaks inset, containment, BSP lateral planes and neighbour
 * probes.
 */
static void aug_edge_normal_in(const aug_quad *q, int i, double out[2])
{
    int j = (i + 1) % q->count;
    double ex = q->c[j][0] - q->c[i][0];
    double ey = q->c[j][1] - q->c[i][1];
    double len = sqrt(ex*ex + ey*ey);
    if (len < 1e-9) { out[0] = out[1] = 0.0; return; }
    out[0] =  ey / len;
    out[1] = -ex / len;
}

/* Inset by shifting every edge inward by r and intersecting adjacent lines.
 * Returns 0 if the polygon collapses or reverses. An AABB inset is incorrect
 * for rotated faces.
 */
static int aug_quad_inset(const aug_quad *in, double r, aug_quad *out)
{
    double nx[SH_AUG_MAX_CORNERS], ny[SH_AUG_MAX_CORNERS], off[SH_AUG_MAX_CORNERS], c[SH_AUG_MAX_CORNERS][3], sh = 0.0;
    int i;

    if (r <= 0.0) { *out = *in; return 1; }
    for (i = 0; i < in->count; i++) {
        double n2[2];
        aug_edge_normal_in(in, i, n2);
        if (n2[0] == 0.0 && n2[1] == 0.0) return 0;
        nx[i] = n2[0]; ny[i] = n2[1];
        off[i] = nx[i]*in->c[i][0] + ny[i]*in->c[i][1] + r;
    }
    for (i = 0; i < in->count; i++) {
        int pv = (i + in->count - 1) % in->count;
        double det = nx[pv]*ny[i] - nx[i]*ny[pv];
        if (det > -1e-9 && det < 1e-9) return 0;
        c[i][0] = (off[pv]*ny[i] - off[i]*ny[pv]) / det;
        c[i][1] = (nx[pv]*off[i] - nx[i]*off[pv]) / det;
        c[i][2] = aug_z_at(in, c[i][0], c[i][1]);
    }
    for (i = 0; i < in->count; i++) {
        int j = (i + 1) % in->count;
        sh += c[i][0]*c[j][1] - c[j][0]*c[i][1];
    }
    if (sh > -1.0) return 0;             /* collapsed, or wound the other way */
    /* Check every corner against every inset half-plane. An over-inset
     * polygon can remain clockwise after turning inside out, so winding alone
     * is insufficient.
     */
    for (i = 0; i < in->count; i++) {
        int j;
        for (j = 0; j < in->count; j++)
            if (nx[j]*c[i][0] + ny[j]*c[i][1] < off[j] - 1e-6) return 0;
    }
    return aug_poly_init(out, c, in->count);
}

/* Inside the quad in XY. The AABB is only the cheap reject; the real test is the
 * sign of every edge's inward normal. */
static int aug_quad_contains_xy(const aug_quad *q, double x, double y)
{
    int i;
    if (x < q->x0 - 1e-6 || x > q->x1 + 1e-6 ||
        y < q->y0 - 1e-6 || y > q->y1 + 1e-6) return 0;
    for (i = 0; i < q->count; i++) {
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
    for (i = 0; i < q->count; i++) {
        double n2[2];
        aug_edge_normal_in(q, i, n2);
        for (k = 0; k < q->count; k++) {
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
    const sh_aug_platform *solids;
    int solid_count;
    int prepared_geometry;
    unsigned original_areas;
} aug_ctx;

/* Read the native floor polygon, not its height bounds. Original module
 * floors can slope too; their maximum Z is not the landing height everywhere. */
static int aug_original_floor(const sh_aas *a,unsigned area,aug_quad *out)
{
    const unsigned char *ar=sh_aas_rec_const(a,SH_AAS_L_AREAS,area);
    double corners[SH_AUG_MAX_CORNERS][3];unsigned count,first,i,k;
    if(!ar||!(sh_aas_get_u32(ar,AR_FLAGS)&AREA_FLAGS_FLOOR))return 0;
    count=sh_aas_get_u16(ar,AR_NUM_EDGES);first=sh_aas_get_u32(ar,AR_FIRST_EDGE_INDEX);
    if(count<3||count>SH_AUG_MAX_CORNERS)return 0;
    for(i=0;i<count;i++) {
        const unsigned char *ix=sh_aas_rec_const(a,SH_AAS_L_EDGEINDEX,first+i),*edge,*v;
        int ei;
        if(!ix)return 0;ei=sh_aas_get_i32(ix,0);
        edge=sh_aas_rec_const(a,SH_AAS_L_EDGES,(unsigned)abs(ei));if(!edge)return 0;
        v=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(edge,ei<0?4:0));
        if(!v)return 0;
        for(k=0;k<3;k++)corners[i][k]=sh_aas_get_f32(v,k*4);
    }
    return aug_poly_init(out,corners,(int)count);
}

/* An AABB, as (minx, miny, minz, maxx, maxy, maxz). */
typedef struct aug_box { double v[6]; } aug_box;

/* ---- interning --------------------------------------------------------- */

/* Intern vertices, edges and planes by value to avoid duplicate geometry. */
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

/* Return a signed edge index: negative means reverse traversal. Mark an
 * existing edge shared when a second area uses it.
 */
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

/* Area bounds: minx, miny, maxx, maxy, minz, maxz. Sloped areas are valid;
 * maxz is only a fallback landing height when no floor polygon is available.
 */
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

/* Use the carrier's cluster so the platform shares its routing scope. Fall
 * back to the largest cluster when no carrier is available.
 */
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

/* Obstacle visibility is an area-indexed compressed bitset, not an opaque
 * carrier blob. A literal supplies seven bits; high-bit tokens skip zero bits.
 * Native movement decodes to the current area count and truncates its corridor
 * at the first invisible area. Lift each original visibility row through the
 * carrier mapping, including every new area, after all geometry is emitted.
 * This conservatively retains the module's visibility partition and includes
 * sibling surfaces without exposing walls from unrelated module regions. */
static int aug_rebuild_pvs(aug_ctx *c)
{
    unsigned old=c->original_areas, n=sh_aas_count(c->a,SH_AAS_L_AREAS);
    unsigned bytes=sh_aas_count(c->a,SH_AAS_L_OBSTACLEPVS), i,j,first;
    unsigned *parent=NULL,*offset=NULL,*rows=NULL;
    unsigned char *visible=NULL,*encoded=NULL;
    int ok=0;
    if(n==old)return 1;
    parent=(unsigned*)malloc(n*sizeof *parent);
    offset=(unsigned*)malloc(old*sizeof *offset);
    rows=(unsigned*)malloc(n*sizeof *rows);
    visible=(unsigned char*)malloc(n);
    encoded=(unsigned char*)malloc((n+6)/7+2);
    if(!parent||!offset||!rows||!visible||!encoded)goto done;
    for(i=0;i<old;i++) {
        parent[i]=i;
        offset[i]=sh_aas_get_u32(sh_aas_rec_const(c->a,SH_AAS_L_AREAS,i),AR_FIRST_OBSTACLE_PVS);
    }
    for(i=old;i<n;i++) {
        int found=0,k;
        for(k=0;k<c->rep->platform_count;k++) {
            const sh_aug_platform_result *p=&c->rep->platforms[k];
            if(p->area==(int)i&&p->carrier>0&&(unsigned)p->carrier<i) {
                parent[i]=parent[p->carrier];found=1;break;
            }
        }
        if(!found)goto done;
    }
    for(i=0;i<old;i++) {
        unsigned pos=0,at=offset[i],used=0;
        memset(visible,0,n);
        while(pos<old) {
            unsigned token,run,k;
            const unsigned char *p;
            if(at>=bytes)goto done;
            p=sh_aas_rec_const(c->a,SH_AAS_L_OBSTACLEPVS,at++);token=*p;
            if(token&0x80) {
                run=token&0x3f;
                if(token&0x40) {
                    if(at>=bytes)goto done;
                    run|=(unsigned)*sh_aas_rec_const(c->a,SH_AAS_L_OBSTACLEPVS,at++)<<6;
                }
                pos+=run+1;
            } else for(k=0;k<7&&pos<old;k++,pos++)visible[pos]=(token>>k)&1;
        }
        if(i)visible[i]=1;
        /* A construction can cross the module's original visibility boundary.
         * Include its actual new route neighbors in both directions as well
         * as the carrier row; otherwise a large deck can still lose an exit. */
        for(j=c->rep->reach_before;j<sh_aas_count(c->a,SH_AAS_L_REACHABILITIES);j++) {
            const unsigned char *r=sh_aas_rec_const(c->a,SH_AAS_L_REACHABILITIES,j);
            unsigned from=sh_aas_get_u16(r,RE_FROM_AREA),to=sh_aas_get_u16(r,RE_TO_AREA);
            if(from>=n||to>=n)goto done;
            if(parent[from]==i)visible[to]=visible[parent[to]]=1;
            if(parent[to]==i)visible[from]=visible[parent[from]]=1;
        }
        for(j=old;j<n;j++)visible[j]=visible[parent[j]];
        pos=0;
        while(pos<n) {
            unsigned run=0,k,token=0;
            while(pos+run<n&&!visible[pos+run]&&run<16384)run++;
            if(run>=7) {
                unsigned count=run-1;
                encoded[used++]=(unsigned char)(0x80|(count&63)|(count>=64?0x40:0));
                if(count>=64)encoded[used++]=(unsigned char)(count>>6);
                pos+=run;
            } else {
                for(k=0;k<7&&pos<n;k++,pos++)token|=(unsigned)visible[pos]<<k;
                encoded[used++]=(unsigned char)token;
            }
        }
        if(!sh_aas_append(c->a,SH_AAS_L_OBSTACLEPVS,used,&first))goto done;
        memcpy(sh_aas_rec(c->a,SH_AAS_L_OBSTACLEPVS,first),encoded,used);
        rows[i]=first-bytes;
    }
    first=sh_aas_count(c->a,SH_AAS_L_OBSTACLEPVS)-bytes;
    for(i=old;i<n;i++) {
        unsigned root=parent[i],end=root+1<old?rows[root+1]:first;
        unsigned length=end-rows[root],at;
        /* Keep a complete row at each area's increasing offset. Readers may
         * use either explicit decoding or the next offset to bound a row. */
        memcpy(encoded,sh_aas_rec_const(c->a,SH_AAS_L_OBSTACLEPVS,bytes+rows[root]),length);
        if(!sh_aas_append(c->a,SH_AAS_L_OBSTACLEPVS,length,&at))goto done;
        memcpy(sh_aas_rec(c->a,SH_AAS_L_OBSTACLEPVS,at),encoded,length);
        rows[i]=at-bytes;
    }
    first=sh_aas_count(c->a,SH_AAS_L_OBSTACLEPVS)-bytes;
    memmove(sh_aas_rec(c->a,SH_AAS_L_OBSTACLEPVS,0),
            sh_aas_rec_const(c->a,SH_AAS_L_OBSTACLEPVS,bytes),first);
    if(!sh_aas_truncate(c->a,SH_AAS_L_OBSTACLEPVS,first))goto done;
    for(i=0;i<n;i++)sh_aas_put_u32(sh_aas_rec(c->a,SH_AAS_L_AREAS,i),
                                   AR_FIRST_OBSTACLE_PVS,rows[i]);
    ok=1;
done:
    free(parent);free(offset);free(rows);free(visible);free(encoded);
    return ok;
}

/* Append one walkable area for `eff` and return its index, or -1. */
static int aug_add_area(aug_ctx *c, const aug_quad *eff, int carrier)
{
    /* Closed edge loop, clockwise from +Z. */
    int vs[SH_AUG_MAX_CORNERS];
    unsigned first_ei = sh_aas_count(c->a, SH_AAS_L_EDGEINDEX);
    unsigned first_pvs = 0, first_area, first_bounds, slot;
    unsigned char *ar, *ab;
    int cluster, can = 0, k;
    unsigned i, n;

    for (k = 0; k < eff->count; k++) {
        vs[k] = aug_vertex(c, (float)eff->c[k][0], (float)eff->c[k][1],
                              (float)eff->c[k][2]);
        if (vs[k] < 0) return -1;
    }
    for (k = 0; k < eff->count; k++) {
        /* Signed: a negative entry means the edge is traversed reversed, so 0 is
         * the failure value here rather than a negative one. */
        int e = aug_edge(c, vs[k], vs[(k + 1) % eff->count]);
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

    /* Final visibility rows are rebuilt once the complete area set is known. */

    if (!sh_aas_append(c->a, SH_AAS_L_AREAS, 1, &first_area)) return -1;
    if (!sh_aas_append(c->a, SH_AAS_L_AREABOUNDS, 1, &first_bounds)) return -1;
    ar = sh_aas_rec(c->a, SH_AAS_L_AREAS, first_area);
    ab = sh_aas_rec(c->a, SH_AAS_L_AREABOUNDS, first_bounds);
    if (!ar || !ab) return -1;

    sh_aas_put_u32(ar, AR_FLAGS, AREA_FLAGS_FLOOR);
    sh_aas_put_u16(ar, AR_TRAVEL_FLAGS, AREA_TRAVEL_FLAGS_FLOOR);
    sh_aas_put_u16(ar, AR_NUM_EDGES, (uint16_t)eff->count);
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

/* Clip only against axis-aligned planes. The resulting AABB remains a
 * superset of the BSP cell, allowing conservative pruning.
 */
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

/* Replace occupied BSP leaf slots under eff with a face-plane test and one
 * lateral test per polygon edge. Failed tests retain the original leaf. Never
 * carve void leaves: their field4 encoding is unresolved, so overhangs
 * outside existing navigation remain unavailable. Returns carved slot count.
 */
static int aug_carve(aug_ctx *c, int area, const aug_quad *eff)
{
    typedef struct { int node; aug_box cell; } aug_frame;
    struct { int plane; int f4; } split[SH_AUG_MAX_CORNERS + 1];
    aug_frame *stack;
    unsigned cap = 256, top = 0;
    int carved = 0, root, i;
    aug_box box;
    const double BIG = 1e9;
    const double eps = 1e-3;

    /* Offset the face plane vertically by BSP_FLOOR_EPS. Multiply by n.z so
     * tilted planes retain the same vertical offset.
     */
    split[0].plane = aug_plane(c, (float)eff->n[0], (float)eff->n[1], (float)eff->n[2],
                                  (float)(eff->d + BSP_FLOOR_EPS * eff->n[2]));
    split[0].f4 = NODE_F4_Z;
    /* Each polygon edge supplies an inward-facing vertical split plane.
     * child0 is the positive/interior side.
     */
    for (i = 0; i < eff->count; i++) {
        double n2[2];
        aug_edge_normal_in(eff, i, n2);
        split[1 + i].plane = aug_plane(c, (float)n2[0], (float)n2[1], 0.0f,
            (float)(-(n2[0]*eff->c[i][0] + n2[1]*eff->c[i][1])));
        split[1 + i].f4 = aug_node_field4(0.0);
    }
    for (i = 0; i < eff->count + 1; i++) if (split[i].plane < 0) return 0;

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
                int keep[SH_AUG_MAX_CORNERS + 1], nkeep = 0, head, outside = 0, k;
                for (k = 0; k < eff->count + 1; k++) {
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

/* Clip a line to the emitted floor loop, independently of its BSP volume.
 * Walking advances from this loop's exit to a neighboring loop's entry. */
static int aug_floor_span(const sh_aas *a,int area,const double start[3],
                          const double end[3],double *lo,double *hi)
{
    const unsigned char *ar=sh_aas_rec_const(a,SH_AAS_L_AREAS,(unsigned)area);
    unsigned first;int i,n;
    if(!ar)return 0;
    n=sh_aas_get_u16(ar,AR_NUM_EDGES);first=sh_aas_get_u32(ar,AR_FIRST_EDGE_INDEX);
    *lo=0;*hi=1;
    for(i=0;i<n;i++) {
        const unsigned char *ix=sh_aas_rec_const(a,SH_AAS_L_EDGEINDEX,first+(unsigned)i);
        const unsigned char *edge,*v,*w;int ei;double nx,ny,d0,d1,t;
        if(!ix)return 0;ei=sh_aas_get_i32(ix,0);
        edge=sh_aas_rec_const(a,SH_AAS_L_EDGES,(unsigned)abs(ei));if(!edge)return 0;
        v=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(edge,ei<0?4:0));
        w=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(edge,ei<0?0:4));
        if(!v||!w)return 0;
        nx=sh_aas_get_f32(w,4)-sh_aas_get_f32(v,4);
        ny=sh_aas_get_f32(v,0)-sh_aas_get_f32(w,0);
        d0=nx*(start[0]-sh_aas_get_f32(v,0))+ny*(start[1]-sh_aas_get_f32(v,4));
        d1=nx*(end[0]-sh_aas_get_f32(v,0))+ny*(end[1]-sh_aas_get_f32(v,4));
        if(d0<0&&d1<0)return 0;
        if((d0<0)==(d1<0))continue;
        t=d0/(d0-d1);
        if(d0<0){if(t>*lo)*lo=t;}else if(t<*hi)*hi=t;
    }
    return n>=3&&*lo<=*hi;
}

/* Reject a near-corner shortcut through an exposed edge even when its small
 * floor gap falls within the native trace tolerance. */
static int aug_floor_wall_at(const sh_aas *a,int area,double x,double y)
{
    const unsigned char *ar=sh_aas_rec_const(a,SH_AAS_L_AREAS,(unsigned)area);unsigned i;
    for(i=0;i<sh_aas_get_u16(ar,AR_NUM_EDGES);i++) {
        const unsigned char *ix=sh_aas_rec_const(a,SH_AAS_L_EDGEINDEX,sh_aas_get_u32(ar,AR_FIRST_EDGE_INDEX)+i);
        const unsigned char *ed=sh_aas_rec_const(a,SH_AAS_L_EDGES,(unsigned)abs(sh_aas_get_i32(ix,0)));
        const unsigned char *v,*w;double dx,dy,len,along,vx,vy;
        if(!(sh_aas_get_u32(ed,8)&1))continue;
        v=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(ed,0));
        w=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(ed,4));
        vx=sh_aas_get_f32(v,0);vy=sh_aas_get_f32(v,4);
        dx=sh_aas_get_f32(w,0)-vx;dy=sh_aas_get_f32(w,4)-vy;len=hypot(dx,dy);
        if(len<1e-8)continue;
        along=((x-vx)*dx+(y-vy)*dy)/len;
        if(along>0.04&&along<len-0.04&&fabs(dx*(y-vy)-dy*(x-vx))<0.02*len)return 1;
    }
    return 0;
}

/* Reachabilities store integer coordinates. Validate the stored point, not
 * just its floating-point precursor: truncation can cross a yawed seam.
 * Preserve the old truncation when valid, otherwise try the adjacent integer
 * corners of the same coordinate cell and retain the closest valid one. */
static int aug_reach_point(const sh_aas *a,int area,const double p[3],double out[3])
{
    double best=1e30;int i,k,found=0;
    for(k=0;k<3;k++) {
        if(!isfinite(p[k])||p[k]<-32768.0||p[k]>32767.0)return 0;
        out[k]=(double)aug_trunc(p[k]);
    }
    if(sh_aas_point_area(a,(float)out[0],(float)out[1],(float)(out[2]+2))==area)return 1;
    for(i=0;i<8;i++) {
        double q[3],d=0;
        for(k=0;k<3;k++){q[k]=(i&(1<<k))?ceil(p[k]):floor(p[k]);d+=(q[k]-p[k])*(q[k]-p[k]);}
        if(d>=best||sh_aas_point_area(a,(float)q[0],(float)q[1],(float)(q[2]+2))!=area)continue;
        memcpy(out,q,sizeof q);best=d;found=1;
    }
    return found;
}

static int aug_reach(aug_ctx *c, unsigned flags, int time, int from, int to,
                     const double s[3], const double e[3])
{
    unsigned first;
    unsigned char *r;
    double qs[3],qe[3];
    aug_quad native_floor;
    if(!aug_reach_point(c->a,from,s,qs)||!aug_reach_point(c->a,to,e,qe))return 0;
    if(flags==REACH_WALK&&fabs(qs[2]-qe[2])>c->step)return 0;
    if(flags==REACH_WALK&&c->prepared_geometry&&
       ((unsigned)from>=c->original_areas||aug_original_floor(c->a,(unsigned)from,&native_floor))&&
       ((unsigned)to>=c->original_areas||aug_original_floor(c->a,(unsigned)to,&native_floor))) {
        double a0,a1,b0,b1,dx=qe[0]-qs[0],dy=qe[1]-qs[1];
        if(!aug_floor_span(c->a,from,qs,qe,&a0,&a1)||!aug_floor_span(c->a,to,qs,qe,&b0,&b1)||
           (b0-a1)*sqrt(dx*dx+dy*dy)>0.2||
           aug_floor_wall_at(c->a,from,qs[0]+a1*dx,qs[1]+a1*dy)||
           aug_floor_wall_at(c->a,to,qs[0]+b0*dx,qs[1]+b0*dy))return 0;
    }
    s=qs;e=qe;
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
            if(sh_aas_point_area(c->a,(float)s[0],(float)s[1],(float)(s[2]+2.0))!=ai ||
               sh_aas_point_area(c->a,(float)e[0],(float)e[1],(float)(e[2]+2.0))!=bi)continue;
            if (aug_reach(c, REACH_WALK, REACH_WALK_TIME, ai, bi, s, e)) added++;
        }
    } else if (oy1 == oy0 && ox1 > ox0) {
        double sgn = (((double)A[1] + A[3]) / 2.0 < oy0) ? 1.0 : -1.0;
        n = aug_samples((float)ox0, (float)ox1, ts, 64);
        for (k = 0; k < n; k++) {
            double s[3], e[3];
            s[0] = ts[k]; s[1] = oy0 - sgn * REACH_SIDE_OFFSET; s[2] = A[5];
            e[0] = ts[k]; e[1] = oy0 + sgn * REACH_SIDE_OFFSET; e[2] = B[5];
            if(sh_aas_point_area(c->a,(float)s[0],(float)s[1],(float)(s[2]+2.0))!=ai ||
               sh_aas_point_area(c->a,(float)e[0],(float)e[1],(float)(e[2]+2.0))!=bi)continue;
            if (aug_reach(c, REACH_WALK, REACH_WALK_TIME, ai, bi, s, e)) added++;
        }
    }
    return added;
}

/* One oriented edge segment and its neighbour, shared by every link regime.
 * drop is near_z-far_z; classify its magnitude separately from direction so
 * rises cannot be mistaken for steps.
 */
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
    aug_quad support;
    int source;
    int prepared;
} aug_peer;

/* Tolerance for physical footprint contact. Small floating-point gaps use
 * step/climb policy rather than leap policy.
 */
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
    for (i = 0; i < a->count; i++) {
        for (k = 0; k < b->count; k++) {
            double ex = b->c[(k + 1) % b->count][0] - b->c[k][0];
            double ey = b->c[(k + 1) % b->count][1] - b->c[k][1];
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

    }
    for (i = 0; i < b->count; i++)
        if (aug_quad_contains_xy(a, b->c[i][0], b->c[i][1])) return 0.0;
    for (i = 0; i < a->count; i++) for (k = 0; k < b->count; k++) {
        int ai = (i+1)%a->count, bi = (k+1)%b->count;
        double ax=a->c[ai][0]-a->c[i][0], ay=a->c[ai][1]-a->c[i][1];
        double bx=b->c[bi][0]-b->c[k][0], by=b->c[bi][1]-b->c[k][1];
        double dx=b->c[k][0]-a->c[i][0], dy=b->c[k][1]-a->c[i][1];
        double det=ax*by-ay*bx;
        if (fabs(det)>1e-9) {
            double t=(dx*by-dy*bx)/det, u=(dx*ay-dy*ax)/det;
            if(t>=0.0&&t<=1.0&&u>=0.0&&u<=1.0)return 0.0;
        }
    }
    return best;
}

/* Evenly spaced edge parameters in [0,1]. */
static int aug_samples_unit(double *out, int cap)
{
    int i, n = cap < 17 ? cap : 17;
    if (n < 2) { if (cap > 0) out[0] = 0.5; return cap > 0 ? 1 : 0; }
    for (i = 0; i < n; i++) out[i] = (double)i / (double)(n - 1);
    return n;
}

/* Find entry distance and an interior landing point along a ray, or return 0
 * beyond maxd. Animation selection and endpoint placement must use this same
 * distance.
 */
static int aug_ray_entry(const aug_quad *target, double ox, double oy,
                         double dx, double dy, double maxd,
                         double *out_dist, double out_land[2])
{
    double lo=0.0, hi=maxd, at;
    int i;
    for(i=0;i<target->count;i++) {
        double n[2], a, b;
        aug_edge_normal_in(target,i,n);
        a=n[0]*(ox-target->c[i][0])+n[1]*(oy-target->c[i][1]);
        b=n[0]*dx+n[1]*dy;
        if(fabs(b)<1e-10){if(a<0.0)return 0;continue;}
        if(b>0.0){double t=-a/b;if(t>lo)lo=t;}
        else {double t=-a/b;if(t<hi)hi=t;}
        if(lo>hi)return 0;
    }
    if(lo>maxd||hi<0.0)return 0;
    *out_dist=lo;
    at=lo+REACH_SIDE_OFFSET*2.0;
    if(at>hi)at=(lo+hi)*0.5;
    out_land[0]=ox+dx*at;out_land[1]=oy+dy*at;
    return 1;
}

/* Fill one segment record from a run of samples facing one neighbour. */
static void aug_close_segment(aug_ctx *c, aug_side *sd, const aug_quad *eff,
                              int e, const double out2[2], double t0, double t1,
                              int who, const aug_peer *peers, int npeers,
                              const aug_quad *req)
{
    int j = (e + 1) % eff->count, q;
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

        /* Measure physical gap against uninset footprints and landing against
         * the peer's inset area. Otherwise agent clearance appears as a
         * physical gap.
         */
        wx = req->c[e][0] + mt * (req->c[j][0] - req->c[e][0]);
        wy = req->c[e][1] + mt * (req->c[j][1] - req->c[e][1]);
        if (aug_ray_entry(peers[q].req, wx, wy, out2[0], out2[1],
                          SH_TRAV_LEAP_MAX_SPAN, &dist, junk))
            sd->gap = dist < AUG_TOUCH_EPS ? 0.0 : dist;
        else {
            /* At a missed corner ray, fall back to whole-footprint clearance. */
            sd->gap = aug_quad_gap(req, peers[q].req);
            if (sd->gap < AUG_TOUCH_EPS) sd->gap = 0.0;
        }
        if (!aug_ray_entry(peers[q].eff, wx, wy, out2[0], out2[1],
                           SH_TRAV_LEAP_MAX_SPAN + 256.0, &dist, sd->land)) {
            sd->land[0] = (peers[q].eff->x0 + peers[q].eff->x1) / 2.0;
            sd->land[1] = (peers[q].eff->y0 + peers[q].eff->y1) / 2.0;
        }
        /* Evaluate the neighbour's height at the actual landing point. */
        {
            int owner;
            for(owner=0;owner<npeers;owner++)if(peers[owner].eff==eff) {
                double physical_gap=aug_quad_gap(&peers[owner].support,&peers[q].support);
                if(physical_gap<=AUG_TOUCH_EPS)sd->gap=0.0;
                break;
            }
        }
        sd->far_z = aug_z_at(peers[q].eff, sd->land[0], sd->land[1]);
        break;
    }
    if (!sd->generated) {
        float tb[6];aug_quad floor;
        sd->far_z = aug_original_floor(c->a,(unsigned)who,&floor)?
            aug_z_at(&floor,sd->land[0],sd->land[1]):
            aug_area_box(c->a,(unsigned)who,tb)?(double)tb[5]:sd->near_z;
    }
    sd->drop = sd->near_z - sd->far_z;
}

/* Filter peers by AABB within reach before detailed geometry tests. The
 * conservative filter may include extra peers but cannot exclude reachable
 * ones.
 */
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

/* Clip an edge's outward strip against a convex peer. The strip uses
 * coordinates (t, distance), so its projected t interval contains every ray
 * that reaches the peer. Narrow contacts cannot fall between sample points. */
static int aug_peer_interval(const aug_quad *source, int edge,
                             const double dir[2], const aug_quad *target,
                             double reach, double *lo, double *hi)
{
    double poly[SH_AUG_MAX_CORNERS+4][2], next[SH_AUG_MAX_CORNERS+4][2];
    int n=4, i, j, k=(edge+1)%source->count;
    double ex=source->c[k][0]-source->c[edge][0];
    double ey=source->c[k][1]-source->c[edge][1];
    poly[0][0]=0;poly[0][1]=0;poly[1][0]=1;poly[1][1]=0;
    poly[2][0]=1;poly[2][1]=reach;poly[3][0]=0;poly[3][1]=reach;
    for(i=0;i<target->count && n;i++) {
        double normal[2], aa,bb,cc;int nn=0;
        aug_edge_normal_in(target,i,normal);
        aa=normal[0]*ex+normal[1]*ey;
        bb=normal[0]*dir[0]+normal[1]*dir[1];
        cc=normal[0]*(source->c[edge][0]-target->c[i][0])+
           normal[1]*(source->c[edge][1]-target->c[i][1]);
        for(j=0;j<n;j++) {
            int prev=(j+n-1)%n;
            double da=aa*poly[prev][0]+bb*poly[prev][1]+cc;
            double db=aa*poly[j][0]+bb*poly[j][1]+cc;
            if((da>=0)!=(db>=0)) {
                double f=da/(da-db);
                if(nn>=SH_AUG_MAX_CORNERS+4)return -1;
                next[nn][0]=poly[prev][0]+f*(poly[j][0]-poly[prev][0]);
                next[nn++][1]=poly[prev][1]+f*(poly[j][1]-poly[prev][1]);
            }
            if(db>=0) {
                if(nn>=SH_AUG_MAX_CORNERS+4)return -1;
                next[nn][0]=poly[j][0];next[nn++][1]=poly[j][1];
            }
        }
        n=nn;memcpy(poly,next,(size_t)n*sizeof poly[0]);
    }
    if(!n)return 0;
    *lo=1;*hi=0;
    for(i=0;i<n;i++){if(poly[i][0]<*lo)*lo=poly[i][0];if(poly[i][0]>*hi)*hi=poly[i][0];}
    return *hi-*lo>1e-9;
}

/* Every peer receives its complete interval. Selecting only the nearest peer
 * would hide stacked landings. Endpoint and swept-body checks decide which
 * links can actually be written. Capacity exhaustion rejects the candidate. */
static int aug_generated_segments(aug_ctx *c,int ai,const aug_quad *eff,
                                  const aug_quad *req,const aug_peer *peers,
                                  int npeers,aug_side *out,int cap,int gaps)
{
    const aug_peer *cand[SH_AUG_MAX_PLATFORMS];
    /* Standing clearance uses the native square XY body, not a circle.
     * Against a yawed edge its support radius is r*(|nx|+|ny|), up to
     * sqrt(2)*r. Two touching surfaces can therefore have their standing
     * regions farther apart than 2*r without any physical gap. */
    double reach=gaps?SH_TRAV_LEAP_MAX_SPAN:2.0*sqrt(2.0)*c->radius+AUG_TOUCH_EPS*2.0;
    int ncand=aug_candidates(req,peers,npeers,ai,reach,cand,SH_AUG_MAX_PLATFORMS);
    int e,q,n=0;
    for(e=0;e<eff->count;e++) {
        double in[2],dir[2];aug_edge_normal_in(eff,e,in);dir[0]=-in[0];dir[1]=-in[1];
        for(q=0;q<ncand;q++) {
            double lo,hi;aug_side side;
            int found=aug_peer_interval(req,e,dir,cand[q]->eff,reach,&lo,&hi);
            if(found<0){c->failed=1;return n;}
            if(!found)continue;
            aug_close_segment(c,&side,eff,e,dir,lo,hi,cand[q]->area,peers,npeers,req);
            if((side.gap>AUG_TOUCH_EPS)!=gaps)continue;
            if(n==cap){c->failed=1;return n;}
            out[n++]=side;
        }
    }
    return n;
}

/* Follow a complete sloped edge through the module BSP. Every split yields
 * exact parameter intervals, including floor leaves narrower than any fixed
 * sampling grid. Zero-width intervals are not navigable connections. */
static void aug_floor_intervals(aug_ctx *c,int node,unsigned depth,
                                const double origin[3],const double delta[3],
                                double lo,double hi,const aug_quad *eff,
                                const aug_quad *req,int edge,const double dir[2],
                                const aug_peer *peers,int npeers,
                                aug_side *out,int cap,int *count,unsigned *visits)
{
    if(c->failed||hi-lo<1e-9)return;
    if(depth>SH_AAS_MAX_DEPTH||++*visits>131072){c->failed=1;return;}
    if(node<=0) {
        int area=-node,have_floor;float bounds[6];double z0,dz,cuts[4];int nc=2,k,j;
        aug_quad floor;
        if(area<=0||(unsigned)area>=c->original_areas||
           !aug_area_box(c->a,(unsigned)area,bounds))return;
        have_floor=aug_original_floor(c->a,(unsigned)area,&floor);
        z0=origin[2]-(have_floor?aug_z_at(&floor,origin[0],origin[1]):bounds[5]);
        dz=delta[2]-(have_floor?aug_z_at(&floor,origin[0]+delta[0],origin[1]+delta[1])-
                                 aug_z_at(&floor,origin[0],origin[1]):0);
        cuts[0]=lo;cuts[1]=hi;
        if(fabs(dz)>1e-10)for(k=-1;k<=1;k+=2) {
            double t=(k*c->step-z0)/dz;
            if(t>lo&&t<hi)cuts[nc++]=t;
        }
        for(k=1;k<nc;k++)for(j=k;j>0&&cuts[j]<cuts[j-1];j--){double t=cuts[j];cuts[j]=cuts[j-1];cuts[j-1]=t;}
        for(k=1;k<nc;k++) {
            if(cuts[k]-cuts[k-1]<1e-9)continue;
            if(*count==cap){c->failed=1;return;}
            aug_close_segment(c,&out[(*count)++],eff,edge,dir,cuts[k-1],cuts[k],area,peers,npeers,req);
        }
    } else {
        const unsigned char *nd=sh_aas_rec_const(c->a,SH_AAS_L_NODES,(unsigned)node);
        const unsigned char *plane;double a,b,dl,dh;int k;
        if(!nd){c->failed=1;return;}
        plane=sh_aas_rec_const(c->a,SH_AAS_L_PLANES,(unsigned)sh_aas_get_i32(nd,ND_PLANE));
        if(!plane){c->failed=1;return;}
        a=sh_aas_get_f32(plane,PL_DIST);b=0;
        for(k=0;k<3;k++){double v=sh_aas_get_f32(plane,PL_A+4*k);a+=v*origin[k];b+=v*delta[k];}
        dl=a+b*lo;dh=a+b*hi;
        if((dl>0)==(dh>0))
            aug_floor_intervals(c,sh_aas_get_i32(nd,dl>0?ND_CHILD0:ND_CHILD1),depth+1,
                origin,delta,lo,hi,eff,req,edge,dir,peers,npeers,out,cap,count,visits);
        else {
            double t=-a/b;
            aug_floor_intervals(c,sh_aas_get_i32(nd,dl>0?ND_CHILD0:ND_CHILD1),depth+1,
                origin,delta,lo,t,eff,req,edge,dir,peers,npeers,out,cap,count,visits);
            aug_floor_intervals(c,sh_aas_get_i32(nd,dh>0?ND_CHILD0:ND_CHILD1),depth+1,
                origin,delta,t,hi,eff,req,edge,dir,peers,npeers,out,cap,count,visits);
        }
    }
}

static int aug_edge_segments(aug_ctx *c,int ai,const aug_quad *eff,
                             const aug_quad *req,const aug_peer *peers,
                             int npeers,aug_side *out,int cap)
{
    int n=aug_generated_segments(c,ai,eff,req,peers,npeers,out,cap,0),e;
    for(e=0;e<eff->count&&!c->failed;e++) {
        double in[2],dir[2],origin[3],delta[3];int j=(e+1)%eff->count;unsigned visits=0;
        aug_edge_normal_in(eff,e,in);dir[0]=-in[0];dir[1]=-in[1];
        origin[0]=req->c[e][0]+dir[0]*REACH_SIDE_OFFSET;
        origin[1]=req->c[e][1]+dir[1]*REACH_SIDE_OFFSET;
        origin[2]=aug_z_at(eff,req->c[e][0],req->c[e][1]);
        delta[0]=req->c[j][0]-req->c[e][0];delta[1]=req->c[j][1]-req->c[e][1];
        delta[2]=aug_z_at(eff,req->c[j][0],req->c[j][1])-origin[2];
        aug_floor_intervals(c,aug_tree_root(c->a),0,origin,delta,0,1,eff,req,e,dir,
            peers,npeers,out,cap,&n,&visits);
    }
    return n;
}

static int aug_edge_gap_segments(aug_ctx *c,int ai,const aug_quad *eff,
                                 const aug_quad *req,const aug_peer *peers,
                                 int npeers,aug_side *out,int cap)
{
    return aug_generated_segments(c,ai,eff,req,peers,npeers,out,cap,1);
}

/* Classify gaps first, then absolute height difference and rise/fall
 * direction.
 */
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
    for (i = 0; i < inner->count; i++)
        if (!aug_quad_contains_xy(outer, inner->c[i][0], inner->c[i][1])) return 0;
    return 1;
}

/* Emit each generated pair once. Strict containment gives ownership to the
 * inner footprint because only it may discover the contact. Otherwise the
 * lower area index owns the pair.
 */
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

/* Emit reciprocal plain walks within maxStepHeight. This handles platforms
 * carved inside an existing slab, where aug_walk_links cannot find a shared
 * boundary.
 */
static int aug_step_links(aug_ctx *c, int ai, const aug_quad *eff,
                          const aug_quad *req, const aug_peer *peers, int npeers)
{
    aug_side sides[512];
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
            /* Place both endpoints inside their own navigable footprints. */
            dn[0] = sides[i].p0[0] + t * (sides[i].p1[0] - sides[i].p0[0])
                  - sides[i].out[0] * REACH_SIDE_OFFSET;
            dn[1] = sides[i].p0[1] + t * (sides[i].p1[1] - sides[i].p0[1])
                  - sides[i].out[1] * REACH_SIDE_OFFSET;
            dn[2] = aug_z_at(eff, dn[0], dn[1]);
            if (sides[i].generated && sides[i].peer >= 0) {
                /* Cast into the peer's effective footprint; the physical wall
                 * alone does not establish a valid standing point.
                 */
                const aug_quad *pe = peers[sides[i].peer].eff;
                double distance, land[2];
                if(!aug_ray_entry(pe,dn[0],dn[1],sides[i].out[0],sides[i].out[1],
                    SH_TRAV_LEAP_MAX_SPAN+256.0,&distance,land))continue;
                /* Native walking clips each floor polygon and accepts only
                 * 0.2 units of horizontal separation at the transition. A
                 * reachability cannot jump a clearance gap or skip a cell. */
                if(peers[sides[i].peer].prepared&&distance-REACH_SIDE_OFFSET>0.2)continue;
                up[0] = land[0];
                up[1] = land[1];
                up[2] = aug_z_at(pe, up[0], up[1]);
            } else {
                up[0] = sides[i].p0[0] + t * (sides[i].p1[0] - sides[i].p0[0])
                      + sides[i].out[0] * REACH_SIDE_OFFSET;
                up[1] = sides[i].p0[1] + t * (sides[i].p1[1] - sides[i].p0[1])
                      + sides[i].out[1] * REACH_SIDE_OFFSET;
                aug_quad floor;
                up[2] = aug_original_floor(c->a,(unsigned)sides[i].floor_area,&floor)?
                    aug_z_at(&floor,up[0],up[1]):sides[i].far_z;
            }
            if(fabs(up[2]-dn[2])>(double)c->step)continue;
            /* Both endpoints must resolve through the BSP to their declared areas. */
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

/* Emit downward walk-off links only under the selected fall policy. AUTO
 * follows maxFallHeight, zero in shipped monster modules.
 */
static int aug_fall_links(aug_ctx *c, int ai, const aug_quad *eff,
                          const aug_quad *req, const aug_peer *peers, int npeers)
{
    aug_side sides[512];
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

typedef struct aug_trav_buffer {
    aug_trav_spec *data;
    int count, capacity;
} aug_trav_buffer;

/* Candidate storage is independent of the native per-area routing budget.
 * A geometry edit can add many valid contact intervals at once. Grow before
 * writing rather than rejecting the complete bake at a fixed sample count. */
static aug_trav_spec *aug_trav_append(aug_ctx *c, aug_trav_buffer *buffer)
{
    if (buffer->count == buffer->capacity) {
        int capacity;
        aug_trav_spec *data;
        if (buffer->capacity > INT_MAX / 2) goto failed;
        capacity = buffer->capacity ? buffer->capacity * 2 : 256;
        if ((size_t)capacity > (size_t)-1 / sizeof *data) goto failed;
        data = (aug_trav_spec *)realloc(buffer->data, (size_t)capacity * sizeof *data);
        if (!data) goto failed;
        buffer->data = data;
        buffer->capacity = capacity;
    }
    return &buffer->data[buffer->count++];
failed:
    c->rep->links_truncated = 1;
    c->failed = 1;
    return NULL;
}

/* Try midpoint anchors first, then spread along the edge. Per-demon animation
 * offsets need different amounts of clear floor, so one anchor can reject an
 * otherwise usable route.
 */
#ifdef SH_AUG_TESTING
/* Test-only anchor cap; 0 leaves sampling uncapped. */
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

/* Test the entire straight traversal segment against the original oriented
 * solids expanded by the agent bounds. Endpoint support is excluded because
 * climbing intentionally enters/leaves those supports. Animation-specific
 * swept poses are not represented by AAS reachability endpoints. */
static int aug_path_is_clear(aug_ctx *c, const aug_peer *peers, int npeers,
                             int from_area, int to_area,
                             const double s3[3], const double e3[3])
{
    int q, k, has_solid=0, from=-1, to=-1;
    for(q=0;q<c->solid_count;q++)if(c->solids[q].depth>0.0f)has_solid=1;
    if(has_solid) {
        for(q=0;q<npeers;q++) {
            if(peers[q].area==from_area)from=peers[q].source;
            if(peers[q].area==to_area)to=peers[q].source;
        }
        return sh_nav_geometry_path_clear(c->solids,c->solid_count,from,to,
            s3,e3,c->radius,c->height);
    }
    /* Legacy surface-only callers have no lower solid boundary. */
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

/* Prepared edges bound standing space and sit inside the physical ledge.
 * Animation offsets start at the physical ledge. Floor endpoints must also
 * clear the complete oriented solid, whose lower face can overhang that edge. */
static int aug_floor_traversal_point(aug_ctx *c,const aug_peer *owner,
    const aug_side *side,double x,double y,double offset,double point[3])
{
    aug_quad floor;int have_floor=aug_original_floor(c->a,(unsigned)side->floor_area,&floor);
    double distance=offset;
    if(owner&&owner->prepared&&owner->source>=0&&owner->source<c->solid_count) {
        double lip=1e9,body_exit,origin[3],direction[3];int e;
        for(e=0;e<owner->support.count;e++) {
            double normal[2],speed,inside;
            aug_edge_normal_in(&owner->support,e,normal);
            speed=normal[0]*side->out[0]+normal[1]*side->out[1];
            inside=normal[0]*(x-owner->support.c[e][0])+normal[1]*(y-owner->support.c[e][1]);
            if(speed < -1e-9 && -inside/speed<lip)lip=-inside/speed;
        }
        if(lip>=1e9)return 0;
        if(lip>0)distance+=lip;
        origin[0]=x;origin[1]=y;
        origin[2]=have_floor?aug_z_at(&floor,x,y):side->far_z;
        direction[0]=side->out[0];direction[1]=side->out[1];
        direction[2]=have_floor?aug_z_at(&floor,x+direction[0],y+direction[1])-origin[2]:0;
        if(!sh_nav_geometry_ray_exit(&c->solids[owner->source],origin,direction,
                                     c->radius,c->height,&body_exit))return 0;
        /* Cover the later integer-coordinate rounding without changing the
         * standing surface or its boundary. */
        if(body_exit>0&&distance<body_exit+2.0)distance=body_exit+2.0;
    }
    point[0]=x+side->out[0]*distance;
    point[1]=y+side->out[1]*distance;
    point[2]=have_floor?aug_z_at(&floor,point[0],point[1]):side->far_z;
    return 1;
}

/* Collect per-demon climbs and leaps for usable edge segments in both
 * directions. Refuse endpoints that resolve outside their claimed BSP areas.
 */
static int aug_traversal_specs(aug_ctx *c, int ai, const aug_quad *eff,
                               const aug_quad *req, const aug_peer *peers,
                               int npeers, aug_trav_buffer *buffer)
{
    aug_side sides[512];
    int n, i, k, d, count = 0;
    const aug_peer *owner=NULL;

    if (c->o->traversal == SH_AUG_TRAVERSAL_NEVER) return 0;
    if (!sh_trav_ready()) return 0;
    for(i=0;i<npeers;i++)if(peers[i].area==ai){owner=&peers[i];break;}

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
        /* Limit leap slope independently from horizontal span. */
        if (leap && span > 0.0 &&
            fabs(sides[i].drop) / span > SH_TRAV_LEAP_MAX_GRADE) continue;

        seglen = sqrt((sides[i].p1[0] - sides[i].p0[0]) * (sides[i].p1[0] - sides[i].p0[0]) +
                      (sides[i].p1[1] - sides[i].p0[1]) * (sides[i].p1[1] - sides[i].p0[1]));
        (void)seglen;
        /* Sample the contact segment so partial neighbours remain reachable. */
        na = aug_trav_anchors(0.0, 1.0, anchors,
                              (int)(sizeof anchors / sizeof anchors[0]));
#ifdef SH_AUG_TESTING
        if (g_test_anchor_cap > 0 && na > g_test_anchor_cap) na = g_test_anchor_cap;
#endif

        for (d = 0; d < 2; d++) {                          /* UP then DOWN */
            int up = (d == SH_TRAV_UP);
            for (k = 0; k < sh_trav_monster_count(); k++) {
                const sh_trav_monster *m = sh_trav_monster_at(k);
                char path[SH_TRAV_PATH_CAP];
                float off = 0.0f;
                int dist = 0, time = 0, aidx, placed = 0, dirn, inward;
                double inner_pt[3], outer_pt[3], dirs;
                aug_trav_spec *sp;

                if (!m) continue;
                dirn = leap ? SH_TRAV_ACROSS : d;
                if (sides[i].generated && !sh_trav_select(m, dirn, (float)span, path, sizeof path,
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
                        /* Land inside the peer's effective footprint at the
                         * measured ray distance. Do not add the animation
                         * offset again: that would change the span after clip
                         * selection.
                         */
                        const aug_quad *pe = peers[sides[i].peer].eff;
                        double distance,land[2];
                        /* A touching contact follows this anchor's ray. Leaps
                         * keep the landing used to select their span above. */
                        if(leap)memcpy(land,sides[i].land,sizeof land);
                        else if(!aug_ray_entry(pe,inner_pt[0],inner_pt[1],
                                sides[i].out[0],sides[i].out[1],
                                SH_TRAV_LEAP_MAX_SPAN+256.0,&distance,land))continue;
                        outer_pt[0] = land[0];
                        outer_pt[1] = land[1];
                        outer_pt[2] = aug_z_at(pe, outer_pt[0], outer_pt[1]);
                        if(!leap) {
                            double actual_span=fabs(inner_pt[2]-outer_pt[2]);
                            if(actual_span<=c->step||!sh_trav_select(m,dirn,(float)actual_span,
                                path,sizeof path,&off,&dist,&time))continue;
                        }
                    } else {
                        int attempt,selected=0;
                        /* Select at this anchor's elevation, then recheck if
                         * moving along a sloped native floor changes the clip.
                         * A nonconvergent choice is not emitted. */
                        off=0.0f;
                        for(attempt=0;attempt<=SH_TRAV_DISTANCES;attempt++) {
                            float next_offset;double actual_span;
                            if(!aug_floor_traversal_point(c,owner,&sides[i],bx,by,fabs(off),outer_pt))break;
                            actual_span=fabs(inner_pt[2]-outer_pt[2]);
                            if(actual_span<=c->step||!sh_trav_select(m,dirn,(float)actual_span,
                                path,sizeof path,&next_offset,&dist,&time))break;
                            next_offset=(float)fabs(next_offset);
                            if(next_offset==off){selected=1;break;}
                            off=next_offset;
                        }
                        if(!selected)continue;
                    }
                    if (sh_aas_point_area(c->a, (float)inner_pt[0], (float)inner_pt[1],
                                          (float)(inner_pt[2] + 2.0)) != ai) continue;
                    if (sh_aas_point_area(c->a, (float)outer_pt[0], (float)outer_pt[1],
                                          (float)(outer_pt[2] + 2.0)) != sides[i].floor_area)
                        continue;
                    if (!aug_path_is_clear(c, peers, npeers, ai, sides[i].floor_area,
                                           inner_pt, outer_pt)) continue;
                    {
                        double stored[3];
                        if(!aug_reach_point(c->a,ai,inner_pt,stored)||
                           !aug_reach_point(c->a,sides[i].floor_area,outer_pt,stored))continue;
                        if(!sides[i].generated&&
                           !sh_nav_geometry_path_clear(c->solids,c->solid_count,-1,-1,
                               stored,stored,c->radius,c->height))continue;
                    }
                    placed = 1;
                    break;
                }
                if (!placed) continue;      /* nowhere on this segment works for it */

                sp = aug_trav_append(c, buffer);
                if (!sp) return count;
                count++;
                memset(sp, 0, sizeof *sp);
                sp->travel_flags = m->travel_flags;
                sp->d30 = m->d30;
                sp->travel_time = time;
                /* Pair ownership follows geometry/index order, not elevation.
                 * A lower bridge can own its higher support: select the actual
                 * uphill/downhill direction independently of which is 'ours'. */
                inward=leap?up:(up==(inner_pt[2]>outer_pt[2]));
                sp->from_area = inward ? sides[i].floor_area : ai;
                sp->to_area   = inward ? ai : sides[i].floor_area;
                memcpy(sp->start, inward ? outer_pt : inner_pt, sizeof sp->start);
                memcpy(sp->end,   inward ? inner_pt : outer_pt, sizeof sp->end);
                /* Facing follows XY travel: inward arriving, outward
                 * departing. Store it in the donor fixed-point scale;
                 * diagonal vectors are valid.
                 */
                dirs = inward ? -1.0 : 1.0;
                sp->dir[0] = sides[i].out[0] * dirs;
                sp->dir[1] = sides[i].out[1] * dirs;
                sp->is_leap = leap;
                _snprintf_s(sp->anim, sizeof sp->anim, _TRUNCATE, "%s", path);
            }
        }
    }
    return count;
}

/* Keep every route before spending the remaining native slots on alternative
 * anchors. A route includes the demon flags, direction, animation and traversal
 * metadata; dropping a later demon just because earlier ones filled the area
 * would turn a capacity fix into a class-specific navigation failure.
 * Shipped links are never removed. Generated walk/fall samples share the same
 * budget as traversals, so a long floor edge cannot crowd out every climb. */
static int aug_budget_traversals(aug_ctx *c, aug_trav_spec *specs, int n,
                                 sh_aug_report *out)
{
    unsigned na = sh_aas_count(c->a, SH_AAS_L_AREAS), i;
    unsigned nr = sh_aas_count(c->a, SH_AAS_L_REACHABILITIES);
    unsigned original = out->reach_before, nb = nr - original;
    aug_trav_spec *choices = (aug_trav_spec *)calloc(nb + n + 1, sizeof *choices);
    unsigned *degree = (unsigned *)calloc(na, sizeof *degree);
    int *rank = (int *)calloc(nb + n + 1, sizeof *rank);
    unsigned char *keep = (unsigned char *)calloc(nb + n + 1, 1);
    unsigned slots = 1, *last = NULL;
    int k, round, max_rank = 0, written = 0, full = -1;
    while (slots < 2 * (nb + (unsigned)n + 1)) slots <<= 1;
    last = (unsigned *)calloc(slots, sizeof *last);
    if (!choices || !degree || !rank || !keep || !last) { c->failed = 1; goto done; }
    for (i = 0; i < nb; i++) {
        const unsigned char *r = sh_aas_rec_const(c->a, SH_AAS_L_REACHABILITIES, original+i);
        choices[i].from_area = sh_aas_get_u16(r, RE_FROM_AREA);
        choices[i].to_area = sh_aas_get_u16(r, RE_TO_AREA);
        choices[i].travel_flags = sh_aas_get_u32(r, RE_TRAVEL_FLAGS);
    }
    /* Reject unusable integer endpoints before choosing route representatives.
     * Otherwise one invalid anchor stops emission of every later traversal, or
     * consumes the only slot reserved for another usable anchor of that route. */
    for (k = 0; k < n; k++) {
        double start[3], end[3];
        if (aug_reach_point(c->a, specs[k].from_area, specs[k].start, start) &&
            aug_reach_point(c->a, specs[k].to_area, specs[k].end, end))
            choices[nb + written++] = specs[k];
    }
    n = (int)nb + written;
    written = 0;
    for (i = 0; i < original; i++) {
        const unsigned char *r = sh_aas_rec_const(c->a, SH_AAS_L_REACHABILITIES, i);
        unsigned from = sh_aas_get_u16(r, RE_FROM_AREA);
        if (from >= na || ++degree[from] > SH_AAS_MAX_AREA_REACHABILITIES) {
            full = (int)from;
            goto overflow;
        }
    }
    for (k = 0; k < n; k++) {
        const aug_trav_spec *s = &choices[k];
        const unsigned char *name = (const unsigned char *)s->anim;
        unsigned hash = 2166136261u, bucket;
        if (choices[k].from_area < 0 || (unsigned)choices[k].from_area >= na) {
            full = choices[k].from_area;
            goto overflow;
        }
        hash = (hash ^ (unsigned)s->from_area) * 16777619u;
        hash = (hash ^ (unsigned)s->to_area) * 16777619u;
        hash = (hash ^ s->travel_flags) * 16777619u;
        hash = (hash ^ s->d30) * 16777619u;
        while (*name) hash = (hash ^ *name++) * 16777619u;
        bucket = hash & (slots - 1);
        while (last[bucket]) {
            unsigned j = last[bucket] - 1;
            if (choices[j].from_area == s->from_area &&
                choices[j].to_area == s->to_area &&
                choices[j].travel_flags == s->travel_flags &&
                choices[j].d30 == s->d30 && !strcmp(choices[j].anim, s->anim)) {
                rank[k] = rank[j] + 1;
                break;
            }
            bucket = (bucket + 1) & (slots - 1);
        }
        last[bucket] = (unsigned)k + 1;
        if (rank[k] > max_rank) max_rank = rank[k];
        if (!rank[k]) {
            if (++degree[choices[k].from_area] > SH_AAS_MAX_AREA_REACHABILITIES) {
                full = choices[k].from_area;
                goto overflow;
            }
            keep[k] = 1;
        }
    }
    /* Round-robin across routes, retaining input order when no reduction is
     * needed. The collector tries midpoint anchors first. */
    for (round = 1; round <= max_rank && round < SH_AAS_MAX_AREA_REACHABILITIES; round++) for (k = 0; k < n; k++) {
        unsigned from = (unsigned)choices[k].from_area;
        if (rank[k] == round && degree[from] < SH_AAS_MAX_AREA_REACHABILITIES) {
            degree[from]++;
            keep[k] = 1;
        }
    }
    /* Only generated basic links move. Shipped traversalPoint indices still
     * refer to the unchanged prefix; new traversal points are emitted later.
     * aug_relink rebuilds every list after this compaction. */
    nr = original;
    for (i = 0; i < nb; i++) if (keep[i]) {
        if (nr != original+i)
            memcpy(sh_aas_rec(c->a, SH_AAS_L_REACHABILITIES, nr),
                   sh_aas_rec_const(c->a, SH_AAS_L_REACHABILITIES, original+i), 40);
        nr++;
    }
    if (!sh_aas_truncate(c->a, SH_AAS_L_REACHABILITIES, nr)) { c->failed = 1; goto done; }
    for (k = (int)nb; k < n; k++) if (keep[k]) specs[written++] = choices[k];
    out->anchors_reduced = n - written - (int)(nr - original);
    goto done;
overflow:
    out->reach_limit_exceeded = 1;
    out->reach_limit_area = full;
    /* Only routes leaving this area consume its budget. Incoming-only routes
     * cannot identify a cause, and un-emitted reports have no area to match. */
    for (k = 0; k < n && out->blamed_count < SH_AUG_MAX_BLAMED; k++) {
        int j, side;
        if (choices[k].from_area != full) continue;
        side = choices[k].to_area;
        for (j = 0; j < out->platform_count; j++) {
            int src = out->platforms[j].source, seen, b;
            if (!out->platforms[j].emitted) continue;
            if (out->platforms[j].area != side && out->platforms[j].area != full)
                continue;
            for (seen = 0, b = 0; b < out->blamed_count; b++)
                if (out->blamed[b] == src) seen = 1;
            if (seen || out->blamed_count >= SH_AUG_MAX_BLAMED) continue;
            out->blamed[out->blamed_count++] = src;
        }
    }
    c->failed = 1;
done:
    free(choices); free(keep); free(rank); free(degree); free(last);
    return c->failed ? 0 : written;
}

/* Append traversal reachabilities as a contiguous tail and pair them with
 * traversalPoints and animation names. Record 0 is a dummy; real points must
 * be grouped by from_area. Returns count written.
 */
static int aug_emit_traversals(aug_ctx *c, aug_trav_spec *specs, int n)
{
    unsigned base, first, i;
    int k, j, written = 0;
    unsigned na;

    if (n <= 0) return 0;

    /* Existing traversal points remain valid when stably regrouped by
     * from_area. Rebuild each area's range; traversalPoint.d28 still refers
     * to an unmoved reachability.
     */

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

    /* Stably group records 1..np-1 by from_area. Keep dummy record 0 and the
     * relative order of shipped points.
     */
    {
        unsigned np = sh_aas_count(c->a, SH_AAS_L_TRAVERSALPOINTS);
        size_t rs = sh_aas_record_size(SH_AAS_L_TRAVERSALPOINTS);
        unsigned char hold[64];
        unsigned m, q;
        if (rs <= sizeof hold) {
            for (m = 2; m < np; m++) {
                unsigned char *cur = sh_aas_rec(c->a, SH_AAS_L_TRAVERSALPOINTS, m);
                unsigned owner;
                if (!cur) continue;
                owner = sh_aas_get_u16(cur, TP_W34);
                memcpy(hold, cur, rs);
                q = m;
                while (q > 1) {
                    unsigned char *prev = sh_aas_rec(c->a, SH_AAS_L_TRAVERSALPOINTS, q - 1);
                    if (!prev || sh_aas_get_u16(prev, TP_W34) <= owner) break;
                    memcpy(sh_aas_rec(c->a, SH_AAS_L_TRAVERSALPOINTS, q), prev, rs);
                    q--;
                }
                if (q != m)
                    memcpy(sh_aas_rec(c->a, SH_AAS_L_TRAVERSALPOINTS, q), hold, rs);
            }
        }
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
typedef struct aug_seam { double lo,hi; } aug_seam;

static int aug_seam_order(const void *a,const void *b)
{
    double d=((const aug_seam*)a)->lo-((const aug_seam*)b)->lo;
    return d<0?-1:d>0?1:0;
}

/* Floor contact is not necessarily a whole, identical 3D edge: a short deck
 * can meet the middle of a long edge, and a step has two different elevations.
 * Split those contacts before classifying walls. Native wall queries inspect
 * edge flags, independently of the routing graph. Keep exposed remainders. */
static int aug_floor_contact(const aug_quad *floor,const double a[3],const double b[3],
                             double ox,double oy,double step,double *lo,double *hi)
{
    double dx=b[0]-a[0],dy=b[1]-a[1],z0,z1;int e;
    *lo=0;*hi=1;
    for(e=0;e<floor->count;e++) {
        double in[2],at,slope;
        aug_edge_normal_in(floor,e,in);
        at=in[0]*(a[0]+ox*0.02-floor->c[e][0])+in[1]*(a[1]+oy*0.02-floor->c[e][1]);
        slope=in[0]*dx+in[1]*dy;
        if(fabs(slope)<1e-9){if(at<0)return 0;}
        else if(slope>0){double t=-at/slope;if(t>*lo)*lo=t;}
        else {double t=-at/slope;if(t<*hi)*hi=t;}
    }
    z0=a[2]-aug_z_at(floor,a[0],a[1]);
    z1=b[2]-aug_z_at(floor,b[0],b[1])-z0;
    if(fabs(z1)<1e-9){if(fabs(z0)>step+1e-5)return 0;}
    else {
        double l=(-step-z0)/z1,h=(step-z0)/z1;
        if(l>h){double t=l;l=h;h=t;}
        if(l>*lo)*lo=l;if(h<*hi)*hi=h;
    }
    return *hi>*lo;
}

static int aug_stitch_edges(aug_ctx *c,const aug_quad *floors,const int *areas,int n)
{
    aug_seam *spans=NULL;
    int i,j,e,k,ok=0,cap=n*SH_AUG_MAX_CORNERS+(int)c->original_areas;
    if(!n)return 1;
    spans=(aug_seam*)malloc((size_t)cap*sizeof *spans);
    if(!spans)goto done;
    for(i=0;i<n;i++) {
        unsigned first=sh_aas_count(c->a,SH_AAS_L_EDGEINDEX),count=0;
        for(e=0;e<floors[i].count;e++) {
            const double *a=floors[i].c[e],*b=floors[i].c[(e+1)%floors[i].count];
            double dx=b[0]-a[0],dy=b[1]-a[1],len=hypot(dx,dy),prev=0;
            int ns=0,t;
            if(len<1e-8)goto done;
            for(j=0;j<n;j++)if(j!=i)
                for(k=0;k<floors[j].count;k++) {
                    const double *v=floors[j].c[k],*w=floors[j].c[(k+1)%floors[j].count];
                    double lo,hi,z0,z1,slope;
                    if(dx*(w[0]-v[0])+dy*(w[1]-v[1])>=0)continue;
                    if(fabs(dx*(v[1]-a[1])-dy*(v[0]-a[0]))>0.02*len||
                       fabs(dx*(w[1]-a[1])-dy*(w[0]-a[0]))>0.02*len)continue;
                    lo=((w[0]-a[0])*dx+(w[1]-a[1])*dy)/(len*len);
                    hi=((v[0]-a[0])*dx+(v[1]-a[1])*dy)/(len*len);
                    if(lo<0)lo=0;if(hi>1)hi=1;if(hi<=lo)continue;
                    z0=a[2]-aug_z_at(&floors[j],a[0],a[1]);
                    z1=b[2]-aug_z_at(&floors[j],b[0],b[1]);slope=z1-z0;
                    if(fabs(slope)<1e-9){if(fabs(z0)>c->step+1e-5)continue;}
                    else {
                        double l=(-c->step-z0)/slope,h=(c->step-z0)/slope;
                        if(l>h){double tmp=l;l=h;h=tmp;}
                        if(l>lo)lo=l;if(h<hi)hi=h;
                    }
                    /* A sub-float fragment can tilt its half-plane severely
                     * after serialization. Snap cuts within the seam tolerance
                     * to existing endpoints instead of creating tiny edges. */
                    if(lo*len<0.02)lo=0;if((1-hi)*len<0.02)hi=1;
                    if((hi-lo)*len<0.02)continue;
                    if(ns==cap)goto done;
                    spans[ns].lo=lo;spans[ns++].hi=hi;
                }
            for(j=1;(unsigned)j<c->original_areas;j++) {
                aug_quad floor;double lo,hi;
                if(!aug_original_floor(c->a,(unsigned)j,&floor)||floor.n[2]<c->min_floor_cos||
                   !aug_floor_contact(&floor,a,b,-dy/len,dx/len,c->step,&lo,&hi))continue;
                if(lo*len<0.02)lo=0;if((1-hi)*len<0.02)hi=1;
                if((hi-lo)*len<0.02)continue;
                if(ns==cap)goto done;
                spans[ns].lo=lo;spans[ns++].hi=hi;
            }
            qsort(spans,(size_t)ns,sizeof *spans,aug_seam_order);
            /* Merge intervals, then alternate exposed and shared portions. */
            for(k=0,t=0;k<ns;k++) {
                if(t&&(spans[k].lo-spans[t-1].hi)*len<=0.02) {
                    if(spans[k].hi>spans[t-1].hi)spans[t-1].hi=spans[k].hi;
                } else spans[t++]=spans[k];
            }
            ns=t;
            for(k=0;k<=2*ns;k++) {
                double end=k==2*ns?1:(k&1)?spans[k/2].hi:spans[k/2].lo;
                int shared=k&1,v0,v1;unsigned edge,ix;unsigned char *rec;
                if((end-prev)*len<0.00001){prev=end;continue;}
                v0=aug_vertex(c,(float)(a[0]+prev*dx),(float)(a[1]+prev*dy),(float)(a[2]+prev*(b[2]-a[2])));
                v1=aug_vertex(c,(float)(a[0]+end*dx),(float)(a[1]+end*dy),(float)(a[2]+end*(b[2]-a[2])));
                if(v0<0||v1<0)goto done;
                /* Do not pass through aug_edge: reusing this area's old edge
                 * is not evidence of sharing it with a second area. */
                if(!sh_aas_append(c->a,SH_AAS_L_EDGES,1,&edge))goto done;
                rec=sh_aas_rec(c->a,SH_AAS_L_EDGES,edge);
                sh_aas_put_u32(rec,0,(unsigned)v0);sh_aas_put_u32(rec,4,(unsigned)v1);
                sh_aas_put_u32(rec,8,shared?EDGE_FLAGS_SHARED:EDGE_FLAGS_BOUNDARY);
                if(!sh_aas_append(c->a,SH_AAS_L_EDGEINDEX,1,&ix))goto done;
                sh_aas_put_i32(sh_aas_rec(c->a,SH_AAS_L_EDGEINDEX,ix),0,edge);
                count++;prev=end;
            }
        }
        if(count>32767)goto done;
        {
            unsigned char *ar=sh_aas_rec(c->a,SH_AAS_L_AREAS,(unsigned)areas[i]);
            sh_aas_put_u16(ar,AR_NUM_EDGES,(uint16_t)count);
            sh_aas_put_u32(ar,AR_FIRST_EDGE_INDEX,first);
        }
    }
    ok=1;
done:
    free(spans);return ok;
}

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

/* Compute XY bounds and centroid height for ordering and legacy clearance
 * checks.
 */
static void aug_plat_bounds(const sh_aug_platform *p, double b[4])
{
    int i;
    b[0] = b[2] = p->c[0][0];
    b[1] = b[3] = p->c[0][1];
    for (i = 1; i < (p->corners ? p->corners : 4); i++) {
        if (p->c[i][0] < b[0]) b[0] = p->c[i][0];
        if (p->c[i][1] < b[1]) b[1] = p->c[i][1];
        if (p->c[i][0] > b[2]) b[2] = p->c[i][0];
        if (p->c[i][1] > b[3]) b[3] = p->c[i][1];
    }
}

static double aug_plat_centroid_z(const sh_aug_platform *p)
{
    int i, count=p->corners?p->corners:4; double z=0.0;
    for(i=0;i<count;i++)z+=p->c[i][2];
    return z/count;
}

static void aug_centre(const aug_quad *p, double *x, double *y)
{
    int i; *x=*y=0.0;
    for(i=0;i<p->count;i++){*x+=p->c[i][0];*y+=p->c[i][1];}
    *x/=p->count;*y/=p->count;
}

/* Legacy clearance check against module areas and all requested platforms,
 * including refused solids. Exact footprint overlap avoids false obstruction
 * from rotated AABBs.
 */
static double aug_headroom(aug_ctx *c, const aug_quad *p,
                           const sh_aug_platform *all, int n, int self)
{
    double best = 1e30;
    /* Use centroid height for this legacy clearance comparison. */
    double pz = aug_quad_max_z(p);
    unsigned i, na = c->original_areas;
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
    /* Allocate scratch on the heap to keep large geometry arrays off the
     * loader stack.
     */
    int *order = NULL, *made = NULL, *wsrc = NULL, *wpieces = NULL;
    aug_quad *effs = NULL, *reqs = NULL;
    aug_peer *peers = NULL;
    unsigned char *wburied = NULL;
    sh_aug_platform *work = NULL;
    unsigned char *block = NULL;
    float mins[3], maxs[3], fw, fd;
    int i, j, k, nmade = 0, rc;

    if (!a || !out) return 0;
    memset(out, 0, sizeof *out);
    out->reach_limit_area = -1;
    if (!opts) opts = &defaults;
    if (n < 0) n = 0;
    if (n > SH_AUG_MAX_PLATFORMS || (n>0&&!plats)) return 0;

    c.a = a; c.o = opts; c.rep = out; c.failed = 0;c.prepared_geometry=0;
    c.solids=plats;c.solid_count=n;c.original_areas=sh_aas_count(a,SH_AAS_L_AREAS);
    c.radius = opts->inset ? aug_agent_radius(a) : 0.0f;
    c.height = aug_agent_height(a);
    c.step = sh_aas_setting_f32(a, SET_MAX_STEP_HEIGHT);
    c.min_floor_cos = sh_aas_setting_f32(a, SET_MIN_FLOOR_COS);
    /* Reject unreadable or invalid minFloorCos; a zero fallback would admit
     * walls.
     */
    if (c.min_floor_cos <= 0.0 || c.min_floor_cos > 1.0) return 0;
    sh_aas_agent_bounds(a, mins, maxs);
    fw = maxs[0] - mins[0];
    fd = maxs[1] - mins[1];

    /* Build walkable pieces from intersecting solids before augmentation.
     * Keep the geometry scratch on the heap.
     */
    {
        size_t nmax = SH_AUG_MAX_PLATFORMS;
        size_t need = nmax * (sizeof *work + sizeof *effs + sizeof *reqs +
                              sizeof *peers + 4 * sizeof(int) + 1);
        unsigned char *at;
        block = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, need);
        if (!block) return 0;           /* nothing has been written yet */
        at = block;
        work     = (sh_aug_platform *)at; at += nmax * sizeof *work;
        effs     = (aug_quad *)at;        at += nmax * sizeof *effs;
        reqs     = (aug_quad *)at;        at += nmax * sizeof *reqs;
        peers    = (aug_peer *)at;        at += nmax * sizeof *peers;
        order    = (int *)at;             at += nmax * sizeof(int);
        made     = (int *)at;             at += nmax * sizeof(int);
        wsrc     = (int *)at;             at += nmax * sizeof(int);
        wpieces  = (int *)at;             at += nmax * sizeof(int);
        wburied  = at;
    }
    out->source_count = n;
    {
        int solids=0;
        for(i=0;i<n;i++)if(plats[i].depth>0.0f)solids++;
        if(solids) {
            c.prepared_geometry=1;
            sh_aug_platform *support=(sh_aug_platform*)calloc(c.original_areas,sizeof *support);
            unsigned area;int ns=0;
            if(!support){HeapFree(GetProcessHeap(),0,block);return 0;}
            for(area=1;area<c.original_areas;area++) {
                aug_quad floor;
                if(aug_original_floor(a,area,&floor)&&floor.n[2]>=c.min_floor_cos) {
                    int v,axis;support[ns].corners=floor.count;
                    for(axis=0;axis<3;axis++)support[ns].n[axis]=(float)floor.n[axis];
                    for(v=0;v<floor.count;v++)for(axis=0;axis<3;axis++)
                        support[ns].c[v][axis]=(float)floor.c[v][axis];
                    ns++;
                }
            }
            n=sh_nav_geometry_build_supported(plats,n,support,ns,c.radius,c.height,c.min_floor_cos,c.step,
                work,wsrc,wpieces,wburied,SH_AUG_MAX_PLATFORMS);
            free(support);
            if(n<0){out->pieces_truncated=1;HeapFree(GetProcessHeap(),0,block);return 0;}
        } else for(i=0;i<n;i++){
            work[i]=plats[i];wsrc[i]=i;wpieces[i]=1;wburied[i]=0;
        }
    }
    plats = work;

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
        pr->source = wsrc[idx];
        pr->pieces = wpieces[idx];
        if (wburied[idx]) {
            /* Report fully buried faces directly instead of failing an
             * unrelated size gate.
             */
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "no standing room after slope, solid and agent-clearance clipping");
            continue;
        }
        pr->carrier = -1;

        pr->tilt_degrees = (float)aug_degrees_from_horizontal(p->n[2]);
        /* Face 4 is an upright top; other faces indicate a tipped box. */
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
        if (!aug_quad_inset(&req, p->prepared ? 0.0 : c.radius, &eff) ||
            (!p->prepared && aug_quad_min_width(&eff) < (fw < fd ? fw : fd))) {
            /* The AABB would overstate a rotated quad's usable size, so the
             * comparison is against its narrowest edge-to-corner width. */
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "too small: %.0f units across after the %.0f-unit "
                        "agent-radius inset, this demon size needs %.0fx%.0f",
                        aug_quad_min_width(&req) - 2.0 * c.radius, c.radius, fw, fd);
            continue;
        }
        head = aug_headroom(&c, &req, plats, p->prepared ? 0 : n, idx);
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
            double mx, my; aug_centre(&eff, &mx, &my);
            carrier = sh_aas_point_area(a, (float)mx, (float)my,
                                        (float)aug_z_at(&eff, mx, my));
        }
        /* Require a carrier under the centre before appending the area.
         * Carving can succeed at an overhang's edge while leaving its centre
         * in void; append-only geometry cannot safely undo that phantom area
         * later.
         */
        if (carrier <= 0) {
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "nothing walkable under the middle of it -- this box "
                        "hangs over space the module has no floor in");
            continue;
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
            /* Unexpected carve failure after area append: do not report it as emitted. */
            _snprintf_s(pr->reason, sizeof pr->reason, _TRUNCATE,
                        "nowhere to splice this into the navigation tree");
            c.failed = 1; break;
        }
        pr->emitted = 1;
        pr->area = area;
        pr->carrier = carrier;
        {
            double mx, my; aug_centre(&eff, &mx, &my);
            pr->centre[0] = (float)mx;
            pr->centre[1] = (float)my;
            pr->centre[2] = (float)aug_z_at(&eff, mx, my);
        }
        effs[nmade] = eff;
        reqs[nmade] = req;
        made[nmade] = area;
        nmade++;
    }

    if(c.prepared_geometry&&!c.failed&&!aug_stitch_edges(&c,effs,made,nmade))c.failed=1;
    if (!c.failed) {
        /* Create shared-boundary walks before the general step linker. */
        for (i = 0; i < nmade; i++) {
            unsigned na = sh_aas_count(a, SH_AAS_L_AREAS);
            unsigned b;
            for (b = 1; b < na; b++) {
                if ((int)b == made[i]) continue;
                aug_walk_links(&c, made[i], (int)b);
                aug_walk_links(&c, (int)b, made[i]);
            }
        }
        /* The peer table contains only emitted surfaces. Its indices no
         * longer match the requested platform array. It supplies neighbours
         * directly when agent clearance separates their BSP footprints.
         */
        for (i = 0; i < nmade; i++) {
            peers[i].req = &reqs[i];
            peers[i].eff = &effs[i];
            peers[i].area = made[i];
            peers[i].support=reqs[i];
            peers[i].source=-1;
            peers[i].prepared=0;
            for(j=0;j<n;j++)if(out->platforms[j].area==made[i]) {
                peers[i].source=out->platforms[j].source;
                peers[i].prepared=plats[j].prepared;
                if(!plats[j].prepared)break;
                double corners[4][3];int v,x;
                for(v=0;v<4;v++)for(x=0;x<3;x++)corners[v][x]=plats[j].support[v][x];
                aug_quad_init(&peers[i].support,corners);break;
            }
        }
        for (i = 0; i < nmade; i++)
            aug_step_links(&c, made[i], &effs[i], &reqs[i], peers, nmade);
        for (i = 0; i < nmade; i++)
            aug_fall_links(&c, made[i], &effs[i], &reqs[i], peers, nmade);

        /* Append traversals last to keep their reachabilities in one tail. */
        {
            aug_trav_buffer buffer = {0};
            {
                int total = 0;
                aug_trav_spec *specs;
                for (i = 0; i < nmade && !c.failed; i++)
                    aug_traversal_specs(&c, made[i], &effs[i], &reqs[i], peers, nmade, &buffer);
                specs = buffer.data;
                total = buffer.count;
                /* Budget basic links and traversals together before allocating
                 * traversal-point indices. Report only emitted leap samples. */
                if (!c.failed) total = aug_budget_traversals(&c, specs, total, out);
                if (!c.failed && total > 0) {
                    int written = aug_emit_traversals(&c, specs, total);
                    if (written != total) c.failed = 1;
                    for (i = 0; i < written; i++) {
                        if (!specs[i].is_leap) continue;
                        for (j = 0; j < n; j++)
                            if (out->platforms[j].area == specs[i].from_area ||
                                out->platforms[j].area == specs[i].to_area)
                                out->platforms[j].leaps++;
                    }
                }
                free(specs);
            }
        }
        /* Report gaps beyond contact/step handling but shorter than available
         * leaps. Both platforms can have floor routes without being linked to
         * each other.
         */
        for (i = 0; i < nmade; i++) {
            for (j = i + 1; j < nmade; j++) {
                double gap = aug_quad_gap(&reqs[i], &reqs[j]);
                unsigned r, nr;
                int linked = 0;
                if (gap <= AUG_TOUCH_EPS || gap >= (double)SH_TRAV_LEAP_MIN_SPAN)
                    continue;
                nr = sh_aas_count(a, SH_AAS_L_REACHABILITIES);
                for (r = 0; r < nr && !linked; r++) {
                    const unsigned char *rr =
                        sh_aas_rec_const(a, SH_AAS_L_REACHABILITIES, r);
                    int f, t;
                    if (!rr) continue;
                    f = (int)sh_aas_get_u16(rr, RE_FROM_AREA);
                    t = (int)sh_aas_get_u16(rr, RE_TO_AREA);
                    if ((f == made[i] && t == made[j]) ||
                        (f == made[j] && t == made[i])) linked = 1;
                }
                if (!linked) out->dead_gaps++;
            }
        }
        aug_relink(&c);
    }

    if (!c.failed && !aug_rebuild_pvs(&c)) c.failed = 1;

    /* trees[0].c is the number of distinct areas the tree references plus one,
     * true in 20/20 shipped payloads. Areas were added, so it must move. */
    if (sh_aas_count(a, SH_AAS_L_TREES) > 0) {
        unsigned char *t = sh_aas_rec(a, SH_AAS_L_TREES, 0);
        if (t) sh_aas_put_i32(t, TR_C, (int32_t)sh_aas_count(a, SH_AAS_L_AREAS));
    }

    /* Report island status for every emitted surface. */
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
            /* Count distinct neighbour areas; link count alone cannot show connectivity. */
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

    rc = c.failed ? 0 : 1;
    HeapFree(GetProcessHeap(), 0, block);
    return rc;
}

#ifdef SH_AUG_TESTING
int sh_aug_test_trav_anchors(double lo, double hi, double *out, int cap)
{
    return aug_trav_anchors(lo, hi, out, cap);
}

/* Test access to the private aug_quad representation. */
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
