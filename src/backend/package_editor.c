#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_editor.h"
#include "hook.h"
#include "backend_log.h"
#ifndef SH_PACKAGE_EDITOR_TESTING
#include "apply_engine.h"
#include "package_runtime.h"
#endif
#if !defined(SH_PACKAGE_EDITOR_TESTING) || defined(SH_PACKAGE_EDITOR_NATIVE_ROWS)
#include "decl_server.h"
#endif

enum { PE_BODY = 1, PE_FLAT = 2 };
static sh_package_occurrence_tracker *g_tracker;
static sh_package_occurrence_snapshot *g_world;
static SRWLOCK g_world_lock = SRWLOCK_INIT;
static sh_package_editor_inspect_fn g_inspect;
static void *g_inspect_context;
static __declspec(thread) int g_inspecting;
typedef void *(*pe_copy_fn)(void *, void *);
typedef void (*pe_dtor_fn)(void *);
typedef void (*pe_clone_fn)(void *, void *, unsigned char);
typedef void (*pe_from_fn)(void *, void *, void *, void *);
typedef char (*pe_prefab_fn)(void *, void *, int *);
static pe_copy_fn g_copy_ctor, g_copy_assign, g_move_assign, g_flat_assign;
static pe_dtor_fn g_body_dtor, g_flat_dtor;
static pe_clone_fn g_clone;
static pe_from_fn g_from;
static pe_prefab_fn g_prefab;
static int g_ready;
static volatile LONG g_faulted;
static char g_failure[512];

static sh_package_occurrence_key pe_key(unsigned kind, const void *object)
{ return (sh_package_occurrence_key){kind, (uintptr_t)object}; }
static int pe_error(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return 0; }
static void pe_log_failure(const char *operation)
{
    if (InterlockedCompareExchange(&g_faulted, 1, 0) == 0) {
        snprintf(g_failure, sizeof(g_failure), "package occurrence ownership failed during %s", operation);
        backend_log(g_failure);
    }
}
int sh_package_editor_healthy(char *error, size_t capacity)
{
    if (!g_faulted) return 1;
    return pe_error(error, capacity, g_failure[0] ? g_failure : "package occurrence ownership is incomplete");
}
void sh_package_editor_bind(sh_package_editor_inspect_fn inspect, void *context)
{ g_inspect = inspect; g_inspect_context = context; }
int sh_package_editor_ready(void) { return g_ready && g_tracker && g_inspect; }
#ifndef SH_PACKAGE_EDITOR_TESTING
static int pe_native_inspect(void *context, unsigned kind, void *flat,
    const sh_package_occurrence_snapshot *previous, sh_package_occurrence_snapshot **next,
    char *error, size_t capacity)
{
    char *json = NULL, *bound = NULL; size_t length = 0, bound_length = 0, count = 0;
    const sh_package_bundle_binding *rows = sh_package_occurrence_bundles(previous, &count);
    unsigned native_kind = kind == SH_PACKAGE_EDITOR_MAP ? SH_APPLY_FLAT_MAP :
        kind == SH_PACKAGE_EDITOR_PREFAB ? SH_APPLY_FLAT_PREFAB : SH_APPLY_FLAT_ENTITY;
    (void)context;
    if (sh_package_runtime_uninitialized()) {
        /* Before any provider is published the native engine can only have
         * built its own objects. Preserve a positive empty snapshot, rather
         * than attempting metadata reads before the registry is initialized. */
        const sh_package_provenance *uses = sh_package_occurrence_provenance(previous);
        if (count || (uses && uses->use_count))
            return pe_error(error, capacity, "package-owned object preceded native provider initialization");
        *next = sh_package_occurrence_prepare_document(NULL, NULL, 0, error, capacity);
        return *next != NULL;
    }
    if (!sh_apply_engine_serialize_flat(flat, native_kind, &json, &length))
        return pe_error(error, capacity, "native occurrence serialization failed");
    bound = sh_package_runtime_capture_document(NULL, sh_package_occurrence_provenance(previous),
        rows, count, json, length, &bound_length, next, NULL, NULL, error, capacity);
    /* Native objects already use the admitted catalog. Portable saved data is
     * rebound before deserialization; changing an active pointer here would
     * bypass its native construction/lifetime contract. */
    int ok = bound && bound_length == length && !memcmp(bound, json, length);
    if (bound && !ok) pe_error(error, capacity, "native occurrence requires rebinding before construction");
    free(bound); free(json); return ok;
}
#endif
/* Native palette contract (SnapPaletteFindEntity, identical bytes in both
 * renderers): rows run from palette+0x10 to the end pointer at palette+0x28,
 * stride 0x6f0; the key is the editor defsub at row+0x150, name at +0x58, and
 * an absent defsub compares as "". */
static int pe_native_view(void *context, void *palette, const unsigned char **entries, size_t *count, size_t *stride)
{
    const unsigned char *begin = NULL, *end = NULL; (void)context;
    __try {
        begin = *(const unsigned char *const *)((const unsigned char *)palette + 0x10);
        end = *(const unsigned char *const *)((const unsigned char *)palette + 0x28);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!begin || end < begin || (size_t)(end - begin) % 0x6f0) return 0;
    *entries = begin; *count = (size_t)(end - begin) / 0x6f0; *stride = 0x6f0; return 1;
}
static const char *pe_native_key(void *context, const void *entry)
{
    const char *key = NULL; (void)context;
    __try {
        const unsigned char *holder = *(const unsigned char *const *)((const unsigned char *)entry + 0x150);
        key = holder ? *(const char *const *)(holder + 0x58) : "";
    } __except(EXCEPTION_EXECUTE_HANDLER) { key = NULL; }
    return key;
}
static int pe_context_equal(const sh_package_provenance_context *a, const sh_package_provenance_context *b)
{
    return a->mode == b->mode && a->tree_count == b->tree_count &&
        !memcmp(a->trees, b->trees, a->tree_count * 32u) &&
        !strcmp(a->component ? a->component : "", b->component ? b->component : "");
}
/* 1 when one of owner's own occurrence uses selected exactly a context in
 * which the linker bound the publication (row: its one binding and contexts;
 * equal-resource sharing lists several, each use keeps its own). */
