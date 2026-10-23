#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "virtual_material_runtime.h"
#include "virtual_material_native.h"
#include "virtual_texture_provider.h"
#include "hook.h"
#include "backend_log.h"

typedef struct vmr_row {
    sh_virtual_material_native *native;
    struct vmr_row *next;
} vmr_row;
typedef struct vmr_entry {
    char *name, *atlas;
    sh_virtual_material_record record;
    int32_t dlc;
    vmr_row *rows;
    sh_virtual_page_source *pages;
    char *page_paths[21];
    int page_kind, page_mode, has_record;
} vmr_entry;
struct sh_virtual_material_view {
    vmr_entry *entries;
    size_t count;
    char error[512];
    struct sh_virtual_material_view *next;
    int published, retired;
};
typedef void *(*vmr_lookup_fn)(void *, void *);
static vmr_lookup_fn g_vmr_lookup;
static SRWLOCK g_vmr_lock = SRWLOCK_INIT;
static sh_virtual_material_view *g_vmr_active, *g_vmr_views;
static volatile LONG g_vmr_ready, g_vmr_attempted;

static int vmr_error(char *error, size_t capacity, const char *message)
{ if (error && capacity) snprintf(error, capacity, "%s", message); return 0; }
static char *vmr_name(const char *name)
{
    size_t n = name ? strnlen(name, 4096) : 0;
    return n && n < 4096 ? sh_virtual_material_name(name, n) : NULL;
}
static int vmr_native_name(const char *name, char out[4096])
{
    size_t n = name ? strnlen(name, 4096) : 0;
    return sh_virtual_material_name_copy(name, n, out, 4096);
}
static int vmr_order(const void *a, const void *b)
{ return strcmp(((const vmr_entry *)a)->name, ((const vmr_entry *)b)->name); }
static vmr_entry *vmr_find(sh_virtual_material_view *view, const char *name)
{
    vmr_entry key = {0}; key.name = (char *)name;
    return view && view->count ? bsearch(&key, view->entries, view->count,
        sizeof(key), vmr_order) : NULL;
}
static int vmr_equal(const vmr_entry *a, const vmr_entry *b)
{
    const sh_virtual_material_record *x = &a->record, *y = &b->record;
    return !strcmp(a->name, b->name) && !strcmp(a->atlas, b->atlas) && a->dlc == b->dlc &&
        a->has_record == b->has_record && a->page_kind == b->page_kind && a->page_mode == b->page_mode &&
        (a->pages && b->pages ? !memcmp(sh_virtual_page_source_identity(a->pages),
            sh_virtual_page_source_identity(b->pages), 32) : a->pages == b->pages) &&
        x->x == y->x && x->y == y->y && x->width == y->width && x->height == y->height &&
        x->flags == y->flags && x->timestamp == y->timestamp && x->checksum == y->checksum;
}
int sh_virtual_material_runtime_binding_equal(const sh_virtual_material_view *a,
    const sh_virtual_material_view *b, const char *name)
{
    const vmr_entry *x, *y;
    if (!name || !*name) return 0;
    x = vmr_find((sh_virtual_material_view *)a, name);
    y = vmr_find((sh_virtual_material_view *)b, name);
    return !x || !y ? x == y : vmr_equal(x, y);
}
static void vmr_free(sh_virtual_material_view *view)
{
    if (!view) return;
    for (size_t i = 0; i < view->count; i++) {
        free(view->entries[i].name); free(view->entries[i].atlas);
        sh_virtual_page_source_release(view->entries[i].pages);
        for (size_t j = 0; j < 21; j++) free(view->entries[i].page_paths[j]);
    }
    free(view->entries); free(view);
}
sh_virtual_material_view *sh_virtual_material_runtime_prepare(
    const sh_virtual_material_runtime_binding *bindings, size_t count,
    char *error, size_t capacity)
{
    sh_virtual_material_view *view = NULL;
    if (error && capacity) error[0] = 0;
    if ((count && !bindings) || count > SIZE_MAX / sizeof(vmr_entry)) {
        vmr_error(error, capacity, "invalid virtual material binding inventory"); return NULL;
    }
    view = calloc(1, sizeof(*view));
    if (!view || (count && !(view->entries = calloc(count, sizeof(*view->entries))))) goto allocation;
    for (size_t i = 0; i < count; i++) {
        vmr_entry *entry = &view->entries[view->count++]; float mapping[4];
        entry->name = vmr_name(bindings[i].name); entry->atlas = vmr_name(bindings[i].atlas);
        entry->has_record = bindings[i].record != NULL;
        if (!entry->name || !entry->atlas ||
            (entry->has_record ? !sh_virtual_material_mapping(bindings[i].record, INT32_MAX, mapping) : !bindings[i].pages)) {
            vmr_error(error, capacity, "invalid resolved virtual material binding"); goto failed;
        }
        if (entry->has_record) entry->record = *bindings[i].record;
        entry->record.name = entry->name;
        entry->dlc = bindings[i].dlc;
        if (bindings[i].pages) {
            if ((bindings[i].page_kind != 1 && bindings[i].page_kind != 2) ||
                bindings[i].page_mode < 0 || bindings[i].page_mode > 2 || !bindings[i].page_paths) {
                vmr_error(error, capacity, "private virtual material lacks its native page contract"); goto failed;
            }
            entry->pages = sh_virtual_page_source_retain(bindings[i].pages);
            entry->page_kind = bindings[i].page_kind; entry->page_mode = bindings[i].page_mode;
            for (unsigned j = 0; j < 21; j++) {
                int present = sh_virtual_page_source_member(entry->pages, j) != NULL;
                if (present != !!bindings[i].page_paths[j] ||
                    (present && !(entry->page_paths[j] = _strdup(bindings[i].page_paths[j])))) goto allocation;
            }
        }
    }
    if (view->count) qsort(view->entries, view->count, sizeof(*view->entries), vmr_order);
    /* Check before compacting so a rejected candidate has ordinary ownership. */
    for (size_t i = 1; i < view->count; i++)
        if (!strcmp(view->entries[i - 1].name, view->entries[i].name) &&
            (!vmr_equal(&view->entries[i - 1], &view->entries[i]) ||
             (view->entries[i].pages && sh_virtual_page_source_equal(view->entries[i - 1].pages,
                view->entries[i].pages, error, capacity) != 1))) {
            vmr_error(error, capacity, "one compiled material has different atlas bindings"); goto failed;
        }
    {
        size_t retained = 0;
        for (size_t i = 0; i < view->count; i++) {
            if (retained && !strcmp(view->entries[retained - 1].name, view->entries[i].name)) {
                free(view->entries[i].name); free(view->entries[i].atlas);
                sh_virtual_page_source_release(view->entries[i].pages);
                for (size_t j = 0; j < 21; j++) free(view->entries[i].page_paths[j]);
            } else { if (retained != i) view->entries[retained] = view->entries[i]; retained++; }
        }
        view->count = retained;
    }
    return view;
allocation:
    vmr_error(error, capacity, "cannot retain virtual material bindings");
failed:
    vmr_free(view); return NULL;
}
/* Enumerate a view's bindings (R-344 stage B), borrowed until the view is released. */
size_t sh_virtual_material_runtime_count(const sh_virtual_material_view *view) { return view ? view->count : 0; }
int sh_virtual_material_runtime_at(const sh_virtual_material_view *view, size_t index,
    sh_virtual_material_runtime_binding *out)
{
    if (!view || index >= view->count || !out) return 0;
    const vmr_entry *entry = view->entries + index;
    *out = (sh_virtual_material_runtime_binding){entry->name, entry->atlas, entry->has_record ? &entry->record : NULL,
        entry->dlc, entry->pages, entry->page_kind, entry->page_mode,
        entry->pages ? (const char *const *)entry->page_paths : NULL};
    return 1;
}
int sh_virtual_material_runtime_seal(sh_virtual_material_view *view,
    const sh_package_compilation *compiled, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!view) return 1;
    if (view->published || !compiled) return vmr_error(error, capacity, "virtual page streams must be sealed before publication");
    for (size_t i = 0; i < view->count; i++) if (view->entries[i].pages &&
        !sh_virtual_page_source_seal(view->entries[i].pages, compiled,
            (const char *const *)view->entries[i].page_paths, error, capacity)) return 0;
    return 1;
}
static int vmr_private(const sh_virtual_material_view *view)
{
    for (size_t i = 0; view && i < view->count; i++) if (view->entries[i].pages) return 1;
    return 0;
}
static int vmr_admissible(sh_virtual_material_view *candidate, char *error, size_t capacity)
{
    if (candidate && candidate->count && !sh_virtual_material_runtime_ready())
        return vmr_error(error, capacity, "native virtual material lookup is unavailable");
    if (vmr_private(candidate) && !sh_vt_provider_ready())
        return vmr_error(error, capacity, "private virtual texture page providers are unavailable");
    return 1;
}
/* Called under g_vmr_lock; reservation and forward publish share the exact
 * alias/registry contract. Inactive registration cannot expose new bindings. */
