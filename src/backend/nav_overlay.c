/* nav_overlay.c -- cut the native placement grid model where the bake cut the
 * module floor. See nav_overlay.h. Pure: no engine state, no hooks. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nav_overlay.h"
#include "aas_edit.h"

/* A triangle clipped by a convex polygon has at most three more corners than
 * the polygon. Seam stitching can give a floor area many collinear corners. */
#define OV_MAX_POINTS   192
#define OV_MAX_CORNERS  (OV_MAX_POINTS - 8)
/* Slivers along a shared edge come from rounding, not from the cut. */
#define OV_MIN_AREA     0.05
#define OV_TRI_CAP      4096
#define OV_VERTEX_BYTES 48

typedef struct ov_poly { int n; double p[OV_MAX_POINTS][2]; } ov_poly;

static double ov_signed_area2(const double (*p)[2], int n)
{
    double s = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;
        s += p[i][0] * p[j][1] - p[j][0] * p[i][1];
    }
    return s * 0.5;
}

static double ov_polygon_area(const sh_ov_polygon *q)
{
    double s = 0.0;
    int i;
    for (i = 0; i < q->count; i++) {
        int j = (i + 1) % q->count;
        s += q->c[i][0] * q->c[j][1] - q->c[j][0] * q->c[i][1];
    }
    return s * 0.5;
}

/* Keep the part of `in` on the side of a->b that `sign` names (+1 left). */
static void ov_clip_half(const ov_poly *in, const double a[2], const double b[2], double sign,
                         ov_poly *out)
{
    int i;
    double ex = b[0] - a[0], ey = b[1] - a[1];
    out->n = 0;
    for (i = 0; i < in->n; i++) {
        const double *p = in->p[i], *q = in->p[(i + 1) % in->n];
        double dp = sign * (ex * (p[1] - a[1]) - ey * (p[0] - a[0]));
        double dq = sign * (ex * (q[1] - a[1]) - ey * (q[0] - a[0]));
        if (dp >= 0.0 && out->n < OV_MAX_POINTS) {
            out->p[out->n][0] = p[0]; out->p[out->n][1] = p[1]; out->n++;
        }
        if ((dp >= 0.0) != (dq >= 0.0) && out->n < OV_MAX_POINTS) {
            double t = dp / (dp - dq);
            out->p[out->n][0] = p[0] + (q[0] - p[0]) * t;
            out->p[out->n][1] = p[1] + (q[1] - p[1]) * t;
            out->n++;
        }
    }
    if (out->n < 3) out->n = 0;
}

static int ov_edge_usable(const sh_ov_polygon *q, int i)
{
    const double *a = q->c[i], *b = q->c[(i + 1) % q->count];
    return fabs(b[0] - a[0]) + fabs(b[1] - a[1]) > 1e-6;
}

/* `in` clipped to the inside of a convex polygon. */
static void ov_clip_convex(const ov_poly *in, const sh_ov_polygon *q, double sign, ov_poly *out,
                           ov_poly *scratch)
{
    int i;
    *out = *in;
    for (i = 0; i < q->count && out->n; i++) {
        if (!ov_edge_usable(q, i)) continue;
        ov_clip_half(out, q->c[i], q->c[(i + 1) % q->count], sign, scratch);
        *out = *scratch;
    }
}

typedef struct ov_frags { ov_poly *v; int n, cap; } ov_frags;

static int ov_push(ov_frags *f, const ov_poly *p)
{
    if (p->n < 3 || fabs(ov_signed_area2((const double (*)[2])p->p, p->n)) < OV_MIN_AREA) return 1;
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 8;
        ov_poly *v;
        if (cap > OV_TRI_CAP) return 0;
        v = (ov_poly *)realloc(f->v, (size_t)cap * sizeof *v);
        if (!v) return 0;
        f->v = v; f->cap = cap;
    }
    f->v[f->n++] = *p;
    return 1;
}

