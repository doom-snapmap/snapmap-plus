#ifndef SH_RESOURCE_GRAPH_NATIVE_H
#define SH_RESOURCE_GRAPH_NATIVE_H
#include "signatures.h"
#include "resource_instances.h"
#include "resource_presentation.h"
/* Always pair end, including a partial begin. Independent scopes opened by
 * presentation code retain their own graph and actual holder ownership. */
int sh_resource_graph_native_presentation_begin(sh_resource_presentation_scope *scope);
void sh_resource_graph_native_presentation_end(sh_resource_presentation_scope *scope);
int sh_resource_graph_native_signature(const char *name);
int sh_resource_graph_native_install(const sig_result *results, size_t count);
int sh_resource_graph_native_ready(void);
/* One synchronous adapter runs after Load/Materialize restores its parent
 * observation, immediately before the real native caller resumes. The return
 * address identifies a verified consumer, never a name-based prediction.
 * Failed native calls keep their original exception and do not notify. */
typedef void (*sh_resource_return_observer)(void *resource,uintptr_t caller);
int sh_resource_graph_native_set_return_observer(sh_resource_return_observer observer);
int sh_resource_graph_native_return_observer_ready(sh_resource_return_observer observer);
/* Separate normal-return adapter for the native three-argument DeclFind.
 * It never replaces the Load/Materialize adapter or reports an exceptional exit. */
int sh_resource_graph_native_set_find_return_observer(sh_resource_return_observer observer);
int sh_resource_graph_native_find_return_observer_ready(sh_resource_return_observer observer);
/* Current owned ParseSource/SetText/ShaderSourceCompile entries only; callers qualify the bodies
 * beyond these entry hooks and the actual declaration receiver separately. */
int sh_resource_graph_native_decl_flow_ready(uintptr_t parse,uintptr_t set_text,uintptr_t compile);
int sh_resource_graph_native_default_origin_begin(sh_resource_default_origin_scope *scope, void *resource);
int sh_resource_graph_native_default_origin_finish(sh_resource_default_origin_scope *scope, void *resource,
    int native_returned, int in_order, int complete);
/* Synchronous actual-input adapters. A borrowed origin is retained directly;
 * it is never reconstructed from a name or a newer provider. NULL makes the
 * current observation incomplete. */
/* Graph recording or a positively scoped actual consumer above any SOURCE or
 * presentation boundary. Independent nested owners remain observable. */
int sh_resource_graph_native_observing(void);
int sh_resource_graph_native_observe_resource(void *resource);
int sh_resource_graph_native_observe_origin(sh_resource_instance_origin *origin);
/* Called synchronously under resident's native render/heap ownership fence,
 * for an actual resource rebuilt by that pass. */
int sh_resource_graph_native_refresh_instances(void *const *resources, size_t count, char *error, size_t capacity);
typedef struct sh_package_resource_capture sh_package_resource_capture;
/* Copy the current native SOURCE frame's identity before routing a file open.
 * An instance-only scope and a private shader cook provide no source target. */
int sh_resource_graph_native_source_target(char family[128], char name[4096]);
/* Actual source-open witness, before its bytes reach the native reader.
 * Consumes capture on every path. Supply either the exact opening provider's
 * immutable capture or baseline=1 from a positive noncompiled source route.
 * Both NULL/0 positively observes unknown source and clears the association;
 * a cached/no-op read supplies no witness and keeps its earlier association.
 * Private candidate preparation supplies neither; it must not be mislabeled
 * with the published provider. Returns 1 when the active source matched. */
int sh_resource_graph_native_source_observed(const char *family, const char *name,
    sh_package_resource_capture *capture, int baseline);
/* source_observed consumes capture on every path: 1 observed, 0 unmatched or
 * excluded, -1 matching actual read lost observation. The stream must retain
 * its existing opening fence on -1 through positive holder recovery. */
int sh_resource_graph_native_source_fault_target(void **object,uint64_t *token);
int sh_resource_graph_native_source_fault_input(void **object,uint64_t *token);
int sh_resource_graph_native_source_fault_recovered(void *object,uint64_t token);
int sh_resource_graph_native_admit_instances(void *const *resources,size_t count,char *error,size_t capacity);
/* The last admission's unattributed-input report (resource_resident.h). */
size_t sh_resource_graph_native_admit_report(const struct sh_resource_admit_report **items);
/* Only fully connected constructor families are admitted through this route. */
int sh_resource_graph_native_consumer_ready(const char *family);
/* Call inside a retirement default's request frame, after the default returned (R-352). Records there the
 * registered resources the object's default routine (vtable +0x50, pinned on this executable) is known to
 * read through static handles, which no hook sees, with those of a declaration class's parser (+0x98).
 * Nonzero, so the frame may complete, only for a routine whose whole read inventory is established: every
 * registered-resource read in it goes through a hooked find, or there is none. Any other routine, a moved
 * or patched one, an unpinned executable, unlive hooks or no current request frame: 0. */
int sh_resource_graph_native_default_observed(void *resource);
#ifdef SH_RESOURCE_GRAPH_TESTING
/* Use base/renderer (1 Vulkan, 0 OpenGL, -1 unpinned) instead of the host image; -2 restores the host. */
void sh_resource_graph_native_test_default_image(const unsigned char *base, int renderer);
#endif
struct sh_package_consumer;
/* Metadata-only prepublication gate for a complete exact movie plan. Native
 * dependency preparation remains in the admitted provider preparation batch.
 * Runtime reserves record/lease storage before publication; movie begin and
 * borrow must then be allocation-free and cannot return an unprepared compiled
 * owner as stock. The native adapter never substitutes a different movie. */
