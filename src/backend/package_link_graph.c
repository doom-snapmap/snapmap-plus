#include "package_link_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lg_item {
    const sh_package_link_node *source;
    sh_package_link_edge *edges;
    size_t *targets;
    size_t index;
} lg_item;

typedef struct lg_group { size_t start, count; int queued; } lg_group;

typedef struct lg_work {
    lg_item *items;
    sh_package_link_edge *edges;
    size_t *targets, *classes, *incoming, *offsets, *queue;
    lg_group *groups;
    size_t count, group_count, queue_begin, queue_count, examined;
} lg_work;

static int lg_size(size_t a, size_t b) { return a < b ? -1 : a > b; }

static int lg_edge_order(const void *left, const void *right)
{
    const sh_package_link_edge *a = left, *b = right;
    return strcmp(a->slot, b->slot);
}

static int lg_shape_order(const void *left, const void *right)
{
    const lg_item *a = left, *b = right;
    int cmp = strcmp(a->source->family, b->source->family);
    size_t i;
    if (!cmp) cmp = strcmp(a->source->path, b->source->path);
    if (!cmp) cmp = a->source->length < b->source->length ? -1 : a->source->length > b->source->length;
    if (!cmp) cmp = memcmp(a->source->content, b->source->content, 32);
    if (!cmp) cmp = memcmp(a->source->semantics, b->source->semantics, 32);
    if (!cmp) cmp = lg_size(a->source->edge_count, b->source->edge_count);
    for (i = 0; !cmp && i < a->source->edge_count; i++) cmp = strcmp(a->edges[i].slot, b->edges[i].slot);
    return cmp;
}

/* Called only within an initially equal shape, so degrees and slots agree. */
static int lg_target_order(const void *left, const void *right)
{
    const lg_item *a = left, *b = right;
    size_t i;
    for (i = 0; i < a->source->edge_count; i++) {
        int cmp = lg_size(a->targets[i], b->targets[i]);
        if (cmp) return cmp;
    }
    return 0;
}

static void *lg_allocate(size_t count, size_t stride)
{
    return count <= SIZE_MAX / stride ? calloc(count ? count : 1, stride) : NULL;
}

static int lg_fail(char *error, size_t capacity, const char *reason)
{
    if (error && capacity) snprintf(error, capacity, "%s", reason);
    return 0;
}

int sh_package_link_originals(const sh_package_link_node *nodes, size_t count,
    const unsigned char *local, unsigned char *out, char *error, size_t capacity)
{
    size_t *offsets = NULL, *cursor = NULL, *incoming = NULL, *queue = NULL;
    size_t edges = 0, head = 0, tail = 0;
    int ok = 0;
    const char *reason = "invalid original resource graph";
    if (error && capacity) *error = 0;
    if (count == SIZE_MAX || (count && (!nodes || !local || !out))) goto done;
    for (size_t i = 0; i < count; i++) {
        if ((nodes[i].edge_count && !nodes[i].edges) || nodes[i].edge_count > SIZE_MAX - edges) goto done;
        edges += nodes[i].edge_count;
        for (size_t j = 0; j < nodes[i].edge_count; j++) if (nodes[i].edges[j].target >= count) goto done;
    }
    reason = "cannot allocate original resource dependency walk";
    offsets = lg_allocate(count + 1, sizeof(*offsets));
    cursor = lg_allocate(count, sizeof(*cursor));
    incoming = lg_allocate(edges, sizeof(*incoming));
    queue = lg_allocate(count, sizeof(*queue));
    if (!offsets || !cursor || !incoming || !queue) goto done;
    for (size_t i = 0; i < count; i++) for (size_t j = 0; j < nodes[i].edge_count; j++)
        offsets[nodes[i].edges[j].target + 1]++;
    for (size_t i = 0; i < count; i++) { offsets[i + 1] += offsets[i]; cursor[i] = offsets[i]; }
    for (size_t i = 0; i < count; i++) for (size_t j = 0; j < nodes[i].edge_count; j++)
        incoming[cursor[nodes[i].edges[j].target]++] = i;
    for (size_t i = 0; i < count; i++) {
        out[i] = local[i] != 0;
        if (!out[i]) queue[tail++] = i;
    }
    while (head < tail) {
        size_t node = queue[head++];
        for (size_t j = offsets[node]; j < offsets[node + 1]; j++) {
            size_t parent = incoming[j];
            if (out[parent]) { out[parent] = 0; queue[tail++] = parent; }
        }
    }
    ok = 1;
done:
    free(offsets); free(cursor); free(incoming); free(queue);
    if (!ok) {
        if (out && count < SIZE_MAX) memset(out, 0, count);
        return lg_fail(error, capacity, reason);
    }
    return 1;
}

