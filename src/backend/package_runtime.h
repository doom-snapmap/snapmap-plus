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

typedef struct sh_package_selector_hold sh_package_selector_hold;
typedef struct sh_package_resource_capture sh_package_resource_capture;
/* Snapshot the immutable provider at the actual native source-read boundary,
 * before a lookup/parse that could outlive publication. This does not retain a
 * serving root. Return 1 for an exact compiled resource, 0 when this source
 * view does not supply it, -1 when unavailable or invalid. A zero result alone
 * is not proof of native baseline; the native adapter must establish that.
 * Unpublished private native preparation returns -1; preserve existing source
 * associations rather than replacing them with a current-provider guess.
 * Start *out NULL; release each capture once, after all module-side references. */
int sh_package_runtime_resource_capture(const char *family, const char *name,
    sh_package_resource_capture **out, char *error, size_t capacity);
void sh_package_runtime_resource_capture_release(sh_package_resource_capture *capture);
sh_package_resource_capture *sh_package_runtime_resource_capture_retain(sh_package_resource_capture *capture);
/* Captured read/open APIs atomically retain the exact opening source. Transfer
 * this hold once to its stream, then release after native destruction.
 * In the captured read/open APIs, both family/name NULL capture the exact
 * served file path and its bound dependencies, without inventing a native
 * declaration identity. Named captures continue to verify native identity.
 * Metadata capture has no opening hold. An untransferred hold is released with
 * the last capture reference. Observation retains metadata independently. */
sh_package_selector_hold *sh_package_runtime_resource_capture_take_opening_hold(sh_package_resource_capture *capture);
/* Initial native census is valid only before any private native preparation.
 * Private preparation must not publish ordinary source-read associations. */
int sh_package_runtime_resource_pristine(void);
int sh_package_runtime_resource_observation_allowed(void);
/* Exact native identity of a source path, from the compiled/catalog indexes
 * or the established native declaration/cooked path grammar. This is only a
 * path association, not evidence that any bytes were read. */
int sh_package_runtime_resource_path_matches(const char *path, const char *family, const char *name);
/* Promote a captured root into an exact serving lease without resolving its
 * name against a newer provider. Its complete bound closure must still be
 * served byte-for-byte; a snapshot cannot revive an already-retired native
 * object. Return 1 on success, -1 on failure with *out unchanged (initially
 * NULL). Native field/object retirement precedes selectors_release(*out). */
int sh_package_runtime_resource_promote(const sh_package_resource_capture *capture,
    sh_package_selector_hold **out, char *error, size_t capacity);
typedef struct sh_package_consumer_hold {
    const sh_package_consumer *binding;
    void *provider;
    sh_package_selector_hold *lifetime;
} sh_package_consumer_hold;
/* Capture the currently served owner's immutable constructor plan, including
 * during activation/recovery. Return 1 with a retained snapshot, 0 for an owner
 * outside this catalog, -1 for invalid arguments. Start out zeroed; do not copy
 * or overwrite an acquired hold. Lookup needs no allocation or native call.
 * Retaining a plan does not itself admit an engine load or preserve all files
 * in the active view: the native consumer still needs its ordinary lifetime
 * lease. Release after the synchronous constructor/consumer request completes. */
int sh_package_runtime_consumer_acquire(const char *family, const char *name, sh_package_consumer_hold *out);
/* Atomically capture that same plan and retain this exact native owner and its
 * bound resource closure in subsequent serving views. No siblings, gameplay
 * use or global policy are promoted. Call before native construction publishes
 * the owner, outside an acquired runtime view; activation callbacks may call
 * it. Return 0 only for an owner outside the catalog, -1 for failure (out stays
 * empty). Release only after native destruction and outstanding consumers have
 * positively retired. A failed teardown must keep the hold. */
int sh_package_runtime_consumer_retain(const char *family, const char *name,
    sh_package_consumer_hold *out, char *error, size_t capacity);
void sh_package_runtime_consumer_release(sh_package_consumer_hold *hold);
/* Movie plans and serving-root records are allocated/validated before provider
 * publication. These operations only borrow stable records and change counts;
 * they allocate nothing and enter no native code. Canonical names are the
 * generated .bswf filename the native constructor derives; callers project a
 * native swf/<stem>.swf spelling first. Do not copy an entered hold. */
typedef struct sh_package_movie_hold {
    const sh_package_consumer *binding;
    void *token;
} sh_package_movie_hold;
/* Constructor: one instance plus one entered frame. 0 is an unowned stock
 * movie; 1 borrows the exact plan; -1 is an invalid API/bookkeeping invariant.
 * Resource admission fails before publication, never in this constructor path.
 * Failure never changes out. No refresh lock is acquired. */
int sh_package_runtime_movie_begin(const char *name, sh_package_movie_hold *out);
/* Callback/destructor: one entered frame for an already-live instance. */
int sh_package_runtime_movie_borrow(const char *name, sh_package_movie_hold *out);
/* End the instance only after successful destruction or completed constructor
 * unwind. The adapter pairs a constructor's unwind destructor exactly once. */
void sh_package_runtime_movie_end_instance(const sh_package_movie_hold *hold);
/* Release the entered frame; the plan stays until all instances/frames finish. */
void sh_package_runtime_movie_release(sh_package_movie_hold *hold);
/* The native font identity (idFont+0x4c hash) a private font form carries:
 * that of the request every live movie plan routes to it. Live means a
 * published or still-borrowed movie provider, so the answer ends with the
 * last plan's retirement. 1 with *hash, 0 when no live plan routes to it, -1
 * when live plans disagree (compiled forms cannot; font semantics). No
 * allocation or native call occurs. */
