/* Reproduce BuildRenderWorld's world-area partition (see grid_room_areas.h).
 *
 * The kd tree follows BuildDisjointBoundsKdTree: a single module becomes one
 * node splitting X at its own minimum with that module on both sides; more
 * modules split at the sorted, de-duplicated box faces (never the first or
 * last value of an axis), choosing the largest product of boxes wholly above
 * and wholly below, X before Y before Z and lower values first on ties.
 * Boxes that straddle the plane go to both sides, clipped to it. */
#include "grid_room_areas.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct area_item { float b[6]; int index; } area_item;
typedef struct area_node { int axis; float dist; int child[2]; } area_node;
typedef struct area_tree { area_node *nodes; int count, capacity, failed; } area_tree;

static int compare_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static int push_node(area_tree *t, int axis, float dist)
{
    if (t->count == t->capacity) {
        int capacity = t->capacity ? t->capacity * 2 : 64;
        area_node *nodes;
        if (capacity > 1 << 20) { t->failed = 1; return -1; }
        nodes = (area_node *)realloc(t->nodes, (size_t)capacity * sizeof *nodes);
        if (!nodes) { t->failed = 1; return -1; }
        t->nodes = nodes; t->capacity = capacity;
    }
    t->nodes[t->count].axis = axis; t->nodes[t->count].dist = dist;
    t->nodes[t->count].child[0] = t->nodes[t->count].child[1] = -1;
    return t->count++;
}
/* Returns a node index, or ~module for a leaf. */
static int build(area_tree *t, const area_item *items, int n, int depth)
{
    int a, i, j, axis = -1, upper_n = 0, lower_n = 0, node, child;
    float best = -1.0f, split = 0;
    float *values;
    area_item *upper, *lower;
    if (t->failed) return -1;
    if (n == 1) return ~items[0].index;
    /* The engine reads past an empty list here; a disjoint layout never gets one. */
    if (n < 1 || depth > 256) { t->failed = 1; return -1; }
    values = (float *)malloc((size_t)n * 2 * sizeof *values);
    if (!values) { t->failed = 1; return -1; }
    for (a = 0; a < 3; ++a) {
        int count = 0, unique = 0;
        for (i = 0; i < n; ++i) { values[count++] = items[i].b[a]; values[count++] = items[i].b[a + 3]; }
        qsort(values, (size_t)count, sizeof *values, compare_float);
        for (i = 0; i < count; ++i) if (!unique || values[unique - 1] != values[i]) values[unique++] = values[i];
        for (j = 1; j < unique - 1; ++j) {
            float s = values[j], above = 0, below = 0, score;
            for (i = 0; i < n; ++i) {
                if (!(items[i].b[a] < s)) above += 1.0f;
                else if (!(s < items[i].b[a + 3])) below += 1.0f;
            }
            score = above * below;
            if (best < score) { best = score; split = s; axis = a; }
        }
    }
    free(values);
    /* Coincident boxes on every axis: the engine keeps the first module whose
     * collision is valid. Placed modules are valid, so that is the first one. */
    if (axis < 0) return ~items[0].index;
    upper = (area_item *)malloc((size_t)n * sizeof *upper);
    lower = (area_item *)malloc((size_t)n * sizeof *lower);
    if (!upper || !lower) { free(upper); free(lower); t->failed = 1; return -1; }
    for (i = 0; i < n; ++i) {
        if (split <= items[i].b[axis]) upper[upper_n++] = items[i];
        else if (split < items[i].b[axis + 3]) {
            upper[upper_n] = items[i]; upper[upper_n++].b[axis] = split;
            lower[lower_n] = items[i]; lower[lower_n++].b[axis + 3] = split;
        } else lower[lower_n++] = items[i];
    }
    node = push_node(t, axis, split);
    if (node >= 0) {
        child = build(t, upper, upper_n, depth + 1); if (!t->failed) t->nodes[node].child[0] = child;
        child = build(t, lower, lower_n, depth + 1); if (!t->failed) t->nodes[node].child[1] = child;
    }
    free(upper); free(lower);
    return node;
}
/* Halve [lo,hi] while wider than size. internal counts kept splits; final
 * counts kept pieces. With culling, a piece apart from [mlo,mhi] is dropped. */
static void halve(float lo, float hi, float mlo, float mhi, int cull, float size, int depth,
                  long long *internal, long long *final, int *failed)
{
    float mid;
    if (cull && (hi < mlo || mhi < lo)) return;
    if (!(size < hi - lo)) { ++*final; return; }
    if (depth > 64) { *failed = 1; return; }
    ++*internal;
    mid = (lo + hi) * 0.5f;
    halve(mid, hi, mlo, mhi, cull, size, depth + 1, internal, final, failed);
    halve(lo, mid, mlo, mhi, cull, size, depth + 1, internal, final, failed);
}
static void walk(const area_tree *t, int ref, const float *cell, const float (*bounds)[6], int n,
                 int cull, float size, int depth, sh_render_area_count *out, int *failed)
{
    float child[6];
    if (*failed) return;
    if (depth > 512) { *failed = 1; return; }
    if (ref >= 0) {
        const area_node *node;
        if (ref >= t->count) { *failed = 1; return; }
        node = &t->nodes[ref];
        ++out->nodes;
        memcpy(child, cell, sizeof child); child[node->axis] = node->dist;
        walk(t, node->child[0], child, bounds, n, cull, size, depth + 1, out, failed);
        memcpy(child, cell, sizeof child); child[node->axis + 3] = node->dist;
        walk(t, node->child[1], child, bounds, n, cull, size, depth + 1, out, failed);
        return;
    }
    {
        int m = ~ref, k;
        long long ix = 0, nx = 0, iy = 0, ny = 0;
        const float *box;
        if (m < 0 || m >= n) { *failed = 1; return; }
        box = bounds[m];
        if (cull) for (k = 0; k < 3; ++k) if (cell[k + 3] < box[k] || box[k + 3] < cell[k]) return;
        /* X pieces are split first; each kept X piece is then split on Y the same way. */
        halve(cell[0], cell[3], box[0], box[3], cull, size, 0, &ix, &nx, failed);
        halve(cell[1], cell[4], box[1], box[4], cull, size, 0, &iy, &ny, failed);
        out->nodes += ix + nx * iy;
        out->areas += nx * ny;
    }
}
/* The kd tree and the starting cell do not depend on the cell size, so a
 * prepared layout is walked once per candidate size. */