static int pe_owner_supplies(const void *owner, const sh_package_provenance *row)
{
    const sh_package_provenance *provenance = owner ? sh_package_occurrence_provenance(owner) : NULL;
    if (!provenance || !row || row->binding_count != 1) return 0;
    for (size_t u = 0; u < provenance->use_count; u++)
        for (size_t c = 0; c < provenance->uses[u].context_count; c++) {
            size_t index = provenance->uses[u].contexts[c];
            if (index >= provenance->context_count) continue;
            for (size_t r = 0; r < row->bindings[0].context_count; r++)
                if (row->bindings[0].contexts[r] < row->context_count &&
                    pe_context_equal(provenance->contexts + index, row->contexts + row->bindings[0].contexts[r])) return 1;
        }
    return 0;
}
#if !defined(SH_PACKAGE_EDITOR_TESTING) || defined(SH_PACKAGE_EDITOR_NATIVE_ROWS)
/* Publication lineage is decl_server's exact record of what it registered. */
static int pe_native_classify(void *context, const void *decl, char *name, size_t capacity)
{ (void)context; return sh_decl_server_row_class(decl, name, capacity); }
static const void *pe_native_resolve(void *context, const char *name)
{ (void)context; return sh_decl_server_row_resolve(name); }
static int pe_native_supplied(void *context, const void *owner, const void *decl)
{
    sh_package_provenance row = {0};
    int supplied = sh_decl_server_row_supply(decl, &row);
    (void)context;
    if (supplied == 1) supplied = pe_owner_supplies(owner, &row);
    sh_package_provenance_free(&row);
    return supplied;
}
static int pe_native_named(void *context, const void *decl, const char *name)
{ (void)context; return sh_decl_server_row_named(decl, name); }
static int pe_native_exact(void *context, const void *decl, char *name, size_t capacity)
{ (void)context; return sh_decl_server_row_exact(decl, name, capacity); }
#endif
void sh_package_editor_bind_native(void)
{
#ifdef SH_PACKAGE_EDITOR_TESTING
    static const sh_package_editor_rows_api rows = {pe_native_view, pe_native_key, NULL, NULL, NULL, NULL};
#else
    static const sh_package_editor_rows_api rows = {pe_native_view, pe_native_key, pe_native_classify,
        pe_native_resolve, pe_native_supplied, NULL, pe_native_named, pe_native_exact};
#endif
    sh_package_editor_bind_rows(&rows);
#ifndef SH_PACKAGE_EDITOR_TESTING
    sh_package_editor_bind(pe_native_inspect, NULL);
#endif
}
void sh_package_editor_clear_world(void)
{
    AcquireSRWLockExclusive(&g_world_lock);
    sh_package_occurrence_snapshot *previous = g_world; g_world = NULL;
    ReleaseSRWLockExclusive(&g_world_lock); sh_package_occurrence_release(previous);
}
sh_package_occurrence_snapshot *sh_package_editor_world(void)
{
    AcquireSRWLockShared(&g_world_lock);
    sh_package_occurrence_snapshot *out = sh_package_occurrence_retain(g_world);
    ReleaseSRWLockShared(&g_world_lock); return out;
}
void sh_package_editor_restore_world(sh_package_occurrence_snapshot *world)
{
    sh_package_occurrence_snapshot *next = sh_package_occurrence_retain(world);
    AcquireSRWLockExclusive(&g_world_lock);
    sh_package_occurrence_snapshot *previous = g_world; g_world = next;
    ReleaseSRWLockExclusive(&g_world_lock); sh_package_occurrence_release(previous);
}

static int pe_order(unsigned kind, const void *flat, uintptr_t **objects, size_t *count,
    char *error, size_t capacity)
{
    uintptr_t first = 0; int total = 0;
    *objects = NULL; *count = 0;
    if (!flat || kind < SH_PACKAGE_EDITOR_MAP || kind > SH_PACKAGE_EDITOR_ENTITY)
        return pe_error(error, capacity, "native occurrence container is unavailable");
    if (kind == SH_PACKAGE_EDITOR_ENTITY) { first = (uintptr_t)flat; total = 1; }
    else {
        size_t offset = kind == SH_PACKAGE_EDITOR_MAP ? 0x20u : 0x30u;
        __try {
            memcpy(&first, (const char *)flat + offset, sizeof(first));
            memcpy(&total, (const char *)flat + offset + 8, sizeof(total));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return pe_error(error, capacity, "native occurrence container is unreadable");
        }
    }
    if (total < 0 || (total && (!first || (size_t)total > (UINTPTR_MAX - first) / 0x1b0u)) ||
        (size_t)total > SIZE_MAX / sizeof(**objects))
        return pe_error(error, capacity, "native occurrence array is invalid");
    if (total) {
        *objects = malloc((size_t)total * sizeof(**objects));
        if (!*objects) return pe_error(error, capacity, "cannot retain native occurrence order");
        for (int i = 0; i < total; i++) (*objects)[i] = first + (size_t)i * 0x1b0u;
    }
    *count = (size_t)total; return 1;
}

static int pe_export_rows(sh_package_occurrence_snapshot *snapshot, const uintptr_t *objects, size_t count);
static int pe_attach(unsigned kind, const void *flat, const sh_package_occurrence_snapshot *document,
    int preserve, char *error, size_t capacity);