static void lg_free(lg_work *work)
{
    free(work->items); free(work->edges); free(work->targets); free(work->classes);
    free(work->incoming); free(work->offsets); free(work->queue); free(work->groups);
}

static void lg_enqueue(lg_work *work, size_t group)
{
    if (!work->groups[group].queued && work->groups[group].count > 1) {
        work->groups[group].queued = 1;
        work->queue[(work->queue_begin + work->queue_count) % work->count] = group;
        work->queue_count++;
    }
}

/* Restore shape order to an earlier order in which some shapes changed: drop
 * both items of every descent below the kept prefix (a monotone stack), sort
 * the few dropped items and merge them back by binary search. The result is
 * in shape order like a full sort; only the arbitrary order of equal shapes
 * may differ. 0, with items untouched, when too many moved or memory ran out:
 * the caller sorts everything. */
static int lg_repair(lg_item *items, const lg_item *placed, size_t count)
{
    size_t *kept = lg_allocate(count, sizeof(*kept)), top = 0, dropped = 0, i, r, k = 0, at = 0;
    unsigned char *drop = lg_allocate(count, 1);
    lg_item *moved = NULL;
    int ok = 0;
    if (!kept || !drop) goto done;
    for (i = 0; i < count; i++) {
        if (top && lg_shape_order(&placed[kept[top - 1]], &placed[i]) > 0) {
            drop[kept[--top]] = 1; drop[i] = 1; dropped += 2;
            if (dropped > count / 8) goto done;
        } else kept[top++] = i;
    }
    if (!(moved = lg_allocate(dropped, sizeof(*moved)))) goto done;
    for (i = 0, r = 0; i < count; i++) if (drop[i]) moved[r++] = placed[i];
    if (dropped > 1) qsort(moved, dropped, sizeof(*moved), lg_shape_order);
    for (r = 0; r < dropped; r++) {
        size_t lo = k, hi = top;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (lg_shape_order(&placed[kept[mid]], &moved[r]) <= 0) lo = mid + 1; else hi = mid;
        }
        while (k < lo) items[at++] = placed[kept[k++]];
        items[at++] = moved[r];
    }
    while (k < top) items[at++] = placed[kept[k++]];
    ok = 1;
done:
    free(kept); free(drop); free(moved);
    return ok;
}

