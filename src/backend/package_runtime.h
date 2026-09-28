/* One atomically published package compilation shared by runtime consumers. */
#ifndef SH_PACKAGE_RUNTIME_H
#define SH_PACKAGE_RUNTIME_H
#include "package_compiler.h"
#include "resource_catalog.h"
#include "package_usage.h"
#include "package_source_graph.h"
#include "package_provenance.h"
#include "package_bundle.h"
#include "package_occurrence.h"
#include "audio_originals.h"
#include "package_audio_program.h"
#include "audio_id_reservations.h"
#include "virtual_material_runtime.h"
#include "package_consumers.h"
#include "package_keys.h"

typedef struct sh_package_consumer_hold {
    const sh_package_consumer *binding;
    void *provider;
} sh_package_consumer_hold;
/* Capture the currently served owner's immutable constructor plan, including
 * during activation/recovery. Return 1 with a retained snapshot, 0 for an owner
 * outside this catalog, -1 for invalid arguments. Start out zeroed; do not copy
 * or overwrite an acquired hold. Lookup needs no allocation or native call.
 * Retaining a plan does not itself admit an engine load or preserve all files
 * in the active view: the native consumer still needs its ordinary lifetime
 * lease. Release after the synchronous constructor/consumer request completes. */
int sh_package_runtime_consumer_acquire(const char *family, const char *name, sh_package_consumer_hold *out);
void sh_package_runtime_consumer_release(sh_package_consumer_hold *hold);
int sh_package_runtime_legacy_read(void *context, const char *type,
    const char *name, const char *path, unsigned char **body, size_t *length,
    char *error, size_t capacity);

int sh_package_runtime_refresh(const char *data_root);
/* Process-owned immutable script field metadata; no native calls. */
const struct sh_decl_script_schema *sh_package_runtime_script_schema(void);
/* Product consumers hold their derived-resource boundary across the final
 * swap. Both callbacks run outside the compilation lock; end pairs with begin,
 * with committed=0 when no swap occurred. Preparation stays outside the guard.
 * Callbacks must not initiate another refresh. */
typedef struct sh_package_activation_guard {
    void (*begin)(void);
    void (*end)(int committed);
} sh_package_activation_guard;
int sh_package_runtime_refresh_guarded(const char *data_root, sh_package_activation_guard guard);
/* The candidate provider is visible during activate, but is not ready for map
 * admission. Return 1 only after every activation step succeeds. On failure the
 * previous provider is restored first, then activate runs with restoring=1 to
 * recover consumers. Both swaps use guard; neither callback holds its boundary
 * or the compilation lock. The latest refresh remains failed even if recovery
 * succeeds. Native exceptions also trigger recovery. Callbacks must not refresh
 * recursively or change the selected map. These are runtime services, not
 * author settings. changes describes effective resources in this direction:
 * old to candidate during activation, candidate to old during recovery. Its
 * owned, sorted paths are prepared before publication and borrowed only for
 * this callback. No allocation or source reread is needed to reverse it after
 * a failure. An empty change set still requires policy/consumer activation;
 * it does not certify native cache readiness or complete dependency coverage. */
typedef int (*sh_package_activation_fn)(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity);
int sh_package_runtime_refresh_activated(const char *data_root, sh_package_activation_guard guard,
    sh_package_activation_fn activate, void *context);
/* Record that the package tree gained or lost sources outside a refresh: a
 * committed whole-package installation publishes files the compiled library has
 * never scanned while a temporary map provider already supplies them. The next
 * map activation or restoration recompiles the library from disk first, in the
 * same transaction, so committed resources are not reported as removed and
 * retired. Idempotent, lock-free, and cleared only by a successful rescan. */
void sh_package_runtime_note_sources_changed(void);
/* Activate private, already validated map source trees under
 * <map_source_root>/overrides; NULL restores the retained local library.
 * Map data never changes or replaces library sources. The library is rescanned
 * here only when note_sources_changed reported a committed installation, and
 * then before the overlay is composed or retired.
 * Resource/provider and policy rollback use the same native callback contract.
 * Restoring the library also retires selected map JSON/references and policy;
 * failure restores that selection with its prior resource provider.
 * Call only at a verified native activation boundary, before parsing the map.
 * This does not decide missing-resource consent or continue a pending load. */