sh_package_occurrence_snapshot *sh_package_editor_snapshot(unsigned kind, const void *flat,
    char *error, size_t capacity)
{
    uintptr_t *objects = NULL; size_t count = 0;
    sh_package_occurrence_key *keys = NULL;
    sh_package_occurrence_snapshot *snapshot = NULL, *world = NULL;
    if (!sh_package_editor_healthy(error, capacity) || !g_tracker ||
        !pe_order(kind, flat, &objects, &count, error, capacity)) return NULL;
    if (count > SIZE_MAX / sizeof(*keys)) goto done;
    keys = calloc(count ? count : 1, sizeof(*keys)); if (!keys) goto done;
    for (size_t i = 0; i < count; i++) keys[i] = pe_key(PE_FLAT, (void *)objects[i]);
    if (kind == SH_PACKAGE_EDITOR_MAP) {
        AcquireSRWLockShared(&g_world_lock); world = sh_package_occurrence_retain(g_world);
        ReleaseSRWLockShared(&g_world_lock);
    }
    snapshot = sh_package_occurrence_export(g_tracker, keys, count, world, error, capacity);
    if (snapshot && !pe_export_rows(snapshot, objects, count)) {
        sh_package_occurrence_release(snapshot); snapshot = NULL;
        pe_error(error, capacity, "exact editor rows could not be recorded");
    }
done:
    free(objects); free(keys); sh_package_occurrence_release(world); return snapshot;
}

/* A loaded map or a staged prefab is authoritative for its objects: each
 * record becomes exactly the document's row, so a reused container never
 * inherits a stale record. */
int sh_package_editor_attach(unsigned kind, const void *flat,
    const sh_package_occurrence_snapshot *document, char *error, size_t capacity)
{ return pe_attach(kind, flat, document, 0, error, capacity); }

int sh_package_editor_inspect(unsigned kind, void *flat, char *error, size_t capacity)
{
    sh_package_occurrence_snapshot *previous = NULL, *next = NULL;
    int ok = 0;
    if (!g_tracker || !g_inspect || g_inspecting)
        return pe_error(error, capacity, "native occurrence inspection is unavailable");
    previous = sh_package_editor_snapshot(kind, flat, error, capacity);
    if (!previous) return 0;
    g_inspecting++;
    __try {
        ok = g_inspect(g_inspect_context, kind, flat, previous, &next, error, capacity) &&
            next && pe_attach(kind, flat, next, 1, error, capacity);
    } __finally {
        g_inspecting--; sh_package_occurrence_release(previous); sh_package_occurrence_release(next);
    }
    return ok;
}

/* Exact editor rows (see sh_package_editor_rows_api). Records are per object,
 * beside the shared occurrence snapshot, and transfer with it. A pending
 * record belongs to an object built without its choice honored, or without
 * any authoritative choice when its name is empty: it is carried exactly and
 * never re-captured, so a fallback cannot become a choice through copy/save.
 * exact (NULL when none was captured) is the captured row's exact publication
 * identity; it is never exported and travels with the record. lost: the
 * object's choice could not be recorded (an allocation failed after the
 * native operation); it is recorded but unresolvable, pending, never captured
 * and never exported, so neither a stale choice nor an engine-order
 * substitute takes its place. */
typedef struct pe_row { unsigned kind; uintptr_t object; const void *decl; LONG generation; int pending; char *name; char *exact; int lost; } pe_row;
/* name: an owned retired generation's recorded identity, matched again among
 * ties under the owner's selected supplier. */
typedef struct pe_want { const void *decl, *owner, *chosen; int recorded, outcome, exact; char name[80]; } pe_want;
static sh_package_editor_rows_api g_rows;
static pe_row *g_row_items;
static size_t g_row_count, g_row_capacity;
static char g_row_none[1]; /* every empty name: never allocated or freed */
static int g_row_overflow; /* a lost record had no slot: unrecorded objects are lost */
static SRWLOCK g_row_lock = SRWLOCK_INIT;
static volatile LONG g_row_generation = 1;
static __declspec(thread) pe_want *t_want;
void sh_package_editor_bind_rows(const sh_package_editor_rows_api *api)
{
    AcquireSRWLockExclusive(&g_row_lock);
    if (api) g_rows = *api; else memset(&g_rows, 0, sizeof(g_rows));
    InterlockedIncrement(&g_row_generation); ReleaseSRWLockExclusive(&g_row_lock);
}
void sh_package_editor_rows_retire(void) { InterlockedIncrement(&g_row_generation); }
static sh_package_editor_rows_api pe_rows_api(void)
{ AcquireSRWLockShared(&g_row_lock); sh_package_editor_rows_api api = g_rows; ReleaseSRWLockShared(&g_row_lock); return api; }
static size_t pe_row_find(unsigned kind, const void *object)
{
    for (size_t i = 0; i < g_row_count; i++)
        if (g_row_items[i].kind == kind && g_row_items[i].object == (uintptr_t)object) return i;
    return SIZE_MAX;
}
#ifdef SH_PACKAGE_EDITOR_TESTING
static long pe_alloc_fail_after = -1;
static int pe_alloc_allowed(void)
{ if (!pe_alloc_fail_after) return 0; if (pe_alloc_fail_after > 0) pe_alloc_fail_after--; return 1; }
#else
#define pe_alloc_allowed() 1
#endif
static void pe_row_free(pe_row *row) { if (row->name != g_row_none) free(row->name); free(row->exact); }
static void pe_row_drop(size_t i) { pe_row_free(g_row_items + i); g_row_items[i] = g_row_items[--g_row_count]; }
/* Locked. Replace object's record by a copy of row (NULL erases). Every copy
 * and slot is made before the record changes: on failure the old record is
 * untouched and 0 is returned, and the caller decides what survives. An
 * equal choice is updated in place and cannot fail. One free slot is kept
 * for pe_row_lose. */