static int lg_prepare(lg_work *work, const sh_package_link_node *nodes, size_t count,
    sh_package_link_verify_equal verify, void *context, sh_package_link_order *order, char *error, size_t capacity)
{
    size_t i, j, edge_count = 0, at = 0, *cursor = NULL;
    if ((count && !nodes) || count == SIZE_MAX)
        return lg_fail(error, capacity, "invalid resource graph input");
    for (i = 0; i < count; i++) {
        const sh_package_link_node *node = &nodes[i];
        if (!node->family || !*node->family || !node->path || !*node->path ||
            (node->edge_count && !node->edges))
            return lg_fail(error, capacity, "resource graph needs complete typed identities");
        if (node->edge_count > SIZE_MAX - edge_count)
            return lg_fail(error, capacity, "resource graph size overflows");
        edge_count += node->edge_count;
        for (j = 0; j < node->edge_count; j++)
            if (!node->edges[j].slot || !*node->edges[j].slot || node->edges[j].target >= count)
                return lg_fail(error, capacity, "resource graph has an invalid binding");
    }
    work->count = count;
    work->items = lg_allocate(count, sizeof(*work->items));
    work->edges = lg_allocate(edge_count, sizeof(*work->edges));
    work->targets = lg_allocate(edge_count, sizeof(*work->targets));
    work->classes = lg_allocate(count, sizeof(*work->classes));
    work->incoming = lg_allocate(edge_count, sizeof(*work->incoming));
    work->offsets = lg_allocate(count + 1, sizeof(*work->offsets));
    work->queue = lg_allocate(count, sizeof(*work->queue));
    work->groups = lg_allocate(count, sizeof(*work->groups));
    cursor = lg_allocate(count, sizeof(*cursor));
    if (!work->items || !work->edges || !work->targets || !work->classes || !work->incoming ||
        !work->offsets || !work->queue || !work->groups || !cursor) {
        free(cursor); return lg_fail(error, capacity, "resource graph allocation failed");
    }
    for (i = 0; i < count; i++) {
        lg_item *item = &work->items[i];
        const sh_package_link_node *node = &nodes[i];
        int order = -1;
        *item = (lg_item){node, work->edges + at, work->targets + at, i};
        if (node->edge_count) memcpy(item->edges, node->edges, node->edge_count * sizeof(*item->edges));
        /* Slots already strictly in order are exactly what sorting would give
         * and repeat none; otherwise sort and check as before. */
        for (j = 1; order < 0 && j < node->edge_count; j++) order = strcmp(item->edges[j - 1].slot, item->edges[j].slot);
        if (order >= 0) {
            qsort(item->edges, node->edge_count, sizeof(*item->edges), lg_edge_order);
            for (j = 1; j < node->edge_count; j++) if (!strcmp(item->edges[j - 1].slot, item->edges[j].slot)) {
                free(cursor);
                if (error && capacity) snprintf(error, capacity, "resource graph repeats binding slot %s in %s %s",
                    item->edges[j].slot, node->family, node->path);
                return 0;
            }
        }
        for (j = 0; j < node->edge_count; j++) work->offsets[item->edges[j].target + 1]++;
        at += node->edge_count;
    }
    for (i = 0; i < count; i++) {
        work->offsets[i + 1] += work->offsets[i]; cursor[i] = work->offsets[i];
    }
    for (i = 0; i < count; i++) for (j = 0; j < nodes[i].edge_count; j++)
        work->incoming[cursor[nodes[i].edges[j].target]++] = i;
    free(cursor);
    int sorted = 0, repaired = 0;
    /* equal[i]: items i - 1 and i have one shape (known when an earlier order
     * was reused unchanged; otherwise grouping compares). */
    unsigned char *equal = NULL;
    if (order && order->items && order->count == count && count > 1) {
        /* Place items in the earlier order when it is a permutation, then
         * keep it if every adjacent pair is still in shape order, or repair
         * it when a few shapes moved. */
        lg_item *placed = lg_allocate(count, sizeof(*placed));
        unsigned char *seen = lg_allocate(count, 1);
        equal = lg_allocate(count, 1);
        if (placed && seen && equal) {
            int permutation = 1;
            for (i = 0; permutation && i < count; i++) {
                size_t index = order->items[i];
                if (index >= count || seen[index]) permutation = 0;
                else { seen[index] = 1; placed[i] = work->items[index]; }
            }
            sorted = permutation;
            for (i = 1; sorted && i < count; i++) {
                int cmp = lg_shape_order(&placed[i - 1], &placed[i]);
                if (cmp > 0) sorted = 0; else equal[i] = !cmp;
            }
            if (sorted) memcpy(work->items, placed, count * sizeof(*placed));
            else if (permutation) repaired = lg_repair(work->items, placed, count);
        }
        free(placed); free(seen);
        if (!sorted) { free(equal); equal = NULL; }
    }
    if (count > 1 && !sorted && !repaired) qsort(work->items, count, sizeof(*work->items), lg_shape_order);
    if (order) {
        size_t *items = order->count == count && order->items ? order->items :
            lg_allocate(count, sizeof(*items));
        if (items) {
            for (i = 0; i < count; i++) items[i] = work->items[i].index;
            if (items != order->items) { free(order->items); order->items = items; }
            order->count = count;
        } else { free(order->items); order->items = NULL; order->count = 0; }
    }
    for (i = 0; i < count;) {
        size_t end = i + 1, group = work->group_count++;
        /* Shape equality is transitive: a run of equal neighbours is a group. */
        while (end < count && (equal ? equal[end] : !lg_shape_order(&work->items[i], &work->items[end]))) end++;
        if (verify) for (j = i + 1; j < end; j++)
            if (!verify(context, work->items[i].index, work->items[j].index, error, capacity)) {
                if (error && capacity && !*error)
                    lg_fail(error, capacity, "resource fingerprint match failed exact verification");
                free(equal); return 0;
            }
        work->groups[group] = (lg_group){i, end - i, 0};
        for (j = i; j < end; j++) work->classes[work->items[j].index] = group;
        lg_enqueue(work, group); i = end;
    }
    free(equal); return 1;
}

