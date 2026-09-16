#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "resource_resident.h"
#include "resource_graph.h"
#include "resource_catalog.h"
#include "package_runtime.h"
#include "engine_globals.h"
#include "process_heap_scope.h"
#include "backend_log.h"

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
    void *resource, *manager;
    char *type, *name;
    int selected, external, retired, reconstructed;
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
} rr_entry;
/* Every identity that already existed when the pass began, with the lifetime
 * tier it had. Touch consults this to tell a resource the pass just created
 * from one the map owns. */
typedef struct rr_known { void *resource; unsigned int level; } rr_known;
struct sh_resource_resident {
    rr_entry *items;
    size_t count, capacity;
    rr_known *known;
    size_t known_count;
    sh_process_heap_scope heap;
    void *renderer;
    void (*adjust)(void *, int);
    void (*update)(void *);
    int render_count, held, rebuilt, defaults_ready, drained, mutated, touch_failed;
    DWORD thread;
};
static struct {
    sh_process_heap_api heap;
    void **head, **renderer;
    DWORD *thread;
    void (*reconstruct)(void *), (*load)(void *), (*string_free)(void *);
    void *(*lookup)(void *, const char *);
    int (*mode)(void);
    void (*rebind)(void);
} g_rr;
static sh_resource_resident *g_rr_recovery;
static __declspec(thread) sh_resource_resident *g_rr_active;

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
static int rr_has(const sh_resource_graph_impact *set, const rr_entry *entry)
{
    sh_resource_graph_identity key = {entry->type, entry->name};
    return set->count && bsearch(&key, set->items, set->count, sizeof(key), rr_compare) != NULL;
}
static void rr_free(sh_resource_resident *pass)
{
    if (!pass) return;
    for (size_t i = 0; i < pass->count; i++) {
        free(pass->items[i].type); free(pass->items[i].name);
    }
    free(pass->items); free(pass->known); free(pass);
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
    const uint8_t *command, *complete, *mode, *reconstruct, *load, *lookup, *rebind;
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
    materials = (void *)glb_resolve(base, "material_manager_ctx", NULL);
    if (!command || !complete || !mode || !reconstruct || !load || !lookup || !rebind || !materials) return 0;
    __try {
        /* The rebind walks the material manager itself; its only list load is
         * lea rcx at +0x30. Any other manager means a different function. */
        if (memcmp(rebind + 0x30, "\x48\x8d\x0d", 3) || rr_relative(rebind + 0x30, 3, 7) != materials) return 0;
        static const size_t slots[] = {0x1a, 0x39, 0x5e, 0x6e};
        static const size_t calls[] = {0x33, 0x48, 0x68, 0x7b};
        static const uint32_t virtuals[] = {0x80, 0x110, 0x108, 0x110};
        renderer = rr_relative(command + slots[0], 3, 7);
        for (size_t i = 0; i < 4; i++) {
            uint32_t slot;
            memcpy(&slot, command + calls[i] + 2, 4);
            if (memcmp(command + slots[i], "\x48\x8b\x0d", 3) ||
                rr_relative(command + slots[i], 3, 7) != renderer ||
                memcmp(command + calls[i], "\xff\x90", 2) || slot != virtuals[i]) return 0;
        }
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
        int count = *(int *)(manager + 0x28);
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
static int rr_change_compare(const void *key, const void *row)
{
    return strcmp((const char *)key, ((const sh_package_change *)row)->path);
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
        if (change) {
            const sh_resource_catalog_entry *const *rows = NULL;
            size_t count = sh_resource_catalog_find_path(catalog, key, &rows);
            int stock = 0;
            for (size_t i = 0; i < count; i++) if (rows[i]->archive < 2) stock = 1;
            entry->selected = 1;
            if (change->kind == SH_PACKAGE_RESOURCE_REMOVED && !stock) entry->retired = 1;
        }
        free(key); result = 1;
done:;
    } __finally { if (initialized) g_rr.string_free(string); }
    return result;
}
sh_resource_resident *sh_resource_resident_begin(const sh_package_changes *changes,
    int restoring, char *error, size_t capacity)
{
    sh_resource_resident *pass = calloc(1, sizeof(*pass));
    sh_resource_graph_impact seeds = {0}, impact = {0};
    const sh_resource_catalog *catalog = sh_package_runtime_catalog();
    const char *phase = "arguments";
    const rr_entry *current = NULL;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (!pass) { rr_error(error, capacity, "resident refresh allocation failed"); return NULL; }
    __try {
        if (g_rr_active || !changes || (changes->count && !changes->items)) goto done;
        for (size_t i = 0; i < changes->count; i++)
            if (!changes->items[i].path || !*changes->items[i].path ||
                (i && strcmp(changes->items[i-1].path, changes->items[i].path) >= 0)) goto done;
        pass->thread = GetCurrentThreadId();
        if (!g_rr.thread || *g_rr.thread != pass->thread || !g_rr.mode ||
            (g_rr.mode() != 2 && g_rr.mode() != 3) || !sh_process_heap_enter(&g_rr.heap, &pass->heap)) goto done;
        phase = "registry snapshot";
        if (!rr_known_snapshot(pass)) goto done;
        if (!changes->count && !g_rr_recovery) goto render_scope;
        phase = "inventory";
        if (!rr_inventory(pass)) goto done;
        phase = "catalog identities";
        for (size_t i = 0; i < changes->count; i++) {
            const sh_resource_catalog_entry *const *rows = NULL;
            size_t count = sh_resource_catalog_find_path(catalog, changes->items[i].path, &rows);
            if (!rr_seed(&seeds, "", changes->items[i].path)) goto done;
            for (size_t j = 0; j < count; j++) if (!rr_seed(&seeds, rows[j]->type, rows[j]->name)) goto done;
        }
        if (g_rr_recovery) {
            if (!restoring) goto done;
            for (size_t i = 0; i < g_rr_recovery->count; i++)
                if (!rr_seed(&seeds, g_rr_recovery->items[i].type, g_rr_recovery->items[i].name)) goto done;
        }
        phase = "native paths";
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            current = entry;
            if (changes->count && (!rr_path(entry, changes, 0, catalog) || !rr_path(entry, changes, 1, catalog))) goto done;
            if (entry->selected && !rr_seed(&seeds, entry->type, entry->name)) goto done;
        }
        current = NULL; phase = "recorded consumers";
        if (sh_resource_graph_consumers(seeds.items, seeds.count, &impact) == SH_RESOURCE_GRAPH_ERROR) goto done;
        qsort(seeds.items, seeds.count, sizeof(*seeds.items), rr_compare);
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            int supplied = entry->selected;   /* its own native path changed */
            entry->selected |= rr_has(&seeds, entry) || rr_has(&impact, entry);
            /* Only content this provider supplies, or permanent content it can
             * change, may be rebuilt. A map-scoped identity such as the live
             * entitydef:world is built by the map that is already gone --
             * it exists in no archive, so reconstruction replaces live engine
             * state with a default and then refuses the whole activation, and
             * the next map load rebuilds it from the new provider anyway.
             * Implicit-text declarations carry no source to re-read at all.
             * Both are reached only as recorded consumers; a provider that
             * does supply the bytes still selects the identity by path. */
            if (!supplied && (entry->implicit || entry->captured_level != RR_LEVEL_PERMANENT))
                entry->selected = 0;
        }
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
        {
            void **table;
            phase = "renderer scope";
            pass->renderer = *g_rr.renderer;
            table = *(void ***)pass->renderer;
            pass->render_count = *(int *)((unsigned char *)pass->renderer + 0x14);
            if (pass->render_count < 0 || pass->render_count == INT_MAX) goto done;
            pass->adjust = (void (*)(void *, int))table[0x110/8];
            pass->update = (void (*)(void *))table[0x108/8];
            ((void (*)(void *, unsigned char, unsigned char, unsigned char))table[0x80/8])(pass->renderer, 0, 0, 1);
            pass->adjust(pass->renderer, 1); pass->held = 1;
            if (*(int *)((unsigned char *)pass->renderer + 0x14) != pass->render_count + 1) goto done;
        }
        ok = 1;
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    sh_resource_graph_impact_free(&seeds); sh_resource_graph_impact_free(&impact);
    if (!ok) {
        if (error && capacity) snprintf(error, capacity, "native resident %s failed%s%s%s%s", phase,
            current ? " for " : "", current ? current->type : "", current ? ":" : "", current ? current->name : "");
        sh_resource_resident_end(pass, 0, error, capacity); return NULL;
    }
    g_rr_active = pass;
    return pass;
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
void sh_resource_resident_external(sh_resource_resident *pass, void *resource)
{
    if (pass && !pass->rebuilt) for (size_t i = 0; i < pass->count; i++)
        if (pass->items[i].resource == resource) {
            pass->items[i].external = 1; pass->mutated = 1;
        }
}
int sh_resource_resident_reconstruct(sh_resource_resident *pass, char *error, size_t capacity)
{
    if (!pass || pass->thread != GetCurrentThreadId()) return rr_error(error, capacity, "resident refresh thread mismatch");
    if (pass->rebuilt) return 1;
    __try {
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            unsigned char *object = entry->resource;
            if (entry->external) continue;
            /* Preserve storage/identity. Native level-four destructors warn;
             * level two permits their synchronous in-place reconstruction.
             * Restore the tier this identity already had: a supplied map-scoped
             * resource stays map-scoped and is still purged with its map. */
            pass->mutated = 1; entry->reconstructed = 1;
            __try {
                if (*(unsigned int *)(object + RR_LEVEL_OFFSET) == RR_LEVEL_PERMANENT)
                    *(unsigned int *)(object + RR_LEVEL_OFFSET) = 2;
                g_rr.reconstruct(object);
            } __finally { *(unsigned int *)(object + RR_LEVEL_OFFSET) = RR_LEVEL_PERMANENT; }
        }
        for (size_t i = 0; i < pass->count; i++) if (!pass->items[i].external) {
            unsigned char *state = (unsigned char *)pass->items[i].resource + 0x2c;
            if (pass->items[i].retired) *state = (*state & 0xfdu) | RR_DEFAULT;
            else *state |= RR_PENDING;
        }
        pass->rebuilt = 1; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return rr_error(error, capacity, "native resident reconstruction failed");
    }
}
int sh_resource_resident_defaults(sh_resource_resident *pass, char *error, size_t capacity)
{
    if (!pass || pass->thread != GetCurrentThreadId() || !pass->rebuilt)
        return rr_error(error, capacity, "resident defaults preceded reconstruction");
    if (pass->defaults_ready) return 1;
    /* Name the identity under the callback: a failure here is the engine's own
     * default handler raising, and the report has to say which resource it was. */
    volatile const rr_entry *failing = NULL;
    __try {
        /* A removed, non-stock source must not fall through to an old native
         * declaration source record. Use the same default callback as native
         * GenericLoad after reconstruction, before any surviving consumer can
         * look it up. No missing-file read or source-generation fallback. */
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            if (!entry->external && entry->retired) {
                failing = entry;
                sh_resource_graph_frame frame;
                sh_resource_graph_begin(&frame, entry->type, entry->name);
                __try {
                    void **table = *(void ***)entry->resource;
                    ((void (*)(void *))table[0x50/8])(entry->resource);
                } __finally { sh_resource_graph_end(&frame, 0); }
            }
        }
        pass->defaults_ready = 1; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (failing) return rr_error_at(error, capacity, "native resident retirement failed",
            ((const rr_entry *)failing)->type, ((const rr_entry *)failing)->name);
        return rr_error(error, capacity, "native resident retirement failed");
    }
}
int sh_resource_resident_drain(sh_resource_resident *pass, char *error, size_t capacity)
{
    if (!pass || pass->thread != GetCurrentThreadId() || !pass->rebuilt)
        return rr_error(error, capacity, "resident refresh was not reconstructed");
    if (!sh_resource_resident_defaults(pass, error, capacity)) return 0;
    /* Draining twice is legal: the declaration pass finishes this refresh
     * before its palette rebuild, and the provider transaction still calls it. */
    if (pass->drained) return 1;
    __try {
        for (size_t i = 0; i < pass->count; i++) {
            rr_entry *entry = pass->items + i;
            unsigned char *object = entry->resource;
            if (entry->external) continue;
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
        /* Reconstruction discarded each rebuilt material's virtual-texture parm
         * binding, which the engine established once at boot in process heap 0.
         * Left alone, the next map load rebuilds it inside the map or persist
         * heap scope; a permanent material then owns storage ResetPersistHeap
         * frees when Play exits, and the following rebind writes through it.
         * Rebind every material here, still inside this pass's heap-0 scope. */
        g_rr.rebind();
        if (pass->held) pass->update(pass->renderer);
        pass->drained = 1;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return rr_error(error, capacity, "native resident loading or consumer update failed");
    }
}
int sh_resource_resident_end(sh_resource_resident *pass, int succeeded, char *error, size_t capacity)
{
    int clean = 1;
    if (!pass) return succeeded;
    if (g_rr_active == pass) g_rr_active = NULL;
    if (pass->touch_failed) clean = 0;
    if (pass->rebuilt && !pass->drained) clean = 0;
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
    if (pass->held) {
        __try {
            pass->adjust(pass->renderer, -1); pass->held = 0;
            if (*(int *)((unsigned char *)pass->renderer + 0x14) != pass->render_count) clean = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { clean = 0; }
    }
    if (!sh_process_heap_leave(&pass->heap)) clean = 0;
    if (!clean) rr_error(error, capacity, "native resident refresh scope did not complete cleanly");
    succeeded = succeeded && clean;
    if (succeeded) { rr_free(g_rr_recovery); g_rr_recovery = NULL; rr_free(pass); }
    else if (pass->mutated) { rr_free(g_rr_recovery); g_rr_recovery = pass; }
    else rr_free(pass);
    return succeeded;
}
