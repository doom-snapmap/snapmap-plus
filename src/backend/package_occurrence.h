/* Native adapters supply object lifetimes; this module owns only provenance. */
#ifndef SH_PACKAGE_OCCURRENCE_H
#define SH_PACKAGE_OCCURRENCE_H

#include <stdint.h>
#include "package_provenance.h"
#include "package_bundle.h"

typedef struct sh_package_occurrence_snapshot sh_package_occurrence_snapshot;
typedef struct sh_package_occurrence_tracker sh_package_occurrence_tracker;
/* Runtime-owned immutable resource lease. Release can run in a native object
 * destructor under the tracker lock: callbacks must only change reference
 * counts/queue work, never acquire provider locks or enter the engine. */
typedef struct sh_package_occurrence_resource {
    void *handle;
    void (*retain)(void *handle);
    void (*release)(void *handle);
} sh_package_occurrence_resource;
typedef struct sh_package_occurrence_key {
    uintptr_t namespace_id, object; /* caller-owned namespace and opaque pointer */
} sh_package_occurrence_key;

enum { SH_PACKAGE_OCCURRENCE_ENTITY = 1, SH_PACKAGE_OCCURRENCE_WORLD = 2 };
enum { SH_PACKAGE_OCCURRENCE_ORIGINAL_TREES = 1, SH_PACKAGE_OCCURRENCE_COMPILER_TREES = 2 };
enum {
    SH_PACKAGE_OCCURRENCE_FAILED = 0,
    SH_PACKAGE_OCCURRENCE_READY = 1,
    SH_PACKAGE_OCCURRENCE_NEEDS_SELECTION = 2
};

/* Explicitly selected uses, never a provider's supplier catalog. An entity
 * slice may contain one object's typed uses and caller-selected historical
 * aggregates; a world slice may contain world uses and historical aggregates.
 * Historical bindings retain every contributor of each selected context.
 * Rows are the caller's actual selected source bindings. Original-tree identity
 * is the modern contract; COMPILER_TREES permits compiler-tree matching only
 * for selected historical aggregates in the slice. Ordinary typed uses still
 * require original-tree identities in that mode. Every matching selected
 * row is retained, never a first-match original substitute. Unrelated trees are
 * omitted. Missing mappings are caller integration failures, not bad packages.
 * Inputs remain unchanged. Returns one owned immutable snapshot or NULL. */
sh_package_occurrence_snapshot *sh_package_occurrence_prepare_selected(
    const sh_package_provenance *selected, unsigned kind, unsigned tree_identity,
    const sh_package_bundle_binding *rows, size_t count, char *error, size_t capacity);

/* Select exact modern entity/N or world uses. Historical aggregates and catalog
 * suppliers return NEEDS_SELECTION without changing *out: the caller must use
 * its verified typed object walk and prepare_selected. This status does not
 * prescribe refusing an old map load. NULL document is explicit empty ownership.
 * Success replaces *out, which must initially be NULL or an owned snapshot. */
int sh_package_occurrence_prepare_entity(const sh_package_provenance *document,
    size_t entity, const sh_package_bundle_binding *rows, size_t count,
    sh_package_occurrence_snapshot **out, char *error, size_t capacity);
int sh_package_occurrence_prepare_world(const sh_package_provenance *document,
    const sh_package_bundle_binding *rows, size_t count,
    sh_package_occurrence_snapshot **out, char *error, size_t capacity);

sh_package_occurrence_snapshot *sh_package_occurrence_retain(sh_package_occurrence_snapshot *snapshot);
void sh_package_occurrence_release(sh_package_occurrence_snapshot *snapshot);
/* Borrowed data remains valid while the immutable snapshot is retained. */
const sh_package_provenance *sh_package_occurrence_provenance(const sh_package_occurrence_snapshot *snapshot);
const sh_package_bundle_binding *sh_package_occurrence_bundles(const sh_package_occurrence_snapshot *snapshot,
    size_t *count);
/* Retain the result of a completed typed document walk. This accepts explicit
 * USES only; historical records must first pass through that walk. The owned
 * document can be attached to a native array or carried across serialization. */
sh_package_occurrence_snapshot *sh_package_occurrence_prepare_document(
    const sh_package_provenance *uses, const sh_package_bundle_binding *rows,
    size_t count, char *error, size_t capacity);
/* Attach before publishing a newly prepared, uniquely owned snapshot. The
 * binding/context pair must belong to an actual selected use. Exact pairs are
 * copied through slices/export; dropping that use drops its lease. Same native
 * names with different source contexts never exchange ownership. This acquires
 * one reference; it does not consume the caller's handle. */
int sh_package_occurrence_bind_resource(sh_package_occurrence_snapshot *snapshot,
    size_t binding, size_t context, sh_package_occurrence_resource resource);