int sh_resource_graph_native_movie_plan_valid(const struct sh_package_consumer *owner);
typedef struct sh_resource_editor_state sh_resource_editor_state;
/* Existing admitted registration/reconstruction/drain transaction only.
 * Moves paired destructor-owned members and resets exact constructor stores,
 * keeping identity/source/unwritten bookkeeping in place. Old storage survives
 * until End. Rejection restores it before prior-provider consumer recovery;
 * borrowed buffers are detached, never freed. Context must outlive the scope.
 * Repeated reads/reconstruction keep intermediate candidate storage separately.
 * End=1 consumes *owned. End=0 retains it because admission or identity
 * restoration remains owed, before any cleanup. End=-1 consumes the active
 * handle into damaged storage retained for diagnostics; NEVER replay cleanup.
 * *committed latches acceptance before cleanup, so an accepted cleanup fault
 * cannot be treated as rollback after original storage was freed. */
sh_resource_editor_state *sh_resource_graph_native_editor_state_begin(
    int (*admitted)(void *), void *context, char *error, size_t capacity);
int sh_resource_graph_native_editor_state_ok(sh_resource_editor_state *state);
/* Read-only admission and live-identity preflight before any disk finalizer. */
int sh_resource_graph_native_editor_state_ready(sh_resource_editor_state *state);
int sh_resource_graph_native_editor_state_end(sh_resource_editor_state **owned,
    int accepted, int *committed, char *error, size_t capacity);
/* Explicit engine preparation, never a compiler/read callback. The admission
 * callback must verify the engine main thread, settled browser and the existing
 * provider activation/recovery transaction, with no package/provider lock held.
 * Native GenericLoad always calls Read, including for already ready objects;
 * it owns native error storage and pending bookkeeping. A shader state scope
 * must span the complete batch, including any native cold materialization.
 *
 * A ready registered fixed shader ABI object's identity and
 * ABI/default payload must survive the parse unchanged, and the observed parser
 * input must equal expected_source. This never sets source bytes or invents a
 * receipt from the expected bytes. The caller holds a validated registry object
 * lifetime and owns expected_source for the complete synchronous operation.
 * renderProg delegates to isolated source cooking. New callers must use
 * prepare_program before native Find: even finding a cold program can allocate
 * GPU state, and rereading an existing one destroys its live handles. */
typedef int (*sh_resource_shader_admission)(void *context);
typedef struct sh_resource_shader_state sh_resource_shader_state;
/* Begin after renderer/resident admission and end only after any prior-provider
 * recovery, while the final render/heap scope is still held. Native parameter
 * publication is staged before forwarding. Allocation, registration or active
 * parameter-block failures suppress that scoped publish and poison preparation.
 * End restores original live values through the verified native state writer,
 * verifies index/registration/block stability and revokes receipts on failure.
 * A failed End must also revoke batch products and block candidate publication.
 * It never reconstructs a fixed ABI object or invents a replacement index/mask.
 * The admission callback/context must outlive the scope, including retained
 * recovery after End=-1; do not return with a borrowed stack context. */
sh_resource_shader_state *sh_resource_graph_native_shader_state_begin(
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity);
/* Failure callback for a batch of preexisting fixed slots: restore their saved
 * same-object payload and live parameter values. No reconstruction/reparse is
 * involved. The scope stays active for final End. Isolated program cooking
 * creates no GPU ownership to recover; this scope restores any fixed ABI
 * dependencies reached while compiling its source. */
int sh_resource_graph_native_shader_state_restore(sh_resource_shader_state *state,
    char *error, size_t capacity);
/* End returns 1 for restored state, 0 for restored state with rejected accepted
 * evidence, and -1 while restoration remains owed. A -1 result retains the
 * scope: keep renderer/heap ownership and retry after restoring the documented
 * registration/admission invariant. Do not free or abandon that obligation.
 * accepted=0 always revokes this scope's receipts even when restoration succeeds. */
int sh_resource_graph_native_shader_state_end(sh_resource_shader_state *state,
    int accepted, char *error, size_t capacity);
int sh_resource_graph_native_prepare_shader(void *resource, const char *family,
    const char *name, const void *expected_source, size_t length,
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity);

/* Immutable installed source view, owned by the caller through the synchronous
 * cook. Use the baseline-reader contract: 1 SnapMap or 2 campaign/multiplayer
 * with malloc-owned bytes, 0 absent, -1 failed. Native source and
 * include opens use this view instead of any currently published overrides.
 * An absent input must not fall through to a different authored generation. */
typedef int (*sh_resource_shader_source_reader)(void *context, const char *path,
    unsigned char **body, size_t *length);
/* Provider bridge: 0 outside a cook source view, 1 with malloc-owned bytes,
 * -1 for a claimed absent/failed read. This is thread-local and synchronous. */
int sh_resource_graph_native_cook_input(const char *path, unsigned char **body, size_t *length);

/* Cook original program source into an isolated, scope-owned native parser and
 * metadata workspace. Call before finding/materializing a renderprog, whether
 * its native name is cold or already registered. No GPU program, registry slot
 * or game file is created/replaced. Only source-cook certificates are emitted;
 * those do not establish equality with a live GPU object or driver acceptance.
 * The same admitted shader state scope covers fixed ABI dependencies. Its End
 * contract still applies on success/failure, including retained restoration. */
int sh_resource_graph_native_prepare_program(void *manager, const char *name,
    const void *expected_source, size_t length,
    sh_resource_shader_source_reader original, void *original_context,
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity);
#endif
