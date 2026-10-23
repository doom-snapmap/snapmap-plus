/* Exact resource origins held by native instance fields. */
#ifndef SH_RESOURCE_INSTANCES_H
#define SH_RESOURCE_INSTANCES_H
#include <stddef.h>
#include <stdint.h>

typedef struct sh_resource_instance_origin sh_resource_instance_origin;
typedef struct sh_resource_instance_api {
    /* Immutable captures identify the view at the source read, never a later
     * name lookup. Callbacks must not call this module or native code. */
    int (*promote)(const void *capture, void **lease);
    void (*release_capture)(void *capture);
    void (*release_lease)(void *lease);
} sh_resource_instance_api;
/* Bind once, before any origins or owners exist. */
int sh_resource_instances_bind(sh_resource_instance_api api);
/* Takes capture on success only. baseline is an independently established
 * source/census identity, not merely the absence of a package lookup. A NULL
 * origin denotes unknown provenance. Baseline origins carry no capture. */
sh_resource_instance_origin *sh_resource_instances_origin(void *capture, int baseline);
/* Caller proved the native built-in/default source contract. Each child is
 * its exact origin observed during that same invocation, never a later name
 * lookup. Weak composites retain metadata; live composites retain children. */
sh_resource_instance_origin *sh_resource_instances_origin_composite(
    sh_resource_instance_origin *const *children, size_t count);
typedef struct sh_resource_default_origin_scope { uint64_t context, serial; } sh_resource_default_origin_scope;
void sh_resource_instances_origin_release(sh_resource_instance_origin *origin);
/* An in-flight native read holds a serving reference before it can suspend or
 * publish a field. Acquire before releasing another live reference. */
int sh_resource_instances_origin_hold(sh_resource_instance_origin *origin);
void sh_resource_instances_origin_unhold(sh_resource_instance_origin *origin);
/* Immutable identity only: lets a source scope reuse one active origin over
 * repeated chunks of the same retained capture. Distinct captures are never
 * inferred equivalent by name or content. Caller already owns origin. */
int sh_resource_instances_origin_matches(const sh_resource_instance_origin *origin,
    const void *capture, int baseline);
/* Bind the actual opening source lifetime once, before publication. Aliases
 * and derived objects never change it. Composite inputs retain their own
 * distinct source identities. Must precede all hold/composite/publish calls;
 * late first binding is rejected instead of mutating a shared origin. */
int sh_resource_instances_origin_source(sh_resource_instance_origin *origin,void *resource,uint64_t token);
/* Each successful native source read publishes the capture opened before it.
 * Resource reconstruction removes only this weak association; live fields
 * and transient reads keep their exact earlier origin. No native pointer is
 * dereferenced. publish borrows origin; lookup returns an owned reference. */
int sh_resource_instances_publish(void *resource, sh_resource_instance_origin *origin);
void sh_resource_instances_forget(void *resource);
sh_resource_instance_origin *sh_resource_instances_lookup(void *resource);

/* Actual construction/reset begins a new monotonically numbered lifetime.
 * A copied constructor uses clone instead. No address/vtable equality can
 * turn a stale token into a new object's token. Zero is a recording failure. */
uint64_t sh_resource_instances_construct(void *object, int observed);
uint64_t sh_resource_instances_token(const void *object);
/* Classify only at a proved native construction/use boundary. kind is an
 * adapter-owned identity cookie; owner is that invocation's actual holder.
 * The owner must have an observed token; its exact lifetime is captured too.
 * A token cannot be reclassified as another kind or a later owner lifetime. New construction/copy
 * clears this metadata instead of inferring class from an address. */
int sh_resource_instances_classify(void *object,uint64_t token,const void *kind,void *owner);
void *sh_resource_instances_class_owner(const void *object,uint64_t token,const void *kind);
/* Lost observation of an actual compiled input. Caller keeps origin live
 * through this call; no allocation is required. Exact origins form a shared
 * quarantine until every marked holder lifetime is replaced or destroyed.
 * Baseline-only inputs do not mark unrelated stock holders. A zero token
 * retains the exact input for process-lifetime quarantine and returns zero;
 * caller must also fence activation with the exact opening captures. */
int sh_resource_instances_fault(void *object,uint64_t token,sh_resource_instance_origin *origin);
/* Existing actual holder, but the new compiled input could not be allocated.
 * Its opening stream independently retains the exact serving origin. */
int sh_resource_instances_mark_fault(void *object,uint64_t token);
/* Only after proved full native replacement, never a partial edit or merely
 * complete recorded fields. A stale token cannot clear a later fault. */
