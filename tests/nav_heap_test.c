/* Queue capacity, duplicate improvements and engine metadata preservation. */
#include <stdio.h>
#include <stddef.h>
#include "../src/backend/nav_heap_queue.h"

static int failed, checks;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void verify(sh_nav_heap_list *q, unsigned used)
{
    unsigned i;
    CHECK(sizeof(sh_nav_heap_node) == 8);
    CHECK(offsetof(sh_nav_heap_list, count) == 8);
    CHECK(offsetof(sh_nav_heap_list, policy) == 16);
    for (i = 0; i < (unsigned)q->count; ++i) {
        unsigned left = 2 * i + 1, right = left + 1, children = 0;
        CHECK((q->nodes[i].index != 0) == (i < used));
        if (i >= used) continue;
        if (left < used) {
            CHECK(q->nodes[i].cost <= q->nodes[left].cost);
            children += 1 + q->nodes[left].children;
        }
        if (right < used) {
            CHECK(q->nodes[i].cost <= q->nodes[right].cost);
            children += 1 + q->nodes[right].children;
        }
        CHECK(q->nodes[i].children == children);
    }
}

static void full_queue(unsigned slots, int duplicate)
{
    sh_nav_heap_node *backing = calloc(slots + 1, sizeof *backing);
    sh_nav_heap_list q = {backing, (int)slots, (int)slots, 0x03800000, 0};
    sh_nav_heap_node pending = {1, 0, (uint16_t)slots, 0};
    unsigned i, used = slots;
    CHECK(backing != NULL);
    if (!backing) return;
    for (i = 0; i < slots; ++i) {
        backing[i].index = (uint16_t)(duplicate ? 1 : i + 1);
        backing[i].cost = (uint16_t)(100 + i % 50000);
    }
    backing[0].children = (uint16_t)(slots - 1);
    memset(backing + slots, 0xa5, sizeof *backing);
    CHECK(sh_nav_heap_admit(&q, pending) == SH_NAV_HEAP_COALESCED);
    if (duplicate) used = slots == 1 ? 1 : 2;
    CHECK(backing[0].index == pending.index && backing[0].cost == 1);
    CHECK(q.nodes == backing && q.count == (int)slots && q.capacity == (int)slots);
    CHECK(q.policy == 0x03800000);
    CHECK(backing[slots].index == 0xa5a5 && backing[slots].cost == 0xa5a5);
    verify(&q, used);
    free(backing);
}

int main(void)
{
    unsigned sizes[] = {1, 3, 7, 127, 2047, 65535}, i;
    sh_nav_heap_node nodes[7] = {{5, 0, 1, 0}};
    sh_nav_heap_list q = {nodes, 7, 7, 0x03800000, 0};
    sh_nav_heap_node pending = {1, 0, 2, 0};
    sh_nav_heap_node saved[7];
    memcpy(saved, nodes, sizeof nodes);
    CHECK(sh_nav_heap_admit(&q, pending) == SH_NAV_HEAP_NATIVE);
    CHECK(!memcmp(saved, nodes, sizeof nodes));
    pending.index = 8;
    CHECK(sh_nav_heap_admit(&q, pending) == SH_NAV_HEAP_INVALID);
    CHECK(!memcmp(saved, nodes, sizeof nodes));
    pending.index = 0;
    CHECK(sh_nav_heap_admit(&q, pending) == SH_NAV_HEAP_INVALID);
    pending.index = 1;
    q.capacity = 6;
    CHECK(sh_nav_heap_admit(&q, pending) == SH_NAV_HEAP_INVALID);
    CHECK(!memcmp(saved, nodes, sizeof nodes));
    CHECK(sh_nav_heap_admit(NULL, pending) == SH_NAV_HEAP_INVALID);
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        full_queue(sizes[i], 0);
        full_queue(sizes[i], 1);
    }
    printf("nav_heap_test: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
