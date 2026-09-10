/* Exercise process-heap admission, ownership and restoration with native doubles. */
#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "process_heap_scope.h"

static struct {
    unsigned char prefix[0x44];
    int heaps[32];
    int depth;
} allocator;
static DWORD owner_thread;
static int get_mode, push_mode, pop_mode, pushes, pops, allocations;
enum { GET_OK, GET_NULL, GET_FAULT, GET_UNREADABLE };
enum { PUSH_OK, PUSH_NOOP, PUSH_FAULT_BEFORE, PUSH_FAULT_AFTER, PUSH_WRONG_HEAP };
enum { POP_OK, POP_NOOP, POP_FAULT_BEFORE, POP_FAULT_AFTER };

static void fault(void) { RaiseException(0xe0000042, 0, 0, NULL); }
static void *get(void)
{
    if (get_mode == GET_NULL) return NULL;
    if (get_mode == GET_FAULT) fault();
    return get_mode == GET_UNREADABLE ? (void *)(uintptr_t)1 : &allocator;
}
static void push(void *self, int heap)
{
    assert(self == &allocator && heap == 0);
    pushes++;
    if (push_mode == PUSH_NOOP || GetCurrentThreadId() != owner_thread) return;
    if (push_mode == PUSH_FAULT_BEFORE) fault();
    assert(allocator.depth >= 0 && allocator.depth < 32);
    allocator.heaps[allocator.depth++] = push_mode == PUSH_WRONG_HEAP ? 2 : heap;
    if (push_mode == PUSH_FAULT_AFTER) fault();
}
static void pop(void *self)
{
    assert(self == &allocator && allocator.depth > 0);
    pops++;
    if (pop_mode == POP_FAULT_BEFORE) fault();
    if (pop_mode == POP_NOOP) return;
    allocator.depth--;
    if (pop_mode == POP_FAULT_AFTER) fault();
}
static const sh_process_heap_api api = {get, push, pop};

static void reset(void)
{
    memset(&allocator, 0, sizeof(allocator));
    assert((char *)&allocator.depth - (char *)&allocator == 0xc4);
    allocator.depth = 1;
    allocator.heaps[0] = 2;
    owner_thread = GetCurrentThreadId();
    get_mode = GET_OK; push_mode = PUSH_OK; pop_mode = POP_OK;
    pushes = pops = allocations = 0;
}

typedef struct block { int heap, alive; } block;
static block allocate(void)
{
    block value;
    allocations++;
    value.heap = allocator.depth ? allocator.heaps[allocator.depth - 1] : 0;
    value.alive = 1;
    return value;
}
static void release_map(block *value)
{
    if (value->heap == 2) value->alive = 0;
}

static void refused_entry(sh_process_heap_scope *scope)
{
    int before = allocations;
    if (sh_process_heap_enter(&api, scope)) {
        (void)allocate();
        assert(!"unexpected admission");
    }
    assert(allocations == before);
}

static DWORD WINAPI worker(LPVOID unused)
{
    sh_process_heap_scope scope;
    (void)unused;
    refused_entry(&scope);
    assert(!scope.active && allocator.depth == 1 && pops == 0);
    return 0;
}

static void test_binding(void)
{
    const uint8_t *base = (const uint8_t *)GetModuleHandleA(NULL);
    const char *names[] = {"MemLocalGet", "MemLocalPushHeap", "MemLocalPopHeap"};
    uintptr_t addresses[] = {(uintptr_t)get, (uintptr_t)push, (uintptr_t)pop};
    sig_result results[4] = {0};
    sh_process_heap_api bound;
    for (int i = 0; i < 3; i++) {
        results[i].name = names[i]; results[i].status = SIG_OK;
        results[i].addr = addresses[i];
        results[i].rva = (uint32_t)(addresses[i] - (uintptr_t)base);
    }
    assert(sh_process_heap_bind(&bound, results, 3, base));
    assert(bound.get == get && bound.push == push && bound.pop == pop);
    for (int i = 0; i < 3; i++) {
        results[i].status = SIG_OK_HOOKED;
        assert(!sh_process_heap_bind(&bound, results, 3, base));
        assert(!bound.get && !bound.push && !bound.pop);
        results[i].status = SIG_OK;
        results[i].rva++;
        assert(!sh_process_heap_bind(&bound, results, 3, base));
        results[i].rva--;
        results[3] = results[i];
        assert(!sh_process_heap_bind(&bound, results, 4, base));
    }
    assert(!sh_process_heap_bind(&bound, results, 2, base));
    assert(!sh_process_heap_bind(&bound, NULL, 0, base));
    assert(!sh_process_heap_bind(&bound, results, 3, NULL));
    assert(!sh_process_heap_bind(NULL, results, 3, base));
    assert(sh_process_heap_bind(&bound, results, 3, base));
}

