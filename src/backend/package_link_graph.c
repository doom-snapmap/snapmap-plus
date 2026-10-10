#include "package_link_graph.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    size_t count, group_count, queue_begin, queue_count, examined, reused, retained_held, retained_live;
} lg_work;

static int lg_size(size_t a, size_t b) { return a < b ? -1 : a > b; }
static unsigned long long lg_now_us(void)
{
    struct timespec now;
    return timespec_get(&now, TIME_UTC) ? (unsigned long long)now.tv_sec * 1000000u + (unsigned long long)now.tv_nsec / 1000u : 0;
}

static int lg_edge_order(const void *left, const void *right)
{
    const sh_package_link_edge *a = left, *b = right;
    return strcmp(a->slot, b->slot);
}

/* Equal pointers are equal strings: shared (interned) slots and names skip
 * the character comparison; the order is strcmp's either way. */
static int lg_text_order(const char *a, const char *b) { return a == b ? 0 : strcmp(a, b); }
static int lg_shape_order(const void *left, const void *right)
{
    const lg_item *a = left, *b = right;
    int cmp = lg_text_order(a->source->family, b->source->family);
    size_t i;
    if (!cmp) cmp = lg_text_order(a->source->path, b->source->path);
    if (!cmp) cmp = a->source->length < b->source->length ? -1 : a->source->length > b->source->length;
    if (!cmp) cmp = memcmp(a->source->content, b->source->content, 32);
    if (!cmp) cmp = memcmp(a->source->semantics, b->source->semantics, 32);
    if (!cmp) cmp = lg_size(a->source->edge_count, b->source->edge_count);
    for (i = 0; !cmp && i < a->source->edge_count; i++) cmp = lg_text_order(a->edges[i].slot, b->edges[i].slot);
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

#ifdef SH_PACKAGE_LINK_GRAPH_TESTING
/* Tests only: count allocations, and refuse the n-th next one (0 never). */
static size_t lg_allocations, lg_refuse_at;
void sh_package_link_graph_test_refuse(size_t nth) { lg_refuse_at = nth; lg_allocations = 0; }
size_t sh_package_link_graph_test_allocations(void) { return lg_allocations; }
int sh_package_link_graph_test_refused(void) { return ++lg_allocations == lg_refuse_at; }
#define lg_refused sh_package_link_graph_test_refused
#else
#define lg_refused() 0
#endif
static void *lg_allocate(size_t count, size_t stride)
{
    return count <= SIZE_MAX / stride && !lg_refused() ? calloc(count ? count : 1, stride) : NULL;
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

/* Put an earlier order (placed, a permutation) back in shape order: keep a
 * monotone prefix, dropping both items of every descent below it, then sort
 * the few dropped items and merge them back by binary search. The result is
 * in shape order like a full sort; only the arbitrary order of equal shapes
 * may differ. equal[i] (i > 0) records whether items i - 1 and i have one
 * shape, reusing the comparison already made wherever two kept items stay
 * adjacent. 2: already in order; 1: repaired; 0, with items untouched, when
 * too many moved or memory ran out, and the caller sorts everything. */
/* Shape comparison for lg_repair: retained (non-NULL) knows the earlier
 * order's shape runs of nodes that compared exactly unchanged, whose order is
 * the one their shapes had; every other pair compares shapes. */
typedef struct lg_runs { const unsigned char *changed; const size_t *runs; } lg_runs;
static int lg_repair_order(const lg_runs *retained, const lg_item *a, const lg_item *b)
{
    if (retained && !retained->changed[a->index] && !retained->changed[b->index])
        return lg_size(retained->runs[a->index], retained->runs[b->index]);
    return lg_shape_order(a, b);
}
static void lg_emit(const lg_runs *retained, const lg_item *items, size_t at, size_t from, size_t previous,
    const unsigned char *same, unsigned char *equal)
{
    if (at) equal[at] = (unsigned char)(from != SIZE_MAX && previous != SIZE_MAX && from == previous + 1 ?
        same[from] : !lg_repair_order(retained, &items[at - 1], &items[at]));
}
static int lg_repair(lg_item *items, const lg_item *placed, size_t count, unsigned char *equal, const lg_runs *retained)
{
    size_t *kept = lg_allocate(count, sizeof(*kept)), top = 0, dropped = 0, i, r, k = 0, at = 0, previous = SIZE_MAX;
    unsigned char *drop = lg_allocate(count, 1), *same = lg_allocate(count, 1); /* same[t]: kept t - 1 and t */
    lg_item *moved = NULL;
    int status = 0;
    if (!kept || !drop || !same) goto done;
    for (i = 0; i < count; i++) {
        int cmp = top ? lg_repair_order(retained, &placed[kept[top - 1]], &placed[i]) : -1;
        if (cmp > 0) {
            drop[kept[--top]] = 1; drop[i] = 1; dropped += 2;
            if (dropped > count / 8) goto done;
        } else { same[top] = (unsigned char)!cmp; kept[top++] = i; }
    }
    if (!dropped) {
        memcpy(items, placed, count * sizeof(*placed));
        for (i = 1; i < count; i++) equal[i] = same[i];
        status = 2; goto done;
    }
    if (!(moved = lg_allocate(dropped, sizeof(*moved)))) goto done;
    for (i = 0, r = 0; i < count; i++) if (drop[i]) moved[r++] = placed[i];
    if (dropped > 1) qsort(moved, dropped, sizeof(*moved), lg_shape_order);
    for (r = 0; r < dropped; r++) {
        size_t lo = k, hi = top;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (lg_repair_order(retained, &placed[kept[mid]], &moved[r]) <= 0) lo = mid + 1; else hi = mid;
        }
        for (; k < lo; k++, at++) {
            items[at] = placed[kept[k]]; lg_emit(retained, items, at, k, previous, same, equal); previous = k;
        }
        items[at] = moved[r]; lg_emit(retained, items, at, SIZE_MAX, previous, same, equal); previous = SIZE_MAX; at++;
    }
    for (; k < top; k++, at++) {
        items[at] = placed[kept[k]]; lg_emit(retained, items, at, k, previous, same, equal); previous = k;
    }
    status = 1;
done:
    free(kept); free(drop); free(same); free(moved);
    return status;
}

static int lg_order_groups(lg_work *work, sh_package_link_verify_equal verify, void *context,
    sh_package_link_order *order, const lg_runs *retained, char *error, size_t capacity);
static int lg_prepare_fresh(lg_work *work, const sh_package_link_node *nodes, size_t count,
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
    return lg_order_groups(work, verify, context, order, NULL, error, capacity);
}

/* Order items by shape (reusing an earlier order when it still sorts them),
 * write that order back, then form and verify the initial shape groups. */
static int lg_order_groups(lg_work *work, sh_package_link_verify_equal verify, void *context,
    sh_package_link_order *order, const lg_runs *retained, char *error, size_t capacity)
{
    size_t i, j, count = work->count;
    int ordered = 0;
    /* equal[i]: items i - 1 and i have one shape (known when an earlier order
     * was reused or repaired; otherwise grouping compares). */
    unsigned char *equal = NULL;
    if (order && order->items && order->count == count && count > 1) {
        /* Place items in the earlier order when it is a permutation, then keep
         * it as it is, or repair it when a few shapes moved. */
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
            if (permutation) ordered = lg_repair(work->items, placed, count, equal, retained) != 0;
        }
        free(placed); free(seen);
        if (!ordered) { free(equal); equal = NULL; }
    }
    if (count > 1 && !ordered) qsort(work->items, count, sizeof(*work->items), lg_shape_order);
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

/* Retained preparation (see sh_package_link_order): exact owned copies of the
 * last call's node shapes. Texts are interned by content, so equal owned texts
 * share one pointer; each is preceded by its taken size and a mark bit used to
 * count the distinct texts the current shapes reference. Blocks are never
 * freed individually: replaced shapes and texts stay until a rebuild, which a
 * call starts when held bytes exceed twice the bytes the last call's shapes
 * used plus LG_SLACK. */
typedef struct lg_block { struct lg_block *next; size_t used, capacity, pad; } lg_block;
struct sh_package_link_prepared {
    sh_package_link_node *nodes; /* owned shapes: interned texts, slot-sorted edges */
    size_t **ranks;              /* input binding -> sorted position; NULL when the input was sorted */
    size_t *runs;                /* each node's shape group in the last order */
    size_t count, capacity;
    size_t held, live;           /* bytes held (blocks, arrays, table) / used by the current shapes */
    const char **texts;
    size_t text_count, text_capacity;
    lg_block *blocks;
    int ordered;                 /* runs describe the last call's order */
};
enum { LG_SEEN = 1024 };
#define LG_BLOCK ((size_t)4 << 20)
#define LG_SLACK (2 * LG_BLOCK)
#define LG_NODE_BYTES (sizeof(sh_package_link_node) + sizeof(size_t *) + sizeof(size_t))
typedef struct lg_seen { const char *in, *own; } lg_seen;

static void lg_prepared_free(struct sh_package_link_prepared *p)
{
    if (!p) return;
    while (p->blocks) { lg_block *next = p->blocks->next; free(p->blocks); p->blocks = next; }
    free(p->nodes); free(p->ranks); free(p->runs); free((void *)p->texts); free(p);
}
static void *lg_take(struct sh_package_link_prepared *p, size_t bytes)
{
    if (!bytes || bytes > SIZE_MAX - sizeof(lg_block) - 15u) return NULL;
    bytes = (bytes + 15u) & ~(size_t)15u;
    if (!p->blocks || p->blocks->capacity - p->blocks->used < bytes) {
        size_t size = bytes > LG_BLOCK ? bytes : LG_BLOCK;
        lg_block *block = lg_refused() ? NULL : malloc(sizeof(*block) + size);
        if (!block) return NULL;
        *block = (lg_block){p->blocks, 0, size, 0}; p->blocks = block;
        p->held += sizeof(*block) + size;
    }
    void *at = (unsigned char *)(p->blocks + 1) + p->blocks->used;
    p->blocks->used += bytes; return at;
}
static size_t lg_text_hash(const char *text)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *text; text++) { hash ^= (unsigned char)*text; hash *= UINT64_C(1099511628211); }
    return (size_t)(hash ^ hash >> 32);
}
/* The one owned copy of text; NULL on allocation failure. */
static const char *lg_intern(struct sh_package_link_prepared *p, const char *text)
{
    size_t at, i;
    if (p->text_count + 1 > p->text_capacity / 2) {
        size_t capacity = p->text_capacity ? p->text_capacity * 2 : 4096;
        const char **grown = lg_allocate(capacity, sizeof(*grown));
        if (!grown) return NULL;
        p->held += (capacity - p->text_capacity) * sizeof(*grown);
        for (i = 0; i < p->text_capacity; i++) if (p->texts[i]) {
            for (at = lg_text_hash(p->texts[i]) & (capacity - 1); grown[at]; at = (at + 1) & (capacity - 1)) {}
            grown[at] = p->texts[i];
        }
        free((void *)p->texts); p->texts = grown; p->text_capacity = capacity;
    }
    for (at = lg_text_hash(text) & (p->text_capacity - 1); p->texts[at]; at = (at + 1) & (p->text_capacity - 1))
        if (!strcmp(p->texts[at], text)) return p->texts[at];
    size_t length = strlen(text), *head = length < SIZE_MAX / 4 ? lg_take(p, sizeof(size_t) + length + 1) : NULL;
    if (!head) return NULL;
    *head = ((sizeof(size_t) + length + 1 + 15u) & ~(size_t)15u) << 1;
    char *own = (char *)(head + 1);
    memcpy(own, text, length + 1);
    p->texts[at] = own; p->text_count++;
    return own;
}
/* Bytes an owned text uses the first time this count sees it, with its share
 * of the interning table (at most four slots per text after growth), else 0. */