static int pe_row_store(unsigned kind, const void *object, const pe_row *row)
{
    char *name = g_row_none, *exact = NULL; size_t i = pe_row_find(kind, object);
    const char *wanted = row && row->name ? row->name : "";
    if (!row) { if (i != SIZE_MAX) pe_row_drop(i); return 1; }
    if (i != SIZE_MAX && !g_row_items[i].lost && !row->lost && !strcmp(g_row_items[i].name, wanted) &&
        (g_row_items[i].exact && row->exact ? !strcmp(g_row_items[i].exact, row->exact) : g_row_items[i].exact == row->exact)) {
        g_row_items[i].decl = row->decl; g_row_items[i].generation = row->generation; g_row_items[i].pending = row->pending;
        return 1;
    }
    if (*wanted && (!pe_alloc_allowed() || !(name = _strdup(wanted)))) return 0;
    if (row->exact && (!pe_alloc_allowed() || !(exact = _strdup(row->exact)))) { if (name != g_row_none) free(name); return 0; }
    if (i == SIZE_MAX && g_row_count + 1 >= g_row_capacity) {
        size_t capacity = g_row_capacity ? g_row_capacity*2 : 16;
        pe_row *items = pe_alloc_allowed() ? realloc(g_row_items, capacity*sizeof(*items)) : NULL;
        if (!items) { if (name != g_row_none) free(name); free(exact); return 0; }
        g_row_items = items; g_row_capacity = capacity;
    }
    if (i != SIZE_MAX) pe_row_free(g_row_items + i); else i = g_row_count++;
    g_row_items[i] = (pe_row){kind, (uintptr_t)object, row->decl, row->generation, row->pending, name, exact, row->lost};
    return 1;
}
/* Locked, after a failed store. The native object already holds its new row,
 * so its previous record is stale, and no record would let a later
 * construction capture whatever the engine resolves. It becomes lost instead,
 * without allocating: the kept slot, else every unrecorded object counts as
 * lost until the rows are cleared. */
static void pe_row_lose(unsigned kind, const void *object)
{
    size_t i = pe_row_find(kind, object);
    if (i != SIZE_MAX) pe_row_free(g_row_items + i);
    else if (g_row_count < g_row_capacity) i = g_row_count++;
    else { g_row_overflow = 1; return; }
    g_row_items[i] = (pe_row){kind, (uintptr_t)object, NULL, 0, 1, g_row_none, NULL, 1};
}
static void pe_row_erase(unsigned kind, const void *object)
{ AcquireSRWLockExclusive(&g_row_lock); pe_row_store(kind, object, NULL); ReleaseSRWLockExclusive(&g_row_lock); }
static void pe_row_clear(void)
{
    AcquireSRWLockExclusive(&g_row_lock);
    while (g_row_count) pe_row_drop(g_row_count-1);
    free(g_row_items); g_row_items = NULL; g_row_capacity = 0; g_row_overflow = 0; ReleaseSRWLockExclusive(&g_row_lock);
}
/* destination takes source's record exactly. A move relabels the source's
 * record in place and cannot fail; a failed copy leaves destination lost,
 * neither its stale record nor none. */
static void pe_row_transfer(unsigned kind, void *destination, unsigned source_kind, void *source, int move)
{
    int ok = 1; size_t i;
    if (kind == source_kind && destination == source) return;
    AcquireSRWLockExclusive(&g_row_lock);
    if (move) {
        pe_row_store(kind, destination, NULL);
        if ((i = pe_row_find(source_kind, source)) != SIZE_MAX) { g_row_items[i].kind = kind; g_row_items[i].object = (uintptr_t)destination; }
    } else {
        i = pe_row_find(source_kind, source); pe_row row = i == SIZE_MAX ? (pe_row){0} : g_row_items[i];
        if (!(ok = pe_row_store(kind, destination, i == SIZE_MAX ? NULL : &row))) pe_row_lose(kind, destination);
    }
    ReleaseSRWLockExclusive(&g_row_lock);
    if (!ok) pe_log_failure("editor row transfer");
}
static int pe_row_pending(unsigned kind, const void *object)
{
    AcquireSRWLockShared(&g_row_lock); size_t i = pe_row_find(kind, object);
    int pending = i != SIZE_MAX && g_row_items[i].pending; ReleaseSRWLockShared(&g_row_lock); return pending;
}
/* Keep object's exact record (or an empty one) as unresolved. */
static void pe_row_pend(unsigned kind, void *object)
{
    int ok = 1; AcquireSRWLockExclusive(&g_row_lock); size_t i = pe_row_find(kind, object);
    if (i != SIZE_MAX) g_row_items[i].pending = 1;
    else { pe_row row = {kind, (uintptr_t)object, NULL, 0, 1, ""}; if (!(ok = pe_row_store(kind, object, &row))) pe_row_lose(kind, object); }
    ReleaseSRWLockExclusive(&g_row_lock);
    if (!ok) pe_log_failure("editor row retention");
}
/* Each exported object's flat record: a named record is the object's choice,
 * honored or retained unresolved; an empty or absent record states none. */
static int pe_export_rows(sh_package_occurrence_snapshot *snapshot, const uintptr_t *objects, size_t count)
{
    const char **rows = calloc(count ? count : 1, sizeof(*rows)); int ok, lost;
    if (!rows) return 0;
    AcquireSRWLockShared(&g_row_lock); lost = g_row_overflow;
    for (size_t i = 0; i < g_row_count && count; i++) {
        const pe_row *row = g_row_items + i; size_t at;
        if (row->kind != PE_FLAT || (!*row->name && !row->lost) || row->object < objects[0]) continue;
        at = (row->object - objects[0]) / 0x1b0u;
        if (at < count && objects[at] == row->object) { if (row->lost) lost = 1; else rows[at] = row->name; }
    }
    ok = !lost && sh_package_occurrence_document_rows(snapshot, rows, count);
    ReleaseSRWLockShared(&g_row_lock);
    free((void *)rows); return ok;
}
/* Copies of each object's recorded row: the document's table, otherwise the
 * row on its owner's OBJECT uses. Every copy precedes any change. */