int sh_package_runtime_font_identity(const char *font, int *hash);
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
/* One owning-storage finish per serialized activation callback. Register while
 * the callback is running, with an owned context that survives its return and
 * exceptions. Reject runs before previous-provider consumer recovery; accept
 * runs after material status, FSM binding and any occurrence finalizer.
 * accepted=2 is a read-only preflight before an irreversible disk finalizer;
 * return readiness without consuming context or setting *committed.
 * Return 1 when consumed, 0 when admission/restoration is still owed, -1 for
 * damaged native storage whose cleanup must never be replayed. Set *committed
 * before irreversible accepted cleanup. A cleanup failure after that latch
 * keeps the candidate provider and disables native use; it cannot roll back
 * storage already freed. Duplicate, stale and recursive registration refuse. */
typedef int (*sh_package_activation_finish_fn)(void *context, int accepted,
    int *committed, char *error, size_t capacity);
int sh_package_runtime_defer_activation_finish(sh_package_activation_finish_fn finish, void *context);
/* A native source stream's destructor did not return. Its opening holds stay
 * owned by the stream quarantine; partial cleanup must never be replayed.
 * Blocks further activation in this process without disturbing served bytes. */
void sh_package_runtime_source_cleanup_failed(void);
/* Takes a failed source witness, including its opening hold. Native consumption
 * may have escaped observation, so activation stays fenced until process exit. */
void sh_package_runtime_source_capture_failed(sh_package_resource_capture *capture);
int sh_package_runtime_refresh_activated(const char *data_root, sh_package_activation_guard guard,
    sh_package_activation_fn activate, void *context);
/* Record that the package tree gained or lost sources outside a refresh: a
 * committed whole-package installation publishes files the compiled library has
 * never scanned while a temporary map provider already supplies them. The next
 * map activation or restoration recompiles the library from disk first, in the
 * same transaction, so committed resources are not reported as removed and
 * retired. Idempotent, lock-free, and cleared only by a successful rescan. */
void sh_package_runtime_note_sources_changed(void);
/* Receives the decl names of the published compilation's generated editor
 * rows, which exist for lookup only (R-255 variant rows), and of the rows an
 * equal exposure stands for, at registration and at every publication, under
 * the runtime lock. The names are borrowed for the call; NULL names with a
 * nonzero count report an allocation failure. */
typedef void (*sh_package_runtime_rows_sink)(const char *const *names, size_t count);
void sh_package_runtime_lookup_rows_sink(sh_package_runtime_rows_sink sink);
/* When the published selection leaves the editor row name out of the pickers
 * for an equal exposure, writes the row they list for it: 1 written; 0 when
 * name stays listed, or kept is too small (then empty). Any thread. */
int sh_package_runtime_exposure_kept(const char *name, char *kept, size_t capacity);
/* A published installation that no map load continues (the MPKG gate) asks
 * the next refresh to fold its packages in as an installation does: the
 * added packages compile alone and the retained library serves the rest. A
 * refresh that finds any installed package removed or changed, or whose
 * increment does not complete, compiles the whole library instead. Other
 * refreshes compile the whole library, except one the installed-package
 * watcher asked for (below). */
void sh_package_runtime_note_install_published(void);
/* Around a refresh the installed-package watcher asked for (1 before, 0
 * after): that refresh reuses the library already served when a fresh scan of
 * the installed packages (accepted packages' names and source trees, and the
 * scan's own rejections) is the one that library was prepared from, so an
 * installation's own folder, committed or rolled back, is no change. The note
 * is consumed by that refresh and never outlives the call. */
void sh_package_runtime_note_watched_change(int watched);
/* sh_user_overrides off for this launch: no user package is served, so a
 * native document carries no package ownership of its own. A document that
 * names package uses is refused rather than run without its packages. Set
 * once at startup, before any map loads. */
void sh_package_runtime_set_serving_disabled(int disabled);
int sh_package_runtime_serving_disabled(void);
/* The last activation's phases in milliseconds (library, view, publish,
 * native activate, evict, total) and its view build's own phases and
 * counters: retained occurrence leases and native selector holds listed,
 * live, and distinct providers retained. Diagnostics for a departure line. */
typedef struct sh_package_activation_stats {
    int change_map, ok;
    double library_ms, view_ms, publish_ms, activate_ms, evict_ms, total_ms;
    /* Within publish (all ours): movie records, the movie rollback view (its
     * clone, retained link and diff), the native boundary guard's entry, and
     * the policy, state-machine and material steps under the lock. */
    double movies_ms, rollback_ms, rollback_clone_ms, rollback_link_ms, guard_ms, locked_ms;
    /* Within native activate: our provider publication, the activation
     * callback (the declaration pass handed the engine) and the state-machine
     * bind; the rows handed over (changed resources and rebound identities). */
    double served_ms, callback_ms, bind_ms;
    size_t changed_rows, rebound_rows;
    /* R-396: this activation also withdrew a departure the map's exit deferred. */
    int departure_withdrawn;
} sh_package_activation_stats;
typedef struct sh_package_view_stats {
    double keys_ms, overlay_ms, bundles_ms, joins_ms, leases_ms, selectors_ms, spawn_ms, strings_ms;
    size_t leases, leases_live, lease_providers, selectors, selectors_live, selector_providers, resources, files;
    double file_map_ms;      /* retained providers' files matched to the view's (within the two retained phases) */
    size_t file_compares;    /* view files those matches compared */
    /* Within the two retained phases, per step of each provider's link:
     * selection, sources, files, rows, projection, policy, filters, bindings,
     * install; the rows a link carried and the equality checks it made. */
    double link_ms[9];
    size_t link_rows, link_compares;
    size_t selectors_released; /* released holds this build unlinked before listing */
} sh_package_view_stats;
void sh_package_runtime_activation_stats(sh_package_activation_stats *activation, sh_package_view_stats *view);
/* The last activation's phases as one line's text (map change, outcome, the
 * view's retained phases, publish, native activate, evict), for whichever
 * route ran it: a map's departure, a retained transition at the native map
 * boundary or the browser's retirement of released retained resources.
 * Returns the length written. */
