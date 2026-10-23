/* Execution contexts for scopes that follow DOOM's script fibers (R-321, R-328).
 *
 * DOOM runs SuperScript on Win32 fibers, so a routine can be suspended inside a scope while another fiber runs on
 * its thread, and can resume on another thread. What such a scope records belongs to an execution context: one
 * fiber, or one thread that runs no fiber. A context is identified by a 64-bit value kept through fiber local storage
 * (FLS), which gives every fiber its own slot, switches it with the fiber and acts as thread local storage on a
 * thread that runs no fiber. Identifiers are never reused in the process, so one never names a later context.
 *
 * The system calls the index's callback with a context's slot value when its fiber is deleted or its thread exits;
 * that includes the thread that ends the process, after ExitProcess has terminated every other thread whatever
 * locks they held. The callback therefore only queues the context, without waiting or allocating. The next lookup
 * by any live context retires queued contexts with every registered owner, before that caller takes an owner's
 * lock: each owner releases what it keeps for the identifier in its own memory and never reads the retired
 * context's stack.
 *
 * The index is never freed while the process runs: the backend is not unloaded at run time (its hooks point into
 * it and its DLL_PROCESS_DETACH releases nothing), and the callback's address relies on that. A build that could
 * unload it would have to free the index first. Only the test seam below frees it. */
#ifndef SH_EXECUTION_CONTEXT_H
#define SH_EXECUTION_CONTEXT_H
#include <stdint.h>

typedef struct sh_execution_owner {
    /* Releases the owner's state for a retired context. It runs on a live thread that holds no owner lock; it does
     * not call this module. */
    void (*retire)(uint64_t context);
    volatile long registered;
    struct sh_execution_owner *next;
} sh_execution_owner;

/* The calling context's identifier, after retiring any queued contexts. create 0 only looks (owner may be NULL) and
 * never allocates the index: 0 when the context has no identifier. create 1 registers owner for retirement (once,
 * before it can keep anything for an identifier) and gives the context an identifier when it has none: 0 when none
 * can be recorded now (no FLS index can be allocated, memory or the slot value cannot be stored, or every identifier
 * has been handed out), and the caller then records nothing. A later call tries again; the index allocation is
 * retried until it succeeds. The thread's last-error value is preserved. */
uint64_t sh_execution_context(sh_execution_owner *owner, int create);
/* The next value of a counter whose values are never reused (identifiers, owners' scope serials): 1 first, then
 * 0 once every value has been handed out, which is terminal. */
uint64_t sh_execution_next(volatile long long *counter);

#ifdef SH_EXECUTION_CONTEXT_TESTING
/* Free the index, which queues every context holding a value, and forget it: the next creation allocates another.
 * No context may use an owner meanwhile. Not a production shutdown path. */
void sh_execution_context_test_shutdown(void);
/* Make next the next identifier handed out (a synthetic reuse; production identifiers are never reused), returning
 * the one that would have been: passing that back resumes the sequence. */
uint64_t sh_execution_context_test_next(uint64_t next);
/* Callback calls so far and the thread that ran the latest; contexts queued and not yet retired. */
long sh_execution_context_test_retirements(unsigned long *thread);
long sh_execution_context_test_queued(void);
#endif
#endif
