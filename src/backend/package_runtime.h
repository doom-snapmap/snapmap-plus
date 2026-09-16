/* One atomically published package compilation shared by runtime consumers. */
#ifndef SH_PACKAGE_RUNTIME_H
#define SH_PACKAGE_RUNTIME_H
#include "package_compiler.h"
#include "resource_catalog.h"
#include "package_usage.h"
#include "package_source_graph.h"
#include "audio_originals.h"

int sh_package_runtime_refresh(const char *data_root);
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
/* Compile an immutable map candidate without publishing resources, changing
 * readiness, or calling native consumers. Inspect it for resource admission,
 * then activate the same bytes after consent. The caller serializes operations
 * on a handle and retains its private source tree for later saving. Preparation
 * failures affect only the returned error, never the active runtime status. */
typedef struct sh_package_map_plan sh_package_map_plan;
sh_package_map_plan *sh_package_runtime_prepare_map(const char *data_root,
    const char *map_source_root, char *error, size_t capacity);
/* Borrowed immutable compilation, valid until plan_free. No runtime acquire or
 * release is required; this candidate is not installed-library availability. */
const sh_package_compilation *sh_package_map_plan_compilation(const sh_package_map_plan *plan);
/* Snapshot the complete installed source inventory after a consented disk
 * publication. This does not compile all installed variants together or change
 * active values. The snapshot stays private through native activation and
 * loading. Failure leaves the previous inventory and plan intact. */
int sh_package_map_plan_prepare_inventory(sh_package_map_plan *plan,
    const char *data_root, char *error, size_t capacity);
/* After successful native loading, finish its disk transaction and publish
 * the already-prepared inventory together. A refused or superseded map leaves
 * the old inventory intact. commit performs no engine calls or runtime refresh;
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
/* Inspect explicit dependencies in this candidate over the retained library.
 * Uses only source bytes and read-only native metadata; never loads resources
 * or changes the observed native graph. Unsupported source readers remain gaps. */
int sh_package_map_plan_source_resources(const sh_package_map_plan *plan,
    sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_references *out,
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
void sh_package_runtime_error(char *out, size_t capacity);

/* Acquire the current provider (prospective during activation, previous after
 * failure). Pair acquire/release even when no snapshot exists. No engine
 * callbacks while holding a snapshot; copy data before entering native code. */
const sh_package_compilation *sh_package_runtime_acquire(void);
void sh_package_runtime_release(void);
/* Valid only while the shared compilation is acquired. */
const sh_resource_catalog *sh_package_runtime_catalog(void);
/* Installed audio identity used to mirror native localized-before-root bank
 * lookup. Acquires its own provider lock; call outside an acquired snapshot. */
int sh_package_runtime_audio_original(const char *path, sh_package_original_identity *out,
    char *error, size_t capacity);
/* The effective installed original bytes and the packaged bank identities
 * behind that same order, for building a native activation catalog. Both take
 * the provider lock; neither enters native code or copies a media payload. */
int sh_package_runtime_audio_originals_ready(void);
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
char *sh_package_runtime_policy(const char *section, size_t *length);
/* Select from typed map resources, after delivery data has been removed.
 * NULL/zero clears the map. Failure preserves the last successful selection.
 * References are copied from the native preparation pass. Refresh resolves
 * those identities and the retained map again, never reuses old owner indices.
 * No engine callbacks occur; callers publish at a map load/save boundary. */
int sh_package_runtime_select_map(const char *json, size_t length,
    const sh_package_references *references, char *error, size_t capacity);
/* Malloc-owned policy for the selected map, or an empty object without a map.
 * Editor strings and resource-registration gates use the installed policy
 * above because they also prepare assets outside the active gameplay map. */
char *sh_package_runtime_active_policy(const char *section, size_t *length);
/* Human-readable inventory, malloc-owned. Show after releasing the snapshot. */
char *sh_package_runtime_summary(void);

#ifdef SH_PACKAGE_RUNTIME_TESTING
void sh_package_runtime_test_audio_originals(const char *root, const wchar_t *language);
void sh_package_runtime_test_empty_catalog(void);
void sh_package_runtime_test_baseline(sh_package_baseline_reader reader, void *context);
/* Offline fixture teardown only. No engine callbacks or native game reset. */
void sh_package_runtime_test_dispose(void);
#endif

#endif