int sh_resource_instances_fault_recovered(void *object,uint64_t token);
int sh_resource_instances_faulted(const void *object,uint64_t token);
int sh_resource_instances_faults(int (*visit)(void *,void *,uint64_t),void *context);
/* Snapshot retained compiled fault inputs, then visit their immutable actual
 * opening identities outside locks. NULL/zero means unresolved identity. */
int sh_resource_instances_fault_sources(int (*visit)(void *,void *,uint64_t,
    const sh_resource_instance_origin *group),void *context);
/* Each field of this holder whose resource contains() selects while its origin is
 * unknown (never observed) or its path is unresolved: visit(context, resource,
 * slot, unresolved). Stops at the first visit returning 0; 1 otherwise. */
int sh_resource_instances_unknown_fields(const void *object,uint64_t token,
    int (*contains)(void *,void *),void *context,
    int (*visit)(void *,void *,uintptr_t,int),void *visit_context);
int sh_resource_instances_fault_group_holders(const sh_resource_instance_origin *group,
    int (*visit)(void *,void *,uint64_t),void *context);
int sh_resource_instances_fault_group_active(const sh_resource_instance_origin *group);
/* Emergency native construction scope: metadata and already-held fault
 * groups only, no allocation/native/stack access. Fresh escaped holders adopt
 * every active group for that actual execution context. An active source's
 * metadata survives reentrant completed destruction until scope end/retire. */
int sh_resource_instances_fault_scope_begin(void *object,uint64_t token,uint64_t context);
void sh_resource_instances_fault_scope_end(void *object,uint64_t token,uint64_t context);
void sh_resource_instances_fault_scope_retire(uint64_t context);
int sh_resource_instances_fault_scope_adopt(uint64_t context,void *object,uint64_t token);
int sh_resource_instances_fault_share(void *source,uint64_t source_token,void *object,uint64_t token);
/* Inspect immutable captures without name/provider lookup. The callback
 * borrows each capture and must retain it before keeping it. */
int sh_resource_instances_origin_captures(sh_resource_instance_origin *origin,
    int (*visit)(void *,const void *),void *context);
/* A verified assignment of one actual resource-pointer field. slot is that
 * field's storage address. origin is the token captured when the native read
 * returned resource; never reacquire the latest provider by name. A NULL
 * resource clears just this field. A non-NULL resource with NULL origin is
 * recorded as unknown, never stock. It replaces prior ownership even if the
 * reader reported failure after writing the field. Skipped/no-op readers
 * must not call this function. The call borrows origin. */
int sh_resource_instances_assign(void *object, uint64_t token, uintptr_t slot,
    void *resource, sh_resource_instance_origin *origin);
/* A typed path survives native container moves without claiming that any
 * resize callback copies its elements. Each step selects an element of the
 * list at offset in its parent element; type is the actual native descriptor.
 * The adapter owns descriptor validation and checked address resolution. */
typedef struct sh_resource_instance_step {
    size_t offset, index, stride;
    uintptr_t type, element_type, element_ops;
} sh_resource_instance_step;
typedef struct sh_resource_instance_path {
    uintptr_t root_type, context;
    size_t root_size, count, leaf;
    const sh_resource_instance_step *steps;
} sh_resource_instance_path;
typedef int (*sh_resource_instance_resolve)(void *context, const void *object,
    const sh_resource_instance_path *path, uintptr_t *slot, void **resource);
/* resolve returns 1 for an actual verified leaf, 0 for an index removed by
 * zero/shrink, -1 for an unresolved/malformed path. It runs outside this
 * module's lock. Reentrant mutations invalidate the pending plan. */
int sh_resource_instances_assign_path(void *object, uint64_t token, uintptr_t slot,
    void *resource, sh_resource_instance_origin *origin, const sh_resource_instance_path *path);
int sh_resource_instances_reconcile(void *object, uint64_t token,
    sh_resource_instance_resolve resolve, void *context);
/* Verified in-place payload replacement changes pointer-only aliases, not
 * independent copied values. Revalidate actual field storage before retiring
 * its earlier origin. Unresolved fields retain protection and stay incomplete. */
int sh_resource_instances_rebind(void *object,uint64_t token,void *resource,
    sh_resource_instance_origin *origin,int (*match)(void *,uintptr_t,void *,const void *,size_t),
    sh_resource_instance_resolve resolve,void *context);
/* Before a destructive native refresh, protect every new origin it may
 * consume while its old fields remain live. Failed/partial refresh retains
 * these shares; clear only after a verified full replacement/reset. Actual
 * completed destruction also releases them. Repeated origins are shared. */