int sh_package_runtime_activate_map(const char *data_root, const char *map_source_root,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context);
/* A native copy/destructor only retains/releases occurrence leases. At an
 * admitted main-thread activation boundary, rebuild the serving view from
 * existing immutable compilations and retire released aliases transactionally.
 * No source folders are rescanned. map is the current document's completed
 * typed ownership: only its exact source contexts promote retained metadata
 * into authored policy. NULL preserves the last explicit map selection. Select the
 * current map JSON/references separately at the same boundary. Failed native
 * activation preserves prior serving/policy ownership and leaves work pending. */
int sh_package_runtime_activate_occurrences(const sh_package_occurrence_snapshot *map,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context);
/* Lock-free scheduling hint, not authorization to run native work. */
int sh_package_runtime_occurrences_pending(void);
/* Compile an immutable map candidate without publishing resources, changing
 * readiness, or calling native consumers. Inspect it for resource admission,
 * then activate the same bytes after consent. The caller serializes operations
 * on a handle and retains its private source tree for later saving. Preparation
 * failures affect only the returned error, never the active runtime status. */
typedef struct sh_package_map_plan sh_package_map_plan;
/* Metadata-free historical documents only: translate custom public resource
 * names using the installed authoring library. No source from the current map
 * participates, no package is installed, and no provider/native object changes.
 * Returns process-heap JSON; shipped/product names and prose stay untouched. */
char *sh_package_runtime_rebind_local_document(const char *json, size_t length,
    size_t *output_length, char *error, size_t capacity);
enum {
    SH_PACKAGE_ADMISSION_FAILED = 0,
    SH_PACKAGE_ADMISSION_READY = 1,
    SH_PACKAGE_ADMISSION_NEEDS_METADATA = 2
};
/* Admit this completed document's prepared native resource closure and any
 * prepared whole installed authoring increment into the current serving view.
 * Existing bytes, map provider and selected map policy remain unchanged. The
 * installed increment does not create gameplay-use ownership. The callback sees only
 * additions; shared settings/scoped strings use its separate metadata contract.
 * document must already retain the plan's exact occurrence leases and survive
 * native placement. Releasing it only schedules retirement at a later admitted
 * boundary. No source scan, owner exclusion or temporary map switch occurs.
 * NEEDS_METADATA means a shared editor contribution needs an upstream private
 * identity or explicit native metadata operation; no package is rejected and
 * no publication/callback has occurred. Failed native activation restores the
 * old provider and invokes the callback with reversed changes for recovery. */
int sh_package_runtime_activate_prepared_occurrences(sh_package_map_plan *plan,
    const sh_package_occurrence_snapshot *document, sh_package_activation_guard guard,
    sh_package_activation_fn activate, void *context, char *error, size_t capacity);
typedef int (*sh_package_occurrence_finalize_fn)(void *context, char *error, size_t capacity);
/* Optional final install/staging commit inside the same provider transaction.
 * finalize runs only after activate succeeds and has restored all native scopes.
 * It must perform no fallible native work after irreversible disk commit; prepare
 * that work in activate. Refusal/exception invokes normal native recovery with
 * the previous provider restored. Successful finalization publishes the plan's
 * already-prepared installed library and inventory together without allocation.
 * A superseded library baseline refuses before native work. Neither callback may
 * call another runtime mutation/commit API while this transaction owns its lock. */
int sh_package_runtime_admit_occurrences(sh_package_map_plan *plan,
    const sh_package_occurrence_snapshot *document, sh_package_activation_guard guard,
    sh_package_activation_fn activate, sh_package_occurrence_finalize_fn finalize,
    void *context, char *error, size_t capacity);
/* Complete a consented source installation after native staging/placement,
 * without pretending its plan replaced the active map. Checks the exact plan
 * leases and all still-served authoring/occurrence bytes before commit; library
 * and inventory publication afterward cannot allocate or fail. Does not change
 * the current map selection. */
