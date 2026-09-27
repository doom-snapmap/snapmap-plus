#ifndef SH_RESOURCE_GRAPH_NATIVE_H
#define SH_RESOURCE_GRAPH_NATIVE_H
#include "signatures.h"
int sh_resource_graph_native_signature(const char *name);
int sh_resource_graph_native_install(const sig_result *results, size_t count);
int sh_resource_graph_native_ready(void);
/* Only fully connected constructor families are admitted through this route. */
int sh_resource_graph_native_consumer_ready(const char *family);
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