int sh_resource_instances_protect(void *object, uint64_t token, sh_resource_instance_origin *origin);
void sh_resource_instances_unprotect(void *object, uint64_t token);
/* Explicit native payload ownership, separate from the weak source map.
 * Replace only at a verified complete source/default/reset boundary. */
int sh_resource_instances_payload(void *object, uint64_t token, sh_resource_instance_origin *origin);
/* Exact currently owned payload, independently live-held for origin_unhold.
 * Never falls back to the weak source map, promotes or rebinds by name.
 * Concurrent payload/lifetime replacement fails exact revalidation. */
sh_resource_instance_origin *sh_resource_instances_payload_hold(const void *object,uint64_t token);
/* Snapshot observed holders of this exact native resource pointer. Visitor
 * runs outside the map lock, with the lifetime token it must verify. Caller
 * supplies the native synchronization excluding object deletion. */
int sh_resource_instances_holders(void *resource,
    int (*visit)(void *context, void *object, uint64_t token), void *context);
/* contains is a pure keyed predicate under the map lock. The batch avoids a
 * whole-instance scan per changed resource; visit remains outside the lock. */
/* Actual opening identities in an owned copied payload, recursively through
 * immutable composites. A recycled source token cannot select an old input. */
int sh_resource_instances_payload_uses(const void *object,uint64_t token,
    int (*contains)(void *,void *),void *context);
int sh_resource_instances_holders_where(int (*contains)(void *context,void *resource),
    int (*visit)(void *context,void *object,uint64_t token),void *context);
int sh_resource_instances_unknown_inputs(const void *object,uint64_t token,
    int (*contains)(void *,void *),void *context);
/* Metadata reference for one already recorded physical value, for retaining
 * its exact entry origin across a native operation that may reenter teardown. */
sh_resource_instance_origin *sh_resource_instances_field_origin(const void *object, uint64_t token, uintptr_t slot);
/* Live share of an exact recorded copied field, with token/slot/length/bytes
 * checked again after sharing. Never promotes a metadata-only/retired origin
 * and never calls native/provider code. Caller releases with origin_unhold.
 * The caller owns native storage while taking its byte snapshot. */
sh_resource_instance_origin *sh_resource_instances_copy_hold(const void *object,uint64_t token,
    uintptr_t slot,const void *bytes,size_t length);
/* A completed native copy has a separate value lifetime from its backing
 * pointer. Snapshot actual copied bytes and retain the origin observed when
 * native read them; a later backing reread must not replace this origin.
 * NULL bytes/zero length are invalid; use assign(...,NULL,NULL) to retire it. */
int sh_resource_instances_copy(void *object, uint64_t token, uintptr_t slot,
    const void *bytes, size_t length, void *backing, sh_resource_instance_origin *origin);
/* Native copy construction: copies field origins by inline offset, retains
 * every exact origin before publishing destination, and replaces a previous
 * destination lifetime only on success. Unknown or out-of-object slots make
 * the copy incomplete rather than guessing a relocation for heap storage. */
uint64_t sh_resource_instances_clone(void *destination, const void *source, size_t size);
/* The native virtual copy can omit fields or override them. match checks the
 * destination's actual pointer (length=0) or copied bytes; it calls no
 * native/module API.
 * Preserve an observed destination field only while match confirms its value,
 * then fill matching copied source fields at still-unobserved destinations.
 * Publish one fresh object token only after the entire plan is retained. */
uint64_t sh_resource_instances_clone_checked(void *destination, const void *source, size_t size,
    int (*match)(void *context, uintptr_t slot, void *resource, const void *bytes, size_t length), void *context);
uint64_t sh_resource_instances_clone_paths_checked(void *destination, const void *source, size_t size,
    int (*match)(void *context, uintptr_t slot, void *resource, const void *bytes, size_t length),
    sh_resource_instance_resolve resolve, void *context);
/* An observed native container relocation: move surviving field slots by
 * offset and retire slots in the removed tail, after native copy/destruction
 * completed. The caller proves these ranges belong to this same owner. */
int sh_resource_instances_relocate(void *object, uint64_t token, uintptr_t old_start,
    size_t old_size, uintptr_t new_start, size_t new_size);
/* Capture token before the native destructor; finish only after it returned.
 * A failed destructor retains ownership. A nested new construction at the
 * same address cannot be retired by the older destructor's finish. */
void sh_resource_instances_destroyed(void *object, uint64_t token, int completed);
/* Scoped observations, not a global completeness certificate. */
int sh_resource_instances_complete(const void *object, uint64_t token);
void sh_resource_instances_counts(size_t *resources, size_t *instances, size_t *fields);
#ifdef SH_RESOURCE_INSTANCES_TESTING
void sh_resource_instances_test_reset(void);
#endif
#endif
