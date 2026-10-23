#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "execution_context.h"

/* A context's slot holds its cell, allocated when it gets its identifier; the cell is freed when the context retires. */
typedef struct ec_cell { uint64_t id; struct ec_cell *next; } ec_cell;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static volatile DWORD g_index = FLS_OUT_OF_INDEXES;
static volatile LONG64 g_next;
static SRWLOCK g_owner_lock = SRWLOCK_INIT;
/* Owners are only ever prepended, so a list read without the lock stays valid. */
static sh_execution_owner *volatile g_owners;
/* Cells the callback queued, waiting for a live caller to retire them. */
static ec_cell *volatile g_retired;
#ifdef SH_EXECUTION_CONTEXT_TESTING
static volatile LONG g_retirements, g_queued;
static volatile DWORD g_retired_on;
#endif

uint64_t sh_execution_next(volatile long long *counter)
{
    LONG64 last = *counter, seen;
    for (;;) {
        /* Every value was handed out: none is handed out again. */
        if ((uint64_t)last == UINT64_MAX) return 0;
        seen = InterlockedCompareExchange64(counter, (LONG64)((uint64_t)last + 1), last);
        if (seen == last) return (uint64_t)last + 1;
        last = seen;
    }
}
/* The index's callback, with the slot value of the fiber being deleted, of the exiting thread (the one ending the
 * process included), or of each fiber when the index is freed. It neither waits nor allocates. */
static void NTAPI ec_retire(void *value)
{
    ec_cell *cell = (ec_cell *)value, *head;
    if (!cell) return;
#ifdef SH_EXECUTION_CONTEXT_TESTING
    InterlockedIncrement(&g_retirements);
    InterlockedIncrement(&g_queued);
    g_retired_on = GetCurrentThreadId();
#endif
    do { head = g_retired; cell->next = head; }
    while (InterlockedCompareExchangePointer((void *volatile *)&g_retired, cell, head) != head);
}
/* Retire every queued context with every registered owner. The caller holds no owner lock. */
static void ec_drain(void)
{
    ec_cell *cell = (ec_cell *)InterlockedExchangePointer((void *volatile *)&g_retired, NULL);
    while (cell) {
        ec_cell *next = cell->next;
        for (sh_execution_owner *owner = g_owners; owner; owner = owner->next) owner->retire(cell->id);
        free(cell);
#ifdef SH_EXECUTION_CONTEXT_TESTING
        InterlockedDecrement(&g_queued);
#endif
        cell = next;
    }
}
static BOOL CALLBACK ec_init(INIT_ONCE *once, void *parameter, void **unused)
{
    DWORD index = FlsAlloc(ec_retire);
    (void)once; (void)parameter; (void)unused;
    /* FALSE leaves the initialization to a later call, when an index may be available. */
    if (index == FLS_OUT_OF_INDEXES) return FALSE;
    g_index = index;
    return TRUE;
}
uint64_t sh_execution_context(sh_execution_owner *owner, int create)
{
    DWORD error = GetLastError(), index;
    uint64_t context = 0;
    ec_cell *cell;
    if (g_retired) ec_drain();
    if (create && !InterlockedCompareExchange(&owner->registered, 0, 0)) {
        AcquireSRWLockExclusive(&g_owner_lock);
        if (!owner->registered) {
            owner->next = g_owners;
            g_owners = owner;
            InterlockedExchange(&owner->registered, 1);
        }
        ReleaseSRWLockExclusive(&g_owner_lock);
    }
    /* Only creation allocates the index: before there is one, no context has a value. */
    index = g_index;
    if (index == FLS_OUT_OF_INDEXES && create && InitOnceExecuteOnce(&g_once, ec_init, NULL, NULL)) index = g_index;
    if (index != FLS_OUT_OF_INDEXES) {
        if ((cell = (ec_cell *)FlsGetValue(index)) != NULL) context = cell->id;
        else if (create && (cell = (ec_cell *)calloc(1, sizeof(*cell))) != NULL) {
            if ((cell->id = sh_execution_next(&g_next)) != 0 && FlsSetValue(index, cell)) context = cell->id;
            else free(cell);
        }
    }
    SetLastError(error);
    return context;
}

#ifdef SH_EXECUTION_CONTEXT_TESTING
void sh_execution_context_test_shutdown(void)
{
    if (g_index != FLS_OUT_OF_INDEXES) FlsFree(g_index);
    g_index = FLS_OUT_OF_INDEXES;
    InitOnceInitialize(&g_once);
}
uint64_t sh_execution_context_test_next(uint64_t next)
{
    return (uint64_t)InterlockedExchange64(&g_next, (LONG64)(next - 1)) + 1;
}
long sh_execution_context_test_retirements(unsigned long *thread)
{
    if (thread) *thread = g_retired_on;
    return InterlockedCompareExchange(&g_retirements, 0, 0);
}
long sh_execution_context_test_queued(void) { return InterlockedCompareExchange(&g_queued, 0, 0); }
#endif
