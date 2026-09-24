#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_occurrence.h"

typedef struct po_resource {
    char *family, *original, *name;
    sh_package_provenance_context context;
    sh_package_occurrence_resource lease;
} po_resource;
struct sh_package_occurrence_snapshot {
    volatile LONG references;
    unsigned kind;
    sh_package_provenance value;
    sh_package_bundle_binding *bundles;
    size_t bundle_count;
    po_resource *resources;
    size_t resource_count;
};
typedef struct po_entry {
    sh_package_occurrence_key key;
    sh_package_occurrence_snapshot *snapshot;
} po_entry;
struct sh_package_occurrence_tracker {
    SRWLOCK lock;
    po_entry *entries;
    size_t count, capacity;
};

static int po_error(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return 0; }

static void po_resource_free(po_resource *resource)
{
    if (resource->lease.handle) resource->lease.release(resource->lease.handle);
    free(resource->family); free(resource->original); free(resource->name);
    free(resource->context.trees); free(resource->context.component);
}
static int po_context_equal(const sh_package_provenance_context *a,
    const sh_package_provenance_context *b)
{
    return a->mode == b->mode && a->tree_count == b->tree_count &&
        !memcmp(a->trees, b->trees, a->tree_count * 32u) &&
        !strcmp(a->component ? a->component : "", b->component ? b->component : "");
}
static int po_resource_matches(const po_resource *row,
    const sh_package_provenance_binding *binding, const sh_package_provenance_context *context)
{
    return !strcmp(row->family, binding->family) && !strcmp(row->original, binding->original) &&
        !strcmp(row->name, binding->name) && po_context_equal(&row->context, context);
}
int sh_package_occurrence_bind_resource(sh_package_occurrence_snapshot *snapshot,
    size_t binding, size_t context, sh_package_occurrence_resource resource)
{
    po_resource row = {0}, *grown;
    int selected = 0;
    if (!snapshot || snapshot->references != 1 || !resource.handle || !resource.retain || !resource.release ||
        binding >= snapshot->value.binding_count || context >= snapshot->value.context_count) return 0;
    const sh_package_provenance_binding *b = snapshot->value.bindings + binding;
    const sh_package_provenance_context *c = snapshot->value.contexts + context;
    for (size_t u = 0; u < snapshot->value.use_count; u++) if (snapshot->value.uses[u].binding == binding)
        for (size_t i = 0; i < snapshot->value.uses[u].context_count; i++)
            if (snapshot->value.uses[u].contexts[i] == context) selected = 1;
    if (!selected) return 0;
    for (size_t i = 0; i < snapshot->resource_count; i++) {
        po_resource *old = snapshot->resources + i;
        if (po_resource_matches(old, b, c) && old->lease.handle == resource.handle &&
            old->lease.retain == resource.retain && old->lease.release == resource.release) return 1;
    }
    row.family = _strdup(b->family); row.original = _strdup(b->original); row.name = _strdup(b->name);
    row.context.mode = c->mode; row.context.tree_count = c->tree_count;
    row.context.component = c->component ? _strdup(c->component) : NULL;
    row.context.trees = malloc(c->tree_count * 32u);
    if (!row.family || !row.original || !row.name || !row.context.trees ||
        (c->component && !row.context.component) || snapshot->resource_count >= SIZE_MAX / sizeof(*grown) ||
        !(grown = realloc(snapshot->resources, (snapshot->resource_count + 1u) * sizeof(*grown)))) {
        po_resource_free(&row); return 0;
    }
    memcpy(row.context.trees, c->trees, c->tree_count * 32u);
    resource.retain(resource.handle); row.lease = resource;
    snapshot->resources = grown; snapshot->resources[snapshot->resource_count++] = row; return 1;
}
static int po_copy_resources(sh_package_occurrence_snapshot *out, const sh_package_occurrence_snapshot *source)
{
    if (!source) return 1;
    for (size_t i = 0; i < source->resource_count; i++) {
        const po_resource *row = source->resources + i;
        for (size_t u = 0; u < out->value.use_count; u++) {
            const sh_package_provenance_use *use = out->value.uses + u;
            for (size_t c = 0; c < use->context_count; c++)
                if (po_resource_matches(row, out->value.bindings + use->binding,
                    out->value.contexts + use->contexts[c]) &&
                    !sh_package_occurrence_bind_resource(out, use->binding, use->contexts[c], row->lease)) return 0;
        }
    }
    return 1;
}