sh_package_occurrence_tracker *sh_package_occurrence_tracker_create(void);
/* Caller stops native callbacks before destroying the tracker. */
void sh_package_occurrence_tracker_free(sh_package_occurrence_tracker *tracker);
/* Keys never dereference engine memory or use uniqueId. Set/copy/move replace
 * destination ownership atomically; unknown sources clear the destination.
 * Erase before pointer reconstruction and at destruction. Different native
 * representations or independently retired containers need distinct namespaces.
 * Clipboard/undo/prefab namespaces survive a live-map namespace's retirement. */
int sh_package_occurrence_set(sh_package_occurrence_tracker *tracker,
    sh_package_occurrence_key key, sh_package_occurrence_snapshot *snapshot);
sh_package_occurrence_snapshot *sh_package_occurrence_get(sh_package_occurrence_tracker *tracker,
    sh_package_occurrence_key key);
int sh_package_occurrence_copy(sh_package_occurrence_tracker *tracker,
    sh_package_occurrence_key destination, sh_package_occurrence_key source);
int sh_package_occurrence_move(sh_package_occurrence_tracker *tracker,
    sh_package_occurrence_key destination, sh_package_occurrence_key source);
void sh_package_occurrence_erase(sh_package_occurrence_tracker *tracker, sh_package_occurrence_key key);
void sh_package_occurrence_erase_namespace(sh_package_occurrence_tracker *tracker, uintptr_t namespace_id);

/* Attach one complete serialized document to the corresponding flat-record
 * pointers by array index. Prepare every slice before changing any entry/world
 * handle. Duplicate/zero pointers, unrecognized owner tokens and incomplete
 * mappings fail transactionally. Unrelated tracker entries survive. The caller
 * serializes document attachment with native container replacement, and owns
 * *world (initially NULL or an owned world snapshot) separately from objects. */
int sh_package_occurrence_attach_document(sh_package_occurrence_tracker *tracker,
    uintptr_t namespace_id, const uintptr_t *objects, size_t count,
    const sh_package_provenance *document, const sh_package_bundle_binding *rows, size_t row_count,
    sh_package_occurrence_snapshot **world, char *error, size_t capacity);
/* Snapshot-aware form preserves native resource leases as well as archives.
 * Prefer this at native document attachment boundaries. */
int sh_package_occurrence_attach_snapshot(sh_package_occurrence_tracker *tracker,
    uintptr_t namespace_id, const uintptr_t *objects, size_t count,
    const sh_package_occurrence_snapshot *document,
    sh_package_occurrence_snapshot **world, char *error, size_t capacity);

/* The explicit native serialization order defines fresh entity/N keys. Unknown
 * entries contribute no ownership; proving hook/typed-site coverage remains the
 * native adapter's job. Only the supplied world snapshot is included. Returns
 * one owned document snapshot with pruned provenance and retained bundle rows,
 * or NULL without changing the tracker/world. A document snapshot cannot be
 * assigned to an object. No native IDs, paths or process pointers are exported. */
sh_package_occurrence_snapshot *sh_package_occurrence_export(sh_package_occurrence_tracker *tracker,
    const sh_package_occurrence_key *order, size_t count, const sh_package_occurrence_snapshot *world,
    char *error, size_t capacity);

/* Exact editor rows of a document (see sh_package_provenance_use.row). Owner
 * tokens are canonical "entity/N" with N < count; owner_index returns 1 with N.
 * Mutators accept only a uniquely owned document snapshot (prepare/export
 * result, before it is shared) and change nothing on failure.
 * document_rows: rows[N] is entity/N's recorded choice (NULL none). It replaces
 * the document's row table and the row of every OBJECT use. The table also
 * keeps the choice of an object without ownership yet, so a later capture that
 * gives it ownership can carry it; a row itself never creates ownership.
 * document_row borrows table entry N (NULL none or no table).
 * carry_rows / carry_document_rows: a rebuilt document regains each owner's
 * row by owner token from saved provenance, or from a document's table (else
 * its use rows). Owners absent from document drop theirs; a different row
 * already recorded fails (never replaced). Returns 1 ok, 0 otherwise. */
int sh_package_occurrence_owner_index(const char *owner, size_t count, size_t *index);
int sh_package_occurrence_document_rows(sh_package_occurrence_snapshot *document,
    const char *const *rows, size_t count);
const char *sh_package_occurrence_document_row(const sh_package_occurrence_snapshot *document, size_t index);
int sh_package_occurrence_carry_rows(sh_package_occurrence_snapshot *document, const sh_package_provenance *from);
int sh_package_occurrence_carry_document_rows(sh_package_occurrence_snapshot *document,
    const sh_package_occurrence_snapshot *from);

#endif