typedef struct area_layout { area_tree tree; int root; float cell[6]; } area_layout;
static int prepare(const float (*bounds)[6], int n, area_layout *l)
{
    area_item *items;
    int i, k;
    memset(l, 0, sizeof *l);
    if (!bounds || n < 1 || n > 65536) return 0;
    for (i = 0; i < n; ++i) for (k = 0; k < 6; ++k) if (!isfinite(bounds[i][k])) return 0;
    items = (area_item *)malloc((size_t)n * sizeof *items);
    if (!items) return 0;
    for (i = 0; i < n; ++i) { memcpy(items[i].b, bounds[i], sizeof items[i].b); items[i].index = i; }
    if (n == 1) {
        l->root = push_node(&l->tree, 0, bounds[0][0]);
        if (l->root >= 0) l->tree.nodes[l->root].child[0] = l->tree.nodes[l->root].child[1] = ~0;
    } else l->root = build(&l->tree, items, n, 0);
    free(items);
    if (l->tree.failed) { free(l->tree.nodes); l->tree.nodes = NULL; return 0; }
    /* The partition starts from the union of every module's box. */
    for (k = 0; k < 3; ++k) { l->cell[k] = 1e30f; l->cell[k + 3] = -1e30f; }
    for (i = 0; i < n; ++i) for (k = 0; k < 3; ++k) {
        if (bounds[i][k] <= l->cell[k]) l->cell[k] = bounds[i][k];
        if (l->cell[k + 3] <= bounds[i][k + 3]) l->cell[k + 3] = bounds[i][k + 3];
    }
    return 1;
}
static int count_layout(const area_layout *l, const float (*bounds)[6], int n, int cull, float size,
                        sh_render_area_count *out)
{
    int failed = 0;
    memset(out, 0, sizeof *out);
    walk(&l->tree, l->root, l->cell, bounds, n, cull, size, 0, out, &failed);
    return !failed;
}
static int valid_size(float size)
{
    return isfinite(size) && size >= SH_RENDER_AREA_CELL && size <= SH_RENDER_AREA_CELL_MAX;
}
int sh_render_areas_cell(const float (*bounds)[6], int n, int cull, float size, sh_render_area_count *out)
{
    area_layout layout;
    int ok;
    if (!out || !valid_size(size)) return 0;
    memset(out, 0, sizeof *out);
    if (!prepare(bounds, n, &layout)) return 0;
    ok = count_layout(&layout, bounds, n, cull, size, out);
    free(layout.tree.nodes);
    return ok;
}
int sh_render_areas(const float (*bounds)[6], int n, int cull, sh_render_area_count *out)
{
    return sh_render_areas_cell(bounds, n, cull, SH_RENDER_AREA_CELL, out);
}
int sh_render_areas_fit(const sh_render_area_count *count)
{
    return count && count->areas <= SH_RENDER_AREA_MAX && count->nodes <= SH_RENDER_NODE_MAX;
}
int sh_render_areas_fit_coarse(const sh_render_area_count *count)
{
    return count && count->areas <= SH_RENDER_AREA_MAX - SH_RENDER_AREA_HEADROOM &&
           count->nodes <= SH_RENDER_NODE_MAX - SH_RENDER_AREA_HEADROOM;
}
int sh_render_area_cell_choose(const float (*bounds)[6], int n, int cull, float max_size,
                               sh_render_cell_choice *out)
{
    area_layout layout;
    sh_render_area_count count;
    float size;
    int ok = 0;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->cell = SH_RENDER_AREA_CELL;
    if (!valid_size(max_size) || !prepare(bounds, n, &layout)) return 0;
    if (!count_layout(&layout, bounds, n, cull, SH_RENDER_AREA_CELL, &out->native)) goto done;
    out->count = out->coarsest = out->native;
    ok = 1;
    /* The native size whenever it loads: stock maps keep their exact areas. */
    if (sh_render_areas_fit(&out->native)) { out->fits = 1; goto done; }
    for (size = SH_RENDER_AREA_CELL * 2; size <= max_size; size *= 2) {
        if (!count_layout(&layout, bounds, n, cull, size, &count)) { ok = 0; goto done; }
        out->coarsest = count;
        if (!out->fits && sh_render_areas_fit_coarse(&count)) {
            out->fits = 1; out->cell = size; out->count = count;
        }
    }
    /* Nothing fits: report the coarsest count, which is what Play would get. */
    if (!out->fits) { out->cell = size / 2; out->count = out->coarsest; }
done:
    free(layout.tree.nodes);
    return ok;
}