sh_package_occurrence_snapshot *sh_package_occurrence_retain(sh_package_occurrence_snapshot *snapshot)
{ if (snapshot) InterlockedIncrement(&snapshot->references); return snapshot; }
void sh_package_occurrence_release(sh_package_occurrence_snapshot *snapshot)
{
    if (!snapshot || InterlockedDecrement(&snapshot->references)) return;
    sh_package_provenance_free(&snapshot->value);
    for (size_t i = 0; i < snapshot->bundle_count; i++) sh_package_bundle_release(snapshot->bundles[i].original);
    for (size_t i = 0; i < snapshot->resource_count; i++) po_resource_free(snapshot->resources + i);
    free(snapshot->resources); free(snapshot->bundles); free(snapshot);
}
const sh_package_provenance *sh_package_occurrence_provenance(const sh_package_occurrence_snapshot *snapshot)
{ return snapshot ? &snapshot->value : NULL; }
const sh_package_bundle_binding *sh_package_occurrence_bundles(const sh_package_occurrence_snapshot *snapshot,
    size_t *count)
{ if (count) *count = snapshot ? snapshot->bundle_count : 0; return snapshot ? snapshot->bundles : NULL; }

static sh_package_occurrence_snapshot *po_create(unsigned kind)
{
    sh_package_occurrence_snapshot *out = calloc(1, sizeof(*out));
    if (out) { out->references = 1; out->kind = kind; out->value.semantics = SH_PACKAGE_PROVENANCE_USES; }
    return out;
}
static int po_add_bundle(sh_package_occurrence_snapshot *out, const sh_package_bundle_binding *row)
{
    char key[SH_PACKAGE_BUNDLE_KEY_CAP];
    sh_package_bundle_binding *grown;
    if (!row->original || !memchr(row->source_name, 0, sizeof(row->source_name))) return 0;
    sh_package_bundle_delivery_key(row->original, key);
    for (size_t i = 0; i < out->bundle_count; i++) {
        char previous[SH_PACKAGE_BUNDLE_KEY_CAP];
        if (memcmp(row->compiler_tree, out->bundles[i].compiler_tree, 32)) continue;
        sh_package_bundle_delivery_key(out->bundles[i].original, previous);
        if (!strcmp(key, previous)) return 1;
    }
    if (out->bundle_count >= SIZE_MAX / sizeof(*grown) - 1u ||
        !(grown = realloc(out->bundles, (out->bundle_count + 1u) * sizeof(*grown)))) return 0;
    out->bundles = grown; out->bundles[out->bundle_count] = *row;
    out->bundles[out->bundle_count++].original = sh_package_bundle_retain(row->original); return 1;
}
static int po_capture_sources(sh_package_occurrence_snapshot *out, unsigned identity,
    const sh_package_bundle_binding *rows, size_t count)
{
    if ((count && !rows) || (identity != SH_PACKAGE_OCCURRENCE_ORIGINAL_TREES &&
        identity != SH_PACKAGE_OCCURRENCE_COMPILER_TREES)) return 0;
    for (size_t u = 0; u < out->value.use_count; u++) {
        const sh_package_provenance_use *use = out->value.uses + u;
        int historical = identity == SH_PACKAGE_OCCURRENCE_COMPILER_TREES && use->kind == SH_PACKAGE_USE_HISTORICAL;
        for (size_t c = 0; c < use->context_count; c++) {
            const sh_package_provenance_context *context = out->value.contexts + use->contexts[c];
            for (size_t t = 0; t < context->tree_count; t++) {
                int found = 0;
                for (size_t i = 0; i < count; i++) {
                    const unsigned char *tree = historical ? rows[i].compiler_tree :
                        sh_package_bundle_fingerprint(rows[i].original);
                    if (!tree || memcmp(tree, context->trees[t], 32)) continue;
                    if (!po_add_bundle(out, rows + i)) return 0;
                    found = 1;
                }
                if (!found) return 0;
            }
        }
    }
    return 1;
}
typedef struct po_selected { unsigned kind; const char *owner; } po_selected;
static int po_normalize(void *context, const sh_package_provenance_use *use, sh_package_provenance_site *site)
{
    po_selected *selected = context;
    if (use->kind == SH_PACKAGE_USE_HISTORICAL) return 1;
    if (selected->kind == SH_PACKAGE_OCCURRENCE_WORLD) return use->kind == SH_PACKAGE_USE_WORLD ? 1 : -1;
    if (use->kind != SH_PACKAGE_USE_OBJECT) return -1;
    if (selected->owner && strcmp(selected->owner, use->owner)) return -1;
    selected->owner = use->owner; site->owner = "object"; return 1;
}
sh_package_occurrence_snapshot *sh_package_occurrence_prepare_selected(const sh_package_provenance *selected,
    unsigned kind, unsigned identity, const sh_package_bundle_binding *rows, size_t count,
    char *error, size_t capacity)
{
    sh_package_occurrence_snapshot *out;
    po_selected choice = {kind, NULL};
    if (error && capacity) error[0] = 0;
    if (kind != SH_PACKAGE_OCCURRENCE_ENTITY && kind != SH_PACKAGE_OCCURRENCE_WORLD) {
        po_error(error, capacity, "occurrence snapshot requires an explicit object or world selection"); return NULL;
    }
    out = po_create(kind);
    if (!out || (selected && !sh_package_provenance_select_uses(selected, po_normalize, &choice, &out->value)) ||
        !po_capture_sources(out, identity, rows, count)) {
        sh_package_occurrence_release(out);
        po_error(error, capacity, "occurrence source ownership is incomplete; the caller must supply its exact selected uses and retained sources");
        return NULL;
    }
    return out;
}
static int po_modern(const sh_package_provenance *document)
{
    if (!document) return SH_PACKAGE_OCCURRENCE_READY;
    if (document->semantics != SH_PACKAGE_PROVENANCE_USES) return SH_PACKAGE_OCCURRENCE_NEEDS_SELECTION;
    if (document->use_count && !document->uses) return SH_PACKAGE_OCCURRENCE_FAILED;
    for (size_t i = 0; i < document->use_count; i++)
        if (document->uses[i].kind == SH_PACKAGE_USE_HISTORICAL) return SH_PACKAGE_OCCURRENCE_NEEDS_SELECTION;
    return SH_PACKAGE_OCCURRENCE_READY;
}
sh_package_occurrence_snapshot *sh_package_occurrence_prepare_document(
    const sh_package_provenance *uses, const sh_package_bundle_binding *rows,
    size_t count, char *error, size_t capacity)
{
    sh_package_occurrence_snapshot *out = NULL;
    if (po_modern(uses) == SH_PACKAGE_OCCURRENCE_READY) {
        out = po_create(3);
        if (out && (!uses || sh_package_provenance_select_uses(uses, NULL, NULL, &out->value)) &&
            po_capture_sources(out, SH_PACKAGE_OCCURRENCE_ORIGINAL_TREES, rows, count)) return out;
    }
    sh_package_occurrence_release(out);
    po_error(error, capacity, "document ownership requires completed typed uses and their exact original sources");
    return NULL;
}
static int po_pick(void *context, const sh_package_provenance_use *use, sh_package_provenance_site *site)
{
    const po_selected *pick = context; (void)site;
    if (pick->kind == SH_PACKAGE_OCCURRENCE_WORLD) return use->kind == SH_PACKAGE_USE_WORLD;
    return use->kind == SH_PACKAGE_USE_OBJECT && !strcmp(use->owner, pick->owner);
}
static int po_prepare(const sh_package_provenance *document, const po_selected *pick,
    const sh_package_bundle_binding *rows, size_t count, sh_package_occurrence_snapshot **out,
    char *error, size_t capacity)
{
    sh_package_provenance selected = {0};
    sh_package_occurrence_snapshot *snapshot = NULL;
    int status = po_modern(document);
    if (error && capacity) error[0] = 0;
    if (!out) return po_error(error, capacity, "occurrence snapshot output is unavailable");
    if (status != SH_PACKAGE_OCCURRENCE_READY) {
        po_error(error, capacity, "historical or unassigned ownership needs the caller's typed object selection"); return status;
    }
    selected.semantics = SH_PACKAGE_PROVENANCE_USES;
    if (!document || sh_package_provenance_select_uses(document, po_pick, (void *)pick, &selected))
        snapshot = sh_package_occurrence_prepare_selected(&selected, pick->kind,
            SH_PACKAGE_OCCURRENCE_ORIGINAL_TREES, rows, count, error, capacity);
    sh_package_provenance_free(&selected);
    if (!snapshot) return po_error(error, capacity, "cannot prepare exact occurrence ownership");
    sh_package_occurrence_release(*out); *out = snapshot; return SH_PACKAGE_OCCURRENCE_READY;
}
int sh_package_occurrence_prepare_entity(const sh_package_provenance *document, size_t entity,
    const sh_package_bundle_binding *rows, size_t count, sh_package_occurrence_snapshot **out,
    char *error, size_t capacity)
{
    char owner[48]; snprintf(owner, sizeof(owner), "entity/%zu", entity);
    po_selected pick = {SH_PACKAGE_OCCURRENCE_ENTITY, owner};
    return po_prepare(document, &pick, rows, count, out, error, capacity);
}
int sh_package_occurrence_prepare_world(const sh_package_provenance *document,
    const sh_package_bundle_binding *rows, size_t count, sh_package_occurrence_snapshot **out,
    char *error, size_t capacity)
{
    po_selected pick = {SH_PACKAGE_OCCURRENCE_WORLD, NULL};
    return po_prepare(document, &pick, rows, count, out, error, capacity);
}