size_t sh_package_runtime_activation_summary(char *out, size_t capacity);
/* A settled save boundary: between enter (nonzero) and the matching leave,
 * an original declaration read by path is read once per source (the catalog,
 * native assets and baked declarations do not change until it ends; a cooked
 * shader program is always read), and the engine's source answers are reused
 * (sh_decl_native_registry_scope). Nested scopes count. Main thread. */
void sh_package_runtime_save_scope(int enter);
/* Original reads since the last reset: reads asked, reads a save scope
 * answered again, and the milliseconds spent in the cooked shader program
 * lookup and in the game catalog. Diagnostics for a caller's timing line. */
typedef struct sh_package_runtime_read_stats {
    size_t originals, answered;
    double shader_ms, catalog_ms;
} sh_package_runtime_read_stats;
void sh_package_runtime_read_stats_take(sh_package_runtime_read_stats *out, int reset);
/* Milliseconds since the last reset in a save's phases: waiting for the
 * runtime locks before a capture and before a policy selection; a capture's
 * provider rows, reader open, binding walk (sh_package_map_uses_select_local),
 * ownership document and deliveries; a selection's policy and requirement
 * check. Diagnostics for a caller's timing line. */
typedef struct sh_package_runtime_phase_stats {
    double capture_lock_ms, rows_ms, reader_ms, walk_ms, document_ms, deliveries_ms;
    double select_lock_ms, select_policy_ms, select_requirements_ms;
} sh_package_runtime_phase_stats;
void sh_package_runtime_phase_stats_take(sh_package_runtime_phase_stats *out, int reset);
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
/* Activations that found the served provider unchanged and kept it instead
 * of running the activation callback (diagnostics, tests). */
long sh_package_runtime_activations_skipped(void);
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
/* R-255: the local first load: as rebind_local_document, and *plan (NULL
 * when none is needed) is a private map plan, with no private sources, serving
 * the parent variants the bound document names; publish it as any prepared
 * map before the native read. */
struct sh_package_map_plan;
char *sh_package_runtime_prepare_local_document(const char *data_root,
    const char *json, size_t length, size_t *output_length, struct sh_package_map_plan **plan,
    char *error, size_t capacity);
/* The parent form a capture reads of a live document naming served variants,
 * or NULL (*changed 0) when it names none. malloc-owned. */
char *sh_package_runtime_fold_document(const char *json, size_t length, size_t *output_length, int *changed);
/* 1 when bound is the capture of an unchanged live document: the document
 * itself, or its parent form when it names served parent variants (R-255).
 * Attach, Play and native construction all accept exactly this. */
int sh_package_runtime_capture_unchanged(const char *json, size_t length, const char *bound, size_t bound_length);
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
/* The same route for a map document. Without recorded bindings, a legacy map's
 * own typed values name the stock identities its historical context derives
 * (for example a sound whose event a delivered bank changes, or an entity that
 * reaches a delivered material); only its sites can select them. */
sh_package_map_plan *sh_package_runtime_prepare_map_document(const char *data_root,
    const char *map_source_root, const sh_package_provenance *bindings,
    const sh_package_bundle_binding *deliveries, size_t count,
    const char *json, size_t length, char *error, size_t capacity);
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
/* Save-time native map representation (R137). Captures the live document,
 * derives one whose eligible package entity uses are native inline state
 * (package_native_inline.h), and captures that derived document to a fixed
 * point: a delivery the derived map would drop keeps its uses when it also
 * changes stock gameplay definitions globally. Returns 1 with the derived
 * document and its own ownership and complete deliveries; 0 when no use
 * converts; -1 on failure. Outputs change only on 1. *report (initially NULL)
 * receives malloc-owned per-site decisions whenever sites were examined. The
 * live editor keeps its package binding; only saved bytes are derived. */
int sh_package_runtime_native_document(const sh_package_map_plan *plan,
    const sh_package_provenance *saved, const sh_package_bundle_binding *additional,
    size_t additional_count, const char *json, size_t length,
    char **derived, size_t *derived_length, sh_package_occurrence_snapshot **ownership,
    struct sh_mpkg_used **deliveries, size_t *delivery_count, char **report,
    char *error, size_t capacity);
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
/* The complete-tree bundle the published library captured at its compile for the installed package whose tree is
 * `fingerprint`, retained for the caller (sh_package_bundle_release); NULL when the library holds no such tree. */
struct sh_package_bundle *sh_package_runtime_installed_bundle(const unsigned char fingerprint[32]);
int sh_package_runtime_has_map_provider(void);
/* R-396: a map's exit leaves the departed map's provider served, complete, and registers nothing; the next
 * activation of any kind (a map's, a refresh's, a retained transition's) withdraws it in its own pass. Returns 1
 * when the withdrawal is deferred (or already was); 0 with the reason when the exit must activate at once: no map
 * provider, an unusable provider, or a departed map serving a string of the engine's own dictionary differently
 * from the library (the browser renders those). Main thread, at the browser. */
