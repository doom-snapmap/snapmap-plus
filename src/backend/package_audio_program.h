/* One retained audio program for the complete set of authored sound scopes. */
#ifndef SH_PACKAGE_AUDIO_PROGRAM_H
#define SH_PACKAGE_AUDIO_PROGRAM_H
#include "package_audio_capture.h"
#include "audio_shared_policy.h"
#include "audio_bus_policy.h"
#include "resource_catalog.h"

typedef struct sh_package_audio_program sh_package_audio_program;
/* Compilation inputs are borrowed through prepare. originals is retained by
 * the owning provider and must outlive this program. Empty scopes do not read
 * audio inventory. No native calls, file writes or package edits occur here. */
sh_package_audio_program *sh_package_audio_program_open(
    const sh_package_compilation *const *scopes, size_t count,
    sh_audio_originals *originals, char *error, size_t capacity);
void sh_package_audio_program_close(sh_package_audio_program *program);
/* Collect while discovering sound declarations. Duplicate requests are benign.
 * All scopes are prepared together after complete dependency discovery, never
 * one independently activated graph per event. Collection ends at prepare. */
int sh_package_audio_program_collect(sh_package_audio_program *program,
    size_t scope, const char *sound, char *error, size_t capacity);
/* Discover named consumers of effective authored bank/media files before
 * reverse palette discovery. Baseline SnapMap sounds need not be copied into
 * a package; campaign-only declarations must be supplied by that scope. */
int sh_package_audio_program_seed(sh_package_audio_program *program, size_t scope,
    const sh_resource_catalog *catalog, int (*visit)(void *, const char *), void *context,
    char *error, size_t capacity);
/* Conservative nomination only. Final exact graph semantics determine whether
 * the sound actually differs and needs a private native identity. */
int sh_package_audio_program_affected(const sh_package_audio_program *program,
    size_t scope, const char *sound);
int sh_package_audio_program_prepare(sh_package_audio_program *program,
    char *error, size_t capacity);
/* Whether a sound was collected. Uncollected SnapMap sounds keep their native
 * event binding and need no program root, owners or output. */
int sh_package_audio_program_contains(const sh_package_audio_program *program,
    size_t scope, const char *sound);
/* The scope whose audio capture failed during the last prepare, else SIZE_MAX. */
size_t sh_package_audio_program_failed_scope(const sh_package_audio_program *program);
/* Retained shared bus assignments with exact private dependency identities and
 * delivery ownership. These are composition inputs, not independent bus objects
 * or authorization to replay the native constructor. Metadata and owners remain
 * valid after authored compiler scopes are freed. */
typedef struct sh_package_audio_bus_value {
    size_t scope, bank, object, record;
    uint32_t bus;
    unsigned kind;
    int baseline; /* Native witness scope; no package/delivery ownership. */
    sh_audio_bus_record field;
    sh_audio_graph_root root;
    sh_package_owners owners, gameplay;
} sh_package_audio_bus_value;
size_t sh_package_audio_program_bus_values(const sh_package_audio_program *program);
const sh_package_audio_bus_value *sh_package_audio_program_bus_at(
    const sh_package_audio_program *program, size_t index);
int sh_package_audio_program_bus_body(const sh_package_audio_program *program,
    size_t index, int bound, unsigned char **body, size_t *length, char *error, size_t capacity);
int sh_package_audio_program_bus_equal(const sh_package_audio_program *program,
    size_t left, size_t right);
/* One composed, unbound shared bus policy. Owner sets include equivalent
 * contributors and the author of a selected removal; unchanged native baseline
 * values do not acquire delivery owners. Numeric binding/native publication
 * must still be performed transactionally before using these recipes. */
const sh_audio_bus_policy *sh_package_audio_program_bus_policy(const sh_package_audio_program *program);
/* Whether the composed bus policy changes anything relative to the native
 * baseline recipes (an authored value, a removal or a new bus). */
int sh_package_audio_program_bus_edited(const sh_package_audio_program *program);
/* Available after bind. Values use the same committed private IDs as the bank
 * streams; failures preserve this entire prior view. This is prepared output,
 * not proof of native bus publication. Owners use the unbound policy indexes. */
const sh_audio_bus_policy *sh_package_audio_program_bound_bus_policy(const sh_package_audio_program *program);
/* The native baseline recipes alone, composed and bound in the same
 * transaction (original IDs). Every key a program can edit has its native
 * value here, including keys no active program leaves unedited. */
const sh_audio_bus_policy *sh_package_audio_program_bound_baseline_bus(const sh_package_audio_program *program);
int sh_package_audio_program_bus_policy_owners(const sh_package_audio_program *program,
    size_t setting,const sh_package_owners **owners,const sh_package_owners **gameplay);
/* Prepared shared controls from selected banks and their installed originals.
 * Bodies are owned through program close. They are not permission to replay
 * whole Init banks: native publication must apply this one plan transactionally.
 * The winning input maps back to a retained scope/bank for diagnostics. The
 * native initialization baseline has baseline=1 and scope=bank=SIZE_MAX. */
