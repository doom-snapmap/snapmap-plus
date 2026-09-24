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
void sh_package_editor_bind_native(void)
{
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
done:
    free(objects); free(keys); sh_package_occurrence_release(world); return snapshot;
}

int sh_package_editor_attach(unsigned kind, const void *flat,
    const sh_package_occurrence_snapshot *document, char *error, size_t capacity)
{
    uintptr_t *objects = NULL; size_t count = 0;
    sh_package_occurrence_snapshot *world = NULL;
    if (!g_tracker || !pe_order(kind, flat, &objects, &count, error, capacity)) return 0;
    int ok = sh_package_occurrence_attach_snapshot(g_tracker, PE_FLAT, objects, count,
        document, &world, error, capacity);
    if (ok == SH_PACKAGE_OCCURRENCE_READY && kind == SH_PACKAGE_EDITOR_MAP) {
        AcquireSRWLockExclusive(&g_world_lock);
        sh_package_occurrence_snapshot *previous = g_world; g_world = world; world = previous;
        ReleaseSRWLockExclusive(&g_world_lock);
    }
    sh_package_occurrence_release(world); free(objects);
    return ok == SH_PACKAGE_OCCURRENCE_READY;
}

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
            next && sh_package_editor_attach(kind, flat, next, error, capacity);
    } __finally {
        g_inspecting--; sh_package_occurrence_release(previous); sh_package_occurrence_release(next);
    }
    return ok;
}

static void *pe_copy(pe_copy_fn original, void *destination, void *source, unsigned kind, int move)
{
    sh_package_occurrence_snapshot *snapshot = sh_package_occurrence_get(g_tracker, pe_key(kind, source));
    void *result = NULL;
    if (destination != source) sh_package_occurrence_erase(g_tracker, pe_key(kind, destination));
    __try {
        result = original(destination, source);
        if (!sh_package_occurrence_set(g_tracker, pe_key(kind, destination), snapshot)) pe_log_failure("entity copy");
        if (move && destination != source) sh_package_occurrence_erase(g_tracker, pe_key(kind, source));
    } __finally { sh_package_occurrence_release(snapshot); }
    return result;
}
static void *pe_copy_ctor(void *destination, void *source) { return pe_copy(g_copy_ctor, destination, source, PE_BODY, 0); }
static void *pe_copy_assign(void *destination, void *source) { return pe_copy(g_copy_assign, destination, source, PE_BODY, 0); }
static void *pe_move_assign(void *destination, void *source) { return pe_copy(g_move_assign, destination, source, PE_BODY, 1); }
static void *pe_flat_assign(void *destination, void *source) { return pe_copy(g_flat_assign, destination, source, PE_FLAT, 0); }
static void pe_body_dtor(void *body)
{ sh_package_occurrence_erase(g_tracker, pe_key(PE_BODY, body)); g_body_dtor(body); }
static void pe_flat_dtor(void *flat)
{ sh_package_occurrence_erase(g_tracker, pe_key(PE_FLAT, flat)); g_flat_dtor(flat); }
static void pe_clone(void *body, void *flat, unsigned char pinned)
{
    sh_package_occurrence_erase(g_tracker, pe_key(PE_FLAT, flat));
    g_clone(body, flat, pinned);
    if (!sh_package_occurrence_copy(g_tracker, pe_key(PE_FLAT, flat), pe_key(PE_BODY, body))) pe_log_failure("entity serialization");
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
    sh_package_occurrence_erase(g_tracker, pe_key(PE_BODY, body));
    __try {
        g_from(palette, variables, flat, body);
        if (!sh_package_occurrence_set(g_tracker, pe_key(PE_BODY, body), snapshot)) pe_log_failure("entity reconstruction");
    } __finally { sh_package_occurrence_release(snapshot); }
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
    if (ok) { sh_package_editor_clear_world(); sh_package_occurrence_tracker_free(g_tracker); g_tracker = NULL;
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