static int ov_polygon_ok(const sh_ov_polygon *q)
{
    return q && q->c && q->count >= 3 && q->count <= OV_MAX_CORNERS &&
           fabs(ov_polygon_area(q)) >= OV_MIN_AREA;
}

int sh_nav_overlay_cut_triangle(const double tri[3][3], const sh_ov_cut *cuts, int cut_count,
                                double (*out)[3][3], int cap, int *touched)
{
    double n[3], u[3], v[3], tsign;
    ov_frags cur = { 0 }, next = { 0 };
    ov_poly *t = NULL, *work = NULL, *part = NULL, *scratch = NULL;
    int c, i, k, written = -1, any = 0;
    if (touched) *touched = 0;
    if (!tri || !out || cap < 1 || (cut_count > 0 && !cuts)) return -1;
    for (k = 0; k < 3; k++) { u[k] = tri[1][k] - tri[0][k]; v[k] = tri[2][k] - tri[0][k]; }
    n[0] = u[1] * v[2] - u[2] * v[1];
    n[1] = u[2] * v[0] - u[0] * v[2];
    n[2] = u[0] * v[1] - u[1] * v[0];
    /* A wall or a degenerate triangle stands on no floor. */
    if (!(fabs(n[2]) > 0.25 * sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]))) goto keep;
    tsign = n[2] > 0 ? 1.0 : -1.0;
    t = (ov_poly *)malloc(4 * sizeof *t);
    if (!t) return -1;
    work = t + 1; part = t + 2; scratch = t + 3;
    t->n = 3;
    for (i = 0; i < 3; i++) { t->p[i][0] = tri[i][0]; t->p[i][1] = tri[i][1]; }
    if (!ov_push(&cur, t)) goto done;
    for (c = 0; c < cut_count; c++) {
        const sh_ov_cut *cut = &cuts[c];
        double cx = 0, cy = 0, cz = 0, zt, wsign;
        double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
        if (!ov_polygon_ok(&cut->whole)) continue;
        for (i = 0; i < cut->whole.count; i++) {
            cx += cut->whole.c[i][0]; cy += cut->whole.c[i][1]; cz += cut->whole.c[i][2];
            for (k = 0; k < 2; k++) {
                if (cut->whole.c[i][k] < lo[k]) lo[k] = cut->whole.c[i][k];
                if (cut->whole.c[i][k] > hi[k]) hi[k] = cut->whole.c[i][k];
            }
        }
        cx /= cut->whole.count; cy /= cut->whole.count; cz /= cut->whole.count;
        /* The triangle's own height under the area's middle. */
        zt = tri[0][2] - (n[0] * (cx - tri[0][0]) + n[1] * (cy - tri[0][1])) / n[2];
        if (fabs(zt - cz) > SH_OV_Z_TOLERANCE) continue;
        {
            double tl[2] = { 1e30, 1e30 }, th[2] = { -1e30, -1e30 };
            for (i = 0; i < 3; i++) for (k = 0; k < 2; k++) {
                if (tri[i][k] < tl[k]) tl[k] = tri[i][k];
                if (tri[i][k] > th[k]) th[k] = tri[i][k];
            }
            if (th[0] <= lo[0] || tl[0] >= hi[0] || th[1] <= lo[1] || tl[1] >= hi[1]) continue;
        }
        wsign = ov_polygon_area(&cut->whole) > 0 ? 1.0 : -1.0;
        next.n = 0;
        for (i = 0; i < cur.n; i++) {
            const ov_poly *f = &cur.v[i];
            int e, p;
            ov_clip_convex(f, &cut->whole, wsign, part, scratch);
            if (part->n < 3 || fabs(ov_signed_area2((const double (*)[2])part->p, part->n)) < OV_MIN_AREA) {
                if (!ov_push(&next, f)) goto done;
                continue;
            }
            any = 1;
            /* The ground outside the cut area, one convex slice per edge. */
            *work = *f;
            for (e = 0; e < cut->whole.count && work->n; e++) {
                const double *a, *b;
                if (!ov_edge_usable(&cut->whole, e)) continue;
                a = cut->whole.c[e]; b = cut->whole.c[(e + 1) % cut->whole.count];
                ov_clip_half(work, a, b, -wsign, part);
                if (!ov_push(&next, part)) goto done;
                ov_clip_half(work, a, b, wsign, scratch);
                *work = *scratch;
            }
            /* The ground inside it the bake kept. */
            for (p = 0; p < cut->piece_count; p++) {
                const sh_ov_polygon *piece = &cut->pieces[p];
                if (!ov_polygon_ok(piece)) continue;
                ov_clip_convex(f, piece, ov_polygon_area(piece) > 0 ? 1.0 : -1.0, part, scratch);
                if (!ov_push(&next, part)) goto done;
            }
        }
        { ov_frags swap = cur; cur = next; next = swap; }
    }
    if (!any) goto keep_free;
    written = 0;
    for (i = 0; i < cur.n; i++) {
        const ov_poly *f = &cur.v[i];
        double fsign = ov_signed_area2((const double (*)[2])f->p, f->n) > 0 ? 1.0 : -1.0;
        for (k = 1; k + 1 < f->n; k++) {
            const double *corner[3];
            int j;
            double fan[3][2];
            fan[0][0] = f->p[0][0]; fan[0][1] = f->p[0][1];
            fan[1][0] = f->p[k][0]; fan[1][1] = f->p[k][1];
            fan[2][0] = f->p[k + 1][0]; fan[2][1] = f->p[k + 1][1];
            /* A corner the clip left on an edge fans into a sliver. */
            if (fabs(ov_signed_area2((const double (*)[2])fan, 3)) < OV_MIN_AREA) continue;
            if (written == cap) { written = -1; goto done; }
            corner[0] = f->p[0];
            corner[1] = fsign == tsign ? f->p[k] : f->p[k + 1];
            corner[2] = fsign == tsign ? f->p[k + 1] : f->p[k];
            for (j = 0; j < 3; j++) {
                out[written][j][0] = corner[j][0];
                out[written][j][1] = corner[j][1];
                out[written][j][2] = tri[0][2] -
                    (n[0] * (corner[j][0] - tri[0][0]) + n[1] * (corner[j][1] - tri[0][1])) / n[2];
            }
            written++;
        }
    }
    if (touched) *touched = 1;
    goto done;