int sh_package_runtime_commit_occurrences(sh_package_map_plan *plan,
    const sh_package_occurrence_snapshot *document,
    int (*commit)(char *error, size_t capacity), char *error, size_t capacity);
/* Only the retained native callback owner may acknowledge completed recovery
 * after its immediate additive rollback failed. Requires the same restored
 * provider and exact callback context; never overrides arbitrary readiness. */
int sh_package_runtime_complete_occurrence_recovery(void *context);
/* Attach exact provider leases before a newly prepared snapshot is published.
 * Normal native capture performs this automatically. The plan/current provider
 * must still be stable, and every pair must exist in its bound source catalog.
 * Does not acquire/resolve resources or accept author-supplied identities. */
int sh_package_runtime_bind_occurrence_resources(const sh_package_map_plan *plan,
    sh_package_occurrence_snapshot *snapshot, char *error, size_t capacity);
/* Native selectors borrow roots in their caller-specified native family and
 * the complete bound dependency closure of those roots. Retain the immutable
 * serving view before publishing native rows,
 * including during the activation callback. This ownership keeps exact native
 * resources served across refresh/map departure without recording gameplay use
 * or promoting its requirements, HUD, or legacy strings. Unrelated resources
 * in that provider are not selector borrows. The family is a product-owned
 * native contract, never an author setting. It is not a source
 * snapshot for portable documents; their occurrence leases remain separate.
 *
 * Release only after positive retirement of every native selector using this
 * hold, including any copied old palette rows. Failed/quarantined teardown must
 * keep it. Release consumes the handle and schedules an admitted update; it
 * performs no engine calls, resource release or provider locking itself.
 * Occupied shared slots keep their old bytes during the full palette replacement
 * pass. After the last hold is released, a second ordinary activation publishes
 * the current library bytes and retires unused private aliases. A map's explicit
 * gameplay binding is never silently replaced to satisfy a different selector.
 */
typedef struct sh_package_selector_hold sh_package_selector_hold;
sh_package_selector_hold *sh_package_runtime_selectors_retain(const char *family, char *error, size_t capacity);
void sh_package_runtime_selectors_release(sh_package_selector_hold *hold);
sh_package_map_plan *sh_package_runtime_prepare_map(const char *data_root,
    const char *map_source_root, char *error, size_t capacity);
/* Non-NULL bindings reconstruct recorded source contexts. NULL preserves the
 * historical map compiler; installed local variants never infer a new mode. */
sh_package_map_plan *sh_package_runtime_prepare_map_with_bindings(const char *data_root,
    const char *map_source_root, const sh_package_provenance *bindings, char *error, size_t capacity);
/* Importer-verified originals must be supplied before scopes are compiled.
 * Each row matches one normalized inventory owner, but portable contexts use
 * its intact authored tree. This distinguishes different original deliveries
 * even when migration produces the same compiler bytes. NULL bindings keeps
 * the historical compilation recipe; deliveries must still cover every owner. */
sh_package_map_plan *sh_package_runtime_prepare_map_deliveries(const char *data_root,
    const char *map_source_root, const sh_package_provenance *bindings,
    const sh_package_bundle_binding *deliveries, size_t count, char *error, size_t capacity);
/* Rebind saved typed references to the prepared names. Returns a process-heap
 * buffer; native byte strings outside reference sites remain unchanged. */
char *sh_package_map_plan_rebind(const sh_package_map_plan *plan, const sh_package_provenance *saved,
    const char *json, size_t length, size_t *output_length, char *error, size_t capacity);
/* Borrowed immutable compilation, valid until plan_free. No runtime acquire or
 * release is required; this candidate is not installed-library availability. */