static void test_success_and_nesting(void)
{
    sh_process_heap_scope outer, inner;
    block map, process, nested;
    reset();
    map = allocate();
    assert(sh_process_heap_enter(&api, &outer) && outer.active);
    process = allocate();
    assert(process.heap == 0 && allocator.depth == 2);
    assert(sh_process_heap_enter(&api, &inner));
    nested = allocate();
    assert(nested.heap == 0 && allocator.depth == 3);
    assert(!sh_process_heap_leave(&outer) && outer.active && pops == 0);
    assert(sh_process_heap_leave(&inner));
    assert(sh_process_heap_leave(&outer));
    assert(allocator.depth == 1 && allocator.heaps[0] == 2 && pushes == pops);
    assert(sh_process_heap_leave(&outer) && pops == 2);
    release_map(&map); release_map(&process); release_map(&nested);
    assert(!map.alive && process.alive && nested.alive);
    allocator.depth = 0;
    assert(sh_process_heap_enter(&api, &outer));
    assert(sh_process_heap_leave(&outer) && allocator.depth == 0);
    allocator.depth = 31;
    assert(sh_process_heap_enter(&api, &outer) && allocator.depth == 32);
    assert(!sh_process_heap_enter(&api, &inner) && !inner.active);
    assert(sh_process_heap_leave(&outer) && allocator.depth == 31);
}

static void test_entry_failures(void)
{
    sh_process_heap_scope scope;
    sh_process_heap_api missing = api;
    HANDLE thread;
    reset();
    assert(!sh_process_heap_enter(&api, NULL));
    assert(!sh_process_heap_enter(NULL, &scope) && !scope.active);
    missing.pop = NULL;
    assert(!sh_process_heap_enter(&missing, &scope) && !scope.active);
    for (int mode = GET_NULL; mode <= GET_UNREADABLE; mode++) {
        get_mode = mode;
        refused_entry(&scope);
        assert(!scope.active && pushes == 0 && pops == 0);
    }
    get_mode = GET_OK;
    allocator.depth = -1;
    refused_entry(&scope);
    allocator.depth = 32;
    refused_entry(&scope);
    assert(!scope.active && pushes == 0 && pops == 0);
    reset();
    thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    assert(thread && WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    assert(allocations == 0 && pushes == 1 && pops == 0);
    reset();
    push_mode = PUSH_NOOP;
    refused_entry(&scope);
    assert(!scope.active && allocator.depth == 1 && pops == 0);
    push_mode = PUSH_FAULT_BEFORE;
    refused_entry(&scope);
    assert(!scope.active && allocator.depth == 1 && pops == 0);
    push_mode = PUSH_FAULT_AFTER;
    refused_entry(&scope);
    assert(!scope.active && allocator.depth == 1 && pops == 1);
    reset();
    push_mode = PUSH_FAULT_AFTER; pop_mode = POP_FAULT_BEFORE;
    refused_entry(&scope);
    assert(scope.active && allocator.depth == 2 && pops == 1);
    pop_mode = POP_OK;
    assert(sh_process_heap_leave(&scope) && allocator.depth == 1);
    reset();
    push_mode = PUSH_WRONG_HEAP;
    refused_entry(&scope);
    assert(scope.active && allocator.depth == 2 && pops == 0);
    allocator.heaps[1] = 0;
    assert(sh_process_heap_leave(&scope) && allocator.depth == 1);
}

static void test_restoration_failures(void)
{
    sh_process_heap_scope scope;
    reset();
    assert(sh_process_heap_enter(&api, &scope));
    allocator.heaps[1] = 2;
    assert(!sh_process_heap_leave(&scope) && scope.active && pops == 0);
    allocator.heaps[1] = 0;
    pop_mode = POP_NOOP;
    assert(!sh_process_heap_leave(&scope) && scope.active && allocator.depth == 2);
    pop_mode = POP_FAULT_BEFORE;
    assert(!sh_process_heap_leave(&scope) && scope.active && allocator.depth == 2);
    pop_mode = POP_OK;
    assert(sh_process_heap_leave(&scope) && allocator.depth == 1);
    reset();
    assert(sh_process_heap_enter(&api, &scope));
    pop_mode = POP_FAULT_AFTER;
    assert(!sh_process_heap_leave(&scope) && scope.active && allocator.depth == 1);
    assert(!sh_process_heap_leave(&scope) && pops == 1);
    assert(sh_process_heap_leave(NULL));
}

int main(void)
{
    test_binding();
    test_success_and_nesting();
    test_entry_failures();
    test_restoration_failures();
    puts("process_heap_scope_test OK");
    return 0;
}