static void lg_refine(lg_work *work, size_t group)
{
    size_t start = work->groups[group].start, end = start + work->groups[group].count;
    size_t i, j, largest = start, largest_count = 0;
    work->groups[group].queued = 0;
    work->examined += end - start;
    for (i = start; i < end; i++) {
        lg_item *item = &work->items[i];
        for (j = 0; j < item->source->edge_count; j++) item->targets[j] = work->classes[item->edges[j].target];
    }
    qsort(work->items + start, end - start, sizeof(*work->items), lg_target_order);
    for (i = start; i < end;) {
        size_t next = i + 1;
        while (next < end && !lg_target_order(&work->items[i], &work->items[next])) next++;
        if (next - i > largest_count) { largest = i; largest_count = next - i; }
        i = next;
    }
    if (largest_count == end - start) return;
    /* Keep the largest partition's ID. Only smaller parts change identity,
     * bounding how often each node's incoming edges need reconsideration. */
    work->groups[group].start = largest; work->groups[group].count = largest_count;
    for (i = start; i < end;) {
        size_t next = i + 1, split;
        while (next < end && !lg_target_order(&work->items[i], &work->items[next])) next++;
        split = i == largest ? group : work->group_count++;
        if (i != largest) work->groups[split] = (lg_group){i, next - i, 0};
        for (j = i; j < next; j++) work->classes[work->items[j].index] = split;
        i = next;
    }
    /* Assign all parts before queueing predecessors: cycles and self edges
     * must see the new classes, including the just-created split groups. */
    for (i = start; i < end; i++) if (i < largest || i >= largest + largest_count) {
        size_t node = work->items[i].index;
        for (j = work->offsets[node]; j < work->offsets[node + 1]; j++)
            lg_enqueue(work, work->classes[work->incoming[j]]);
    }
}

void sh_package_link_classes_free(sh_package_link_classes *classes)
{
    if (classes) { free(classes->nodes); free(classes->representatives); free(classes->identities); memset(classes, 0, sizeof(*classes)); }
}

void sh_package_link_order_free(sh_package_link_order *order)
{
    if (order) { free(order->items); order->items = NULL; order->count = 0; }
}
int sh_package_link_equivalence_verified(const sh_package_link_node *nodes, size_t count,
    sh_package_link_verify_equal verify, void *context,
    sh_package_link_classes *out, char *error, size_t capacity)
{
    return sh_package_link_equivalence_ordered(nodes, count, verify, context, NULL, out, error, capacity);
}
int sh_package_link_equivalence_ordered(const sh_package_link_node *nodes, size_t count,
    sh_package_link_verify_equal verify, void *context, sh_package_link_order *order,
    sh_package_link_classes *out, char *error, size_t capacity)
{
    lg_work work = {0};
    int ok = 0;
    size_t i;
    if (error && capacity) error[0] = 0;
    if (!out) return lg_fail(error, capacity, "resource graph needs an output");
    sh_package_link_classes_free(out);
    if (!lg_prepare(&work, nodes, count, verify, context, order, error, capacity)) goto done;
    while (work.queue_count) {
        size_t group = work.queue[work.queue_begin];
        work.queue_begin = (work.queue_begin + 1) % work.count; work.queue_count--;
        lg_refine(&work, group);
    }
    out->representatives = lg_allocate(work.group_count, sizeof(*out->representatives));
    if (!out->representatives) { lg_fail(error, capacity, "resource graph allocation failed"); goto done; }
    for (i = 0; i < work.group_count; i++) out->representatives[i] = work.items[work.groups[i].start].index;
    out->nodes = work.classes; work.classes = NULL;
    out->node_count = count; out->count = work.group_count; out->examined = work.examined;
    ok = 1;
done:
    lg_free(&work);
    if (!ok) sh_package_link_classes_free(out);
    return ok;
}

int sh_package_link_equivalence(const sh_package_link_node *nodes, size_t count,
    sh_package_link_classes *out, char *error, size_t capacity)
{
    return sh_package_link_equivalence_verified(nodes, count, NULL, NULL, out, error, capacity);
}