static char **pe_seed_prepare(const sh_package_occurrence_snapshot *document, size_t count)
{
    const sh_package_provenance *value = sh_package_occurrence_provenance(document);
    const char **rows = calloc(count ? count : 1, sizeof(*rows));
    char **names = calloc(count ? count : 1, sizeof(*names));
    int ok = rows && names;
    for (size_t i = 0; ok && i < count; i++) rows[i] = sh_package_occurrence_document_row(document, i);
    for (size_t i = 0; ok && value && i < value->use_count; i++) {
        const sh_package_provenance_use *use = value->uses + i; size_t at;
        if (use->kind == SH_PACKAGE_USE_OBJECT && use->row &&
            sh_package_occurrence_owner_index(use->owner, count, &at) && !rows[at]) rows[at] = use->row;
    }
    for (size_t i = 0; ok && i < count; i++)
        if (rows[i]) ok = pe_alloc_allowed() && (names[i] = _strdup(rows[i])) != NULL;
    if (!ok && names) { for (size_t i = 0; i < count; i++) free(names[i]); free(names); names = NULL; }
    free((void *)rows); return names;
}
/* Locked. Room for added more records; on failure nothing changes. */
static int pe_row_reserve(size_t added)
{
    size_t need, capacity; pe_row *items;
    if (added <= g_row_capacity - g_row_count) return 1;
    if (added > SIZE_MAX / sizeof(*items) - g_row_count) return 0;
    need = g_row_count + added; capacity = g_row_capacity ? g_row_capacity : 16;
    while (capacity < need) capacity = capacity > SIZE_MAX / sizeof(*items) / 2 ? need : capacity * 2;
    if (!pe_alloc_allowed() || !(items = realloc(g_row_items, capacity * sizeof(*items)))) return 0;
    g_row_items = items; g_row_capacity = capacity; return 1;
}
/* Ownership, records and the map's world change together or not at all:
 * every row copy and record slot is prepared before the tracker's atomic
 * attach, and the records commit infallibly under the same lock. Seeds come
 * from a retired generation so their names resolve before any construction.
 * preserve (an inspection's rebuild) keeps an equal record and the record of
 * an object the document states no row for, so it never erases a choice. */
static int pe_attach(unsigned kind, const void *flat, const sh_package_occurrence_snapshot *document,
    int preserve, char *error, size_t capacity)
{
    uintptr_t *objects = NULL; size_t count = 0, added = 0; char **names;
    sh_package_occurrence_snapshot *world = NULL; int status = 0;
    if (!g_tracker || !pe_order(kind, flat, &objects, &count, error, capacity)) return 0;
    if (!(names = pe_seed_prepare(document, count))) {
        free(objects); return pe_error(error, capacity, "exact editor rows could not be restored");
    }
    AcquireSRWLockExclusive(&g_row_lock);
    for (size_t i = 0; i < count; i++) added += names[i] && pe_row_find(PE_FLAT, (const void *)objects[i]) == SIZE_MAX;
    if (!pe_row_reserve(added)) pe_error(error, capacity, "exact editor rows could not be restored");
    else status = sh_package_occurrence_attach_snapshot(g_tracker, PE_FLAT, objects, count,
        document, &world, error, capacity);
    if (status == SH_PACKAGE_OCCURRENCE_READY) {
        for (size_t i = 0; i < count; i++) {
            size_t found = pe_row_find(PE_FLAT, (const void *)objects[i]);
            if (!names[i]) { if (!preserve && found != SIZE_MAX) pe_row_drop(found); continue; }
            if (preserve && found != SIZE_MAX && !strcmp(g_row_items[found].name, names[i])) continue;
            if (found == SIZE_MAX) found = g_row_count++; else pe_row_free(g_row_items + found);
            g_row_items[found] = (pe_row){PE_FLAT, objects[i], NULL, 0, 0, names[i], NULL}; names[i] = NULL;
        }
        if (kind == SH_PACKAGE_EDITOR_MAP) {
            AcquireSRWLockExclusive(&g_world_lock);
            sh_package_occurrence_snapshot *previous = g_world; g_world = world; world = previous;
            ReleaseSRWLockExclusive(&g_world_lock);
        }
    }
    ReleaseSRWLockExclusive(&g_row_lock);
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names); sh_package_occurrence_release(world); free(objects);
    return status == SH_PACKAGE_OCCURRENCE_READY;
}
/* object came from body's authoritative row: a published row replaces its
 * record (an equal choice in place; unrecordable, lost), a positively
 * native-original row clears it, unknown retains it. A
 * published row whose exact identity is unreadable is unknown, so a record
 * never names a choice by its portable identity alone. */