static int vmr_register_locked(sh_virtual_material_view *candidate, char *error, size_t capacity)
{
    if (candidate && candidate->retired) {
        return vmr_error(error, capacity, "cannot publish retired virtual material bindings");
    }
    if (candidate) for (sh_virtual_material_view *v = g_vmr_views; v; v = v->next)
        for (size_t i = 0; i < candidate->count; i++) {
            vmr_entry *other = vmr_find(v, candidate->entries[i].name);
            if (other && !vmr_equal(other, &candidate->entries[i])) {
                return vmr_error(error, capacity, "a retained material alias has a different atlas binding");
            }
        }
    if (candidate && !candidate->published) {
        candidate->published = 1; candidate->next = g_vmr_views; g_vmr_views = candidate;
    }
    return 1;
}
int sh_virtual_material_runtime_reserve(sh_virtual_material_view *candidate, char *error, size_t capacity)
{
    int ok;
    if (error && capacity) *error = 0;
    if (!vmr_admissible(candidate, error, capacity)) return 0;
    AcquireSRWLockExclusive(&g_vmr_lock);
    ok = vmr_register_locked(candidate, error, capacity);
    ReleaseSRWLockExclusive(&g_vmr_lock); return ok;
}
int sh_virtual_material_runtime_publish(sh_virtual_material_view *candidate,
    sh_virtual_material_view **previous, char *error, size_t capacity)
{
    int ok;
    if (error && capacity) *error = 0;
    if (!previous) return vmr_error(error, capacity, "missing virtual material rollback handle");
    if (!vmr_admissible(candidate, error, capacity)) return 0;
    AcquireSRWLockExclusive(&g_vmr_lock);
    ok = vmr_register_locked(candidate, error, capacity);
    if (ok) { *previous = g_vmr_active; g_vmr_active = candidate; }
    ReleaseSRWLockExclusive(&g_vmr_lock); return ok;
}
void sh_virtual_material_runtime_restore(sh_virtual_material_view *previous)
{
    AcquireSRWLockExclusive(&g_vmr_lock);
    g_vmr_active = previous;
    ReleaseSRWLockExclusive(&g_vmr_lock);
}
sh_virtual_material_view *sh_virtual_material_runtime_join(
    const sh_virtual_material_view *a, const sh_virtual_material_view *b,
    char *error, size_t capacity)
{
    size_t ac = a ? a->count : 0, bc = b ? b->count : 0, used = 0;
    sh_virtual_material_runtime_binding *bindings;
    sh_virtual_material_view *view;
    if (error && capacity) error[0] = 0;
    if (ac > SIZE_MAX - bc || ac + bc > SIZE_MAX / sizeof(*bindings)) {
        vmr_error(error, capacity, "virtual material binding count overflow"); return NULL;
    }
    bindings = calloc(ac + bc ? ac + bc : 1, sizeof(*bindings));
    if (!bindings) { vmr_error(error, capacity, "cannot copy virtual material bindings"); return NULL; }
    /* The caller retains the source views. Their binding metadata is immutable;
     * only native association lists and status change after preparation. */
    const sh_virtual_material_view *inputs[] = {a, b};
    for (size_t j = 0; j < 2; j++) if (inputs[j])
        for (size_t i = 0; i < inputs[j]->count; i++) {
            const vmr_entry *entry = &inputs[j]->entries[i];
            bindings[used++] = (sh_virtual_material_runtime_binding){
                entry->name, entry->atlas, entry->has_record ? &entry->record : NULL, entry->dlc,
                entry->pages, entry->page_kind, entry->page_mode, (const char *const *)entry->page_paths};
        }
    view = sh_virtual_material_runtime_prepare(bindings, used, error, capacity);
    free(bindings); return view;
}
sh_virtual_material_view *sh_virtual_material_runtime_filter(
    const sh_virtual_material_view *source, int (*keep)(void *, const char *), void *context,
    char *error, size_t capacity)
{
    size_t count = source ? source->count : 0, used = 0;
    sh_virtual_material_runtime_binding *bindings;
    sh_virtual_material_view *view = NULL;
    if (error && capacity) *error = 0;
    if (!keep || count > SIZE_MAX / sizeof(*bindings) ||
        !(bindings = calloc(count ? count : 1, sizeof(*bindings)))) {
        vmr_error(error, capacity, "cannot select virtual material bindings"); return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        const vmr_entry *entry = source->entries + i;
        int selected = keep(context, entry->name);
        if (selected < 0) { vmr_error(error, capacity, "virtual material binding selection failed"); goto done; }
        if (selected) bindings[used++] = (sh_virtual_material_runtime_binding){
            entry->name, entry->atlas, entry->has_record ? &entry->record : NULL, entry->dlc,
            entry->pages, entry->page_kind, entry->page_mode, (const char *const *)entry->page_paths};
    }
    view = sh_virtual_material_runtime_prepare(bindings, used, error, capacity);
done:
    free(bindings); return view;
}
/* Caller holds both the lookup lock and the native consumer barrier. */
static void vmr_collect(sh_virtual_material_view *view)
{
    for (size_t i = 0; i < view->count; i++) {
        vmr_row **slot = &view->entries[i].rows;
        while (*slot) {
            vmr_row *row = *slot;
            if (sh_virtual_material_native_release(&row->native)) { *slot = row->next; free(row); }
            else slot = &row->next;
        }
    }
}
typedef struct vmr_atlases { char **names; size_t count, capacity; } vmr_atlases;
static int vmr_atlas_add(vmr_atlases *set, const char *atlas)
{
    for (size_t i = 0; i < set->count; i++) if (!strcmp(set->names[i], atlas)) return 1;
    if (set->count == set->capacity) {
        size_t next = set->capacity ? set->capacity * 2 : 8;
        char **grown = next > set->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(set->names, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        set->names = grown; set->capacity = next;
    }
    if (!(set->names[set->count] = _strdup(atlas))) return 0;
    set->count++; return 1;
}
static void vmr_atlases_free(vmr_atlases *set)
{
    for (size_t i = 0; i < set->count; i++) free(set->names[i]);
    free(set->names); memset(set, 0, sizeof(*set));
}
static int vmr_atlas_kept(void *context, const char *name)
{
    const vmr_atlases *set = context;
    for (size_t i = 0; i < set->count; i++) if (!strcmp(set->names[i], name)) return 1;
    return 0;
}
int sh_virtual_material_runtime_providers(char *error, size_t capacity)
{
    vmr_atlases wanted = {0}; int ok = 1;
    if (error && capacity) *error = 0;
    AcquireSRWLockShared(&g_vmr_lock);
    for (size_t i = 0; ok && g_vmr_active && i < g_vmr_active->count; i++)
        if (g_vmr_active->entries[i].pages) ok = vmr_atlas_add(&wanted, g_vmr_active->entries[i].atlas);
    ReleaseSRWLockShared(&g_vmr_lock);
    if (!ok) vmr_error(error, capacity, "cannot list private virtual texture page providers");
    else if (wanted.count) ok = sh_vt_provider_acquire((const char *const *)wanted.names, wanted.count, error, capacity);
    if (!ok) {
        /* A private material must not parse against a missing provider; the
         * activation's status check reports this instead of a fallback. */
        AcquireSRWLockExclusive(&g_vmr_lock);
        if (g_vmr_active && !*g_vmr_active->error) {
            snprintf(g_vmr_active->error, sizeof(g_vmr_active->error), "%s",
                error && *error ? error : "private virtual texture page providers could not be built");
            backend_log(g_vmr_active->error);
        }
        ReleaseSRWLockExclusive(&g_vmr_lock);
    }
    vmr_atlases_free(&wanted); return ok;
}
void sh_virtual_material_runtime_collect(void)
{
    vmr_atlases kept = {0}; int listed = 1;
    AcquireSRWLockExclusive(&g_vmr_lock);
    sh_virtual_material_view **slot = &g_vmr_views;
    while (*slot) {
        sh_virtual_material_view *view = *slot; int live = 0;
        vmr_collect(view);
        for (size_t i = 0; i < view->count; i++) if (view->entries[i].rows) { live = 1; break; }
        if (view->retired && !live) { *slot = view->next; vmr_free(view); }
        else slot = &view->next;
    }
    /* Every retained view keeps its private providers; so does any parsed
     * material still selecting one, which the provider scan checks itself. */
    for (sh_virtual_material_view *view = g_vmr_views; listed && view; view = view->next)
        for (size_t i = 0; listed && i < view->count; i++)
            if (view->entries[i].pages) listed = vmr_atlas_add(&kept, view->entries[i].atlas);
    ReleaseSRWLockExclusive(&g_vmr_lock);
    if (listed) (void)sh_vt_provider_collect(vmr_atlas_kept, &kept);
    vmr_atlases_free(&kept);
}
int sh_virtual_material_runtime_retire(sh_virtual_material_view **handle)
{
    int ok = 0;
    if (!handle || !*handle) return 1;
    AcquireSRWLockExclusive(&g_vmr_lock);
    if (*handle != g_vmr_active) {
        if ((*handle)->published) (*handle)->retired = 1;
        else vmr_free(*handle);
        *handle = NULL; ok = 1;
    }
    ReleaseSRWLockExclusive(&g_vmr_lock); return ok;
}
int sh_virtual_material_runtime_release(sh_virtual_material_view **handle)
{
    sh_virtual_material_view *view, **slot; int ok = 0;
    if (!handle || !*handle) return 1;
    AcquireSRWLockExclusive(&g_vmr_lock);
    view = *handle;
    if (view == g_vmr_active) goto done;
    vmr_collect(view);
    for (size_t i = 0; i < view->count; i++) if (view->entries[i].rows) goto done;
    slot = &g_vmr_views;
    while (*slot && *slot != view) slot = &(*slot)->next;
    if (*slot) *slot = view->next;
    vmr_free(view); *handle = NULL; ok = 1;
done:
    ReleaseSRWLockExclusive(&g_vmr_lock); return ok;
}
int sh_virtual_material_runtime_status(char *error, size_t capacity)
{
    int ok;
    if (error && capacity) error[0] = 0;
    AcquireSRWLockShared(&g_vmr_lock);
    ok = !g_vmr_active || !*g_vmr_active->error;
    if (!ok) vmr_error(error, capacity, g_vmr_active->error);
    ReleaseSRWLockShared(&g_vmr_lock); return ok;
}
int sh_virtual_material_runtime_healthy(const sh_virtual_material_view *view)
{
    int healthy;
    AcquireSRWLockShared(&g_vmr_lock); healthy = !view || !*view->error;
    ReleaseSRWLockShared(&g_vmr_lock); return healthy;
}
static void vmr_fault(const char *name, const char *reason)
{
    if (g_vmr_active && !*g_vmr_active->error) {
        snprintf(g_vmr_active->error, sizeof(g_vmr_active->error),
            "virtual material %s: %s", name ? name : "?", reason);
        backend_log(g_vmr_active->error);
    }
}
/* Native reads are inside the hook's SEH scope. No engine calls or provider
 * lock acquisition occurs while the lookup lock is held. Each temporary native
 * material gets its own association; equal canonical assets still have one
 * registered engine identity. Never edit a shared native atlas row. */
static int vmr_lookup(void *owner, void *material, void **out)
{
    const char *name = *(const char **)((unsigned char *)material + 8);
    char key[4096], atlas[4096]; vmr_entry *entry;
    uintptr_t provider; int32_t extent; void *existing;
    vmr_row *available = NULL; int result = -1;
    if (!vmr_native_name(name, key)) return 0; /* Cannot name a compiler-owned binding. */
    entry = vmr_find(g_vmr_active, key);
    if (!entry) return 0;
    if (!entry->has_record) { *out = NULL; return 1; }
    provider = owner ? *(uintptr_t *)((unsigned char *)owner + 0x130) : 0;
    if (!provider) { vmr_fault(entry->name, "native atlas is unavailable"); return -1; }
    if (!vmr_native_name(*(const char **)(provider + 0x1a0), atlas) || strcmp(atlas, entry->atlas)) {
        vmr_fault(entry->name, "native page provider differs from the compiled binding"); goto done;
    }
    extent = *(int32_t *)(provider + 0x2fc);
    existing = *(void **)((unsigned char *)material + 0x170);
    /* Unchanged objects may still borrow the prior provider's row. Retain that
     * owner through rollback/cleanup rather than moving its live association. */
    for (sh_virtual_material_view *v = g_vmr_views; v; v = v->next) {
        vmr_entry *peer = vmr_find(v, entry->name);
        if (!peer || !vmr_equal(entry, peer)) continue;
        for (vmr_row *row = peer->rows; row; row = row->next) {
            uintptr_t bound = sh_virtual_material_native_material(row->native);
            if (existing ? existing == sh_virtual_material_native_row(row->native) : bound == (uintptr_t)material) {
                if (bound != (uintptr_t)material ||
                    !sh_virtual_material_native_atlas(row->native, (uintptr_t)owner, extent)) {
                    vmr_fault(entry->name, "cached atlas association requires native reconstruction"); goto done;
                }
                *out = sh_virtual_material_native_row(row->native); result = 1; goto done;
            }
            if (!bound && peer == entry) available = row;
        }
    }
    if (existing) { vmr_fault(entry->name, "material has an unrelated cached atlas row"); goto done; }
    if (available) {
        /* This row has no native material consumer. Parse will publish its new
         * association and apply the VMTR setter itself before returning. */
        if (!sh_virtual_material_native_refresh(available->native, (uintptr_t)owner, extent)) {
            vmr_fault(entry->name, "material rectangle does not fit the native atlas"); goto done;
        }
    } else {
        available = calloc(1, sizeof(*available));
        if (!available) { vmr_fault(entry->name, "cannot retain native atlas association"); goto done; }
        available->native = sh_virtual_material_native_create(&entry->record, entry->name,
            (uintptr_t)owner, extent, entry->dlc, NULL, 0);
        if (!available->native) {
            free(available); vmr_fault(entry->name, "cannot create native atlas association"); goto done;
        }
        available->next = entry->rows; entry->rows = available;
    }
    *out = sh_virtual_material_native_attach(available->native, (uintptr_t)material); result = *out ? 1 : -1;
done:
    return result;
}
static void *vmr_lookup_hook(void *owner, void *material)
{
    void *row = NULL; int claimed = 0;
    if (material && InterlockedCompareExchange(&g_vmr_ready, 0, 0)) {
        AcquireSRWLockExclusive(&g_vmr_lock);
        __try {
            if (g_vmr_active && g_vmr_active->count) claimed = vmr_lookup(owner, material, &row);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            vmr_fault(NULL, "native atlas lookup raised an exception"); claimed = -1;
        }
        ReleaseSRWLockExclusive(&g_vmr_lock);
    }
    return claimed ? row : g_vmr_lookup(owner, material);
}
int sh_virtual_material_runtime_install(const sig_result *results, size_t count)
{
    void *target = NULL;
    if (InterlockedCompareExchange(&g_vmr_attempted, 1, 0)) return sh_virtual_material_runtime_ready();
    for (size_t i = 0; results && i < count; i++)
        if (results[i].name && !strcmp(results[i].name, "VirtualMaterialLookup")) {
            if (target || results[i].status != SIG_OK || !results[i].addr) goto failed;
            target = (void *)results[i].addr;
        }
    if (!target) goto failed;
    /* Six complete position-independent instructions, ending before the first
     * relative branch. Identical prologue verified in both retail renderers. */
    g_vmr_lookup = (vmr_lookup_fn)hook_prepare(target, vmr_lookup_hook, 16);
    if (!g_vmr_lookup || hook_commit((void *)g_vmr_lookup) != B2_PATCH_OK) goto failed;
    sh_vt_provider_on_initialize(sh_virtual_material_runtime_providers);
    InterlockedExchange(&g_vmr_ready, 1); return 1;
failed:
    backend_log("package virtual material lookup unavailable"); return 0;
}
int sh_virtual_material_runtime_ready(void)
{ return InterlockedCompareExchange(&g_vmr_ready, 0, 0) != 0; }