int sh_package_runtime_defer_departure(char *reason, size_t capacity);
/* Whether a deferred departure still waits for its withdrawal. */
int sh_package_runtime_departure_deferred(void);
/* R-398: build, without registering it, the view a deferred departure's
 * withdrawal will leave (the library with the departed map's held rows), so a
 * map opening over the deferred departure reads it in its preparation scope
 * and its own activation withdraws the departure in the same registration.
 * 1 when staged (or nothing is deferred), 0 with error when it cannot be. */
int sh_package_runtime_stage_departure(char *error, size_t capacity);
int sh_package_runtime_departure_staged(void);
/* Whether the browser's retirement of released consumers' retained rows may wait for the next map boundary
 * (R-398); 0 with the reason when it must run now. */
int sh_package_runtime_retirement_deferrable(char *reason, size_t capacity);
/* Around a map's preparation on this thread (1 enter, 0 leave): the staged
 * departure view is what its reads see. */
void sh_package_runtime_preparation_scope(int enter);
/* R-400: start the boot worker at process attach (no engine state exists yet). It opens the installed catalog and
 * answers the newest warm record's catalog reads, so the start inside the engine's boot promotion does not read
 * them again. data_root is the package data root the start will use. No-op in tests. */
void sh_package_runtime_boot_prefetch_start(const char *data_root);
/* R-400: inside the boot promotion of a boot whose first registration waits for the menu, prepare the shader
 * sources the installed packages' hint names (engine thread; nothing is compiled or registered) and keep them for
 * the first start. 0 with why when there is no hint or a preparation failed. */
int sh_package_runtime_boot_prepare_shaders(const char *data_root, char *why, size_t capacity);
#ifdef SH_PACKAGE_RUNTIME_TESTING
/* Test builds have no engine dictionary: the stand-in answers sh_strids_native_key (NULL: no key is native). */
void sh_package_runtime_test_native_string_keys(int (*native)(const char *key));
#endif
/* Fixed-name rows the served provider keeps at a departed map's meaning while that map's native
 * consumers hold them (0 when none); the next native map boundary's unload may release them. */
int sh_package_runtime_bindings_deferred(void);
/* R-401 register-once (off unless SNAPMAP_PLUS_REGISTER_ONCE=1 or <data root>\register-once.flag): private rows
 * that leave the served view stay registered in the engine until the library changes. enabled reports the
 * switch; settled is 1 inside an activation callback that hands the engine no change, every row it serves being
 * one the engine already holds with these bytes; dormant_count is the number of rows kept registered so. */
int sh_package_runtime_register_once_enabled(void);
int sh_package_runtime_register_once_settled(void);
size_t sh_package_runtime_dormant_count(void);
#ifdef SH_PACKAGE_RUNTIME_TESTING
void sh_package_runtime_test_register_once(int enabled);
/* kept, readmitted, rebound, withdrawn, handed, and skipped (bit 0) / released-only (bit 1) of the last activation. */
void sh_package_runtime_test_register_once_stats(size_t out[6]);
#endif
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
/* R-398 perf17: acquire without waiting (an engine callback may run while this thread holds the provider
 * lock); *held 0 and NULL when it is unavailable. Release only when *held. */
const sh_package_compilation *sh_package_runtime_try_acquire(int *held);
/* The toybox roster the string refresh qualifies package labels against
 * (strids.h sh_strids_palette.entries): every tile as the current provider
 * serves it, resolved through the served editor entries first. 1 with the
 * entries (owned here until the next call), 0 while the shipped entries are
 * not readable (base labels), -1 on failure. */
struct sh_strids_palette_entry;
int sh_package_runtime_palette(void *context, const struct sh_strids_palette_entry **entries, size_t *count);
/* Valid only while the shared compilation is acquired. */
const sh_resource_catalog *sh_package_runtime_catalog(void);
/* Whether a material text's native parse would stop at a Program-kind parameter
 * naming a render program neither this compilation nor the installed catalog
 * declares, so the engine gives it the default (decl_material.h
 * sh_decl_material_native_default). 1 with that program copied out, 0 when every
 * program is declared, -1 when the text or the inventory is unavailable. */
int sh_package_runtime_material_native_default(const sh_package_compilation *compiled,
    const unsigned char *body, size_t length, char *program, size_t capacity);
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
/* serving: the root is kept only to serve consumers that began while it was
 * exposed (a rejected candidate's movies), without audio policy authority
 * (sh_audio_active_program). */
typedef struct sh_package_runtime_audio {
    void *owner;
    sh_package_audio_program *program;
    sh_audio_id_lease *lease;
    int serving;
} sh_package_runtime_audio;
int sh_package_runtime_audio_programs(sh_package_runtime_audio **rows, size_t *count,
    char *error, size_t capacity);
void sh_package_runtime_audio_release(void *owner);
/* Retain only this audio provider's exact sealed opaque file. Caller holds
 * its owner throughout this allocation-free lookup, then releases the returned
 * file after opening/reading it. No OS open or current-provider/global lookup. */
int sh_package_runtime_audio_file(void *owner, const char *engine_path,
    sh_package_file **file);
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
/* Same reads, with an optional exact native source witness captured under the
 * same provider lock as the returned bytes/stream. Target family/name come
 * from the active native source frame. Start *capture NULL. A successful read
 * with no capture is unknown provenance, never a stock-source certificate.
 * Read semantics are unchanged if witness allocation or identity fails. */
int sh_package_runtime_read_captured(const char *path, unsigned char **body, size_t *length,
    const char *family, const char *name, sh_package_resource_capture **capture);