static void pe_row_capture(unsigned kind, void *object, const void *body)
{
    sh_package_editor_rows_api api = pe_rows_api();
    char name[1024] = "", exact[80] = ""; const void *decl = NULL; int class = -1, ok;
    if (!api.classify) return;
    __try { decl = *(const void *const *)body; } __except(EXCEPTION_EXECUTE_HANDLER) { decl = NULL; }
    if (decl) class = api.classify(api.context, decl, name, sizeof(name));
    if (class > 0 && api.exact && api.exact(api.context, decl, exact, sizeof(exact)) != 1) class = -1;
    if (class < 0) return;
    pe_row row = {kind, (uintptr_t)object, decl, g_row_generation, 0, name, api.exact ? exact : NULL};
    AcquireSRWLockExclusive(&g_row_lock);
    if (!(ok = pe_row_store(kind, object, class ? &row : NULL))) pe_row_lose(kind, object);
    ReleaseSRWLockExclusive(&g_row_lock);
    if (!ok) pe_log_failure("editor row capture");
}
/* A record from a retired generation is never compared; it re-resolves by
 * identity. Every dependency variant of an authored row carries the same
 * portable identity, so a choice captured in this process re-resolves by its
 * exact identity, to the one current row carrying it, whoever owns the
 * occurrence, unless the owner's selected supplier supplies a row for the
 * key (steer). A saved record has only the portable identity, which only an
 * owner's supplier makes exact; an unowned one resolves nothing, as does a
 * lost record. An empty pending record states no choice, so ownership
 * derives one again. */
static pe_want pe_row_wanted(const void *flat, const void *owner)
{
    pe_want want = {NULL, owner, NULL, 0, -1}; char name[1024] = ""; sh_package_editor_rows_api api;
    int exact = 0;
    AcquireSRWLockShared(&g_row_lock); api = g_rows;
    size_t i = pe_row_find(PE_FLAT, flat);
    if (i == SIZE_MAX ? g_row_overflow : g_row_items[i].lost) want.recorded = 1; /* unresolvable */
    else if (i != SIZE_MAX && *g_row_items[i].name) {
        want.recorded = 1; exact = g_row_items[i].exact && *g_row_items[i].exact;
        if (g_row_items[i].generation == g_row_generation) want.decl = g_row_items[i].decl;
        else snprintf(name, sizeof(name), "%s", exact ? g_row_items[i].exact : g_row_items[i].name);
    }
    ReleaseSRWLockShared(&g_row_lock);
    if (want.recorded && !want.decl && exact && !owner && api.resolve) want.decl = api.resolve(api.context, name);
    if (want.recorded && !want.decl && strlen(name) < sizeof(want.name)) {
        snprintf(want.name, sizeof(want.name), "%s", name); want.exact = exact;
    }
    return want;
}
/* A portable identity names the authored row, not its dependency variant;
 * among ties carrying it, only the one its owner's selected supplier supplies
 * is the same choice. Without that supplier the choice stays unavailable,
 * however few rows carry the identity. A record saved before the portable
 * identity names its row by registered bytes; the row's own lineage (named)
 * still recognizes it, under the same supplier, and capture then records the
 * portable identity for the next save. */
static int pe_row_named(const sh_package_editor_rows_api *api, const void *decl, const char *name)
{
    char identity[80] = "";
    if (api->named) return api->named(api->context, decl, name) == 1;
    if (api->exact && api->exact(api->context, decl, identity, sizeof(identity)) == 1 && !strcmp(identity, name)) return 1;
    return api->classify(api->context, decl, identity, sizeof(identity)) > 0 && !strcmp(identity, name);
}
/* decl is the current publication of exact, this process's exact identity of
 * one dependency variant (never the portable identity all variants share). */
static int pe_row_carries(const sh_package_editor_rows_api *api, const void *decl, const char *exact)
{
    char identity[80] = "";
    return api->exact && api->exact(api->context, decl, identity, sizeof(identity)) == 1 && !strcmp(identity, exact);
}
/* Body built from flat. Only an authoritative outcome of the construction's
 * first lookup, bound as the body's row, captures what was built; otherwise
 * the flat's exact record is carried as unresolved. */
