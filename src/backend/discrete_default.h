/* Discrete animation defaults without a GPU readback (R-321).
 *
 * The engine's discrete animation default (vtable +0x50: VK 0x141453ab0, GL 0x141447790)
 * builds a temporary box model (VK 0x141455870, GL 0x1414495e0) and finishes it with the
 * flag word 2 every static model is constructed with: upload static (usage 1) buffers, then
 * free CPU data when the static free setting is on. Its generator copies the box geometry by
 * mapping those buffers back, which Vulkan refuses with idCommon::Error(6) whenever the game
 * renders. That builder's call to the finish is redirected here. Inside a scope this backend
 * opens around its own resident defaults and loads, the box stays on the CPU instead (flag
 * word 1: keep CPU data, no upload); the generator then copies CPU arrays and the class's
 * own upload (usage 0) builds the same default. The native CPU ownership and transparency-sort
 * helpers still run, after the finish's common preparation and before the generator. Their
 * per-renderer entry points are verified with the finish. Sorted and borrowed geometry stays
 * usable without uploading an intermediate static buffer. Outside a scope, and for every other caller of the finish, nothing changes;
 * no renderer or headless state is read or written.
 *
 * A scope belongs to an execution context, not to an OS thread: DOOM runs script on Win32
 * fibers, so a fiber may run while another is suspended inside a scope, and may resume on
 * another thread. Scopes are recorded per execution context (execution_context.h) in this
 * module's memory, as size_owners records its brackets, and go when the context retires. */
#ifndef BACKEND_DISCRETE_DEFAULT_H
#define BACKEND_DISCRETE_DEFAULT_H

#include <stdint.h>

/* A handle to an open scope; the scope's record is this module's. */
typedef struct sh_discrete_scope {
    uint64_t context;                   /* the execution context that entered it (0: not linked) */
    uint64_t serial;                    /* its record there; serials are never reused */
} sh_discrete_scope;

/* Verify this pinned executable's builder call and finish, then redirect the call with one
 * aligned displacement exchange. 1 installed (or already installed), 0 refused, unchanged. */
int sh_discrete_default_install(const uint8_t *base);
/* Whether a default of this resource family can be built in this process: always, except a
 * discrete animation without the redirect on Vulkan or on an executable that is not pinned. */
int sh_discrete_default_ready(const char *family);
/* Open a scope on the calling execution context. 1 linked; 0 when no context could be recorded
 * (nothing is linked, and the scope must not be relied on). Scopes nest within a context. */
int sh_discrete_default_enter(sh_discrete_scope *scope);
/* Close it, on the context that entered it. 1 when it was the context's innermost scope; 0 when
 * it was not (scopes above it are dropped), when its context retired while it was open, when it
 * is not linked, or when another context calls it (nothing changes then). */
int sh_discrete_default_leave(sh_discrete_scope *scope);
/* Legacy pinned-installation probe; this is not a dependency-completeness predicate.
 * Retirement frames use sh_resource_graph_native_default_observed and its read inventory. */
int sh_discrete_default_observed(const char *family);
/* How many temporary boxes the redirect kept on the CPU, and how many in a scope it left to
 * the engine's route because its construction data was unreadable (diagnostics). */
long sh_discrete_default_adjusted(void);
long sh_discrete_default_declined(void);

#ifdef SH_DISCRETE_DEFAULT_TESTING
/* Restore the call, release the relay and the scope contexts, and forget the install. 0 if the
 * call stays redirected. */
int sh_discrete_default_test_reset(void);
/* Execution contexts with an open scope. */
size_t sh_discrete_default_test_contexts(void);
/* Replace verified native helpers with result-producing fixtures after installation. */
void sh_discrete_default_test_helpers(void (*detach)(void *), void (*sort)(void *));
#endif

#endif /* BACKEND_DISCRETE_DEFAULT_H */