static int po_key_order(sh_package_occurrence_key a, sh_package_occurrence_key b)
{
    if (a.namespace_id != b.namespace_id) return a.namespace_id < b.namespace_id ? -1 : 1;
    return a.object < b.object ? -1 : a.object != b.object;
}
static int po_key_valid(sh_package_occurrence_key key) { return key.namespace_id && key.object; }
static size_t po_position(const po_entry *entries, size_t count, sh_package_occurrence_key key)
{
    size_t begin = 0;
    while (begin < count) {
        size_t middle = begin + (count - begin) / 2u;
        if (po_key_order(entries[middle].key, key) < 0) begin = middle + 1u;
        else count = middle;
    }
    return begin;
}
static void po_erase_at(sh_package_occurrence_tracker *tracker, size_t at)
{
    sh_package_occurrence_release(tracker->entries[at].snapshot);
    memmove(tracker->entries + at, tracker->entries + at + 1u,
        (tracker->count - at - 1u) * sizeof(*tracker->entries)); tracker->count--;
}
static int po_set(sh_package_occurrence_tracker *tracker, sh_package_occurrence_key key,
    sh_package_occurrence_snapshot *snapshot)
{
    size_t at = po_position(tracker->entries, tracker->count, key);
    int exists = at < tracker->count && !po_key_order(tracker->entries[at].key, key);
    if (!snapshot) { if (exists) po_erase_at(tracker, at); return 1; }
    if (!exists && tracker->count == tracker->capacity) {
        size_t capacity = tracker->capacity ? tracker->capacity * 2u : 16u;
        po_entry *grown;
        if (capacity <= tracker->capacity || capacity > SIZE_MAX / sizeof(*grown) ||
            !(grown = realloc(tracker->entries, capacity * sizeof(*grown)))) return 0;
        tracker->entries = grown; tracker->capacity = capacity;
    }
    sh_package_occurrence_retain(snapshot);
    if (exists) sh_package_occurrence_release(tracker->entries[at].snapshot);
    else {
        memmove(tracker->entries + at + 1u, tracker->entries + at,
            (tracker->count - at) * sizeof(*tracker->entries)); tracker->count++;
    }
    tracker->entries[at] = (po_entry){key, snapshot}; return 1;
}
sh_package_occurrence_tracker *sh_package_occurrence_tracker_create(void)
{
    sh_package_occurrence_tracker *tracker = calloc(1, sizeof(*tracker));
    if (tracker) InitializeSRWLock(&tracker->lock); return tracker;
}
void sh_package_occurrence_tracker_free(sh_package_occurrence_tracker *tracker)
{
    if (!tracker) return;
    for (size_t i = 0; i < tracker->count; i++) sh_package_occurrence_release(tracker->entries[i].snapshot);
    free(tracker->entries); free(tracker);
}
int sh_package_occurrence_set(sh_package_occurrence_tracker *tracker, sh_package_occurrence_key key,
    sh_package_occurrence_snapshot *snapshot)
{
    int ok;
    if (!tracker || !po_key_valid(key) || (snapshot && snapshot->kind != SH_PACKAGE_OCCURRENCE_ENTITY)) return 0;
    AcquireSRWLockExclusive(&tracker->lock); ok = po_set(tracker, key, snapshot);
    ReleaseSRWLockExclusive(&tracker->lock); return ok;
}
sh_package_occurrence_snapshot *sh_package_occurrence_get(sh_package_occurrence_tracker *tracker,
    sh_package_occurrence_key key)
{
    sh_package_occurrence_snapshot *out = NULL;
    if (!tracker || !po_key_valid(key)) return NULL;
    AcquireSRWLockShared(&tracker->lock);
    size_t at = po_position(tracker->entries, tracker->count, key);
    if (at < tracker->count && !po_key_order(tracker->entries[at].key, key))
        out = sh_package_occurrence_retain(tracker->entries[at].snapshot);
    ReleaseSRWLockShared(&tracker->lock); return out;
}
static int po_transfer(sh_package_occurrence_tracker *tracker, sh_package_occurrence_key destination,
    sh_package_occurrence_key source, int move)
{
    int ok;
    sh_package_occurrence_snapshot *snapshot = NULL;
    if (!tracker || !po_key_valid(destination) || !po_key_valid(source)) return 0;
    if (!po_key_order(destination, source)) return 1;
    AcquireSRWLockExclusive(&tracker->lock);
    size_t at = po_position(tracker->entries, tracker->count, source);
    if (at < tracker->count && !po_key_order(tracker->entries[at].key, source))
        snapshot = sh_package_occurrence_retain(tracker->entries[at].snapshot);
    ok = po_set(tracker, destination, snapshot);
    if (ok && move) po_set(tracker, source, NULL);
    sh_package_occurrence_release(snapshot);
    ReleaseSRWLockExclusive(&tracker->lock); return ok;
}
int sh_package_occurrence_copy(sh_package_occurrence_tracker *tracker,
    sh_package_occurrence_key destination, sh_package_occurrence_key source)
{ return po_transfer(tracker, destination, source, 0); }
int sh_package_occurrence_move(sh_package_occurrence_tracker *tracker,
    sh_package_occurrence_key destination, sh_package_occurrence_key source)
{ return po_transfer(tracker, destination, source, 1); }
void sh_package_occurrence_erase(sh_package_occurrence_tracker *tracker, sh_package_occurrence_key key)
{ sh_package_occurrence_set(tracker, key, NULL); }
void sh_package_occurrence_erase_namespace(sh_package_occurrence_tracker *tracker, uintptr_t namespace_id)
{
    if (!tracker) return;
    AcquireSRWLockExclusive(&tracker->lock);
    for (size_t i = tracker->count; i; i--) if (tracker->entries[i - 1u].key.namespace_id == namespace_id)
        po_erase_at(tracker, i - 1u);
    ReleaseSRWLockExclusive(&tracker->lock);
}