const sh_package_compilation *sh_package_map_plan_compilation(const sh_package_map_plan *plan);
struct sh_mpkg_used;
/* Walk actual typed sites against one stable provider and retain their exact
 * authored sources. NULL plan uses the active authoring view. Additional rows
 * are immutable sources already held by copied native objects; they do not
 * authorize borrowing values from an unrelated provider. saved may be NULL.
 * Native map prose is preserved byte-for-byte apart from verified references.
 * The returned JSON uses malloc; ownership is an owned document snapshot.
 * Optional deliveries/count request complete used bundles from this same walk.
 * Run at a settled native editor boundary, outside an acquired runtime view. */
char *sh_package_runtime_capture_document(const sh_package_map_plan *plan,
    const sh_package_provenance *saved, const sh_package_bundle_binding *additional,
    size_t additional_count, const char *json, size_t length, size_t *output_length,
    sh_package_occurrence_snapshot **ownership, struct sh_mpkg_used **deliveries,
    size_t *delivery_count, char *error, size_t capacity);
/* Atomically attach every original retained delivery to its exact source owner
 * and normalized compiler tree. The originals must match the identity already
 * bound at preparation; use prepare_map_deliveries for normalized imports.
 * Only an unpublished plan may be changed; failure retains its prior ownership. */
int sh_package_map_plan_attach_bundles(sh_package_map_plan *plan,
    const sh_package_bundle_binding *bindings, size_t count,
    char *error, size_t capacity);
/* Compiler-side native binding output, supplied after scoped linking and before
 * activation. Every alias must name a material in this immutable compilation.
 * No author metadata or live lookup may decide these rows. Repeated preparation
 * replaces the unpublished view atomically; active/shared plans are immutable. */
int sh_package_map_plan_bind_virtual_materials(sh_package_map_plan *plan,
    const sh_virtual_material_runtime_binding *bindings, size_t count,
    char *error, size_t capacity);
/* Capture the complete installed tree after a consented disk publication and
 * compile its new authoring components in private source contexts. Preserve
 * every existing library binding and retain all new authored roots, including
 * unused siblings. The candidate library and inventory stay private until the
 * native/staging and disk transaction succeeds. This does not record gameplay
 * use. Failure leaves the published owners and the previous plan intact. */
int sh_package_map_plan_prepare_inventory(sh_package_map_plan *plan,
    const char *data_root, char *error, size_t capacity);
/* After successful native loading, finish its disk transaction and publish
 * the already-prepared authoring library and inventory together. A refused or
 * superseded map leaves both old owners intact. commit performs no engine calls or runtime refresh;
 * it returns 1 only after its complete install is committed. No allocation or
 * source scan follows that callback. NULL plan denotes an active vanilla map. */
int sh_package_runtime_commit_map(sh_package_map_plan *plan,
    int (*commit)(char *error, size_t capacity), char *error, size_t capacity);
/* Read-only availability against the retained installed library and verified
 * SnapMap/product originals. Campaign-only baseline data does not establish
 * availability in SnapMap. Map-private data can never satisfy this check.
 * Resolved dependency paths and native readiness remain the caller's concern. */
int sh_package_map_plan_missing(const sh_package_map_plan *plan,
    const char *const *required_paths, size_t count,
    sh_package_missing *out, char *error, size_t capacity);
/* Check the full compiled payload against that same local-only view. Zero
 * missing files avoids a content-based reinstall without claiming the map's
 * author supplied every possible engine dependency. Does not activate it. */
int sh_package_map_plan_payload_missing(const sh_package_map_plan *plan,
    sh_package_missing *out, char *error, size_t capacity);
/* Resolve known paths against the candidate's inventory/catalog. Graph and
 * inline observations remain separately scoped as described by package_usage;
 * this does not certify that native observations match candidate source bytes. */
int sh_package_map_plan_resources(const sh_package_map_plan *plan,
    const char *json, size_t length, const sh_package_references *references,
    sh_package_references *out);
/* Inspect explicit dependencies in this candidate and original game/product
 * baselines. Local override values cannot supply missing authored source.
 * Uses only source bytes and read-only native metadata; never loads resources
 * or changes the observed native graph. Unsupported source readers remain gaps. */
int sh_package_map_plan_source_resources(const sh_package_map_plan *plan,
    sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_references *out,
    sh_package_source_graph_report *report, char *error, size_t capacity);
