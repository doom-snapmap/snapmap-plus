#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "resource_resident.h"
#include "discrete_default.h"
#include "package_shader_receipt.h"
#include "render_scope.h"
#include "resource_graph.h"
#include "resource_catalog.h"
#include "package_runtime.h"
#include "engine_globals.h"
#include "process_heap_scope.h"
#include "backend_log.h"
#include "typeinfo.h"
#include "virtual_material_runtime.h"
#include "size_owners.h"

#define RR_LIST_LIMIT 4096u
#define RR_ENTRY_LIMIT 262144u
#define RR_PENDING 2u
#define RR_DEFAULT 4u
/* idDecl::Read tests this byte and returns success without reading a source;
 * the dependency observer uses the same flag (resource_graph_native.c). */
#define RR_IMPLICIT_TEXT_OFFSET 0x48u
/* idResource lifetime tier; the map-transition purge tests only this field. */
#define RR_LEVEL_OFFSET 0x28u
#define RR_LEVEL_PERMANENT 4u
typedef struct rr_entry {
    void *resource, *manager, *parent;
    char *type, *name;
    int selected, external, retired, reconstructed;
    /* Left unmutated by this pass: a native holder reads it through a field the
     * product could not attribute and cannot rebuild (rr_instance_admit). */
    int isolated;
    /* A removed identity left as last parsed (rr_keeps_parsed_content). */
    int kept;
    /* An added change names this identity, by path or manager identity. */
    int added;
    /* A retired shell an addition brings back: a defaulted object this provider
     * retired, with no native record, which only this pass's change names. */
    int revived;
    /* State as captured before this pass touched anything. A resource the
     * engine already served as a default, or that already retained a source
     * record, must be allowed to come back the same way. */
    unsigned char captured_state;
    int captured_source;
    /* The engine's own implicit-text flag: this declaration carries no source
     * to re-read, so reconstruction could only replace it with a default. */
    int implicit;
    /* Lifetime tier as found: 1 map, 2 full teardown, 4 permanent. */
    unsigned int captured_level;
    /* Why the pass selected it (static text), for ownership diagnostics. */
    const char *cause;
    /* Selected only as a recorded consumer of a changed identity. */
    int consumer;
    /* A shell this provider retired (rr_retired_shell) when the pass began. A restoration keeps the value
     * the failed activation captured for an identity it retained: that pass may have retired it since. */
    int shell_before;
    /* A forward pass retires such a shell again as a recorded consumer of a change: its default is
     * rebuilt with the new dependencies. An additive admission may own it, as it owns a revived shell. */
    int refreshed;
    /* Owned by an additive admission (sh_resource_resident_release_revived): reconstruction waits for
     * its default (sh_resource_resident_defaults), after the addition's declarations exist, so the old
     * default stays intact until then and no lookup meets fresh storage without one. */
    int deferred;
} rr_entry;
/* Every identity that already existed when the pass began, with the lifetime
 * tier it had. Touch consults this to tell a resource the pass just created
 * from one the map owns. */
typedef struct rr_known { void *resource; unsigned int level; size_t entry; } rr_known;
struct sh_resource_resident {
    rr_entry *items;
    size_t count, capacity, default_faults, recovered_defaults;
    rr_known *known;
    size_t known_count;
    /* Additive passes: recorded consumers of the changes when the pass began. */
    sh_resource_graph_impact prior;
    sh_process_heap_scope heap;
    sh_render_scope render;
    int rebuilt, defaults_ready, drained, mutated, touch_failed, restoring;
    DWORD thread;
    /* Images this pass rebuilds in place: their copied sizes are refreshed after the drain (R-321). */
    char **images;
    size_t image_count;
    /* The first retirement default that raised an engine error or could not be built: the pass fails with it. */
    char default_failure[320];
    void *transfer;
    sh_resource_instance_transfer_prepare transfer_prepare;
    sh_resource_instance_transfer_finish transfer_finish;
    sh_resource_instance_transfer_dispose transfer_dispose;
};
static struct {
    sh_process_heap_api heap;
    void **head, **renderer;
    DWORD *thread;
    void (*reconstruct)(void *), (*load)(void *), (*string_free)(void *);
    void *(*lookup)(void *, const char *);
    int (*mode)(void);
    void (*rebind)(void);
    void (*rebind_material)(void *);
    void (*world_text_fonts)(void *);
    /* Engine default construction (discrete_default.c): a scope on the calling execution
     * stack around this pass's own defaults and loads, and whether a family's default can be
     * built in this process. */
    int (*default_enter)(sh_discrete_scope *scope);
    int (*default_leave)(sh_discrete_scope *scope);
    int (*default_ready)(const char *family);
    /* Whether an object's default made every read where the graph records it (R-352). */
    int (*default_observed)(void *resource);
    sh_resource_default_probe_prepare default_prepare;
    sh_resource_default_probe_finish default_finish;
    sh_resource_default_invoke default_invoke;
    int (*origin_begin)(sh_resource_default_origin_scope *,void *);
    int (*origin_finish)(sh_resource_default_origin_scope *,void *,int,int,int);
    int (*instance_refresh)(void *const *,size_t,char *,size_t);
    int (*instance_admit)(void *const *,size_t,char *,size_t);
    size_t (*instance_report)(const sh_resource_admit_report **items);
    sh_resource_instance_transfer_prepare transfer_prepare;
    sh_resource_instance_transfer_finish transfer_finish;
    sh_resource_instance_transfer_dispose transfer_dispose;
} g_rr;
static sh_resource_resident *g_rr_recovery;
/* Failed native scope restoration must retain its owner, even if resource
 * reconstruction never began. Do not let a new pass inherit a stranded hold. */
static sh_resource_resident *g_rr_scope_recovery;
static __declspec(thread) sh_resource_resident *g_rr_active;
static int rr_transfer_prepare(sh_resource_resident *pass,char *error,size_t capacity);
static int rr_transfer_finish(sh_resource_resident *pass,char *error,size_t capacity);
/* Identities this provider retired. Reconstruction gave each fresh storage and
 * the defaulted state, and no later pass has supplied it again. Only these may
 * be revived by an addition: an engine default this provider never retired --
 * a missing declaration a consumer looked up, or a parse that failed -- is an
 * existing object. Declarations are never freed and stay permanent, so the
 * address with its identity names one object for the life of the process. */
typedef struct rr_retirement { void *resource; char *type, *name; } rr_retirement;
static rr_retirement *g_rr_retirements;
static size_t g_rr_retirement_count, g_rr_retirement_capacity;

static int rr_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return 0;
}
/* Same failure, with the identity the engine raised on. */
static int rr_error_at(char *error, size_t capacity, const char *message,
    const char *type, const char *name)
{
    if (error && capacity) snprintf(error, capacity, "%s for %s:%s", message,
        type ? type : "?", name ? name : "?");
    return 0;
}
static char rr_fold(char c)
{
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return c == '\\' ? '/' : c;
}
static char *rr_copy(const char *text)
{
    size_t n;
    char *out;
    if (!text || !(n = strnlen(text, 4096)) || n == 4096) return NULL;
    out = (char *)malloc(n + 1);
    if (out) for (size_t i = 0; i <= n; i++) out[i] = rr_fold(text[i]);
    return out;
}
static int rr_compare(const void *a, const void *b)
{
    const sh_resource_graph_identity *x = a, *y = b;
    int cmp = strcmp(x->type, y->type);
    return cmp ? cmp : strcmp(x->name, y->name);
}
static int rr_seed(sh_resource_graph_impact *set, const char *type, const char *name)
{
    sh_resource_graph_identity *next;
    if (set->count >= SIZE_MAX / sizeof(*next)) return 0;
    next = realloc(set->items, (set->count + 1) * sizeof(*next));
    if (!next) return 0;
    set->items = next; next += set->count++;
    next->type = *type ? rr_copy(type) : _strdup(""); next->name = rr_copy(name);
    return next->type && next->name;
}
/* Renderer parameters and program flags are engine-fixed objects: they hold
 * their dependencies (a default image, say) as live objects that refresh in
 * place, and each owns a native slot that compiled programs address.
 * Rebuilding one in place disturbs programs already bound to it: after a boot
 * rebuilt transmap, the SDF font program sampled its default image. Unless the
 * provider supplies the object itself, it is neither rebuilt nor a path to its
 * consumers. */
static int rr_fixed_renderer(void *context, const char *type, const char *name)
{
    (void)context; (void)name;
    return !_stricmp(type, "renderparm") || !_stricmp(type, "renderprogflag");
}
/* A removed private copy of these families keeps its last parsed content
 * instead of the engine default: the engine never drops a declaration, and a
 * native consumer walks every instance and uses its parsed fields. Play's
 * precache resolves each blocking volume render model inspector's
 * hotspotModel and triplanarModel by name and dereferences the result (VK
 * FUN_1411caba0), which a default instance's empty names fault. Nothing
 * current names a removed copy, and its names change with its content. */
static int rr_keeps_parsed_content(const char *type)
{ return type && !_stricmp(type, "snappropertyinspector_blockingvolumerendermodel"); }
static int rr_has(const sh_resource_graph_impact *set, const rr_entry *entry)
{
    sh_resource_graph_identity key = {entry->type, entry->name};
    return set->count && bsearch(&key, set->items, set->count, sizeof(key), rr_compare) != NULL;
}
static void rr_free(sh_resource_resident *pass)
{
    if (!pass) return;
    if (pass->transfer && pass->transfer_dispose) pass->transfer_dispose(pass->transfer);
    for (size_t i = 0; i < pass->count; i++) {
        free(pass->items[i].type); free(pass->items[i].name);
    }
    for (size_t i = 0; i < pass->image_count; i++) free(pass->images[i]);
    free(pass->images);
    free(pass->items); free(pass->known); sh_resource_graph_impact_free(&pass->prior); free(pass);
}
static int rr_known_compare(const void *key, const void *row)
{
    void *const *a = key;
    const rr_known *b = row;
    if ((uintptr_t)*a < (uintptr_t)b->resource) return -1;
    return (uintptr_t)*a > (uintptr_t)b->resource ? 1 : 0;
}
static int rr_known_order(const void *a, const void *b)
{
    const rr_known *x = a, *y = b;
    if ((uintptr_t)x->resource < (uintptr_t)y->resource) return -1;
    return (uintptr_t)x->resource > (uintptr_t)y->resource ? 1 : 0;
}
/* Pointer/level snapshot of the whole registry. Cheaper than the inventory:
 * no identity strings, no native path calls. */