static size_t lg_mark(const char *own)
{
    size_t *head = (size_t *)own - 1;
    if (*head & 1u) return 0;
    *head |= 1u; return (*head >> 1) + 4 * sizeof(const char *);
}
/* Within one call an input pointer names fixed text, and owned texts are
 * unique by content, so a confirmed (in, own) pair answers later queries. */
static int lg_same_text(lg_seen *seen, const char *in, const char *own)
{
    lg_seen *entry = &seen[(((uintptr_t)in >> 4) ^ ((uintptr_t)in >> 14)) & (LG_SEEN - 1)];
    if (entry->in == in) return entry->own == own;
    if (strcmp(in, own)) return 0;
    entry->in = in; entry->own = own; return 1;
}
static int lg_unchanged(const struct sh_package_link_prepared *p, lg_seen *seen, size_t i,
    const sh_package_link_node *node)
{
    const sh_package_link_node *own = &p->nodes[i];
    const size_t *rank = p->ranks[i];
    size_t j;
    if (node->length != own->length || node->edge_count != own->edge_count ||
        memcmp(node->content, own->content, 32) || memcmp(node->semantics, own->semantics, 32) ||
        !lg_same_text(seen, node->family, own->family) || strcmp(node->path, own->path)) return 0;
    for (j = 0; j < node->edge_count; j++) {
        const sh_package_link_edge *edge = &own->edges[rank ? rank[j] : j];
        if (node->edges[j].target != edge->target || !lg_same_text(seen, node->edges[j].slot, edge->slot)) return 0;
    }
    return 1;
}
typedef struct lg_ranked { sh_package_link_edge edge; size_t at; } lg_ranked;
static int lg_ranked_order(const void *left, const void *right)
{
    const lg_ranked *a = left, *b = right;
    return strcmp(a->edge.slot, b->edge.slot);
}
/* Copy node i's shape: 1, 0 with *repeated naming a repeated slot, -1 when
 * memory ran out. Bindings are sorted exactly as the fresh preparation sorts
 * them; ranks keep the input order its incoming lists follow. */