/* Admission checks only the supported gameplay-use closure, including typed
 * dependencies and their required files. Complete delivery bundles remain
 * intact. Incomplete source coverage returns failure with an empty missing set,
 * never installation success or a prompt to reinstall unrelated bundle files.
 * A successful inspection retains its paths and map-byte identity for later
 * inventory rescans. Failed inspection invalidates that snapshot. Successful
 * selection of changed map bytes or uncovered observed resources also
 * invalidates it; continuing that provider after a rescan needs reinspection.
 * Exiting to vanilla still uses native activation/retirement ownership.
 * This does not establish native activation/late-lookup readiness. */
int sh_package_map_plan_usage_missing(const sh_package_map_plan *plan,
    sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_missing *out,
    sh_package_source_graph_report *report, char *error, size_t capacity);
/* NULL restores the library. A non-NULL plan is retained by successful
 * activation; the caller can free its handle afterward, including on failure.
 * Sources are rescanned only for a reported committed installation, as in
 * activate_map. The same native boundary/rollback contract applies. */
int sh_package_runtime_activate_prepared_map(sh_package_map_plan *plan,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context);
void sh_package_map_plan_free(sh_package_map_plan *plan);
/* Acquire only the installed library, excluding temporary map resources.
 * Pair with runtime_release. This is the input to local availability checks. */
const sh_package_compilation *sh_package_runtime_library_acquire(void);
int sh_package_runtime_has_map_provider(void);
/* Editor inspectors read and extend the serving generation's spawn-type key
 * codes (the map's while one serves, else the library's). decode gives the
 * keys a value's private bits name (a marked code only the other table
 * defines is read there) and the bits the generation keeps historically. code
 * gives the one code for a set of keys, adopting a new group code into that
 * generation's table and publishing it as a document capture does: 1 with
 * *code, 0 when no code is left, -1 while a refresh holds the providers. */
void sh_package_runtime_key_decode(uint32_t value, sh_package_key_set *keys, uint32_t *historical);
int sh_package_runtime_key_code(const sh_package_key_set *keys, uint32_t *code);
/* Bind verified startup metadata addresses once before the first refresh.
 * Native status is 1 after reader registration, 0 initializing, -1 invalid.
 * These are product startup services, not package author controls. */
void sh_package_runtime_bind_native(uintptr_t accessor, uintptr_t type_container,
    uintptr_t game_system_export);
int sh_package_runtime_native_status(void);
/* Whether the latest refresh succeeded. A failed attempt retains the entire
 * previous compilation, but must not admit a newly installed package. */
int sh_package_runtime_ready(void);
/* Whether a previously activated provider is still usable. A rejected local
 * authoring composition does not invalidate a successful provider or prevent
 * preparing an independent map. False during activation or failed recovery. */
int sh_package_runtime_admission_ready(void);
/* True only before the first compilation/publish attempt has produced either
 * a provider or a failure. Native editor bootstrap objects are vanilla then. */
int sh_package_runtime_uninitialized(void);
void sh_package_runtime_error(char *out, size_t capacity);

/* Acquire the current provider (prospective during activation, previous after
 * failure). Pair acquire/release even when no snapshot exists. No engine
 * callbacks while holding a snapshot; copy data before entering native code. */
const sh_package_compilation *sh_package_runtime_acquire(void);
void sh_package_runtime_release(void);
/* Valid only while the shared compilation is acquired. */
const sh_resource_catalog *sh_package_runtime_catalog(void);
const sh_package_provenance *sh_package_runtime_bindings(void);
/* While the current compilation is acquired, retain this exact owner/tree's
 * complete authored delivery. The returned handle outlives that acquire,
 * provider retirement and editable folder changes. No descriptor-ID match or
 * current-directory fallback is permitted. Caller releases the handle. */
sh_package_bundle *sh_package_runtime_source_bundle(size_t owner,
    const unsigned char compiler_tree[32]);
/* Copy the compiled sound's bound Wwise event under the provider lock. Native
 * callbacks retain no resource/source pointer; absent names clear the output. */