const sh_audio_shared_policy *sh_package_audio_program_shared(const sh_package_audio_program *program);
int sh_package_audio_program_shared_origin(const sh_package_audio_program *program,
    size_t input, size_t *scope, size_t *bank, int *baseline);
/* Available after bind: the same settings with private references (such as a
 * switch's modulator) relocated by the committed transaction. Native
 * publication consumes this view; failed binds preserve the previous one. */
const sh_audio_shared_policy *sh_package_audio_program_bound_shared(const sh_package_audio_program *program);
/* Delivery owners of an edited setting: every equivalent authored contributor
 * plus its private dependency suppliers. Baseline-equal settings own nothing. */
int sh_package_audio_program_shared_owners(const sh_package_audio_program *program,
    size_t setting, const sh_package_owners **owners, const sh_package_owners **gameplay);
/* Installed native Init controls as a baseline-only policy (every setting
 * unedited). Empty when no Init bank is installed. Reads originals only. */
int sh_package_audio_baseline_shared(sh_audio_originals *originals, sh_audio_shared_policy *out,
    char *error, size_t capacity);
/* Identity includes exact resolved HIRC/media behavior. Equal digest roots are
 * checked with the graph's exact partition before being returned to the outer
 * compiler. A sound must have been collected before prepare. */
int sh_package_audio_program_root(const sh_package_audio_program *program,
    size_t scope, const char *sound, sh_audio_graph_root *out,
    char *error, size_t capacity);
/* Delivery ownership of captured records, separate from event equality. Owned
 * by this prepared program. Original-only records carry no package ownership. */
int sh_package_audio_program_owners(const sh_package_audio_program *program,
    size_t scope, const char *sound, const sh_package_owners **owners,
    const sh_package_owners **gameplay);
/* Retain the outer compiler's output names before closing its source scopes.
 * Different scoped bindings cannot acquire the same output name. Multiple
 * declarations may intentionally invoke the same captured event. */
int sh_package_audio_program_output(sh_package_audio_program *program,
    size_t scope, const char *sound, const char *linked, char *error, size_t capacity);
/* Bind numeric IDs, bus policy and declaration scalars as one prepared result.
 * Failure preserves previous bank edits, bus values and declaration scalars. Native ID
 * reservations are caller-owned and must be rolled back after any failure.
 * This does not publish banks or change native registries. */
int sh_package_audio_program_bind(sh_package_audio_program *program,
    sh_package_compilation *compiled, sh_audio_graph_assign assign, void *context,
    char *error, size_t capacity);
/* Ordered prepared banks and retained sources for later transactional
 * materialization. Source metadata is valid even after compilation inputs are
 * freed. binding is NULL before a successful bind. */
size_t sh_package_audio_program_banks(const sh_package_audio_program *program, size_t scope);
size_t sh_package_audio_program_scopes(const sh_package_audio_program *program);
const sh_audio_graph_bank *sh_package_audio_program_bank(const sh_package_audio_program *program,
    size_t scope, size_t index, const sh_audio_bank_binding **binding);
/* Native activation plan for a bound program: every scope bank in native
 * registration order, deduplicated by the caller. numeric=1 loads the bound
 * identity by file ID (relocated or decimally named banks); otherwise the
 * native name loader opens filename. The native baseline witness scope is not
 * an activation scope. Visit returns 0 to stop; the call then fails. */
int sh_package_audio_program_loads(const sh_package_audio_program *program,
    int (*visit)(void *, uint32_t id, const char *filename, int numeric), void *context,
    char *error, size_t capacity);
/* Bound streamed media (including prefetched streams) uses its assigned
 * numeric filename. Resident DATA is already in the prepared bank stream.
 * Requires a successful bind when this scope contains streamed media. */
int sh_package_audio_program_media(const sh_package_audio_program *program, size_t scope,
    int (*visit)(void *, uint32_t authored, uint32_t bound, const sh_audio_graph_media *),
    void *context, char *error, size_t capacity);
/* Materialize an already-bound program into an unpublished linked provider.
 * Uses the caller-admitted private cache directory and existing resource table.
 * Exact outputs share sealed files; unchanged vanilla originals stay native.
 * Retains original availability paths, delivery ownership and bound dependency
 * lifetimes. Source packages are never edited. On success replaces/frees *compiled
 * and transfers its source ownership; failure leaves it unchanged. Complete cache
 * objects may survive failure for reuse, but no partial provider is published.
 * Private bank output excludes shared controls/bus definitions; the retained
 * plan and full bound recipes must be published separately. Does not activate
 * audio, publish shared policy or reserve native numeric IDs. */
int sh_package_audio_program_materialize(const sh_package_audio_program *program,
    sh_package_compilation **compiled, const char *directory, char *error, size_t capacity);
#endif