static int po_document_owners(const sh_package_provenance *document, size_t count)
{
    if (!document) return 1;
    for (size_t i = 0; i < document->use_count; i++) {
        const sh_package_provenance_use *use = document->uses + i;
        size_t value = 0; const char *at; char canonical[48];
        if (use->kind != SH_PACKAGE_USE_OBJECT) continue;
        if (!use->owner || strncmp(use->owner, "entity/", 7)) return 0;
        at = use->owner + 7; if (!*at) return 0;
        for (; *at; at++) {
            if (*at < '0' || *at > '9' || value > (SIZE_MAX - (size_t)(*at - '0')) / 10u) return 0;
            value = value * 10u + (size_t)(*at - '0');
        }
        snprintf(canonical, sizeof(canonical), "entity/%zu", value);
        if (value >= count || strcmp(canonical, use->owner)) return 0;
    }
    return 1;
}
static int po_attach_document(sh_package_occurrence_tracker *tracker,
    uintptr_t namespace_id, const uintptr_t *objects, size_t count, const sh_package_provenance *document,
    const sh_package_bundle_binding *rows, size_t row_count, sh_package_occurrence_snapshot **world,
    const sh_package_occurrence_snapshot *leased, char *error, size_t capacity)
{
    sh_package_occurrence_snapshot **snapshots = NULL, *next_world = NULL;
    sh_package_occurrence_tracker next = {0};
    po_entry *old = NULL; size_t old_count = 0;
    int status = po_modern(document), ok = 0;
    if (error && capacity) error[0] = 0;
    if (!tracker || !namespace_id || !world || (count && !objects) ||
        count > SIZE_MAX / sizeof(*snapshots)) goto done;
    if (status == SH_PACKAGE_OCCURRENCE_NEEDS_SELECTION) {
        po_error(error, capacity, "historical or unassigned ownership needs the caller's typed object selection"); return status;
    }
    if (!status || !po_document_owners(document, count)) goto done;
    snapshots = calloc(count ? count : 1u, sizeof(*snapshots)); if (!snapshots) goto done;
    for (size_t i = 0; i < count; i++) {
        if (!objects[i]) goto done;
        for (size_t j = 0; j < i; j++) if (objects[j] == objects[i]) goto done;
        if (sh_package_occurrence_prepare_entity(document, i, rows, row_count,
            snapshots + i, error, capacity) != SH_PACKAGE_OCCURRENCE_READY ||
            !po_copy_resources(snapshots[i], leased)) goto done;
    }
    if (sh_package_occurrence_prepare_world(document, rows, row_count, &next_world,
        error, capacity) != SH_PACKAGE_OCCURRENCE_READY || !po_copy_resources(next_world, leased)) goto done;
    AcquireSRWLockExclusive(&tracker->lock);
    if (tracker->count > SIZE_MAX - count || tracker->count + count > SIZE_MAX / sizeof(*next.entries)) goto unlock;
    next.capacity = tracker->count + count;
    next.entries = calloc(next.capacity ? next.capacity : 1u, sizeof(*next.entries)); if (!next.entries) goto unlock;
    next.count = tracker->count;
    for (size_t i = 0; i < next.count; i++) {
        next.entries[i] = tracker->entries[i]; sh_package_occurrence_retain(next.entries[i].snapshot);
    }
    for (size_t i = 0; i < count; i++) po_set(&next, (sh_package_occurrence_key){namespace_id, objects[i]}, snapshots[i]);
    old = tracker->entries; old_count = tracker->count;
    tracker->entries = next.entries; tracker->count = next.count; tracker->capacity = next.capacity;
    next.entries = NULL; ok = 1;
unlock:
    ReleaseSRWLockExclusive(&tracker->lock);
    if (ok) { sh_package_occurrence_release(*world); *world = next_world; next_world = NULL; }
done:
    for (size_t i = 0; i < old_count; i++) sh_package_occurrence_release(old[i].snapshot);
    free(old);
    if (snapshots) for (size_t i = 0; i < count; i++) sh_package_occurrence_release(snapshots[i]);
    free(snapshots); sh_package_occurrence_release(next_world);
    if (!ok) po_error(error, capacity, "document occurrence preparation failed; previous object and world ownership remains intact");
    return ok;
}
int sh_package_occurrence_attach_document(sh_package_occurrence_tracker *tracker,
    uintptr_t namespace_id, const uintptr_t *objects, size_t count, const sh_package_provenance *document,
    const sh_package_bundle_binding *rows, size_t row_count, sh_package_occurrence_snapshot **world,
    char *error, size_t capacity)
{
    return po_attach_document(tracker, namespace_id, objects, count, document, rows, row_count,
        world, NULL, error, capacity);
}
int sh_package_occurrence_attach_snapshot(sh_package_occurrence_tracker *tracker,
    uintptr_t namespace_id, const uintptr_t *objects, size_t count,
    const sh_package_occurrence_snapshot *document,
    sh_package_occurrence_snapshot **world, char *error, size_t capacity)
{
    return po_attach_document(tracker, namespace_id, objects, count,
        document ? &document->value : NULL, document ? document->bundles : NULL,
        document ? document->bundle_count : 0, world, document, error, capacity);
}