static int lg_store(struct sh_package_link_prepared *p, size_t i, const sh_package_link_node *node,
    const char **repeated)
{
    sh_package_link_node *own = &p->nodes[i];
    size_t n = node->edge_count, j;
    int order = -1;
    *own = (sh_package_link_node){0};
    p->ranks[i] = NULL;
    own->family = lg_intern(p, node->family); own->path = lg_intern(p, node->path);
    sh_package_link_edge *edges = n ? lg_take(p, n * sizeof(*edges)) : NULL;
    if (!own->family || !own->path || (n && !edges)) return -1;
    for (j = 1; order < 0 && j < n; j++) order = strcmp(node->edges[j - 1].slot, node->edges[j].slot);
    if (order < 0) for (j = 0; j < n; j++) edges[j] = node->edges[j];
    else {
        lg_ranked *sorted = lg_allocate(n, sizeof(*sorted));
        size_t *rank = lg_take(p, n * sizeof(*rank));
        if (!sorted || !rank) { free(sorted); return -1; }
        for (j = 0; j < n; j++) sorted[j] = (lg_ranked){node->edges[j], j};
        qsort(sorted, n, sizeof(*sorted), lg_ranked_order);
        for (j = 0; j < n; j++) {
            if (j && !strcmp(sorted[j - 1].edge.slot, sorted[j].edge.slot)) {
                *repeated = sorted[j].edge.slot; free(sorted); return 0;
            }
            edges[j] = sorted[j].edge; rank[sorted[j].at] = j;
        }
        free(sorted);
        p->ranks[i] = rank;
    }
    for (j = 0; j < n; j++) if (!(edges[j].slot = lg_intern(p, edges[j].slot))) return -1;
    own->length = node->length;
    memcpy(own->content, node->content, 32); memcpy(own->semantics, node->semantics, 32);
    own->edges = edges; own->edge_count = n;
    return 1;
}
/* The fresh preparation's checks, items, incoming lists, order and groups,
 * from retained shapes. 1 done; 0 failed (input rejected, or memory ran out
 * after the order was written); -1 memory ran out before anything observable
 * changed, so the caller prepares fresh. Any failure discards the retained
 * copy; the caller then resets work. */