int sh_package_runtime_open_file_captured(const char *path, FILE **stream, uint64_t *length,
    const char *family, const char *name, sh_package_resource_capture **capture);
int sh_package_runtime_read_decl_alias_captured(const char *path, unsigned char **body, size_t *length,
    const char *family, const char *name, sh_package_resource_capture **capture);
/* Current compiled declaration presence only; no body copy or native load. */
int sh_package_runtime_decl_alias_exists(const char *alias);
/* Current provider identity set, including aliases sharing physical bytes.
 * This does not assert that a native object has finished loading. */
int sh_package_runtime_serves_native_identity(const char *type, const char *name);
int sh_package_runtime_module_variants(char ***names, size_t *count);
/* A value that changes whenever a new served compilation is published
 * (installed library, map composition, removal or rollback). Lock-free. */
long sh_package_runtime_served_serial(void);
void sh_package_runtime_module_variants_free(char **names, size_t count);

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
/* Per-input accounting of the installed library, malloc-owned. Without a query:
 * every discovered unit with its disposition totals and the inputs needing
 * attention. A package id or folder lists each of its files and folders with
 * its disposition and exposures; any other query matches an authored or engine
 * path in every package. Listings page from row first (1-based), 250 rows at
 * a time. Reads retained compiler records only; scans and hashes nothing. */
char *sh_package_runtime_inputs(const char *query, size_t first);
/* Every exposure of one listed entry (its row number under the same query):
 * each compiled row naming the file and each generated row proved to read it,
 * one line per row and per native identity, 250 lines from first. */
char *sh_package_runtime_input_exposures(const char *query, size_t entry, size_t first);

#ifdef SH_PACKAGE_RUNTIME_TESTING
/* Synthetic verified event metadata for composed save/conversion controls.
 * NULL restores no proof; the production callback checks the pinned image and
 * live registry. The native registry decoder has its own byte-level controls. */
void sh_package_runtime_test_discarded_name_event(int (*event)(void *, const char *), void *context);
void sh_package_runtime_test_event_decl_constructs(int (*event)(void *, const char *, size_t, size_t, const char *, const char *), void *context);
/* One-based fault in exact-consumer hold/identity allocation; zero disables. */
void sh_package_runtime_test_consumer_allocation_fault(unsigned at);
/* Remaining fault countdown and reserved/live record counts. These expose no
 * native state and allocate nothing; fixture teardown must reach zero. */
unsigned sh_package_runtime_test_consumer_allocation_remaining(void);
size_t sh_package_runtime_test_movie_records(size_t *providers, size_t *live);
/* Root providers with an audio program still allocated, whoever holds them; a
 * root's compilation, sources and pins are released with it. */
size_t sh_package_runtime_test_audio_roots(void);
/* Every provider still allocated: roots, views and library clones. */
size_t sh_package_runtime_test_providers(void);
/* Body bytes held in memory by the compilations of every allocated provider. */
size_t sh_package_runtime_test_owned_bytes(void);
/* A reader of the published library as a lease holds it: one more reference to its provider, released by
 * hold_release (R-347 retained readers). */
void *sh_package_runtime_test_hold_library(void);
const sh_package_compilation *sh_package_runtime_test_held_compilation(const void *hold);
const sh_resource_catalog *sh_package_runtime_test_held_catalog(const void *hold);
void sh_package_runtime_test_hold_release(void *hold);
/* Providers in the published library's parent ancestry, the library included. */
size_t sh_package_runtime_test_library_depth(void);
#include "package_source_link.h"
/* Capture-only adapter for the synthetic native reader fixtures. */
void sh_package_runtime_test_constructor_requests(sh_package_source_implicit capture, void *context);
#include "package_virtual_material.h"
/* A synthetic original page provider, with the same complete atlas contract
 * the production native adapter supplies. NULL restores the normal adapter. */
typedef int (*sh_package_runtime_test_atlas_fn)(void *context, size_t scope,
    const sh_package_compilation *compiled, const char *material, const char *page_file,
    sh_package_virtual_atlas *out, char *error, size_t capacity);
void sh_package_runtime_test_atlas(sh_package_runtime_test_atlas_fn atlas, void *context);
/* Read installed loose originals (the engine's .vmtr tables beside base) through
 * these native assets, as production does; NULL clears. The runtime retains it. */
struct sh_package_native_assets;
void sh_package_runtime_test_native_assets(struct sh_package_native_assets *assets);
/* Synthetic native image exclusion evidence for library compiles (image_exclusion.h contract:
 * 1 excluded placeholder, 0 not, -1 unreadable). NULL certifies nothing. */
void sh_package_runtime_test_image_excluded(int (*excluded)(void *context, const char *name,
    const unsigned char *installed, size_t length));
/* Make occurrence leases pending (1) or published (0) for the retirement poll. */
void sh_package_runtime_test_occurrences_pending(int pending);
/* Run the activation callback even for a view the runtime finds unchanged (1),
 * to model an activation that fails; 0 restores the ordinary decision. */
void sh_package_runtime_test_force_activation(int forced);
/* R-309: settle a publication's in-place image recovery set against a previous publication
 * (has_previous; its set previous). The next one is a root with need and gate or, with rollback,
 * a movie rollback view restoring the previous set from a candidate that published need. changes
 * receives the availability transitions, published the next set (owned; free each and the array). */
/* R-309: replace each later library provider's in-place recovery need and declared image gate
 * (NULL names restores the compiled ones). */