static int po_export_use(void *context, const sh_package_provenance_use *use, sh_package_provenance_site *site)
{ if (use->kind == SH_PACKAGE_USE_OBJECT) site->owner = context; return 1; }
static int po_export_add(sh_package_occurrence_snapshot *out, const sh_package_occurrence_snapshot *source,
    const char *owner)
{
    sh_package_provenance remapped = {0}, joined = {0};
    if (!source) return 1;
    int ok = sh_package_provenance_select_uses(&source->value, po_export_use, (void *)owner, &remapped) &&
        sh_package_provenance_overlay(&out->value, &remapped, &joined);
    sh_package_provenance_free(&remapped);
    if (ok) {
        sh_package_provenance_free(&out->value); out->value = joined; memset(&joined, 0, sizeof(joined));
        for (size_t i = 0; i < source->bundle_count && ok; i++) ok = po_add_bundle(out, source->bundles + i);
        if (ok) ok = po_copy_resources(out, source);
    }
    sh_package_provenance_free(&joined); return ok;
}
sh_package_occurrence_snapshot *sh_package_occurrence_export(sh_package_occurrence_tracker *tracker,
    const sh_package_occurrence_key *order, size_t count, const sh_package_occurrence_snapshot *world,
    char *error, size_t capacity)
{
    sh_package_occurrence_snapshot *out = NULL, **snapshots = NULL;
    if (error && capacity) error[0] = 0;
    if (!tracker || (count && !order) || count > SIZE_MAX / sizeof(*snapshots) ||
        (world && world->kind != SH_PACKAGE_OCCURRENCE_WORLD)) goto failed;
    out = po_create(3); snapshots = calloc(count ? count : 1u, sizeof(*snapshots));
    if (!out || !snapshots) goto failed;
    for (size_t i = 0; i < count; i++) if (!po_key_valid(order[i])) goto failed;
    AcquireSRWLockShared(&tracker->lock);
    for (size_t i = 0; i < count; i++) {
        size_t at = po_position(tracker->entries, tracker->count, order[i]);
        if (at < tracker->count && !po_key_order(tracker->entries[at].key, order[i]))
            snapshots[i] = sh_package_occurrence_retain(tracker->entries[at].snapshot);
    }
    ReleaseSRWLockShared(&tracker->lock);
    for (size_t i = 0; i < count; i++) {
        char owner[48]; snprintf(owner, sizeof(owner), "entity/%zu", i);
        if (!po_export_add(out, snapshots[i], owner)) goto failed;
    }
    if (!po_export_add(out, world, NULL)) goto failed;
    for (size_t i = 0; i < count; i++) sh_package_occurrence_release(snapshots[i]);
    free(snapshots); return out;
failed:
    if (snapshots) for (size_t i = 0; i < count; i++) sh_package_occurrence_release(snapshots[i]);
    free(snapshots); sh_package_occurrence_release(out);
    po_error(error, capacity, "cannot export exact object and world ownership"); return NULL;
}
