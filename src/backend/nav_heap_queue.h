/* Bound the native path-search queue while preserving its best candidates. */
#ifndef SH_NAV_HEAP_QUEUE_H
#define SH_NAV_HEAP_QUEUE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct sh_nav_heap_node {
    uint16_t cost, children, index, padding;
} sh_nav_heap_node;

typedef struct sh_nav_heap_list {
    sh_nav_heap_node *nodes;
    int count, capacity;
    uint32_t policy, reserved;
} sh_nav_heap_list;

enum { SH_NAV_HEAP_INVALID, SH_NAV_HEAP_NATIVE, SH_NAV_HEAP_COALESCED };

static int sh_nav_heap_index_order(const void *a, const void *b)
{
    const sh_nav_heap_node *x = a, *y = b;
    if (x->index != y->index) return (int)x->index - (int)y->index;
    return (int)x->cost - (int)y->cost;
}

static int sh_nav_heap_cost_order(const void *a, const void *b)
{
    const sh_nav_heap_node *x = a, *y = b;
    if (x->cost != y->cost) return (int)x->cost - (int)y->cost;
    return (int)x->index - (int)y->index;
}

/* The engine sizes this complete binary tree for the cluster's reach/portal
 * count, but queues another copy when a candidate improves. Its push has no
 * capacity check. Normally leave its ordering untouched. At capacity, remove
 * redundant copies and incorporate the pending improvement in place.
 *
 * Indices are one-based and cannot exceed the number of slots. Therefore a
 * full queue either already contains the pending index or contains duplicates:
 * coalescing always fits every distinct candidate without allocating or
 * changing the engine list's ownership, count or capacity. */
static int sh_nav_heap_admit(sh_nav_heap_list *list, sh_nav_heap_node pending)
{
    sh_nav_heap_node *nodes;
    size_t i, used = 0, unique = 0, slots;
    int found = 0;
    if (!list || !list->nodes || list->count < 1 || list->count > list->capacity ||
        !pending.index || (unsigned)pending.index > (unsigned)list->count)
        return SH_NAV_HEAP_INVALID;
    nodes = list->nodes;
    slots = (size_t)list->count;
    if (!nodes[0].index ||
        ((size_t)nodes[0].children + 1 < slots && nodes[0].children < UINT16_MAX - 1))
        return SH_NAV_HEAP_NATIVE;

    /* Validate before moving anything. Keep refusal non-destructive. */
    for (i = 0; i < slots; ++i)
        if ((size_t)nodes[i].index > slots) return SH_NAV_HEAP_INVALID;
    for (i = 0; i < slots; ++i)
        if (nodes[i].index) nodes[used++] = nodes[i];
    qsort(nodes, used, sizeof *nodes, sh_nav_heap_index_order);
    for (i = 0; i < used; ++i) {
        if (unique && nodes[unique - 1].index == nodes[i].index) continue;
        nodes[unique++] = nodes[i];
        if (nodes[unique - 1].index == pending.index) {
            if (pending.cost < nodes[unique - 1].cost) nodes[unique - 1] = pending;
            found = 1;
        }
    }
    if (!found) nodes[unique++] = pending;
    qsort(nodes, unique, sizeof *nodes, sh_nav_heap_cost_order);
    memset(nodes + unique, 0, (slots - unique) * sizeof *nodes);
    /* Cost order is a valid complete min-heap. Restore exact subtree counts
     * consumed by the engine's push/pop, including their empty index sentinel. */
    for (i = unique; i-- > 0;) {
        size_t left = 2 * i + 1, right = left + 1;
        unsigned children = 0;
        if (left < unique) children += 1u + nodes[left].children;
        if (right < unique) children += 1u + nodes[right].children;
        nodes[i].children = (uint16_t)children;
    }
    return SH_NAV_HEAP_COALESCED;
}

#endif