static void pe_row_built(void *body, void *flat, const pe_want *want)
{
    const void *decl = NULL;
    pe_row_transfer(PE_BODY, body, PE_FLAT, flat, 0);
    if (!pe_rows_api().classify) return;
    __try { decl = *(const void *const *)body; } __except(EXCEPTION_EXECUTE_HANDLER) { decl = NULL; }
    if (want->outcome >= SH_ROW_ENGINE && want->outcome <= SH_ROW_NATIVE_DEFAULT && decl && decl == want->chosen)
        pe_row_capture(PE_BODY, body, body);
    else pe_row_pend(PE_BODY, body);
}
const void *sh_package_editor_row_steer(void *palette, const char *name, const void *engine, int *outcome)
{
    pe_want *want = t_want; sh_package_editor_rows_api api;
    const unsigned char *entries = NULL; const void *native = NULL, *slot = NULL, *wanted = NULL, *derived = NULL, *baseline;
    size_t count = 0, stride = 0, ties = 0, natives = 0, slots = 0, unknown = 0, unreadable = 0, supplied = 0;
    size_t named_supplied = 0, carrying = 0, exact_ties = 0, owner_rows = 0;
    const void *named_owned = NULL, *exact_entry = NULL;
    int result = SH_ROW_ENGINE;
    if (!want) { if (outcome) *outcome = result; return engine; } /* only during pe_from */
    api = pe_rows_api();
    if (engine && name && api.view && api.key && api.classify &&
        api.view(api.context, palette, &entries, &count, &stride) && entries && stride >= sizeof(void *) &&
        (const unsigned char *)engine >= entries && (size_t)((const unsigned char *)engine - entries) < count*stride) {
        __try {
            for (size_t i = 0; i < count; i++) {
                const unsigned char *entry = entries + i*stride; const void *decl = *(const void *const *)entry;
                const char *key = api.key(api.context, entry); int class, named;
                if (!key) { unreadable++; continue; }
                if (strcmp(key, name)) continue;
                ties++;
                /* The record's identity present in this publication, whoever owns the choice. */
                named = !want->decl && *want->name && pe_row_named(&api, decl, want->name);
                if (want->decl ? decl == want->decl : named) carrying++;
                if (want->decl && decl == want->decl) wanted = entry;
                else if (named && want->owner && api.supplied &&
                         api.supplied(api.context, want->owner, decl) == 1 && !named_supplied++) named_owned = entry;
                if (named && want->exact && pe_row_carries(&api, decl, want->name) && !exact_ties++) exact_entry = entry;
                /* Any row the owner's selection supplies, or may: the owner is this key's source. */
                if (want->exact && want->owner && (!api.supplied || api.supplied(api.context, want->owner, decl) != 0)) owner_rows++;
                if (!(class = api.classify(api.context, decl, NULL, 0)) && !natives++) native = entry;
                else if (class < 0) unknown++;
                else if (class == 2 && !slots++) slot = entry;
                if (class > 0 && want->owner && !want->recorded && api.supplied &&
                    api.supplied(api.context, want->owner, decl) == 1 && !supplied++) derived = entry;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) { unreadable++; } /* engine strings */
        if (!wanted && named_supplied == 1) wanted = named_owned; /* never an unconfirmed variant */
        /* A captured choice keeps its exact identity across publications: an
         * owner whose selection supplies nothing for this key is no source for
         * it. One that does is the key's source; that conflict stays unresolved. */
        if (!wanted && exact_ties == 1 && !owner_rows && !unreadable) {
            char line[384];
            wanted = exact_entry;
            snprintf(line, sizeof(line), "MPKG: editor row exact choice retained for %.200s (%s; ties=%zu carrying=%zu owner_rows=0)",
                name, want->owner ? "owned" : "unowned", ties, carrying);
            backend_log(line);
        }
        if (wanted) { result = SH_ROW_RECORDED; engine = wanted; }
        else if (unreadable && (want->recorded || ties)) result = SH_ROW_UNREADABLE;
        else if (want->recorded) result = SH_ROW_UNAVAILABLE;
        else if (want->owner) {
            /* Owned without a record, whatever the tie count: only its own
             * supplying publication is a choice. A sole native or unrelated
             * row is built but never captured as authored. */
            result = supplied == 1 ? SH_ROW_DERIVED : supplied ? SH_ROW_OWNED_AMBIGUOUS : SH_ROW_OWNED_NONE;
            if (result == SH_ROW_DERIVED) engine = derived;
        } else if (ties > 1) result = natives == 1 && !unknown ? SH_ROW_NATIVE_DEFAULT : SH_ROW_AMBIGUOUS;
        /* The native representation: the native original, else the one changed
         * row in its native SnapMap slot (built, never captured as authored). */
        baseline = unknown ? NULL : natives == 1 ? native : !natives && slots == 1 ? slot : NULL;
        if ((result == SH_ROW_NATIVE_DEFAULT || result >= SH_ROW_UNAVAILABLE) && baseline) engine = baseline;
        if (result >= SH_ROW_UNAVAILABLE && (ties > 1 || want->recorded || result == SH_ROW_UNREADABLE)) {
            static const char *const names[] = {"unavailable", "owned without supplier", "owned with several suppliers", "ambiguous", "unreadable"};
            char line[512];
            snprintf(line, sizeof(line), "MPKG: editor row %s for %.200s (%s; ties=%zu carrying=%zu native=%zu slot=%zu unknown=%zu unreadable=%zu supplied=%zu%s); choice retained unresolved, built %s",
                names[result - SH_ROW_UNAVAILABLE], name, want->owner ? "owned" : "unowned", ties, carrying, natives, slots, unknown,
                unreadable, want->recorded ? named_supplied : supplied, want->exact ? (owner_rows ? " exact, owner supplies key" : " exact") : "",
                !baseline || engine != baseline ? "by engine order" : natives ? "native original" : "native slot");
            backend_log(line);
        }
        if (want->outcome < 0) { want->outcome = result; want->chosen = *(const void *const *)engine; }
    }
    if (outcome) *outcome = result;
    return engine;
}
static void *pe_copy(pe_copy_fn original, void *destination, void *source, unsigned kind, int move)
{
    sh_package_occurrence_snapshot *snapshot = sh_package_occurrence_get(g_tracker, pe_key(kind, source));
    void *result = NULL;
    if (destination != source) sh_package_occurrence_erase(g_tracker, pe_key(kind, destination));
    __try {
        result = original(destination, source);
        if (!sh_package_occurrence_set(g_tracker, pe_key(kind, destination), snapshot)) pe_log_failure("entity copy");
        pe_row_transfer(kind, destination, kind, source, move);
        if (move && destination != source) sh_package_occurrence_erase(g_tracker, pe_key(kind, source));
    } __finally { sh_package_occurrence_release(snapshot); }
    return result;
}
static void *pe_copy_ctor(void *destination, void *source) { return pe_copy(g_copy_ctor, destination, source, PE_BODY, 0); }
static void *pe_copy_assign(void *destination, void *source) { return pe_copy(g_copy_assign, destination, source, PE_BODY, 0); }
static void *pe_move_assign(void *destination, void *source) { return pe_copy(g_move_assign, destination, source, PE_BODY, 1); }
static void *pe_flat_assign(void *destination, void *source) { return pe_copy(g_flat_assign, destination, source, PE_FLAT, 0); }
static void pe_body_dtor(void *body)
{ sh_package_occurrence_erase(g_tracker, pe_key(PE_BODY, body)); pe_row_erase(PE_BODY, body); g_body_dtor(body); }
static void pe_flat_dtor(void *flat)
{ sh_package_occurrence_erase(g_tracker, pe_key(PE_FLAT, flat)); pe_row_erase(PE_FLAT, flat); g_flat_dtor(flat); }
static void pe_clone(void *body, void *flat, unsigned char pinned)
{
    sh_package_occurrence_erase(g_tracker, pe_key(PE_FLAT, flat));
    g_clone(body, flat, pinned);
    if (!sh_package_occurrence_copy(g_tracker, pe_key(PE_FLAT, flat), pe_key(PE_BODY, body))) pe_log_failure("entity serialization");
    pe_row_transfer(PE_FLAT, flat, PE_BODY, body, 0); if (!pe_row_pending(PE_BODY, body)) pe_row_capture(PE_FLAT, flat, body);
}
static void pe_from(void *palette, void *variables, void *flat, void *body)
{
    sh_package_occurrence_snapshot *snapshot = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, flat));
    char error[512] = "";
    if (!snapshot && g_inspect && !g_inspecting) {
        if (!sh_package_editor_inspect(SH_PACKAGE_EDITOR_ENTITY, flat, error, sizeof(error))) {
            if (*error) backend_log(error);
            pe_log_failure("entity reconstruction inspection");
        } else snapshot = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, flat));
    }
    /* After inspection: map-authoritative ownership is known before any default. */
    pe_want want = pe_row_wanted(flat, snapshot), *previous = t_want;
    sh_package_occurrence_erase(g_tracker, pe_key(PE_BODY, body));
    __try {
        t_want = &want;
        g_from(palette, variables, flat, body);
        if (!sh_package_occurrence_set(g_tracker, pe_key(PE_BODY, body), snapshot)) pe_log_failure("entity reconstruction");
    } __finally { t_want = previous; sh_package_occurrence_release(snapshot); }
    pe_row_built(body, flat, &want);
}
static char pe_prefab(void *prefab, void *editor, int *status)
{
    char result = g_prefab(prefab, editor, status), error[512] = "";
    if (result && g_inspect && !g_inspecting && !sh_package_editor_inspect(SH_PACKAGE_EDITOR_PREFAB, prefab, error, sizeof(error))) {
        if (*error) backend_log(error); return 0;
    }
    return result;
}