void sh_package_runtime_test_recovery_need(const char *const *names, size_t count, int gate);
int sh_package_runtime_test_recovery(char *const *previous, size_t previous_count, int has_previous,
    char *const *need, size_t need_count, int gate, int rollback,
    sh_package_changes *changes, char ***published, size_t *published_count);
/* The historical bindings a map without recorded provenance receives. */
int sh_package_runtime_test_legacy_bindings(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_provenance *out);
int sh_package_runtime_test_historical_derived(sh_package_provenance *historical,
    const sh_package_provenance *linked, const sh_package_references *roots);
int sh_package_runtime_test_consumer_changes(const sh_package_consumers *before,
    const sh_package_consumers *after, sh_package_changes *changes);
/* The installed library's native pruning over a caller-built compilation and
 * its captured consumers, without a catalog (single-name files only). */
size_t sh_package_runtime_test_prune_native(sh_package_compilation *compiled, sh_package_consumers **consumers,
    size_t movies[2], char *error, size_t capacity);
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
/* R-400: answer the newest warm record's catalog reads through reader/context now, as the boot worker does at
 * attach; the next start's revalidation uses them. Counts: answers held, and how many the start used. */
void sh_package_runtime_test_boot_prefetch(const char *data_root, sh_package_baseline_reader reader, void *context);
size_t sh_package_runtime_test_boot_prefetch_counts(size_t *used);
void sh_package_runtime_test_boot_prefetch_release(void);
/* Installed resource paths the input report treats as game resources while the
 * fixture runs without a catalog, so its legacy-layout test is reachable;
 * NULL restores none. Production reads the installed game's catalog. */
void sh_package_runtime_test_catalog_paths(const char *const *paths, size_t count);
/* The AI state-machine web hook activation issues to and publishes through
 * (fsm_web.h): a fixture passes the real sh_fsm_web_* entry points (or its own);
 * all NULL restores a compiler fixture without the hook. */
struct sh_fsm_web_table;
void sh_package_runtime_test_fsm_hook(int (*installed)(void),
    int (*issue)(const char *const *names, size_t count), int (*publish)(const struct sh_fsm_web_table *table));
/* Opt-in real context linking/capture against immutable synthetic native readers.
 * This small-schema fixture omits baked product defaults during preparation.
 * Passing a zero source disables the seam and preserves older offline fixtures. */
void sh_package_runtime_test_source_readers(sh_decl_registry_source registry, uintptr_t reflection);
/* Nonzero: compiles open the native schema from those source readers, as
 * production opens the process's own, so entity definitions take the typed
 * composition that decides their served text and native names (R-274). */
void sh_package_runtime_test_typed_compile(int enabled);
/* Shipped SnapMap editor declarations (names under snapeditorentitydef/) whose
 * texts the test baseline supplies; NULL makes placement unavailable, while a
 * library compile then reads a fixture that ships no editor entries. */
void sh_package_runtime_test_editor_rows(const char *const *names, size_t count);
/* R-255: disable reader-bound parent variants (the discriminating negative). */
void sh_package_runtime_test_variants(int enabled);
/* M116's correction disabled: every row placing a variant's parent presents it,
 * whatever binding context supplies the row (the palette refusal's negative). */
void sh_package_runtime_test_variant_rows_any(int any);
/* Make the shipped editor entries unavailable to library compiles, as a live
 * compile without the game's catalog would find them (0 restores them). */
void sh_package_runtime_test_editor_rows_unavailable(int unavailable);
/* Faults for the settled derived document's re-read (0 none): 1 re-reads the
 * original document as derived, so its uses convert again; 2 lists no use of
 * the derived document. Either withdraws the conversion. */
void sh_package_runtime_test_derived_fault(int mode);
/* Native string reservations for linked library compiles (NULL: none), as the
 * live compile takes from the dictionary; toybox label aliases need them. */
struct sh_package_string_reservations;
void sh_package_runtime_test_strings(const struct sh_package_string_reservations *strings);
/* The native resource registry linked library compiles read provider paths'
 * identities from (a zero head_slot: none), as the live compile reads the
 * engine's declaration resource list. */
void sh_package_runtime_test_native_identities(sh_native_resource_registry_source identities);
/* The game's script metadata linked compiles read (NULL: none), as the live
 * compile captures it from the loaded script module. The caller keeps it. */
struct sh_decl_script_schema;
void sh_package_runtime_test_scripts(struct sh_decl_script_schema *scripts);
/* Keep every product builtin while source readers are installed (nonzero), as
 * production does, for a caller whose registry models every builtin's class.
 * By default source-reader fixtures compile without product builtins. */
void sh_package_runtime_test_builtins(int keep);
/* The toybox roster (sh_package_runtime_palette) of a caller's compilation. */
int sh_package_runtime_test_palette_of(const sh_package_compilation *compiled,
    const struct sh_strids_palette_entry **entries, size_t *count);
/* The converter's placement check over a caller-built compilation: 1 placed,
 * 0 refused (reason; *engine for a native limit), -1 editor rows unreadable. */
int sh_package_runtime_test_placement(const sh_package_compilation *compiled, const char *from, const char *base,
    int *engine, char *reason, size_t capacity);
/* Link the installed library through those readers, as production does, so
 * owner isolation and unserved coverage run end to end. Off by default. */
void sh_package_runtime_test_installed_links(int enabled);
/* Compose block declaration families without bound native readers. Off by default. */
void sh_package_runtime_test_block_compose(int enabled);
/* An installation's whole link omits the whole compile's rows declared at path
 * that package owns (both borrowed until reset); none when path is NULL. */