int sh_package_runtime_sound_event(const char *name, uint32_t *event);
/* Audio programs served by the published view, in precedence order: current
 * map, local library, then retained occurrence and selector owners. Each row
 * retains its owning provider (and so its program, ID lease and originals)
 * until sh_package_runtime_audio_release. The array itself is caller-owned. */
typedef struct sh_package_runtime_audio {
    void *owner;
    sh_package_audio_program *program;
    sh_audio_id_lease *lease;
} sh_package_runtime_audio;
int sh_package_runtime_audio_programs(sh_package_runtime_audio **rows, size_t *count,
    char *error, size_t capacity);
void sh_package_runtime_audio_release(void *owner);
/* Native Init shared controls from the published provider's installed originals. */
int sh_package_runtime_audio_baseline(sh_audio_shared_policy *out, char *error, size_t capacity);
/* Installed audio identity used to mirror native localized-before-root bank
 * lookup. Acquires its own provider lock; call outside an acquired snapshot. */
int sh_package_runtime_audio_original(const char *path, sh_package_original_identity *out,
    char *error, size_t capacity);
/* The effective installed original bytes and the packaged bank identities
 * behind that same order, for building a native activation catalog. Both take
 * the provider lock; neither enters native code or copies a media payload. */
int sh_package_runtime_audio_originals_ready(void);
/* Callback borrows the current provider's immutable metadata under its lock.
 * It must not reenter runtime APIs or retain these pointers after returning. */
int sh_package_runtime_audio_original_banks(const char *language, const char *bank_prefix,
    sh_audio_originals_inventory_visit visit,
    void *context, char *error, size_t capacity);
int sh_package_runtime_audio_original_read(const char *path, uint64_t offset, void *out,
    size_t span, uint64_t *length, char *error, size_t capacity);
/* Read-only enumeration of every registered declaration family and the
 * composition route the compiler takes for it: the engine's reflected state
 * type, one of the verified custom adapters, or none. Opens its own
 * metadata-only schema and loads no declaration. Returns the number of families
 * reported, or -1 with error set. */
typedef void (*sh_package_family_visit)(void *context, const char *type,
    const char *state, const char *route);
int sh_package_runtime_declaration_families(sh_package_family_visit visit, void *context,
    char *error, size_t capacity);

int sh_package_runtime_audio_packaged_banks(sh_audio_originals_bank_visit visit, void *visitor,
    char *error, size_t capacity);

/* Read a malloc-owned copy. 1 = claimed, 0 = not owned, -1 = owned but failed. */
int sh_package_runtime_read(const char *engine_path, unsigned char **body, size_t *length);
/* Open an opaque resource's verified immutable cache file without copying it
 * into memory. Caller owns fclose. Zero means no opaque resource matched;
 * compiled declarations still use read. -1 refuses an owned but invalid file. */
int sh_package_runtime_open_file(const char *engine_path, FILE **stream, uint64_t *length);
/* Resolve a previously published decltree alias through the current snapshot.
 * A removed replacement uses its compiled SnapMap restoration entry. Return
 * 1 with owned bytes, or -1 for an absent/failed provider or retired identity.
 * Never fall back to historical publication bytes. */
int sh_package_runtime_read_decl_alias(const char *alias, unsigned char **body, size_t *length);
/* Current compiled declaration presence only; no body copy or native load. */
int sh_package_runtime_decl_alias_exists(const char *alias);
/* Current provider identity set, including aliases sharing physical bytes.
 * This does not assert that a native object has finished loading. */
int sh_package_runtime_serves_native_identity(const char *type, const char *name);
char *sh_package_runtime_policy(const char *section, size_t *length);
/* Select from typed map resources, after delivery data has been removed.
 * NULL/zero clears the map. Failure preserves the last successful selection.
 * References are copied from the native preparation pass. Refresh resolves
 * those identities and the retained map again, never reuses old owner indices.
 * No engine callbacks occur; callers publish at a map load/save boundary. */
int sh_package_runtime_select_map(const char *json, size_t length,
    const sh_package_references *references, char *error, size_t capacity);