static int rr_known_snapshot(sh_resource_resident *pass)
{
    void *list = *g_rr.head;
    unsigned lists = 0;
    size_t capacity = 0;
    while (list && lists++ < RR_LIST_LIMIT) {
        unsigned char *manager = list;
        void **items = *(void ***)(manager + 0x20);
        int count = *(int *)(manager + 0x28);
        if (count < 0 || (unsigned)count >= RR_ENTRY_LIMIT || (count && !items)) return 0;
        for (int i = 0; i < count; i++) if (items[i]) {
            if (pass->known_count == capacity) {
                size_t next_capacity = capacity ? capacity * 2 : 4096;
                rr_known *next = next_capacity > capacity && next_capacity <= SIZE_MAX / sizeof(*next) ?
                    realloc(pass->known, next_capacity * sizeof(*next)) : NULL;
                if (!next) return 0;
                pass->known = next; capacity = next_capacity;
            }
            pass->known[pass->known_count].resource = items[i];
            pass->known[pass->known_count].entry = SIZE_MAX;
            pass->known[pass->known_count].level =
                *(unsigned int *)((unsigned char *)items[i] + RR_LEVEL_OFFSET);
            pass->known_count++;
        }
        list = *(void **)(manager + 0x18);
    }
    if (list) return 0;
    qsort(pass->known, pass->known_count, sizeof(*pass->known), rr_known_order);
    return 1;
}
static const uint8_t *rr_site(const sig_result *results, size_t count, const uint8_t *base, const char *name)
{
    const uint8_t *out = NULL;
    for (size_t i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, name)) {
        if (out || results[i].status != SIG_OK || !results[i].addr ||
            results[i].addr != (uintptr_t)base + results[i].rva) return NULL;
        out = (const uint8_t *)results[i].addr;
    }
    return out;
}
static void *rr_relative(const uint8_t *instruction, size_t offset, size_t length)
{
    int32_t displacement;
    memcpy(&displacement, instruction + offset, 4);
    return (void *)(instruction + length + displacement);
}
int sh_resource_resident_bind(const sig_result *results, size_t count, const uint8_t *base)
{
    const uint8_t *command, *complete, *mode, *reconstruct, *load, *lookup, *rebind, *fonts;
    sh_process_heap_api heap;
    void **renderer;
    void *materials;
    if (!results || !base || !sh_process_heap_bind(&heap, results, count, base)) return 0;
    command = rr_site(results, count, base, "ResourceReloadRenderScope");
    complete = rr_site(results, count, base, "PublishedMapComplete");
    mode = rr_site(results, count, base, "DeclSourceModeCall");
    reconstruct = rr_site(results, count, base, "ResourceReconstruct");
    load = rr_site(results, count, base, "ResourceGenericLoad");
    lookup = rr_site(results, count, base, "ResourceLookup");
    rebind = rr_site(results, count, base, "MaterialVirtualTextureRebind");
    fonts = rr_site(results, count, base, "SnapWorldTextFonts");
    materials = (void *)glb_resolve(base, "material_manager_ctx", NULL);
    if (!command || !complete || !mode || !reconstruct || !load || !lookup || !rebind || !fonts || !materials) return 0;
    __try {
        /* The rebind walks the material manager itself; its only list load is
         * lea rcx at +0x30. Any other manager means a different function. */
        if (memcmp(rebind + 0x30, "\x48\x8d\x0d", 3) || rr_relative(rebind + 0x30, 3, 7) != materials) return 0;
        /* Both verified walks pass one material at +0x59. Keep its helper
         * separate so active-editor additions never rebind existing objects. */
        if (memcmp(rebind + 0x50, "\x48\x8b\x4c\x24\x28\x48\x8b\x0c\x0f\xe8", 10)) return 0;
        g_rr.rebind_material = (void (*)(void *))rr_relative(rebind + 0x59, 1, 5);
        renderer = sh_render_scope_renderer(command);
        if (!renderer) return 0;
        if (*mode != 0xe8 || complete[0xb9] != 0xe8) return 0;
        g_rr.string_free = (void (*)(void *))rr_relative(complete + 0xb9, 1, 5);
        g_rr.mode = (int (*)(void))rr_relative(mode, 1, 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_rr.head = (void **)glb_resolve(base, "decl_resource_head", NULL);
    g_rr.thread = (DWORD *)glb_resolve(base, "main_thread_id", NULL);
    if (!g_rr.head || !g_rr.thread) return 0;
    g_rr.heap = heap; g_rr.renderer = renderer;
    g_rr.reconstruct = (void (*)(void *))reconstruct;
    g_rr.load = (void (*)(void *))load;
    g_rr.lookup = (void *(*)(void *, const char *))lookup;
    g_rr.rebind = (void (*)(void))rebind;
    g_rr.world_text_fonts = (void (*)(void *))fonts;
    return 1;
}
static int rr_inventory(sh_resource_resident *pass)
{
    void *list = *g_rr.head;
    unsigned lists = 0;
    while (list && lists++ < RR_LIST_LIMIT) {
        unsigned char *manager = list;
        void **items = *(void ***)(manager + 0x20);
        const char *type = *(const char **)(manager + 8);
        const char *class_name = *(const char **)(manager + 0x10);
        int inherits = class_name ? sh_typeinfo_class_derives(class_name, "idDeclTypeInfo") : 0;
        int count = *(int *)(manager + 0x28);
        if (inherits < 0) return 0;
        if (count < 0 || count >= RR_ENTRY_LIMIT || (count && !items)) return 0;
        for (int i = 0; i < count; i++) if (items[i]) {
            rr_entry *entry;
            if (pass->count >= RR_ENTRY_LIMIT) return 0;
            if (pass->count == pass->capacity) {
                size_t capacity = pass->capacity ? pass->capacity * 2 : 1024;
                rr_entry *next = realloc(pass->items, capacity * sizeof(*next));
                if (!next) return 0;
                pass->items = next; pass->capacity = capacity;
            }
            entry = pass->items + pass->count++; memset(entry, 0, sizeof(*entry));
            entry->resource = items[i]; entry->manager = list;
            {
                rr_known *known = bsearch(&entry->resource, pass->known, pass->known_count,
                    sizeof(*pass->known), rr_known_compare);
                if (!known) return 0;
                known->entry = pass->count - 1;
            }
            /* Reflection proves this prefix before reading the native parent.
             * Other resource families use +0x58 for unrelated state. */
            if (inherits) entry->parent = *(void **)((unsigned char *)items[i] + 0x58);
            if (*(void **)((unsigned char *)items[i] + 0x10) != list) return 0;
            entry->type = rr_copy(type);
            entry->name = rr_copy(*(const char **)((unsigned char *)items[i] + 8));
            if (!entry->type || !entry->name) return 0;
            entry->captured_state = *((unsigned char *)items[i] + 0x2c);
            entry->captured_source = *(void **)((unsigned char *)items[i] + 0x18) != NULL;
            entry->implicit = *((unsigned char *)items[i] + RR_IMPLICIT_TEXT_OFFSET) != 0;
            entry->captured_level = *(unsigned int *)((unsigned char *)items[i] + RR_LEVEL_OFFSET);
        }
        list = *(void **)(manager + 0x18);
    }
    return list == NULL;
}
static size_t rr_retirement_find(const void *resource)
{
    for (size_t i = 0; i < g_rr_retirement_count; i++) if (g_rr_retirements[i].resource == resource) return i;
    return SIZE_MAX;
}
/* Record the decision reconstruction just applied. A supplied identity leaves
 * the ledger; a retired one enters it. An allocation failure leaves it out,
 * which only refuses a later revival. */
static void rr_retirement_note(const rr_entry *entry)
{
    size_t i = rr_retirement_find(entry->resource);
    rr_retirement *row;
    if (i != SIZE_MAX) {
        free(g_rr_retirements[i].type); free(g_rr_retirements[i].name);
        g_rr_retirements[i] = g_rr_retirements[--g_rr_retirement_count];
    }
    if (!entry->retired) return;
    if (g_rr_retirement_count == g_rr_retirement_capacity) {
        size_t capacity = g_rr_retirement_capacity ? 2 * g_rr_retirement_capacity : 64;
        rr_retirement *next = realloc(g_rr_retirements, capacity * sizeof(*next));
        if (!next) return;
        g_rr_retirements = next; g_rr_retirement_capacity = capacity;
    }
    row = g_rr_retirements + g_rr_retirement_count;
    row->resource = entry->resource; row->type = rr_copy(entry->type); row->name = rr_copy(entry->name);
    if (row->type && row->name) g_rr_retirement_count++;
    else { free(row->type); free(row->name); }
}
static int rr_retired_by_provider(const rr_entry *entry)
{
    size_t i = rr_retirement_find(entry->resource);
    return i != SIZE_MAX && !strcmp(g_rr_retirements[i].type, entry->type) &&
        !strcmp(g_rr_retirements[i].name, entry->name);
}
/* A shell this provider retired, as found: still the engine default with no
 * native record, and no later pass has supplied it. */
static int rr_retired_shell(const rr_entry *entry)
{
    return !entry->implicit && (entry->captured_state & RR_DEFAULT) && !(entry->captured_state & RR_PENDING) &&
        !entry->captured_source && rr_retired_by_provider(entry);
}
/* The failed activation's entry for an object it retained: the same address and identity. */
static const rr_entry *rr_recovery_entry(const rr_entry *entry)
{
    for (size_t i = 0; g_rr_recovery && i < g_rr_recovery->count; i++) {
        const rr_entry *item = g_rr_recovery->items + i;
        if (item->resource == entry->resource && !strcmp(item->type, entry->type) && !strcmp(item->name, entry->name))
            return item;
    }
    return NULL;
}
static int rr_change_compare(const void *key, const void *row)
{
    return strcmp((const char *)key, ((const sh_package_change *)row)->path);
}
static int rr_change_identity(const sh_package_change *change, const rr_entry *entry)
{
    return (change->type && change->name && !strcmp(change->type, entry->type) && !strcmp(change->name, entry->name)) ||
        (change->previous_type && change->previous_name && !strcmp(change->previous_type, entry->type) &&
            !strcmp(change->previous_name, entry->name));
}
static int rr_path(rr_entry *entry, const sh_package_changes *changes, int mode,
    const sh_resource_catalog *catalog)
{
    unsigned char string[0x30];
    int initialized = 0, result = 0;
    __try {
        void **table = *(void ***)entry->resource;
        const char *data;
        char *key = NULL;
        int length;
        const sh_package_change *change;
        ((void *(*)(void *, void *, int))table[0x38/8])(entry->resource, string, mode);
        initialized = 1;
        length = *(int *)(string + 8); data = *(const char **)(string + 0x10);
        if (length < 0 || length >= 4096 || !data || data[length] || memchr(data, 0, length)) goto done;
        if (!length) { result = 1; goto done; }
        key = rr_copy(data);
        if (!key) goto done;
        change = bsearch(key, changes->items, changes->count, sizeof(*changes->items), rr_change_compare);
        if (change && change->kind == SH_PACKAGE_RESOURCE_ADDED) entry->added = 1;
        if (change) {
            const sh_resource_catalog_entry *const *rows = NULL;
            size_t count = sh_resource_catalog_find_path(catalog, key, &rows);
            int stock = 0;
            for (size_t i = 0; i < count; i++) if (rows[i]->archive < 2) stock = 1;
            entry->selected = 1;
            if (change->kind == SH_PACKAGE_RESOURCE_REMOVED && !stock &&
                !(changes->identities_complete && (rr_change_identity(change, entry) ||
                    sh_package_runtime_serves_native_identity(entry->type, entry->name)))) entry->retired = 1;
        }
        free(key); result = 1;
done:;
    } __finally { if (initialized) g_rr.string_free(string); }
    return result;
}
sh_resource_resident *sh_resource_resident_begin(const sh_package_changes *changes,
    int restoring, int initial, char *error, size_t capacity)
{
    sh_resource_resident *pass = calloc(1, sizeof(*pass));
    sh_resource_graph_impact seeds = {0}, impact = {0}, retired = {0}, additions = {0}, retained = {0};
    /* R-309 in-place image recovery: images it recovers, images it returns to the native decision. */
    sh_resource_graph_impact recovering = {0}, returning = {0}, recovery_impact = {0};
    /* Fonts that copied the size of an image this pass rebuilds: rebuilt with it (R-321). */
    sh_resource_graph_impact owners = {0};
    const sh_resource_catalog *catalog = sh_package_runtime_catalog();
    const char *phase = "arguments";
    const rr_entry *current = NULL;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (!pass) { rr_error(error, capacity, "resident refresh allocation failed"); return NULL; }
    if (g_rr_scope_recovery) {
        sh_resource_resident_end(g_rr_scope_recovery, 0, error, capacity);
        if (g_rr_scope_recovery) {
            rr_free(pass); rr_error(error, capacity, "previous native refresh scopes remain held"); return NULL;
        }
    }
    __try {
        if (g_rr_active || !changes || (changes->count && !changes->items) ||
            (changes->removed_count && !changes->removed) || (changes->added_count && !changes->added) ||
            (changes->rebound_count && !changes->rebound) ||
            (changes->availability_count && !changes->availability)) goto done;
        for (size_t i = 0; i < changes->count; i++)
            if (!changes->items[i].path || !*changes->items[i].path ||
                (i && strcmp(changes->items[i-1].path, changes->items[i].path) >= 0)) goto done;
        pass->thread = GetCurrentThreadId();
    pass->restoring = restoring != 0;
        if (!g_rr.thread || *g_rr.thread != pass->thread || !g_rr.mode ||
            (g_rr.mode() != 2 && g_rr.mode() != 3) || !sh_process_heap_enter(&g_rr.heap, &pass->heap)) goto done;
        phase = "registry snapshot";
        if (!rr_known_snapshot(pass)) goto done;
        if (!changes->count && !changes->added_count && !changes->removed_count &&
            !changes->rebound_count && !changes->availability_count && !g_rr_recovery) goto render_scope;
        phase = "inventory";
        if (!rr_inventory(pass)) goto done;
        phase = "catalog identities";
        for (size_t i = 0; i < changes->count; i++) {
            const sh_resource_catalog_entry *const *rows = NULL;
            size_t count = sh_resource_catalog_find_path(catalog, changes->items[i].path, &rows);
            if (!rr_seed(&seeds, "", changes->items[i].path)) goto done;
            for (size_t j = 0; j < count; j++) if (!rr_seed(&seeds, rows[j]->type, rows[j]->name)) goto done;
            /* Compiler aliases have no archive row. Their manager identity
             * selects native objects even when GetPath names another cooked
             * representation. Replacements retain both sides of the binding. */
            const sh_package_change *change = &changes->items[i];
            if (change->type && change->name) {
                if (!rr_seed(&seeds, change->type, change->name)) goto done;
                if (change->kind == SH_PACKAGE_RESOURCE_ADDED && !rr_seed(&additions, change->type, change->name)) goto done;
                int stock = 0;
                for (size_t j = 0; j < count; j++) if (rows[j]->archive < 2) stock = 1;
                if (!changes->identities_complete && change->kind == SH_PACKAGE_RESOURCE_REMOVED && !stock &&
                    !rr_seed(&retired, change->type, change->name)) goto done;
            }
            if (change->previous_type && change->previous_name &&
                !rr_seed(&seeds, change->previous_type, change->previous_name)) goto done;
        }
        /* Retirement uses the complete provider difference, not individual
         * paths. One removed permutation can still leave its object supplied
         * by other files. Stock identities retain their installed fallback. */
        for (size_t i = 0; i < changes->removed_count; i++) {
            const sh_package_native_identity *identity = changes->removed + i;
            const sh_resource_catalog_entry *const *rows = NULL;
            size_t count;
            int stock = 0;
            if (!identity->type || !identity->name || !rr_seed(&seeds, identity->type, identity->name)) goto done;
            count = sh_resource_catalog_find(catalog, identity->type, identity->name, &rows);
            for (size_t j = 0; j < count; j++) if (rows[j]->archive < 2) stock = 1;
            if (!stock && !rr_seed(&retired, identity->type, identity->name)) goto done;
        }
        for (size_t i = 0; i < changes->added_count; i++) {
            const sh_package_native_identity *identity = changes->added + i;
            if (!identity->type || !identity->name || !rr_seed(&seeds, identity->type, identity->name) ||
                !rr_seed(&additions, identity->type, identity->name)) goto done;
        }
        for (size_t i = 0; i < changes->rebound_count; i++) {
            const sh_package_native_identity *identity = changes->rebound + i;
            if (!identity->type || !*identity->type || !identity->name || !*identity->name ||
                !rr_seed(&seeds, identity->type, identity->name)) goto done;
        }
        /* The bytes do not change; the declared gate does. Each object is rebuilt in place and
         * reloads under the gate the requirement owner applies before reconstruction. */
        for (size_t i = 0; i < changes->availability_count; i++) {
            const sh_package_availability_change *change = changes->availability + i;
            if (!change->type || !*change->type || !change->name || !*change->name ||
                !rr_seed(&seeds, change->type, change->name) ||
                !rr_seed(change->recover ? &recovering : &returning, change->type, change->name)) goto done;
        }
        if (g_rr_recovery) {
            if (!restoring) goto done;
            for (size_t i = 0; i < g_rr_recovery->count; i++)
                if (!rr_seed(&seeds, g_rr_recovery->items[i].type, g_rr_recovery->items[i].name) ||
                    !rr_seed(&retained, g_rr_recovery->items[i].type, g_rr_recovery->items[i].name)) goto done;
        }
        phase = "native paths";
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            current = entry;
            if (changes->count && (!rr_path(entry, changes, 0, catalog) || !rr_path(entry, changes, 1, catalog))) goto done;
            if (entry->selected && !rr_seed(&seeds, entry->type, entry->name)) goto done;
        }
        current = NULL; phase = "copied image sizes";
        /* Copied sizes (R-309, R-321): an in-place rebuild keeps every holder's pointer, not a size a native
         * owner copied through the material size getters. Each image this pass rebuilds needs a recorded
         * history; its fonts are rebuilt with it, and an owner whose copy has no native refresh makes the
         * rebuild wait for that owner. Derived sprite sizes, codex layouts and hotBox geometry refresh after
         * the drain. */
        qsort(seeds.items, seeds.count, sizeof(*seeds.items), rr_compare);
        /* An image the history cannot clear (registered before the history began, or a size read for a
         * caller with no known copy owner) is isolated rather than refusing the pass: it keeps its
         * registered object, sizes and content, so no copied size can go stale, and the identity's new
         * bytes wait for the engine's own reload. Only a process with no size-read history at all still
         * refuses, since then no image rebuild can be cleared. */
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            if (_stricmp(entry->type, "image") || !(entry->selected || rr_has(&seeds, entry))) continue;
            if (!sh_resource_graph_size_history_ready()) {
                if (error && capacity) snprintf(error, capacity, "in-place image rebuild cannot show "
                    "image:%s free of copied sizes: no size-read history is recorded in this process", entry->name);
                phase = NULL; goto done;
            }
            unsigned history = sh_resource_graph_history("image", entry->name);
            const char *why = (history & SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE) ?
                "it was registered before the size-read history began" :
                (history & SH_RESOURCE_GRAPH_HISTORY_ESCAPED) ?
                "a material size getter read its size for a caller with no known copy owner" : NULL;
            if (why) {
                char line[640];
                entry->isolated = 1; pass->mutated = 1;
                snprintf(line, sizeof(line), "native resident isolates image:%s: an in-place rebuild cannot show it free "
                    "of copied sizes (%s); the image keeps its registered object, sizes and content, and its new bytes "
                    "wait for the engine's own reload", entry->name, why);
                backend_log(line);
                continue;
            }
            char **grown = realloc(pass->images, (pass->image_count + 1) * sizeof(*grown));
            if (!grown) goto done;
            pass->images = grown;
            if (!(pass->images[pass->image_count] = _strdup(entry->name))) goto done;
            pass->image_count++;
        }
        {
            char **fonts = NULL, blocker[512];
            size_t font_count = 0;
            int planned = sh_size_owners_plan((const char *const *)pass->images, pass->image_count, &fonts,
                &font_count, blocker, sizeof(blocker));
            for (size_t i = 0; i < font_count; i++) {
                if (planned && !rr_seed(&owners, "font", fonts[i])) planned = -1;
                free(fonts[i]);
            }
            free(fonts);
            if (planned < 0) goto done;
            if (!planned) {
                if (error && capacity) snprintf(error, capacity, "in-place image rebuild waits: %s", blocker);
                phase = NULL; goto done;
            }
            qsort(owners.items, owners.count, sizeof(*owners.items), rr_compare);
        }
        current = NULL; phase = "recorded consumers";
        if (sh_resource_graph_consumers_bounded(seeds.items, seeds.count, rr_fixed_renderer, NULL,
                &impact) == SH_RESOURCE_GRAPH_ERROR) goto done;
        qsort(recovering.items, recovering.count, sizeof(*recovering.items), rr_compare);
        qsort(returning.items, returning.count, sizeof(*returning.items), rr_compare);
        if (recovering.count || returning.count) {
            /* An in-place image rebuild refreshes holders that read the image when they use it
             * (fixed renderer objects and their parm-state copies of the pointer), not state a
             * consumer copied at its own load, such as a font's cached atlas size. Every recorded
             * consumer must therefore be rebuilt by this pass, which selects permanent ones; a live
             * consumer it cannot rebuild, or an image no recorded parse ever named, refuses. */
            phase = "in-place image recovery consumers";
            /* Only registered images are rebuilt; an absent one loads fresh under the gate when used. */
            sh_resource_graph_identity *live = NULL;
            size_t live_count = 0;
            if (!(live = calloc(recovering.count + returning.count, sizeof(*live)))) goto done;
            for (size_t i = 0; i < pass->count; i++) {
                rr_entry *entry = pass->items + i;
                if ((rr_has(&recovering, entry) || rr_has(&returning, entry)) &&
                    live_count < recovering.count + returning.count)
                    live[live_count++] = (sh_resource_graph_identity){entry->type, entry->name};
            }
            /* Coverage is judged on the images' own recorded consumer closure, not the whole graph:
             * each must have a node, and every consumer the walk visits, including fixed renderer
             * holders it stops at, must be completely observed. A recorded edge alone shows only
             * that one consumer exists. */
            char unobserved[640] = "";
            sh_resource_graph_status covered = sh_resource_graph_consumers_observed(live, live_count,
                rr_fixed_renderer, NULL, &recovery_impact, unobserved, sizeof(unobserved));
            free(live);
            if (covered == SH_RESOURCE_GRAPH_ERROR) goto done;
            if (covered != SH_RESOURCE_GRAPH_COMPLETE) {
                if (error && capacity) snprintf(error, capacity, "in-place image recovery cannot establish its "
                    "consumers: %s is not completely observed", *unobserved ? unobserved : "a seed");
                phase = NULL; goto done;
            }
            qsort(recovery_impact.items, recovery_impact.count, sizeof(*recovery_impact.items), rr_compare);
            for (size_t i = 0; i < pass->count; i++) {
                rr_entry *entry = pass->items + i;
                int seed = rr_has(&recovering, entry) || rr_has(&returning, entry);
                if (!seed && rr_has(&recovery_impact, entry) &&
                    (entry->implicit || entry->captured_level != RR_LEVEL_PERMANENT)) {
                    current = entry;
                    if (error && capacity) snprintf(error, capacity, "in-place image recovery has a live consumer "
                        "this pass cannot rebuild: %s:%s (level %u%s)", entry->type, entry->name,
                        entry->captured_level, entry->implicit ? ", implicit text" : "");
                    phase = NULL; goto done;
                }
            }
        }
        qsort(seeds.items, seeds.count, sizeof(*seeds.items), rr_compare);
        qsort(retired.items, retired.count, sizeof(*retired.items), rr_compare);
        qsort(additions.items, additions.count, sizeof(*additions.items), rr_compare);
        qsort(retained.items, retained.count, sizeof(*retained.items), rr_compare);
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            /* Archive aliases identify source declarations whose virtual path
             * names their cooked representation. They are direct changes too,
             * even before startup promotes them to permanent lifetime. */
            int supplied = entry->selected || rr_has(&seeds, entry);
            int owner = rr_has(&owners, entry);
            const rr_entry *before = rr_has(&retained, entry) ? rr_recovery_entry(entry) : NULL;
            entry->shell_before = before ? before->shell_before : rr_retired_shell(entry);
            entry->cause = rr_has(&recovering, entry) ? "a declared image requirement recovers it in place" :
                rr_has(&returning, entry) ? "its in-place recovery ends; the native image decision applies" :
                entry->selected ? "its native path changes" : rr_has(&retained, entry) ?
                "retained from the failed activation" : supplied ? "the change names it" :
                owner ? "it copied the size of an image this pass rebuilds" :
                rr_has(&impact, entry) ? "a recorded consumer of a changed identity" : NULL;
            entry->consumer = !supplied && (owner || rr_has(&impact, entry));
            if (rr_has(&additions, entry)) entry->added = 1;
            if (rr_has(&retired, entry)) entry->retired = 1;
            /* Rebuilt as a recorded consumer, a shell this provider retired
             * would load a source no provider supplies. It is retired again
             * instead: reconstruction frees what it holds, its default is
             * rebuilt, and it stays in the ledger, so a later addition can
             * still revive it. A provider that supplies it again selects it
             * by path (R-321). */
            if (entry->consumer && !entry->retired && rr_retired_shell(entry)) {
                entry->retired = 1; entry->refreshed = !restoring;
                entry->cause = "a recorded consumer of a changed identity, retired by this provider; "
                    "its default is rebuilt";
            }
            /* The same holds for such a shell a restoration rebuilds only because the failed activation
             * retained it, when it was already one as that activation began and still is: the provider
             * restored now was live then and did not supply it, and no pass has supplied it since. Loaded, it
             * would come back as the engine default with its row gone, and a later addition naming it would
             * be refused as never retired. An identity the failed activation retired for the first time was
             * supplied when it began; it loads from the restored provider and leaves the ledger. */
            else if (restoring && !entry->retired && entry->shell_before && rr_has(&retained, entry) &&
                rr_retired_shell(entry)) {
                entry->retired = 1;
                entry->cause = "retained from the failed activation, retired by this provider; "
                    "its default is rebuilt";
            }
            if (entry->retired && rr_keeps_parsed_content(entry->type)) {
                entry->retired = entry->selected = 0; entry->kept = 1; continue;
            }
            entry->selected = supplied || owner || rr_has(&impact, entry);
            /* Only content this provider supplies, or permanent content it can
             * change, may be rebuilt. A map-scoped identity such as the live
             * entitydef:world is built by the map that is already gone --
             * it exists in no archive, so reconstruction replaces live engine
             * state with a default and then refuses the whole activation, and
             * the next map load rebuilds it from the new provider anyway.
             * Implicit-text declarations carry no source to re-read at all.
             * Both are reached only as recorded consumers; a provider that
             * does supply the bytes still selects the identity by path. */
            if (!supplied && (entry->implicit || entry->captured_level != RR_LEVEL_PERMANENT)) {
                /* Its fontInfo would keep the old size: wait until the font is gone or permanent. */
                if (owner) {
                    current = entry;
                    if (error && capacity) snprintf(error, capacity, "in-place image rebuild has a copied-size "
                        "owner this pass cannot rebuild: %s:%s (level %u%s)", entry->type, entry->name,
                        entry->captured_level, entry->implicit ? ", implicit text" : "");
                    phase = NULL; goto done;
                }
                entry->selected = 0;
            }
        }
        /* Type-info inheritance copies fields from the parent; retaining the
         * parent's address does not update a child's copy. Capture and follow
         * these native links even when the source was parsed before our graph.
         * Startup children are not permanent yet. Only this proven inheritance
         * relationship may admit them, not arbitrary startup consumers. */
        phase = "native inheritance";
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            void *parent = entry->parent;
            size_t depth = 0;
            if (entry->selected || entry->kept || entry->implicit ||
                (!initial && entry->captured_level != RR_LEVEL_PERMANENT)) continue;
            while (parent) {
                const rr_known *known = bsearch(&parent, pass->known, pass->known_count,
                    sizeof(*pass->known), rr_known_compare);
                const rr_entry *ancestor;
                if (!known || known->entry >= pass->count || ++depth > pass->count) goto done;
                ancestor = pass->items + known->entry;
                if (ancestor->selected) { entry->selected = 1; entry->cause = "native inheritance from a selected parent"; break; }
                parent = ancestor->parent;
            }
        }
        /* An identity this provider retired stays registered as a defaulted
         * shell: reconstruction left it fresh storage, no native record and
         * the default, and the ledger says no later pass supplied it. When an
         * addition names it again, only this pass supplies it. Bringing it
         * back is an addition, not a change to an existing consumer, unless
         * something else selected stays live. A default that retains a native
         * record, or that this provider never retired, still blocks. */
        for (size_t i = 0; !restoring && i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            entry->revived = entry->selected && entry->added && !entry->retired && !entry->kept &&
                rr_retired_shell(entry);
        }
        /* Each retirement's default must be buildable in this process before
         * anything is reconstructed. Refusing here changes nothing; refusing
         * later would leave fresh storage that a later lookup returns as it
         * is, because find-or-load does not default an object whose error
         * slot is clear (R-321). */
        for (size_t i = 0; g_rr.default_ready && i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            if (!entry->selected || !entry->retired || g_rr.default_ready(entry->type)) continue;
            current = entry;
            if (error && capacity) snprintf(error, capacity, "native resident retirement cannot build the engine "
                "default for %s:%s in this process", entry->type, entry->name);
            phase = NULL; goto done;
        }
        if (!restoring) { pass->prior = impact; memset(&impact, 0, sizeof(impact)); }
        /* Compact only after native path capture; no engine call may mutate the
         * inventory during its collection. Names survive native reconstruction. */
        {
            size_t kept = 0;
            for (size_t i = 0; i < pass->count; i++) {
                if (pass->items[i].selected) pass->items[kept++] = pass->items[i];
                else { free(pass->items[i].type); free(pass->items[i].name); }
            }
            pass->count = kept;
        }