void sh_package_runtime_test_whole_omit(const char *path, const char *package);
/* Link installed libraries under this sh_package_shader_renderer contract
 * (no shader adapter callbacks); none when renderer < 0. */
void sh_package_runtime_test_shader_renderer(int renderer);
/* Link installed libraries with this complete shader adapter (a configured
 * native-assets handle's, over replayed native state); NULL restores the above. */
void sh_package_runtime_test_shader_adapter(const sh_package_source_shader *shader);
/* Library compiles read the installation under base instead of <exe dir>\base (fixture catalogs); "" restores it.
 * counts reports how many compiles opened a catalog and how many reused a live provider's. */
void sh_package_runtime_test_catalog_base(const char *base);
void sh_package_runtime_test_catalog_counts(long *opens, long *reuses);
/* How many published archive-backed images were released to their backing after publication (R-347). */
long sh_package_runtime_test_detached_published(void);
/* 1: the release point finds no record origins, so the record write reads the released images again. */
void sh_package_runtime_test_origins_late(int late);
/* Library compiles ask these shader parameter questions (name and renderparm source) through their adapter, twice
 * each, as the source link asks a linked renderprog's references (fixtures without native semantics cannot link one;
 * the answers do not change the compile); both borrowed until reset (NULL). */
void sh_package_runtime_test_shader_questions(const char *const *names, const char *const *sources, size_t count);
/* Fail the next count shader accounting captures as an exhausted heap would. */
void sh_package_runtime_test_shader_input_failures(long count);
/* Fail the next count composed-input captures (the records are lost, the library still serves). */
void sh_package_runtime_test_composed_input_failures(long count);
/* 0 lets a watched refresh serve any increment, bypassing pr_increment_exact. */
void sh_package_runtime_test_exact_increment(int enabled);
/* R-393 link decisions: every indexed or memoized answer a retained link takes
 * (lease and selector resource selection, a movie rollback's reserved names,
 * a binding still served) is also taken the way the unindexed scan takes it.
 * checks[k] and mismatches[k] count them by kind (0 lease rows, 1 selector
 * rows, 2 movie rows, 3 served bindings, 4 still-served view bindings). */
void sh_package_runtime_test_link_decisions(long checks[5], long mismatches[5]);
/* 0 takes the indexed answers alone (timing probes); 1, the default, checks them. */
void sh_package_runtime_test_link_verify(int enabled);
/* 1: links take the scans' answers (the unindexed rebuild) instead of the indexed ones. */
void sh_package_runtime_test_link_scan_answers(int enabled);
/* 0: warm records are written without their identity provenance section, as an older build wrote them (R-397). */
void sh_package_runtime_test_warm_provenance(int write);
/* 1: an addition may take the closure increment (R-397; on in the product, off by default in test builds). */
void sh_package_runtime_test_closure_additions(int enabled);
/* 1: a closure increment of any size runs (offline oracle of large closures). */
void sh_package_runtime_test_closure_any_size(int enabled);
/* One served row's identity provenance, comma-separated package names (R-397). */
int sh_package_runtime_test_row_provenance(const char *path, char *out, size_t capacity);
/* The served library's rows with identity provenance and a digest over them (R-397). */
size_t sh_package_runtime_test_provenance(unsigned long long *digest);
/* 0: a watched removal compiles the whole library instead of subtracting (the oracle). */
void sh_package_runtime_test_subtract(int enabled);
/* The packages a removal of the named ones would recompile (diagnostics); the count. */
size_t sh_package_runtime_test_subtract_footprint(const char *names, char *out, size_t capacity);
/* An original every later installation increment also binds, captured as its
 * pruned-original witnesses are (NULL: none). */
void sh_package_runtime_test_install_witness(const sh_compiled_resource *original);
/* The resource producer every compile uses instead of the grid assets' (NULL
 * restores them), so a fixture can generate rows from inputs it chooses. */
void sh_package_runtime_test_producer(sh_package_producer producer);
/* The served library's published digest (pr_library_digest); 0 without one. */
int sh_package_runtime_test_library_digest(char digest[65]);
void sh_package_runtime_test_spawn_keys(int enabled);
int sh_package_runtime_test_witness_compose(sh_package_compilation *served, const sh_compiled_resource *original,
    char *error, size_t capacity);
#include "package_native_inline.h"
/* Native state comparison of two definition texts, supplied by tests in
 * place of the engine evaluator (plumbing only). layers are the target
 * chain's privately prepared definitions, as sh_native_decl_eval_compare. */
typedef sh_native_eval_status (*sh_package_runtime_native_compare)(const sh_native_eval_layer *layers,
    size_t layer_count, const char *target, const char *candidate, const sh_native_eval_reduction *reduction,
    char *detail, size_t capacity);
void sh_package_runtime_test_native_compare(sh_package_runtime_native_compare compare);
/* Times g_refresh_lock was acquired again on its holding thread (a test build aborts at the first). */
long sh_package_runtime_test_refresh_reentries(void);
int sh_package_runtime_test_source_inputs(sh_decl_registry_source *registry, uintptr_t *reflection,
    sh_package_baseline_reader *baseline, void **baseline_context);
/* The served editor rows of compiled that leave the pickers as equal
 * exposures another row stands for (owned sorted names; *sets counts the
 * equal sets), reading unserved originals with original. 1 done, 0 failed. */
int sh_package_runtime_test_exposure(const sh_package_compilation *compiled, const sh_package_provenance *bindings,
    const sh_package_sources *sources, sh_package_baseline_reader original, void *context,
    char ***names, size_t *count, size_t *sets);