typedef struct pe_hook {
    const char *name;
    size_t anchor, stolen, lea;
    void *detour;
    void **original;
} pe_hook;
/* Tail anchors distinguish copy/move assignment's long shared prefix. Their
 * paired offsets and unchanged 15-byte entry are verified before detouring. */
static pe_hook g_hooks[] = {
    {"EntityBodyCopyCtor", 0, 19, SIZE_MAX, pe_copy_ctor, (void **)&g_copy_ctor},
    {"EntityBodyCopyAssignAnchor", 914, 15, SIZE_MAX, pe_copy_assign, (void **)&g_copy_assign},
    {"EntityBodyMoveAssignAnchor", 913, 15, SIZE_MAX, pe_move_assign, (void **)&g_move_assign},
    {"EntityBodyDtor", 0, 15, SIZE_MAX, pe_body_dtor, (void **)&g_body_dtor},
    {"EntityFromFlat", 0, 21, SIZE_MAX, pe_from, (void **)&g_from},
    {"EntityClone", 0, 15, SIZE_MAX, pe_clone, (void **)&g_clone},
    {"FlatEntityAssign", 0, 16, SIZE_MAX, pe_flat_assign, (void **)&g_flat_assign},
    {"EntityDefDtor", 0, 17, 10, pe_flat_dtor, (void **)&g_flat_dtor},
    {"PrefabPopulate", 0, 20, SIZE_MAX, pe_prefab, (void **)&g_prefab},
};
int sh_package_editor_uninstall(void)
{
    int ok = 1; g_ready = 0;
    for (size_t i = sizeof(g_hooks)/sizeof(*g_hooks); i; i--) if (*g_hooks[i-1].original) {
        if (hook_unpatch(*g_hooks[i-1].original)) *g_hooks[i-1].original = NULL; else ok = 0;
    }
    if (ok) { sh_package_editor_clear_world(); sh_package_occurrence_tracker_free(g_tracker); g_tracker = NULL; pe_row_clear();
        InterlockedExchange(&g_faulted, 0); g_failure[0] = 0; }
    return ok;
}
int sh_package_editor_install(const sig_result *results, size_t count, const uint8_t *base)
{
    static const unsigned char assign_entry[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20};
    uintptr_t targets[sizeof(g_hooks)/sizeof(*g_hooks)] = {0};
    if (g_ready) return 1;
    if (!results || !base || !sh_package_editor_uninstall()) return 0;
    for (size_t i = 0; i < sizeof(g_hooks)/sizeof(*g_hooks); i++) {
        const pe_hook *h = g_hooks+i;
        for (size_t j = 0; j < count; j++) if (results[j].name && !strcmp(results[j].name, h->name)) {
            if (targets[i] || results[j].status != SIG_OK || results[j].rva < h->anchor ||
                results[j].addr != (uintptr_t)base + results[j].rva) return 0;
            targets[i] = results[j].addr - h->anchor;
        }
        if (!targets[i]) return 0;
        if (h->anchor) {
            __try { if (memcmp((void *)targets[i], assign_entry, sizeof(assign_entry))) return 0; }
            __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }
    }
    g_tracker = sh_package_occurrence_tracker_create(); if (!g_tracker) return 0;
    for (size_t i = 0; i < sizeof(g_hooks)/sizeof(*g_hooks); i++) {
        pe_hook *h = g_hooks+i;
        *h->original = h->lea == SIZE_MAX ? hook_prepare((void *)targets[i], h->detour, h->stolen) :
            hook_prepare_rip_lea((void *)targets[i], h->detour, h->stolen, h->lea);
        if (!*h->original) { sh_package_editor_uninstall(); return 0; }
    }
    for (size_t i = 0; i < sizeof(g_hooks)/sizeof(*g_hooks); i++)
        if (hook_commit(*g_hooks[i].original) != B2_PATCH_OK) { sh_package_editor_uninstall(); return 0; }
    g_ready = 1; return 1;
}