/* Policy-only selection at an editor save/play boundary. Exact typed source
 * contexts choose metadata from retained original inventories, even when equal
 * native objects have other suppliers. No native resources are published or
 * retired; the next admitted activation incorporates the pending promotion.
 * document is the completed ownership walk of these bytes, never a catalog. */
int sh_package_runtime_select_map_occurrences(const char *json, size_t length,
    const sh_package_references *references, const sh_package_occurrence_snapshot *document,
    char *error, size_t capacity);
/* Malloc-owned policy for the selected map, or an empty object without a map.
 * Editor strings and resource-registration gates use the installed policy
 * above because they also prepare assets outside the active gameplay map. */
char *sh_package_runtime_active_policy(const char *section, size_t *length);
/* Human-readable inventory, malloc-owned. Show after releasing the snapshot. */
char *sh_package_runtime_summary(void);

#ifdef SH_PACKAGE_RUNTIME_TESTING
#include "package_source_link.h"
/* Capture-only adapter for the synthetic native reader fixtures. */
void sh_package_runtime_test_constructor_requests(sh_package_source_implicit capture, void *context);
/* The historical bindings a map without recorded provenance receives. */
int sh_package_runtime_test_legacy_bindings(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_provenance *out);
int sh_package_runtime_test_consumer_changes(const sh_package_consumers *before,
    const sh_package_consumers *after, sh_package_changes *changes);
int sh_package_runtime_test_native_changes(const sh_package_compilation *before,
    const sh_package_compilation *after, const sh_resource_catalog *catalog,
    sh_package_consumers *old_consumers, sh_package_consumers *new_consumers,
    sh_package_changes *changes, char *error, size_t capacity);
#ifdef SH_PACKAGE_RUNTIME_NATIVE_TESTING
struct sh_package_native_assets;
struct sh_package_shader_preparation;
/* Focused host fixture for the production lock/lifetime/retry path. Native
 * boundary callbacks are supplied by the fixture; queue and receipts are real. */
int sh_package_runtime_test_native_ready(void);
int sh_package_runtime_test_native_run(struct sh_package_shader_preparation *batch,
    char *error, size_t capacity);
int sh_package_runtime_test_prepare_native_sources(struct sh_package_native_assets *assets,
    uint64_t **completed, size_t *completed_count, char *error, size_t capacity);
#endif
void sh_package_runtime_test_audio_originals(const char *root, const wchar_t *language);
void sh_package_runtime_test_empty_catalog(void);
void sh_package_runtime_test_baseline(sh_package_baseline_reader reader, void *context);
/* Opt-in real context linking/capture against immutable synthetic native readers.
 * This small-schema fixture omits baked product defaults during preparation.
 * Passing a zero source disables the seam and preserves older offline fixtures. */
void sh_package_runtime_test_source_readers(sh_decl_registry_source registry, uintptr_t reflection);
/* Link the installed library through those readers, as production does, so
 * owner isolation and unserved coverage run end to end. Off by default. */
void sh_package_runtime_test_installed_links(int enabled);
void sh_package_runtime_test_spawn_keys(int enabled);
int sh_package_runtime_test_source_inputs(sh_decl_registry_source *registry, uintptr_t *reflection,
    sh_package_baseline_reader *baseline, void **baseline_context);
/* 1 when the spawn-key group tables last published are the serving
 * providers' (the installed library's and the active map's). */
int sh_package_runtime_test_key_groups_current(void);
/* Give the serving providers these group tables (copies) and publish them. */
void sh_package_runtime_test_key_tables(const sh_package_key_groups *library, const sh_package_key_groups *map);
void sh_package_runtime_test_plan_key_table(sh_package_map_plan *plan, const sh_package_key_groups *table);
int sh_package_runtime_test_unmodified_type(uint32_t type);
void sh_package_runtime_test_serving_key_table(sh_package_key_groups *table);
/* Offline fixture teardown only. No engine callbacks or native game reset. */
void sh_package_runtime_test_dispose(void);
#endif

#endif