/* Install that selection as the published one (NULL compiled clears it) and
 * publish compiled's lookup rows to the registered sink. 1 done, 0 failed. */
int sh_package_runtime_test_publish_exposure(const sh_package_compilation *compiled, const sh_package_provenance *bindings,
    const sh_package_sources *sources, sh_package_baseline_reader original, void *context);
/* The report's reason the published selection leaves name out of the pickers
 * (static text), or NULL when name stays listed. */
const char *sh_package_runtime_test_exposure_reason(const char *name);
/* 1 when the spawn-key group tables last published are the serving
 * providers' (the installed library's and the active map's). */
int sh_package_runtime_test_key_groups_current(void);
/* Whether the refresh in progress is the one the installed-package watcher
 * asked for (sh_package_runtime_note_watched_change). */
int sh_package_runtime_test_watched_change(void);
/* Give the serving providers these group tables (copies) and publish them. */
void sh_package_runtime_test_key_tables(const sh_package_key_groups *library, const sh_package_key_groups *map);
void sh_package_runtime_test_plan_key_table(sh_package_map_plan *plan, const sh_package_key_groups *table);
/* Unpublished map/installed candidate catalogs for namespace lifecycle checks. */
const sh_package_provenance *sh_package_runtime_test_plan_bindings(const sh_package_map_plan *plan, int installed);
int sh_package_runtime_test_unmodified_type(uint32_t type);
int sh_package_runtime_test_exact_type(uint32_t type);
void sh_package_runtime_test_serving_key_table(sh_package_key_groups *table);
/* Offline fixture teardown only. No engine callbacks or native game reset. */
void sh_package_runtime_test_dispose(void);
/* The product and game image identity keying remembered decisions, and the
 * renderer executable whose memo holds them ("vk" by default). */
void sh_package_runtime_test_build_identity(const char *identity);
void sh_package_runtime_test_image_slot(const char *slot);
/* Warm publication (R-344 stage B). Testing builds use it only when enabled: 1 on, 0 off, -1 the SMP_WARM
 * environment variable ("1") decides. product is added to the product identity (a changed product);
 * directory, when not empty, holds the record and its pointer instead of the data root's package cache. */
void sh_package_runtime_test_warm(int enabled);
void sh_package_runtime_test_warm_product(const char *product);
void sh_package_runtime_test_warm_directory(const char *directory);
/* Library compiles started (each compile call of a refresh, including retries). */
long sh_package_runtime_test_compiles(void);
/* Map plans served from the departed map's retained compile instead of a new one, and whether one is kept now. */
long sh_package_runtime_test_map_reuses(void);
int sh_package_runtime_test_departed_map(void);
/* Warm hits, misses and written records so far, and the last warm diagnostic line. */
void sh_package_runtime_test_warm_counts(long *hits, long *misses, long *writes, char *last, size_t capacity);
/* Unchanged-scope reuse (R-348). Testing builds use it only when enabled: 1 on, 0 off, -1 the SMP_REUSE environment
 * variable ("1") decides; records offered (loaded) and written, compiles failed over to one without the record,
 * bindings bound, and the last diagnostic line. */
void sh_package_runtime_test_reuse(int enabled);
void sh_package_runtime_test_reuse_counts(long *loads, long *saves, long *fallbacks, long *bound, char *last, size_t capacity);
/* The capture templates linked compiles took from offered records (R-348), in total. */
long sh_package_runtime_test_reuse_templates(void);
/* Fail the nth allocation point of a warm load (0: none), as exhausted memory would. */
void sh_package_runtime_test_warm_failure(long nth);
/* R-398 map context records: on (1), off (0) or SMP_MAP_RECORDS=1 (-1, the default); counts of loads used, loads
 * not used and records written, and the last record line. */
void sh_package_runtime_test_map_records(int enabled);
void sh_package_runtime_test_map_record_counts(long *hits, long *misses, long *writes, char *last, size_t capacity);
/* 1: test baseline answers carry a backing that reads them again (as the catalog's pinned rows do), so library
 * originals are released and published originals are backed; 2: the same answers without one; 0: off. */
void sh_package_runtime_test_baseline_backing(int mode);
/* The last warm miss line (a cold start's own write follows it). */
void sh_package_runtime_test_warm_miss(char *out, size_t capacity);
/* Nonzero: the warm record's file refuses its first streamed bytes (an interrupted write). */
void sh_package_runtime_test_warm_sink_failure(int on);
/* The comparison text (sh_package_runtime_test_publication_text): 1 fails the audio program digest, 2 the left-out
 * identity, 3 a link history node read; each must leave no text. 0: off. */
void sh_package_runtime_test_text_failure(int which);
/* Checks the comparison text over the current library: equal copies are equal; recovery names with spaces, long
 * tails of recovery names and exclusion lines, and absent against empty fields stay distinct. 1 when all hold. */
int sh_package_runtime_test_text_framing(char *detail, size_t capacity);
/* Keep a native link history in testing compiles too (production always keeps one); needs identities. */
void sh_package_runtime_test_link_history(int enabled);
/* Canonical text of the published library provider, its inventory and the link history: every field with
 * exact bytes, for comparing a cold start with a warm one. malloc-owned; NULL without a library. */
char *sh_package_runtime_test_publication_text(size_t *length);
/* R-398: the served map provider's publication (resources, bindings, historical bindings, key groups, consumers,
 * webs) as one hex digest, for comparing a compiled map context with a loaded one. malloc-owned; NULL without a map. */
char *sh_package_runtime_test_publication_digest(void);
#endif

#endif