static int lg_prepare_retained(lg_work *work, const sh_package_link_node *nodes, size_t count,
    sh_package_link_verify_equal verify, void *context, sh_package_link_order *order, char *error, size_t capacity)
{
    struct sh_package_link_prepared *p = order->prepared;
    size_t i, j, edge_count = 0, at = 0, *cursor = NULL, previous;
    unsigned char *changed = NULL;
    lg_seen *seen = NULL;
    const char *repeated = NULL;
    size_t repeated_node = SIZE_MAX;
    int status = -1;
    if ((count && !nodes) || count == SIZE_MAX) return lg_fail(error, capacity, "invalid resource graph input");
    /* Bound what replaced shapes and texts leave behind. */
    if (p && p->held > 2 * p->live + LG_SLACK) { lg_prepared_free(p); order->prepared = p = NULL; }
    if (!p && !(order->prepared = p = lg_allocate(1, sizeof(*p)))) return -1;
    previous = p->count;
    if (count > p->capacity) {
        size_t k;
        sh_package_link_node *grown_nodes = count <= SIZE_MAX / LG_NODE_BYTES && !lg_refused() ?
            realloc(p->nodes, count * sizeof(*grown_nodes)) : NULL;
        if (grown_nodes) p->nodes = grown_nodes;
        size_t **grown_ranks = grown_nodes ? realloc(p->ranks, count * sizeof(*grown_ranks)) : NULL;
        if (grown_ranks) p->ranks = grown_ranks;
        size_t *grown_runs = grown_ranks ? realloc(p->runs, count * sizeof(*grown_runs)) : NULL;
        if (!grown_runs) goto discard;
        p->runs = grown_runs;
        for (k = p->capacity; k < count; k++) p->ranks[k] = NULL;
        p->held += (count - p->capacity) * LG_NODE_BYTES;
        p->capacity = count;
    }
    changed = lg_allocate(count, 1);
    seen = lg_allocate(LG_SEEN, sizeof(*seen));
    if (!changed || !seen) goto discard;
    for (i = 0; i < count; i++) {
        const sh_package_link_node *node = &nodes[i];
        if (!node->family || !*node->family || !node->path || !*node->path ||
            (node->edge_count && !node->edges)) {
            lg_fail(error, capacity, "resource graph needs complete typed identities"); status = 0; goto discard;
        }
        if (node->edge_count > SIZE_MAX - edge_count) {
            lg_fail(error, capacity, "resource graph size overflows"); status = 0; goto discard;
        }
        edge_count += node->edge_count;
        for (j = 0; j < node->edge_count; j++)
            if (!node->edges[j].slot || !*node->edges[j].slot || node->edges[j].target >= count) {
                lg_fail(error, capacity, "resource graph has an invalid binding"); status = 0; goto discard;
            }
        if (i < previous && lg_unchanged(p, seen, i, node)) { work->reused++; continue; }
        changed[i] = 1;
        if (repeated) continue; /* rejected below unless a later node fails a check above */
        int stored = lg_store(p, i, node, &repeated);
        if (stored < 0) goto discard;
        if (!stored) repeated_node = i;
    }
    if (repeated) {
        if (error && capacity) snprintf(error, capacity, "resource graph repeats binding slot %s in %s %s",
            repeated, nodes[repeated_node].family, nodes[repeated_node].path);
        status = 0; goto discard;
    }
    p->count = count;
    p->live = count * LG_NODE_BYTES;
    for (i = 0; i < p->text_capacity; i++) if (p->texts[i]) *((size_t *)p->texts[i] - 1) &= ~(size_t)1u;
    work->count = count;
    work->items = lg_allocate(count, sizeof(*work->items));
    work->targets = lg_allocate(edge_count, sizeof(*work->targets));
    work->classes = lg_allocate(count, sizeof(*work->classes));
    work->incoming = lg_allocate(edge_count, sizeof(*work->incoming));
    work->offsets = lg_allocate(count + 1, sizeof(*work->offsets));
    work->queue = lg_allocate(count, sizeof(*work->queue));
    work->groups = lg_allocate(count, sizeof(*work->groups));
    cursor = lg_allocate(count, sizeof(*cursor));
    if (!work->items || !work->targets || !work->classes || !work->incoming ||
        !work->offsets || !work->queue || !work->groups || !cursor) goto discard;
    for (i = 0; i < count; i++) {
        const sh_package_link_node *own = &p->nodes[i];
        work->items[i] = (lg_item){own, (sh_package_link_edge *)own->edges, work->targets + at, i};
        p->live += lg_mark(own->family) + lg_mark(own->path) +
            ((own->edge_count * sizeof(*own->edges) + 15u) & ~(size_t)15u) +
            (p->ranks[i] ? (own->edge_count * sizeof(size_t) + 15u) & ~(size_t)15u : 0);
        for (j = 0; j < own->edge_count; j++) {
            work->offsets[own->edges[j].target + 1]++; p->live += lg_mark(own->edges[j].slot);
        }
        at += own->edge_count;
    }
    for (i = 0; i < count; i++) {
        work->offsets[i + 1] += work->offsets[i]; cursor[i] = work->offsets[i];
    }
    /* Each incoming list holds its predecessors in node order, whatever the
     * order of a node's own bindings: the fresh preparation's lists. */
    for (i = 0; i < count; i++) for (j = 0; j < p->nodes[i].edge_count; j++)
        work->incoming[cursor[p->nodes[i].edges[j].target]++] = i;
    free(cursor); cursor = NULL;
    /* Unchanged nodes keep their earlier shape order when the node set kept its
     * size (the fresh path's only reuse case): their runs answer comparisons
     * among them; every other comparison compares shapes. */
    lg_runs runs = {changed, p->runs};
    int ordered = p->ordered && previous == count;
    p->ordered = 0;
    if (!lg_order_groups(work, verify, context, order, ordered ? &runs : NULL, error, capacity)) {
        status = 0; goto discard;
    }
    memcpy(p->runs, work->classes, count * sizeof(*p->runs));
    p->ordered = 1;
    work->retained_held = p->held; work->retained_live = p->live;
    free(changed); free(seen);
    return 1;
discard:
    free(cursor); free(changed); free(seen);
    lg_prepared_free(order->prepared); order->prepared = NULL;
    return status;
}
static int lg_prepare(lg_work *work, const sh_package_link_node *nodes, size_t count,
    sh_package_link_verify_equal verify, void *context, sh_package_link_order *order, char *error, size_t capacity)
{
    if (order && !order->unretained) {
        int status = lg_prepare_retained(work, nodes, count, verify, context, order, error, capacity);
        if (status >= 0) return status;
        lg_free(work); *work = (lg_work){0};
        if (error && capacity) error[0] = 0;
    }
    return lg_prepare_fresh(work, nodes, count, verify, context, order, error, capacity);
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

void sh_package_link_order_discard(sh_package_link_order *order)
{
    if (order) { lg_prepared_free(order->prepared); order->prepared = NULL; }
}
void sh_package_link_order_free(sh_package_link_order *order)
{
    if (order) {
        free(order->items); order->items = NULL; order->count = 0;
        lg_prepared_free(order->prepared); order->prepared = NULL;
    }
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
    unsigned long long began = lg_now_us(), shaped;
    if (!lg_prepare(&work, nodes, count, verify, context, order, error, capacity)) goto done;
    shaped = lg_now_us();
    while (work.queue_count) {
        size_t group = work.queue[work.queue_begin];
        work.queue_begin = (work.queue_begin + 1) % work.count; work.queue_count--;
        lg_refine(&work, group);
    }
    out->representatives = lg_allocate(work.group_count, sizeof(*out->representatives));
    if (!out->representatives) { lg_fail(error, capacity, "resource graph allocation failed"); goto done; }
    for (i = 0; i < work.group_count; i++) out->representatives[i] = work.items[work.groups[i].start].index;
    out->nodes = work.classes; work.classes = NULL;
    out->node_count = count; out->count = work.group_count; out->examined = work.examined; out->reused = work.reused;
    out->retained_held = work.retained_held; out->retained_live = work.retained_live;
    out->shapes_us = shaped - began; out->refine_us = lg_now_us() - shaped;
    ok = 1;
done:
    lg_free(&work);
    if (!ok) { sh_package_link_classes_free(out); sh_package_link_order_discard(order); }
    return ok;
}

int sh_package_link_equivalence(const sh_package_link_node *nodes, size_t count,
    sh_package_link_classes *out, char *error, size_t capacity)
{
    return sh_package_link_equivalence_verified(nodes, count, NULL, NULL, out, error, capacity);
}