keep_free:
    free(t); t = NULL;
keep:
    memcpy(out[0], tri, sizeof out[0]);
    written = 1;
done:
    free(t); free(cur.v); free(next.v);
    return written;
}

/* ---- .bmodel ---------------------------------------------------------- */

static uint32_t ov_be32(const unsigned char *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned ov_be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static void ov_put32(unsigned char *p, uint32_t v)
{ p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static float ov_getf(const unsigned char *p) { uint32_t b = ov_be32(p); float f; memcpy(&f, &b, 4); return f; }
static void ov_putf(unsigned char *p, float f) { uint32_t b; memcpy(&b, &f, 4); ov_put32(p, b); }
static int ov_block(size_t size, size_t at, size_t count, size_t stride)
{ return at <= size && stride && count <= (size - at) / stride; }

typedef struct ov_surface {
    size_t start;       /* first byte of the surface */
    size_t geometry;    /* its 52-byte geometry header */
    uint32_t vertices, indices;
} ov_surface;

/* The surface table of a stock .bmodel; the layout sh_grid_bmodel accepts. */
static int ov_surfaces(const unsigned char *s, size_t n, ov_surface **out, uint32_t *count, size_t *tail)
{
    size_t at = 12;
    uint32_t magic, surfaces, i, j;
    ov_surface *list;
    *out = NULL;
    if (!s || n < 16 || n > 64u * 1024u * 1024u) return 0;
    magic = ov_be32(s); surfaces = ov_be32(s + 8);
    if ((magic != 0x1a4c4d42 && magic != 0x1b4c4d42) || surfaces < 1 || surfaces > 10000) return 0;
    list = (ov_surface *)calloc(surfaces, sizeof *list);
    if (!list) return 0;
    for (i = 0; i < surfaces; i++) {
        uint32_t length, patches;
        list[i].start = at;
        if (!ov_block(n, at, 1, 4)) goto bad;
        length = (uint32_t)s[at] | (uint32_t)s[at + 1] << 8 | (uint32_t)s[at + 2] << 16 | (uint32_t)s[at + 3] << 24;
        at += 4;
        if (length > 10000 || !ov_block(n, at, length, 1)) goto bad;
        at += length;
        if (magic == 0x1b4c4d42) at += 4;
        if (!ov_block(n, at, 1, 12)) goto bad;
        patches = ov_be32(s + at + 8); at += 12;
        if (patches > 10000) goto bad;
        for (j = 0; j < patches; j++) {
            if (!ov_block(n, at, 1, 4)) goto bad;
            length = (uint32_t)s[at] | (uint32_t)s[at + 1] << 8 | (uint32_t)s[at + 2] << 16 | (uint32_t)s[at + 3] << 24;
            at += 4;
            if (length > 10000 || !ov_block(n, at, length, 1)) goto bad;
            at += length;
        }
        if (!ov_block(n, at, 1, 52)) goto bad;
        list[i].geometry = at;
        list[i].vertices = ov_be32(s + at); list[i].indices = ov_be32(s + at + 4);
        if (ov_be32(s + at + 8) != 0x1801f || list[i].vertices > 65535 ||
            list[i].indices > 1000000 || list[i].indices % 3) goto bad;
        at += 52;
        if (!ov_block(n, at, list[i].vertices, OV_VERTEX_BYTES)) goto bad;
        at += (size_t)list[i].vertices * OV_VERTEX_BYTES;
        if (!ov_block(n, at, list[i].indices, 2)) goto bad;
        for (j = 0; j < list[i].indices; j++)
            if (ov_be16(s + at + (size_t)j * 2) >= list[i].vertices) goto bad;
        at += (size_t)list[i].indices * 2;
        if (!ov_block(n, at, 1, 32) || ov_be32(s + at + 28) != magic) goto bad;
        at += 32;
    }
    *out = list; *count = surfaces; *tail = at;
    return 1;
bad:
    free(list);
    return 0;
}

int sh_nav_overlay_triangle_count(const unsigned char *model, size_t model_len)
{
    ov_surface *list;
    uint32_t count, i;
    size_t tail;
    long long triangles = 0;
    if (!ov_surfaces(model, model_len, &list, &count, &tail)) return -1;
    for (i = 0; i < count; i++) triangles += list[i].indices / 3;
    free(list);
    return triangles > 0x7fffffff ? -1 : (int)triangles;
}

/* ---- navigation polygons ---------------------------------------------- */

typedef struct ov_area { int count; double (*c)[3]; } ov_area;

static int ov_read_area(const sh_aas *a, unsigned area, ov_area *out)
{
    const unsigned char *ar = sh_aas_rec_const(a, SH_AAS_L_AREAS, area);
    unsigned first, e;
    int n;
    out->count = 0; out->c = NULL;
    if (!ar) return 0;
    n = sh_aas_get_u16(ar, 6); first = sh_aas_get_u32(ar, 8);
    if (n < 3 || n > OV_MAX_CORNERS) return 1;     /* nothing to draw, not an error */
    out->c = (double (*)[3])malloc((size_t)n * sizeof *out->c);
    if (!out->c) return 0;
    for (e = 0; e < (unsigned)n; e++) {
        const unsigned char *index = sh_aas_rec_const(a, SH_AAS_L_EDGEINDEX, first + e), *edge, *vertex;
        int ei, vi, k;
        if (!index) goto bad;
        ei = sh_aas_get_i32(index, 0);
        edge = sh_aas_rec_const(a, SH_AAS_L_EDGES, (unsigned)abs(ei));
        if (!edge) goto bad;
        vi = sh_aas_get_i32(edge, ei < 0 ? 4 : 0);
        vertex = sh_aas_rec_const(a, SH_AAS_L_VERTICES, (unsigned)vi);
        if (!vertex) goto bad;
        for (k = 0; k < 3; k++) out->c[e][k] = sh_aas_get_f32(vertex, (unsigned)k * 4);
    }
    out->count = n;
    return 1;
bad:
    free(out->c); out->c = NULL;
    return 0;
}

static double ov_area_size(const ov_area *a)
{
    sh_ov_polygon q;
    if (a->count < 3) return 0.0;
    q.count = a->count; q.c = (const double (*)[3])a->c;
    return fabs(ov_polygon_area(&q));
}

static int ov_contains(const ov_area *a, double x, double y)
{
    sh_ov_polygon q;
    double sign;
    int i;
    if (a->count < 3) return 0;
    q.count = a->count; q.c = (const double (*)[3])a->c;
    sign = ov_polygon_area(&q) > 0 ? 1.0 : -1.0;
    for (i = 0; i < a->count; i++) {
        const double *p = a->c[i], *r = a->c[(i + 1) % a->count];
        if (sign * ((r[0] - p[0]) * (y - p[1]) - (r[1] - p[1]) * (x - p[0])) < -1e-3) return 0;
    }
    return 1;
}

static void ov_centre(const ov_area *a, double c[3])
{
    int i, k;
    c[0] = c[1] = c[2] = 0;
    for (i = 0; i < a->count; i++) for (k = 0; k < 3; k++) c[k] += a->c[i][k];
    if (a->count) for (k = 0; k < 3; k++) c[k] /= a->count;
}

/* ---- vertices --------------------------------------------------------- */

typedef struct ov_vertices {
    unsigned char *bytes;   /* count * 48 */
    uint32_t count, cap, original;
    int *slots;             /* open-addressed hash of the added vertices */
    uint32_t slot_cap;
} ov_vertices;

static uint32_t ov_hash(const unsigned char *p)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < OV_VERTEX_BYTES; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static int ov_rehash(ov_vertices *v, uint32_t cap)
{
    int *slots = (int *)malloc((size_t)cap * sizeof *slots);
    uint32_t i;
    if (!slots) return 0;
    for (i = 0; i < cap; i++) slots[i] = -1;
    for (i = v->original; i < v->count; i++) {
        uint32_t h = ov_hash(v->bytes + (size_t)i * OV_VERTEX_BYTES) & (cap - 1);
        while (slots[h] >= 0) h = (h + 1) & (cap - 1);
        slots[h] = (int)i;
    }
    free(v->slots); v->slots = slots; v->slot_cap = cap;
    return 1;
}

/* The index of a vertex with exactly these bytes among the added ones,
 * appended if new. -1 past the 16-bit index range. */
static int ov_vertex(ov_vertices *v, const unsigned char *bytes)
{
    uint32_t h;
    if ((v->count - v->original + 1) * 2 > v->slot_cap &&
        !ov_rehash(v, v->slot_cap ? v->slot_cap * 2 : 256)) return -1;
    h = ov_hash(bytes) & (v->slot_cap - 1);
    while (v->slots[h] >= 0) {
        if (!memcmp(v->bytes + (size_t)v->slots[h] * OV_VERTEX_BYTES, bytes, OV_VERTEX_BYTES))
            return v->slots[h];
        h = (h + 1) & (v->slot_cap - 1);
    }
    if (v->count >= 65535) return -1;
    if (v->count == v->cap) {
        uint32_t cap = v->cap * 2 + 64;
        unsigned char *b = (unsigned char *)realloc(v->bytes, (size_t)cap * OV_VERTEX_BYTES);
        if (!b) return -1;
        v->bytes = b; v->cap = cap;
    }
    memcpy(v->bytes + (size_t)v->count * OV_VERTEX_BYTES, bytes, OV_VERTEX_BYTES);
    v->slots[h] = (int)v->count;
    return (int)v->count++;
}

/* A new corner on stock triangle (a, b, c): its position, the texture
 * coordinates interpolated from the three corners, and every other attribute
 * of the first corner (the stock floor shares one normal and colour). */
static void ov_make_vertex(const unsigned char *a, const unsigned char *b, const unsigned char *c,
                           const double p[3], unsigned char *out)
{
    double ax = ov_getf(a), ay = ov_getf(a + 4), bx = ov_getf(b), by = ov_getf(b + 4);
    double cx = ov_getf(c), cy = ov_getf(c + 4);
    double d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy), wa = 1, wb = 0, wc = 0;
    int k;
    if (fabs(d) > 1e-9) {
        wa = ((by - cy) * (p[0] - cx) + (cx - bx) * (p[1] - cy)) / d;
        wb = ((cy - ay) * (p[0] - cx) + (ax - cx) * (p[1] - cy)) / d;
        wc = 1.0 - wa - wb;
    }
    memcpy(out, a, OV_VERTEX_BYTES);
    for (k = 0; k < 3; k++) ov_putf(out + k * 4, (float)p[k]);
    for (k = 0; k < 2; k++)
        ov_putf(out + 12 + k * 4, (float)(wa * ov_getf(a + 12 + k * 4) +
                                          wb * ov_getf(b + 12 + k * 4) +
                                          wc * ov_getf(c + 12 + k * 4)));
}

/* ---- the build -------------------------------------------------------- */

int sh_nav_overlay_build(const unsigned char *model, size_t model_len,
                         const unsigned char *shipped, size_t shipped_len,
                         const unsigned char *cut, size_t cut_len, unsigned first_area,
                         unsigned char **out, size_t *out_len, sh_nav_overlay_stats *stats)
{
    char err[128];
    sh_aas *before = NULL, *after = NULL;
    ov_surface *surfaces = NULL;
    uint32_t surface_count = 0, s;
    size_t tail = 0, size, at;
    unsigned original = 0, i, total;
    ov_area *old_areas = NULL, *new_areas = NULL;
    int *parent = NULL, result = -1, cut_count = 0, piece_total = 0;
    sh_ov_cut *cuts = NULL;
    sh_ov_polygon *pieces = NULL;
    double (*tris)[3][3] = NULL;
    ov_vertices *verts = NULL;
    uint16_t **indices = NULL;
    uint32_t *index_count = NULL;
    unsigned char *buffer = NULL;
    sh_nav_overlay_stats st;

    memset(&st, 0, sizeof st);
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!out || !out_len || !model || !shipped || !cut) goto finish;
    if (!ov_surfaces(model, model_len, &surfaces, &surface_count, &tail)) goto finish;
    before = sh_aas_parse(shipped, shipped_len, err, sizeof err);
    after = sh_aas_parse(cut, cut_len, err, sizeof err);
    if (!before || !after) goto finish;
    original = sh_aas_count(before, SH_AAS_L_AREAS);
    total = sh_aas_count(after, SH_AAS_L_AREAS);
    if (original < 1 || first_area < original || first_area > total) goto finish;

    old_areas = (ov_area *)calloc(original, sizeof *old_areas);
    new_areas = (ov_area *)calloc(first_area, sizeof *new_areas);
    parent = (int *)calloc(first_area, sizeof *parent);
    if (!old_areas || !new_areas || !parent) goto finish;
    for (i = 1; i < original; i++)
        if (!ov_read_area(before, i, &old_areas[i]) || !ov_read_area(after, i, &new_areas[i])) goto finish;
    for (i = original; i < first_area; i++) {
        double c[3];
        unsigned a;
        if (!ov_read_area(after, i, &new_areas[i])) goto finish;
        parent[i] = 0;
        if (new_areas[i].count < 3) continue;
        ov_centre(&new_areas[i], c);
        for (a = 1; a < original; a++) {
            double oc[3];
            if (old_areas[a].count < 3 || !ov_contains(&old_areas[a], c[0], c[1])) continue;
            ov_centre(&old_areas[a], oc);
            if (fabs(oc[2] - c[2]) > SH_OV_Z_TOLERANCE) continue;
            parent[i] = (int)a;
            break;
        }
    }

    /* A cut area lost ground: its own polygon shrank, or pieces of it were
     * appended. Seam stitching can add collinear corners without either. */
    cuts = (sh_ov_cut *)calloc(original, sizeof *cuts);
    pieces = (sh_ov_polygon *)calloc((size_t)first_area + 1, sizeof *pieces);
    if (!cuts || !pieces) goto finish;
    for (i = 1; i < original; i++) {
        unsigned p;
        int appended = 0;
        if (old_areas[i].count < 3) continue;
        for (p = original; p < first_area; p++) if (parent[p] == (int)i) appended++;
        if (!appended && fabs(ov_area_size(&new_areas[i]) - ov_area_size(&old_areas[i])) < 1.0) continue;
        cuts[cut_count].whole.count = old_areas[i].count;
        cuts[cut_count].whole.c = (const double (*)[3])old_areas[i].c;
        cuts[cut_count].pieces = &pieces[piece_total];
        if (new_areas[i].count >= 3) {
            pieces[piece_total].count = new_areas[i].count;
            pieces[piece_total].c = (const double (*)[3])new_areas[i].c;
            piece_total++;
        }
        for (p = original; p < first_area; p++) if (parent[p] == (int)i && new_areas[p].count >= 3) {
            pieces[piece_total].count = new_areas[p].count;
            pieces[piece_total].c = (const double (*)[3])new_areas[p].c;
            piece_total++;
        }
        cuts[cut_count].piece_count = (int)(&pieces[piece_total] - cuts[cut_count].pieces);
        cut_count++;
    }
    st.cut_areas = cut_count;
    st.pieces = piece_total;
    for (s = 0; s < surface_count; s++) {
        st.triangles_before += (int)(surfaces[s].indices / 3);
        st.vertices_before += (int)surfaces[s].vertices;
    }
    if (!cut_count) { result = 0; goto finish; }

    tris = (double (*)[3][3])malloc(OV_TRI_CAP * sizeof *tris);
    verts = (ov_vertices *)calloc(surface_count, sizeof *verts);
    indices = (uint16_t **)calloc(surface_count, sizeof *indices);
    index_count = (uint32_t *)calloc(surface_count, sizeof *index_count);
    if (!tris || !verts || !indices || !index_count) goto finish;
    size = 12;
    for (s = 0; s < surface_count; s++) {
        const ov_surface *sf = &surfaces[s];
        const unsigned char *vb = model + sf->geometry + 52;
        const unsigned char *ib = vb + (size_t)sf->vertices * OV_VERTEX_BYTES;
        uint32_t t, cap = sf->indices + 64;
        ov_vertices *v = &verts[s];
        v->cap = sf->vertices + 64;
        v->bytes = (unsigned char *)malloc((size_t)v->cap * OV_VERTEX_BYTES);
        indices[s] = (uint16_t *)malloc((size_t)cap * sizeof **indices);
        if (!v->bytes || !indices[s]) goto finish;
        memcpy(v->bytes, vb, (size_t)sf->vertices * OV_VERTEX_BYTES);
        v->count = v->original = sf->vertices;
        for (t = 0; t + 2 < sf->indices; t += 3) {
            unsigned corner[3];
            double tri[3][3];
            int k, j, made, touched = 0;
            for (k = 0; k < 3; k++) {
                corner[k] = ov_be16(ib + (size_t)(t + k) * 2);
                for (j = 0; j < 3; j++) tri[k][j] = ov_getf(vb + (size_t)corner[k] * OV_VERTEX_BYTES + j * 4);
            }
            made = sh_nav_overlay_cut_triangle((const double (*)[3])tri, cuts, cut_count,
                                               tris, OV_TRI_CAP, &touched);
            if (made < 0) goto finish;
            if (index_count[s] + (uint32_t)made * 3 > cap) {
                uint16_t *grown;
                cap = (index_count[s] + (uint32_t)made * 3) * 2;
                if (cap > 1000000) goto finish;
                grown = (uint16_t *)realloc(indices[s], (size_t)cap * sizeof *grown);
                if (!grown) goto finish;
                indices[s] = grown;
            }
            if (!touched) {
                for (k = 0; k < 3; k++) indices[s][index_count[s]++] = (uint16_t)corner[k];
                continue;
            }
            st.triangles_replaced++;
            for (j = 0; j < made; j++) for (k = 0; k < 3; k++) {
                unsigned char bytes[OV_VERTEX_BYTES];
                int index;
                ov_make_vertex(vb + (size_t)corner[0] * OV_VERTEX_BYTES,
                               vb + (size_t)corner[1] * OV_VERTEX_BYTES,
                               vb + (size_t)corner[2] * OV_VERTEX_BYTES, tris[j][k], bytes);
                index = ov_vertex(v, bytes);
                if (index < 0) goto finish;
                indices[s][index_count[s]++] = (uint16_t)index;
            }
        }
        if (index_count[s] > 1000000) goto finish;
        st.triangles_after += (int)(index_count[s] / 3);
        st.vertices_after += (int)v->count;
        size += (sf->geometry - sf->start) + 52 + (size_t)v->count * OV_VERTEX_BYTES +
                (size_t)index_count[s] * 2 + 32;
    }
    size += model_len - tail;
    buffer = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!buffer) goto finish;
    memcpy(buffer, model, 12);
    at = 12;
    for (s = 0; s < surface_count; s++) {
        const ov_surface *sf = &surfaces[s];
        size_t bounds = sf->geometry + 52 + (size_t)sf->vertices * OV_VERTEX_BYTES + (size_t)sf->indices * 2;
        uint32_t k;
        memcpy(buffer + at, model + sf->start, sf->geometry - sf->start + 52);
        ov_put32(buffer + at + (sf->geometry - sf->start), verts[s].count);
        ov_put32(buffer + at + (sf->geometry - sf->start) + 4, index_count[s]);
        at += sf->geometry - sf->start + 52;
        memcpy(buffer + at, verts[s].bytes, (size_t)verts[s].count * OV_VERTEX_BYTES);
        at += (size_t)verts[s].count * OV_VERTEX_BYTES;
        for (k = 0; k < index_count[s]; k++) {
            buffer[at++] = (unsigned char)(indices[s][k] >> 8);
            buffer[at++] = (unsigned char)indices[s][k];
        }
        /* The pieces lie inside the stock triangles, so the stock bounds hold. */
        memcpy(buffer + at, model + bounds, 32);
        at += 32;
    }
    memcpy(buffer + at, model + tail, model_len - tail);
    at += model_len - tail;
    if (at != size) { HeapFree(GetProcessHeap(), 0, buffer); goto finish; }
    *out = buffer; *out_len = size;
    result = 1;
finish:
    if (stats) *stats = st;
    if (old_areas) for (i = 0; i < original; i++) free(old_areas[i].c);
    if (new_areas) for (i = 0; i < first_area; i++) free(new_areas[i].c);
    if (verts) for (s = 0; s < surface_count; s++) { free(verts[s].bytes); free(verts[s].slots); }
    if (indices) for (s = 0; s < surface_count; s++) free(indices[s]);
    free(old_areas); free(new_areas); free(parent); free(cuts); free(pieces); free(tris);
    free(verts); free(indices); free(index_count); free(surfaces);
    sh_aas_free(before); sh_aas_free(after);
    return result;
}