render_scope:
        phase = "renderer scope";
        if (!sh_render_scope_enter(g_rr.renderer, g_rr.thread, &pass->render)) goto done;
        /* Private page providers must exist before any material that names
         * one parses; the engine classifies the material by that name. */
        phase = "virtual texture page providers";
        if (!sh_virtual_material_runtime_providers(NULL, 0)) goto done;
        ok = 1;
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    sh_resource_graph_impact_free(&seeds); sh_resource_graph_impact_free(&impact);
    sh_resource_graph_impact_free(&retired); sh_resource_graph_impact_free(&additions);
    sh_resource_graph_impact_free(&retained);
    sh_resource_graph_impact_free(&recovering); sh_resource_graph_impact_free(&returning);
    sh_resource_graph_impact_free(&recovery_impact); sh_resource_graph_impact_free(&owners);
    if (!ok) {
        /* A NULL phase carries its own complete message (the in-place recovery refusals). */
        if (error && capacity && phase) snprintf(error, capacity, "native resident %s failed%s%s%s%s", phase,
            current ? " for " : "", current ? current->type : "", current ? ":" : "", current ? current->name : "");
        sh_resource_resident_end(pass, 0, error, capacity); return NULL;
    }
    g_rr_active = pass;
    if (!rr_transfer_prepare(pass,error,capacity)) {
        sh_resource_resident_end(pass,0,error,capacity); return NULL;
    }
    return pass;
}
int sh_resource_resident_owned(const sh_resource_resident *pass)
{
    __try {
        if (!pass || g_rr_active != pass || pass->thread != GetCurrentThreadId() ||
            !g_rr.thread || *g_rr.thread != pass->thread || !sh_render_scope_owner(&pass->render) ||
            pass->render.active != 2 || pass->heap.active != 2 || !pass->heap.api ||
            pass->heap.api->get() != pass->heap.self) return 0;
        const unsigned char *heap = pass->heap.self;
        return *(const int *)((const unsigned char *)pass->render.renderer + 0x14) == pass->render.count + 1 &&
            *(const int *)(heap + 0xc4) == pass->heap.depth + 1 &&
            *(const int *)(heap + 0x44 + pass->heap.depth * 4) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void sh_resource_resident_touch(void *resource)
{
    sh_resource_resident *pass = g_rr_active;
    const rr_known *known;
    if (!pass || !resource) return;
    /* New content this pass created needs permanent lifetime to survive the
     * map-transition purge. An identity that already existed keeps the tier it
     * had: promoting a map-scoped object would outlive the heap that owns it
     * and leave the registry pointing at freed storage. */
    known = pass->known ? bsearch(&resource, pass->known, pass->known_count,
                                  sizeof(*pass->known), rr_known_compare) : NULL;
    if (known && known->level != RR_LEVEL_PERMANENT) return;
    __try { *(unsigned int *)((unsigned char *)resource + RR_LEVEL_OFFSET) = RR_LEVEL_PERMANENT; }
    __except (EXCEPTION_EXECUTE_HANDLER) { pass->touch_failed = 1; }
}
int sh_resource_resident_selected(const sh_resource_resident *pass, const void *resource)
{
    if (pass) for (size_t i = 0; i < pass->count; i++)
        if (pass->items[i].resource == resource) return 1;
    return 0;
}
int sh_resource_resident_describe(const sh_resource_resident *pass, const void *resource, char *out, size_t capacity)
{
    if (out && capacity) *out = 0;
    if (pass) for (size_t i = 0; i < pass->count; i++) if (pass->items[i].resource == resource) {
        const rr_entry *entry = pass->items + i;
        if (out && capacity) snprintf(out, capacity, "%s:%s (%s; state=0x%02x level=%u)", entry->type ? entry->type : "?",
            entry->name ? entry->name : "?", entry->cause ? entry->cause : "selected", (unsigned)entry->captured_state,
            entry->captured_level);
        return 1;
    }
    return 0;
}
size_t sh_resource_resident_selected_count(const sh_resource_resident *pass)
{ return pass ? pass->count : SIZE_MAX; }
size_t sh_resource_resident_selected_type(const sh_resource_resident *pass, const char *type)
{
    size_t count = 0;
    if (!pass || !type) return SIZE_MAX;
    for (size_t i = 0; i < pass->count; i++)
        if (pass->items[i].type && !_stricmp(pass->items[i].type, type)) count++;
    return count;
}
static const char *rr_blocking_reason(const sh_resource_resident *pass, const rr_entry *entry)
{
    if (pass->restoring) return "recovery";
    if (!entry->added) return "the addition does not name it";
    if (!(entry->captured_state & RR_DEFAULT)) return "live";
    if (entry->captured_state & RR_PENDING) return "pending";
    if (entry->captured_source) return "default with a native record";
    if (!rr_retired_by_provider(entry)) return "default this provider never retired";
    return "existing";
}
size_t sh_resource_resident_blocking_count(const sh_resource_resident *pass, char *first, size_t capacity)
{
    size_t blocking = 0;
    if (first && capacity) first[0] = 0;
    if (!pass) return SIZE_MAX;
    for (size_t i = 0; i < pass->count; i++) if (!pass->items[i].revived && !pass->items[i].refreshed) {
        if (!blocking++ && first && capacity)
            snprintf(first, capacity, "%s:%s: %s", pass->items[i].type, pass->items[i].name,
                rr_blocking_reason(pass, pass->items + i));
    }
    return blocking;
}
size_t sh_resource_resident_release_revived(const sh_resource_resident *pass, void **existing, size_t count)
{
    size_t kept = 0;
    if (!pass || !existing) return count;
    for (size_t i = 0; i < count; i++) {
        int owned = 0;
        for (size_t j = 0; j < pass->count && !owned; j++)
            owned = (pass->items[j].revived || pass->items[j].refreshed) && pass->items[j].resource == existing[i];
        if (!owned) existing[kept++] = existing[i];
    }
    for (size_t j = 0; j < pass->count; j++) if (pass->items[j].revived || pass->items[j].refreshed) {
        char line[512];
        rr_entry *entry = pass->items + j;
        /* Its old default stays until the addition's declarations exist. */
        if (entry->refreshed && !pass->rebuilt) entry->deferred = 1;
        snprintf(line, sizeof(line), entry->revived ?
            "native resident addition revives retired %s:%s (retired by this provider, engine default, no native record)" :
            "native resident addition refreshes retired %s:%s (retired by this provider, a recorded consumer of the "
            "addition; its default is rebuilt after the addition's declarations exist)", entry->type, entry->name);
        backend_log(line);
    }
    return kept;
}
int sh_resource_resident_take_prior(sh_resource_resident *pass, sh_resource_graph_impact *out)
{
    if (!pass || !out) return 0;
    sh_resource_graph_impact_free(out);
    *out = pass->prior; memset(&pass->prior, 0, sizeof(pass->prior));
    return 1;
}
int sh_resource_resident_acquired(const sh_resource_resident *pass, const void *resource,
    const sh_resource_graph_impact *prior)
{
    if (!pass || !pass->restoring || !prior) return 0;
    for (size_t i = 0; i < pass->count; i++) if (pass->items[i].resource == resource) {
        const rr_entry *entry = pass->items + i;
        return entry->consumer && !entry->implicit && entry->captured_level == RR_LEVEL_PERMANENT &&
            !rr_has(prior, entry);
    }
    return 0;
}
int sh_resource_resident_revives(const sh_resource_resident *pass, const void *resource,
    const char *type, const char *name)
{
    if (!pass || !resource || !type || !name) return 0;
    for (size_t i = 0; i < pass->count; i++) {
        const rr_entry *entry = pass->items + i;
        if (entry->revived && entry->resource == resource && entry->type && entry->name &&
            !_stricmp(entry->type, type) && !_stricmp(entry->name, name)) return 1;
    }
    return 0;
}
void sh_resource_resident_external(sh_resource_resident *pass, void *resource)
{
    if (pass && !pass->rebuilt) for (size_t i = 0; i < pass->count; i++)
        if (pass->items[i].resource == resource) {
            pass->items[i].external = 1; pass->mutated = 1;
        }
}
void sh_resource_resident_set_default_construction(int (*enter)(sh_discrete_scope *scope),
    int (*leave)(sh_discrete_scope *scope), int (*ready)(const char *family))
{ g_rr.default_enter = enter; g_rr.default_leave = leave; g_rr.default_ready = ready; }
void sh_resource_resident_set_default_observation(int (*observed)(void *resource))
{ g_rr.default_observed = observed; }
void sh_resource_resident_set_default_invoke(sh_resource_default_invoke invoke)
{ g_rr.default_invoke = invoke; }
void sh_resource_resident_set_default_origins(int (*begin)(sh_resource_default_origin_scope *,void *),
    int (*finish)(sh_resource_default_origin_scope *,void *,int,int,int))
{
    g_rr.origin_begin = begin && finish ? begin : NULL;
    g_rr.origin_finish = begin && finish ? finish : NULL;
}
void sh_resource_resident_set_instance_refresh(int (*refresh)(void *const *,size_t,char *,size_t))
{ g_rr.instance_refresh = refresh; }
void sh_resource_resident_set_instance_admit(int (*admit)(void *const *,size_t,char *,size_t))
{ g_rr.instance_admit=admit; }
void sh_resource_resident_set_instance_report(size_t (*report)(const sh_resource_admit_report **items))
{ g_rr.instance_report=report; }
size_t sh_resource_resident_isolated_count(const sh_resource_resident *pass)
{
    size_t n=0;
    for(size_t i=0;pass && i<pass->count;i++) n+=pass->items[i].isolated!=0;
    return n;
}
int sh_resource_resident_isolated(const sh_resource_resident *pass, const void *resource)
{
    for(size_t i=0;pass && i<pass->count;i++) if(pass->items[i].resource==resource) return pass->items[i].isolated;
    return 0;
}
int sh_resource_resident_isolated_identity(const sh_resource_resident *pass, const char *type, const char *name)
{
    for(size_t i=0;pass && type && name && i<pass->count;i++)
        if(pass->items[i].isolated && pass->items[i].type && pass->items[i].name &&
            !_stricmp(pass->items[i].type,type) && !_stricmp(pass->items[i].name,name)) return 1;
    return 0;
}
void sh_resource_resident_set_instance_transfer(sh_resource_instance_transfer_prepare prepare,
    sh_resource_instance_transfer_finish finish,sh_resource_instance_transfer_dispose dispose)
{
    int present=prepare && finish && dispose;
    g_rr.transfer_prepare=present?prepare:NULL;
    g_rr.transfer_finish=present?finish:NULL;
    g_rr.transfer_dispose=present?dispose:NULL;
}
static void **rr_transfer_resources(sh_resource_resident *pass,char *error,size_t capacity)
{
    if(pass->count>SIZE_MAX/sizeof(void *)) {
        rr_error(error,capacity,"native instance transfer resource list overflow"); return NULL;
    }
    void **resources=malloc((pass->count?pass->count:1)*sizeof(*resources));
    if(!resources) { rr_error(error,capacity,"native instance transfer resource list allocation failed"); return NULL; }
    for(size_t i=0;i<pass->count;i++) resources[i]=pass->items[i].resource;
    return resources;
}
static int rr_transfer_prepare(sh_resource_resident *pass,char *error,size_t capacity)
{
    const sh_resource_resident *prior=g_rr_recovery;
    if(prior && prior->transfer &&
       (prior->transfer_prepare!=g_rr.transfer_prepare || prior->transfer_finish!=g_rr.transfer_finish ||
        prior->transfer_dispose!=g_rr.transfer_dispose))
        return rr_error(error,capacity,"native instance transfer recovery implementation changed");
    if(!g_rr.transfer_prepare) return 1;
    if(!sh_resource_resident_owned(pass))
        return rr_error(error,capacity,"native instance transfer has no owning exclusion");
    pass->transfer_prepare=g_rr.transfer_prepare;
    pass->transfer_finish=g_rr.transfer_finish;
    pass->transfer_dispose=g_rr.transfer_dispose;
    void **resources=rr_transfer_resources(pass,error,capacity); if(!resources) return 0;
    int ok=0;
    __try { ok=pass->transfer_prepare(resources,pass->count,prior?prior->transfer:NULL,
                                     &pass->transfer,error,capacity); }
    __except(EXCEPTION_EXECUTE_HANDLER) { rr_error(error,capacity,"native instance transfer prepare raised an engine error"); }
    free(resources); return ok;
}
static int rr_transfer_finish(sh_resource_resident *pass,char *error,size_t capacity)
{
    if(!pass->transfer) return 1;
    if(!pass->transfer_finish || !sh_resource_resident_owned(pass))
        return rr_error(error,capacity,"native instance transfer lost its owning exclusion");
    void **resources=rr_transfer_resources(pass,error,capacity); if(!resources) return 0;
    int ok=0;
    __try { ok=pass->transfer_finish(pass->transfer,resources,pass->count,error,capacity); }
    __except(EXCEPTION_EXECUTE_HANDLER) { rr_error(error,capacity,"native instance transfer finish raised an engine error"); }
    free(resources); return ok;
}
/* One admission report line, with the input named from this pass's entries. */
static void rr_admit_report_line(const sh_resource_resident *pass,const sh_resource_admit_report *item)
{
    const char *type="?",*name="?";
    char line[640];
    for(size_t i=0;i<pass->count;i++) if(pass->items[i].resource==item->input) {
        type=pass->items[i].type?pass->items[i].type:"?"; name=pass->items[i].name?pass->items[i].name:"?"; break;
    }
    if(item->tolerated)
        snprintf(line,sizeof(line),"native resident: %s holder (vtable +0x%llx) reads %s:%s through %s%llx the product never "
            "observed; tolerated: the holder is rebuilt from its inputs after the mutation",
            item->holder_kind,(unsigned long long)item->holder_vtable_rva,type,name,
            item->slot==SIZE_MAX?"a typed path":"field +0x",item->slot==SIZE_MAX?0ull:(unsigned long long)item->slot);
    else
        snprintf(line,sizeof(line),"native resident isolates %s:%s: %s holder (vtable +0x%llx) reads it through %s%llx the "
            "product never observed%s and cannot be rebuilt; this pass leaves the identity's object and text as they are",
            type,name,item->holder_kind,(unsigned long long)item->holder_vtable_rva,
            item->slot==SIZE_MAX?"a typed path":"field +0x",item->slot==SIZE_MAX?0ull:(unsigned long long)item->slot,
            item->unresolved_path?" (its path no longer resolves)":"");
    backend_log(line);
}
/* Admission before mutation: holders reading an input through a field the product
 * cannot attribute. A holder the refresh rebuilds whole after the mutation is
 * tolerated (reported). One it cannot rebuild isolates that input: the entry is
 * left unmutated and the admission is asked again without it. The pass fails only
 * when an unattributed input cannot be isolated (not one of its own entries) or
 * admission refuses for another reason. */
static int rr_instance_admit(sh_resource_resident *pass,char *error,size_t capacity)
{
    if(!g_rr.instance_admit || !pass->count) return 1;
    if(pass->count>SIZE_MAX/sizeof(void *)) return rr_error(error,capacity,"native instance admission resource list overflow");
    void **resources=malloc(pass->count*sizeof(*resources));
    if(!resources) return rr_error(error,capacity,"native instance admission allocation failed");
    int admitted=0;
    for(int attempt=0;attempt<2 && !admitted;attempt++) {
        size_t count=0,isolated=0; int reported=0;
        const sh_resource_admit_report *items=NULL; size_t item_count=0;
        for(size_t i=0;i<pass->count;i++)
            if(!pass->items[i].external && !pass->items[i].isolated) resources[count++]=pass->items[i].resource;
        __try { admitted=g_rr.instance_admit(resources,count,error,capacity); reported=1; }
        __except(EXCEPTION_EXECUTE_HANDLER) { rr_error(error,capacity,"native instance admission raised an engine error"); }
        if(!reported) break;
        if(g_rr.instance_report) item_count=g_rr.instance_report(&items);
        for(size_t k=0;k<item_count;k++) {
            rr_admit_report_line(pass,items+k);
            if(items[k].tolerated) continue;
            for(size_t i=0;i<pass->count;i++) if(pass->items[i].resource==items[k].input && !pass->items[i].isolated) {
                pass->items[i].isolated=1; pass->mutated=1; isolated++;
            }
        }
        if(!admitted && !isolated) break;
    }
    free(resources); return admitted;
}
void sh_resource_resident_set_default_probe(sh_resource_default_probe_prepare prepare,
    sh_resource_default_probe_finish finish)
{
    g_rr.default_prepare = prepare && finish ? prepare : NULL;
    g_rr.default_finish = prepare && finish ? finish : NULL;
}
/* The default construction scope, on the calling execution stack (discrete_default.h). */
static int rr_scope_enter(sh_discrete_scope *scope) { return !g_rr.default_enter || g_rr.default_enter(scope); }
static int rr_scope_leave(sh_discrete_scope *scope) { return !g_rr.default_leave || g_rr.default_leave(scope); }
/* A drain's default construction scope did not end in order: a box built inside it may have taken
 * the engine's route. The pass fails with it, as with a failed retirement default (R-321). */
static int rr_scope_lost(sh_resource_resident *pass, char *error, size_t capacity)
{
    static const char message[] = "native resident default construction scope did not end in order";
    if (!pass->default_faults++) snprintf(pass->default_failure, sizeof(pass->default_failure), "%s", message);
    backend_log(message);
    return rr_error(error, capacity, message);
}
/* Preserve storage/identity. Native level-four destructors warn; level two
 * permits their synchronous in-place reconstruction. Restore the tier this
 * identity already had: a supplied map-scoped resource stays map-scoped and is
 * still purged with its map. */
static void rr_reconstruct_one(rr_entry *entry)
{
    unsigned char *object = entry->resource;
    entry->reconstructed = 1;
    __try {
        if (*(unsigned int *)(object + RR_LEVEL_OFFSET) == RR_LEVEL_PERMANENT)
            *(unsigned int *)(object + RR_LEVEL_OFFSET) = 2;
        sh_package_shader_receipt_invalidate((uintptr_t)object);
        g_rr.reconstruct(object);
    } __finally { *(unsigned int *)(object + RR_LEVEL_OFFSET) = RR_LEVEL_PERMANENT; }
}
int sh_resource_resident_reconstruct(sh_resource_resident *pass, char *error, size_t capacity)
{
    if (!pass || pass->thread != GetCurrentThreadId()) return rr_error(error, capacity, "resident refresh thread mismatch");
    if (pass->rebuilt) return 1;
    if(!rr_instance_admit(pass,error,capacity)) return 0;
    __try {
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            if (entry->external || entry->isolated || entry->deferred) continue;
            pass->mutated = 1; rr_reconstruct_one(entry);
        }
        for (size_t i = 0; i < pass->count; i++) if (!pass->items[i].external && !pass->items[i].isolated && !pass->items[i].deferred) {
            unsigned char *state = (unsigned char *)pass->items[i].resource + 0x2c;
            if (pass->items[i].retired) *state = (*state & 0xfdu) | RR_DEFAULT;
            else *state |= RR_PENDING;
            rr_retirement_note(pass->items + i);
        }
        pass->rebuilt = 1; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return rr_error(error, capacity, "native resident reconstruction failed");
    }
}
/* The class's default callback, inside the default construction scope. NULL, or why it failed.
 * The identity's dependency frame also records what the default asked for and did not find, so
 * a later addition of that identity reaches this one as a recorded consumer. It completes only
 * when the default returned inside a scope that ended in order and the observation predicate, asked
 * while the frame is current, accepts the default routine the object ran (R-352); an image recovery
 * whose consumer closure reaches the identity can then retire it again instead of refusing it as
 * unobserved. */
static const char *rr_default(rr_entry *entry)
{
    const char *failure = NULL;
    sh_discrete_scope scope;
    sh_resource_graph_frame frame;
    sh_resource_default_read_receipt receipt = {0};
    sh_resource_default_read_input input = {0};
    sh_resource_default_probe_prepare prepare = g_rr.default_prepare;
    sh_resource_default_probe_finish finish = g_rr.default_finish;
    sh_resource_default_invoke invoke = g_rr.default_invoke;
    int (*origin_begin)(sh_resource_default_origin_scope *,void *) = g_rr.origin_begin;
    int (*origin_finish)(sh_resource_default_origin_scope *,void *,int,int,int) = g_rr.origin_finish;
    sh_resource_default_origin_scope origin_scope = {0};
    volatile int origin_entered = 0;
    volatile int returned = 0, prepared = 0;
    int ordered = 0, observed = 0;
    if (!rr_scope_enter(&scope)) return "native resident default construction scope could not be opened";
    sh_resource_graph_begin_requests(&frame, entry->type, entry->name);
    __try {
        void **table = *(void ***)entry->resource;
        void *actual = table[0x50/8];
        if (origin_begin) {
            __try { origin_entered = origin_begin(&origin_scope,entry->resource); }
            __except(EXCEPTION_EXECUTE_HANDLER) { origin_entered = 0; }
        }
        if (prepare) {
            __try { prepare(&receipt, &input, entry->resource, actual); prepared = 1; }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                memset(&receipt, 0, sizeof(receipt)); memset(&input, 0, sizeof(input));
            }
        }
        if (invoke) {
            if (invoke(entry->resource, actual)) returned = 1;
            else failure = "native resident default construction adapter failed";
        } else { ((void (*)(void *))actual)(entry->resource); returned = 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failure = "native resident default construction raised an engine error";
    }
    /* A scope that did not end in order was lost while the default ran, so its box may have
     * taken the engine's route. */
    __try { ordered = rr_scope_leave(&scope); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ordered = 0; }
    if (!ordered && !failure) failure = "native resident default construction scope did not end in order";
    if (finish) {
        __try { observed = finish(&receipt, &input, returned, prepared && ordered && sh_resource_graph_frame_current(&frame)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { observed = 0; }
    } else if (returned && !failure && g_rr.default_observed) {
        __try { observed = g_rr.default_observed(entry->resource); }
        __except (EXCEPTION_EXECUTE_HANDLER) { observed = 0; }
    }
    if (origin_finish) {
        int origin_ok = 0;
        __try { origin_ok = origin_finish(&origin_scope,entry->resource,returned && !failure,
            origin_entered && ordered && sh_resource_graph_frame_current(&frame),observed); }
        __except(EXCEPTION_EXECUTE_HANDLER) { origin_ok = 0; }
        observed = observed && origin_ok;
    }
    sh_resource_graph_end(&frame, returned && !failure && observed);
    return failure;
}
int sh_resource_resident_defaults(sh_resource_resident *pass, char *error, size_t capacity)
{
    if (!pass || pass->thread != GetCurrentThreadId() || !pass->rebuilt)
        return rr_error(error, capacity, "resident defaults preceded reconstruction");
    if (pass->defaults_ready) return 1;
    if(!rr_instance_admit(pass,error,capacity)) return 0;
    /* A removed, non-stock source must not fall through to an old native
     * declaration source record. Reconstruction already gave each retired
     * identity fresh storage and set its defaulted state. A later find-or-load
     * returns such an object as it is while its error slot is clear, without
     * defaulting it, so this calls the class's default callback now, before
     * any surviving consumer can look it up. No missing-file read or
     * source-generation fallback.
     *
     * The callback runs inside the default construction scope, where the
     * discrete animation default keeps its temporary box on the CPU instead
     * of reading back static buffers, which Vulkan refuses while it renders.
     * A default that throws went through the engine's error path, which
     * reports the error before it throws, so it is not a clean retirement.
     * Every remaining retirement still gets its default and every load of
     * the pass still runs; then the drain and the end fail with the first
     * failure (R-321).
     *
     * A shell an additive admission refreshes is rebuilt only now, in place as
     * reconstruction rebuilds the others, after the admission's declarations
     * exist: its default then finds what the addition supplies, and until now
     * its old default stayed intact for any lookup. A rebuild the engine
     * refuses fails the pass and leaves that object to the restoration. */
    for (size_t i = 0; i < pass->count; i++) {
        rr_entry *entry = pass->items + i;
        char line[512];
        volatile int rebuilt = 0;
        if (!entry->deferred) continue;
        entry->deferred = 0; pass->mutated = 1;
        __try {
            unsigned char *state = (unsigned char *)entry->resource + 0x2c;
            rr_reconstruct_one(entry);
            *state = (unsigned char)((*state & 0xfdu) | RR_DEFAULT);
            rr_retirement_note(entry); rebuilt = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { entry->external = 1; }
        if (rebuilt) continue;
        snprintf(line, sizeof(line), "native resident reconstruction failed for %s:%s", entry->type, entry->name);
        if (!pass->default_faults++) snprintf(pass->default_failure, sizeof(pass->default_failure), "%s", line);
        backend_log(line);
    }
    for (size_t i = 0; i < pass->count; i++) {
        rr_entry *entry = pass->items + i;
        const char *failure;
        char line[512];
        if (entry->external || entry->isolated || !entry->retired) continue;
        failure = g_rr.default_ready && !g_rr.default_ready(entry->type) ?
            "native resident default construction is unavailable in this process" : rr_default(entry);
        if (!failure) continue;
        snprintf(line, sizeof(line), "%s for %s:%s", failure, entry->type ? entry->type : "?",
            entry->name ? entry->name : "?");
        if (!pass->default_faults++) snprintf(pass->default_failure, sizeof(pass->default_failure), "%s", line);
        backend_log(line);
    }
    pass->defaults_ready = 1;
    if (pass->default_faults) {
        size_t used = strlen(pass->default_failure);
        snprintf(pass->default_failure + used, sizeof(pass->default_failure) - used,
            " (%zu retirement default(s) failed)", pass->default_faults);
        backend_log(pass->default_failure);
    }
    return 1;
}
size_t sh_resource_resident_recovered_defaults(const sh_resource_resident *pass)
{ return pass ? pass->recovered_defaults : 0; }
static int rr_pointer_compare(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)*(void *const *)a, y = (uintptr_t)*(void *const *)b;
    return x < y ? -1 : x > y;
}
static int rr_existing(void *resource, void *const *existing, size_t count)
{ return count && bsearch(&resource, existing, count, sizeof(*existing), rr_pointer_compare) != NULL; }
static int rr_finalize_additions(sh_resource_resident *pass, void *const *existing,
    size_t existing_count, char *error, size_t capacity)
{
    void *list = *g_rr.head;
    size_t visited = 0, lists = 0;
    while (list && lists++ < RR_LIST_LIMIT) {
        unsigned char *manager = list;
        const char *type = *(const char **)(manager + 8);
        void **items = *(void ***)(manager + 0x20);
        int count = *(int *)(manager + 0x28);
        if (!type || count < 0 || count >= RR_ENTRY_LIMIT || (count && !items))
            return rr_error(error, capacity, "fresh resource inventory is incomplete");
        for (int i = 0; i < count; i++) if (items[i]) {
            if (++visited > RR_ENTRY_LIMIT) return rr_error(error, capacity, "fresh resource inventory overflow");
            void *resource = items[i];
            if (rr_existing(resource, existing, existing_count)) continue;
            /* Lookup observers cover ordinary loads; the complete registry walk
             * also covers native objects created indirectly during parsing. */
            *(unsigned int *)((unsigned char *)resource + RR_LEVEL_OFFSET) = RR_LEVEL_PERMANENT;
            if (!_stricmp(type, "material")) g_rr.rebind_material(resource);
            else if (!_stricmp(type, "snapeditorsettings")) {
                void **fonts = (void **)((unsigned char *)resource + 0x688);
                g_rr.world_text_fonts(fonts);
                if (!fonts[0] || !fonts[1] || !fonts[2])
                    return rr_error(error, capacity, "fresh world text fonts did not resolve");
            }
        }
        list = *(void **)(manager + 0x18);
    }
    if (list || !sh_resource_resident_owned(pass))
        return rr_error(error, capacity, "fresh resource finalization lost its admitted scope");
    return 1;
}
static int rr_drain_leaves(sh_resource_resident *pass,
    int (*leaf)(const char *family), char *error, size_t capacity);
int sh_resource_resident_drain_leaves(sh_resource_resident *pass,
    int (*leaf)(const char *family), char *error, size_t capacity)
{
    /* A load that fails reaches the engine default: build it inside the scope. */
    sh_discrete_scope scope;
    volatile int ok = 0, ordered = 1;
    if (!rr_scope_enter(&scope))
        return rr_error(error, capacity, "native resident default construction scope could not be opened");
    __try { ok = rr_drain_leaves(pass, leaf, error, capacity); }
    __finally { ordered = rr_scope_leave(&scope); }
    if (ok && !ordered) return rr_scope_lost(pass, error, capacity);
    return ok;
}
static int rr_drain_leaves(sh_resource_resident *pass,
    int (*leaf)(const char *family), char *error, size_t capacity)
{
    if (!pass || !leaf || pass->thread != GetCurrentThreadId() || !pass->rebuilt)
        return rr_error(error, capacity, "resident leaf reload has no rebuilt scope");
    if(!rr_instance_admit(pass,error,capacity)) return 0;
    __try {
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            unsigned char *object = entry->resource;
            if (entry->external || entry->isolated || !leaf(entry->type) || !(object[0x2c] & RR_PENDING)) continue;
            if (g_rr.lookup(entry->manager, entry->name) != object)
                return rr_error(error, capacity, "native resident lookup changed its captured identity");
            if (object[0x2c] & RR_PENDING) { object[0x2c] &= 0xfdu; g_rr.load(object); }
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return rr_error(error, capacity, "native resident leaf reload failed");
    }
}
static int rr_drain_scoped(sh_resource_resident *pass, void *const *existing,
    size_t existing_count, int additions, char *error, size_t capacity);
static int rr_drain(sh_resource_resident *pass, void *const *existing,
    size_t existing_count, int additions, char *error, size_t capacity)
{
    /* Defaults, and loads that fail, reach the engine default: build them inside the scope. */
    sh_discrete_scope scope;
    volatile int ok = 0, ordered = 1;
    if (!rr_scope_enter(&scope))
        return rr_error(error, capacity, "native resident default construction scope could not be opened");
    __try { ok = rr_drain_scoped(pass, existing, existing_count, additions, error, capacity); }
    __finally { ordered = rr_scope_leave(&scope); }
    if (ok && !ordered) return rr_scope_lost(pass, error, capacity);
    return ok;
}
static int rr_drain_scoped(sh_resource_resident *pass, void *const *existing,
    size_t existing_count, int additions, char *error, size_t capacity)
{
    if (!pass || pass->thread != GetCurrentThreadId() || !pass->rebuilt)
        return rr_error(error, capacity, "resident refresh was not reconstructed");
    if (additions) {
        if ((existing_count && !existing) || !g_rr.rebind_material || !sh_resource_resident_owned(pass))
            return rr_error(error, capacity, "fresh resource finalization has no admitted ownership");
        for (size_t i = 0; i < pass->count; i++)
            if (rr_existing(pass->items[i].resource, existing, existing_count))
                return rr_error(error, capacity, "fresh resource finalization selected a pre-existing object");
    }
    if (!sh_resource_resident_defaults(pass, error, capacity)) return 0;
    /* Draining twice is legal: the declaration pass finishes this refresh
     * before its palette rebuild, and the provider transaction still calls it.
     * A retirement default that failed fails every drain (R-321). */
    if (pass->drained && !additions) return pass->default_faults ? rr_error(error, capacity, pass->default_failure) :
        sh_virtual_material_runtime_status(error, capacity);
    if(!rr_instance_admit(pass,error,capacity)) return 0;
    __try {
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            unsigned char *object = entry->resource;
            if (entry->external || entry->isolated) continue;
            if (object[0x2c] & RR_PENDING) {
                if (g_rr.lookup(entry->manager, entry->name) != object)
                    return rr_error(error, capacity, "native resident lookup changed its captured identity");
                if (object[0x2c] & RR_PENDING) {
                    object[0x2c] &= 0xfdu; g_rr.load(object);
                }
            }
            /* Compare against the captured state: an identity the engine only
             * ever serves as a default reloads as a default, and demanding
             * otherwise would refuse an activation that changed nothing. */
            {
                int defaulted = (object[0x2c] & RR_DEFAULT) && !(entry->captured_state & RR_DEFAULT);
                int retained = *(void **)(object + 0x18) != NULL && !entry->captured_source;
                /* A revived shell was default before; its addition must parse. */
                if (entry->revived) {
                    if (object[0x2c] & (RR_PENDING | RR_DEFAULT)) {
                        if (error && capacity) snprintf(error, capacity,
                            "revived resource '%s:%s' did not parse its addition (state=0x%02x)",
                            entry->type, entry->name, (unsigned)object[0x2c]);
                        return 0;
                    }
                    continue;
                }
                /* Recovery restores the provider that was live before a failed
                 * activation. That provider has no source for an identity the
                 * failed attempt created, so the engine's default is the correct
                 * state for it; the retirement decision cannot know that,
                 * because it reads the change set and the installed catalog, not
                 * the provider being restored. Report each one and continue:
                 * failing recovery instead would leave the session with the
                 * attempt's half-published state and no way back. A still
                 * pending mark is a real inconsistency and still fails. */
                if (pass->restoring && !(object[0x2c] & RR_PENDING) && (defaulted || retained)) {
                    char line[512];
                    snprintf(line, sizeof(line),
                        "native resident recovery left %s:%s at the engine default (state=0x%02x was 0x%02x%s%s); "
                        "the restored provider supplies no source for it",
                        entry->type ? entry->type : "?", entry->name ? entry->name : "?",
                        (unsigned)object[0x2c], (unsigned)entry->captured_state,
                        defaulted ? ", newly defaulted" : "", retained ? ", source newly retained" : "");
                    backend_log(line);
                    pass->recovered_defaults++;
                    continue;
                }
                if ((object[0x2c] & RR_PENDING) || (!entry->retired && (defaulted || retained))) {
                    if (error && capacity) snprintf(error, capacity,
                        "resource '%s:%s' failed native refresh (state=0x%02x was 0x%02x%s%s%s)",
                        entry->type, entry->name, (unsigned)object[0x2c],
                        (unsigned)entry->captured_state,
                        (object[0x2c] & RR_PENDING) ? ", still pending" : "",
                        defaulted ? ", newly defaulted" : "",
                        retained ? ", source newly retained" : "");
                    return 0;
                }
            }
        }
        /* The settings parser retains font names but does not resolve its three
         * cached idFont pointers. The stock editor copies those pointers into
         * its World Text manager without the game's initialization step. Run
         * that native finalizer after every load, including declaration-owned
         * loads, before palette previews can consume the settings. Both images
         * place snapWorldTextSettings_t at +0x688. */
        for (size_t i = 0; !additions && i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            if (!strcmp(entry->type, "snapeditorsettings")) {
                void **fonts = (void **)((unsigned char *)entry->resource + 0x688);
                g_rr.world_text_fonts(fonts);
                if (!fonts[0] || !fonts[1] || !fonts[2])
                    return rr_error_at(error, capacity, "world text fonts did not resolve", entry->type, entry->name);
            }
        }
        /* Reconstruction discarded each rebuilt material's virtual-texture parm
         * binding, which the engine established once at boot in process heap 0.
         * Left alone, the next map load rebuilds it inside the map or persist
         * heap scope; a permanent material then owns storage ResetPersistHeap
         * frees when Play exits, and the following rebind writes through it.
         * Rebind every material here, still inside this pass's heap-0 scope. */
        if (additions) {
            if (!rr_finalize_additions(pass, existing, existing_count, error, capacity)) return 0;
        } else g_rr.rebind();
        if (!sh_virtual_material_runtime_status(error, capacity)) return 0;
        if (!rr_transfer_finish(pass,error,capacity)) return 0;
        if (g_rr.instance_refresh && pass->count) {
            if (pass->count > SIZE_MAX / sizeof(void *))
                return rr_error(error,capacity,"native instance refresh resource list overflow");
            void **resources = malloc(pass->count*sizeof(*resources));
            if (!resources) return rr_error(error,capacity,"native instance refresh allocation failed");
            for (size_t i=0;i<pass->count;i++) resources[i]=pass->items[i].resource;
            int refreshed = 0;
            __try { refreshed = g_rr.instance_refresh(resources,pass->count,error,capacity); }
            __except(EXCEPTION_EXECUTE_HANDLER) { rr_error(error,capacity,"native instance refresh raised an engine error"); }
            free(resources); if (!refreshed) return 0;
        }
        if (!sh_render_scope_update(&pass->render))
            return rr_error(error, capacity, "native resident renderer update failed");
        /* Copied sizes follow the rebuilt images, in either direction (R-321). A restoring pass must finish
         * restoring: it reports what it could not refresh. */
        if (pass->image_count) {
            char line[512];
            if (!sh_size_owners_refresh((const char *const *)pass->images, pass->image_count, pass->restoring, line,
                    sizeof(line))) {
                if (!pass->restoring) return rr_error(error, capacity, line);
                /* Everything else is restored; the values it could not write back keep their obligation and
                 * the restoration reports failure, so a later restoration retries them. */
                char report[640];
                snprintf(report, sizeof(report), "native resident recovery left copied sizes stale: %s", line);
                backend_log(report);
                pass->drained = 1;
                return rr_error(error, capacity, line);
            } else {
                snprintf(line, sizeof(line), "native resident refreshed copied sizes for %zu image(s) rebuilt in place",
                    pass->image_count);
                backend_log(line);
            }
        }
        pass->drained = 1;
        /* Every load ran; a retirement default that failed still fails the pass. */
        return pass->default_faults ? rr_error(error, capacity, pass->default_failure) : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return rr_error(error, capacity, "native resident loading or consumer update failed");
    }
}
int sh_resource_resident_drain(sh_resource_resident *pass, char *error, size_t capacity)
{ return rr_drain(pass, NULL, 0, 0, error, capacity); }
int sh_resource_resident_drain_additions(sh_resource_resident *pass,
    void *const *existing, size_t existing_count, char *error, size_t capacity)
{ return rr_drain(pass, existing, existing_count, 1, error, capacity); }
int sh_resource_resident_end(sh_resource_resident *pass, int succeeded, char *error, size_t capacity)
{
    int clean = 1;
    if (!pass) return succeeded;
    if (pass->thread && pass->thread != GetCurrentThreadId())
        return rr_error(error, capacity, "native resident cleanup requires its owning thread");
    if (g_rr_active == pass) g_rr_active = NULL;
    if (pass->touch_failed) clean = 0;
    if (pass->rebuilt && !pass->drained) clean = 0;
    if (pass->default_faults) clean = 0;
    if (succeeded && !sh_virtual_material_runtime_status(error, capacity)) succeeded = 0;
    for (size_t i = 0; i < pass->count; i++) if (pass->items[i].reconstructed) {
        __try {
            unsigned char *state = (unsigned char *)pass->items[i].resource + 0x2c;
            if (*state & RR_PENDING) { *state &= 0xfdu; clean = 0; }
            /* The pass works at permanent lifetime so nested loads pass the
             * engine's depth guard, then gives each identity back the tier it
             * had: a map-scoped resource stays map-scoped and is retired with
             * its map instead of outliving the heap that owns it. */
            *(unsigned int *)((unsigned char *)pass->items[i].resource + RR_LEVEL_OFFSET) =
                pass->items[i].captured_level;
        } __except (EXCEPTION_EXECUTE_HANDLER) { clean = 0; }
    }
    if (pass->render.active) {
        __try { sh_virtual_material_runtime_collect(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { clean = 0; }
        if (!sh_render_scope_leave(&pass->render)) clean = 0;
    }
    if (!sh_process_heap_leave(&pass->heap)) clean = 0;
    if (pass->render.active || pass->heap.active) {
        g_rr_scope_recovery = pass;
        return rr_error(error, capacity, "native resident refresh scopes remain held for recovery");
    }
    if (g_rr_scope_recovery == pass) g_rr_scope_recovery = NULL;
    if (!clean) rr_error(error, capacity, pass->default_faults ? pass->default_failure :
        "native resident refresh scope did not complete cleanly");
    succeeded = succeeded && clean;
    /* The refresh journal settles only when a pass succeeds: a forward pass commits it, a restoration completes
     * it. After a failure it stays for the (next) restoration. */
    if (succeeded) sh_size_owners_journal_clear();
    if (succeeded) { rr_free(g_rr_recovery); g_rr_recovery = NULL; rr_free(pass); }
    else if (pass->mutated) { rr_free(g_rr_recovery); g_rr_recovery = pass; }
    else rr_free(pass);
    return succeeded;
}
